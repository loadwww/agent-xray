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
    // 解析降级计数（events 组帧阶段统计，写 meta 的 unsupported_requests / parse_degraded）
    int unsupportedRequests = 0; // 非 /chat/completions 端点的请求：不还原，只留原始事件
    int parseFailed = 0;         // 白名单端点但请求体解析失败
    int truncatedEvents = 0;     // body 超过 max_body_bytes 被截断的事件
};

// runIdNamespace：当前 session 目录名，参与 run id 生成，保证不同 session
// 即使首请求同秒、根哈希相同也不会撞 id。留空时用 "0000" 占位。
ChainResult buildRuns(std::vector<CallPair> pairs, const ChainOptions& opts = ChainOptions{},
                      const std::string& runIdNamespace = "");

// 主 run：步数最多的那条（并列取最早）；用于 report / export / diff
const Run* primaryRun(const ChainResult& cr);

// 按 run id 精确查找（--run / Web UI 多 run 访问非主 run）；找不到返回 nullptr
const Run* findRunById(const ChainResult& cr, const std::string& id);

} // namespace axr
