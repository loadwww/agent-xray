#include "chain/fuzzy.h"

#include "chain/fingerprint.h"

namespace axr {

double messageSimilarity(const std::vector<Message>& a, const std::vector<Message>& b, int window) {
    const std::size_t k = static_cast<std::size_t>(window > 0 ? window : 20);
    const std::size_t n = (a.size() < b.size() ? a.size() : b.size());
    const std::size_t limit = (k < n ? k : n);
    if (limit == 0) return 0.0;

    std::size_t same = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (canonical(a[i]) == canonical(b[i])) ++same;
    }
    return static_cast<double>(same) / static_cast<double>(limit);
}

int fuzzyParent(const std::vector<LLMCall>& calls, int idx, const ChainOptions& opts) {
    if (idx <= 0) return -1;
    int best = -1;
    double bestScore = 0.0;
    for (int j = 0; j < idx; ++j) {
        // 父必须先于子，且消息数严格更少
        if (calls[j].messages.size() >= calls[idx].messages.size()) continue;
        const double score = messageSimilarity(calls[j].messages, calls[idx].messages, opts.fuzzyWindow);
        if (score >= opts.fuzzyThreshold && score > bestScore) {
            bestScore = score;
            best = j;
        }
    }
    return best;
}

} // namespace axr
