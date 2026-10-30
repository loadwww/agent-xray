#include "proxy/route.h"

namespace axr {

RouteResult resolveUpstream(const std::string& path, const std::vector<UpstreamDef>& upstreams) {
    RouteResult best;
    std::size_t bestLen = 0;

    for (const auto& up : upstreams) {
        if (up.match.empty() || up.url.empty()) continue;
        if (path.size() < up.match.size()) continue;
        if (path.compare(0, up.match.size(), up.match) != 0) continue;
        // 边界检查：/v1 不应匹配 /v1beta
        if (path.size() > up.match.size() && path[up.match.size()] != '/') continue;
        if (up.match.size() >= bestLen) { // >= 让后配置的同长度规则覆盖先前的
            best.matched = true;
            best.upstreamName = up.name;
            best.modelMap = up.modelMap;
            bestLen = up.match.size();

            std::string tail = path.substr(up.match.size());
            if (tail.empty()) tail = "/";
            std::string base = up.url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            best.url = base + tail;
        }
    }
    return best;
}

} // namespace axr
