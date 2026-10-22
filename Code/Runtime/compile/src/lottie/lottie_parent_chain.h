#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace xt::compile {

/// One layer's parent chain, as LottieParentChainWalker::walk() returns it (T-257b).
struct LottieParentChain {
    /// The layers to place, top down: order[0] hangs from `attachTo`, each later one from the one
    /// before it, and back() is the layer walk() was asked for. Empty when it was already placed.
    std::vector<size_t> order;
    /// The already placed layer order[0] hangs from, or -1 (the composition's root).
    std::ptrdiff_t attachTo = -1;
    /// The layer met a second time while walking up (a parent cycle), or -1.
    std::ptrdiff_t cycleAt = -1;
    /// The walk stopped after `maxChain` layers while order[0] still had a parent to follow.
    bool tooLong = false;
};

/// The one walker of Lottie `parent` chains, shared by the compiler (lottie_build.cpp) and the
/// native importer (Code/Editor/Import/src/lottie_native.cpp), each with its own chain limit and its own
/// reporting. Never recurses: a chain is walked up, marking each layer "visiting" (meeting one
/// again is a cycle), until a placed layer, a layer without (or with a dangling) parent, or
/// `maxChain` layers; the chain is then returned top down and every layer of it counts as placed.
class LottieParentChainWalker {
public:
    /// `parentOf(pos)` is the position of layer `pos`'s parent, or -1 (none, or dangling).
    LottieParentChainWalker(size_t layerCount, std::function<std::ptrdiff_t(size_t)> parentOf,
                            size_t maxChain);

    /// Walks `pos`'s chain; see LottieParentChain.
    LottieParentChain walk(size_t pos);

    bool placed(size_t pos) const { return m_state[pos] == kPlaced; }

private:
    static constexpr uint8_t kVisiting = 1;
    static constexpr uint8_t kPlaced = 2;

    std::vector<uint8_t> m_state;
    std::function<std::ptrdiff_t(size_t)> m_parentOf;
    size_t m_maxChain;
};

}  // namespace xt::compile
