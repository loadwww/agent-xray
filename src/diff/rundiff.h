#pragma once
// run 差异对比（§8 axr diff / §10 M4）：按 Step 序号对齐，报步骤与工具级变化
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// 一个 Step 的变化
struct StepDelta {
    int index = 0;                  // 0-based 序号（展示为 S{index}）
    std::string change;             // added | removed | changed
    std::vector<std::string> fields; // change=="changed" 时具体字段：model / kind / tokens / tools / fuzzy_in
    std::string modelBefore;
    std::string modelAfter;
    int tokensBefore = 0;
    int tokensAfter = 0;
};

// 一次工具调用的变化（工具按 run 内出现顺序全局编号 T0、T1…）
struct ToolDelta {
    int toolIndex = 0;      // 全局工具序号
    int stepIndex = 0;      // 所在 Step（0-based）
    std::string change;     // added | removed | name | arguments | result
    std::string name;       // 工具名（name 变化时为新名）
    std::string before;
    std::string after;
};

struct RunDiff {
    int baseSteps = 0;
    int curSteps = 0;
    int baseTools = 0;
    int curTools = 0;
    int baseTokens = 0;
    int curTokens = 0;
    std::vector<StepDelta> steps;
    std::vector<ToolDelta> tools;
    std::vector<std::string> notes; // 缺失步骤 / 重试 / 模糊边等非结构性差异

    bool empty() const { return steps.empty() && tools.empty(); }
};

std::size_t countTools(const Run& run);

// 按 Step 序号对齐做差异
RunDiff diffRuns(const Run& base, const Run& cur);

} // namespace axr
