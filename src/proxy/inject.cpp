#include "proxy/inject.h"

#include <algorithm>
#include <cctype>

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

bool shouldRetryWithoutUsage(int status, const std::string& responseBody) {
    if (status != 400 || responseBody.empty()) return false;
    std::string low = responseBody;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low.find("stream_options") != std::string::npos;
}

bool rewriteModel(std::string& bodyJson, const std::map<std::string, std::string>& modelMap,
                  std::string* applied) {
    if (modelMap.empty()) return false;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(bodyJson);
    } catch (...) {
        return false;
    }
    if (!body.is_object() || !body.contains("model") || !body["model"].is_string()) return false;
    const std::string from = body["model"].get<std::string>();
    auto it = modelMap.find(from);
    if (it == modelMap.end() || it->second.empty()) return false;
    body["model"] = it->second;
    bodyJson = body.dump();
    if (applied) *applied = from + " -> " + it->second;
    return true;
}

} // namespace axr
