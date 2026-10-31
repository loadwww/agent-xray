#pragma once
// serve / run 共用的命令行参数与配置加载
#include <string>
#include <vector>

#include "config/config.h"
#include "common/result.h"

namespace axr {

struct ProxyCliOpts {
    std::string configPath;
    std::string rootDir;
    std::string bind;
    int listen = -1;
    bool allowRemote = false; // P2-13：非回环 bind 必须显式开启
    std::vector<std::string> command; // "--" 之后的内容（axr run 用）
};

// 解析选项；"--" 之后的内容原样进入 command
Status parseProxyArgs(const std::vector<std::string>& args, ProxyCliOpts& out);

// P2-13：bind 是否为本机回环（127.0.0.1 / localhost / ::1）
bool isLoopbackBind(const std::string& bind);

// 非回环 bind 且未加 --allow-remote 时拒绝启动，避免误把采集代理暴露到网络
Status enforceBindPolicy(const Config& cfg, bool allowRemote);

// --config 指定文件；否则尝试当前目录 config.yaml；都没有则用默认配置
Result<Config> loadEffectiveConfig(const ProxyCliOpts& opts);

// 运行目标解析：run 目录 / events.jsonl / runID（在 <storage.root>/runs 下查找）
std::string resolveRunInput(const std::string& arg, const Config& cfg);

} // namespace axr
