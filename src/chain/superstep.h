#pragma once
// 算法三：superstep 归并（§7.3.3）
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// cur 相对 prev 新增的 tool 消息 → 上一响应触发的工具执行
std::vector<ToolExec> toolsFromDelta(const std::vector<Message>& prevMsgs,
                                     const std::vector<Message>& curMsgs,
                                     const std::vector<ToolCall>& prevRespCalls,
                                     int approxDurationMs);

// 组装单个 Step
Step buildStep(int index, const LLMCall& call, const LLMResponse& resp, std::vector<ToolExec> tools);

} // namespace axr
