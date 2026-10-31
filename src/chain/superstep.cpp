#include "chain/superstep.h"

#include <algorithm>
#include <cctype>

namespace axr {
namespace {

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// 内置词表：工具结果以文本回填时（如 AutoGPT 的 "Error: ... Error code: 403 Forbidden"）的兜底信号
const std::vector<std::string>& builtinErrorPatterns() {
    static const std::vector<std::string> kPatterns = {
        "error:",      "error code", "traceback",  "exception",    "forbidden",  "unauthorized",
        "rate limit",  "timed out",  "timed-out",  "http 400",     "http 401",   "http 403",
        "http 404",    "http 429",   "http 500",   "http 502",     "http 503",   "status 400",
        "status 401",  "status 403", "status 404", "status 429",   "status 500", "unprocessable",
        "failed to",   "failed:",    "failure",    "unable to",    "denied",     "too much output",
    };
    return kPatterns;
}

// 结构化信号：结果本身就是 JSON 对象且带非空 error 字段（比文本匹配更可靠，视为确认失败）
bool jsonHasError(const std::string& text) {
    std::size_t b = 0;
    while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    if (b >= text.size() || text[b] != '{') return false;
    const nlohmann::json j = nlohmann::json::parse(text.substr(b), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto it = j.find("error");
    if (it == j.end() || it->is_null()) return false;
    if (it->is_string()) return !it->get<std::string>().empty();
    if (it->is_boolean()) return it->get<bool>();
    return true; // object / array / 数值等非空 error 均视为报错
}

bool textLooksFailed(const std::string& text, const std::vector<std::string>& patterns) {
    if (text.empty()) return false;
    const std::string head = text.size() > 600 ? text.substr(0, 600) : text;
    const std::string low = lowerAscii(head);
    const std::size_t p = low.find_first_not_of(" \t\r\n");
    if (p != std::string::npos && low.compare(p, 5, "error") == 0) return true;
    const std::vector<std::string>& pats = patterns.empty() ? builtinErrorPatterns() : patterns;
    for (const auto& pat : pats) {
        if (pat.empty()) continue;
        if (low.find(lowerAscii(pat)) != std::string::npos) return true;
    }
    return false;
}

} // namespace

std::string classifyToolError(const std::string& result, bool protocolIsError,
                              const std::vector<std::string>& errorPatterns) {
    if (jsonHasError(result)) return "confirmed";
    if (protocolIsError) return "confirmed";
    if (textLooksFailed(result, errorPatterns)) return "inferred";
    return "none";
}

std::vector<ToolExec> toolsFromDelta(const std::vector<Message>& prevMsgs,
                                     const std::vector<Message>& curMsgs,
                                     const std::vector<ToolCall>& prevRespCalls,
                                     int approxDurationMs,
                                     const std::vector<std::string>& errorPatterns) {
    std::vector<ToolExec> out;
    if (curMsgs.size() <= prevMsgs.size()) return out;

    for (std::size_t i = prevMsgs.size(); i < curMsgs.size(); ++i) {
        const Message& m = curMsgs[i];
        if (m.role != Role::Tool) continue;

        ToolExec te;
        te.callId = m.toolCallId;
        te.result = firstText(m);
        te.errorKind = classifyToolError(te.result, m.isError, errorPatterns);
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
