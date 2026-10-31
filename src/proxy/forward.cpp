#include "proxy/forward.h"

#include <algorithm>
#include <cctype>

namespace axr {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

bool isHttpsUrl(const std::string& url) {
    return lower(url).rfind("https://", 0) == 0;
}

// forwardHttp 的平台实现：
//   Windows → forward_winhttp.cpp（系统内置 WinHTTP，零第三方依赖）
//   其他    → forward_curl.cpp（libcurl easy handle）

} // namespace axr