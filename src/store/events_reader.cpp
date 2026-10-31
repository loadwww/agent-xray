#include "store/events_reader.h"

#include <filesystem>
#include <fstream>
#include <iterator>
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
    // 请求正文不可解析的原因（summary / 截断 / 坏 JSON）。响应解析会整体覆盖
    // CallPair.resp，这里单独留着，出队时再回填，供 WP3 判定 run 不可用
    std::string reqErr;
};

// 输入路径 → session 目录名（run id 命名空间）。目录取自身名，
// events.jsonl 取父目录名，其它文件也退回父目录名
std::string deriveSessionNamespace(const std::string& input) {
    const std::filesystem::path p(input);
    if (p.filename().string() == "events.jsonl") return p.parent_path().filename().string();
    if (std::filesystem::is_directory(p)) return p.filename().string();
    return p.parent_path().filename().string();
}

} // namespace

std::string resolveEventsPath(const std::string& input) {
    std::filesystem::path p(input);
    if (std::filesystem::is_directory(p)) {
        std::filesystem::path candidate = p / "events.jsonl";
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return input;
}

bool parseEventLine(const std::string& line, RawEvent& e) {
    if (line.empty() || line == "\r") return false;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return false;
    }
    e = RawEvent{};
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
    e.truncated = j.value("truncated", false);
    return true;
}

Result<std::vector<RawEvent>> readEventsFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<std::vector<RawEvent>>::Fail("无法打开 events 文件: " + path);

    std::vector<RawEvent> events;
    std::string line;
    int bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        RawEvent e;
        if (!parseEventLine(line, e)) {
            ++bad;
            continue;
        }
        events.push_back(std::move(e));
    }
    if (bad > 0) logWarn("events", "跳过 " + std::to_string(bad) + " 行无法解析的记录");
    return Result<std::vector<RawEvent>>::Ok(std::move(events));
}

Result<bool> EventsTail::poll(const std::string& path) {
    std::error_code ec;
    const std::size_t size = std::filesystem::file_size(path, ec);
    if (ec) return Result<bool>::Fail("无法打开 events 文件: " + path + "（" + ec.message() + "）");
    const std::filesystem::file_time_type mt = std::filesystem::last_write_time(path, ec);
    const long long mtTick = ec ? -1 : static_cast<long long>(mt.time_since_epoch().count());

    // 文件大小与 mtime 都没变：直接复用缓存
    if (size == offset && mtTick == mtimeTick && pending.empty()) return Result<bool>::Ok(false);
    // 文件被截断 / 重建（例如外部清理）：从头重读
    if (size < offset) {
        events.clear();
        offset = 0;
        pending.clear();
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<bool>::Fail("无法打开 events 文件: " + path);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    offset += buf.size();
    mtimeTick = mtTick;
    pending += buf;

    bool added = false;
    int bad = 0;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = pending.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = pending.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        RawEvent e;
        if (parseEventLine(line, e)) {
            events.push_back(std::move(e));
            added = true;
        } else if (!line.empty() && line != "\r") {
            ++bad;
        }
        start = nl + 1;
    }
    pending.erase(0, start); // 未写完的尾行留到下次
    if (bad > 0) logWarn("events", "跳过 " + std::to_string(bad) + " 行无法解析的记录");
    return Result<bool>::Ok(added);
}

bool isSupportedEndpoint(const std::string& upstream) {
    // 只还原 chat completions；路径以 /chat/completions 结尾即视为白名单，
    // 本地上游（127.0.0.1:11434/v1/...）与带网关前缀的路径都能命中
    static const std::string kSuffix = "/chat/completions";
    std::string path = upstream;
    const std::size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    return path.size() >= kSuffix.size() &&
           path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

Result<std::vector<CallPair>> assembleCalls(const std::vector<RawEvent>& events,
                                            AssembleStats* stats) {
    std::map<std::string, Pending> pendings;
    std::vector<std::string> order;

    for (const auto& e : events) {
        if (e.truncated && stats) ++stats->truncatedEvents;
        if (e.hop.empty()) continue;

        if (e.kind == "req") {
            // 非白名单端点（Responses API / embeddings / models …）：不组帧，
            // 原始事件仍留在 events.jsonl，计数后显式隔离，避免污染链与统计
            if (!isSupportedEndpoint(e.upstream)) {
                if (stats) ++stats->unsupportedRequests;
                continue;
            }
            Pending p;
            p.pair.hop = e.hop;
            p.pair.call.id = e.hop;
            p.pair.call.endpoint = e.upstream;
            p.pair.call.startedAt = e.ts;
            p.lastTs = e.ts;

            // P2-14：被截断的请求体不参与解析（截断必然破坏 JSON / 消息链），
            // 直接计入 parseFailed；pair 仍保留，链上能看到这条失败请求
            if (e.truncated) {
                if (stats) ++stats->parseFailed;
                p.pair.resp.err = "req_body 被截断，无法解析";
                p.reqErr = p.pair.resp.err;
                pendings[e.hop] = std::move(p);
                order.push_back(e.hop);
                continue;
            }

            nlohmann::json body;
            try {
                body = nlohmann::json::parse(e.reqBody);
            } catch (...) {
                if (stats) ++stats->parseFailed;
                p.pair.resp.err = "req_body 解析失败";
                p.reqErr = p.pair.resp.err;
                pendings[e.hop] = std::move(p);
                order.push_back(e.hop);
                continue;
            }
            auto parsed = parseOpenAiRequest(body, e.upstream);
            if (!parsed.ok()) {
                if (stats) ++stats->parseFailed;
                p.pair.resp.err = parsed.err;
                p.reqErr = parsed.err;
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
            if (e.truncated && !p.stream) {
                // P2-14：截断的非流式响应不解析正文，计入 parseFailed；延迟 / 状态照常保留
                if (stats) ++stats->parseFailed;
                p.pair.resp.err = "resp_body 被截断，无法解析";
                p.pair.resp.callId = e.hop;
            } else if (!p.stream) {
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
            // 延迟放在最后写：上面解析 resp_body 会整体覆盖结构体（曾因此丢掉 latency_ms）
            p.pair.resp.latencyMs = e.latencyMs;
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
        // 请求正文不可解析时保留原因（响应解析会把 resp.err 覆盖掉）
        if (!p.reqErr.empty() && p.pair.resp.err.empty()) p.pair.resp.err = p.reqErr;
        // 响应未到也算一条（err 已记录），保持链完整可诊断
        out.push_back(std::move(p.pair));
    }
    return Result<std::vector<CallPair>>::Ok(std::move(out));
}

Result<ChainResult> buildChainFromRawEvents(const std::vector<RawEvent>& events, const ChainOptions& opts,
                                            const std::string& runIdNamespace) {
    AssembleStats stats;
    auto pairs = assembleCalls(events, &stats);
    if (!pairs.ok()) return Result<ChainResult>::Fail(pairs.err);
    ChainResult cr = buildRuns(std::move(pairs.value), opts, runIdNamespace);
    cr.unsupportedRequests = stats.unsupportedRequests;
    cr.parseFailed = stats.parseFailed;
    cr.truncatedEvents = stats.truncatedEvents;
    return Result<ChainResult>::Ok(std::move(cr));
}

Result<ChainResult> buildChainFromEvents(const std::string& input, const ChainOptions& opts,
                                         const std::string& runIdNamespace) {
    const std::string path = resolveEventsPath(input);
    auto events = readEventsFile(path);
    if (!events.ok()) return Result<ChainResult>::Fail(events.err);
    const std::string ns = runIdNamespace.empty() ? deriveSessionNamespace(input) : runIdNamespace;
    return buildChainFromRawEvents(events.value, opts, ns);
}

} // namespace axr
