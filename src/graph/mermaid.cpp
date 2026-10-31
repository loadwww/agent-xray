#include "graph/mermaid.h"

#include <cstdio>
#include <string>
#include <vector>

namespace axr {
namespace {

const char* kCircled[] = {"", "①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩",
                          "⑪", "⑫", "⑬", "⑭", "⑮", "⑯", "⑰", "⑱", "⑲", "⑳"};

std::string circled(int n) {
    if (n >= 1 && n <= 20) return kCircled[n];
    return "[" + std::to_string(n) + "]";
}

std::string fmtSeconds(int ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
    return std::string(buf);
}

std::string shorten(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "…";
}

std::string stepNodeId(int index) { return "S" + std::to_string(index); }

// 长链折叠布局：首尾各保留 2 步，中间连续"带工具的步骤"合并成循环块（§7.4）
struct Block {
    int begin = 0;
    int end = 0; // 闭区间
    bool collapsed = false;
    std::string id;
};

std::vector<Block> layoutBlocks(const Run& run, const MermaidOptions& opts) {
    const int n = static_cast<int>(run.steps.size());
    std::vector<Block> blks;
    const int firstKeep = 2;
    const int lastKeep = 2;

    if (n <= opts.collapseThreshold || n < firstKeep + lastKeep + 2) {
        for (int i = 0; i < n; ++i) blks.push_back(Block{i, i, false, stepNodeId(i)});
        return blks;
    }

    int i = 0;
    while (i < n) {
        const bool inMiddle = (i >= firstKeep && i <= n - lastKeep - 1);
        if (inMiddle && !run.steps[i].tools.empty()) {
            int j = i;
            while (j + 1 <= n - lastKeep - 1 && !run.steps[j + 1].tools.empty()) ++j;
            if (j - i + 1 >= 2) {
                Block b;
                b.begin = i;
                b.end = j;
                b.collapsed = true;
                b.id = "Sc" + std::to_string(i);
                blks.push_back(b);
                i = j + 1;
                continue;
            }
        }
        blks.push_back(Block{i, i, false, stepNodeId(i)});
        ++i;
    }
    return blks;
}

} // namespace

std::string escapeMermaidLabel(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\n': out += "<br/>"; break;
            case '\r': break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string toMermaid(const Run& run, const MermaidOptions& opts) {
    std::string out = "flowchart TD\n";
    const auto blks = layoutBlocks(run, opts);

    // 1. 步骤节点（折叠块用独立节点）
    bool anyLoop = false;
    for (const auto& b : blks) {
        if (b.collapsed) {
            int steps = b.end - b.begin + 1;
            int tools = 0;
            for (int i = b.begin; i <= b.end; ++i) tools += static_cast<int>(run.steps[i].tools.size());
            const std::string label = "「循环」" + std::to_string(steps) + " 步 · " + std::to_string(tools) +
                                      " 次工具调用（已折叠）";
            out += "    " + b.id + "[\"" + escapeMermaidLabel(label) + "\"]\n";
            anyLoop = true;
            continue;
        }
        const Step& s = run.steps[b.begin];
        std::string label = circled(b.begin + 1) + " " + s.call.model;
        label += " · " + std::to_string(s.tokens) + " tok";
        if (s.response.latencyMs > 0) label += " · " + fmtSeconds(s.response.latencyMs);
        if (!s.response.toolCalls.empty()) {
            label += "<br/>tool_calls: ";
            for (std::size_t t = 0; t < s.response.toolCalls.size(); ++t) {
                if (t) label += ", ";
                label += s.response.toolCalls[t].name;
            }
        } else if (s.kind == "final") {
            label += "<br/>final answer";
        }
        out += "    " + b.id + "[\"" + escapeMermaidLabel(label) + "\"]\n";
    }

    // 2. 步骤之间连边；模糊匹配的边画虚线并注"推测"
    for (std::size_t k = 1; k < blks.size(); ++k) {
        const int target = blks[k].begin;
        const bool fuzzy = !blks[k].collapsed && run.steps[target].fuzzyIn;
        out += "    " + blks[k - 1].id + (fuzzy ? " -.->|推测| " : " --> ") + blks[k].id + "\n";
    }

    // 3. 工具菱形节点（折叠块不展开，避免图过大）
    bool anyFail = false;
    for (std::size_t k = 0; k < blks.size(); ++k) {
        const Block& b = blks[k];
        if (b.collapsed) continue;
        const Step& s = run.steps[b.begin];
        const std::string nextId = (k + 1 < blks.size()) ? blks[k + 1].id : std::string();
        for (std::size_t t = 0; t < s.tools.size(); ++t) {
            const ToolExec& te = s.tools[t];
            const std::string id = "T" + std::to_string(b.begin) + "_" + std::to_string(t);
            std::string label = "tool: " + te.name;
            if (!te.arguments.empty()) label += "<br/>" + shorten(te.arguments, 48);
            if (te.durationMs > 0) label += "<br/>~" + fmtSeconds(te.durationMs);
            out += "    " + id + "{\"" + escapeMermaidLabel(label) + "\"}\n";
            out += "    " + b.id + " --> " + id + "\n";
            if (!nextId.empty()) out += "    " + id + " --> " + nextId + "\n";
            if (te.errorKind != "none" && !te.errorKind.empty()) {
                anyFail = true;
                out += "    class " + id + " fail\n";
            }
        }
    }

    if (anyLoop) out += "    classDef loop fill:#eef,stroke:#66c,stroke-dasharray:4 3\n";
    if (anyFail) out += "    classDef fail fill:#fdd,stroke:#c00\n";
    return out;
}

} // namespace axr
