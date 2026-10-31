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
    bool all = false; // --all：run 级平铺（同一 session 的多个 run 各占一行）
    ProxyCliOpts cfgOpts;
};

Status parseListArgs(const std::vector<std::string>& args, ListOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") out.asJson = true;
        else if (a == "--all") out.all = true;
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
        j["usage_observed"] = m.usageObserved;
        j["models"] = m.models;
        j["has_meta"] = m.hasMeta;
        j["age_days"] = m.ageDays;
        arr.push_back(std::move(j));
    }
    return arr;
}

// --all 的 JSON：run 级平铺，每条带目录名（dir）与 runId（run）
nlohmann::json toJsonArrayAll(const std::vector<RunMeta>& metas) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : metas) {
        if (m.runs.empty()) {
            // 旧 meta（无 runs 字段）或未收尾目录：退化成目录级一条
            nlohmann::json j;
            j["dir"] = m.dirName;
            j["run"] = m.primary;
            j["started_at"] = m.startedAt;
            j["steps"] = m.stepCount;
            j["tools"] = m.toolCount;
            j["tokens"] = m.totalTokens;
            j["usage_observed"] = m.usageObserved;
            j["incomplete"] = m.incomplete;
            j["requests"] = m.requestCount;
            j["retries"] = m.retryCount;
            j["primary"] = true;
            arr.push_back(std::move(j));
            continue;
        }
        for (const auto& b : m.runs) {
            nlohmann::json j;
            j["dir"] = m.dirName;
            j["run"] = b.id;
            j["started_at"] = b.startedAt;
            j["steps"] = b.steps;
            j["tools"] = b.tools;
            j["tokens"] = b.tokens;
            j["usage_observed"] = b.usageObserved;
            j["incomplete"] = b.incomplete;
            j["requests"] = b.requests;
            j["retries"] = b.retries;
            j["fuzzy_edges"] = b.fuzzyEdges;
            j["primary"] = (b.id == m.primary);
            arr.push_back(std::move(j));
        }
    }
    return arr;
}

std::string tokensText(int tokens, bool observed) {
    return observed ? std::to_string(tokens) : "—"; // P2-9：未观测到 usage 不显示 0
}

} // namespace

int cmdList(const std::vector<std::string>& args) {
    ListOpts opts;
    Status st = parseListArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr list [--root dir] [--config config.yaml] [--json] [--all]\n";
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
        std::cout << (opts.all ? toJsonArrayAll(metas.value) : toJsonArray(metas.value)).dump(2) << "\n";
        return 0;
    }

    if (opts.all) {
        std::cout << "共 " << metas.value.size() << " 个目录（root: " << cfg.value.storage.root << "）\n";
        std::cout << "| run | 目录 | 时间 | 状态 | Step | 工具 | token |\n"
                  << "|---|---|---|---|---|---|---|\n";
        for (const auto& m : metas.value) {
            if (m.runs.empty()) {
                const std::string status = m.hasMeta ? m.status : "无 meta";
                std::cout << "| " << cell(m.primary) << " | " << cell(m.dirName) << " | " << cell(m.startedAt)
                          << " | " << status << " | " << m.stepCount << " | " << m.toolCount << " | "
                          << tokensText(m.totalTokens, m.usageObserved) << " |\n";
                continue;
            }
            for (const auto& b : m.runs) {
                const std::string mark = (b.id == m.primary) ? "*" : "";
                std::cout << "| " << b.id << mark << " | " << m.dirName << " | " << cell(b.startedAt) << " | "
                          << (b.incomplete ? "incomplete" : "complete") << " | " << b.steps << " | " << b.tools
                          << " | " << tokensText(b.tokens, b.usageObserved) << " |\n";
            }
        }
        return 0;
    }

    std::cout << "共 " << metas.value.size() << " 个 run（root: " << cfg.value.storage.root << "）\n";
    if (metas.value.empty()) return 0;
    std::cout << "| run | 时间 | 状态 | Step | 工具 | token |\n"
              << "|---|---|---|---|---|---|\n";
    for (const auto& m : metas.value) {
        const std::string status = m.hasMeta ? m.status : "无 meta";
        std::cout << "| " << cell(m.dirName) << " | " << cell(m.startedAt) << " | " << status << " | "
                  << m.stepCount << " | " << m.toolCount << " | " << tokensText(m.totalTokens, m.usageObserved)
                  << " |\n";
    }
    return 0;
}

} // namespace axr
