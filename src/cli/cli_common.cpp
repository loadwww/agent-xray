#include "cli/cli_common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "common/log.h"

namespace axr {

Status parseProxyArgs(const std::vector<std::string>& args, ProxyCliOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {
            out.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            return Status::Ok();
        }
        if (a == "--config" && i + 1 < args.size()) {
            out.configPath = args[++i];
        } else if (a == "--root" && i + 1 < args.size()) {
            out.rootDir = args[++i];
        } else if (a == "--listen" && i + 1 < args.size()) {
            try {
                out.listen = std::stoi(args[++i]);
            } catch (...) {
                return Status::Fail("--listen 需要整数端口");
            }
        } else if (a == "--bind" && i + 1 < args.size()) {
            out.bind = args[++i];
        } else if (a == "--allow-remote") {
            out.allowRemote = true;
        } else if (!a.empty() && a[0] == '-') {
            return Status::Fail("未知选项: " + a);
        } else {
            return Status::Fail("多余参数: " + a + "（命令请写在 -- 之后）");
        }
    }
    return Status::Ok();
}

Result<Config> loadEffectiveConfig(const ProxyCliOpts& opts) {
    Config cfg = defaultConfig();
    if (!opts.configPath.empty()) {
        auto loaded = loadConfig(opts.configPath, cfg);
        if (!loaded.ok()) return Result<Config>::Fail(loaded.err);
        cfg = loaded.value;
    } else if (std::filesystem::exists("config.yaml")) {
        auto loaded = loadConfig("config.yaml", cfg);
        if (!loaded.ok()) return Result<Config>::Fail(loaded.err);
        cfg = loaded.value;
    }

    if (!opts.rootDir.empty()) cfg.storage.root = expandHome(opts.rootDir);
    if (opts.listen > 0) cfg.proxy.listen = opts.listen;
    if (!opts.bind.empty()) cfg.proxy.bind = opts.bind;
    return Result<Config>::Ok(cfg);
}

bool isLoopbackBind(const std::string& bind) {
    std::string b = bind;
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return b == "127.0.0.1" || b == "localhost" || b == "::1";
}

Status enforceBindPolicy(const Config& cfg, bool allowRemote) {
    if (allowRemote || isLoopbackBind(cfg.proxy.bind)) return Status::Ok();
    return Status::Fail("bind " + cfg.proxy.bind +
                        " 不是回环地址；确认要暴露到网络时请显式加 --allow-remote");
}

std::string resolveRunInput(const std::string& arg, const Config& cfg) {
    if (arg.empty()) return arg;
    if (std::filesystem::exists(arg)) return arg;

    // 纯 runID（不含路径分隔符）→ <root>/runs/<id>
    if (arg.find('/') == std::string::npos && arg.find('\\') == std::string::npos) {
        const std::filesystem::path candidate =
            std::filesystem::path(cfg.storage.root) / "runs" / arg;
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return arg;
}

} // namespace axr
