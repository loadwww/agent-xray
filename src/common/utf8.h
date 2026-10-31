#pragma once

#include <cstddef>
#include <string>

namespace axr {

// 按字节预算截断 UTF-8 字符串：截断点落在多字节字符中间时回退到该字符起点，
// 再追加省略号。保证结果始终是合法 UTF-8 —— 否则 nlohmann::json::dump 会抛
// type_error.316，曾导致 /api/run 在长中文正文下 500。
inline std::string utf8Truncate(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t end = maxBytes;
    // 续字节（10xxxxxx）不能作为截断边界，回退到字符首字节
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    // end < maxBytes 时 s[end] 必为某多字节字符首字节，该字符被切开，整字符丢弃；
    // end == maxBytes 时截断点恰好在字符边界上，原样保留
    return s.substr(0, end) + "…";
}

} // namespace axr
