#include "proxy/server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "common/log.h"
#include "common/time_util.h"
#include "httplib.h"
#include "proxy/forward.h"
#include "proxy/inject.h"
#include "proxy/redact.h"
#include "proxy/route.h"
#include "ui/webui.h"

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

// S2 真流式转发的线程间桥：上游读取线程 push，httplib chunked provider 阻塞取（§7.1.4）
class StreamBridge {
public:
    ~StreamBridge() {
        cancel();
        join();
    }

    void setHeaders(int status, const std::string& ctype) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = status;
        ctype_ = ctype;
        headersReady_ = true;
        cv_.notify_all();
    }

    // 等上游响应头；只等一次，超时或上游提前失败都返回 false
    bool waitHeaders(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, timeout, [this] { return headersReady_ || finished_; });
        return headersReady_;
    }

    int status() const {
        std::lock_guard<std::mutex> lk(mu_);
        return status_;
    }

    std::string contentType() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ctype_;
    }

    // 生产者：队列满则阻塞（背压），客户端断开后直接丢弃
    void push(const std::string& chunk) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return cancelled_ || q_.size() < kMaxQueued; });
        if (cancelled_) return;
        q_.push_back(chunk);
        cv_.notify_all();
    }

    bool abortRequested() const {
        std::lock_guard<std::mutex> lk(mu_);
        return cancelled_;
    }

    // 消费者：httplib 内容提供者回调（在服务线程上运行）
    bool writeNext(httplib::DataSink& sink) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return cancelled_ || finished_ || !q_.empty(); });
        if (cancelled_) return false;
        if (q_.empty()) { // 上游已结束且队列已空
            lk.unlock();
            sink.done();
            return true;
        }
        const std::string chunk = std::move(q_.front());
        q_.pop_front();
        cv_.notify_all();
        lk.unlock();
        if (!sink.write(chunk.data(), chunk.size())) {
            cancel(); // 客户端断开：通知生产者尽快收手
            return false;
        }
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lk(mu_);
        finished_ = true;
        cv_.notify_all();
    }

    void cancel() {
        std::lock_guard<std::mutex> lk(mu_);
        cancelled_ = true;
        cv_.notify_all();
    }

    void start(std::thread&& th) { worker_ = std::move(th); }

    void join() {
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }

private:
    static constexpr std::size_t kMaxQueued = 64;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    int status_ = 0;
    std::string ctype_;
    bool headersReady_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
    std::thread worker_;
};

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
    // 内置 Web UI：GET /__xray 与浏览器访问根路径；不转发、不落事件
    if (req.method == "GET") {
        WebResponse wr;
        bool matched = true;
        if (req.path == "/__xray" || req.path == "/__xray/") {
            wr = webUiPage();
        } else if (req.path == "/__xray/api/runs") {
            wr = webUiRuns(cfg_);
        } else if (req.path == "/__xray/api/run") {
            wr = webUiRun(cfg_, req.get_param_value("id"));
        } else if (webUiWantsHtml(req.path, req.get_header_value("Accept"))) {
            wr = webUiPage();
        } else {
            matched = false;
        }
        if (matched) {
            res.status = wr.status;
            res.set_header("Cache-Control", "no-store");
            res.set_content(wr.body, wr.contentType.c_str());
            return;
        }
    }

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

    // 请求体改写：模型名映射（upstreams[].model_map）+ 流式补 usage（§7.2.3）
    std::string forwardBody = req.body;
    std::string modelApplied;
    if (rewriteModel(forwardBody, route.modelMap, &modelApplied)) {
        ++rewritten_;
        logInfo("proxy", hop + " 模型名改写: " + modelApplied);
    }
    if (cfg_.proxy.injectUsage && stream && injectStreamUsage(forwardBody)) {
        ++rewritten_;
        logInfo("proxy", hop + " 已补 stream_options.include_usage");
    }

    bool reqTruncated = false;
    const std::string reqForRecord =
        truncateCopy(redactBody(forwardBody, cfg_.redact.patterns), cfg_.proxy.maxBodyBytes, reqTruncated);

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
    fr.body = forwardBody; // 转发用原始值（仅 inject_usage 场景是被改写的副本）
    for (const auto& kv : headers) fr.headers.emplace_back(kv.first, kv.second);

    if (stream) {
        handleStream(fr, hop, res); // S2：边收边发
        return;
    }

    ForwardResponse fwd = forwardHttp(fr, nullptr);

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

    logInfo("proxy", hop + " " + std::to_string(fwd.status) + " " + std::to_string(fwd.latencyMs) + "ms");

    bool respTruncated = false;
    Event respEv;
    respEv.kind = "resp";
    respEv.hop = hop;
    respEv.status = fwd.status;
    respEv.latencyMs = fwd.latencyMs;
    respEv.respBody = truncateCopy(redactBody(fwd.body, cfg_.redact.patterns), cfg_.proxy.maxBodyBytes, respTruncated);
    respEv.truncated = respTruncated;
    emit(respEv);

    // 上游 4xx/5xx：除 resp 外再记一条 err，便于直接定位失败请求（T11）
    if (fwd.status >= 400) {
        Event errEv;
        errEv.kind = "err";
        errEv.hop = hop;
        errEv.status = fwd.status;
        errEv.error = "upstream status " + std::to_string(fwd.status);
        emit(errEv);
        logWarn("proxy", hop + " 上游返回 " + std::to_string(fwd.status));
    }

    res.status = fwd.status > 0 ? fwd.status : 502;
    std::string ctype = fwd.contentType;
    if (ctype.empty()) ctype = "application/json";
    res.set_content(fwd.body, ctype.c_str());
}

// S2 真流式（§7.1.4）：上游读取在 worker 线程，客户端由 chunked provider 边取边写。
// 队列满时 worker 阻塞（背压），客户端断开则置 cancelled 并中断上游读取。
void ReverseProxySource::handleStream(const ForwardRequest& fr, const std::string& hop, httplib::Response& res) {
    auto bridge = std::make_shared<StreamBridge>();
    auto emitFn = onEvent_; // 拷贝到 worker，避免 worker 引用 this
    bridge->start(std::thread([bridge, fr, hop, emitFn] {
        ForwardResponse fwd = forwardHttp(
            fr,
            [&](const std::string& chunk) {
                Event ce;
                ce.kind = "sse_chunk";
                ce.hop = hop;
                ce.data = chunk;
                if (emitFn) emitFn(ce);
                bridge->push(chunk);
            },
            [&](int status, const std::string& ctype) { bridge->setHeaders(status, ctype); },
            [&] { return bridge->abortRequested(); });

        if (!fwd.error.empty()) {
            Event ee;
            ee.kind = "err";
            ee.hop = hop;
            ee.status = 502;
            ee.error = fwd.error;
            if (emitFn) emitFn(ee);
            logError("proxy", "转发失败 " + fr.url + ": " + fwd.error);
            bridge->finish();
            return;
        }
        if (!bridge->abortRequested()) {
            Event endEv;
            endEv.kind = "sse_end";
            endEv.hop = hop;
            if (emitFn) emitFn(endEv);

            Event respEv;
            respEv.kind = "resp";
            respEv.hop = hop;
            respEv.status = fwd.status;
            respEv.latencyMs = fwd.latencyMs;
            if (emitFn) emitFn(respEv);

            if (fwd.status >= 400) {
                Event errEv;
                errEv.kind = "err";
                errEv.hop = hop;
                errEv.status = fwd.status;
                errEv.error = "upstream status " + std::to_string(fwd.status);
                if (emitFn) emitFn(errEv);
                logWarn("proxy", hop + " 上游返回 " + std::to_string(fwd.status));
            }
        }
        logInfo("proxy", hop + " " + std::to_string(fwd.status) + " stream " + std::to_string(fwd.body.size()) + "B " +
                             std::to_string(fwd.latencyMs) + "ms");
        bridge->finish();
    }));

    if (!bridge->waitHeaders(std::chrono::seconds(120))) {
        bridge->cancel();
        bridge->join();
        res.status = 502;
        res.set_content(nlohmann::json{{"error", {{"message", "agentxray: upstream error before response headers"}}}}.dump(),
                        "application/json");
        return;
    }

    res.status = bridge->status() > 0 ? bridge->status() : 502;
    std::string ctype = bridge->contentType();
    if (ctype.empty()) ctype = "text/event-stream";
    res.set_chunked_content_provider(
        ctype, [bridge](std::size_t, httplib::DataSink& sink) { return bridge->writeNext(sink); },
        [bridge](bool) {
            bridge->cancel();
            bridge->join();
        });
}

} // namespace axr
