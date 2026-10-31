#include "cli/cli.h"

#include <iostream>
#include <string>

#include "chain/run.h"
#include "cli/cli_common.h"
#include "diff/rundiff.h"
#include "store/events_reader.h"

namespace axr {
namespace {

struct DiffOpts {
    std::string base;
    std::string cur;
    ProxyCliOpts cfgOpts;
};

Status parseDiffArgs(const std::vector<std::string>& args, DiffOpts& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config" && i + 1 < args.size()) out.cfgOpts.configPath = args[++i];
        else if (a == "--root" && i + 1 < args.size()) out.cfgOpts.rootDir = args[++i];
        else if (!a.empty() && a[0] == '-') return Status::Fail("未知选项: " + a);
        else if (out.base.empty()) out.base = a;
        else if (out.cur.empty()) out.cur = a;
        else return Status::Fail("多余参数: " + a);
    }
    if (out.base.empty() || out.cur.empty())
        return Status::Fail("需要两个目标：axr diff <base> <cur>（run 目录 / events.jsonl / runID）");
    return Status::Ok();
}

// 按 UTF-8 字符边界截断，避免把中文/emoji 切坏
std::string clip(const std::string& s, std::size_t maxBytes = 60) {
    if (s.size() <= maxBytes) return s;
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

void printRunLine(const char* tag, const Run& run) {
    std::cout << "  " << tag << ": " << run.id << " · " << run.steps.size() << " Step · "
              << countTools(run) << " 工具 · "
              << (run.total.observed ? std::to_string(run.total.promptTokens + run.total.completionTokens) : "—")
              << " token\n";
}

void printStepDelta(const StepDelta& d) {
    if (d.change == "added") {
        std::cout << "  + 新增 S" << d.index << "（" << d.modelAfter << " · " << d.tokensAfter << " tok）\n";
    } else if (d.change == "removed") {
        std::cout << "  - 删除 S" << d.index << "（" << d.modelBefore << " · " << d.tokensBefore << " tok）\n";
    } else {
        std::cout << "  ~ 变化 S" << d.index << "：";
        if (d.fields.empty()) {
            std::cout << "工具内容变化";
        }
        for (std::size_t i = 0; i < d.fields.size(); ++i) {
            if (i) std::cout << "、";
            const std::string& f = d.fields[i];
            if (f == "tokens") std::cout << "token " << d.tokensBefore << " → " << d.tokensAfter;
            else if (f == "model") std::cout << "模型 " << d.modelBefore << " → " << d.modelAfter;
            else std::cout << f;
        }
        std::cout << "\n";
    }
}

void printToolDelta(const ToolDelta& d) {
    const std::string where = "（S" + std::to_string(d.stepIndex) + "）";
    if (d.change == "added") {
        std::cout << "  + 新增 T" << d.toolIndex << " " << d.name << where << " 参数 " << clip(d.after) << "\n";
    } else if (d.change == "removed") {
        std::cout << "  - 删除 T" << d.toolIndex << " " << d.name << where << " 参数 " << clip(d.before) << "\n";
    } else if (d.change == "name") {
        std::cout << "  ~ 名称变化 T" << d.toolIndex << where << "：" << d.before << " → " << d.after << "\n";
    } else if (d.change == "arguments") {
        std::cout << "  ~ 参数变化 T" << d.toolIndex << " " << d.name << where << "：" << clip(d.before)
                  << " → " << clip(d.after) << "\n";
    } else if (d.change == "result") {
        std::cout << "  ~ 结果变化 T" << d.toolIndex << " " << d.name << where << "：" << clip(d.before)
                  << " → " << clip(d.after) << "\n";
    }
}

} // namespace

int cmdDiff(const std::vector<std::string>& args) {
    DiffOpts opts;
    Status st = parseDiffArgs(args, opts);
    if (!st.ok()) {
        std::cerr << st.err << "\n用法: axr diff <base> <cur> [--config config.yaml] [--root dir]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts.cfgOpts);
    if (!cfg.ok()) {
        std::cerr << cfg.err << "\n";
        return 1;
    }

    const std::string basePath = resolveRunInput(opts.base, cfg.value);
    auto baseChain = buildChainFromEvents(basePath, cfg.value.chainOptions());
    if (!baseChain.ok()) {
        std::cerr << "base: " << baseChain.err << "\n";
        return 1;
    }
    const Run* base = primaryRun(baseChain.value);
    if (!base) {
        std::cerr << "base 中未还原出 run: " << basePath << "\n";
        return 1;
    }

    const std::string curPath = resolveRunInput(opts.cur, cfg.value);
    auto curChain = buildChainFromEvents(curPath, cfg.value.chainOptions());
    if (!curChain.ok()) {
        std::cerr << "cur: " << curChain.err << "\n";
        return 1;
    }
    const Run* cur = primaryRun(curChain.value);
    if (!cur) {
        std::cerr << "cur 中未还原出 run: " << curPath << "\n";
        return 1;
    }

    const RunDiff d = diffRuns(*base, *cur);

    std::cout << "Run 对比\n";
    printRunLine("base", *base);
    printRunLine("cur ", *cur);
    std::cout << "\n";

    if (d.empty()) {
        std::cout << "未发现步骤或工具差异\n";
    } else {
        if (!d.steps.empty()) {
            std::cout << "步骤差异\n";
            for (const auto& s : d.steps) printStepDelta(s);
        }
        if (!d.tools.empty()) {
            std::cout << "工具差异\n";
            for (const auto& t : d.tools) printToolDelta(t);
        }
    }

    std::cout << "\n用量差异\n";
    const int dt = d.curTokens - d.baseTokens;
    std::cout << "  token " << d.baseTokens << " → " << d.curTokens << "（" << (dt > 0 ? "+" : "") << dt
              << "）\n";

    for (const auto& n : d.notes) std::cout << "  备注: " << n << "\n";
    return 0;
}

} // namespace axr
