#include "common/time_util.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace axr {
namespace {

time_t toUtcTm_t(const std::tm& tm) {
#ifdef _WIN32
    return _mkgmtime(const_cast<std::tm*>(&tm));
#else
    return timegm(const_cast<std::tm*>(&tm));
#endif
}

} // namespace

std::string toRfc3339(TimePoint tp) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    if (ms < 0) ms += 1000;

    time_t t = static_cast<time_t>(secs);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif

    // 本地时区偏移（分钟）
    const time_t localAsUtc = toUtcTm_t(local);
    const int offsetMin = static_cast<int>((localAsUtc - t) / 60);

    char buf[40];
    char sign = offsetMin >= 0 ? '+' : '-';
    int absMin = offsetMin >= 0 ? offsetMin : -offsetMin;
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld%c%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<long long>(ms), sign, absMin / 60, absMin % 60);
    return std::string(buf);
}

std::string nowRfc3339() { return toRfc3339(std::chrono::system_clock::now()); }

Result<TimePoint> parseRfc3339(const std::string& s) {
    // 兼容 "2026-10-30T14:30:12Z" / "...+08:00" / 可选 ".123"
    if (s.size() < 19) return Result<TimePoint>::Fail("rfc3339 too short: " + s);
    std::tm tm{};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6) {
        return Result<TimePoint>::Fail("rfc3339 parse failed: " + s);
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;

    std::size_t pos = 19;
    long long fracMs = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        std::string digits;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) digits.push_back(s[pos++]);
        if (!digits.empty()) {
            // 取前三位补成毫秒
            std::string ms3 = digits.substr(0, 3);
            while (ms3.size() < 3) ms3.push_back('0');
            fracMs = std::stoll(ms3);
        }
    }

    int offsetMin = 0;
    if (pos < s.size()) {
        if (s[pos] == 'Z' || s[pos] == 'z') {
            offsetMin = 0;
        } else if (s[pos] == '+' || s[pos] == '-') {
            const int sign = (s[pos] == '+') ? 1 : -1;
            int oh = 0, om = 0;
            if (std::sscanf(s.c_str() + pos + 1, "%2d:%2d", &oh, &om) != 2) {
                // 兼容 +0800
                if (std::sscanf(s.c_str() + pos + 1, "%2d%2d", &oh, &om) != 2)
                    return Result<TimePoint>::Fail("rfc3339 bad offset: " + s);
            }
            offsetMin = sign * (oh * 60 + om);
        }
    }

    const time_t utcSeconds = toUtcTm_t(tm) - offsetMin * 60;
    auto tp = TimePoint(std::chrono::seconds(utcSeconds)) + std::chrono::milliseconds(fracMs);
    return Result<TimePoint>::Ok(tp);
}

long long diffMillis(TimePoint to, TimePoint from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

} // namespace axr
