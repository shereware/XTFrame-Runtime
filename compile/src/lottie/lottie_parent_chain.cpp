#include "lottie_parent_chain.h"

#include <utility>

namespace xt::compile {

LottieParentChainWalker::LottieParentChainWalker(size_t layerCount,
                                                 std::function<std::ptrdiff_t(size_t)> parentOf,
                                                 size_t maxChain)
    : m_state(layerCount, 0), m_parentOf(std::move(parentOf)), m_maxChain(maxChain) {}

LottieParentChain LottieParentChainWalker::walk(size_t pos) {
    LottieParentChain chain;
    if (m_state[pos] == kPlaced) {
        return chain;
    }
    for (size_t at = pos;;) {
        m_state[at] = kVisiting;
        chain.order.push_back(at);
        const std::ptrdiff_t parent = m_parentOf(at);
        if (parent < 0) {
            break;
        }
        const size_t parentPos = static_cast<size_t>(parent);
        if (m_state[parentPos] == kPlaced) {
            chain.attachTo = parent;
            break;
        }
        if (m_state[parentPos] == kVisiting) {
            chain.cycleAt = parent;
            break;
        }
        if (chain.order.size() >= m_maxChain) {
            chain.tooLong = true;
            break;
        }
        at = parentPos;
    }
    std::vector<size_t> topDown(chain.order.rbegin(), chain.order.rend());
    chain.order = std::move(topDown);
    for (size_t at : chain.order) {
        m_state[at] = kPlaced;
    }
    return chain;
}

}  // namespace xt::compile
