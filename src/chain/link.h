#pragma once
// 算法二：父子关系建立（§7.3.2）
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

struct ChainOptions {
    int runGapMinutes = 30;
    double fuzzyThreshold = 0.90;
    int fuzzyWindow = 20;
};

struct Link {
    int from = -1;     // 上一步请求下标
    int to = -1;       // 当前请求下标
    int newMsgs = 0;   // 新增消息条数
    bool fuzzy = false; // 是否来自模糊匹配
    bool retry = false; // 是否重试（完整哈希重复）
};

struct LinkResult {
    // parent[i] = 父请求下标，-1 表示无（即 run 根）
    std::vector<int> parent;
    std::vector<Link> links;
};

LinkResult buildLinks(const std::vector<LLMCall>& calls, const ChainOptions& opts);

} // namespace axr
