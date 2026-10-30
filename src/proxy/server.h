#pragma once
// Mode A 反向代理（§7.1）：Source 抽象为未来 MITM 预留，上层只依赖 Source
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "config/config.h"
#include "proxy/event.h"

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

private:
    void handle(const httplib::Request& req, httplib::Response& res);
    void emit(const Event& e);

    Config cfg_;
    std::function<void(const Event&)> onEvent_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<long long> hopCounter_{0};
};

} // namespace axr
