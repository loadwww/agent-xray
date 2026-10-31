#pragma once
// 算法三：superstep 归并（§7.3.3）
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// 工具失败分级（P2-7）。判定顺序：结果 JSON 含 error → 协议层 is_error → 结果文本启发式。
// 返回 none | inferred | confirmed；errorPatterns 非空时整体覆盖内置词表。
std::string classifyToolError(const std::string& result, bool protocolIsError,
                              const std::vector<std::string>& errorPatterns = {});

// cur 相对 prev 新增的 tool 消息 → 上一响应触发的工具执行
std::vector<ToolExec> toolsFromDelta(const std::vector<Message>& prevMsgs,
                                     const std::vector<Message>& curMsgs,
                                     const std::vector<ToolCall>& prevRespCalls,
                                     int approxDurationMs,
                                     const std::vector<std::string>& errorPatterns = {});

// 终止型工具：仅用于交付最终答案、没有真实副作用（如 smolagents 的 final_answer）
bool isTerminalTool(const std::string& name);

// 组装单个 Step
Step buildStep(int index, const LLMCall& call, const LLMResponse& resp, std::vector<ToolExec> tools);

} // namespace axr
