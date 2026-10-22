#include "xt/gesture.h"

#include <algorithm>
#include <cmath>

/// Docs/design/27-interaction.md §27.2.2/§27.7, Docs/tasks/T-194.md goal 6. The recognizer only
/// classifies; hit testing and dispatch happen in the Document the resulting InputEvent reaches.
///
/// Not covered by the design (conservative choices made here):
///   - A double tap reports its first tap too (Tap, then DoubleTap on the second).
///   - A swipe's direction is the dominant axis of its travel; a touch that was a hold never
///     becomes a tap or swipe.
///   - A pinch is reported on every move after its threshold, at the fingers' midpoint, with the
///     scale relative to the distance when the second finger touched down.

namespace xt {

namespace {

float distance(Vec2 a, Vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

}  // namespace

GestureRecognizer::Finger* GestureRecognizer::find(uint64_t id) {
    for (Finger& f : m_fingers) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

void GestureRecognizer::emit(GestureKind kind, Vec2 pos, NavDirection dir, float scale) {
    InputEvent ev;
    ev.kind = InputEvent::Gesture;
    ev.pos = pos;
    ev.gesture = kind;
    ev.direction = dir;
    ev.gestureScale = scale;
    ev.player = m_player;
    m_pending.push_back(ev);
}

void GestureRecognizer::touchDown(uint64_t finger, Vec2 pos, float time) {
    if (find(finger) != nullptr) {
        return;
    }
    if (m_fingers.empty()) {
        m_moved = false;
        m_held = false;
        m_multi = false;
        m_pinching = false;
    }
    m_fingers.push_back(Finger{finger, pos, pos, time});
    if (m_fingers.size() == 2) {
        m_multi = true;
        m_pinchStart = distance(m_fingers[0].pos, m_fingers[1].pos);
    }
}

void GestureRecognizer::touchMove(uint64_t finger, Vec2 pos, float /*time*/) {
    Finger* f = find(finger);
    if (f == nullptr) {
        return;
    }
    f->pos = pos;
    if (m_multi) {
        if (m_fingers.size() >= 2 && m_pinchStart > 0) {
            const float scale = distance(m_fingers[0].pos, m_fingers[1].pos) / m_pinchStart;
            if (m_pinching || std::fabs(scale - 1.0f) >= m_config.pinchMinChange) {
                m_pinching = true;
                const Vec2 mid{(m_fingers[0].pos.x + m_fingers[1].pos.x) * 0.5f,
                               (m_fingers[0].pos.y + m_fingers[1].pos.y) * 0.5f};
                emit(GestureKind::Pinch, mid, NavDirection::Up, scale);
            }
        }
        return;
    }
    if (distance(f->start, pos) > m_config.tapMaxMove) {
        m_moved = true;
    }
}

void GestureRecognizer::touchUp(uint64_t finger, Vec2 pos, float time) {
    Finger* f = find(finger);
    if (f == nullptr) {
        return;
    }
    const Finger done = *f;
    m_fingers.erase(m_fingers.begin() + (f - m_fingers.data()));
    if (m_multi) {
        return;  // The sequence ends when the last finger lifts; nothing single-finger remains.
    }
    if (m_held) {
        return;
    }
    const float dt = time - done.startTime;
    const Vec2 travel{pos.x - done.start.x, pos.y - done.start.y};
    const float dist = std::sqrt(travel.x * travel.x + travel.y * travel.y);
    if (dist >= m_config.swipeMinDistance && dt <= m_config.swipeMaxTime) {
        const NavDirection dir = std::fabs(travel.x) >= std::fabs(travel.y)
                                     ? (travel.x > 0 ? NavDirection::Right : NavDirection::Left)
                                     : (travel.y > 0 ? NavDirection::Down : NavDirection::Up);
        emit(GestureKind::Swipe, done.start, dir, 1);
        m_hasLastTap = false;
        return;
    }
    if (m_moved || dist > m_config.tapMaxMove || dt > m_config.tapMaxTime) {
        return;
    }
    emit(GestureKind::Tap, pos, NavDirection::Up, 1);
    if (m_hasLastTap && time - m_lastTapTime <= m_config.doubleTapInterval &&
        distance(m_lastTapPos, pos) <= 2.0f * m_config.tapMaxMove) {
        emit(GestureKind::DoubleTap, pos, NavDirection::Up, 1);
        m_hasLastTap = false;
        return;
    }
    m_hasLastTap = true;
    m_lastTapPos = pos;
    m_lastTapTime = time;
}

void GestureRecognizer::update(float time) {
    if (m_fingers.size() != 1 || m_multi || m_moved || m_held) {
        return;
    }
    const Finger& f = m_fingers.front();
    if (time - f.startTime >= m_config.holdTime) {
        m_held = true;
        m_hasLastTap = false;
        emit(GestureKind::Hold, f.pos, NavDirection::Up, 1);
    }
}

bool GestureRecognizer::poll(InputEvent& out) {
    if (m_pending.empty()) {
        return false;
    }
    out = m_pending.front();
    m_pending.erase(m_pending.begin());
    return true;
}

}  // namespace xt
