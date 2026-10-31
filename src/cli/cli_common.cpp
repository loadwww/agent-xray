#include "cli/cli_common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "chain/run.h"
#include "common/log.h"
#include "store/events_reader.h"

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

namespace {

std::string readAllQuiet(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 按最后修改时间倒序取最近 N 个 session 目录（避免对全量目录建链）
std::vector<std::filesystem::path> recentRunDirs(const std::filesystem::path& runsDir, std::size_t limit) {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(runsDir, ec)) {
        if (ec) break;
        if (e.is_directory()) dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        std::error_code ea, eb;
        const auto ta = std::filesystem::last_write_time(a, ea);
        const auto tb = std::filesystem::last_write_time(b, eb);
        return ta > tb;
    });
    if (dirs.size() > limit) dirs.resize(limit);
    return dirs;
}

} // namespace

bool isRunIdLike(const std::string& s) {
    // YYYYMMDD-HHMMSS-xxxxxxxx-nnnn-iiii（后两段都是 hex，序号用 %04x 生成）
    if (s.size() != 34) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 15 || i == 24 || i == 29) {
            if (c != '-') return false;
            continue;
        }
        if (i < 15) {
            if (c < '0' || c > '9') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

Result<RunTarget> resolveRunTarget(const std::string& arg, const Config& cfg) {
    RunTarget t;
    t.path = arg;
    if (arg.empty()) return Result<RunTarget>::Fail("缺少 run 目标");
    if (std::filesystem::exists(arg)) return Result<RunTarget>::Ok(t);

    const bool hasSep = arg.find('/') != std::string::npos || arg.find('\\') != std::string::npos;

    if (isRunIdLike(arg) && !hasSep) {
        const std::filesystem::path runsDir = std::filesystem::path(cfg.storage.root) / "runs";
        if (!std::filesystem::is_directory(runsDir))
            return Result<RunTarget>::Fail("找不到 run: " + arg + "（" + runsDir.string() + " 不存在）");

        // 1) 已落盘 session：meta.runs[].id 精确匹配（最便宜）
        std::vector<std::string> hits;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(runsDir, ec)) {
            if (ec) break;
            if (!e.is_directory()) continue;
            const std::string raw = readAllQuiet((e.path() / "meta.json").string());
            if (raw.empty()) continue;
            const nlohmann::json m = nlohmann::json::parse(raw, nullptr, false);
            if (m.is_discarded()) continue;
            bool matched = m.value("id", "") == arg;
            if (!matched && m.contains("runs") && m["runs"].is_array()) {
                for (const auto& x : m["runs"]) {
                    if (x.value("id", "") == arg) {
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) hits.push_back(e.path().string());
        }
        if (hits.size() == 1) {
            t.path = hits.front();
            t.runId = arg;
            return Result<RunTarget>::Ok(t);
        }
        if (hits.size() > 1) {
            std::string msg = "run id " + arg + " 命中多个 session，请改用 session 目录：";
            for (const auto& h : hits) msg += " " + h;
            return Result<RunTarget>::Fail(msg);
        }

        // 2) 未收尾 / 无 meta 的 session：只扫最近 20 个目录，实时建链匹配
        std::vector<std::filesystem::path> recent = recentRunDirs(runsDir, 20);
        for (const auto& dir : recent) {
            if (!std::filesystem::exists(dir / "events.jsonl")) continue;
            auto chain = buildChainFromEvents(dir.string(), cfg.chainOptions());
            if (!chain.ok()) continue;
            if (findRunById(chain.value, arg) != nullptr) {
                t.path = dir.string();
                t.runId = arg;
                return Result<RunTarget>::Ok(t);
            }
        }
        return Result<RunTarget>::Fail("找不到 run: " + arg + "（已扫描 " + runsDir.string() + " 下最近 " +
                                       std::to_string(recent.size()) + " 个 session）");
    }

    // 纯 sessionId → <root>/runs/<id>
    if (!hasSep) {
        const std::filesystem::path candidate = std::filesystem::path(cfg.storage.root) / "runs" / arg;
        if (std::filesystem::exists(candidate)) {
            t.path = candidate.string();
            return Result<RunTarget>::Ok(t);
        }
    }
    return Result<RunTarget>::Ok(t); // 原样返回，交给建链逻辑报错
}

} // namespace axr
