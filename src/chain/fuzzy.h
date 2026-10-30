#pragma once
// 算法四：模糊匹配兜底（§7.3.4）
#include <vector>

#include "chain/link.h"
#include "parse/model.h"

namespace axr {

// 前 window 条消息的逐条相等率；0.0 ~ 1.0
double messageSimilarity(const std::vector<Message>& a, const std::vector<Message>& b, int window);

// 为 calls[idx] 找模糊父节点；找不到返回 -1
int fuzzyParent(const std::vector<LLMCall>& calls, int idx, const ChainOptions& opts);

} // namespace axr
