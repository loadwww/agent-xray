#pragma once
// sha256（自实现，避免为它引 OpenSSL，见设计文档 §7.3.1）
#include <string>

namespace axr {

// 返回 64 位十六进制小写字符串
std::string sha256Hex(const std::string& data);

} // namespace axr
