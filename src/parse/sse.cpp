#include "parse/sse.h"

#include <cctype>

namespace axr {
namespace {

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string trimLeftCopy(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string trimRightCopy(const std::string& s) {
    std::size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(0, e);
}

} // namespace

void SseAccumulator::feed(const std::string& chunk) {
    buf_ += chunk;
    // 逐行处理完整行；半行留在缓冲里（边界 2：一行 data 被拆到两个 chunk）
    while (true) {
        const std::size_t pos = buf_.find('\n');
        if (pos == std::string::npos) break;
        std::string line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        processLine(trimRightCopy(line));
    }
}

void SseAccumulator::finish() {
    if (!buf_.empty()) {
        processLine(trimRightCopy(buf_));
        buf_.clear();
    }
}

void SseAccumulator::processLine(const std::string& rawLine) {
    if (rawLine.empty()) return; // 空行是事件分隔符（边界 7）
    if (done_) return;

    std::string line = rawLine;
    if (startsWith(line, "data:")) {
        line = trimLeftCopy(line.substr(5));
    } else if (startsWith(line, ":") || startsWith(line, "event:") || startsWith(line, "id:")) {
        return; // 注释行 / 心跳 / 事件名，忽略
    } else {
        ++unknownLines_; // 未知行：跳过并计数（§12 风险对策）
        return;
    }

    if (line == "[DONE]") { // 边界 3
        done_ = true;
        return;
    }
    if (line.empty()) return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        ++unknownLines_;
        return;
    }
    if (!j.is_object()) return;
    if (j.contains("usage") && !j["usage"].is_null()) applyUsage(j["usage"]);
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;

    const auto& choice = j["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) applyDelta(choice["delta"]);
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        finishReason_ = choice["finish_reason"].get<std::string>();
    }
}

void SseAccumulator::applyDelta(const nlohmann::json& delta) {
    if (delta.contains("content") && !delta["content"].is_null()) {
        if (delta["content"].is_string()) text_ += delta["content"].get<std::string>();
    }
    // 部分实现把最终 text 放在 message.content（非 delta），一并兼容
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            if (!tc.is_object()) continue;
            int idx = tc.value("index", 0);
            ToolCall& slot = toolCalls_[idx];
            if (tc.contains("id") && tc["id"].is_string()) slot.id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& f = tc["function"];
                if (f.contains("name") && f["name"].is_string()) slot.name = f["name"].get<std::string>();
                // ★增量字符串拼接，不是覆盖
                if (f.contains("arguments") && f["arguments"].is_string())
                    slot.arguments += f["arguments"].get<std::string>();
            }
        }
    }
}

void SseAccumulator::applyUsage(const nlohmann::json& usage) {
    usage_.observed = true;
    usage_.promptTokens = usage.value("prompt_tokens", 0);
    usage_.completionTokens = usage.value("completion_tokens", 0);
    if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
        usage_.cachedTokens = usage["prompt_tokens_details"].value("cached_tokens", 0);
    }
    if (usage.contains("cached_tokens") && usage["cached_tokens"].is_number()) {
        usage_.cachedTokens = usage["cached_tokens"].get<int>();
    }
}

LLMResponse SseAccumulator::take(const std::string& callId) const {
    LLMResponse resp;
    resp.callId = callId;
    resp.text = text_;
    resp.toolCalls.clear();
    for (const auto& kv : toolCalls_) resp.toolCalls.push_back(kv.second); // 按 index 有序
    resp.usage = usage_;
    resp.finishReason = finishReason_;
    resp.streamed = true;
    return resp;
}

} // namespace axr
