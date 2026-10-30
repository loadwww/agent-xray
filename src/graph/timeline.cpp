#include "graph/timeline.h"

#include <cstdio>

#include "common/time_util.h"

namespace axr {
namespace {

std::string fmtSec(int ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
    return std::string(buf);
}

} // namespace

std::string toMarkdownTimeline(const Run& run) {
    const auto base = parseRfc3339(run.startedAt);
    std::string out = "| # | 类型 | 模型 | 相对时间 | 耗时 | Token | 工具 |\n";
    out += "|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        std::string rel = "-";
        const auto t = parseRfc3339(s.call.startedAt);
        if (base.ok() && t.ok()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "+%.1fs", static_cast<double>(diffMillis(t.value, base.value)) / 1000.0);
            rel = buf;
        }
        std::string tools;
        for (std::size_t k = 0; k < s.tools.size(); ++k) {
            if (k) tools += ", ";
            tools += s.tools[k].name;
        }
        out += "| " + std::to_string(i + 1) + " | " + s.kind + " | " + s.call.model + " | " + rel + " | " +
               fmtSec(s.response.latencyMs) + " | " + std::to_string(s.tokens) + " | " +
               (tools.empty() ? "-" : tools) + " |\n";
    }
    return out;
}

} // namespace axr
