#pragma once
// 消息规范化（canonical）：去掉随机 id / 空白，产出稳定字节串，供前缀哈希使用
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// 单条消息 → 稳定字符串
std::string canonical(const Message& m);

// 整套 messages → 稳定字符串（调试用）
std::string canonicalMessages(const std::vector<Message>& msgs);

} // namespace axr
