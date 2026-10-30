#include "store/finalize.h"

#include <filesystem>
#include <string>

#include "chain/run.h"
#include "common/log.h"
#include "common/time_util.h"
#include "graph/mermaid.h"
#include "store/events_reader.h"
#include "store/run_dir.h"

namespace axr {
namespace {

std::string joinPath(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / name).string();
}

void writeMeta(const std::string& runDir, const RunSummary& s, const Run* run, long long durationMs) {
    nlohmann::json m;
    m["id"] = s.sessionId;
    m["started_at"] = run ? run->startedAt : "";
    m["duration_ms"] = durationMs;
    m["step_count"] = s.stepCount;
    m["tool_count"] = s.toolCount;
    m["total_tokens"] = s.totalTokens;
    m["cost_usd"] = run ? run->costUsd : 0.0;
    nlohmann::json models = nlohmann::json::array();
    if (run) {
        for (const auto& st : run->steps) {
            if (st.call.model.empty()) continue;
            bool seen = false;
            for (const auto& x : models)
                if (x.get<std::string>() == st.call.model) seen = true;
            if (!seen) models.push_back(st.call.model);
        }
    }
    m["models"] = models;
    m["status"] = s.status;
    m["incomplete"] = s.incomplete;
    if (s.droppedEvents > 0) m["dropped_events"] = s.droppedEvents;
    writeTextFile(joinPath(runDir, "meta.json"), m.dump(2));
}

} // namespace

RunSummary finalizeRun(const std::string& runDir, const Config& cfg, long long droppedEvents) {
    RunSummary s;
    s.runDir = runDir;
    s.droppedEvents = droppedEvents;
    s.sessionId = std::filesystem::path(runDir).filename().string();

    const std::string eventsPath = joinPath(runDir, "events.jsonl");
    if (!std::filesystem::exists(eventsPath)) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, 0);
        logWarn("run", "没有事件文件，跳过分析: " + eventsPath);
        return s;
    }

    auto chain = buildChainFromEvents(runDir, cfg.chainOptions());
    if (!chain.ok()) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, 0);
        logError("run", "建链失败: " + chain.err);
        return s;
    }
    if (chain.value.runs.empty()) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, 0);
        logWarn("run", "未从事件流中还原出任何 run");
        return s;
    }

    // 主 run：步数最多的那条；并列时取开始最早的
    const Run* primary = &chain.value.runs.front();
    for (const auto& r : chain.value.runs) {
        if (r.steps.size() >= primary->steps.size()) primary = &r;
    }

    s.runId = primary->id;
    s.stepCount = static_cast<int>(primary->steps.size());
    s.toolCount = 0;
    for (const auto& st : primary->steps) s.toolCount += static_cast<int>(st.tools.size());
    s.totalTokens = primary->total.promptTokens + primary->total.completionTokens;
    s.incomplete = primary->incomplete;
    s.missing = primary->missing;
    s.status = primary->incomplete ? "incomplete" : "complete";
    s.primaryModel = primary->steps.empty() ? "" : primary->steps.front().call.model;

    long long durationMs = 0;
    const auto t0 = parseRfc3339(primary->startedAt);
    const auto t1 = parseRfc3339(primary->endedAt);
    if (t0.ok() && t1.ok()) durationMs = diffMillis(t1.value, t0.value);

    writeTextFile(joinPath(runDir, "run.json"), nlohmann::json(*primary).dump(2));
    writeTextFile(joinPath(runDir, "graph.mmd"), toMermaid(*primary));
    writeMeta(runDir, s, primary, durationMs);

    logInfo("run", "分析完成: " + s.sessionId + " steps=" + std::to_string(s.stepCount) +
                        " tools=" + std::to_string(s.toolCount) + " tokens=" + std::to_string(s.totalTokens));
    if (droppedEvents > 0) logWarn("run", "事件队列丢弃 " + std::to_string(droppedEvents) + " 条");
    return s;
}

} // namespace axr
