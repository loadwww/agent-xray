#pragma once
// events.jsonl 事件模型（§6.3）：append-only 原始事件流
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace axr {

struct Event {
    long long seq = 0;
    std::string ts;   // RFC3339（毫秒）
    std::string kind; // req | sse_chunk | sse_end | resp | err
    std::string hop;

    // req
    std::string upstream;
    std::string reqBody;
    std::map<std::string, std::string> reqHeaders;

    // sse_chunk
    std::string data;

    // resp
    int status = 0;
    long long latencyMs = 0;
    std::string respBody;

    // err
    std::string error;

    bool truncated = false;

    nlohmann::json toJson() const;
};

} // namespace axr
