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
    std::vector<std::string> command; // "--" 之后的内容（axr run 用）
};

// 解析选项；"--" 之后的内容原样进入 command
Status parseProxyArgs(const std::vector<std::string>& args, ProxyCliOpts& out);

// --config 指定文件；否则尝试当前目录 config.yaml；都没有则用默认配置
Result<Config> loadEffectiveConfig(const ProxyCliOpts& opts);

} // namespace axr
