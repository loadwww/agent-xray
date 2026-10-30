#include "cost/pricing.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace axr {

const ModelPrice& priceFor(const PricingCfg& pricing, const std::string& model) {
    auto it = pricing.models.find(model);
    if (it != pricing.models.end()) return it->second;
    return pricing.fallback;
}

CostBreakdown computeCost(const Run& run, const PricingCfg& pricing) {
    CostBreakdown cb;
    cb.stepUsd.assign(run.steps.size(), 0.0);
    cb.stepKnown.assign(run.steps.size(), false);

    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const Step& s = run.steps[i];
        const Usage& u = s.response.usage;
        // T9：上游不回 usage 时不能假装成本为 0
        const bool known = u.promptTokens > 0 || u.completionTokens > 0 || u.cachedTokens > 0;
        cb.stepKnown[i] = known;
        if (!known) {
            cb.complete = false;
            continue;
        }
        cb.hasUsage = true;

        const ModelPrice& p = priceFor(pricing, s.call.model);
        const double cachedPrice = p.cached >= 0.0 ? p.cached : p.input / 10.0;

        const int cached = std::min(u.cachedTokens, u.promptTokens);
        const int fresh = u.promptTokens - cached;
        const double cost =
            (static_cast<double>(fresh) * p.input + static_cast<double>(cached) * cachedPrice +
             static_cast<double>(u.completionTokens) * p.output) /
            1000000.0;

        cb.stepUsd[i] = cost;
        cb.totalUsd += cost;
        cb.byModel[s.call.model.empty() ? "<unknown>" : s.call.model] += cost;

        std::set<std::string> names;
        for (const auto& te : s.tools) {
            if (!te.name.empty()) names.insert(te.name);
        }
        for (const auto& n : names) cb.byTool[n] += cost;
    }
    return cb;
}

void applyCost(Run& run, const PricingCfg& pricing) {
    const CostBreakdown cb = computeCost(run, pricing);
    run.costUsd = cb.totalUsd;
    for (std::size_t i = 0; i < run.steps.size(); ++i) run.steps[i].costUsd = cb.stepUsd[i];
}

std::string fmtCost(double usd) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "$%.6f", usd);
    return std::string(buf);
}

} // namespace axr
