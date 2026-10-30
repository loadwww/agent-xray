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

const char* kVersion = "0.1.0-m2";

void printUsage() {
    std::cout << "AgentXray " << kVersion << "\n\n"
              << "用法:\n"
              << "  axr serve [--config config.yaml] [--root dir] [--listen port] [--bind host]\n"
              << "  axr run [--config config.yaml] [--root dir] [--listen port] -- <command...>\n"
              << "  axr replay <run目录|events.jsonl> [--format mermaid|json|text] [--out file]\n"
              << "  axr selftest [--fixtures <dir>]\n"
              << "  axr list | show | report | export | diff | prune   （后续阶段实现）\n"
              << "  axr version\n";
}

const char* notYetStaged(const std::string& cmd) {
    if (cmd == "show" || cmd == "report" || cmd == "export") return "阶段 3（M3：分析质量）";
    if (cmd == "diff" || cmd == "prune" || cmd == "list") return "阶段 4（M4：工程化）";
    return "";
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

    const char* stage = notYetStaged(cmd);
    if (!stage[0]) {
        std::cerr << "未知命令: " << cmd << "\n\n";
        printUsage();
        return 2;
    }
    std::cout << "命令 " << cmd << " 尚未实现（计划在 " << stage << "）\n";
    return 3;
}

} // namespace axr

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    return axr::runCli(argc, argv);
}
