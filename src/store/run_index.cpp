#include "store/run_index.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace axr {
namespace {

std::string joinPath(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / name).string();
}

double ageInDays(const std::filesystem::directory_entry& e) {
    try {
        const auto mt = e.last_write_time();
        const auto now = std::filesystem::file_time_type::clock::now();
        const double secs = std::chrono::duration<double>(now - mt).count();
        return secs / 86400.0;
    } catch (...) {
        return 0.0;
    }
}

void readMetaFile(const std::string& metaPath, RunMeta& m) {
    std::ifstream in(metaPath, std::ios::binary);
    if (!in) return;
    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return; // 坏 meta.json 当不存在处理
    }
    m.hasMeta = true;
    m.id = j.value("id", m.dirName);
    m.startedAt = j.value("started_at", "");
    m.status = j.value("status", "unknown");
    m.incomplete = j.value("incomplete", false);
    m.stepCount = j.value("step_count", 0);
    m.toolCount = j.value("tool_count", 0);
    m.failTools = j.value("fail_tools", 0);
    m.requestCount = j.value("request_count", 0);
    m.retryCount = j.value("retry_count", 0);
    m.totalTokens = j.value("total_tokens", 0);
    // 旧 meta 没有 usage_observed：有非零 token 就视为观测到
    m.usageObserved = j.value("usage_observed", m.totalTokens > 0);
    m.reportPath = j.value("report", "");
    m.primary = j.value("primary", "");
    if (j.contains("runs") && j["runs"].is_array()) {
        for (const auto& x : j["runs"]) {
            if (!x.is_object()) continue;
            RunBrief b;
            b.id = x.value("id", "");
            b.startedAt = x.value("started_at", "");
            b.steps = x.value("steps", 0);
            b.tools = x.value("tools", 0);
            b.tokens = x.value("tokens", 0);
            b.usageObserved = x.value("usage_observed", b.tokens > 0);
            b.requests = x.value("requests", 0);
            b.retries = x.value("retries", 0);
            b.fuzzyEdges = x.value("fuzzy_edges", 0);
            b.incomplete = x.value("incomplete", false);
            if (!b.id.empty()) m.runs.push_back(std::move(b));
        }
    }
    if (j.contains("models") && j["models"].is_array()) {
        for (const auto& x : j["models"])
            if (x.is_string()) m.models.push_back(x.get<std::string>());
    }
}

} // namespace

std::string timeFromDirName(const std::string& dirName) {
    // 期望 YYYYMMDD-HHMMSS-xxxx（共 20 字符，后缀是十六进制随机串）
    if (dirName.size() != 20 || dirName[8] != '-' || dirName[15] != '-') return "";
    for (std::size_t i = 0; i < 15; ++i) {
        if (i == 8) continue;
        if (dirName[i] < '0' || dirName[i] > '9') return "";
    }
    for (std::size_t i = 16; i < 20; ++i) {
        const char c = dirName[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return "";
    }
    return dirName.substr(0, 4) + "-" + dirName.substr(4, 2) + "-" + dirName.substr(6, 2) + " " +
           dirName.substr(9, 2) + ":" + dirName.substr(11, 2) + ":" + dirName.substr(13, 2);
}

Result<std::vector<RunMeta>> listRuns(const std::string& root) {
    using namespace std::filesystem;
    const path runsDir = path(root) / "runs";
    if (!exists(runsDir) || !is_directory(runsDir)) return Result<std::vector<RunMeta>>::Fail("没有 runs 目录: " + runsDir.string());

    std::vector<RunMeta> out;
    for (const auto& e : directory_iterator(runsDir)) {
        if (!e.is_directory()) continue;
        RunMeta m;
        m.dirName = e.path().filename().string();
        m.path = e.path().string();
        m.id = m.dirName;
        m.ageDays = ageInDays(e);
        readMetaFile(joinPath(m.path, "meta.json"), m);
        if (m.startedAt.empty()) m.startedAt = timeFromDirName(m.dirName);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const RunMeta& a, const RunMeta& b) {
        // meta 里是 2026-10-30T20:43:57.277+08:00，无 meta 时按目录名推导成 2026-10-30 21:18:10。
        // 两种格式直接比会把空格（0x20）排在 T 前面，需先统一成 T 再比较。
        auto key = [](const std::string& t) {
            std::string s = t;
            if (s.size() > 10 && s[10] == ' ') s[10] = 'T';
            return s;
        };
        const std::string ka = key(a.startedAt), kb = key(b.startedAt);
        if (ka != kb) return ka > kb;
        return a.dirName > b.dirName;
    });
    return Result<std::vector<RunMeta>>::Ok(std::move(out));
}

Result<PruneOutcome> pruneRuns(const std::string& root, int days, bool apply) {
    if (days < 0) return Result<PruneOutcome>::Fail("--days 不能为负数");
    auto listed = listRuns(root);
    if (!listed.ok()) return Result<PruneOutcome>::Fail(listed.err);

    PruneOutcome out;
    for (const auto& m : listed.value) {
        if (m.ageDays < static_cast<double>(days)) continue;
        PruneCandidate c;
        c.meta = m;
        out.candidates.push_back(std::move(c));
    }
    if (!apply) return Result<PruneOutcome>::Ok(std::move(out));

    int removed = 0;
    for (auto& c : out.candidates) {
        std::error_code ec;
        const std::filesystem::path p = c.meta.path;
        const auto n = std::filesystem::remove_all(p, ec);
        if (!ec && n > 0) {
            ++removed;
            c.removed = true;
        }
    }
    out.removed = removed;
    return Result<PruneOutcome>::Ok(std::move(out));
}

} // namespace axr
