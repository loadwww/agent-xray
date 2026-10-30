#include "chain/validate.h"

#include "chain/superstep.h"

namespace axr {

bool validate(const Run& r, std::vector<std::string>& missing) {
    missing.clear();

    for (std::size_t i = 1; i < r.steps.size(); ++i) {
        const int delta = static_cast<int>(r.steps[i].call.messages.size()) -
                          static_cast<int>(r.steps[i - 1].call.messages.size());
        // 正常：新增 1 条 assistant + N 条 tool（N >= 0）
        if (delta < 1) missing.push_back("step " + std::to_string(i + 1) + ": 消息数未增长");
    }

    for (std::size_t i = 0; i < r.steps.size(); ++i) {
        for (const auto& te : r.steps[i].tools) {
            if (te.callId.empty()) {
                missing.push_back("step " + std::to_string(i + 1) + ": 孤立 tool 结果");
            } else if (findToolCall(r.steps[i].response.toolCalls, te.callId) == nullptr) {
                missing.push_back("step " + std::to_string(i + 1) + ": tool 结果无法对上 tool_call id");
            }
        }
    }

    // 最后一步若仍要求工具但没有后续请求 → 采集不完整（纯流量视角的固有缺口）
    // 例外：终止型工具（final_answer 等）本身就是交最终答案，不需要后续请求
    if (!r.steps.empty() && !r.steps.back().response.toolCalls.empty()) {
        bool onlyTerminal = true;
        for (const auto& tc : r.steps.back().response.toolCalls)
            if (!isTerminalTool(tc.name)) onlyTerminal = false;
        if (!onlyTerminal) missing.push_back("step " + std::to_string(r.steps.size()) + ": 末尾 tool_calls 无后续请求");
    }

    return missing.empty();
}

} // namespace axr
