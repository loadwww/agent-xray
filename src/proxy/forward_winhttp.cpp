// forwardHttp 的 Windows 实现（WinHTTP，系统 Schannel 支持 HTTPS，零第三方依赖）
// 原 forward.cpp 内容整体迁入，语义不变（§7.1.3）
#include "proxy/forward.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#ifdef _WIN32

namespace axr {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string fromWide(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string winErr(DWORD code) {
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf,
                   sizeof(buf) - 1, nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s.empty() ? ("WinHTTP error " + std::to_string(code)) : (s + " (" + std::to_string(code) + ")");
}

struct UrlParts {
    bool secure = true;
    std::wstring host;
    int port = 443;
    std::wstring path = L"/";
};

bool splitUrl(const std::string& url, UrlParts& out) {
    const std::size_t schemePos = url.find("://");
    if (schemePos == std::string::npos) return false;
    const std::string scheme = lower(url.substr(0, schemePos));
    out.secure = (scheme == "https");
    out.port = out.secure ? 443 : 80;

    const std::size_t hostStart = schemePos + 3;
    const std::size_t slash = url.find('/', hostStart);
    std::string hostPort = url.substr(hostStart, slash == std::string::npos ? std::string::npos : slash - hostStart);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

    const std::size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        try {
            out.port = std::stoi(hostPort.substr(colon + 1));
        } catch (...) {
            return false;
        }
        hostPort = hostPort.substr(0, colon);
    }
    if (hostPort.empty()) return false;
    out.host = toWide(hostPort);
    out.path = toWide(path.empty() ? "/" : path);
    return true;
}

// 透传时丢弃的头（由 WinHTTP 自己决定）
bool skipHeader(const std::string& nameLower) {
    return nameLower == "host" || nameLower == "content-length" || nameLower == "connection" ||
           nameLower == "transfer-encoding" || nameLower == "accept-encoding" || nameLower == "expect" ||
           nameLower == "upgrade" || nameLower == "keep-alive" || nameLower == "te" ||
           nameLower == "proxy-connection" || nameLower == "x-forwarded-for" || nameLower == "x-forwarded-host";
}

} // namespace

ForwardResponse forwardHttp(const ForwardRequest& req, const ChunkSink& onChunk, const HeaderSink& onHeaders,
                            const AbortCheck& abortCheck) {
    ForwardResponse out;
    UrlParts parts;
    if (!splitUrl(req.url, parts)) {
        out.error = "无法解析上游 URL: " + req.url;
        return out;
    }

    const auto t0 = std::chrono::steady_clock::now();

    HINTERNET hSession = WinHttpOpen(L"AgentXray/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        out.error = "WinHttpOpen 失败: " + winErr(GetLastError());
        return out;
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 300000, 300000);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), static_cast<INTERNET_PORT>(parts.port), 0);
    if (!hConnect) {
        out.error = "连接上游失败: " + winErr(GetLastError());
        WinHttpCloseHandle(hSession);
        return out;
    }

    const std::wstring verb = toWide(req.method.empty() ? "POST" : req.method);
    HINTERNET hReq = WinHttpOpenRequest(hConnect, verb.c_str(), parts.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        out.error = "WinHttpOpenRequest 失败: " + winErr(GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return out;
    }

    std::wstring headersW;
    for (const auto& h : req.headers) {
        if (skipHeader(lower(h.first))) continue;
        headersW += toWide(h.first) + L": " + toWide(h.second) + L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(hReq,
                                         headersW.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersW.c_str(),
                                         headersW.empty() ? 0 : static_cast<DWORD>(-1),
                                         req.body.empty() ? nullptr : const_cast<char*>(req.body.data()),
                                         static_cast<DWORD>(req.body.size()),
                                         static_cast<DWORD>(req.body.size()), 0);
    if (!sent) {
        out.error = "发送请求失败: " + winErr(GetLastError());
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return out;
    }

    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        out.error = "接收响应失败: " + winErr(GetLastError());
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return out;
    }

    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(statusCode);

    wchar_t ctypeBuf[256] = {0};
    size = sizeof(ctypeBuf) - sizeof(wchar_t);
    if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ctypeBuf, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        out.contentType = fromWide(std::wstring(ctypeBuf, ctypeBuf + wcslen(ctypeBuf)));
    }

    // 头先给出去：真流式转发据此提前设置客户端响应状态与 content-type
    if (onHeaders) onHeaders(out.status, out.contentType);

    std::string buf(8192, '\0');
    DWORD read = 0;
    while (true) {
        const BOOL got = WinHttpReadData(hReq, buf.data(), static_cast<DWORD>(buf.size()), &read);
        if (!got) {
            out.error = "读取响应失败: " + winErr(GetLastError());
            break;
        }
        if (read == 0) break;
        const std::string chunk = buf.substr(0, read);
        out.body += chunk;
        if (onChunk) onChunk(chunk);
        if (abortCheck && abortCheck()) break; // 客户端已断开：停止读上游
    }

    out.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
}

} // namespace axr

#endif // _WIN32