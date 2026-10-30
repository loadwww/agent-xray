#pragma once
// graph 层：Run → Mermaid 流程图（§7.4）
#include <string>

#include "parse/model.h"

namespace axr {

struct MermaidOptions {
    int collapseThreshold = 12; // M3 起用于长链折叠，M1 仅保留字段
};

// 标签转义：& " < > 换行（§7.4 硬约束）
std::string escapeMermaidLabel(const std::string& s);

std::string toMermaid(const Run& run, const MermaidOptions& opts = MermaidOptions{});

} // namespace axr
