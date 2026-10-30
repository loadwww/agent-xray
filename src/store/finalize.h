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
    int stepCount = 0;
    int toolCount = 0;
    int totalTokens = 0;
    bool incomplete = false;
    std::vector<std::string> missing;
    std::string primaryModel;
    std::string status; // complete | incomplete | empty
    long long droppedEvents = 0;
};

RunSummary finalizeRun(const std::string& runDir, const Config& cfg, long long droppedEvents);

} // namespace axr
