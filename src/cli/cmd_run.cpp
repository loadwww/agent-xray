#include "cli/cli.h"

#include <filesystem>
#include <iostream>

#include "cli/cli_common.h"
#include "common/log.h"
#include "proxy/runner.h"

namespace axr {

int cmdRun(const std::vector<std::string>& args) {
    ProxyCliOpts opts;
    auto parsed = parseProxyArgs(args, opts);
    if (!parsed.ok()) {
        std::cerr << "错误: " << parsed.err << "\n"
                  << "用法: axr run [--config config.yaml] [--root dir] [--listen port] [--bind host]"
                     " [--allow-remote] -- <command...>\n";
        return 2;
    }
    if (opts.command.empty()) {
        std::cerr << "用法: axr run [--config config.yaml] [--root dir] [--listen port] [--bind host]"
                     " [--allow-remote] -- <command...>\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts);
    if (!cfg.ok()) {
        logError("run", cfg.err);
        return 2;
    }

    auto bindOk = enforceBindPolicy(cfg.value, opts.allowRemote);
    if (!bindOk.ok()) {
        logError("run", bindOk.err);
        return 2;
    }

    int childExit = 0;
    auto summary = runTarget(cfg.value, opts.command, childExit);
    if (!summary.ok()) {
        logError("run", summary.err);
        return 1;
    }
    const RunSummary& s = summary.value;

    std::cout << "✓ 采集完成\n";
    std::cout << "  run: " << s.sessionId << "\n";
    if (s.status == "empty") {
        std::cout << "  （未捕获到 LLM 流量，检查 agent 是否使用 OPENAI_BASE_URL）\n";
    } else {
        std::cout << "  " << s.stepCount << " steps · " << s.toolCount << " tools · "
                  << (s.usageObserved ? std::to_string(s.totalTokens) : std::string("—"))
                  << " tokens" << (s.incomplete ? " · ⚠ incomplete" : "") << "\n";
        for (const auto& m : s.missing) std::cout << "  ! " << m << "\n";
        std::cout << "  输出: " << s.runDir << "\n";
        std::cout << "  流程图: " << (std::filesystem::path(s.runDir) / "graph.mmd").string() << "\n";
        if (!s.reportPath.empty()) std::cout << "  报告: " << s.reportPath << "\n";
    }
    if (childExit != 0) std::cout << "  目标进程退出码: " << childExit << "\n";
    return 0;
}

} // namespace axr
