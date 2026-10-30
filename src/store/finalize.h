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
    double costUsd = 0.0;
    bool costKnown = true;
    bool incomplete = false;
    std::vector<std::string> missing;
    std::string primaryModel;
    std::string status; // complete | incomplete | empty
    long long droppedEvents = 0;
    int rewrittenRequests = 0; // 请求体被改写过的次数（model_map / inject_usage，meta 记 rewritten）
};

RunSummary finalizeRun(const std::string& runDir, const Config& cfg, long long droppedEvents,
                       int rewrittenRequests = 0);

} // namespace axr
