#include "store/finalize.h"

#include <filesystem>
#include <string>

#include "chain/run.h"
#include "common/log.h"
#include "common/time_util.h"
#include "graph/mermaid.h"
#include "store/events_reader.h"
#include "store/run_dir.h"
#include "ui/report.h"
#include "ui/webui.h"

namespace axr {
namespace {

std::string joinPath(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / name).string();
}

// 单条 run 的摘要（meta.json 的 runs[] 元素）；现有顶层字段仍指主 run，保持向后兼容
nlohmann::json runBriefJson(const Run& r) {
    int tools = 0;
    for (const auto& st : r.steps) tools += static_cast<int>(st.tools.size());
    return nlohmann::json{{"id", r.id},
                          {"started_at", r.startedAt},
                          {"steps", static_cast<int>(r.steps.size())},
                          {"tools", tools},
                          {"tokens", r.total.promptTokens + r.total.completionTokens},
                          {"incomplete", r.incomplete},
                          {"requests", r.requestCount},
                          {"retries", r.retryCount},
                          {"fuzzy_edges", r.fuzzyEdges}};
}

void writeMeta(const std::string& runDir, const RunSummary& s, const Run* run,
               const std::vector<Run>& allRuns, long long durationMs) {
    nlohmann::json m;
    m["id"] = s.sessionId;
    m["started_at"] = run ? run->startedAt : "";
    m["duration_ms"] = durationMs;
    m["step_count"] = s.stepCount;
    m["tool_count"] = s.toolCount;
    if (run) m["fail_tools"] = countFailedTools(*run);
    m["request_count"] = s.requestCount;
    m["retry_count"] = s.retryCount;
    m["total_tokens"] = s.totalTokens;
    if (run) m["fuzzy_edges"] = run->fuzzyEdges;
    if (run) {
        // 多 run：同一 session 里还可能有子 agent / 第二个任务，全部登记，不再静默丢弃
        m["primary"] = run->id;
        nlohmann::json runs = nlohmann::json::array();
        for (const auto& r : allRuns) runs.push_back(runBriefJson(r));
        m["runs"] = std::move(runs);
    }
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
    m["report"] = s.reportPath;
    if (s.droppedEvents > 0) m["dropped_events"] = s.droppedEvents;
    if (s.rewrittenRequests > 0) m["rewritten"] = true;
    writeTextFile(joinPath(runDir, "meta.json"), m.dump(2));
}

} // namespace

RunSummary finalizeRun(const std::string& runDir, const Config& cfg, long long droppedEvents,
                       int rewrittenRequests) {
    RunSummary s;
    s.runDir = runDir;
    s.droppedEvents = droppedEvents;
    s.rewrittenRequests = rewrittenRequests;
    s.sessionId = std::filesystem::path(runDir).filename().string();

    const std::string eventsPath = joinPath(runDir, "events.jsonl");
    if (!std::filesystem::exists(eventsPath)) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, {}, 0);
        logWarn("run", "没有事件文件，跳过分析: " + eventsPath);
        return s;
    }

    auto chain = buildChainFromEvents(runDir, cfg.chainOptions());
    if (!chain.ok()) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, {}, 0);
        logError("run", "建链失败: " + chain.err);
        return s;
    }
    if (chain.value.runs.empty()) {
        s.status = "empty";
        writeMeta(runDir, s, nullptr, {}, 0);
        logWarn("run", "未从事件流中还原出任何 run");
        return s;
    }

    // 主 run：步数最多的那条；并列时取开始最早的
    Run* primary = &chain.value.runs.front();
    for (auto& r : chain.value.runs) {
        if (r.steps.size() >= primary->steps.size()) primary = &r;
    }

    s.runId = primary->id;
    s.stepCount = static_cast<int>(primary->steps.size());
    s.toolCount = 0;
    for (const auto& st : primary->steps) s.toolCount += static_cast<int>(st.tools.size());
    s.requestCount = primary->requestCount;
    s.retryCount = primary->retryCount;
    s.totalTokens = primary->total.promptTokens + primary->total.completionTokens;
    s.incomplete = primary->incomplete;
    s.missing = primary->missing;
    s.status = primary->incomplete ? "incomplete" : "complete";
    s.primaryModel = primary->steps.empty() ? "" : primary->steps.front().call.model;

    long long durationMs = 0;
    const auto t0 = parseRfc3339(primary->startedAt);
    const auto t1 = parseRfc3339(primary->endedAt);
    if (t0.ok() && t1.ok()) durationMs = diffMillis(t1.value, t0.value);

    // M3：报告
    MermaidOptions mo;
    mo.collapseThreshold = cfg.chain.collapseThreshold;
    s.reportPath = joinPath(runDir, "report.md");

    writeTextFile(joinPath(runDir, "run.json"), nlohmann::json(*primary).dump(2));
    writeTextFile(joinPath(runDir, "graph.mmd"), toMermaid(*primary, mo));
    writeTextFile(s.reportPath, buildReport(*primary, mo));

    // 非主 run 也落盘：run-<i>.json（i 从 0，按 runs 顺序跳过主 run），供 show/UI 按 id 取回
    int secondaryIdx = 0;
    for (const auto& r : chain.value.runs) {
        if (&r == primary) continue;
        writeTextFile(joinPath(runDir, "run-" + std::to_string(secondaryIdx++) + ".json"),
                      nlohmann::json(r).dump(2));
    }
    if (secondaryIdx > 0) {
        logInfo("run", "本 session 共 " + std::to_string(chain.value.runs.size()) +
                           " 个 run，非主 run 已写入 run-0.json 等文件");
    }

    writeMeta(runDir, s, primary, chain.value.runs, durationMs);

    logInfo("run", "分析完成: " + s.sessionId + " steps=" + std::to_string(s.stepCount) +
                        " tools=" + std::to_string(s.toolCount) + " tokens=" + std::to_string(s.totalTokens));
    if (droppedEvents > 0) logWarn("run", "事件队列丢弃 " + std::to_string(droppedEvents) + " 条");
    return s;
}

} // namespace axr
