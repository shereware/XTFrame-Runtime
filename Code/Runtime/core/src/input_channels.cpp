#include "xt/input_channels.h"

#include <algorithm>
#include <cmath>
#include <utility>

/// Input channels: the name grammar, the key names and the per-player state (Docs/design/
/// 60-input-channels.md §60.2, §60.5; Docs/tasks/T-873.md). See xt/input_channels.h.

namespace xt {

namespace {

struct KeyName {
    const char* name;
    KeyCode code;
};

// The canonical name of each code comes first; aliases follow (the spellings `key(<name>)` accepted
// before T-873).
constexpr KeyName kKeyNames[] = {
    {"Enter", KeyCode::Enter},
    {"Escape", KeyCode::Escape},
    {"Backspace", KeyCode::Backspace},
    {"Tab", KeyCode::Tab},
    {"Space", KeyCode::Space},
    {"Delete", KeyCode::Delete},
    {"Home", KeyCode::Home},
    {"End", KeyCode::End},
    {"ArrowLeft", KeyCode::Left},
    {"ArrowRight", KeyCode::Right},
    {"ArrowUp", KeyCode::Up},
    {"ArrowDown", KeyCode::Down},
    {"PageUp", KeyCode::PageUp},
    {"PageDown", KeyCode::PageDown},
    {"Insert", KeyCode::Insert},
    {"F1", KeyCode::F1},
    {"F2", KeyCode::F2},
    {"F3", KeyCode::F3},
    {"F4", KeyCode::F4},
    {"F5", KeyCode::F5},
    {"F6", KeyCode::F6},
    {"F7", KeyCode::F7},
    {"F8", KeyCode::F8},
    {"F9", KeyCode::F9},
    {"F10", KeyCode::F10},
    {"F11", KeyCode::F11},
    {"F12", KeyCode::F12},
    // T-873 (60 §60.2): modifiers, punctuation and the numeric keypad.
    {"ShiftLeft", KeyCode::ShiftLeft},
    {"ShiftRight", KeyCode::ShiftRight},
    {"ControlLeft", KeyCode::ControlLeft},
    {"ControlRight", KeyCode::ControlRight},
    {"AltLeft", KeyCode::AltLeft},
    {"AltRight", KeyCode::AltRight},
    {"MetaLeft", KeyCode::MetaLeft},
    {"MetaRight", KeyCode::MetaRight},
    {"CapsLock", KeyCode::CapsLock},
    {"Minus", KeyCode::Minus},
    {"Equal", KeyCode::Equal},
    {"BracketLeft", KeyCode::BracketLeft},
    {"BracketRight", KeyCode::BracketRight},
    {"Backslash", KeyCode::Backslash},
    {"Semicolon", KeyCode::Semicolon},
    {"Quote", KeyCode::Quote},
    {"Backquote", KeyCode::Backquote},
    {"Comma", KeyCode::Comma},
    {"Period", KeyCode::Period},
    {"Slash", KeyCode::Slash},
    {"Numpad0", KeyCode::Numpad0},
    {"Numpad1", KeyCode::Numpad1},
    {"Numpad2", KeyCode::Numpad2},
    {"Numpad3", KeyCode::Numpad3},
    {"Numpad4", KeyCode::Numpad4},
    {"Numpad5", KeyCode::Numpad5},
    {"Numpad6", KeyCode::Numpad6},
    {"Numpad7", KeyCode::Numpad7},
    {"Numpad8", KeyCode::Numpad8},
    {"Numpad9", KeyCode::Numpad9},
    {"NumpadAdd", KeyCode::NumpadAdd},
    {"NumpadSubtract", KeyCode::NumpadSubtract},
    {"NumpadMultiply", KeyCode::NumpadMultiply},
    {"NumpadDivide", KeyCode::NumpadDivide},
    {"NumpadDecimal", KeyCode::NumpadDecimal},
    {"NumpadEnter", KeyCode::NumpadEnter},
    // Aliases.
    {"Left", KeyCode::Left},
    {"Right", KeyCode::Right},
    {"Up", KeyCode::Up},
    {"Down", KeyCode::Down},
};

constexpr const char* kLetterNames[26] = {
    "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL", "KeyM",
    "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX", "KeyY", "KeyZ"};
constexpr const char* kDigitNames[10] = {"Digit0", "Digit1", "Digit2", "Digit3", "Digit4",
                                         "Digit5", "Digit6", "Digit7", "Digit8", "Digit9"};

// key.Shift / Control / Alt / Meta: either side held.
constexpr const char* kModifierNames[4] = {"Shift", "Control", "Alt", "Meta"};

constexpr const char* kPadButtons[15] = {"a",  "b",  "x",  "y",  "back", "guide", "start", "ls",
                                         "rs", "lb", "rb", "up", "down", "left",  "right"};
constexpr const char* kPadAxes[6] = {"lx", "ly", "rx", "ry", "lt", "rt"};

bool parseUint(std::string_view text, uint32_t max, uint32_t& out) {
    if (text.empty() || text.size() > 6) {
        return false;
    }
    uint32_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10u + static_cast<uint32_t>(c - '0');
    }
    if (v > max) {
        return false;
    }
    out = v;
    return true;
}

/// `<name>(<n>)` at the start of `text` -> n (min..max) and the rest after `)`.
bool callArg(std::string_view text, std::string_view name, uint32_t min, uint32_t max, uint32_t& n,
             std::string_view& rest) {
    if (text.size() < name.size() + 2 || text.substr(0, name.size()) != name || text[name.size()] != '(') {
        return false;
    }
    const size_t close = text.find(')', name.size() + 1);
    if (close == std::string_view::npos ||
        !parseUint(text.substr(name.size() + 1, close - name.size() - 1), max, n) || n < min) {
        return false;
    }
    rest = text.substr(close + 1);
    return true;
}

/// The part of a midi name after `midi.` (or after `midi.ch(c).`).
bool canonicalMidi(std::string_view text, bool allowChannel, std::string& out) {
    uint32_t n = 0;
    std::string_view rest;
    if (allowChannel && callArg(text, "ch", 1, 16, n, rest)) {
        if (rest.size() < 2 || rest[0] != '.') {
            return false;
        }
        std::string inner;
        if (!canonicalMidi(rest.substr(1), false, inner)) {
            return false;
        }
        out = "ch(" + std::to_string(n) + ")." + inner;
        return true;
    }
    if (text == "pitch" || text == "pressure") {
        out = std::string(text);
        return true;
    }
    for (const char* call : {"note", "cc"}) {
        if (callArg(text, call, 0, 127, n, rest) && rest.empty()) {
            out = std::string(call) + "(" + std::to_string(n) + ")";
            return true;
        }
    }
    return false;
}

/// A channel name without a player prefix, blanks already removed.
bool canonicalName(std::string_view text, std::string& out, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err != nullptr) {
            *err = why;
        }
        return false;
    };
    if (text.rfind("pad.", 0) == 0) {
        const std::string_view rest = text.substr(4);
        for (const char* b : kPadButtons) {
            if (rest == b) {
                out = std::string(text);
                return true;
            }
        }
        for (const char* a : kPadAxes) {
            if (rest == a) {
                out = std::string(text);
                return true;
            }
        }
        return fail(
            "unknown gamepad channel \"" + std::string(text) +
            "\" (pad.a|b|x|y|lb|rb|back|start|guide|ls|rs|up|down|left|right, pad.lx|ly|rx|ry|lt|rt)");
    }
    if (text.rfind("key.", 0) == 0) {
        const std::string_view rest = text.substr(4);
        for (const char* m : kModifierNames) {
            if (rest == m) {
                out = std::string(text);
                return true;
            }
        }
        KeyCode code = KeyCode::Unknown;
        if (!keyCodeFromName(rest, code)) {
            return fail("unknown key \"" + std::string(rest) + "\" in \"" + std::string(text) + "\"");
        }
        out = "key." + std::string(keyCodeName(code));
        return true;
    }
    if (text == "wheel.x" || text == "wheel.y" || text == "pointer.dx" || text == "pointer.dy" ||
        text == "pointer.left" || text == "pointer.middle" || text == "pointer.right") {
        out = std::string(text);
        return true;
    }
    if (text.rfind("midi.", 0) == 0) {
        std::string inner;
        if (!canonicalMidi(text.substr(5), true, inner)) {
            return fail(
                "unknown MIDI channel \"" + std::string(text) +
                "\" (midi.note(0-127), midi.cc(0-127), midi.pitch, midi.pressure, midi.ch(1-16).<...>)");
        }
        out = "midi." + inner;
        return true;
    }
    if ((text.rfind("host.", 0) == 0 && text.size() > 5) ||
        (text.rfind("action:", 0) == 0 && text.size() > 7)) {
        out = std::string(text);
        return true;
    }
    return fail("unknown input channel \"" + std::string(text) +
                "\" (pad.*, key.*, wheel.*, pointer.*, midi.*, host.<name>, action:<name>)");
}

std::string withoutBlanks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

bool keyCodeFromName(std::string_view name, KeyCode& out) {
    for (const KeyName& k : kKeyNames) {
        if (name == k.name) {
            out = k.code;
            return true;
        }
    }
    // `A`..`Z` / `KeyA`..`KeyZ`, `0`..`9` / `Digit0`..`Digit9`.
    std::string_view s = name;
    if (s.size() == 4 && s.substr(0, 3) == "Key") {
        s = s.substr(3);
    } else if (s.size() == 6 && s.substr(0, 5) == "Digit") {
        s = s.substr(5);
    }
    if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z') {
        out = static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::KeyA) + static_cast<uint16_t>(s[0] - 'A'));
        return true;
    }
    if (s.size() == 1 && s[0] >= '1' && s[0] <= '9') {
        out = static_cast<KeyCode>(static_cast<uint16_t>(KeyCode::Key1) + static_cast<uint16_t>(s[0] - '1'));
        return true;
    }
    if (s.size() == 1 && s[0] == '0') {
        out = KeyCode::Key0;
        return true;
    }
    return false;
}

std::string_view keyCodeName(KeyCode code) {
    const auto v = static_cast<uint16_t>(code);
    if (v >= static_cast<uint16_t>(KeyCode::KeyA) && v <= static_cast<uint16_t>(KeyCode::KeyZ)) {
        return kLetterNames[v - static_cast<uint16_t>(KeyCode::KeyA)];
    }
    if (v >= static_cast<uint16_t>(KeyCode::Key1) && v <= static_cast<uint16_t>(KeyCode::Key9)) {
        return kDigitNames[1 + v - static_cast<uint16_t>(KeyCode::Key1)];
    }
    if (code == KeyCode::Key0) {
        return kDigitNames[0];
    }
    for (const KeyName& k : kKeyNames) {
        if (k.code == code) {
            return k.name;
        }
    }
    return {};
}

bool parseChannelRef(std::string_view text, std::string& canonical, uint8_t& player, std::string* err) {
    const std::string t = withoutBlanks(text);
    std::string_view rest = t;
    player = 0;
    uint32_t n = 0;
    std::string_view after;
    if (rest.rfind("player(", 0) == 0) {
        if (!callArg(rest, "player", 0, kMaxPlayers - 1, n, after) || after.size() < 2 || after[0] != '.') {
            if (err != nullptr) {
                *err = "expected player(<0-7>).<channel>, got \"" + t + "\"";
            }
            return false;
        }
        player = static_cast<uint8_t>(n);
        rest = after.substr(1);
    }
    return canonicalName(rest, canonical, err);
}

bool fixedChannelKind(std::string_view name, ChannelKind& out) {
    if (name.rfind("host.", 0) == 0 || name.rfind("action:", 0) == 0) {
        return false;
    }
    if (name.rfind("pad.", 0) == 0) {
        const std::string_view rest = name.substr(4);
        out = ChannelKind::Button;
        for (const char* a : kPadAxes) {
            if (rest == a) {
                out = ChannelKind::Axis;
            }
        }
        return true;
    }
    if (name.rfind("key.", 0) == 0 || name.rfind("pointer.left", 0) == 0 || name == "pointer.middle" ||
        name == "pointer.right") {
        out = ChannelKind::Button;
        return true;
    }
    if (name.rfind("wheel.", 0) == 0 || name == "pointer.dx" || name == "pointer.dy") {
        out = ChannelKind::Relative;
        return true;
    }
    if (name.rfind("midi.", 0) == 0) {
        const size_t last = name.rfind('.');
        const std::string_view tail = name.substr(last + 1);
        if (tail.rfind("cc(", 0) == 0) {
            return false;  // Axis, or relative when its first push says so (60 §60.6).
        }
        out = tail.rfind("note(", 0) == 0 ? ChannelKind::Button : ChannelKind::Axis;
        return true;
    }
    return false;
}

// -- InputChannels ------------------------------------------------------------------------------

InputChannels::InputChannels() {
    for (size_t i = 0; i < 15; ++i) {
        m_padButtons[i] = standard(std::string("pad.") + kPadButtons[i]);
    }
    for (size_t i = 0; i < 6; ++i) {
        m_padAxes[i] = standard(std::string("pad.") + kPadAxes[i]);
    }
    m_wheel[0] = standard("wheel.x");
    m_wheel[1] = standard("wheel.y");
    m_pointerDelta[0] = standard("pointer.dx");
    m_pointerDelta[1] = standard("pointer.dy");
    // PointerButton order: Left, Right, Middle.
    m_pointerButtons[0] = standard("pointer.left");
    m_pointerButtons[1] = standard("pointer.right");
    m_pointerButtons[2] = standard("pointer.middle");
    for (size_t i = 0; i < 4; ++i) {
        m_modifiers[i] = standard(std::string("key.") + kModifierNames[i]);
    }
}

ChannelId InputChannels::standard(std::string_view name) {
    if (auto it = m_byName.find(std::string(name)); it != m_byName.end()) {
        return it->second;
    }
    Channel c;
    c.name = std::string(name);
    ChannelKind kind = ChannelKind::Axis;
    if (fixedChannelKind(name, kind)) {
        c.kind = kind;
        c.fixed = true;
        c.decided = true;
    }
    m_channels.push_back(std::move(c));
    const auto id = static_cast<ChannelId>(m_channels.size());
    m_byName.emplace(std::string(name), id);
    return id;
}

ChannelId InputChannels::intern(std::string_view name) {
    if (auto it = m_byName.find(std::string(name)); it != m_byName.end()) {
        return it->second;
    }
    std::string canonical;
    if (!canonicalName(withoutBlanks(name), canonical, nullptr)) {
        return kInvalidChannel;
    }
    return standard(canonical);
}

ChannelId InputChannels::find(std::string_view name) const {
    if (auto it = m_byName.find(std::string(name)); it != m_byName.end()) {
        return it->second;
    }
    std::string canonical;
    if (!canonicalName(withoutBlanks(name), canonical, nullptr)) {
        return kInvalidChannel;
    }
    auto it = m_byName.find(canonical);
    return it == m_byName.end() ? kInvalidChannel : it->second;
}

std::string_view InputChannels::name(ChannelId id) const {
    return id == kInvalidChannel || id > m_channels.size() ? std::string_view{} : m_channels[id - 1].name;
}

ChannelKind InputChannels::kind(ChannelId id) const {
    return id == kInvalidChannel || id > m_channels.size() ? ChannelKind::Axis : m_channels[id - 1].kind;
}

const InputChannels::PlayerState* InputChannels::state(ChannelId id, uint8_t player) const {
    if (id == kInvalidChannel || id > m_channels.size() || player >= kMaxPlayers) {
        return nullptr;
    }
    return &m_channels[id - 1].players[player];
}

void InputChannels::push(ChannelId id, float value, float strength, uint8_t flags, uint8_t player) {
    if (id == kInvalidChannel || id > m_channels.size() || player >= kMaxPlayers || !std::isfinite(value)) {
        return;
    }
    Channel& c = m_channels[id - 1];
    if (!c.decided) {
        c.kind = (flags & kChannelRelative) != 0 ? ChannelKind::Relative
                 : (flags & kChannelButton) != 0 ? ChannelKind::Button
                                                 : ChannelKind::Axis;
        c.decided = true;
    }
    PlayerState& s = c.players[player];
    if (!s.pendingSet && c.kind == ChannelKind::Relative) {
        s.pendingValue = 0;
    }
    switch (c.kind) {
        case ChannelKind::Relative:
            s.pendingValue += value;
            break;
        case ChannelKind::Axis:
            s.pendingValue = value;
            break;
        case ChannelKind::Button: {
            const bool down = value >= 0.5f;
            if (down && !s.pendingDown) {
                s.pendingPress = true;
            } else if (!down && s.pendingDown) {
                s.pendingRelease = true;
            }
            s.pendingDown = down;
            break;
        }
    }
    s.pendingStrength = std::isfinite(strength) ? (strength < 0 ? 0 : strength > 1 ? 1 : strength) : 1.0f;
    s.pendingSet = true;
    ++m_pushes;
    if (!s.queued) {
        s.queued = true;
        m_next.push_back(Change{id, player});
    }
}

void InputChannels::commit() {
    m_changes.clear();
    m_work.clear();
    m_work.swap(m_next);
    m_pushes = 0;
    for (const Change& entry : m_work) {
        Channel& c = m_channels[entry.id - 1];
        PlayerState& s = c.players[entry.player];
        s.queued = false;
        s.previous = s.value;
        s.pressed = false;
        s.released = false;
        s.changed = false;
        switch (c.kind) {
            case ChannelKind::Relative:
                s.value = s.pendingSet ? s.pendingValue : 0.0f;
                s.changed = s.value != 0.0f;
                s.pendingValue = 0;
                break;
            case ChannelKind::Axis:
                if (s.pendingSet) {
                    s.value = s.pendingValue;
                }
                s.changed = s.value != s.previous;
                break;
            case ChannelKind::Button:
                s.value = s.pendingDown ? 1.0f : 0.0f;
                s.pressed = s.pendingPress;
                s.released = s.pendingRelease;
                s.changed = s.pressed || s.released || s.value != s.previous;
                break;
        }
        if (s.pendingSet) {
            s.strength = s.pendingStrength;
        }
        s.pendingSet = false;
        s.pendingPress = false;
        s.pendingRelease = false;
        s.moved = s.value != s.previous || s.pressed || s.released;
        if (s.moved || s.changed) {
            m_changes.push_back(entry);
            // The next commit brings `previous`, the edges and a relative sum back to rest.
            if (!s.queued) {
                s.queued = true;
                m_next.push_back(entry);
            }
        }
    }
}

float InputChannels::value(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr ? s->value : 0.0f;
}

float InputChannels::strength(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr ? s->strength : 1.0f;
}

float InputChannels::delta(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    if (s == nullptr) {
        return 0.0f;
    }
    return kind(id) == ChannelKind::Relative ? s->value : s->value - s->previous;
}

bool InputChannels::pressed(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr && s->pressed;
}

bool InputChannels::released(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr && s->released;
}

bool InputChannels::changed(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr && s->changed;
}

bool InputChannels::moved(ChannelId id, uint8_t player) const {
    const PlayerState* s = state(id, player);
    return s != nullptr && s->moved;
}

bool InputChannels::held(ChannelId id) const {
    if (id == kInvalidChannel || id > m_channels.size()) {
        return false;
    }
    const Channel& c = m_channels[id - 1];
    if (c.kind == ChannelKind::Relative) {
        return false;
    }
    for (const PlayerState& s : c.players) {
        if (s.value != 0.0f) {
            return true;
        }
    }
    return false;
}

void InputChannels::listen(ChannelId id) {
    if (id != kInvalidChannel && std::find(m_listened.begin(), m_listened.end(), id) == m_listened.end()) {
        m_listened.push_back(id);
    }
}

bool InputChannels::listenedHeld() const {
    return std::any_of(m_listened.begin(), m_listened.end(), [this](ChannelId id) { return held(id); });
}

void InputChannels::seedFrom(const InputChannels& from) {
    for (const Channel& src : from.m_channels) {
        if (src.kind == ChannelKind::Relative || !src.decided) {
            continue;
        }
        bool any = false;
        for (const PlayerState& s : src.players) {
            any = any || s.value != 0.0f;
        }
        if (!any) {
            continue;
        }
        const ChannelId id = intern(src.name);
        if (id == kInvalidChannel) {
            continue;
        }
        Channel& dst = m_channels[id - 1];
        if (!dst.decided) {
            dst.kind = src.kind;
            dst.decided = true;
        }
        // As pushes: the next commit makes them current (the bindings that read them re-evaluate).
        for (size_t p = 0; p < kMaxPlayers; ++p) {
            const PlayerState& s = src.players[p];
            if (s.value == 0.0f) {
                continue;
            }
            PlayerState& d = dst.players[p];
            d.pendingValue = s.value;
            d.pendingDown = true;
            d.pendingStrength = s.strength;
            d.pendingSet = true;
            ++m_pushes;
            if (!d.queued) {
                d.queued = true;
                m_next.push_back(Change{id, static_cast<uint8_t>(p)});
            }
        }
    }
}

void InputChannels::applyEvent(const InputEvent& event, std::vector<Push>* out) {
    const uint8_t player = event.player;
    if (player >= kMaxPlayers) {
        return;
    }
    auto send = [&](ChannelId id, float value) {
        push(id, value, 1.0f, 0, player);
        if (out != nullptr) {
            out->push_back(Push{id, value, 1.0f, 0, player});
        }
    };
    switch (event.kind) {
        case InputEvent::GamepadButtonDown:
        case InputEvent::GamepadButtonUp: {
            const auto b = static_cast<size_t>(event.gamepadButton);
            if (b < 15) {
                send(m_padButtons[b], event.kind == InputEvent::GamepadButtonDown ? 1.0f : 0.0f);
            }
            return;
        }
        case InputEvent::GamepadAxisMotion: {
            const auto a = static_cast<size_t>(event.gamepadAxis);
            if (a < 6) {
                send(m_padAxes[a], event.axisValue);
            }
            return;
        }
        case InputEvent::KeyDown:
        case InputEvent::KeyUp: {
            const std::string_view keyName = keyCodeName(event.key);
            if (keyName.empty()) {
                return;
            }
            const bool down = event.kind == InputEvent::KeyDown;
            send(standard("key." + std::string(keyName)), down ? 1.0f : 0.0f);
            const auto code = static_cast<uint16_t>(event.key);
            const auto first = static_cast<uint16_t>(KeyCode::ShiftLeft);
            if (code >= first && code <= static_cast<uint16_t>(KeyCode::MetaRight)) {
                const auto bit = static_cast<uint8_t>(1u << (code - first));
                const uint8_t before = m_sidedHeld[player];
                m_sidedHeld[player] =
                    down ? static_cast<uint8_t>(before | bit) : static_cast<uint8_t>(before & ~bit);
                const size_t m = (code - first) / 2u;  // Shift, Control, Alt, Meta pairs.
                const auto pair = static_cast<uint8_t>(3u << (2u * m));
                const bool wasHeld = (before & pair) != 0;
                const bool isHeld = (m_sidedHeld[player] & pair) != 0;
                if (wasHeld != isHeld) {
                    send(m_modifiers[m], isHeld ? 1.0f : 0.0f);
                }
            }
            return;
        }
        case InputEvent::Wheel:
            if (event.wheelDelta.x != 0.0f) {
                send(m_wheel[0], event.wheelDelta.x);
            }
            if (event.wheelDelta.y != 0.0f) {
                send(m_wheel[1], event.wheelDelta.y);
            }
            return;
        case InputEvent::PointerMove:
        case InputEvent::PointerDown:
        case InputEvent::PointerUp: {
            if (m_hasPointer[player]) {
                const Vec2 d = event.pos - m_lastPointer[player];
                if (d.x != 0.0f) {
                    send(m_pointerDelta[0], d.x);
                }
                if (d.y != 0.0f) {
                    send(m_pointerDelta[1], d.y);
                }
            }
            m_lastPointer[player] = event.pos;
            m_hasPointer[player] = true;
            const auto b = static_cast<size_t>(event.button);
            if (event.kind != InputEvent::PointerMove && b < 3) {
                send(m_pointerButtons[b], event.kind == InputEvent::PointerDown ? 1.0f : 0.0f);
            }
            return;
        }
        case InputEvent::PointerLeave:
            m_hasPointer[player] = false;
            return;
        default:
            return;
    }
}

}  // namespace xt
