#include "ui/report.h"

#include <cstdio>
#include <set>
#include <vector>

#include "common/time_util.h"
#include "graph/timeline.h"

namespace axr {
namespace {

std::string cell(const std::string& s, std::size_t maxLen) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r') t += ' ';
        else if (c == '|') t += "\\|";
        else t.push_back(c);
    }
    if (t.size() > maxLen) t = t.substr(0, maxLen) + "…";
    return t.empty() ? "-" : t;
}

std::string fmtSec(long long ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
    return std::string(buf);
}

std::string modelsOf(const Run& run) {
    std::set<std::string> names;
    for (const auto& s : run.steps)
        if (!s.call.model.empty()) names.insert(s.call.model);
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out.empty() ? "-" : out;
}

int toolCount(const Run& run) {
    int n = 0;
    for (const auto& s : run.steps) n += static_cast<int>(s.tools.size());
    return n;
}

long long durationMs(const Run& run) {
    auto a = parseRfc3339(run.startedAt);
    auto b = parseRfc3339(run.endedAt);
    if (!a.ok() || !b.ok()) return 0;
    const long long ms = diffMillis(b.value, a.value);
    return ms > 0 ? ms : 0;
}

} // namespace

std::string buildReport(const Run& run, const MermaidOptions& mo) {
    std::string out;
    const int tokens = run.total.promptTokens + run.total.completionTokens;
    const std::string tokensText = run.total.observed ? std::to_string(tokens) : "—";
    const int toolN = toolCount(run);

    out += "# Run: " + run.id + "\n\n";

    // 缺失步骤警告放最顶部：不能让用户误以为 agent 真的少走了一步（§7.3.5）
    if (run.incomplete) {
        out += "> ⚠ 检测到缺失步骤（采集不完整，图上的断点不代表 agent 行为）\n";
        for (const auto& m : run.missing) out += "> - " + m + "\n";
        out += "\n";
    }

    out += "- 模型：" + modelsOf(run) + "\n";
    out += "- 步数：" + std::to_string(run.steps.size()) + "　工具调用：" + std::to_string(toolN) +
           "　总 token：" + tokensText + "\n";
    out += "- 状态：" + std::string(run.incomplete ? "incomplete" : "complete") +
           "　耗时：" + fmtSec(durationMs(run)) + "\n";
    out += "- 原始请求：" + std::to_string(run.requestCount) + "　重试：" + std::to_string(run.retryCount) +
           "　模糊匹配边：" + std::to_string(run.fuzzyEdges) + "\n\n";

    out += "## 执行流程\n\n```mermaid\n" + toMermaid(run, mo) + "```\n\n";

    out += "## 时间线\n\n" + toMarkdownTimeline(run) + "\n";

    out += "## 工具调用\n\n";
    if (toolN == 0) {
        out += "（本轮没有工具调用）\n\n";
    } else {
        out += "| # | 工具 | 参数摘要 | 结果摘要 | 耗时 |\n|---|---|---|---|---|\n";
        int k = 0;
        for (const auto& s : run.steps) {
            for (const auto& te : s.tools) {
                ++k;
                out += "| " + std::to_string(k) + " | " + cell(te.name, 24) + " | " + cell(te.arguments, 40) +
                       " | " + cell(te.result, 40) + " | " + (te.durationMs > 0 ? "~" + fmtSec(te.durationMs) : "-") +
                       " |\n";
            }
        }
        out += "\n";
    }

    // 单模型 run 不再逐行重复模型名（上方概览已经写过一次），只有中途切模型时才保留该列
    std::set<std::string> modelNames;
    for (const auto& s : run.steps) {
        if (!s.call.model.empty()) modelNames.insert(s.call.model);
    }
    const bool showModel = modelNames.size() > 1;

    out += "## Token 用量\n\n";
    out += showModel ? "| Step | Model | Prompt | Completion | 合计 |\n|---|---|---|---|---|\n"
                     : "| Step | Prompt | Completion | 合计 |\n|---|---|---|---|\n";
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        const bool observed = s.response.usage.observed;
        const std::string sum = observed
                                    ? std::to_string(s.response.usage.promptTokens + s.response.usage.completionTokens)
                                    : "—";
        out += "| " + std::to_string(i + 1) + " |";
        if (showModel) out += " " + cell(s.call.model, 24) + " |";
        out += " " + (observed ? std::to_string(s.response.usage.promptTokens) : "—") + " | " +
               (observed ? std::to_string(s.response.usage.completionTokens) : "—") + " | " + sum + " |\n";
    }
    out += "\nTotal: " + std::to_string(run.steps.size()) + " steps · " + tokensText + " tokens\n\n";

    out += "## 备注\n\n";
    out += "- 工具耗时均为推断值（无 MCP 代理，标注 `~`）\n";
    if (run.fuzzyEdges > 0) out += "- 有 " + std::to_string(run.fuzzyEdges) + " 条模糊匹配边，图上虚线标注\"推测\"，非确定关系\n";
    out += run.missing.empty() ? "- 未检测到数据缺失\n" : "- 检测到数据缺失，见顶部警告\n";
    return out;
}

} // namespace axr
