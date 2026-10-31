#include "cli/cli.h"

#include <chrono>
#include <iostream>
#include <set>
#include <string>

#include "cli/cli_common.h"
#include "graph/timeline.h"
#include "store/events_reader.h"

namespace axr {
namespace {

// axr show 的参数：目标路径/runID + --run / --config / --root / --bench
struct ShowOpts {
    std::string target;
    std::string runId; // --run：只看指定 run（多 run session 里的非主 run）
    bool bench = false;
    ProxyCliOpts cfgOpts;
};

Status parseShowArgs(const std::vector<std::string>& args, ShowOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--bench") out.bench = true;
        else if (a == "--run" && i + 1 < args.size()) out.runId = args[++i];
        else if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else if (out.target.empty()) out.target = a;
        else return Status::Fail("多余参数: " + a);
    }
    if (out.target.empty()) return Status::Fail("缺少 run 目标（run 目录 / events.jsonl / runID）");
    return Status::Ok();
}

void printRun(const Run& run) {
    int tools = 0;
    for (const auto& s : run.steps) tools += static_cast<int>(s.tools.size());

    std::cout << "Run: " << run.id << "\n";
    std::cout << "  状态: " << (run.incomplete ? "incomplete" : "complete") << " · 原始请求 "
              << run.requestCount << "（重试 " << run.retryCount << "）· Step " << run.steps.size() << " · 工具 "
              << tools << " · fuzzy " << run.fuzzyEdges << "\n";
    // 模型只打印一次（时间线表里不再逐行重复）；中途切换模型时会全部列在这里
    std::set<std::string> models;
    for (const auto& s : run.steps) {
        if (!s.call.model.empty()) models.insert(s.call.model);
    }
    std::string modelText;
    for (const auto& m : models) {
        if (!modelText.empty()) modelText += ", ";
        modelText += m;
    }
    const std::string tokenText =
        run.total.observed ? std::to_string(run.total.promptTokens + run.total.completionTokens) : "—";
    std::cout << "  token: " << tokenText << " · 模型: "
              << (modelText.empty() ? "-" : modelText) << "\n";
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
        std::cerr << st.err
                  << "\n用法: axr show <run目录|events.jsonl|runID> [--run runId] [--config config.yaml] [--root dir]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts.cfgOpts);
    if (!cfg.ok()) {
        std::cerr << cfg.err << "\n";
        return 1;
    }
    const std::string target = resolveRunInput(opts.target, cfg.value);
    const auto t0 = std::chrono::steady_clock::now();
    auto chain = buildChainFromEvents(target, cfg.value.chainOptions());
    if (!chain.ok()) {
        std::cerr << chain.err << "\n";
        return 1;
    }
    if (opts.bench) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "建链耗时: " << ms << " ms（" << target << "）\n\n";
    }
    if (chain.value.runs.empty()) {
        std::cout << "未从 " << target << " 中还原出任何 run\n";
        return 1;
    }
    if (!opts.runId.empty()) {
        const Run* r = findRunById(chain.value, opts.runId);
        if (r == nullptr) {
            std::cerr << "找不到 run: " << opts.runId << "（该 session 的 run:";
            for (const auto& x : chain.value.runs) std::cerr << " " << x.id;
            std::cerr << "）\n";
            return 1;
        }
        printRun(*r);
        return 0;
    }
    for (const auto& r : chain.value.runs) {
        printRun(r);
        std::cout << "\n";
    }
    return 0;
}

} // namespace axr
