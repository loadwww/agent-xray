#include "proxy/event.h"

namespace axr {

nlohmann::json Event::toJson() const {
    nlohmann::json j;
    j["seq"] = seq;
    j["ts"] = ts;
    j["kind"] = kind;
    j["hop"] = hop;

    if (kind == "req") {
        j["upstream"] = upstream;
        j["req_body"] = reqBody;
        nlohmann::json h = nlohmann::json::object();
        for (const auto& kv : reqHeaders) h[kv.first] = kv.second;
        j["req_headers"] = h;
        if (truncated) j["truncated"] = true;
    } else if (kind == "sse_chunk") {
        j["data"] = data;
    } else if (kind == "resp") {
        j["status"] = status;
        j["latency_ms"] = latencyMs;
        if (!respBody.empty()) j["resp_body"] = respBody;
        if (truncated) j["truncated"] = true;
    } else if (kind == "err") {
        j["error"] = error;
        if (status > 0) j["status"] = status;
    }
    return j;
}

} // namespace axr
