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
};

// 若输入是目录，自动拼 events.jsonl
std::string resolveEventsPath(const std::string& input);

Result<std::vector<RawEvent>> readEventsFile(const std::string& path);

// 把 events 组帧成 [LLMCall + LLMResponse]
Result<std::vector<CallPair>> assembleCalls(const std::vector<RawEvent>& events);

// 一步到位：events 路径（目录或文件）→ 切 run / 建链
Result<ChainResult> buildChainFromEvents(const std::string& input, const ChainOptions& opts = ChainOptions{});

} // namespace axr
