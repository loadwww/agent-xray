#pragma once
// 时间工具：RFC3339 序列化/解析，毫秒差计算
#include <chrono>
#include <string>

#include "common/result.h"

namespace axr {

using TimePoint = std::chrono::system_clock::time_point;

// 2026-10-30T14:30:12.100+08:00（本地时区偏移）
std::string toRfc3339(TimePoint tp);
std::string nowRfc3339();

// 解析 RFC3339（支持 Z 与 ±HH:MM 偏移、可选小数秒）
Result<TimePoint> parseRfc3339(const std::string& s);

long long diffMillis(TimePoint to, TimePoint from);

} // namespace axr
