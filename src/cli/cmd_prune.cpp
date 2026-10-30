#include "cli/cli.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "cli/cli_common.h"
#include "store/run_index.h"

namespace axr {
namespace {

struct PruneOpts {
    int days = -1;
    bool apply = false;
    ProxyCliOpts cfgOpts;
};

Status parsePruneArgs(const std::vector<std::string>& args, PruneOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--days" && i + 1 < args.size()) {
            try {
                out.days = std::stoi(args[++i]);
            } catch (...) {
                return Status::Fail("--days 需要整数天数");
            }
        } else if (a == "--apply") {
            out.apply = true;
        } else if (a == "--config" && i + 1 < args.size()) {
            out.cfgOpts.configPath = args[++i];
        } else if (a == "--root" && i + 1 < args.size()) {
            out.cfgOpts.rootDir = args[++i];
        } else if (!a.empty() && a[0] == '-') {
            return Status::Fail("未知选项: " + a);
        } else {
            return Status::Fail("多余参数: " + a);
        }
    }
    if (out.days < 0) return Status::Fail("需要 --days（例如 axr prune --days 7）");
    return Status::Ok();
}

std::string ageText(double days) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f 天", days);
    return buf;
}

} // namespace

int cmdPrune(const std::vector<std::string>& args) {
    PruneOpts opts;
    Status st = parsePruneArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr prune --days N [--apply] [--root dir] [--config config.yaml]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts.cfgOpts);
    if (!cfg.ok()) {
        std::cerr << cfg.err << "\n";
        return 1;
    }

    auto outcome = pruneRuns(cfg.value.storage.root, opts.days, opts.apply);
    if (!outcome.ok()) {
        std::cerr << outcome.err << "\n";
        return 1;
    }

    const auto& cands = outcome.value.candidates;
    if (cands.empty()) {
        std::cout << "没有满足条件的 run（mtime >= " << opts.days << " 天）\n";
        return 0;
    }
    std::cout << (opts.apply ? "已删除 " : "dry-run：将删除 ") << (opts.apply ? outcome.value.removed : cands.size())
              << " 个 run（mtime >= " << opts.days << " 天）";
    if (!opts.apply) std::cout << "；加 --apply 执行";
    std::cout << "\n";
    for (const auto& c : cands) {
        std::cout << "  " << (c.removed ? "- " : "") << c.meta.dirName << "（" << ageText(c.meta.ageDays)
                  << " · " << c.meta.stepCount << " Step）" << (c.removed ? " 已删除" : "") << "\n";
    }
    return 0;
}

} // namespace axr
