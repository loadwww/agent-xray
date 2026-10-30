#include "chain/link.h"

#include <map>

#include "chain/fuzzy.h"
#include "chain/prefix.h"

namespace axr {

LinkResult buildLinks(const std::vector<LLMCall>& calls, const ChainOptions& opts) {
    LinkResult result;
    result.parent.assign(calls.size(), -1);
    if (calls.empty()) return result;

    // 完整哈希 → 请求下标（用于重试识别）
    std::map<std::string, int> fullHashOwner;
    // 任意前缀哈希 → 拥有该前缀的请求下标（增量构建，只含更早的请求）
    std::map<std::string, int> prefixOwner;

    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto hashes = prefixHashes(calls[i].messages);
        const std::string full = hashes.back();

        // 重试：完整哈希已出现过 → 标记，不建新边
        auto retryIt = fullHashOwner.find(full);
        if (retryIt != fullHashOwner.end() && retryIt->second != static_cast<int>(i)) {
            Link retry;
            retry.from = retryIt->second;
            retry.to = static_cast<int>(i);
            retry.newMsgs = 0;
            retry.retry = true;
            result.links.push_back(retry);
            result.parent[i] = -1; // 重试请求不参与链（避免重复步骤）
            continue;
        }

        // 从后往前找最长命中的前缀
        int bestParent = -1;
        int bestK = -1;
        for (int k = static_cast<int>(calls[i].messages.size()); k >= 1; --k) {
            auto it = prefixOwner.find(hashes[static_cast<std::size_t>(k)]);
            if (it != prefixOwner.end() && it->second != static_cast<int>(i)) {
                bestParent = it->second;
                bestK = k;
                break;
            }
        }

        Link link;
        link.to = static_cast<int>(i);
        if (bestParent >= 0) {
            link.from = bestParent;
            link.newMsgs = static_cast<int>(calls[i].messages.size()) - bestK;
            link.fuzzy = false;
            result.parent[i] = bestParent;
            result.links.push_back(link);
        } else if (i > 0) {
            // 前缀失配 → 模糊匹配兜底
            const int fuzzy = fuzzyParent(calls, static_cast<int>(i), opts);
            if (fuzzy >= 0) {
                link.from = fuzzy;
                link.newMsgs = static_cast<int>(calls[i].messages.size()) - static_cast<int>(calls[fuzzy].messages.size());
                link.fuzzy = true;
                result.parent[i] = fuzzy;
                result.links.push_back(link);
            }
        }

        // 把自己的所有前缀记入索引（用最后一个下标：最新拥有者）
        fullHashOwner[full] = static_cast<int>(i);
        for (std::size_t k = 0; k < hashes.size(); ++k) prefixOwner[hashes[k]] = static_cast<int>(i);
    }

    return result;
}

} // namespace axr
