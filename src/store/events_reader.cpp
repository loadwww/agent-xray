#include "store/events_reader.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "common/log.h"
#include "parse/openai.h"
#include "parse/sse.h"

namespace axr {
namespace {

struct Pending {
    CallPair pair;
    SseAccumulator acc;
    bool stream = false;
    std::string lastTs;
    bool hasResp = false;
};

} // namespace

std::string resolveEventsPath(const std::string& input) {
    std::filesystem::path p(input);
    if (std::filesystem::is_directory(p)) {
        std::filesystem::path candidate = p / "events.jsonl";
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return input;
}

Result<std::vector<RawEvent>> readEventsFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<std::vector<RawEvent>>::Fail("无法打开 events 文件: " + path);

    std::vector<RawEvent> events;
    std::string line;
    int bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (...) {
            ++bad;
            continue;
        }
        RawEvent e;
        e.seq = j.value("seq", 0);
        e.ts = j.value("ts", "");
        e.kind = j.value("kind", "");
        e.hop = j.value("hop", "");
        e.upstream = j.value("upstream", "");
        e.reqBody = j.value("req_body", "");
        e.respBody = j.value("resp_body", "");
        e.data = j.value("data", "");
        e.status = j.value("status", 0);
        e.latencyMs = j.value("latency_ms", 0);
        e.err = j.value("error", "");
        events.push_back(std::move(e));
    }
    if (bad > 0) logWarn("events", "跳过 " + std::to_string(bad) + " 行无法解析的记录");
    return Result<std::vector<RawEvent>>::Ok(std::move(events));
}

Result<std::vector<CallPair>> assembleCalls(const std::vector<RawEvent>& events) {
    std::map<std::string, Pending> pendings;
    std::vector<std::string> order;

    for (const auto& e : events) {
        if (e.hop.empty()) continue;

        if (e.kind == "req") {
            Pending p;
            p.pair.hop = e.hop;
            p.pair.call.id = e.hop;
            p.pair.call.endpoint = e.upstream;
            p.pair.call.startedAt = e.ts;
            p.lastTs = e.ts;

            nlohmann::json body;
            try {
                body = nlohmann::json::parse(e.reqBody);
            } catch (...) {
                p.pair.resp.err = "req_body 解析失败";
                pendings[e.hop] = std::move(p);
                order.push_back(e.hop);
                continue;
            }
            auto parsed = parseOpenAiRequest(body, e.upstream);
            if (!parsed.ok()) {
                p.pair.resp.err = parsed.err;
            } else {
                p.pair.call.messages = parsed.value.messages;
                p.pair.call.tools = parsed.value.tools;
                p.pair.call.params = parsed.value.params;
                p.pair.call.model = parsed.value.model;
                p.pair.call.provider = parsed.value.provider;
            }
            p.stream = isStreamRequest(body);
            p.pair.resp.streamed = p.stream;
            pendings[e.hop] = std::move(p);
            order.push_back(e.hop);
            continue;
        }

        auto it = pendings.find(e.hop);
        if (it == pendings.end()) continue;
        Pending& p = it->second;
        p.lastTs = e.ts;

        if (e.kind == "sse_chunk") {
            p.acc.feed(e.data);
        } else if (e.kind == "sse_end") {
            p.acc.finish();
            p.pair.resp = p.acc.take(e.hop);
            p.pair.resp.streamed = true;
            p.hasResp = true;
        } else if (e.kind == "resp") {
            p.pair.resp.latencyMs = e.latencyMs;
            if (!p.stream) {
                if (!e.respBody.empty()) {
                    nlohmann::json body;
                    try {
                        body = nlohmann::json::parse(e.respBody);
                        auto parsed = parseOpenAiResponse(body);
                        if (parsed.ok()) p.pair.resp = parsed.value;
                        else p.pair.resp.err = parsed.err;
                    } catch (...) {
                        p.pair.resp.err = "resp_body 解析失败";
                    }
                    p.pair.resp.callId = e.hop;
                }
            } else {
                // 流式：保留 sse_end 的累积结果，只补延迟
                if (!p.hasResp) {
                    p.acc.finish();
                    p.pair.resp = p.acc.take(e.hop);
                    p.pair.resp.streamed = true;
                }
            }
            if (e.status >= 400 && p.pair.resp.err.empty())
                p.pair.resp.err = "upstream status " + std::to_string(e.status);
            p.hasResp = true;
        } else if (e.kind == "err") {
            p.pair.resp.err = e.err.empty() ? "upstream error" : e.err;
            p.hasResp = true;
        }
    }

    std::vector<CallPair> out;
    for (const auto& hop : order) {
        auto it = pendings.find(hop);
        if (it == pendings.end()) continue;
        Pending& p = it->second;
        p.pair.call.endedAt = p.lastTs;
        // 响应未到也算一条（err 已记录），保持链完整可诊断
        out.push_back(std::move(p.pair));
    }
    return Result<std::vector<CallPair>>::Ok(std::move(out));
}

Result<ChainResult> buildChainFromEvents(const std::string& input, const ChainOptions& opts) {
    const std::string path = resolveEventsPath(input);
    auto events = readEventsFile(path);
    if (!events.ok()) return Result<ChainResult>::Fail(events.err);
    auto pairs = assembleCalls(events.value);
    if (!pairs.ok()) return Result<ChainResult>::Fail(pairs.err);
    return Result<ChainResult>::Ok(buildRuns(std::move(pairs.value), opts));
}

} // namespace axr
