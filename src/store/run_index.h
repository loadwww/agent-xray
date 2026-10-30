#pragma once
// run 目录索引（§8 axr list / axr prune）：读 meta.json，按目录 mtime 计算年龄
#include <string>
#include <vector>

#include "common/result.h"

namespace axr {

struct RunMeta {
    std::string dirName;   // <root>/runs 下的目录名（即 sessionId）
    std::string path;      // 目录绝对/相对路径
    std::string id;        // meta.json 的 id（缺省等于 dirName）
    std::string startedAt; // RFC3339；meta 缺失时从目录名推导，可能为空
    std::string status = "unknown"; // complete | incomplete | empty | unknown
    bool incomplete = false;
    bool hasMeta = false;
    int stepCount = 0;
    int toolCount = 0;
    int requestCount = 0;
    int retryCount = 0;
    int totalTokens = 0;
    double costUsd = 0.0;
    bool costKnown = false;
    std::vector<std::string> models;
    std::string reportPath;
    double ageDays = 0.0; // 目录 mtime 距今天数
};

// 列出 <root>/runs 下所有 run（按 startedAt 倒序）；目录不存在时返回错误
Result<std::vector<RunMeta>> listRuns(const std::string& root);

struct PruneCandidate {
    RunMeta meta;
    bool removed = false; // apply=true 且删除成功
};

struct PruneOutcome {
    std::vector<PruneCandidate> candidates; // 命中的目录（含删除结果）
    int removed = 0;
};

// 删除 mtime 年龄 >= days 的 run 目录；apply=false 时只列出（dry-run）
Result<PruneOutcome> pruneRuns(const std::string& root, int days, bool apply);

// 目录名 YYYYMMDD-HHMMSS-xxxx → "YYYY-MM-DD HH:MM:SS"；不匹配返回空串
std::string timeFromDirName(const std::string& dirName);

} // namespace axr
