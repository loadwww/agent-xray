#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cli/cli.h"
#include "common/log.h"

namespace axr {
namespace {

const char* kVersion = "0.1.1";

void printUsage() {
    std::cout << "AgentXray " << kVersion << "\n\n"
              << "用法:\n"
              << "  axr serve [--config config.yaml] [--root dir] [--listen port] [--bind host]\n"
              << "  axr run [--config config.yaml] [--root dir] [--listen port] -- <command...>\n"
              << "  axr list [--root dir] [--config config.yaml] [--json]\n"
              << "  axr show <run目录|events.jsonl|runID> [--bench] [--config config.yaml]\n"
              << "  axr report <run目录|events.jsonl|runID> [--out file] [--config config.yaml]\n"
              << "  axr export <run目录|events.jsonl|runID> --format mermaid|json [--out file]\n"
              << "  axr diff <base> <cur> [--config config.yaml] [--root dir]\n"
              << "  axr prune --days N [--apply] [--root dir]\n"
              << "  axr replay <run目录|events.jsonl> [--format mermaid|json|text] [--out file]\n"
              << "  axr selftest [--fixtures <dir>]\n"
              << "  axr version\n";
}

} // namespace

int runCli(int argc, char** argv) {
    std::vector<std::string> raw;
    for (int i = 1; i < argc; ++i) raw.push_back(argv[i]);

    // 全局 --verbose
    std::vector<std::string> args;
    for (const auto& a : raw) {
        if (a == "--verbose" || a == "-v") setVerbose(true);
        else args.push_back(a);
    }

    if (args.empty()) {
        printUsage();
        return 0;
    }

    const std::string cmd = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "version") {
        std::cout << "AgentXray " << kVersion << "\n";
        return 0;
    }
    if (cmd == "replay") return cmdReplay(rest);
    if (cmd == "selftest") return cmdSelftest(rest);
    if (cmd == "serve") return cmdServe(rest);
    if (cmd == "run") return cmdRun(rest);
    if (cmd == "show") return cmdShow(rest);
    if (cmd == "report") return cmdReport(rest);
    if (cmd == "list") return cmdList(rest);
    if (cmd == "export") return cmdExport(rest);
    if (cmd == "diff") return cmdDiff(rest);
    if (cmd == "prune") return cmdPrune(rest);

    std::cerr << "未知命令: " << cmd << "\n\n";
    printUsage();
    return 2;
}

} // namespace axr

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    return axr::runCli(argc, argv);
}
