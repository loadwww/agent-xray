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

bool isTerminalTool(const std::string& name) {
    // 部分 agent 框架用"工具调用"来交付最终答案（smolagents 的 final_answer），
    // 这类调用不产生真实工具执行，也不应有后续请求
    return name == "final_answer";
}

Step buildStep(int index, const LLMCall& call, const LLMResponse& resp, std::vector<ToolExec> tools) {
    Step s;
    s.index = index;
    s.call = call;
    s.response = resp;

    // 响应里有调用、但后续请求没回填 tool 结果的，补成"仅调用信息"：
    // 有些 agent（smolagents）把工具结果以文本 Observation 回填，协议层看不到 tool 消息
    for (const auto& tc : resp.toolCalls) {
        if (isTerminalTool(tc.name)) continue;
        bool found = false;
        for (const auto& te : tools)
            if (!te.callId.empty() && te.callId == tc.id) found = true;
        if (found) continue;
        ToolExec te;
        te.callId = tc.id;
        te.name = tc.name;
        te.arguments = tc.arguments;
        te.approx = true;
        tools.push_back(te);
    }

    s.tools = std::move(tools);
    s.tokens = resp.usage.promptTokens + resp.usage.completionTokens;
    bool onlyTerminal = true;
    for (const auto& tc : resp.toolCalls)
        if (!isTerminalTool(tc.name)) onlyTerminal = false;
    s.kind = (resp.toolCalls.empty() || onlyTerminal) ? "final" : "llm";
    return s;
}

} // namespace axr
