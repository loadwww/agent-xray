#pragma once
// SSE 状态机：把逐 chunk 的流式响应还原成完整 LLMResponse（§7.2.2，最容易出 bug 的地方）
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "parse/model.h"

namespace axr {

class SseAccumulator {
public:
    // 收到一段原始 SSE 字节（可能半行、可能多行）
    void feed(const std::string& chunk);

    // 流结束时调用，处理残留缓冲
    void finish();

    bool done() const { return done_; }
    int unknownLines() const { return unknownLines_; }

    // 产出累积结果（callId 由调用方赋值）
    LLMResponse take(const std::string& callId) const;

private:
    void processLine(const std::string& line);
    void applyDelta(const nlohmann::json& delta);
    void applyUsage(const nlohmann::json& usage);

    std::string buf_;
    std::string text_;
    std::map<int, ToolCall> toolCalls_;
    Usage usage_;
    std::string finishReason_;
    bool done_ = false;
    int unknownLines_ = 0;
};

} // namespace axr
