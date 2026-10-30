#include "cli/cli.h"

#include <filesystem>
#include <iostream>
#include <string>

#include "cli/cli_common.h"
#include "store/events_reader.h"
#include "store/run_dir.h"
#include "ui/report.h"

namespace axr {
namespace {

// axr report 的参数：目标 + --out / --config / --root
struct ReportOpts {
    std::string target;
    std::string out;
    ProxyCliOpts cfgOpts;
};

Status parseReportArgs(const std::vector<std::string>& args, ReportOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--out" && i + 1 < args.size()) out.out = args[++i];
        else if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else if (out.target.empty()) out.target = a;
        else return Status::Fail("多余参数: " + a);
    }
    if (out.target.empty()) return Status::Fail("缺少 run 目标（run 目录 / events.jsonl / runID）");
    return Status::Ok();
}

} // namespace

int cmdReport(const std::vector<std::string>& args) {
    ReportOpts opts;
    Status st = parseReportArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr report <run目录|events.jsonl|runID> [--out file] [--config config.yaml]\n";
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
    const Run* run = primaryRun(chain.value);
    if (!run) {
        std::cerr << "未从 " << target << " 中还原出任何 run\n";
        return 1;
    }

    MermaidOptions mo;
    mo.collapseThreshold = cfg.value.chain.collapseThreshold;
    const std::string report = buildReport(*run, mo);

    std::string outPath = opts.out;
    if (outPath.empty()) {
        const std::string eventsPath = resolveEventsPath(target);
        outPath = (std::filesystem::path(eventsPath).parent_path() / "report.md").string();
    }
    if (!writeTextFile(outPath, report)) {
        std::cerr << "无法写入 " << outPath << "\n";
        return 1;
    }
    std::cout << "报告: " << outPath << "\n";
    std::cout << "  run: " << run->id << " · " << run->steps.size() << " steps · "
              << (run->incomplete ? "incomplete" : "complete") << "\n";
    return 0;
}

} // namespace axr
