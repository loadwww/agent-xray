#include "proxy/runner.h"

#include <filesystem>
#include <map>

#include "common/log.h"
#include "proxy/recorder.h"
#include "proxy/server.h"
#include "store/run_dir.h"

namespace axr {

// spawnWithEnv 的平台实现：
//   Windows → spawn_win.cpp（CreateProcess，MSVC 命令行转义 + 宽字符环境块）
//   其他    → spawn_posix.cpp（fork + execve，独立 envp 数组）

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
    RunSummary summary = finalizeRun(runDir, cfg, dropped, source.rewrittenRequests());
    return Result<RunSummary>::Ok(summary);
}

} // namespace axr