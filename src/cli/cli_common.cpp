#include "cli/cli_common.h"

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

} // namespace axr
