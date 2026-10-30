#pragma once
// 统一日志：输出到 stderr，--verbose 才打印 debug
#include <string>

namespace axr {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void setVerbose(bool v);
bool verbose();

void log(LogLevel level, const std::string& tag, const std::string& msg);

inline void logInfo(const std::string& tag, const std::string& msg) { log(LogLevel::Info, tag, msg); }
inline void logWarn(const std::string& tag, const std::string& msg) { log(LogLevel::Warn, tag, msg); }
inline void logError(const std::string& tag, const std::string& msg) { log(LogLevel::Error, tag, msg); }
inline void logDebug(const std::string& tag, const std::string& msg) { log(LogLevel::Debug, tag, msg); }

} // namespace axr
