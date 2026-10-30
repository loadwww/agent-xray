#include "chain/fingerprint.h"

#include <cctype>

namespace axr {
namespace {

// 归一化文本：CRLF → LF，首尾裁剪，内部连续空白折叠为单空格
std::string normalizeText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || c == ' ') {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
            continue;
        }
        out.push_back(c);
        prevSpace = false;
    }
    // 裁剪首尾空格
    std::size_t b = 0, e = out.size();
    while (b < e && out[b] == ' ') ++b;
    while (e > b && out[e - 1] == ' ') --e;
    return out.substr(b, e - b);
}

// 参数串只做裁剪，不解析（保持原始字符串，不同 provider 差异留到以后）
std::string normalizeArgs(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

constexpr char kSep = '\x1f';

} // namespace

std::string canonical(const Message& m) {
    std::string out;
    out += toStr(m.role);
    out.push_back(kSep);
    for (const auto& c : m.content) {
        if (c.type == "text") {
            out += "t:";
            out += normalizeText(c.text);
        } else {
            // 图片只以 digest 参与，天然去掉了会变化的 URL / base64
            out += "i:";
            out += c.digest;
        }
        out.push_back(kSep);
    }
    // assistant 的 tool_calls：id 是随机量 → 丢弃，只保留 name + arguments
    for (const auto& tc : m.toolCalls) {
        out += "tc:";
        out += tc.name;
        out.push_back('\x1e');
        out += normalizeArgs(tc.arguments);
        out.push_back(kSep);
    }
    // tool 消息的 tool_call_id 同样是随机量 → 丢弃；is_error 参与判定
    if (m.isError) out += "err:1";
    out.push_back(kSep);
    return out;
}

std::string canonicalMessages(const std::vector<Message>& msgs) {
    std::string out;
    for (const auto& m : msgs) {
        out += canonical(m);
        out.push_back('\x1d');
    }
    return out;
}

} // namespace axr
