#pragma once
// 录制结束后的收尾：events -> chain -> run.json / graph.mmd / meta.json
#include <string>
#include <vector>

#include "config/config.h"

namespace axr {

struct RunSummary {
    std::string sessionId;
    std::string runDir;
    std::string runId;
    std::string reportPath;
    int stepCount = 0;
    int toolCount = 0;
    int requestCount = 0;
    int retryCount = 0;
    int totalTokens = 0;
    bool incomplete = false;
    std::vector<std::string> missing;
    std::string primaryModel;
    std::string status; // complete | incomplete | empty
    long long droppedEvents = 0;
    int rewrittenRequests = 0; // 请求体被改写过的次数（model_map / inject_usage，meta 记 rewritten）
    bool usageInjected = true; // 流式 usage 注入是否仍生效（被上游拒绝降级后为 false）
    // 解析降级计数（>0 时 meta 写 parse_degraded:true 并分别计数）
    int unsupportedRequests = 0; // 非 /chat/completions 端点的请求：不还原，原始流量留在 events.jsonl
    int parseFailed = 0;         // 白名单端点但请求体解析失败
    int truncatedEvents = 0;     // body 超过 max_body_bytes 被截断的事件
};

RunSummary finalizeRun(const std::string& runDir, const Config& cfg, long long droppedEvents,
                       int rewrittenRequests = 0, bool usageInjectionDisabled = false);

} // namespace axr
