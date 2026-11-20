#pragma once

#include <cstdint>

/// Why something was invalidated (Docs/design/57-measurement.md §57.3, Docs/tasks/T-747.md): every
/// write that can invalidate style, layout, transform or paint carries one of these codes, the
/// per-frame cause table (FrameStats::causes) counts them, and the Document's visual generation
/// (Document::visualGeneration()) moves on every such write. The list is fixed (§57.3 item 1).

namespace xt {

/// The cause of one invalidating write (57 §57.3 item 1). New values only ever go before Count.
enum class DirtyCause : uint8_t {
    Animation = 0,   ///< A CSS `@keyframes` animation.
    Timeline = 1,    ///< A timeline (`@timeline` group, an `<anim>` scene, Document::play...).
    Transition = 2,  ///< A CSS transition (and a binding tween).
    Binding = 3,     ///< A port value and the bindings reading it.
    Flow = 4,        ///< A write from a flow graph (xt_flow's sinks).
    HostApi = 5,     ///< A direct host call (setStyle, setText, setCustomState, ...).
    Input = 6,       ///< Hover, focus, active, a state change from input.
    Viewport = 7,    ///< Size, DPR, safe area, tokens / theme, the paint options.
    External = 8,    ///< An external texture's content or binding.
    Clock = 9,       ///< Time alone: the caret, time-driven modifiers, shader time, particles.
    Load = 10,       ///< Load, reload, unload, `<repeat>` entries added or removed.
    Count = 11
};

/// The number of causes (the rows of FrameStats::causes).
inline constexpr uint32_t kDirtyCauseCount = static_cast<uint32_t>(DirtyCause::Count);
/// The kinds of invalidation counted per cause (the columns of FrameStats::causes).
inline constexpr uint32_t kDirtyKindStyle = 0;
inline constexpr uint32_t kDirtyKindLayout = 1;
inline constexpr uint32_t kDirtyKindTransform = 2;
inline constexpr uint32_t kDirtyKindPaint = 3;
inline constexpr uint32_t kDirtyKindCount = 4;
/// The node ids FrameStats::causeNodes keeps per cause (detail level >= 1).
inline constexpr uint32_t kDirtyCauseNodeSlots = 4;

/// The English name of `cause` (`Animation`, `Timeline`, ...); "?" for Count or an unknown value.
inline const char* dirtyCauseName(DirtyCause cause) {
    switch (cause) {
        case DirtyCause::Animation:
            return "Animation";
        case DirtyCause::Timeline:
            return "Timeline";
        case DirtyCause::Transition:
            return "Transition";
        case DirtyCause::Binding:
            return "Binding";
        case DirtyCause::Flow:
            return "Flow";
        case DirtyCause::HostApi:
            return "HostApi";
        case DirtyCause::Input:
            return "Input";
        case DirtyCause::Viewport:
            return "Viewport";
        case DirtyCause::External:
            return "External";
        case DirtyCause::Clock:
            return "Clock";
        case DirtyCause::Load:
            return "Load";
        case DirtyCause::Count:
            break;
    }
    return "?";
}

}  // namespace xt
