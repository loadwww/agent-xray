#include "cli/cli.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#include "cli/cli_common.h"
#include "common/log.h"
#include "proxy/recorder.h"
#include "proxy/server.h"
#include "store/finalize.h"
#include "store/run_dir.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace axr {
namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI onCtrlSignal(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE; // 交给我们自己收尾，不立即终止进程
    }
    return FALSE;
}
#endif

void printSummary(const RunSummary& s) {
    if (s.status == "empty") {
        std::cout << "  （未捕获到 LLM 流量）\n";
        return;
    }
    std::cout << "  " << s.stepCount << " steps · " << s.toolCount << " tools · " << s.totalTokens << " tokens"
              << (s.incomplete ? " · ⚠ incomplete" : "") << "\n";
    for (const auto& m : s.missing) std::cout << "  ! " << m << "\n";
}

} // namespace

int cmdServe(const std::vector<std::string>& args) {
    ProxyCliOpts opts;
    auto parsed = parseProxyArgs(args, opts);
    if (!parsed.ok() || !opts.command.empty()) {
        std::cerr << (parsed.ok() ? "错误: serve 不接受 -- 之后的命令\n" : ("错误: " + parsed.err + "\n"))
                  << "用法: axr serve [--config config.yaml] [--root dir] [--listen port] [--bind host]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts);
    if (!cfg.ok()) {
        logError("serve", cfg.err);
        return 2;
    }

    const std::string sessionId = makeSessionId();
    const std::string runDir = ensureRunDir(cfg.value.storage.root, sessionId);
    if (runDir.empty()) {
        logError("serve", "无法创建 run 目录: " + cfg.value.storage.root);
        return 1;
    }

    Recorder rec((std::filesystem::path(runDir) / "events.jsonl").string(), cfg.value.proxy.maxRunBytes,
                 cfg.value.proxy.maxQueue);
    ReverseProxySource source(cfg.value);
    if (!source.start([&rec](const Event& e) { rec.push(e); })) {
        return 1;
    }

    const std::string base = "http://" + cfg.value.proxy.bind + ":" + std::to_string(cfg.value.proxy.listen);
    std::cout << "AgentXray 代理已启动\n"
              << "  监听: " << base << "\n"
              << "  用法: OPENAI_BASE_URL=" << base << "/v1\n"
              << "  会话: " << runDir << "\n"
              << "  按 Ctrl+C 结束并生成分析产物\n";

#ifdef _WIN32
    SetConsoleCtrlHandler(onCtrlSignal, TRUE);
#endif

    while (!g_stop.load() && source.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n正在收尾...\n";
    source.stop();
    rec.flush();
    const RunSummary s = finalizeRun(runDir, cfg.value, rec.dropped());
    std::cout << "✓ 采集完成\n  run: " << sessionId << "\n";
    printSummary(s);
    if (s.status != "empty") std::cout << "  流程图: " << runDir << "\\graph.mmd\n";
    return 0;
}

} // namespace axr
