#pragma once
// 上游路由（§7.1.1）：取最长匹配前缀；match 是本地虚拟前缀，转发时剥掉
#include <string>
#include <vector>

#include "config/config.h"

namespace axr {

struct RouteResult {
    bool matched = false;
    std::string url;          // 拼好的上游完整 URL
    std::string upstreamName; // 配置里的 name，用于事件标注
};

RouteResult resolveUpstream(const std::string& path, const std::vector<UpstreamDef>& upstreams);

} // namespace axr
