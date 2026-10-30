#pragma once
// cost 层：token → 美元（§7.5）
#include <map>
#include <string>
#include <vector>

#include "config/config.h"
#include "parse/model.h"

namespace axr {

struct CostBreakdown {
    bool hasUsage = false; // 是否至少有一处 usage
    bool complete = true;  // 所有步骤都有 usage（否则成本显示 n/a）
    double totalUsd = 0.0;
    std::vector<double> stepUsd;  // 与 run.steps 对齐
    std::vector<bool> stepKnown;  // 对应步骤是否有 usage
    std::map<std::string, double> byModel;
    std::map<std::string, double> byTool; // 按触发该工具的 step 成本汇总
};

// 未知模型走 fallback
const ModelPrice& priceFor(const PricingCfg& pricing, const std::string& model);

CostBreakdown computeCost(const Run& run, const PricingCfg& pricing);

// 把成本写回 Step.costUsd / Run.costUsd（落盘与报告用）
void applyCost(Run& run, const PricingCfg& pricing);

// "$0.001234" / "n/a"
std::string fmtCost(double usd);

} // namespace axr
