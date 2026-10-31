#pragma once
// Mode A 反向代理（§7.1）：Source 抽象为未来 MITM 预留，上层只依赖 Source
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "proxy/event.h"
#include "proxy/forward.h"

namespace httplib {
class Server;
struct Request;
struct Response;
} // namespace httplib

namespace axr {

class Source {
public:
    virtual ~Source() = default;
    virtual bool start(std::function<void(const Event&)> onEvent) = 0;
    virtual void stop() = 0;
};

// 本期唯一实现：明文 HTTP 反代 + WinHTTP 转发 + 逐事件录制
class ReverseProxySource : public Source {
public:
    explicit ReverseProxySource(const Config& cfg);
    ~ReverseProxySource() override;

    bool start(std::function<void(const Event&)> onEvent) override;
    void stop() override;

    int port() const { return cfg_.proxy.listen; }
    bool running() const;
    // 因 inject_usage 改写的请求数（meta.json 里记 rewritten 用）
    int rewrittenRequests() const { return static_cast<int>(rewritten_.load()); }
    // 上游拒绝 stream_options 后已自动降级（meta.json 里记 usage_injected:false）
    bool usageInjectionDisabled() const { return usageInjectionDisabled_.load(); }

private:
    void handle(const httplib::Request& req, httplib::Response& res);
    // S2 真流式：上游读取线程 → 队列 → chunked provider（§7.1.4）
    // fallbackFr 非空时，首个 400 若提到 stream_options 就去掉注入重试一次（P2-8）
    void handleStream(const ForwardRequest& fr, const ForwardRequest* fallbackFr, const std::string& hop,
                      httplib::Response& res);
    void emit(const Event& e);

    Config cfg_;
    std::function<void(const Event&)> onEvent_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<long long> hopCounter_{0};
    std::atomic<long long> rewritten_{0};
    std::atomic<bool> usageInjectionDisabled_{false};
};

} // namespace axr
