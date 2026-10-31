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

// P2-15：解析单行 events.jsonl；空行 / 坏行返回 false
bool parseEventLine(const std::string& line, RawEvent& out);

// P2-15：增量读取器。按文件大小 + mtime 判断是否有新增，只读新增字节；
// 未写完的尾行留在 pending，下次拼接后再解析。
struct EventsTail {
    std::vector<RawEvent> events;
    std::size_t offset = 0;      // 已读取的字节数
    long long mtimeTick = -1;    // last_write_time().time_since_epoch().count()
    std::string pending;         // 尚未以换行结尾的尾巴

    // 返回 true 表示读到了新事件；文件打不开时 Fail
    Result<bool> poll(const std::string& path);
};

// upstream 是否指向可还原的端点（路径以 /chat/completions 结尾；
// 兼容 http://127.0.0.1:11434/v1/chat/completions 这类本地上游）
bool isSupportedEndpoint(const std::string& upstream);

// 把 events 组帧成 [LLMCall + LLMResponse]；stats 非空时回填解析降级计数
Result<std::vector<CallPair>> assembleCalls(const std::vector<RawEvent>& events,
                                            AssembleStats* stats = nullptr);

// 一步到位：events 路径（目录或文件）→ 切 run / 建链
// runIdNamespace 留空时自动取输入路径所在 session 目录名，保证不同 session 的 run id 不撞
Result<ChainResult> buildChainFromEvents(const std::string& input, const ChainOptions& opts = ChainOptions{},
                                         const std::string& runIdNamespace = "");

// P2-15：已解析的事件 → 切 run / 建链（供增量缓存复用，避免重复读盘）
Result<ChainResult> buildChainFromRawEvents(const std::vector<RawEvent>& events,
                                            const ChainOptions& opts = ChainOptions{},
                                            const std::string& runIdNamespace = "");

} // namespace axr
