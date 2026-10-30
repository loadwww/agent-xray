#pragma once
// 上游 HTTP(S) 转发（§7.1.3 的等价实现）
// 说明：本机未提供 libcurl 开发包，改用 Windows 内置 WinHTTP（系统 Schannel 支持 HTTPS，零第三方依赖）
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace axr {

struct ForwardRequest {
    std::string method;
    std::string url;
    std::string body;
    // 透传头（原始值，脱敏只作用于落盘副本）
    std::vector<std::pair<std::string, std::string>> headers;
};

struct ForwardResponse {
    int status = 0;          // HTTP 状态码；0 表示请求失败
    std::string error;       // 失败原因
    std::string contentType; // 响应 content-type
    std::string body;        // 完整响应体（流式时已按序拼接）
    long long latencyMs = 0;
};

// onChunk 在收到每段数据时回调（用于录 sse_chunk）；可为空
using ChunkSink = std::function<void(const std::string& chunk)>;
// 收到响应头时回调（真流式转发要提前拿到 status / content-type）；可为空
using HeaderSink = std::function<void(int status, const std::string& contentType)>;
// 返回 true 表示客户端已断开，应尽快中断上游读取；可为空
using AbortCheck = std::function<bool()>;

ForwardResponse forwardHttp(const ForwardRequest& req, const ChunkSink& onChunk,
                            const HeaderSink& onHeaders = nullptr, const AbortCheck& abortCheck = nullptr);

// 判断上游 URL 是否为 https
bool isHttpsUrl(const std::string& url);

} // namespace axr
