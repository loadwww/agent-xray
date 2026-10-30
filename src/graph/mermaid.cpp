#include "graph/mermaid.h"

#include <cstdio>
#include <string>

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
    (void)opts;
    std::string out;
    out += "flowchart TD\n";

    // 每个 Step 一个矩形节点
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        std::string label = circled(static_cast<int>(i) + 1) + " " + s.call.model;
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
        out += "    S" + std::to_string(i) + "[\"" + escapeMermaidLabel(label) + "\"]\n";
    }

    // 工具节点（菱形）+ 边
    bool anyFail = false;
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        const bool hasNext = (i + 1 < run.steps.size());
        if (s.tools.empty()) {
            if (hasNext) out += "    S" + std::to_string(i) + " --> S" + std::to_string(i + 1) + "\n";
            continue;
        }
        for (std::size_t t = 0; t < s.tools.size(); ++t) {
            const ToolExec& te = s.tools[t];
            const std::string id = "T" + std::to_string(i) + "_" + std::to_string(t);
            std::string label = "tool: " + te.name;
            if (!te.arguments.empty()) label += "<br/>" + shorten(te.arguments, 48);
            if (te.durationMs > 0) label += "<br/>~" + fmtSeconds(te.durationMs);
            out += "    " + id + "{\"" + escapeMermaidLabel(label) + "\"}\n";
            out += "    S" + std::to_string(i) + " --> " + id + "\n";
            if (hasNext) out += "    " + id + " --> S" + std::to_string(i + 1) + "\n";
            if (te.isError) {
                anyFail = true;
                out += "    class " + id + " fail\n";
            }
        }
    }

    if (anyFail) out += "    classDef fail fill:#fdd,stroke:#c00\n";
    return out;
}

} // namespace axr
