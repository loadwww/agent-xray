#include "proxy/inject.h"

#include <nlohmann/json.hpp>

namespace axr {

bool injectStreamUsage(std::string& bodyJson) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(bodyJson);
    } catch (...) {
        return false; // 非 JSON 原样转发，交给上游判断
    }
    if (!body.is_object() || !body.contains("stream") || !body["stream"].is_boolean() ||
        !body["stream"].get<bool>()) {
        return false;
    }
    if (body.contains("stream_options") && body["stream_options"].is_object() &&
        body["stream_options"].value("include_usage", false)) {
        return false; // 客户端已经要求了 usage
    }
    body["stream_options"]["include_usage"] = true;
    bodyJson = body.dump();
    return true;
}

} // namespace axr
