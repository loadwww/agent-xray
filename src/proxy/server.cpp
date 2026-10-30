#include "proxy/server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>

#include "common/log.h"
#include "common/time_util.h"
#include "httplib.h"
#include "proxy/forward.h"
#include "proxy/redact.h"
#include "proxy/route.h"

namespace axr {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string truncateCopy(const std::string& s, long long maxBytes, bool& truncated) {
    if (static_cast<long long>(s.size()) <= maxBytes) return s;
    truncated = true;
    return s.substr(0, static_cast<std::size_t>(maxBytes));
}

} // namespace

ReverseProxySource::ReverseProxySource(const Config& cfg) : cfg_(cfg) {
    svr_ = std::make_unique<httplib::Server>();
}

ReverseProxySource::~ReverseProxySource() { stop(); }

bool ReverseProxySource::start(std::function<void(const Event&)> onEvent) {
    if (svr_ && svr_->is_running()) return true;
    onEvent_ = std::move(onEvent);

    if (!cfg_.proxy.upstreams.empty()) {
        for (const auto& up : cfg_.proxy.upstreams)
            logInfo("proxy", "上游 " + up.name + ": " + up.match + " -> " + up.url);
    }

    svr_->set_payload_max_length(static_cast<std::size_t>(cfg_.proxy.maxBodyBytes));
    svr_->set_read_timeout(300, 0);
    svr_->set_write_timeout(300, 0);

    auto handler = [this](const httplib::Request& req, httplib::Response& res) { handle(req, res); };
    svr_->Post(R"(/.*)", handler);
    svr_->Get(R"(/.*)", handler);

    if (!svr_->bind_to_port(cfg_.proxy.bind.c_str(), static_cast<int>(cfg_.proxy.listen))) {
        logError("proxy", "无法监听 " + cfg_.proxy.bind + ":" + std::to_string(cfg_.proxy.listen));
        return false;
    }
    thread_ = std::thread([this] { svr_->listen_after_bind(); });

    for (int i = 0; i < 200; ++i) { // 最多等 1s
        if (svr_->is_running()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!svr_->is_running()) {
        logError("proxy", "代理启动失败");
        return false;
    }
    logInfo("proxy", "监听 http://" + cfg_.proxy.bind + ":" + std::to_string(cfg_.proxy.listen));
    return true;
}

void ReverseProxySource::stop() {
    if (svr_ && svr_->is_running()) svr_->stop();
    if (thread_.joinable()) thread_.join();
}

bool ReverseProxySource::running() const { return svr_ && svr_->is_running(); }

void ReverseProxySource::emit(const Event& e) {
    if (onEvent_) onEvent_(e);
}

void ReverseProxySource::handle(const httplib::Request& req, httplib::Response& res) {
    const std::string hop = "h-" + std::to_string(++hopCounter_);

    std::map<std::string, std::string> headers;
    for (const auto& kv : req.headers) {
        const std::string name = lower(kv.first);
        // httplib 写入的内部字段（local_addr / remote_port 等）不算请求头
        if (name.compare(0, 6, "local_") == 0 || name.compare(0, 7, "remote_") == 0) continue;
        headers[name] = kv.second;
    }

    auto route = resolveUpstream(req.path, cfg_.proxy.upstreams);
    if (!route.matched) {
        Event errEv;
        errEv.kind = "err";
        errEv.hop = hop;
        errEv.status = 404;
        errEv.error = "no upstream matched path " + req.path;
        emit(errEv);
        res.status = 404;
        res.set_content(nlohmann::json{{"error", {{"message", "agentxray: no upstream matched " + req.path}}}}
                            .dump(),
                        "application/json");
        logWarn("proxy", "未匹配上游: " + req.path);
        return;
    }

    bool stream = false;
    try {
        const nlohmann::json body = nlohmann::json::parse(req.body);
        stream = body.value("stream", false);
    } catch (...) {
        // 非 JSON 也照样转发，交给上游判断
    }

    bool reqTruncated = false;
    const std::string reqForRecord =
        truncateCopy(redactBody(req.body, cfg_.redact.patterns), cfg_.proxy.maxBodyBytes, reqTruncated);

    Event reqEv;
    reqEv.kind = "req";
    reqEv.hop = hop;
    reqEv.upstream = route.url;
    reqEv.reqBody = reqForRecord;
    reqEv.reqHeaders = redactHeaders(headers, cfg_.redact.headers);
    reqEv.truncated = reqTruncated;
    emit(reqEv);

    ForwardRequest fr;
    fr.method = req.method;
    fr.url = route.url;
    fr.body = req.body; // 转发必须用原始值
    for (const auto& kv : headers) fr.headers.emplace_back(kv.first, kv.second);

    ForwardResponse fwd = forwardHttp(fr, [&](const std::string& chunk) {
        if (!stream) return;
        Event chunkEv;
        chunkEv.kind = "sse_chunk";
        chunkEv.hop = hop;
        chunkEv.data = chunk;
        emit(chunkEv);
    });

    if (!fwd.error.empty()) {
        Event errEv;
        errEv.kind = "err";
        errEv.hop = hop;
        errEv.status = 502;
        errEv.error = fwd.error;
        emit(errEv);
        res.status = 502;
        res.set_content(nlohmann::json{{"error", {{"message", "agentxray: upstream error: " + fwd.error}}}}.dump(),
                        "application/json");
        logError("proxy", "转发失败 " + route.url + ": " + fwd.error);
        return;
    }

    if (stream) {
        Event endEv;
        endEv.kind = "sse_end";
        endEv.hop = hop;
        emit(endEv);
        logInfo("proxy", hop + " " + std::to_string(fwd.status) + " stream " + std::to_string(fwd.body.size()) +
                             "B " + std::to_string(fwd.latencyMs) + "ms");
    } else {
        logInfo("proxy", hop + " " + std::to_string(fwd.status) + " " + std::to_string(fwd.latencyMs) + "ms");
    }

    bool respTruncated = false;
    Event respEv;
    respEv.kind = "resp";
    respEv.hop = hop;
    respEv.status = fwd.status;
    respEv.latencyMs = fwd.latencyMs;
    if (!stream) {
        respEv.respBody =
            truncateCopy(redactBody(fwd.body, cfg_.redact.patterns), cfg_.proxy.maxBodyBytes, respTruncated);
        respEv.truncated = respTruncated;
    }
    emit(respEv);

    res.status = fwd.status > 0 ? fwd.status : 502;
    std::string ctype = fwd.contentType;
    if (ctype.empty()) ctype = stream ? "text/event-stream" : "application/json";
    res.set_content(fwd.body, ctype.c_str());
}

} // namespace axr
