#include "cli/cli.h"

#include <fstream>
#include <iostream>

#include "common/log.h"
#include "graph/mermaid.h"
#include "graph/timeline.h"
#include "store/events_reader.h"

namespace axr {
namespace {

struct ReplayArgs {
    std::string path;
    std::string format = "mermaid"; // mermaid | json | text
    std::string out;
};

std::string parseValue(const std::vector<std::string>& args, const std::string& key) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) return args[i + 1];
    }
    return "";
}

void printRunText(const Run& run) {
    std::cout << "Run: " << run.id << "\n";
    std::cout << "  状态: " << (run.incomplete ? "incomplete" : "complete")
              << "  steps=" << run.steps.size() << "  tokens=" << (run.total.promptTokens + run.total.completionTokens)
              << "  " << run.startedAt << " → " << run.endedAt << "\n";
    for (const auto& m : run.missing) std::cout << "  ! " << m << "\n";
    std::cout << toMarkdownTimeline(run) << "\n";
}

} // namespace

int cmdReplay(const std::vector<std::string>& args) {
    ReplayArgs opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--format" && i + 1 < args.size()) {
            opts.format = args[++i];
        } else if (a == "--out" && i + 1 < args.size()) {
            opts.out = args[++i];
        } else if (opts.path.empty()) {
            opts.path = a;
        }
    }
    if (opts.path.empty()) {
        std::cerr << "用法: axr replay <fixture目录|events.jsonl> [--format mermaid|json|text] [--out file]\n";
        return 2;
    }

    auto chain = buildChainFromEvents(opts.path);
    if (!chain.ok()) {
        logError("replay", chain.err);
        return 1;
    }
    const ChainResult& cr = chain.value;
    logInfo("replay", "runs=" + std::to_string(cr.runs.size()) + " retry=" + std::to_string(cr.retryCount) +
                          " fuzzy=" + std::to_string(cr.fuzzyCount));

    std::string output;
    if (opts.format == "json") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : cr.runs) arr.push_back(r);
        output = arr.dump(2);
    } else if (opts.format == "text") {
        for (const auto& r : cr.runs) printRunText(r);
        return 0;
    } else {
        for (const auto& r : cr.runs) {
            output += "%% run " + r.id + (r.incomplete ? " (incomplete)" : "") + "\n";
            output += toMermaid(r);
            output += "\n";
        }
    }

    if (!opts.out.empty()) {
        std::ofstream out(opts.out, std::ios::binary);
        if (!out) {
            logError("replay", "无法写入 " + opts.out);
            return 1;
        }
        out << output;
        logInfo("replay", "已写入 " + opts.out);
    } else {
        std::cout << output;
    }
    return 0;
}

} // namespace axr
