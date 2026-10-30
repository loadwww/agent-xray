#pragma once
// 内置 Web UI（GET /__xray 与浏览器访问根路径）：run 列表 + 流程图 + 步骤明细
#include <string>

#include "config/config.h"

namespace axr {

struct WebResponse {
    int status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
};

WebResponse webUiPage();
WebResponse webUiRuns(const Config& cfg);
WebResponse webUiRun(const Config& cfg, const std::string& id);

// 浏览器直接访问代理根路径时（Accept 含 text/html）返回页面，避免抢占 API 客户端的 GET /
bool webUiWantsHtml(const std::string& path, const std::string& accept);

} // namespace axr
