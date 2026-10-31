#pragma once
// store 层：events.jsonl 读取与组帧（replay 的数据入口）
#include <string>
#include <vector>

#include "chain/run.h"
#include "common/result.h"

namespace axr {

struct RawEvent {
    long long seq = 0;
    std::string ts;
    std::string kind;   // req | sse_chunk | sse_end | resp | err
    std::string hop;    // 一次请求-响应的标识
    std::string upstream;
    std::string reqBody;
    std::string respBody; // 非流式响应的 body（流式由 sse_chunk 承载）
    std::string data;     // sse_chunk 的原始片段
    int status = 0;
    int latencyMs = 0;
    std::string err;
    bool truncated = false; // 事件体超过 max_body_bytes 被截断
};

// 组帧阶段的解析降级统计（写进 ChainResult / meta.json）
struct AssembleStats {
    int unsupportedRequests = 0; // 非 /chat/completions 端点：跳过组帧，只留原始事件
    int parseFailed = 0;         // 白名单端点但请求体解析失败
    int truncatedEvents = 0;     // 带 truncated 标记的事件数
};

// 若输入是目录，自动拼 events.jsonl
std::string resolveEventsPath(const std::string& input);

Result<std::vector<RawEvent>> readEventsFile(const std::string& path);

// upstream 是否指向可还原的端点（路径以 /chat/completions 结尾；
// 兼容 http://127.0.0.1:11434/v1/chat/completions 这类本地上游）
bool isSupportedEndpoint(const std::string& upstream);

// 把 events 组帧成 [LLMCall + LLMResponse]；stats 非空时回填解析降级计数
Result<std::vector<CallPair>> assembleCalls(const std::vector<RawEvent>& events,
                                            AssembleStats* stats = nullptr);

// 一步到位：events 路径（目录或文件）→ 切 run / 建链
Result<ChainResult> buildChainFromEvents(const std::string& input, const ChainOptions& opts = ChainOptions{});

} // namespace axr
