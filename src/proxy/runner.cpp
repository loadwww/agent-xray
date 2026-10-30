#include "proxy/runner.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <map>
#include <thread>

#include "common/log.h"
#include "proxy/recorder.h"
#include "proxy/server.h"
#include "store/run_dir.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace axr {
namespace {

#ifdef _WIN32

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// MSVC 运行库的参数转义规则（CreateProcess 解析命令行时遵循）
std::string quoteArg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < a.size() && a[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == a.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (a[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += a[i];
        }
    }
    out += '"';
    return out;
}

std::string upperKey(const std::wstring& key) {
    std::string s;
    s.reserve(key.size());
    for (wchar_t c : key) s.push_back(static_cast<char>(towupper(c)));
    return s;
}

// 合并进程当前环境 + 覆盖项，生成宽字符环境块
std::wstring buildEnvBlock(const std::map<std::string, std::string>& extra) {
    std::map<std::string, std::string> overrides;
    for (const auto& kv : extra) overrides[upperKey(toWide(kv.first))] = kv.second;

    std::wstring block;
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            if (entry[0] == L'=') continue; // 隐藏变量（=C: 等）不重复写入
            const std::size_t eq = entry.find(L'=');
            const std::string key = (eq == std::wstring::npos) ? upperKey(entry) : upperKey(entry.substr(0, eq));
            if (overrides.count(key)) continue;
            block += entry;
            block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& kv : overrides) {
        block += toWide(kv.first);
        block.push_back(L'=');
        block += toWide(kv.second);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

#endif

} // namespace

Result<int> spawnWithEnv(const std::vector<std::string>& argv, const std::map<std::string, std::string>* extraEnv) {
    if (argv.empty()) return Result<int>::Fail("命令为空");
#ifdef _WIN32
    std::string cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline.push_back(' ');
        cmdline += quoteArg(argv[i]);
    }

    std::wstring envBlock;
    if (extraEnv) envBlock = buildEnvBlock(*extraEnv);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmdW = toWide(cmdline);
    const BOOL ok = CreateProcessW(nullptr, cmdW.data(), nullptr, nullptr, TRUE,
                                   extraEnv ? CREATE_UNICODE_ENVIRONMENT : 0,
                                   extraEnv ? const_cast<wchar_t*>(envBlock.c_str()) : nullptr, nullptr, &si, &pi);
    if (!ok) {
        return Result<int>::Fail("创建子进程失败（错误码 " + std::to_string(GetLastError()) + "）: " + cmdline);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Result<int>::Ok(static_cast<int>(code));
#else
    (void)extraEnv;
    return Result<int>::Fail("当前平台暂不支持子进程执行");
#endif
}

Result<RunSummary> runTarget(const Config& cfg, const std::vector<std::string>& command, int& childExitCode) {
    if (command.empty()) return Result<RunSummary>::Fail("用法: axr run -- <command...>");

    const std::string sessionId = makeSessionId();
    const std::string runDir = ensureRunDir(cfg.storage.root, sessionId);
    if (runDir.empty()) return Result<RunSummary>::Fail("无法创建 run 目录: " + cfg.storage.root);

    logInfo("run", "会话: " + runDir);

    Recorder rec((std::filesystem::path(runDir) / "events.jsonl").string(), cfg.proxy.maxRunBytes,
                 cfg.proxy.maxQueue);
    ReverseProxySource source(cfg);
    if (!source.start([&rec](const Event& e) { rec.push(e); })) {
        return Result<RunSummary>::Fail("代理启动失败（端口 " + std::to_string(cfg.proxy.listen) + " 可能被占用）");
    }

    const std::string base = "http://" + cfg.proxy.bind + ":" + std::to_string(cfg.proxy.listen);
    std::map<std::string, std::string> env = {
        {"OPENAI_BASE_URL", base + "/v1"},
        {"OPENAI_API_BASE", base + "/v1"},
        {"DEEPSEEK_BASE_URL", base + "/v1"},
        {"OLLAMA_HOST", base + "/ollama"},
    };

    std::string cmdText;
    for (const auto& a : command) cmdText += (cmdText.empty() ? "" : " ") + a;
    logInfo("run", "执行: " + cmdText);

    auto child = spawnWithEnv(command, &env);
    childExitCode = child.ok() ? child.value : -1;
    if (!child.ok()) logError("run", child.err);

    source.stop();
    rec.flush();
    const long long dropped = rec.dropped();
    // rec 析构前完成 finalize，确保 events.jsonl 已落盘
    RunSummary summary = finalizeRun(runDir, cfg, dropped);
    return Result<RunSummary>::Ok(summary);
}

} // namespace axr
