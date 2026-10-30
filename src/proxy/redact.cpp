#include "proxy/redact.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "common/log.h"

namespace axr {

const char* kRedacted = "***";

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::map<std::string, std::string> redactHeaders(const std::map<std::string, std::string>& in,
                                                 const std::vector<std::string>& keys) {
    std::map<std::string, std::string> out;
    for (const auto& kv : in) {
        bool hit = false;
        for (const auto& k : keys) {
            if (lower(kv.first) == lower(k)) {
                hit = true;
                break;
            }
        }
        out[kv.first] = hit ? std::string(kRedacted) : kv.second;
    }
    return out;
}

std::string redactBody(const std::string& body, const std::vector<std::string>& patterns) {
    if (body.empty() || patterns.empty()) return body;
    std::string out = body;
    for (const auto& p : patterns) {
        if (p.empty()) continue;
        try {
            const std::regex re(p, std::regex::ECMAScript);
            out = std::regex_replace(out, re, kRedacted);
        } catch (const std::regex_error& e) {
            logWarn("redact", "非法脱敏正则 [" + p + "]: " + e.what());
        }
    }
    return out;
}

} // namespace axr
