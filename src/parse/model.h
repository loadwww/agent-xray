#pragma once
// ★§5 统一数据结构：全项目围绕本文件展开，改动会引发返工，先定稿再写逻辑★
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace axr {

enum class Role { System, User, Assistant, Tool };

std::string toStr(Role r);
Role fromStr(const std::string& s);

// 多模态只存元信息，绝不把 base64 塞进结构体
struct Content {
    std::string type;      // "text" | "image"
    std::string text;      // type=="text" 时有效
    std::string mime;      // type=="image"
    int bytes = 0;
    std::string digest;    // 图片内容的 sha256 前16位
};

struct ToolCall {
    std::string id;        // OpenAI: tool_call.id
    std::string name;
    std::string arguments; // 原始 JSON 字符串（流式拼接后的完整值）
};

struct ToolDef {
    std::string name;
    std::string description;
    std::string schemaJson; // parameters 的原始 JSON
};

struct Message {
    Role role = Role::User;
    std::vector<Content> content;
    std::vector<ToolCall> toolCalls;   // 仅 assistant
    std::string toolCallId;            // 仅 tool
    bool isError = false;
};

struct Usage {
    int promptTokens = 0;
    int completionTokens = 0;
    int cachedTokens = 0;
    // 上游是否真的给了 usage 对象（P2-9）：false 时 0 token 只是"未观测"，展示为 "—"
    bool observed = false;
};

struct LLMCall {
    std::string id;         // 内部生成：run 内唯一
    std::string provider;   // "openai"（含 deepseek/ollama/vllm）
    std::string model;
    std::string endpoint;   // 上游真实 URL
    std::vector<Message> messages;
    std::vector<ToolDef> tools;
    nlohmann::json params = nlohmann::json::object(); // temperature / max_tokens / tool_choice 等原样保存
    std::string startedAt;  // RFC3339
    std::string endedAt;
};

struct LLMResponse {
    std::string callId;
    std::string text;
    std::vector<ToolCall> toolCalls;
    Usage usage;
    std::string finishReason;
    bool streamed = false;
    std::string err;
    int latencyMs = 0;
};

struct ToolExec {
    std::string callId;
    std::string name;
    std::string arguments;
    std::string result;
    // 失败等级（判定逻辑见 chain/superstep.h 的 classifyToolError）：
    // none = 未发现失败；inferred = 按结果文本推断；confirmed = 结果 JSON 明确报错 / 协议层 is_error
    std::string errorKind = "none";
    int durationMs = 0;
    // 耗时来源：inferred = 由相邻请求时间差推断（流量推断）；mcp = MCP 代理实测；otel = OTel span 实测
    std::string durationSource = "inferred";
};

struct Step {
    int index = 0;
    LLMCall call;
    LLMResponse response;
    std::vector<ToolExec> tools;   // 本轮并行触发的工具
    int tokens = 0;
    std::string kind;              // "llm" | "final"
    bool fuzzyIn = false;          // 与上一步的连接来自模糊匹配（§7.3.4，图上虚线标注"推测"）
};

struct Run {
    std::string id;        // 时间 + 根哈希前8位
    std::string rootHash;
    std::string startedAt;
    std::string endedAt;
    std::vector<Step> steps;
    Usage total;
    bool incomplete = false;          // ★完整性校验结果
    std::vector<std::string> missing; // 缺失步骤的描述
    std::vector<Run> children;        // 并行子链（本期预留，不做树展开）
    std::vector<std::string> tags;
    int requestCount = 0;  // 归入本 run 的原始 HTTP 请求数（含重试）
    int retryCount = 0;    // 其中完整哈希重复的重试请求数（不进 steps）
    int fuzzyEdges = 0;    // 本 run 内模糊匹配边数
};

// ---- JSON 互转（写在 model.cpp；字段缺失用默认值，不抛异常）----
void to_json(nlohmann::json& j, const Content& c);
void from_json(const nlohmann::json& j, Content& c);
void to_json(nlohmann::json& j, const ToolCall& c);
void from_json(const nlohmann::json& j, ToolCall& c);
void to_json(nlohmann::json& j, const ToolDef& c);
void from_json(const nlohmann::json& j, ToolDef& c);
void to_json(nlohmann::json& j, const Message& c);
void from_json(const nlohmann::json& j, Message& c);
void to_json(nlohmann::json& j, const Usage& c);
void from_json(const nlohmann::json& j, Usage& c);
void to_json(nlohmann::json& j, const LLMCall& c);
void from_json(const nlohmann::json& j, LLMCall& c);
void to_json(nlohmann::json& j, const LLMResponse& c);
void from_json(const nlohmann::json& j, LLMResponse& c);
void to_json(nlohmann::json& j, const ToolExec& c);
void from_json(const nlohmann::json& j, ToolExec& c);
void to_json(nlohmann::json& j, const Step& c);
void from_json(const nlohmann::json& j, Step& c);
void to_json(nlohmann::json& j, const Run& c);
void from_json(const nlohmann::json& j, Run& c);

// ---- 小工具 ----
// 取消息中第一段文本（tool 结果的便捷读取）
std::string firstText(const Message& m);
// 按 id 找 tool_call
const ToolCall* findToolCall(const std::vector<ToolCall>& calls, const std::string& id);
// 消息是否带 tool_calls
inline bool hasToolCalls(const Message& m) { return !m.toolCalls.empty(); }

} // namespace axr
