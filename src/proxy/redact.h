#pragma once
// 脱敏（§7.1.7）：只作用于落盘副本，转发给上游的请求必须用原始值
#include <map>
#include <string>
#include <vector>

namespace axr {

extern const char* kRedacted;

// 头名大小写不敏感；命中的值替换为 ***
std::map<std::string, std::string> redactHeaders(const std::map<std::string, std::string>& in,
                                                 const std::vector<std::string>& keys);

// 按配置的正则（ECMAScript）替换 body 中的密钥
std::string redactBody(const std::string& body, const std::vector<std::string>& patterns);

// P2-13 summary 采集模式：不落正文，只保留长度 + sha256 前 16 位
std::string summarizeBody(const std::string& body);

} // namespace axr
