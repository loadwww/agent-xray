#include "chain/prefix.h"

#include "chain/fingerprint.h"
#include "common/hash.h"

namespace axr {

std::vector<std::string> prefixHashes(const std::vector<Message>& msgs) {
    std::vector<std::string> h;
    h.reserve(msgs.size() + 1);
    h.push_back(sha256Hex("root").substr(0, 32));
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        const std::string stepHash = sha256Hex(canonical(msgs[i]));
        h.push_back(sha256Hex(h.back() + stepHash).substr(0, 32));
    }
    return h;
}

} // namespace axr
