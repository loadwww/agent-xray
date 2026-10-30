#include "cli/cli.h"

#include <filesystem>
#include <iostream>
#include <string>

#include "chain/run.h"
#include "cli/cli_common.h"
#include "cost/pricing.h"
#include "graph/mermaid.h"
#include "store/events_reader.h"
#include "store/run_dir.h"

namespace axr {
namespace {

struct ExportOpts {
    std::string target;
    std::string format; // mermaid | json
    std::string out;
    ProxyCliOpts cfgOpts;
};

Status parseExportArgs(const std::vector<std::string>& args, ExportOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--format" && i + 1 < args.size()) out.format = args[++i];
        else if (a == "--out" && i + 1 < args.size()) out.out = args[++i];
        else if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else if (out.target.empty()) out.target = a;
        else return Status::Fail("多余参数: " + a);
    }
    if (out.target.empty()) return Status::Fail("缺少 run 目标（run 目录 / events.jsonl / runID）");
    if (out.format != "mermaid" && out.format != "json")
        return Status::Fail("--format 需要 mermaid 或 json");
    return Status::Ok();
}

} // namespace

int cmdExport(const std::vector<std::string>& args) {
    ExportOpts opts;
    Status st = parseExportArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr export <run目录|events.jsonl|runID> --format mermaid|json [--out file]\n";
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
    const Run* found = primaryRun(chain.value);
    if (!found) {
        std::cerr << "未从 " << target << " 中还原出任何 run\n";
        return 1;
    }

    // 复制一份：导出时按当前单价表补齐成本字段，不改动链上数据
    Run run = *found;
    applyCost(run, cfg.value.pricing);

    std::string payload;
    if (opts.format == "mermaid") {
        MermaidOptions mo;
        mo.collapseThreshold = cfg.value.chain.collapseThreshold;
        payload = toMermaid(run, mo);
    } else {
        payload = nlohmann::json(run).dump(2);
    }
    if (!payload.empty() && payload.back() != '\n') payload.push_back('\n');

    if (opts.out.empty()) {
        std::cout << payload;
        return 0;
    }
    if (!writeTextFile(opts.out, payload)) {
        std::cerr << "无法写入 " << opts.out << "\n";
        return 1;
    }
    std::cout << "导出: " << opts.out << "（" << opts.format << "）\n";
    return 0;
}

} // namespace axr
