#include "parse/model.h"

namespace axr {
namespace {

template <typename T>
T valueOr(const nlohmann::json& j, const char* key, T def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

std::string strOr(const nlohmann::json& j, const char* key, const std::string& def = "") {
    return valueOr<std::string>(j, key, def);
}

} // namespace

std::string toStr(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "user";
}

Role fromStr(const std::string& s) {
    if (s == "system") return Role::System;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    return Role::User;
}

// ---------- Content ----------
void to_json(nlohmann::json& j, const Content& c) {
    j = nlohmann::json{{"type", c.type}, {"text", c.text}, {"mime", c.mime}, {"bytes", c.bytes}, {"digest", c.digest}};
}
void from_json(const nlohmann::json& j, Content& c) {
    c.type = strOr(j, "type", "text");
    c.text = strOr(j, "text");
    c.mime = strOr(j, "mime");
    c.bytes = valueOr<int>(j, "bytes", 0);
    c.digest = strOr(j, "digest");
}

// ---------- ToolCall ----------
void to_json(nlohmann::json& j, const ToolCall& c) {
    j = nlohmann::json{{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}};
}
void from_json(const nlohmann::json& j, ToolCall& c) {
    c.id = strOr(j, "id");
    c.name = strOr(j, "name");
    c.arguments = strOr(j, "arguments");
}

// ---------- ToolDef ----------
void to_json(nlohmann::json& j, const ToolDef& c) {
    j = nlohmann::json{{"name", c.name}, {"description", c.description}, {"schema", c.schemaJson}};
}
void from_json(const nlohmann::json& j, ToolDef& c) {
    c.name = strOr(j, "name");
    c.description = strOr(j, "description");
    c.schemaJson = strOr(j, "schema");
}

// ---------- Message ----------
void to_json(nlohmann::json& j, const Message& m) {
    j = nlohmann::json{{"role", toStr(m.role)}, {"content", m.content}, {"tool_calls", m.toolCalls},
                       {"tool_call_id", m.toolCallId}, {"is_error", m.isError}};
}
void from_json(const nlohmann::json& j, Message& m) {
    m.role = fromStr(strOr(j, "role", "user"));
    m.content.clear();
    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& item : j["content"]) m.content.push_back(item.get<Content>());
    }
    m.toolCalls.clear();
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& item : j["tool_calls"]) m.toolCalls.push_back(item.get<ToolCall>());
    }
    m.toolCallId = strOr(j, "tool_call_id");
    m.isError = valueOr<bool>(j, "is_error", false);
}

// ---------- Usage ----------
void to_json(nlohmann::json& j, const Usage& u) {
    j = nlohmann::json{{"prompt_tokens", u.promptTokens},
                       {"completion_tokens", u.completionTokens},
                       {"cached_tokens", u.cachedTokens}};
}
void from_json(const nlohmann::json& j, Usage& u) {
    u.promptTokens = valueOr<int>(j, "prompt_tokens", 0);
    u.completionTokens = valueOr<int>(j, "completion_tokens", 0);
    u.cachedTokens = valueOr<int>(j, "cached_tokens", 0);
}

// ---------- LLMCall ----------
void to_json(nlohmann::json& j, const LLMCall& c) {
    j = nlohmann::json{{"id", c.id},           {"provider", c.provider}, {"model", c.model},
                       {"endpoint", c.endpoint}, {"messages", c.messages},  {"tools", c.tools},
                       {"params", c.params},    {"started_at", c.startedAt}, {"ended_at", c.endedAt}};
}
void from_json(const nlohmann::json& j, LLMCall& c) {
    c.id = strOr(j, "id");
    c.provider = strOr(j, "provider", "openai");
    c.model = strOr(j, "model");
    c.endpoint = strOr(j, "endpoint");
    c.messages.clear();
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& item : j["messages"]) c.messages.push_back(item.get<Message>());
    }
    c.tools.clear();
    if (j.contains("tools") && j["tools"].is_array()) {
        for (const auto& item : j["tools"]) c.tools.push_back(item.get<ToolDef>());
    }
    if (j.contains("params") && j["params"].is_object()) c.params = j["params"];
    else c.params = nlohmann::json::object();
    c.startedAt = strOr(j, "started_at");
    c.endedAt = strOr(j, "ended_at");
}

// ---------- LLMResponse ----------
void to_json(nlohmann::json& j, const LLMResponse& r) {
    j = nlohmann::json{{"call_id", r.callId}, {"text", r.text},       {"tool_calls", r.toolCalls},
                       {"usage", r.usage},     {"finish_reason", r.finishReason},
                       {"streamed", r.streamed}, {"error", r.err},    {"latency_ms", r.latencyMs}};
}
void from_json(const nlohmann::json& j, LLMResponse& r) {
    r.callId = strOr(j, "call_id");
    r.text = strOr(j, "text");
    r.toolCalls.clear();
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& item : j["tool_calls"]) r.toolCalls.push_back(item.get<ToolCall>());
    }
    if (j.contains("usage") && j["usage"].is_object()) r.usage = j["usage"].get<Usage>();
    r.finishReason = strOr(j, "finish_reason");
    r.streamed = valueOr<bool>(j, "streamed", false);
    r.err = strOr(j, "error");
    r.latencyMs = valueOr<int>(j, "latency_ms", 0);
}

// ---------- ToolExec ----------
void to_json(nlohmann::json& j, const ToolExec& t) {
    j = nlohmann::json{{"call_id", t.callId}, {"name", t.name},         {"arguments", t.arguments},
                       {"result", t.result},  {"is_error", t.isError},   {"duration_ms", t.durationMs},
                       {"approx", t.approx}};
}
void from_json(const nlohmann::json& j, ToolExec& t) {
    t.callId = strOr(j, "call_id");
    t.name = strOr(j, "name");
    t.arguments = strOr(j, "arguments");
    t.result = strOr(j, "result");
    t.isError = valueOr<bool>(j, "is_error", false);
    t.durationMs = valueOr<int>(j, "duration_ms", 0);
    t.approx = valueOr<bool>(j, "approx", true);
}

// ---------- Step ----------
void to_json(nlohmann::json& j, const Step& s) {
    j = nlohmann::json{{"index", s.index}, {"call", s.call},   {"response", s.response},
                       {"tools", s.tools}, {"tokens", s.tokens}, {"cost_usd", s.costUsd},
                       {"kind", s.kind}};
}
void from_json(const nlohmann::json& j, Step& s) {
    s.index = valueOr<int>(j, "index", 0);
    if (j.contains("call") && j["call"].is_object()) s.call = j["call"].get<LLMCall>();
    if (j.contains("response") && j["response"].is_object()) s.response = j["response"].get<LLMResponse>();
    s.tools.clear();
    if (j.contains("tools") && j["tools"].is_array()) {
        for (const auto& item : j["tools"]) s.tools.push_back(item.get<ToolExec>());
    }
    s.tokens = valueOr<int>(j, "tokens", 0);
    s.costUsd = valueOr<double>(j, "cost_usd", 0.0);
    s.kind = strOr(j, "kind");
}

// ---------- Run ----------
void to_json(nlohmann::json& j, const Run& r) {
    j = nlohmann::json{{"id", r.id},
                       {"root_hash", r.rootHash},
                       {"started_at", r.startedAt},
                       {"ended_at", r.endedAt},
                       {"steps", r.steps},
                       {"total", r.total},
                       {"cost_usd", r.costUsd},
                       {"incomplete", r.incomplete},
                       {"missing", r.missing},
                       {"children", r.children},
                       {"tags", r.tags}};
}
void from_json(const nlohmann::json& j, Run& r) {
    r.id = strOr(j, "id");
    r.rootHash = strOr(j, "root_hash");
    r.startedAt = strOr(j, "started_at");
    r.endedAt = strOr(j, "ended_at");
    r.steps.clear();
    if (j.contains("steps") && j["steps"].is_array()) {
        for (const auto& item : j["steps"]) r.steps.push_back(item.get<Step>());
    }
    if (j.contains("total") && j["total"].is_object()) r.total = j["total"].get<Usage>();
    r.costUsd = valueOr<double>(j, "cost_usd", 0.0);
    r.incomplete = valueOr<bool>(j, "incomplete", false);
    r.missing.clear();
    if (j.contains("missing") && j["missing"].is_array()) {
        for (const auto& item : j["missing"]) r.missing.push_back(item.get<std::string>());
    }
    r.children.clear();
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& item : j["children"]) r.children.push_back(item.get<Run>());
    }
    r.tags.clear();
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& item : j["tags"]) r.tags.push_back(item.get<std::string>());
    }
}

// ---------- helpers ----------
std::string firstText(const Message& m) {
    for (const auto& c : m.content) {
        if (c.type == "text") return c.text;
    }
    return "";
}

const ToolCall* findToolCall(const std::vector<ToolCall>& calls, const std::string& id) {
    for (const auto& c : calls) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

} // namespace axr
