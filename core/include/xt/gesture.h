#pragma once

#include <cstdint>
#include <vector>

#include "xt/input.h"
#include "xt/types.h"

/// Touch gesture recognition (docs/design/27-interaction.md §27.2.2 `gesture(...)`, §27.7,
/// docs/tasks/T-194.md goal 6). A host feeds raw touch points in; recognized gestures come out as
/// InputEvent::Gesture events for Context::pushInput(). Pure logic (no platform code): xt_shell
/// drives it from SDL touch events.

namespace xt {

/// Thresholds of a GestureRecognizer. Distances are logical pixels, times seconds.
struct GestureConfig {
    /// A tap moves less than this from its touch-down point.
    float tapMaxMove = 10;
    /// A tap lasts at most this long.
    float tapMaxTime = 0.3f;
    /// A second tap within this time (and 2 * tapMaxMove) of the first is a double tap.
    float doubleTapInterval = 0.3f;
    /// A finger held still (within tapMaxMove) this long is a hold.
    float holdTime = 0.5f;
    /// A swipe travels at least this far...
    float swipeMinDistance = 50;
    /// ...within this time.
    float swipeMaxTime = 0.6f;
    /// A pinch reports once the finger distance ratio moved this far from 1 (then on every move).
    float pinchMinChange = 0.1f;
};

/// Turns touch points into Tap, DoubleTap, Swipe, Pinch and Hold gestures (GestureKind). One
/// finger: tap / double tap / swipe / hold; two fingers: pinch (single-finger gestures of that
/// touch sequence are then suppressed). Not thread-safe.
class GestureRecognizer {
public:
    /// Replaces the thresholds; takes effect for the next touch sequence.
    void setConfig(const GestureConfig& config) { m_config = config; }
    /// The current thresholds.
    const GestureConfig& config() const { return m_config; }

    /// A finger touched down at `pos` (logical pixels) at `time` (seconds).
    void touchDown(uint64_t finger, Vec2 pos, float time);
    /// A finger moved.
    void touchMove(uint64_t finger, Vec2 pos, float time);
    /// A finger lifted.
    void touchUp(uint64_t finger, Vec2 pos, float time);
    /// Advances time without a touch event (needed for Hold); call once per frame.
    void update(float time);
    /// Pops the oldest recognized gesture into `out` (kind Gesture, `player` = the recognizer's
    /// player); false when none is pending.
    bool poll(InputEvent& out);
    /// The InputEvent::player stamped on the gestures (default 0).
    void setPlayer(uint8_t player) { m_player = player; }

private:
    struct Finger {
        uint64_t id = 0;
        Vec2 start{};
        Vec2 pos{};
        float startTime = 0;
    };
    Finger* find(uint64_t id);
    void emit(GestureKind kind, Vec2 pos, NavDirection dir, float scale);

    GestureConfig m_config{};
    std::vector<Finger> m_fingers;
    std::vector<InputEvent> m_pending;
    uint8_t m_player = 0;
    bool m_moved = false;     // The single finger left the tap radius.
    bool m_held = false;      // Hold already reported for this touch.
    bool m_multi = false;     // A second finger joined: only pinch for this sequence.
    float m_pinchStart = 0;   // Finger distance when the second finger went down.
    bool m_pinching = false;  // pinchMinChange crossed.
    bool m_hasLastTap = false;
    Vec2 m_lastTapPos{};
    float m_lastTapTime = 0;
};

}  // namespace xt
