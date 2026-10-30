#pragma once
// 内置 Web UI（GET /__xray 与浏览器访问根路径）：run 列表 + 流程图 + 步骤明细
#include <string>

#include "config/config.h"
#include "parse/model.h"

namespace axr {

struct WebResponse {
    int status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
};

// 工具结果是否像失败：协议层没有 is_error 时（结果被框架以文本回填），靠文本兜底
// （403 / Error: / Failed to / 超时 / 限流 …）。Web UI 高亮与 meta.json 的失败计数共用。
bool toolResultLooksFailed(const std::string& result);
int countFailedTools(const Run& run);

WebResponse webUiPage();
WebResponse webUiRuns(const Config& cfg);
WebResponse webUiRun(const Config& cfg, const std::string& id);

// 浏览器直接访问代理根路径时（Accept 含 text/html）返回页面，避免抢占 API 客户端的 GET /
bool webUiWantsHtml(const std::string& path, const std::string& accept);

} // namespace axr
