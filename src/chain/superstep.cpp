#include "chain/superstep.h"

namespace axr {

std::vector<ToolExec> toolsFromDelta(const std::vector<Message>& prevMsgs,
                                     const std::vector<Message>& curMsgs,
                                     const std::vector<ToolCall>& prevRespCalls,
                                     int approxDurationMs) {
    std::vector<ToolExec> out;
    if (curMsgs.size() <= prevMsgs.size()) return out;

    for (std::size_t i = prevMsgs.size(); i < curMsgs.size(); ++i) {
        const Message& m = curMsgs[i];
        if (m.role != Role::Tool) continue;

        ToolExec te;
        te.callId = m.toolCallId;
        te.result = firstText(m);
        te.isError = m.isError;
        te.durationMs = approxDurationMs;
        te.approx = true; // 无 MCP 代理时恒为推断值

        const ToolCall* tc = findToolCall(prevRespCalls, m.toolCallId);
        if (tc) {
            te.name = tc->name;
            te.arguments = tc->arguments;
        }
        out.push_back(te);
    }
    return out;
}

Step buildStep(int index, const LLMCall& call, const LLMResponse& resp, std::vector<ToolExec> tools) {
    Step s;
    s.index = index;
    s.call = call;
    s.response = resp;
    s.tools = std::move(tools);
    s.tokens = resp.usage.promptTokens + resp.usage.completionTokens;
    s.kind = resp.toolCalls.empty() ? "final" : "llm";
    return s;
}

} // namespace axr
