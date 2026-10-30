#include "cli/cli.h"

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "cli/cli_common.h"
#include "store/run_index.h"

namespace axr {
namespace {

struct ListOpts {
    bool asJson = false;
    ProxyCliOpts cfgOpts;
};

Status parseListArgs(const std::vector<std::string>& args, ListOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") out.asJson = true;
        else if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else return Status::Fail("多余参数: " + a);
    }
    return Status::Ok();
}

std::string cell(const std::string& s) {
    if (s.empty()) return "-";
    return s;
}

nlohmann::json toJsonArray(const std::vector<RunMeta>& metas) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : metas) {
        nlohmann::json j;
        j["dir"] = m.dirName;
        j["id"] = m.id;
        j["started_at"] = m.startedAt;
        j["status"] = m.status;
        j["incomplete"] = m.incomplete;
        j["step_count"] = m.stepCount;
        j["tool_count"] = m.toolCount;
        j["request_count"] = m.requestCount;
        j["retry_count"] = m.retryCount;
        j["total_tokens"] = m.totalTokens;
        j["models"] = m.models;
        j["has_meta"] = m.hasMeta;
        j["age_days"] = m.ageDays;
        arr.push_back(std::move(j));
    }
    return arr;
}

} // namespace

int cmdList(const std::vector<std::string>& args) {
    ListOpts opts;
    Status st = parseListArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr list [--root dir] [--config config.yaml] [--json]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts.cfgOpts);
    if (!cfg.ok()) {
        std::cerr << cfg.err << "\n";
        return 1;
    }

    auto metas = listRuns(cfg.value.storage.root);
    if (!metas.ok()) {
        std::cerr << metas.err << "\n";
        return 1;
    }

    if (opts.asJson) {
        std::cout << toJsonArray(metas.value).dump(2) << "\n";
        return 0;
    }

    std::cout << "共 " << metas.value.size() << " 个 run（root: " << cfg.value.storage.root << "）\n";
    if (metas.value.empty()) return 0;
    std::cout << "| run | 时间 | 状态 | Step | 工具 | token |\n"
              << "|---|---|---|---|---|---|\n";
    for (const auto& m : metas.value) {
        const std::string status = m.hasMeta ? m.status : "无 meta";
        std::cout << "| " << cell(m.dirName) << " | " << cell(m.startedAt) << " | " << status << " | "
                  << m.stepCount << " | " << m.toolCount << " | " << m.totalTokens << " |\n";
    }
    return 0;
}

} // namespace axr
