#pragma once
// 极简 config.yaml 解析（§9）：只支持本项目用到的子集，不引入 yaml-cpp
#include <map>
#include <string>
#include <vector>

#include "chain/link.h"
#include "common/result.h"

namespace axr {

// 单价：USD / 1M tokens（§7.5）
struct ModelPrice {
    double input = 1.00;
    double output = 3.00;
    double cached = -1.0; // <0 表示按 input 的 1/10 计
};

struct PricingCfg {
    std::map<std::string, ModelPrice> models;
    ModelPrice fallback{1.00, 3.00, -1.0}; // 未知模型兜底
};

// 上游路由：match 是本地虚拟前缀，转发时被剥掉（见 route.h）
struct UpstreamDef {
    std::string match; // 如 "/v1"
    std::string url;   // 如 "https://api.deepseek.com/v1"
    std::string name;  // 如 "deepseek"
    // 模型名改写：客户端只认自家的模型名（如 AutoGPT 的 gpt-4o），转发前换成上游真实的模型名
    std::map<std::string, std::string> modelMap;
};

struct Config {
    struct ProxyCfg {
        std::string bind = "127.0.0.1";
        int listen = 8080;
        long long maxBodyBytes = 8LL * 1024 * 1024;
        long long maxRunBytes = 256LL * 1024 * 1024;
        int maxQueue = 10000;
        bool injectUsage = false; // 流式请求自动补 stream_options.include_usage（§7.2.3）
        std::vector<UpstreamDef> upstreams;
    } proxy;

    struct ChainCfg {
        int runGapMinutes = 30;
        double fuzzyThreshold = 0.90;
        int fuzzyWindow = 20;
        int collapseThreshold = 12; // M3 长链折叠阈值（§7.4）
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

    PricingCfg pricing;

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
