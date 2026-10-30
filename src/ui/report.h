#pragma once
// ui 层（L1）：Run → report.md（§7.6）
#include <string>

#include "graph/mermaid.h"
#include "parse/model.h"

namespace axr {

std::string buildReport(const Run& run, const MermaidOptions& mo = MermaidOptions{});

} // namespace axr
