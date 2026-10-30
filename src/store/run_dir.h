#pragma once
// run 目录与文件写入（§6.1 目录布局）
#include <string>

namespace axr {

// 会话 id：YYYYMMDD-HHMMSS-<4位随机十六进制>
std::string makeSessionId();

// 创建 <root>/runs/<sessionId> 并返回路径（失败返回空串）
std::string ensureRunDir(const std::string& root, const std::string& sessionId);

// 覆盖写入文本文件
bool writeTextFile(const std::string& path, const std::string& content);

} // namespace axr
