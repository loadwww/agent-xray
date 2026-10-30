#pragma once
// 极简 config.yaml 解析（§9）：只支持本项目用到的子集，不引入 yaml-cpp
#include <string>
#include <vector>

#include "chain/link.h"
#include "common/result.h"

namespace axr {

// 上游路由：match 是本地虚拟前缀，转发时被剥掉（见 route.h）
struct UpstreamDef {
    std::string match; // 如 "/v1"
    std::string url;   // 如 "https://api.deepseek.com/v1"
    std::string name;  // 如 "deepseek"
};

struct Config {
    struct ProxyCfg {
        std::string bind = "127.0.0.1";
        int listen = 8080;
        long long maxBodyBytes = 8LL * 1024 * 1024;
        long long maxRunBytes = 256LL * 1024 * 1024;
        int maxQueue = 10000;
        std::vector<UpstreamDef> upstreams;
    } proxy;

    struct ChainCfg {
        int runGapMinutes = 30;
        double fuzzyThreshold = 0.90;
        int fuzzyWindow = 20;
    } chain;

    struct RedactCfg {
        std::vector<std::string> headers{"authorization", "x-api-key", "api-key"};
        std::vector<std::string> patterns{"sk-[A-Za-z0-9]{20,}"};
        bool pii = false;
    } redact;

    struct StorageCfg {
        std::string root; // 空表示默认 %USERPROFILE%\.xray
        int retentionDays = 30;
    } storage;

    ChainOptions chainOptions() const;
};

// %USERPROFILE%\.xray（取不到时退回当前目录下的 .xray）
std::string defaultStorageRoot();
// 展开开头的 "~"
std::string expandHome(const std::string& path);
// 默认配置：仓库内 samples/ 未加载时的兜底（deepseek + ollama 两条上游）
Config defaultConfig();

// 解析 YAML 子集文本（selftest 也走这里，便于断言）
Result<Config> parseConfigText(const std::string& text, const Config& base = defaultConfig());
Result<Config> loadConfig(const std::string& path, const Config& base = defaultConfig());

} // namespace axr
