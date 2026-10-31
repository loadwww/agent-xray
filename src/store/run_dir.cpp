#include "store/run_dir.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

#include "common/log.h"

namespace axr {

std::string makeSessionId() {
    std::time_t now = std::time(nullptr);
    std::tm tmLocal{};
#ifdef _WIN32
    localtime_s(&tmLocal, &now);
#else
    localtime_r(&now, &tmLocal);
#endif
    char stamp[32] = {0};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmLocal);

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 0xffff);
    char suffix[8] = {0};
    std::snprintf(suffix, sizeof(suffix), "%04x", dist(gen));

    return std::string(stamp) + "-" + suffix;
}

std::string ensureRunDir(const std::string& root, const std::string& sessionId) {
    try {
        const std::filesystem::path dir = std::filesystem::path(root) / "runs" / sessionId;
        std::filesystem::create_directories(dir);
#ifndef _WIN32
        // P2-13：run 目录含完整会话内容，仅属主可读写（Windows 依赖用户目录 ACL）
        std::error_code ec;
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) logWarn("storage", "无法设置 run 目录权限 0700: " + ec.message());
#endif
        return dir.string();
    } catch (const std::exception& e) {
        logError("storage", "无法创建 run 目录: " + std::string(e.what()));
        return "";
    }
}

bool writeTextFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        logError("storage", "无法写入 " + path);
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

} // namespace axr
