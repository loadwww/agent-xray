#pragma once
// 算法一：Merkle 滚动前缀哈希（§7.3.1）
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// 返回长度 = msgs.size() + 1：
//   H[0] = sha256("root")
//   H[k] = sha256(H[k-1] + sha256(canonical(messages[k-1])))
// 取前 128bit（32 hex）作为键，碰撞概率可忽略（§12）
std::vector<std::string> prefixHashes(const std::vector<Message>& msgs);

} // namespace axr
