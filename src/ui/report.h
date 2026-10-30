#pragma once
// ui 层（L1）：Run + 成本 → report.md（§7.6）
#include <string>

#include "cost/pricing.h"
#include "graph/mermaid.h"
#include "parse/model.h"

namespace axr {

std::string buildReport(const Run& run, const CostBreakdown& cost,
                        const MermaidOptions& mo = MermaidOptions{});

} // namespace axr
