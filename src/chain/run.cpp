#include "chain/run.h"

#include <algorithm>
#include <map>
#include <set>

#include "chain/prefix.h"
#include "chain/superstep.h"
#include "chain/validate.h"
#include "common/time_util.h"

namespace axr {
namespace {

// "2026-10-30T14:30:12.100+08:00" → "20261030-143012"
std::string compactStamp(const std::string& rfc3339) {
    if (rfc3339.size() < 19) return "unknown";
    std::string date, time;
    for (char c : rfc3339.substr(0, 10)) {
        if (c >= '0' && c <= '9') date.push_back(c);
    }
    for (char c : rfc3339.substr(11, 8)) {
        if (c >= '0' && c <= '9') time.push_back(c);
    }
    if (date.size() != 8 || time.size() != 6) return "unknown";
    return date + "-" + time;
}

long long gapMinutes(const std::string& a, const std::string& b) {
    auto ta = parseRfc3339(a);
    auto tb = parseRfc3339(b);
    if (!ta.ok() || !tb.ok()) return 0;
    return diffMillis(tb.value, ta.value) / 60000;
}

} // namespace

ChainResult buildRuns(std::vector<CallPair> pairs, const ChainOptions& opts) {
    ChainResult result;
    if (pairs.empty()) return result;

    // 1. 按时间排序（RFC3339 字符串同偏移可直接比大小，这里用解析保证稳健）
    std::stable_sort(pairs.begin(), pairs.end(), [](const CallPair& a, const CallPair& b) {
        auto ta = parseRfc3339(a.call.startedAt);
        auto tb = parseRfc3339(b.call.startedAt);
        if (ta.ok() && tb.ok()) return ta.value < tb.value;
        return a.call.startedAt < b.call.startedAt;
    });

    std::vector<LLMCall> calls;
    calls.reserve(pairs.size());
    for (auto& p : pairs) calls.push_back(p.call);

    // 2. 建父子关系
    LinkResult lr = buildLinks(calls, opts);
    result.links = lr.links;

    std::set<int> retryIdx;
    for (const auto& l : lr.links) {
        if (l.retry) retryIdx.insert(l.to);
        if (l.fuzzy) ++result.fuzzyCount;
    }
    result.retryCount = static_cast<int>(retryIdx.size());

    // 3. 邻接表 + 根识别
    std::map<int, std::vector<int>> children;
    std::vector<int> roots;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (retryIdx.count(static_cast<int>(i))) continue;
        const int parent = lr.parent[i];
        if (parent < 0) roots.push_back(static_cast<int>(i));
        else children[parent].push_back(static_cast<int>(i));
    }

    // 4. 逐根收集后代，按时间窗切成若干 run
    std::map<int, std::size_t> callToRun; // 请求下标 → run 下标（重试计数归并要用）
    for (int root : roots) {
        std::vector<int> group;
        std::vector<int> stack{root};
        std::set<int> seen{root};
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            group.push_back(cur);
            auto it = children.find(cur);
            if (it == children.end()) continue;
            for (int c : it->second) {
                if (seen.insert(c).second) stack.push_back(c);
            }
        }
        std::sort(group.begin(), group.end());

        // 时间窗兜底切分
        std::vector<std::vector<int>> segments;
        std::vector<int> seg{group.front()};
        for (std::size_t i = 1; i < group.size(); ++i) {
            if (gapMinutes(pairs[group[i - 1]].call.startedAt, pairs[group[i]].call.startedAt) > opts.runGapMinutes) {
                segments.push_back(seg);
                seg.clear();
            }
            seg.push_back(group[i]);
        }
        segments.push_back(seg);

        for (const auto& s : segments) {
            Run run;
            run.rootHash = prefixHashes(pairs[s.front()].call.messages).back();
            run.id = compactStamp(pairs[s.front()].call.startedAt) + "-" + run.rootHash.substr(0, 8);
            run.startedAt = pairs[s.front()].call.startedAt;
            run.requestCount = static_cast<int>(s.size());

            for (std::size_t k = 0; k < s.size(); ++k) {
                const CallPair& cp = pairs[s[k]];
                std::vector<ToolExec> tools;
                if (k + 1 < s.size()) {
                    const CallPair& next = pairs[s[k + 1]];
                    const auto startNext = parseRfc3339(next.call.startedAt);
                    const auto endPrev = parseRfc3339(cp.call.endedAt);
                    long long approxMs = 0;
                    if (startNext.ok() && endPrev.ok()) approxMs = diffMillis(startNext.value, endPrev.value);
                    tools = toolsFromDelta(cp.call.messages, next.call.messages, cp.resp.toolCalls,
                                           static_cast<int>(approxMs > 0 ? approxMs : 0));
                }
                Step step = buildStep(static_cast<int>(k), cp.call, cp.resp, std::move(tools));
                // 相邻两步之间若是模糊匹配边，标记到后一步（图上画虚线并注"推测"）
                if (k > 0) {
                    for (const auto& l : lr.links) {
                        if (l.fuzzy && l.from == s[k - 1] && l.to == s[k]) {
                            step.fuzzyIn = true;
                            ++run.fuzzyEdges;
                            break;
                        }
                    }
                }
                run.total.promptTokens += cp.resp.usage.promptTokens;
                run.total.completionTokens += cp.resp.usage.completionTokens;
                run.total.cachedTokens += cp.resp.usage.cachedTokens;
                run.steps.push_back(std::move(step));
            }

            run.endedAt = pairs[s.back()].call.endedAt.empty() ? pairs[s.back()].call.startedAt
                                                               : pairs[s.back()].call.endedAt;
            run.incomplete = !validate(run, run.missing);
            const std::size_t runIdx = result.runs.size();
            for (int idx : s) callToRun[idx] = runIdx;
            result.runs.push_back(std::move(run));
        }
    }

    // 5. 重试请求（完整哈希重复）不进 steps，但归入其父请求所在的 run 统计
    for (const auto& l : lr.links) {
        if (!l.retry) continue;
        auto it = callToRun.find(l.from);
        if (it == callToRun.end()) continue;
        Run& r = result.runs[it->second];
        ++r.retryCount;
        ++r.requestCount;
    }

    std::sort(result.runs.begin(), result.runs.end(),
              [](const Run& a, const Run& b) { return a.startedAt < b.startedAt; });
    return result;
}

const Run* primaryRun(const ChainResult& cr) {
    if (cr.runs.empty()) return nullptr;
    const Run* best = &cr.runs.front();
    for (const auto& r : cr.runs)
        if (r.steps.size() >= best->steps.size()) best = &r;
    return best;
}

} // namespace axr
