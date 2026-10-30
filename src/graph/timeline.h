#pragma once
// graph 层：时间线（markdown 表格，不上 SVG，§7.4）
#include <string>

#include "parse/model.h"

namespace axr {

std::string toMarkdownTimeline(const Run& run);

} // namespace axr
