#pragma once
// run 切分与 Step 组装（§7.3.2 / §7.3.3）
#include <string>
#include <vector>

#include "chain/link.h"
#include "parse/model.h"

namespace axr {

// 一条请求 + 它的响应（按时间排序的输入单元）
struct CallPair {
    LLMCall call;
    LLMResponse resp;
    std::string hop; // 调试用
};

struct ChainResult {
    std::vector<Run> runs;
    std::vector<Link> links;
    int retryCount = 0;
    int fuzzyCount = 0;
};

ChainResult buildRuns(std::vector<CallPair> pairs, const ChainOptions& opts = ChainOptions{});

// 主 run：步数最多的那条（并列取最早）；用于 report / export / diff
const Run* primaryRun(const ChainResult& cr);

// 按 run id 精确查找（--run / Web UI 多 run 访问非主 run）；找不到返回 nullptr
const Run* findRunById(const ChainResult& cr, const std::string& id);

} // namespace axr
