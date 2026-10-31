// forwardHttp 的 Linux/macOS 实现（libcurl easy handle，P1-4）
// 要点：
//   - CURLOPT_NOSIGNAL=1：代理运行在心跳线程里，不给进程装信号处理器
//   - 头/体回调只写 sink（status/content-type 提取 + onChunk 透传），不做业务解析
//   - 客户端断开：abortCheck 挂在 XFERINFOFUNCTION 上，返回 1 中止传输
//   - 流式首块尽快下发：WRITEFUNCTION 按到达顺序透传，BUFFERSIZE 调小
//   - 不设 CURLOPT_ACCEPT_ENCODING：不自动解压，与 WinHTTP 的透传语义一致
#include "proxy/forward.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#if !defined(_WIN32)
#include <curl/curl.h>

namespace axr {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 透传时丢弃的头（由 libcurl 自己决定）
bool skipHeader(const std::string& nameLower) {
    return nameLower == "host" || nameLower == "content-length" || nameLower == "connection" ||
           nameLower == "transfer-encoding" || nameLower == "accept-encoding" || nameLower == "expect" ||
           nameLower == "upgrade" || nameLower == "keep-alive" || nameLower == "te" ||
           nameLower == "proxy-connection" || nameLower == "x-forwarded-for" || nameLower == "x-forwarded-host";
}

struct CurlCtx {
    ForwardResponse* out = nullptr;
    ChunkSink onChunk;
    HeaderSink onHeaders;
    AbortCheck abortCheck;
    bool headersEmitted = false;
};

// 响应头回调：只提取状态行与 content-type，其余丢弃
std::size_t headerCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t total = size * nmemb;
    auto* ctx = static_cast<CurlCtx*>(userdata);
    std::string line(data, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

    if (!line.empty()) {
        if (line.compare(0, 5, "HTTP/") == 0) {
            int code = 0;
            // HTTP/1.1 200 ... 或 libcurl 合成的 HTTP/2 200
            if (std::sscanf(line.c_str(), "HTTP/%*s %d", &code) == 1) ctx->out->status = code;
        } else {
            const std::string lowerLine = lower(line);
            const std::string marker = "content-type:";
            if (lowerLine.compare(0, marker.size(), marker) == 0) {
                const std::size_t p = line.find(':');
                if (p != std::string::npos) {
                    std::string v = line.substr(p + 1);
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                    ctx->out->contentType = v;
                }
            }
        }
        return total;
    }

    // 空行 = 头块结束：此刻把状态/content-type 交出去，流式转发据此先回客户端
    if (!ctx->headersEmitted && ctx->onHeaders) {
        ctx->headersEmitted = true;
        ctx->onHeaders(ctx->out->status, ctx->out->contentType);
    }
    return total;
}

// 响应体回调：追加进 sink 并逐段交给 onChunk（不做解析）
std::size_t writeCallback(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t n = size * nmemb;
    if (n == 0) return 0;
    auto* ctx = static_cast<CurlCtx*>(userdata);
    const std::string chunk(data, n);
    ctx->out->body += chunk;
    if (ctx->onChunk) ctx->onChunk(chunk);
    return n;
}

// 进度回调：abortCheck 为真（客户端已断开）时返回非零，libcurl 中止本次传输
int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<CurlCtx*>(userdata);
    if (ctx->abortCheck && ctx->abortCheck()) return 1;
    return 0;
}

} // namespace

ForwardResponse forwardHttp(const ForwardRequest& req, const ChunkSink& onChunk, const HeaderSink& onHeaders,
                            const AbortCheck& abortCheck) {
    ForwardResponse out;
    CurlCtx ctx{&out, onChunk, onHeaders, abortCheck, false};

    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init 失败";
        return out;
    }

    char errBuf[CURL_ERROR_SIZE] = {0};
    struct curl_slist* headerList = nullptr;
    for (const auto& h : req.headers) {
        if (skipHeader(lower(h.first))) continue;
        headerList = curl_slist_append(headerList, (h.first + ": " + h.second).c_str());
    }

    const auto t0 = std::chrono::steady_clock::now();
    CURLcode rc = CURLE_OK;

    // NOSIGNAL：不注册信号处理器（真实进程内转发，多线程安全）
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.empty() ? "POST" : req.method.c_str());
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    // 与 WinHTTP 对齐：连 10s、收 300s（低于 1B/s 持续 300s 判死）
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 300L);

    // 透传语义：不自动解压、不自动跟随重定向
    // （不设置 CURLOPT_ACCEPT_ENCODING；FOLLOWLOCATION 默认 0）

    // 流式首块尽快下发：小读缓冲 + 按到达顺序透传
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errBuf);

    rc = curl_easy_perform(curl);
    // 客户端断开导致的 CURLE_ABORTED_BY_CALLBACK 是预期路径，不当作上游错误
    if (rc == CURLE_ABORTED_BY_CALLBACK) rc = CURLE_OK;

    if (rc == CURLE_OK && (out.status == 0)) {
        long code = 0;
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) out.status = static_cast<int>(code);
    }
    if (rc != CURLE_OK) {
        std::string reason = errBuf[0] ? errBuf : curl_easy_strerror(rc);
        out.error = "curl 失败（" + std::string(curl_easy_strerror(rc)) + "）: " + reason;
    }

    out.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return out;
}

} // namespace axr

#endif // !_WIN32