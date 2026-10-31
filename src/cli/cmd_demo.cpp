#include "cli/cli.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/demo_assets.h"
#include "config/config.h"
#include "diff/rundiff.h"
#include "parse/model.h"
#include "proxy/server.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#include <cstdlib>
#endif

namespace axr {
namespace {

#ifdef _WIN32
std::atomic<bool> g_demoStop{false};

BOOL WINAPI onDemoCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_demoStop = true;
        return TRUE;
    }
    return FALSE;
}
#else
volatile sig_atomic_t g_demoStopSig = 0;
void onDemoSignal(int) { g_demoStopSig = 1; }
#endif

struct DemoOpts {
    int port = 8787;
    bool noOpen = false;
    bool wantDiff = false;
};

// 临时根目录：遵循 TMPDIR / XDG_CACHE_HOME，Windows 用 %TEMP%；带时间戳避免残留目录复用
std::filesystem::path demoRoot() {
#ifdef _WIN32
    std::filesystem::path base = std::filesystem::temp_directory_path();
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* tmp = std::getenv("TMPDIR");
    std::filesystem::path base = xdg && *xdg ? std::filesystem::path(xdg) / "axr"
                                             : (tmp && *tmp ? std::filesystem::path(tmp) : std::filesystem::temp_directory_path());
#endif
    const long long stamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return base / ("axr-demo-" + std::to_string(stamp % 1000000));
}

void cleanupRoot(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// A1：把内嵌示例释放到临时目录，不碰用户的 runs 目录
bool writeDemoFiles(const std::filesystem::path& root, std::string& err) {
    for (int i = 0; i < demo::kDemoFileCount; ++i) {
        const std::string rel = demo::kDemoFiles[i].path;
        const std::filesystem::path dst = root / std::filesystem::path(rel);
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        if (ec) {
            err = "无法创建临时目录: " + dst.parent_path().string() + " (" + ec.message() + ")";
            return false;
        }
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "无法写入示例文件: " + dst.string();
            return false;
        }
        out.write(demo::kDemoFiles[i].content, static_cast<std::streamsize>(std::strlen(demo::kDemoFiles[i].content)));
        if (!out) {
            err = "写入示例文件失败: " + dst.string();
            return false;
        }
    }
    return true;
}

std::vector<std::filesystem::path> demoSessionDirs(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> dirs;
    const std::filesystem::path runsDir = root / "runs";
    std::error_code ec;
    if (!std::filesystem::is_directory(runsDir, ec)) return dirs;
    for (const auto& e : std::filesystem::directory_iterator(runsDir, ec)) {
        if (!e.is_directory()) continue;
        // 只收有 run.json 的示例会话
        if (!std::filesystem::is_regular_file(e.path() / "run.json", ec)) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

bool loadDemoRun(const std::filesystem::path& dir, Run& run, std::string& err) {
    std::ifstream in(dir / "run.json", std::ios::binary);
    if (!in) {
        err = "缺少 run.json: " + dir.string();
        return false;
    }
    try {
        nlohmann::json j;
        in >> j;
        run = j.get<Run>();
        return true;
    } catch (const std::exception& e) {
        err = "run.json 解析失败: " + std::string(e.what());
        return false;
    }
}

std::string clip(const std::string& s, std::size_t maxLen = 60) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "…";
}

void printStepDelta(const StepDelta& d) {
    if (d.change == "added") {
        std::cout << "  + 新增 S" << d.index << "（" << d.modelAfter << " · " << d.tokensAfter << " tok）\n";
    } else if (d.change == "removed") {
        std::cout << "  - 删除 S" << d.index << "（" << d.modelBefore << " · " << d.tokensBefore << " tok）\n";
    } else {
        std::cout << "  ~ 变化 S" << d.index << "：";
        if (d.fields.empty()) {
            std::cout << "工具内容变化";
        }
        for (std::size_t i = 0; i < d.fields.size(); ++i) {
            if (i) std::cout << "、";
            const std::string& f = d.fields[i];
            if (f == "tokens") std::cout << "token " << d.tokensBefore << " → " << d.tokensAfter;
            else if (f == "model") std::cout << "模型 " << d.modelBefore << " → " << d.modelAfter;
            else std::cout << f;
        }
        std::cout << "\n";
    }
}

void printToolDelta(const ToolDelta& d) {
    const std::string where = "（S" + std::to_string(d.stepIndex + 1) + "）";
    if (d.change == "added") {
        std::cout << "  + 新增 T" << d.toolIndex << " " << d.name << where << " 参数 " << clip(d.after) << "\n";
    } else if (d.change == "removed") {
        std::cout << "  - 删除 T" << d.toolIndex << " " << d.name << where << " 参数 " << clip(d.before) << "\n";
    } else if (d.change == "name") {
        std::cout << "  ~ 名称变化 T" << d.toolIndex << where << "：" << d.before << " → " << d.after << "\n";
    } else if (d.change == "arguments") {
        std::cout << "  ~ 参数变化 T" << d.toolIndex << " " << d.name << where << "：" << clip(d.before) << " → "
                  << clip(d.after) << "\n";
    } else if (d.change == "result") {
        std::cout << "  ~ 结果变化 T" << d.toolIndex << " " << d.name << where << "：" << clip(d.before) << " → "
                  << clip(d.after) << "\n";
    }
}

// A1 --diff：直接打印两次示例 run 的差异后退出
int demoDiff(const std::filesystem::path& root) {
    const auto dirs = demoSessionDirs(root);
    if (dirs.size() < 2) {
        std::cerr << "错误: 示例数据缺少两次 run（找到 " << dirs.size() << " 个）\n";
        return 1;
    }
    Run base, cur;
    std::string err;
    if (!loadDemoRun(dirs[0], base, err) || !loadDemoRun(dirs[1], cur, err)) {
        std::cerr << "错误: " << err << "\n";
        return 1;
    }

    const RunDiff d = diffRuns(base, cur);
    const auto tokens = [](const Run& r) {
        return r.total.observed ? std::to_string(r.total.promptTokens + r.total.completionTokens) : std::string("—");
    };

    std::cout << "AgentXray 演示 · 同一任务两次运行的差异\n"
              << "  run A: " << base.id << " · " << base.steps.size() << " Step · " << countTools(base) << " 工具 · "
              << tokens(base) << " token（一次成功）\n"
              << "  run B: " << cur.id << " · " << cur.steps.size() << " Step · " << countTools(cur) << " 工具 · "
              << tokens(cur) << " token（改错文件后绕圈）\n\n";

    if (d.empty()) {
        std::cout << "两次运行结构一致。\n";
        return 0;
    }
    if (!d.steps.empty()) {
        std::cout << "步骤差异\n";
        for (const auto& s : d.steps) printStepDelta(s);
        std::cout << "\n";
    }
    if (!d.tools.empty()) {
        std::cout << "工具差异\n";
        for (const auto& t : d.tools) printToolDelta(t);
        std::cout << "\n";
    }
    for (const auto& n : d.notes) std::cout << "  ! " << n << "\n";
    std::cout << "完整图与报告: axr demo（浏览器打开页面后切换两次 run）\n";
    return 0;
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    std::wstring w(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const std::string opener =
#ifdef __APPLE__
        "open";
#else
        "xdg-open";
#endif
    std::system((opener + " '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

int demoServe(const std::filesystem::path& root, const DemoOpts& opts) {
    Config cfg = defaultConfig();
    cfg.storage.root = root.string();
    cfg.storage.retentionDays = 0;       // 不清理示例
    cfg.storage.idleFinalizeMinutes = 0; // 不轮转
    cfg.proxy.bind = "127.0.0.1";
    cfg.proxy.listen = opts.port;

    ReverseProxySource source(cfg);
#ifdef _WIN32
    SetConsoleCtrlHandler(onDemoCtrl, TRUE);
#else
    std::signal(SIGINT, onDemoSignal);
    std::signal(SIGTERM, onDemoSignal);
#endif

    if (!source.start([](const Event&) {})) {
        std::cerr << "错误: 无法在 127.0.0.1:" << opts.port << " 上启动页面（端口被占用？换 --port 试试）\n";
        return 1;
    }

    const std::string base = "http://127.0.0.1:" + std::to_string(opts.port);
    std::cout << "AgentXray 演示（离线运行，无需 API key，示例数据只在临时目录）\n"
              << "  临时目录: " << root.string() << "\n"
              << "  页面: " << base << "/__xray\n"
              << "  示例: run A 正常完成；run B 改错文件后绕圈（含明确失败与疑似失败）\n"
              << "  差异: Ctrl+C 退出后执行 axr demo --diff\n";
    if (!opts.noOpen) {
        std::cout << "  已尝试用默认浏览器打开页面\n";
        openBrowser(base + "/__xray");
    }
    std::cout << "  按 Ctrl+C 退出\n";

#ifdef _WIN32
    while (!g_demoStop.load() && source.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
#else
    while (g_demoStopSig == 0 && source.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
    source.stop();
    return 0;
}

} // namespace

int cmdDemo(const std::vector<std::string>& args) {
    DemoOpts opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            std::cout << "用法: axr demo [--port 8787] [--no-open] [--diff]\n\n"
                         "  释放内置的两次示例 run 到临时目录并打开 Web UI（离线，无需 API key）。\n"
                         "  --no-open  不自动打开浏览器（CI / 服务器）\n"
                         "  --diff     只打印两次示例 run 的差异后退出\n";
            return 0;
        }
        if (a == "--no-open") {
            opts.noOpen = true;
        } else if (a == "--diff") {
            opts.wantDiff = true;
        } else if (a == "--port" && i + 1 < args.size()) {
            const std::string v = args[++i];
            try {
                opts.port = std::stoi(v);
            } catch (...) {
                std::cerr << "错误: --port 不是整数: " << v << "\n";
                return 2;
            }
            if (opts.port < 1 || opts.port > 65535) {
                std::cerr << "错误: --port 超出范围: " << opts.port << "\n";
                return 2;
            }
        } else {
            std::cerr << "未知选项: " << a << "\n用法: axr demo [--port 8787] [--no-open] [--diff]\n";
            return 2;
        }
    }

    const std::filesystem::path root = demoRoot();
    std::string err;
    cleanupRoot(root); // 清掉同名的残留（极少见）
    if (!writeDemoFiles(root, err)) {
        std::cerr << "错误: " << err << "\n";
        cleanupRoot(root);
        return 1;
    }

    int rc = 0;
    if (opts.wantDiff) {
        rc = demoDiff(root);
    } else {
        rc = demoServe(root, opts);
    }
    cleanupRoot(root);
    if (rc == 0 && !opts.wantDiff) std::cout << "✓ 演示已退出，临时目录已清理\n";
    return rc;
}

} // namespace axr
