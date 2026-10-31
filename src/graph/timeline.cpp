#include "graph/timeline.h"

#include <cstdio>
#include <set>

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
    // 一次 run 内通常只有一个模型，重复打印没有信息量；只在模型真的发生切换时才保留「模型」列
    std::set<std::string> names;
    for (const auto& s : run.steps) {
        if (!s.call.model.empty()) names.insert(s.call.model);
    }
    const bool showModel = names.size() > 1;

    std::string out = showModel ? "| # | 类型 | 模型 | 相对时间 | 耗时 | Token | 工具 |\n"
                                : "| # | 类型 | 相对时间 | 耗时 | Token | 工具 |\n";
    out += showModel ? "|---|---|---|---|---|---|---|\n" : "|---|---|---|---|---|---|\n";
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
        out += "| " + std::to_string(i + 1) + " | " + s.kind + " |";
        if (showModel) out += " " + s.call.model + " |";
        out += " " + rel + " | " + fmtSec(s.response.latencyMs) + " | " +
               (s.response.usage.observed ? std::to_string(s.tokens) : "—") + " | " +
               (tools.empty() ? "-" : tools) + " |\n";
    }
    return out;
}

} // namespace axr
