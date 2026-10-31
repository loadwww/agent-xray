#include "parse/openai.h"

#include <cctype>

#include "common/hash.h"

namespace axr {
namespace {

std::string trimCopy(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

Content parseContentItem(const nlohmann::json& item) {
    Content c;
    if (item.is_string()) {
        c.type = "text";
        c.text = item.get<std::string>();
        return c;
    }
    if (!item.is_object()) {
        c.type = "text";
        return c;
    }
    const std::string type = item.value("type", "text");
    if (type == "text" || type == "input_text" || type == "output_text") {
        c.type = "text";
        if (item.contains("text") && item["text"].is_string()) c.text = item["text"].get<std::string>();
        return c;
    }
    // 图像：只留元信息与 digest，绝不保存 base64 正文（§5.3 硬约束）
    if (type == "image_url" || type == "image") {
        c.type = "image";
        std::string url;
        if (item.contains("image_url")) {
            const auto& iu = item["image_url"];
            if (iu.is_string()) url = iu.get<std::string>();
            else if (iu.is_object() && iu.contains("url") && iu["url"].is_string())
                url = iu["url"].get<std::string>();
        } else if (item.contains("url") && item["url"].is_string()) {
            url = item["url"].get<std::string>();
        }
        // data:base64 形态只对负载做摘要，不落正文
        const std::size_t comma = url.find(',');
        const std::string payload = (url.rfind("data:", 0) == 0 && comma != std::string::npos) ? url.substr(comma + 1) : url;
        c.digest = sha256Hex(payload).substr(0, 16);
        c.bytes = 0;
        const std::size_t semi = url.find(';');
        if (url.rfind("data:", 0) == 0 && semi != std::string::npos && semi > 5) c.mime = url.substr(5, semi - 5);
        return c;
    }
    c.type = "text";
    if (item.contains("text") && item["text"].is_string()) c.text = item["text"].get<std::string>();
    return c;
}

void parseUsage(const nlohmann::json& j, Usage& u) {
    if (!j.is_object()) return;
    u.observed = true;
    u.promptTokens = j.value("prompt_tokens", 0);
    u.completionTokens = j.value("completion_tokens", 0);
    if (j.contains("prompt_tokens_details") && j["prompt_tokens_details"].is_object()) {
        u.cachedTokens = j["prompt_tokens_details"].value("cached_tokens", 0);
    }
    if (j.contains("cached_tokens") && j["cached_tokens"].is_number()) {
        u.cachedTokens = j["cached_tokens"].get<int>();
    }
}

} // namespace

bool isStreamRequest(const nlohmann::json& body) {
    return body.is_object() && body.contains("stream") && body["stream"].is_boolean() && body["stream"].get<bool>();
}

Message parseOpenAiMessage(const nlohmann::json& m) {
    Message msg;
    if (!m.is_object()) return msg;
    msg.role = fromStr(m.value("role", "user"));

    if (m.contains("content") && !m["content"].is_null()) {
        const auto& c = m["content"];
        if (c.is_string()) {
            Content one;
            one.type = "text";
            one.text = c.get<std::string>();
            msg.content.push_back(one);
        } else if (c.is_array()) {
            for (const auto& item : c) msg.content.push_back(parseContentItem(item));
        }
    }

    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) {
            if (!tc.is_object()) continue;
            ToolCall call;
            call.id = tc.value("id", "");
            if (tc.contains("function") && tc["function"].is_object()) {
                call.name = tc["function"].value("name", "");
                if (tc["function"].contains("arguments")) {
                    const auto& a = tc["function"]["arguments"];
                    call.arguments = a.is_string() ? a.get<std::string>() : a.dump();
                }
            }
            msg.toolCalls.push_back(call);
        }
    }

    msg.toolCallId = m.value("tool_call_id", "");
    return msg;
}

Result<LLMCall> parseOpenAiRequest(const nlohmann::json& body, const std::string& endpoint) {
    if (!body.is_object()) return Result<LLMCall>::Fail("请求体不是 JSON 对象");
    if (!body.contains("messages") || !body["messages"].is_array())
        return Result<LLMCall>::Fail("请求体缺少 messages");

    LLMCall call;
    call.provider = "openai";
    call.endpoint = endpoint;
    call.model = body.value("model", std::string());

    for (const auto& m : body["messages"]) call.messages.push_back(parseOpenAiMessage(m));

    if (body.contains("tools") && body["tools"].is_array()) {
        for (const auto& t : body["tools"]) {
            if (!t.is_object()) continue;
            // 只接受 function 形态
            std::string type = t.value("type", "function");
            if (type != "function") continue;
            ToolDef def;
            if (t.contains("function") && t["function"].is_object()) {
                const auto& f = t["function"];
                def.name = f.value("name", "");
                def.description = f.value("description", "");
                if (f.contains("parameters")) def.schemaJson = f["parameters"].dump();
            }
            if (!def.name.empty()) call.tools.push_back(def);
        }
    }

    // 其余参数原样保留
    nlohmann::json params = nlohmann::json::object();
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (it.key() == "messages" || it.key() == "tools" || it.key() == "model") continue;
        params[it.key()] = it.value();
    }
    call.params = params;
    return Result<LLMCall>::Ok(call);
}

Result<LLMResponse> parseOpenAiResponse(const nlohmann::json& body) {
    LLMResponse resp;
    if (!body.is_object()) return Result<LLMResponse>::Fail("响应体不是 JSON 对象");

    if (body.contains("error") && !body["error"].is_null()) {
        resp.err = body["error"].is_string() ? body["error"].get<std::string>() : body["error"].dump();
        return Result<LLMResponse>::Ok(resp);
    }

    if (!body.contains("choices") || !body["choices"].is_array() || body["choices"].empty()) {
        return Result<LLMResponse>::Fail("响应体缺少 choices");
    }

    const auto& choice = body["choices"][0];
    resp.finishReason = choice.value("finish_reason", "");
    if (choice.contains("message") && choice["message"].is_object()) {
        const Message msg = parseOpenAiMessage(choice["message"]);
        for (const auto& c : msg.content) {
            if (c.type == "text") resp.text += c.text;
        }
        resp.toolCalls = msg.toolCalls;
    }

    if (body.contains("usage") && !body["usage"].is_null()) parseUsage(body["usage"], resp.usage);
    return Result<LLMResponse>::Ok(resp);
}

} // namespace axr
