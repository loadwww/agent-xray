#include "diff/rundiff.h"

#include <algorithm>

namespace axr {
namespace {

// 每个 Step 内第一个工具的全局序号
std::vector<int> toolStarts(const Run& run) {
    std::vector<int> starts(run.steps.size(), 0);
    int n = 0;
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        starts[i] = n;
        n += static_cast<int>(run.steps[i].tools.size());
    }
    return starts;
}

int tokenCount(const Run& run) {
    return run.total.promptTokens + run.total.completionTokens;
}

} // namespace

std::size_t countTools(const Run& run) {
    std::size_t n = 0;
    for (const auto& s : run.steps) n += s.tools.size();
    return n;
}

RunDiff diffRuns(const Run& base, const Run& cur) {
    RunDiff d;
    d.baseSteps = static_cast<int>(base.steps.size());
    d.curSteps = static_cast<int>(cur.steps.size());
    d.baseTools = static_cast<int>(countTools(base));
    d.curTools = static_cast<int>(countTools(cur));
    d.baseTokens = tokenCount(base);
    d.curTokens = tokenCount(cur);

    const std::vector<int> baseStart = toolStarts(base);
    const std::vector<int> curStart = toolStarts(cur);
    const int n = std::max(d.baseSteps, d.curSteps);

    for (int i = 0; i < n; ++i) {
        const bool hasBase = i < d.baseSteps;
        const bool hasCur = i < d.curSteps;

        if (!hasBase) {
            StepDelta sd;
            sd.index = i;
            sd.change = "added";
            sd.modelAfter = cur.steps[i].call.model;
            sd.tokensAfter = cur.steps[i].tokens;
            d.steps.push_back(sd);
            const Step& s = cur.steps[i];
            for (std::size_t t = 0; t < s.tools.size(); ++t) {
                ToolDelta td;
                td.toolIndex = curStart[i] + static_cast<int>(t);
                td.stepIndex = i;
                td.change = "added";
                td.name = s.tools[t].name;
                td.after = s.tools[t].arguments;
                d.tools.push_back(td);
            }
            continue;
        }
        if (!hasCur) {
            StepDelta sd;
            sd.index = i;
            sd.change = "removed";
            sd.modelBefore = base.steps[i].call.model;
            sd.tokensBefore = base.steps[i].tokens;
            d.steps.push_back(sd);
            const Step& s = base.steps[i];
            for (std::size_t t = 0; t < s.tools.size(); ++t) {
                ToolDelta td;
                td.toolIndex = baseStart[i] + static_cast<int>(t);
                td.stepIndex = i;
                td.change = "removed";
                td.name = s.tools[t].name;
                td.before = s.tools[t].arguments;
                d.tools.push_back(td);
            }
            continue;
        }

        const Step& bs = base.steps[i];
        const Step& cs = cur.steps[i];
        StepDelta sd;
        sd.index = i;
        sd.modelBefore = bs.call.model;
        sd.modelAfter = cs.call.model;
        sd.tokensBefore = bs.tokens;
        sd.tokensAfter = cs.tokens;
        if (bs.call.model != cs.call.model) sd.fields.push_back("model");
        if (bs.kind != cs.kind) sd.fields.push_back("kind");
        if (bs.tokens != cs.tokens) sd.fields.push_back("tokens");
        if (bs.tools.size() != cs.tools.size()) sd.fields.push_back("tools");
        if (bs.fuzzyIn != cs.fuzzyIn) sd.fields.push_back("fuzzy_in");

        const std::size_t commonTools = std::min(bs.tools.size(), cs.tools.size());
        bool toolChanged = false;
        for (std::size_t t = 0; t < commonTools; ++t) {
            const ToolExec& bt = bs.tools[t];
            const ToolExec& ct = cs.tools[t];
            const int idx = curStart[i] + static_cast<int>(t);
            if (bt.name != ct.name) {
                ToolDelta td;
                td.toolIndex = idx;
                td.stepIndex = i;
                td.change = "name";
                td.name = ct.name;
                td.before = bt.name;
                td.after = ct.name;
                d.tools.push_back(td);
                toolChanged = true;
                continue;
            }
            if (bt.arguments != ct.arguments) {
                ToolDelta td;
                td.toolIndex = idx;
                td.stepIndex = i;
                td.change = "arguments";
                td.name = ct.name;
                td.before = bt.arguments;
                td.after = ct.arguments;
                d.tools.push_back(td);
                toolChanged = true;
            }
            if (bt.result != ct.result) {
                ToolDelta td;
                td.toolIndex = idx;
                td.stepIndex = i;
                td.change = "result";
                td.name = ct.name;
                td.before = bt.result;
                td.after = ct.result;
                d.tools.push_back(td);
                toolChanged = true;
            }
        }
        for (std::size_t t = commonTools; t < cs.tools.size(); ++t) {
            ToolDelta td;
            td.toolIndex = curStart[i] + static_cast<int>(t);
            td.stepIndex = i;
            td.change = "added";
            td.name = cs.tools[t].name;
            td.after = cs.tools[t].arguments;
            d.tools.push_back(td);
            toolChanged = true;
        }
        for (std::size_t t = commonTools; t < bs.tools.size(); ++t) {
            ToolDelta td;
            td.toolIndex = baseStart[i] + static_cast<int>(t);
            td.stepIndex = i;
            td.change = "removed";
            td.name = bs.tools[t].name;
            td.before = bs.tools[t].arguments;
            d.tools.push_back(td);
            toolChanged = true;
        }
        if (!sd.fields.empty() || toolChanged) {
            sd.change = "changed";
            d.steps.push_back(sd);
        }
    }

    if (base.missing.size() != cur.missing.size()) {
        d.notes.push_back("缺失步骤: base " + std::to_string(base.missing.size()) + " 条 → cur " +
                          std::to_string(cur.missing.size()) + " 条");
    }
    if (base.requestCount != cur.requestCount) {
        d.notes.push_back("原始请求: base " + std::to_string(base.requestCount) + " → cur " +
                          std::to_string(cur.requestCount));
    }
    if (base.retryCount != cur.retryCount) {
        d.notes.push_back("重试: base " + std::to_string(base.retryCount) + " → cur " +
                          std::to_string(cur.retryCount));
    }
    if (base.fuzzyEdges != cur.fuzzyEdges) {
        d.notes.push_back("模糊边: base " + std::to_string(base.fuzzyEdges) + " → cur " +
                          std::to_string(cur.fuzzyEdges));
    }
    return d;
}

} // namespace axr
