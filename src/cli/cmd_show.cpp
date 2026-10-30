#include "cli/cli.h"

#include <iostream>
#include <string>

#include "cli/cli_common.h"
#include "cost/pricing.h"
#include "graph/timeline.h"
#include "store/events_reader.h"

namespace axr {
namespace {

// axr show 的参数：目标路径/runID + --config / --root
struct ShowOpts {
    std::string target;
    ProxyCliOpts cfgOpts;
};

Status parseShowArgs(const std::vector<std::string>& args, ShowOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else if (out.target.empty()) out.target = a;
        else return Status::Fail("多余参数: " + a);
    }
    if (out.target.empty()) return Status::Fail("缺少 run 目标（run 目录 / events.jsonl / runID）");
    return Status::Ok();
}

void printRun(const Run& run, const Config& cfg) {
    const CostBreakdown cost = computeCost(run, cfg.pricing);
    int tools = 0;
    for (const auto& s : run.steps) tools += static_cast<int>(s.tools.size());

    std::cout << "Run: " << run.id << "\n";
    std::cout << "  状态: " << (run.incomplete ? "incomplete" : "complete") << " · 原始请求 "
              << run.requestCount << "（重试 " << run.retryCount << "）· Step " << run.steps.size() << " · 工具 "
              << tools << " · fuzzy " << run.fuzzyEdges << "\n";
    std::cout << "  token: " << (run.total.promptTokens + run.total.completionTokens)
              << " · 成本: " << (cost.complete ? fmtCost(cost.totalUsd) : "n/a") << "\n";
    std::cout << "  时间: " << run.startedAt << " → " << run.endedAt << "\n";
    for (const auto& m : run.missing) std::cout << "  ! " << m << "\n";

    std::cout << toMarkdownTimeline(run);

    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        if (s.tools.empty()) continue;
        std::cout << "  " << (i + 1) << " → ";
        for (std::size_t t = 0; t < s.tools.size(); ++t) {
            if (t) std::cout << "、";
            std::cout << s.tools[t].name;
        }
        std::cout << "（并行 " << s.tools.size() << "）\n";
    }
}

} // namespace

int cmdShow(const std::vector<std::string>& args) {
    ShowOpts opts;
    Status st = parseShowArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr show <run目录|events.jsonl|runID> [--config config.yaml] [--root dir]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts.cfgOpts);
    if (!cfg.ok()) {
        std::cerr << cfg.err << "\n";
        return 1;
    }
    const std::string target = resolveRunInput(opts.target, cfg.value);
    auto chain = buildChainFromEvents(target, cfg.value.chainOptions());
    if (!chain.ok()) {
        std::cerr << chain.err << "\n";
        return 1;
    }
    if (chain.value.runs.empty()) {
        std::cout << "未从 " << target << " 中还原出任何 run\n";
        return 1;
    }
    for (const auto& r : chain.value.runs) {
        printRun(r, cfg.value);
        std::cout << "\n";
    }
    return 0;
}

} // namespace axr
