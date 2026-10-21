#include "trigger_emit.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "css_number.h"  // core/src: the CSS number (T-272).
#include "style_emit.h"  // selectorMatchesStatically().
#include "xt/compile/aliases.h"
#include "xt/compile/validate.h"
#include "xt/input.h"  // KeyCode, for `key(<k>)`.
#include "xt/ir_pod.h"
#include "xt/node.h"  // T-390: ElementState bits for `if state(<built-in>)`.

/// docs/design/27-interaction.md §27.1-27.2, 03 §3.12.2-3.12.3, 12 §12.7, docs/tasks/T-174.md.
///
/// Not covered by the design/task sheet (conservative choices made here):
///   - State names and state groups are document-wide: a state belongs to at most one group
///     (declaring it in two groups is an error); a group declared twice must list the same
///     members. Bits are assigned in order of first appearance: `state-groups` attributes
///     (document order), then `:state()` selectors (rule order), then trigger events/actions.
///   - Top-level `,` separates actions like `;` (`.xts` declarations end at `;`, so a stylesheet
///     writes `on-press: sound(ui_click), state(Selected, toggle)`, or quotes the whole list).
///   - `if` must start a statement; its branches are `then`-sequences; `else` binds to the nearest
///     `if`; nested `if` is an error. The compared value is the event's value (a port value from
///     T-175; 0 for every T-174 event).
///   - T-175: `port(<name>)`, `emit(<port>)` and `if` arguments name ports as seen from the
///     trigger's element (bind_emit.h's PortScopes::resolve); `.xts` triggers resolve per matched
///     element too. An `on:port(<name>)` naming no declared port is a warning (it never fires); an
///     undeclared `emit()` port only reaches the host by name, as in T-174. `if` arguments may be
///     numbers, `N%` (relative to the event port's `<port>_max` companion) or ports; `eq`/`ne` also
///     accept a word or a quoted string (an enum value or text).
///   - `play` modes may be combined (`reverse, loop`); `from=` must name a marker of that
///     `@timeline`. `seek(<tl>, <t>)` takes seconds (`s`/`ms` suffix or a bare number) or a marker.
///   - `focus(<id>)` accepts `id` or `#id` and must name an element of the document.
///   - `set` and `spawn` are part of 27.2 but not implemented yet: they are errors ("not supported
///     yet") rather than silently ignored.
///   - T-194 events: `drag.start`, `drag.over`, `drag.leave`, `drop` (= `drag.drop`), `back`,
///     (T-388: `drag.cancel`),
///     `nav.blocked[(<up|down|left|right>)]`, `gesture(tap|doubletap|swipe[(<dir>)]|pinch|hold)` and
///     `player(<n>).<event>` (n = 0..7, 0-based like InputEvent::player; the inner event is one of
///     press, release, click, focus, blur, hover, leave, back, nav.blocked). Actions:
///     `layer(overlay|normal)` and `haptic(<cue>[, <strength>])`.
///   - T-194 attributes: `draggable`, `drop-target`, `autofocus` may be bare or `"true"`/`"false"`;
///     `nav:up|down|left|right="#id"` must name an element id of the document.
///   - T-177: `item-enter` / `item-exit` are events of a `<repeat>` template root (repeat_emit.cpp
///     moves the `<repeat>` attributes there); `remove` is only valid in `item-exit`, and an
///     `item-exit` without it is a warning (the instance is removed as its actions start).
///   - `.xts` trigger declarations only apply through selectors that match without any
///     pseudo-class or `:state()` (like 18.12's static effect lookup); `!important` is ignored.

namespace xt::compile {

namespace {

bool isStateName(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
        return false;
    }
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

std::string trimCopyTriggerEmit(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

// -- Action text lexer ----------------------------------------------------------------------

enum class Tk : uint8_t { Word, LParen, RParen, Comma, Semi, Eq, Str, Pipe, End };

struct Tok {
    Tk kind = Tk::End;
    std::string text;
    size_t pos = 0;  // T-201: byte offset in the action text (set()'s raw value expression).
};

bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/' ||
           c == '#' || c == '+' || c == '%';
}

bool lexActions(std::string_view text, std::vector<Tok>& out, std::string& err) {
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        Tok t;
        t.pos = i;
        if (c == '|') {
            t.kind = Tk::Pipe;  // T-201: only inside set()'s value (`event.value | range(...)`).
            ++i;
        } else if (c == '(' || c == ')' || c == ',' || c == ';' || c == '=') {
            t.kind = c == '('   ? Tk::LParen
                     : c == ')' ? Tk::RParen
                     : c == ',' ? Tk::Comma
                     : c == ';' ? Tk::Semi
                                : Tk::Eq;
            ++i;
        } else if (c == '"' || c == '\'') {
            size_t end = text.find(c, i + 1);
            if (end == std::string_view::npos) {
                err = "unterminated string";
                return false;
            }
            t.kind = Tk::Str;
            t.text = std::string(text.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (isWordChar(c)) {
            size_t start = i;
            while (i < text.size() && isWordChar(text[i])) {
                ++i;
            }
            t.kind = Tk::Word;
            t.text = std::string(text.substr(start, i - start));
        } else {
            err = std::string("unexpected character '") + c + "'";
            return false;
        }
        out.push_back(std::move(t));
    }
    Tok end;
    end.pos = text.size();
    out.push_back(end);
    return true;
}

// -- Keys -------------------------------------------------------------------------------------

bool keyFromName(std::string_view name, uint32_t& out) {
    struct Entry {
        const char* name;
        KeyCode code;
    };
    static constexpr Entry kNamed[] = {
        {"Enter", KeyCode::Enter},
        {"Escape", KeyCode::Escape},
        {"Backspace", KeyCode::Backspace},
        {"Tab", KeyCode::Tab},
        {"Space", KeyCode::Space},
        {"Delete", KeyCode::Delete},
        {"Home", KeyCode::Home},
        {"End", KeyCode::End},
        {"Left", KeyCode::Left},
        {"Right", KeyCode::Right},
        {"Up", KeyCode::Up},
        {"Down", KeyCode::Down},
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
    };
    for (const Entry& e : kNamed) {
        if (name == e.name) {
            out = static_cast<uint32_t>(e.code);
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
        out = static_cast<uint32_t>(KeyCode::KeyA) + static_cast<uint32_t>(s[0] - 'A');
        return true;
    }
    if (s.size() == 1 && s[0] >= '1' && s[0] <= '9') {
        out = static_cast<uint32_t>(KeyCode::Key1) + static_cast<uint32_t>(s[0] - '1');
        return true;
    }
    if (s.size() == 1 && s[0] == '0') {
        out = static_cast<uint32_t>(KeyCode::Key0);
        return true;
    }
    return false;
}

/// Parses `<n>`, `<n>s` or `<n>ms` into seconds.
bool parseSecondsTriggerEmit(std::string_view text, float& out) {
    float v = 0;
    size_t length = 0;
    if (!css_number::read(text, v, length)) {  // T-272: CSS number, exponent included.
        return false;
    }
    std::string_view unit = text.substr(length);
    if (unit.empty() || unit == "s") {
        out = v;
    } else if (unit == "ms") {
        out = v / 1000.0f;
    } else {
        return false;
    }
    return true;
}

bool parseNumberTriggerEmit(std::string_view text, float& out) {
    float v = 0;
    size_t length = 0;
    if (!css_number::read(text, v, length) || length != text.size()) {
        return false;
    }
    out = v;
    return true;
}

// -- Events -------------------------------------------------------------------------------------

struct ParsedEvent {
    TriggerEvent event = TriggerEvent::None;
    std::string arg0;  // key / port / timeline / state name / media id (T-191).
    std::string arg1;  // marker name.
    uint32_t key = 0;
    std::string canonical;
    uint8_t button = 0;                // T-390 (08.13): IrTrigger::button (PointerButton + 1, 0 = any).
    const NameAlias* alias = nullptr;  // T-408: the registered alias the event was written with.
};

/// `<name>(<arg>)<rest>` -> arg and rest; false when `text` does not start with `<name>(`.
bool splitCall(std::string_view text, std::string_view name, std::string& arg, std::string_view& rest) {
    if (text.size() < name.size() + 2 || text.substr(0, name.size()) != name || text[name.size()] != '(') {
        return false;
    }
    size_t close = text.find(')', name.size() + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    arg = trimCopyTriggerEmit(text.substr(name.size() + 1, close - name.size() - 1));
    rest = text.substr(close + 1);
    return true;
}

/// T-386 (08.11): a set() value `pointer.fraction(#id).x|y` / `pointer.local(#id).x|y`, optionally
/// followed by `| <converter> ...`. 0 = not a pointer value, 1 = parsed, -1 = malformed (`err`).
int parsePointerValue(std::string_view text, SetSource& source, std::string& id, std::string& converters,
                      std::string& err) {
    if (text.rfind("pointer.", 0) != 0) {
        return 0;
    }
    std::string head;
    std::string_view tail = text;
    const size_t bar = text.find('|');
    if (bar != std::string_view::npos) {
        tail = text.substr(0, bar);
        converters = trimCopyTriggerEmit(text.substr(bar + 1));
        if (converters.empty()) {
            err = "set() value \"" + std::string(text) + "\": expected a converter after '|'";
            return -1;
        }
    }
    for (char c : tail) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            head.push_back(c);
        }
    }
    std::string arg;
    std::string_view rest;
    const bool fraction = splitCall(head, "pointer.fraction", arg, rest);
    if ((!fraction && !splitCall(head, "pointer.local", arg, rest)) || arg.size() < 2 || arg[0] != '#' ||
        (rest != ".x" && rest != ".y")) {
        err = "set() value \"" + std::string(text) +
              "\": expected pointer.fraction(#<id>).x|.y or pointer.local(#<id>).x|.y";
        return -1;
    }
    id = arg.substr(1);
    const bool y = rest == ".y";
    source = fraction ? (y ? SetSource::PointerFractionY : SetSource::PointerFractionX)
                      : (y ? SetSource::PointerLocalY : SetSource::PointerLocalX);
    return 1;
}

/// T-411 (27 §27.2.3): whether a set() value is a bind-source expression, compiled per element by
/// compileActionValue(): it starts with `port(`, `item(`, `repeat.` or `outer.` (rejected there),
/// or it is a constant (number, `true` / `false`, quoted string) followed by `| <converter>`.
bool isSourceValue(std::string_view text) {
    if (text.rfind("port(", 0) == 0 || text.rfind("item(", 0) == 0 || text.rfind("repeat.", 0) == 0 ||
        text.rfind("outer.", 0) == 0) {
        return true;
    }
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            quote = c == quote ? 0 : quote;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == '|' && depth == 0) {
            const std::string head = trimCopyTriggerEmit(text.substr(0, i));
            float number = 0;
            const bool quoted =
                head.size() >= 2 && (head[0] == '\'' || head[0] == '"') && head.back() == head[0];
            return quoted || head == "true" || head == "false" || parseNumberTriggerEmit(head, number);
        }
    }
    return false;
}

/// T-386 (08.11): the events whose triggers may read `pointer.fraction()` / `pointer.local()`
/// (the Pointer* events and the clicks; there is no wheel trigger).
/// T-392 (27.3.5): `value` or `value | <converters>` -- the trigger element's text.
bool isElementValue(std::string_view text) {
    if (text.rfind("value", 0) != 0) {
        return false;
    }
    const std::string rest = trimCopyTriggerEmit(text.substr(5));
    return rest.empty() || rest[0] == '|';
}

bool isPointerTriggerEvent(TriggerEvent ev) {
    switch (ev) {
        case TriggerEvent::Hover:
        case TriggerEvent::Leave:
        case TriggerEvent::Press:
        case TriggerEvent::Release:
        case TriggerEvent::Click:
        case TriggerEvent::DoubleClick:
        case TriggerEvent::PointerMove:
        case TriggerEvent::PointerCancel:
        case TriggerEvent::PointerDrag:  // T-394 (08.15).
            return true;
        default:
            return false;
    }
}

/// T-194: `up|down|left|right` -> NavDirection.
bool navDirectionFromName(std::string_view name, uint32_t& out) {
    static constexpr std::pair<const char*, NavDirection> kDirs[] = {{"up", NavDirection::Up},
                                                                     {"down", NavDirection::Down},
                                                                     {"left", NavDirection::Left},
                                                                     {"right", NavDirection::Right}};
    for (const auto& d : kDirs) {
        if (name == d.first) {
            out = static_cast<uint32_t>(d.second);
            return true;
        }
    }
    return false;
}

/// T-390 (08.13): `left|middle|right` -> PointerButton.
bool pointerButtonFromName(std::string_view name, PointerButton& out) {
    if (name == "left") {
        out = PointerButton::Left;
    } else if (name == "middle") {
        out = PointerButton::Middle;
    } else if (name == "right") {
        out = PointerButton::Right;
    } else {
        return false;
    }
    return true;
}

bool parseEvent(std::string_view raw, ParsedEvent& out, std::string& err) {
    std::string text;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            text.push_back(c);
        }
    }
    // T-390 (08.13): `press(<button>)`, `release(...)`, `click(...)`, `dblclick(...)`,
    // `pointermove(...)` (and the pointerdown / pointerup spellings) filter on the button.
    for (const char* base :
         {"press", "release", "click", "dblclick", "pointermove", "pointerdown", "pointerup"}) {
        std::string arg;
        std::string_view rest;
        if (!splitCall(text, base, arg, rest)) {
            continue;
        }
        PointerButton button = PointerButton::Left;
        if (!rest.empty() || !pointerButtonFromName(arg, button)) {
            err = std::string("expected ") + base + "(left|middle|right)";
            return false;
        }
        if (!parseEvent(base, out, err)) {
            return false;
        }
        out.button = static_cast<uint8_t>(static_cast<uint8_t>(button) + 1u);
        out.canonical += "(" + arg + ")";
        return true;
    }
    struct Simple {
        const char* name;
        TriggerEvent event;
    };
    static constexpr Simple kSimple[] = {
        {"hover", TriggerEvent::Hover},
        {"leave", TriggerEvent::Leave},
        {"press", TriggerEvent::Press},
        {"release", TriggerEvent::Release},
        {"click", TriggerEvent::Click},
        {"dblclick", TriggerEvent::DoubleClick},  // T-379 (08.10).
        // T-386 (08.11): `pointerdown` / `pointerup` are `press` / `release` (same canonical name).
        {"pointerdown", TriggerEvent::Press},
        {"pointerup", TriggerEvent::Release},
        {"pointermove", TriggerEvent::PointerMove},
        {"pointercancel", TriggerEvent::PointerCancel},
        {"pointerdrag", TriggerEvent::PointerDrag},    // T-394 (08.15).
        {"scroll", TriggerEvent::Scroll},              // T-387 (08.12).
        {"outsidepress", TriggerEvent::OutsidePress},  // T-387 (08.12): light-dismiss.
        {"focus", TriggerEvent::Focus},
        {"blur", TriggerEvent::Blur},
        {"focusin", TriggerEvent::FocusIn},    // T-397 (08.17): bubbling focus.
        {"focusout", TriggerEvent::FocusOut},  // T-397 (08.17).
        {"check", TriggerEvent::Check},
        {"uncheck", TriggerEvent::Uncheck},
        {"show", TriggerEvent::Show},
        {"hide", TriggerEvent::Hide},
        {"item-enter", TriggerEvent::ItemEnter},  // T-177: `<repeat item-enter>` (template root).
        {"item-exit", TriggerEvent::ItemExit},    // T-177: `<repeat item-exit>`.
        {"change", TriggerEvent::Change},         // T-392 (27.3.5).
        {"input", TriggerEvent::Input},           // T-392 (27.3.5).
    };
    for (const Simple& s : kSimple) {
        if (text == s.name) {
            out.event = s.event;
            out.alias = findNameAlias(AliasKind::Trigger, text);  // T-408: pointerdown / pointerup.
            out.canonical = out.alias != nullptr ? out.alias->replacement : text;
            return true;
        }
    }
    std::string arg;
    std::string_view rest;
    if (splitCall(text, "key", arg, rest) && rest.empty()) {
        if (!keyFromName(arg, out.key)) {
            err = "unknown key \"" + arg + "\" in key()";
            return false;
        }
        out.event = TriggerEvent::Key;
        out.arg0 = arg;
        out.canonical = "key(" + std::to_string(out.key) + ")";
        return true;
    }
    if (splitCall(text, "port", arg, rest) && rest.empty() && !arg.empty()) {
        out.event = TriggerEvent::Port;
        out.arg0 = arg;
        out.canonical = "port(" + arg + ")";
        return true;
    }
    if (splitCall(text, "timeline", arg, rest) && !arg.empty()) {
        out.arg0 = arg;
        if (rest == ".end") {
            out.event = TriggerEvent::TimelineEnd;
            out.canonical = "timeline(" + arg + ").end";
            return true;
        }
        std::string marker;
        std::string_view tail;
        if (rest.size() > 1 && rest[0] == '.' && splitCall(rest.substr(1), "marker", marker, tail) &&
            tail.empty() && !marker.empty()) {
            out.event = TriggerEvent::TimelineMarker;
            out.arg1 = marker;
            out.canonical = "timeline(" + arg + ").marker(" + marker + ")";
            return true;
        }
        err = "expected timeline(<name>).end or timeline(<name>).marker(<m>)";
        return false;
    }
    if (splitCall(text, "media", arg, rest) && !arg.empty()) {
        // T-191 (13.8.3): `media(<id>).end` -- a `<video>`/`<sequence>` played to its end.
        if (rest != ".end") {
            err = "expected media(<id>).end";
            return false;
        }
        out.event = TriggerEvent::MediaEnd;
        out.arg0 = arg;
        out.canonical = "media(" + arg + ").end";
        return true;
    }
    if (splitCall(text, "state", arg, rest)) {
        if (!isStateName(arg) || (rest != ".enter" && rest != ".exit")) {
            err = "expected state(<Name>).enter or state(<Name>).exit";
            return false;
        }
        out.event = rest == ".enter" ? TriggerEvent::StateEnter : TriggerEvent::StateExit;
        out.arg0 = arg;
        out.canonical = "state(" + arg + ")" + std::string(rest);
        return true;
    }
    // T-392 (27.3.5): `state-enter(<Name>)` / `state-exit(<Name>)` = `state(<Name>).enter|.exit`.
    for (const bool enter : {true, false}) {
        if (splitCall(text, enter ? "state-enter" : "state-exit", arg, rest)) {
            if (!isStateName(arg) || !rest.empty()) {
                err = std::string("expected ") + (enter ? "state-enter" : "state-exit") + "(<Name>)";
                return false;
            }
            out.event = enter ? TriggerEvent::StateEnter : TriggerEvent::StateExit;
            out.arg0 = arg;
            out.canonical = "state(" + arg + ")" + (enter ? ".enter" : ".exit");
            out.alias =
                findNameAlias(AliasKind::Trigger, enter ? "state-enter(<Name>)" : "state-exit(<Name>)");
            return true;
        }
    }
    // T-194 (27.2.2, 27.7).
    if (text == "drag.start" || text == "drag.over" || text == "drag.leave" || text == "drop" ||
        text == "drag.drop" || text == "back" || text == "drag.cancel") {
        out.event = text == "drag.start"    ? TriggerEvent::DragStart
                    : text == "drag.over"   ? TriggerEvent::DragOver
                    : text == "drag.leave"  ? TriggerEvent::DragLeave
                    : text == "drag.cancel" ? TriggerEvent::DragCancel  // T-388 (08.12).
                    : text == "back"        ? TriggerEvent::Back
                                            : TriggerEvent::Drop;
        out.canonical = text == "drag.drop" ? "drop" : text;
        return true;
    }
    if (text == "drag.value" || text == "nav.value") {  // T-201 (27.7 `draggable="axis-x"`).
        out.event = text == "drag.value" ? TriggerEvent::DragValue : TriggerEvent::NavValue;
        out.canonical = text;
        return true;
    }
    if (text == "nav.blocked" || text.rfind("nav.blocked(", 0) == 0) {
        out.event = TriggerEvent::NavBlocked;
        out.key = kInvalidString;
        out.canonical = "nav.blocked";
        if (text != "nav.blocked") {
            if (!splitCall(text.substr(4), "blocked", arg, rest) || !rest.empty() ||
                !navDirectionFromName(arg, out.key)) {
                err = "expected nav.blocked or nav.blocked(up|down|left|right)";
                return false;
            }
            out.canonical += "(" + arg + ")";
        }
        return true;
    }
    if (text.rfind("gesture(", 0) == 0 && text.back() == ')') {
        const std::string inner = text.substr(8, text.size() - 9);
        static constexpr std::pair<const char*, GestureKind> kGestures[] = {
            {"tap", GestureKind::Tap},
            {"doubletap", GestureKind::DoubleTap},
            {"swipe", GestureKind::Swipe},
            {"pinch", GestureKind::Pinch},
            {"hold", GestureKind::Hold}};
        out.event = TriggerEvent::Gesture;
        out.arg1 = "";
        for (const auto& g : kGestures) {
            if (inner == g.first) {
                out.key = static_cast<uint32_t>(g.second);
                out.canonical = "gesture(" + inner + ")";
                return true;
            }
        }
        std::string dir;
        std::string_view tail;
        uint32_t dirCode = 0;
        if (splitCall(inner, "swipe", dir, tail) && tail.empty() && navDirectionFromName(dir, dirCode)) {
            out.key = static_cast<uint32_t>(GestureKind::Swipe);
            out.arg1 = std::to_string(dirCode);
            out.canonical = "gesture(swipe(" + dir + "))";
            return true;
        }
        err = "expected gesture(tap|doubletap|swipe|swipe(<dir>)|pinch|hold)";
        return false;
    }
    if (splitCall(text, "player", arg, rest)) {
        char* end = nullptr;
        const long n = std::strtol(arg.c_str(), &end, 10);
        if (arg.empty() || end == arg.c_str() || *end != '\0' || n < 0 || n >= kMaxPlayers ||
            rest.size() < 2 || rest[0] != '.') {
            err = "expected player(<0..7>).<event>";
            return false;
        }
        ParsedEvent inner;
        if (!parseEvent(rest.substr(1), inner, err)) {
            return false;
        }
        switch (inner.event) {
            case TriggerEvent::Press:
            case TriggerEvent::Release:
            case TriggerEvent::Click:
            case TriggerEvent::Focus:
            case TriggerEvent::Blur:
            case TriggerEvent::Hover:
            case TriggerEvent::Leave:
            case TriggerEvent::Back:
                break;
            case TriggerEvent::NavBlocked:
                if (inner.key != kInvalidString) {
                    err = "player(<n>).nav.blocked takes no direction";
                    return false;
                }
                break;
            default:
                err = "player(<n>) does not support the event \"" + std::string(rest.substr(1)) + "\"";
                return false;
        }
        out.event = TriggerEvent::Player;
        out.key = static_cast<uint32_t>(n);
        out.arg1 = std::to_string(static_cast<unsigned>(inner.event));
        out.canonical = "player(" + std::to_string(n) + ")." + inner.canonical;
        out.alias = inner.alias;  // T-408
        return true;
    }
    err = "unknown event \"" + text + "\"";
    return false;
}

// -- Actions ------------------------------------------------------------------------------------

struct Arg {
    std::string key;  // `from` of `from=<v>`, else empty.
    std::string value;
    bool isString = false;
};

struct CallTriggerEmit {
    std::string name;
    std::vector<Arg> args;
    bool hasParens = false;
};

/// T-390 (08.13): one term of an `if` condition list (`[not] <term> and ...`).
struct CondTerm {
    IfTerm kind = IfTerm::Compare;
    bool negate = false;
    std::string operand;  // Button: left|middle|right; State: the name; Port: the port name.
    CallTriggerEmit cmp;  // Compare / Port: the comparison (eq(1), gt(0.5), ...).
};

struct Stmt {
    bool isIf = false;
    bool ifActive = false;             // T-386 (08.11): `if active` (no comparison).
    std::vector<CondTerm> terms;       // T-390: non-empty = a kIfTerms list.
    CallTriggerEmit condCall;          // The `if` comparison, compiled per element (T-175).
    std::vector<CallTriggerEmit> seq;  // Plain statement, or the `if` true branch.
    std::vector<CallTriggerEmit> alt;  // `else` branch.
};

/// A trigger compiled independently of the element it ends up on.
struct CompiledTrigger {
    IrTrigger trigger{};
    std::vector<IrTriggerAction> actions;
    std::vector<IrConverterStep> conditions;  // Offsets in If actions are relative to this vector.
    std::string canonical;
    bool ok = false;
    // T-175: names resolved per element (PortScopes::resolve), and where the trigger was written.
    std::string eventPort;                                  // `port(<name>)`.
    std::vector<std::pair<size_t, std::string>> emitPorts;  // (action index, port name).
    struct SetPort {
        size_t slot = 0;
        std::string port;
        std::string converters;  // After `event.value |` (T-386: or a pointer value's `|`); empty = none.
        std::string source;      // T-411: a bind-source value (isSourceValue()), compiled per element.
    };
    // T-386 (08.11): (action index, `#id` text) of a `pointer.fraction()` / `pointer.local()` value.
    std::vector<std::pair<size_t, std::string>> pointerRefs;
    bool usesElementValue = false;                             // T-392 (27.3.5): a set(..., value) action.
    std::vector<SetPort> setPorts;                             // T-201: set() targets, resolved per element.
    std::vector<std::pair<size_t, std::string>> stateTargets;  // T-201: (action index, `#id`).
    struct SpawnRef {
        size_t slot = 0;
        std::string symbol;
        std::string at;  // `at=<id>` (SpawnAt::Element), else empty.
    };
    std::vector<SpawnRef> spawns;  // T-201: spawn() pools and `at` ids, resolved per element.
    // Index-aligned with `conditions`. T-390: an empty name = a kIfTerms header compiled already;
    // `#port` = a Port term header whose port (args[0]) is resolved per element.
    std::vector<CallTriggerEmit> condCalls;
    std::string file;
    int line = 0;
    int col = 0;
};

/// The comparison converters an `if` accepts: op and arity.
bool comparisonOp(std::string_view name, size_t& argc, ConverterOp* op = nullptr) {
    struct Op {
        const char* name;
        ConverterOp op;
        size_t argc;
    };
    static constexpr Op kOps[] = {
        {"lt", ConverterOp::Lt, 1},           {"le", ConverterOp::Le, 1}, {"gt", ConverterOp::Gt, 1},
        {"ge", ConverterOp::Ge, 1},           {"eq", ConverterOp::Eq, 1}, {"ne", ConverterOp::Ne, 1},
        {"between", ConverterOp::Between, 2},
    };
    for (const Op& o : kOps) {
        if (name == o.name) {
            argc = o.argc;
            if (op != nullptr) {
                *op = o.op;
            }
            return true;
        }
    }
    return false;
}

class Emitter {
public:
    Emitter(const DomTree& dom, const RuleSet& rules, const PortScopes& ports, IrWriter& writer,
            std::string_view fileName, DiagnosticSink& sink, const RepeatScopes* repeats = nullptr)
        : m_dom(dom),
          m_rules(rules),
          m_ports(ports),
          m_repeats(repeats),
          m_writer(writer),
          m_fileName(fileName),
          m_sink(sink) {
        for (const TimelineDef& def : rules.timelines) {
            m_timelines.emplace(def.name, &def);
        }
        for (const DomNode& n : dom.nodes) {
            if (const DomAttr* id = n.attr("id")) {
                m_ids.insert(id->value);
                if (n.tag == "video" || n.tag == "sequence") {
                    m_mediaIds.insert(id->value);  // T-191: play/stop/seek/media() targets.
                }
                if (n.tag == "particles") {
                    m_particleIds.insert(id->value);  // T-202: play/stop switch emission.
                }
            }
        }
    }

    bool run(CustomStateBuilder& states, TriggerBuilder& out) {
        checkInteractionAttributes();
        collectGroups();
        collectSelectorStates();
        collectBindStates();
        compileRuleTriggers();
        emitElements(out);
        for (const std::string& name : m_stateNames) {
            states.names.push_back(m_writer.intern(name));
        }
        for (const std::string& group : m_groupOrder) {
            uint32_t mask = 0;
            for (const std::string& member : m_groups[group]) {
                auto it = m_stateIndex.find(member);
                if (it != m_stateIndex.end()) {
                    mask |= 1u << it->second;
                }
            }
            states.groups.push_back(IrStateGroup{m_writer.intern(group), mask});
        }
        return m_ok;
    }

private:
    const DomTree& m_dom;
    const RuleSet& m_rules;
    const PortScopes& m_ports;
    const RepeatScopes* m_repeats = nullptr;  // T-411: `item()` / `repeat.*` set() values.
    IrWriter& m_writer;
    std::string m_fileName;
    DiagnosticSink& m_sink;
    bool m_ok = true;
    bool m_reportedTooMany = false;
    std::vector<std::string> m_stateNames;
    std::map<std::string, uint32_t> m_stateIndex;
    std::vector<std::string> m_groupOrder;
    std::map<std::string, std::vector<std::string>> m_groups;
    std::map<std::string, std::string> m_groupOfState;
    std::map<std::string, const TimelineDef*> m_timelines;
    std::set<std::string> m_ids;
    std::set<std::string> m_mediaIds;
    std::set<std::string> m_particleIds;
    // Per rule, per trigger declaration: the compiled trigger (index-aligned with StyleRule::triggers).
    std::vector<std::vector<CompiledTrigger>> m_ruleTriggers;

    void error(const std::string& file, const std::string& msg, int line, int col) {
        m_sink.error(file, msg, line, col);
        m_ok = false;
    }

    /// T-194 (05.9.1): `draggable`, `drop-target`, `autofocus` (bare or true/false) and
    /// `nav:<dir>="#id"` (the id must exist).
    void checkInteractionAttributes() {
        for (const DomNode& node : m_dom.nodes) {
            for (const DomAttr& attr : node.attrs) {
                if (attr.name == "draggable" || attr.name == "drop-target" || attr.name == "autofocus" ||
                    attr.name == "focus-trap") {  // T-201: `focus-trap` (08.9 kFocusTrap).
                    const std::string v = trimCopyTriggerEmit(attr.value);
                    if (attr.name == "draggable" && (v == "axis-x" || v == "any-button")) {
                        continue;  // T-201 (27.7): a slider track; T-394 (08.15): any button drags.
                    }
                    if (!v.empty() && v != "true" && v != "false") {
                        error(m_fileName, attr.name + " must be true or false", attr.line, attr.col);
                    }
                    continue;
                }
                // T-388 (08.12): `drag-autoscroll` (on a scroll container) and `drag-threshold`
                // (logical px, on a draggable) stay plain attributes the dispatcher reads.
                if (attr.name == "drag-autoscroll") {
                    const std::string v = trimCopyTriggerEmit(attr.value);
                    if (v != "true" && v != "false") {
                        error(m_fileName, "drag-autoscroll must be true or false", attr.line, attr.col);
                    }
                    continue;
                }
                if (attr.name == "drag-ghost") {
                    // T-394 (08.15): `none` draws no ghost (the only value; leave it out for one).
                    if (trimCopyTriggerEmit(attr.value) != "none") {
                        error(m_fileName, "drag-ghost must be none", attr.line, attr.col);
                    }
                    continue;
                }
                if (attr.name == "drag-threshold") {
                    const std::string v = trimCopyTriggerEmit(attr.value);
                    char* end = nullptr;
                    const float px = std::strtof(v.c_str(), &end);
                    if (v.empty() || end != v.c_str() + v.size() || !(px >= 0.0f)) {
                        error(m_fileName, "drag-threshold must be a non-negative number of pixels", attr.line,
                              attr.col);
                    }
                    continue;
                }
                if (attr.name.rfind("nav:", 0) != 0) {
                    continue;
                }
                uint32_t dir = 0;
                if (!navDirectionFromName(std::string_view(attr.name).substr(4), dir)) {
                    error(m_fileName,
                          "unknown navigation direction \"" + attr.name +
                              "\" (expected nav:up, "
                              "nav:down, nav:left or nav:right)",
                          attr.line, attr.col);
                    continue;
                }
                std::string id = trimCopyTriggerEmit(attr.value);
                if (!id.empty() && id[0] == '#') {
                    id.erase(0, 1);
                }
                if (m_ids.count(id) == 0) {
                    error(m_fileName, attr.name + " target \"" + id + "\" is not an element id", attr.line,
                          attr.col);
                }
            }
        }
    }

    /// The bit of state `name`, registering it on first use; -1 (after an error) past 32 names.
    int stateBit(const std::string& name, const std::string& file, int line, int col) {
        auto it = m_stateIndex.find(name);
        if (it != m_stateIndex.end()) {
            return static_cast<int>(it->second);
        }
        if (m_stateNames.size() >= kMaxCustomStates) {
            if (!m_reportedTooMany) {
                error(file,
                      "more than " + std::to_string(kMaxCustomStates) + " custom state names (\"" + name +
                          "\" is one too many)",
                      line, col);
                m_reportedTooMany = true;
            }
            return -1;
        }
        uint32_t bit = static_cast<uint32_t>(m_stateNames.size());
        m_stateNames.push_back(name);
        m_stateIndex.emplace(name, bit);
        return static_cast<int>(bit);
    }

    // `state-groups="A: x|y; B: p|q"` on any element (including instance roots, which carry their
    // symbol's declaration).
    void collectGroups() {
        for (const DomNode& n : m_dom.nodes) {
            const DomAttr* attr = n.attr("state-groups");
            if (attr == nullptr) {
                continue;
            }
            std::string_view text(attr->value);
            size_t start = 0;
            while (start <= text.size()) {
                size_t semi = text.find(';', start);
                std::string section = trimCopyTriggerEmit(text.substr(
                    start, semi == std::string_view::npos ? std::string_view::npos : semi - start));
                start = semi == std::string_view::npos ? text.size() + 1 : semi + 1;
                if (section.empty()) {
                    continue;
                }
                size_t colon = section.find(':');
                std::string group = colon == std::string::npos
                                        ? std::string()
                                        : trimCopyTriggerEmit(section.substr(0, colon));
                if (!isStateName(group)) {
                    error(m_fileName,
                          "invalid state-groups section \"" + section + "\" (expected <Group>: a|b|c)",
                          attr->line, attr->col);
                    continue;
                }
                std::vector<std::string> members;
                std::string_view list = std::string_view(section).substr(colon + 1);
                size_t mstart = 0;
                bool bad = false;
                while (mstart <= list.size()) {
                    size_t bar = list.find('|', mstart);
                    std::string member = trimCopyTriggerEmit(list.substr(
                        mstart, bar == std::string_view::npos ? std::string_view::npos : bar - mstart));
                    mstart = bar == std::string_view::npos ? list.size() + 1 : bar + 1;
                    if (!isStateName(member)) {
                        error(m_fileName, "invalid state name \"" + member + "\" in state-groups", attr->line,
                              attr->col);
                        bad = true;
                        continue;
                    }
                    if (builtinStateAlias(member) != 0) {
                        // T-390 / T-392 (27.3.2, 27.3.5): a built-in state, no custom state slot.
                        continue;
                    }
                    if (std::find(members.begin(), members.end(), member) == members.end()) {
                        members.push_back(member);
                    }
                }
                if (bad || members.empty()) {
                    continue;
                }
                auto existing = m_groups.find(group);
                if (existing != m_groups.end()) {
                    std::vector<std::string> a = existing->second;
                    std::vector<std::string> b = members;
                    std::sort(a.begin(), a.end());
                    std::sort(b.begin(), b.end());
                    if (a != b) {
                        error(m_fileName, "state group \"" + group + "\" is declared with different members",
                              attr->line, attr->col);
                    }
                    continue;
                }
                bool conflict = false;
                for (const std::string& member : members) {
                    auto owner = m_groupOfState.find(member);
                    if (owner != m_groupOfState.end()) {
                        error(m_fileName,
                              "state \"" + member + "\" is in two state groups (\"" + owner->second +
                                  "\" and \"" + group + "\")",
                              attr->line, attr->col);
                        conflict = true;
                    }
                }
                if (conflict) {
                    continue;
                }
                for (const std::string& member : members) {
                    m_groupOfState.emplace(member, group);
                    stateBit(member, m_fileName, attr->line, attr->col);
                }
                m_groupOrder.push_back(group);
                m_groups.emplace(group, std::move(members));
            }
        }
    }

    // Registers the name of every `:state(<Name>)` selector condition (negated ones included).
    void collectSelectorStates() {
        for (const StyleRule& rule : m_rules.rules) {
            for (const Selector& sel : rule.selectors) {
                for (const SelectorPart& part : sel.parts) {
                    for (const SelectorCond& cond : part.conds) {
                        if (cond.kind == SelectorCondKind::CustomState) {
                            stateBit(cond.name, rule.file.empty() ? m_fileName : rule.file, rule.line, 0);
                        }
                    }
                }
            }
        }
    }

    // T-175: `bind:state.<Name>` attributes (27.3.2) name custom states too.
    void collectBindStates() {
        for (const DomNode& n : m_dom.nodes) {
            for (const DomAttr& attr : n.attrs) {
                if (attr.name.rfind("bind:state.", 0) != 0) {
                    continue;
                }
                const std::string name = attr.name.substr(11);
                // T-392 (27.3.5): a built-in name (bind:state.Focused ...) makes no custom state.
                if (isStateName(name) && builtinStateAlias(name) == 0) {
                    stateBit(name, m_fileName, attr.line, attr.col);
                }
            }
        }
    }

    /// T-175: compiles an `if` comparison for `element`: numbers, `N%` (relative to the event
    /// port's `_max` companion), ports, and for eq/ne words or strings. False with `err`.
    bool condition(const CallTriggerEmit& c, uint32_t element, int eventPort, IrConverterStep& out,
                   std::string& err) {
        size_t argc = 0;
        ConverterOp op{};
        comparisonOp(c.name, argc, &op);
        out = IrConverterStep{};
        out.op = static_cast<uint8_t>(op);
        out.argCount = static_cast<uint8_t>(c.args.size());
        const bool words = op == ConverterOp::Eq || op == ConverterOp::Ne;
        for (size_t i = 0; i < c.args.size() && i < 4; ++i) {
            const Arg& a = c.args[i];
            ConverterArg kind = ConverterArg::Number;
            uint32_t bits = 0;
            float number = 0;
            if (a.isString) {
                if (!words) {
                    err = c.name + "() compares numbers, got \"" + a.value + "\"";
                    return false;
                }
                kind = ConverterArg::String;
                bits = m_writer.intern(a.value);
            } else if (parseNumberTriggerEmit(a.value, number)) {
                kind = ConverterArg::Number;
            } else if (a.value.size() > 1 && a.value.back() == '%' &&
                       parseNumberTriggerEmit(std::string_view(a.value).substr(0, a.value.size() - 1),
                                              number)) {
                if (eventPort < 0 || m_ports.maxCompanion(static_cast<uint32_t>(eventPort)) < 0) {
                    err = c.name + "(" + a.value +
                          "): a percentage needs a port(<name>) event whose port has a \"<name>_max\" "
                          "companion";
                    return false;
                }
                kind = ConverterArg::Percent;
            } else if (int port = m_ports.resolve(a.value, element); port >= 0) {
                kind = ConverterArg::Port;
                bits = static_cast<uint32_t>(port);
            } else if (words) {
                kind = ConverterArg::String;
                bits = m_writer.intern(a.value);
            } else {
                err = c.name + "() compares against numbers, percentages or ports, got \"" + a.value + "\"";
                return false;
            }
            out.argKinds = static_cast<uint16_t>(out.argKinds | (static_cast<uint16_t>(kind) << (4u * i)));
            if (kind == ConverterArg::String || kind == ConverterArg::Port) {
                std::memcpy(&out.args[i], &bits, sizeof(float));
            } else {
                out.args[i] = number;
            }
        }
        return true;
    }

    // -- Action parsing --

    struct ActionParser {
        std::vector<Tok> toks;
        size_t pos = 0;
        std::string err;
        std::string_view text;  // T-201: the lexed text (set()'s raw value).

        /// T-201: `set(<port>, <value>)`'s value up to the closing ')', kept as raw text (it may
        /// be `event.value | <converter>(...) ...`).
        bool parseSetValue(CallTriggerEmit& out) {
            const size_t first = pos;
            int depth = 0;
            while (cur().kind != Tk::End &&
                   !(depth == 0 && (cur().kind == Tk::RParen || cur().kind == Tk::Comma))) {
                depth += cur().kind == Tk::LParen ? 1 : (cur().kind == Tk::RParen ? -1 : 0);
                ++pos;
            }
            if (cur().kind != Tk::RParen || pos == first) {
                err = "set() takes a port and a value";
                return false;
            }
            Arg arg;
            if (pos == first + 1 && (toks[first].kind == Tk::Word || toks[first].kind == Tk::Str)) {
                arg.value = toks[first].text;
                arg.isString = toks[first].kind == Tk::Str;
            } else {
                arg.value =
                    trimCopyTriggerEmit(text.substr(toks[first].pos, toks[pos].pos - toks[first].pos));
            }
            out.args.push_back(std::move(arg));
            ++pos;  // ')'.
            return true;
        }

        const Tok& cur() const { return toks[pos]; }
        bool isWord(std::string_view w) const { return cur().kind == Tk::Word && cur().text == w; }

        bool parseCall(CallTriggerEmit& out) {
            if (cur().kind != Tk::Word) {
                err = "expected an action";
                return false;
            }
            out.name = cur().text;
            ++pos;
            if (cur().kind != Tk::LParen) {
                return true;  // Bare `show` / `hide` (checked by the compiler).
            }
            out.hasParens = true;
            ++pos;
            if (cur().kind == Tk::RParen) {
                ++pos;
                return true;
            }
            for (;;) {
                Arg arg;
                if (cur().kind != Tk::Word && cur().kind != Tk::Str) {
                    err = "expected an argument in " + out.name + "()";
                    return false;
                }
                arg.value = cur().text;
                arg.isString = cur().kind == Tk::Str;
                ++pos;
                if (cur().kind == Tk::Eq && !arg.isString) {
                    ++pos;
                    if (cur().kind != Tk::Word && cur().kind != Tk::Str) {
                        err = "expected a value after '" + arg.value + "=' in " + out.name + "()";
                        return false;
                    }
                    arg.key = arg.value;
                    arg.value = cur().text;
                    arg.isString = cur().kind == Tk::Str;
                    ++pos;
                }
                out.args.push_back(std::move(arg));
                if (cur().kind == Tk::Comma && out.name == "set" && out.args.size() == 1) {
                    ++pos;
                    return parseSetValue(out);
                }
                if (cur().kind == Tk::Comma) {
                    ++pos;
                    continue;
                }
                if (cur().kind == Tk::RParen) {
                    ++pos;
                    return true;
                }
                err = "expected ',' or ')' in " + out.name + "()";
                return false;
            }
        }

        bool parseSeq(std::vector<CallTriggerEmit>& out) {
            for (;;) {
                if (isWord("if")) {
                    err = "'if' must start a statement";
                    return false;
                }
                CallTriggerEmit call;
                if (!parseCall(call)) {
                    return false;
                }
                out.push_back(std::move(call));
                if (isWord("then")) {
                    ++pos;
                    continue;
                }
                return true;
            }
        }

        bool parseList(std::vector<Stmt>& out) {
            for (;;) {
                while (cur().kind == Tk::Semi || cur().kind == Tk::Comma) {
                    ++pos;
                }
                if (cur().kind == Tk::End) {
                    return true;
                }
                Stmt stmt;
                if (isWord("if") && condListAhead()) {
                    ++pos;  // T-390 (08.13): `if [not] <term> and ...`.
                    stmt.isIf = true;
                    if (!parseTerms(stmt.terms)) {
                        return false;
                    }
                    if (!parseSeq(stmt.seq)) {
                        return false;
                    }
                    if (isWord("else")) {
                        ++pos;
                        if (!parseSeq(stmt.alt)) {
                            return false;
                        }
                    }
                } else if (isWord("if")) {
                    ++pos;
                    stmt.isIf = true;
                    if (isWord("active") && pos + 1 < toks.size() && toks[pos + 1].kind != Tk::LParen) {
                        ++pos;  // T-386 (08.11): `if active <actions>`.
                        stmt.ifActive = true;
                        if (!parseSeq(stmt.seq)) {
                            return false;
                        }
                        if (isWord("else")) {
                            ++pos;
                            if (!parseSeq(stmt.alt)) {
                                return false;
                            }
                        }
                        out.push_back(std::move(stmt));
                        if (cur().kind != Tk::Semi && cur().kind != Tk::Comma && cur().kind != Tk::End) {
                            err = "expected ';' between actions, got \"" + cur().text + "\"";
                            return false;
                        }
                        continue;
                    }
                    CallTriggerEmit cond;
                    if (!parseCall(cond) || !cond.hasParens) {
                        if (err.empty()) {
                            err = "expected a comparison after 'if'";
                        }
                        return false;
                    }
                    if (!checkCondition(cond)) {
                        return false;
                    }
                    stmt.condCall = std::move(cond);
                    if (!parseSeq(stmt.seq)) {
                        return false;
                    }
                    if (isWord("else")) {
                        ++pos;
                        if (!parseSeq(stmt.alt)) {
                            return false;
                        }
                    }
                } else if (!parseSeq(stmt.seq)) {
                    return false;
                }
                out.push_back(std::move(stmt));
                if (cur().kind != Tk::Semi && cur().kind != Tk::Comma && cur().kind != Tk::End) {
                    err = "expected ';' between actions, got \"" + cur().text + "\"";
                    return false;
                }
            }
        }

        /// T-390 (08.13): true when the `if` at `pos` uses a term (`not`, `button()`, `state()`,
        /// `port()`) or `and`; a lone comparison or `active` keeps its T-175 / T-386 encoding.
        bool condListAhead() const {
            size_t p = pos + 1;
            if (p >= toks.size() || toks[p].kind != Tk::Word) {
                return false;
            }
            const std::string& w = toks[p].text;
            if (w == "not") {
                return true;
            }
            if (w == "external" || w == "dragging") {
                return true;  // T-393 (08.14), T-394 (08.15): only a term.
            }
            const bool call = p + 1 < toks.size() && toks[p + 1].kind == Tk::LParen;
            if (call && (w == "button" || w == "state" || w == "port")) {
                return true;
            }
            // Skip the first condition (`active` or a call) and look for `and`.
            ++p;
            if (call) {
                int depth = 0;
                for (; p < toks.size(); ++p) {
                    depth += toks[p].kind == Tk::LParen ? 1 : (toks[p].kind == Tk::RParen ? -1 : 0);
                    if (depth == 0) {
                        ++p;
                        break;
                    }
                }
            }
            return p < toks.size() && toks[p].kind == Tk::Word && toks[p].text == "and";
        }

        bool parseTerms(std::vector<CondTerm>& out) {
            for (;;) {
                CondTerm term;
                if (isWord("not")) {
                    term.negate = true;
                    ++pos;
                }
                if (isWord("active") && pos + 1 < toks.size() && toks[pos + 1].kind != Tk::LParen) {
                    term.kind = IfTerm::Active;
                    ++pos;
                } else if (isWord("external") &&
                           (pos + 1 >= toks.size() || toks[pos + 1].kind != Tk::LParen)) {
                    term.kind = IfTerm::External;  // T-393 (08.14).
                    ++pos;
                } else if (isWord("dragging") &&
                           (pos + 1 >= toks.size() || toks[pos + 1].kind != Tk::LParen)) {
                    term.kind = IfTerm::Dragging;  // T-394 (08.15).
                    ++pos;
                } else {
                    CallTriggerEmit c;
                    if (!parseCall(c) || !c.hasParens) {
                        if (err.empty()) {
                            err =
                                "expected a condition after 'if' (active, external, dragging, button(), "
                                "state(), port() or a comparison)";
                        }
                        return false;
                    }
                    const bool one = c.args.size() == 1 && c.args[0].key.empty() && !c.args[0].isString;
                    if (c.name == "button") {
                        PointerButton b = PointerButton::Left;
                        if (!one || !pointerButtonFromName(c.args[0].value, b)) {
                            err = "expected button(left|middle|right)";
                            return false;
                        }
                        term.kind = IfTerm::Button;
                        term.operand = c.args[0].value;
                    } else if (c.name == "state") {
                        if (!one || !isStateName(c.args[0].value)) {
                            err = "expected state(<Name>) in 'if'";
                            return false;
                        }
                        term.kind = IfTerm::State;
                        term.operand = c.args[0].value;
                    } else if (c.name == "port") {
                        if (!one) {
                            err = "expected port(<name>) <op> <value> in 'if'";
                            return false;
                        }
                        term.kind = IfTerm::Port;
                        term.operand = c.args[0].value;
                        size_t argc = 0;
                        if (cur().kind != Tk::Word || !comparisonOp(cur().text, argc) || argc != 1) {
                            err = "expected eq, ne, gt, lt, ge or le after port(" + term.operand + ")";
                            return false;
                        }
                        term.cmp.name = cur().text;
                        term.cmp.hasParens = true;
                        ++pos;
                        if (cur().kind != Tk::Word && cur().kind != Tk::Str) {
                            err = "expected a value after port(" + term.operand + ") " + term.cmp.name;
                            return false;
                        }
                        Arg value;
                        value.value = cur().text;
                        value.isString = cur().kind == Tk::Str;
                        ++pos;
                        term.cmp.args.push_back(std::move(value));
                    } else {
                        if (!checkCondition(c)) {
                            return false;
                        }
                        term.kind = IfTerm::Compare;
                        term.cmp = std::move(c);
                    }
                }
                out.push_back(std::move(term));
                if (!isWord("and")) {
                    return true;
                }
                ++pos;
            }
        }

        /// Only the comparison and its arity are checked here; the arguments are resolved per
        /// element (T-175, Emitter::condition()).
        bool checkCondition(const CallTriggerEmit& c) {
            size_t argc = 0;
            if (!comparisonOp(c.name, argc)) {
                err = "'if' only accepts a comparison converter (lt, le, gt, ge, eq, ne, between), got \"" +
                      c.name + "\"";
                return false;
            }
            if (c.args.size() != argc) {
                err = c.name + "() takes " + std::to_string(argc) + " argument(s)";
                return false;
            }
            for (const Arg& a : c.args) {
                if (!a.key.empty()) {
                    err = c.name + "() takes no named argument";
                    return false;
                }
            }
            return true;
        }
    };

    /// Converts one call into an action; false with `err` on anything invalid.
    bool makeAction(const CallTriggerEmit& c, IrTriggerAction& out, const std::string& file, int line,
                    int col, std::string& err) {
        out = IrTriggerAction{};
        auto argCount = [&](size_t lo, size_t hi) {
            if (c.args.size() < lo || c.args.size() > hi) {
                err = c.name + "() takes " +
                      (lo == hi ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi)) +
                      " argument(s)";
                return false;
            }
            return true;
        };
        auto plain = [&](size_t i) { return c.args[i].key.empty(); };
        auto timeline = [&](const std::string& name, const TimelineDef*& def) {
            auto it = m_timelines.find(name);
            if (it == m_timelines.end()) {
                err = "unknown timeline \"" + name + "\" in " + c.name + "()";
                return false;
            }
            def = it->second;
            return true;
        };
        auto hasMarker = [](const TimelineDef* def, const std::string& name) {
            for (const auto& m : def->markers) {
                if (m.first == name) {
                    return true;
                }
            }
            return false;
        };
        auto mode = [&](size_t i, uint8_t& flags) {
            if (i >= c.args.size()) {
                flags = static_cast<uint8_t>(TriggerMode::On);
                return true;
            }
            const std::string& v = c.args[i].value;
            if (!plain(i) || (v != "on" && v != "off" && v != "toggle")) {
                err = c.name + "() mode must be on, off or toggle";
                return false;
            }
            flags = static_cast<uint8_t>(v == "on"    ? TriggerMode::On
                                         : v == "off" ? TriggerMode::Off
                                                      : TriggerMode::Toggle);
            return true;
        };
        if (!c.hasParens && c.name != "show" && c.name != "hide" && c.name != "remove") {
            err = "expected '(' after \"" + c.name + "\"";
            return false;
        }
        // T-191 (28.2): play/stop/seek on a `<video>`/`<sequence>` id that is not a timeline name.
        auto isMedia = [&](const std::string& name) {
            return m_timelines.count(name) == 0 && m_mediaIds.count(name) != 0;
        };
        // T-202 (03 §3.12.5): play/stop(<particles id>) switch the emitter's emission; they share
        // kActionMedia (the runtime dispatches on the target element).
        if ((c.name == "play" || c.name == "stop" || c.name == "seek") && !c.args.empty() && plain(0) &&
            m_timelines.count(c.args[0].value) == 0 && m_particleIds.count(c.args[0].value) != 0) {
            const std::string& id = c.args[0].value;
            if (c.name == "seek") {
                err = "seek() does not apply to particles \"" + id + "\"";
                return false;
            }
            out.arg0 = m_writer.intern(id);
            out.arg1 = kInvalidString;
            out.flags = kActionMedia;
            out.kind = static_cast<uint8_t>(c.name == "play" ? TriggerAction::Play : TriggerAction::Stop);
            return argCount(1, 1);
        }
        if ((c.name == "play" || c.name == "stop" || c.name == "seek") && !c.args.empty() && plain(0) &&
            isMedia(c.args[0].value)) {
            const std::string& id = c.args[0].value;
            out.arg0 = m_writer.intern(id);
            out.arg1 = kInvalidString;
            out.flags = kActionMedia;
            if (c.name == "play") {
                out.kind = static_cast<uint8_t>(TriggerAction::Play);
                for (size_t i = 1; i < c.args.size(); ++i) {
                    const Arg& a = c.args[i];
                    if (!(a.key.empty() && a.value == "loop")) {
                        err = "play() of media \"" + id + "\" only takes the option loop";
                        return false;
                    }
                    out.flags |= kPlayLoop;
                }
                return true;
            }
            if (c.name == "stop") {
                out.kind = static_cast<uint8_t>(TriggerAction::Stop);
                return argCount(1, 1);
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Seek);
            float t = 0;
            if (!argCount(2, 2) || !plain(1)) {
                return false;
            }
            if (!parseSecondsTriggerEmit(c.args[1].value, t) || t < 0.0f) {
                err = "seek() of media \"" + id + "\" needs a time, got \"" + c.args[1].value + "\"";
                return false;
            }
            out.arg2 = t;
            return true;
        }
        if (c.name == "play") {
            if (c.args.empty() || !plain(0)) {
                err = "play() needs a timeline name";
                return false;
            }
            const TimelineDef* def = nullptr;
            if (!timeline(c.args[0].value, def)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Play);
            out.arg0 = m_writer.intern(c.args[0].value);
            out.arg1 = kInvalidString;
            for (size_t i = 1; i < c.args.size(); ++i) {
                const Arg& a = c.args[i];
                if (a.key == "from") {
                    if (!hasMarker(def, a.value)) {
                        err = "timeline \"" + c.args[0].value + "\" has no marker \"" + a.value + "\"";
                        return false;
                    }
                    out.arg1 = m_writer.intern(a.value);
                } else if (a.key.empty() && a.value == "reverse") {
                    out.flags |= kPlayReverse;
                } else if (a.key.empty() && a.value == "loop") {
                    out.flags |= kPlayLoop;
                } else if (a.key.empty() && a.value == "once") {
                    out.flags |= kPlayOnce;
                } else if (a.key.empty() && a.value == "hold") {
                    out.flags |= kPlayHold;
                } else {
                    err =
                        "unknown play() option \"" + (a.key.empty() ? a.value : a.key + "=" + a.value) + "\"";
                    return false;
                }
            }
            return true;
        }
        if (c.name == "stop") {
            const TimelineDef* def = nullptr;
            if (!argCount(1, 1) || !plain(0) || !timeline(c.args[0].value, def)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Stop);
            out.arg0 = m_writer.intern(c.args[0].value);
            return true;
        }
        if (c.name == "seek") {
            const TimelineDef* def = nullptr;
            if (!argCount(2, 2) || !plain(0) || !plain(1) || !timeline(c.args[0].value, def)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Seek);
            out.arg0 = m_writer.intern(c.args[0].value);
            out.arg1 = kInvalidString;
            float t = 0;
            if (hasMarker(def, c.args[1].value)) {
                out.arg1 = m_writer.intern(c.args[1].value);
            } else if (parseSecondsTriggerEmit(c.args[1].value, t) && t >= 0.0f) {
                out.arg2 = t;
            } else {
                err = "seek() needs a time or a marker of \"" + c.args[0].value + "\", got \"" +
                      c.args[1].value + "\"";
                return false;
            }
            return true;
        }
        if (c.name == "state") {
            // T-201: `target=#<id>` (resolved per element in its symbol scope, emitElements).
            for (size_t i = 0; i < c.args.size(); ++i) {
                if (c.args[i].key != "target") {
                    continue;
                }
                if (c.args[i].value.empty() || c.args[i].isString) {
                    err = "state() target must be #<id>";
                    return false;
                }
                CallTriggerEmit own = c;
                own.args.erase(own.args.begin() + static_cast<std::ptrdiff_t>(i));
                if (!makeAction(own, out, file, line, col, err)) {
                    return false;
                }
                out.flags = static_cast<uint8_t>(out.flags | kStateTarget);
                out.arg1 = kInvalidString;
                return true;
            }
            if (!argCount(1, 2) || !plain(0) || !isStateName(c.args[0].value)) {
                if (err.empty()) {
                    err = "state() needs a state name";
                }
                return false;
            }
            if (!mode(1, out.flags)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::State);
            if (const uint16_t builtin = builtinStateAlias(c.args[0].value); builtin != 0) {
                // T-390 (27.3.2): the built-in state, no custom state slot (Disabled keeps arg0 = 0);
                // T-392 (27.3.5): Focused / Hover / Active / Checked carry their ElementState bit.
                out.flags = static_cast<uint8_t>(out.flags | kStateBuiltin);
                out.arg0 = builtin == kStateDisabled ? 0u : static_cast<uint32_t>(builtin);
                return true;
            }
            int bit = stateBit(c.args[0].value, file, line, col);
            if (bit < 0) {
                err = "too many custom states";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::State);
            out.arg0 = static_cast<uint32_t>(bit);
            return true;
        }
        if (c.name == "class") {
            if (!argCount(1, 2) || !plain(0) || c.args[0].value.empty()) {
                return false;
            }
            if (!mode(1, out.flags)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Class);
            out.arg0 = m_writer.intern(c.args[0].value);
            return true;
        }
        if (c.name == "show" || c.name == "hide") {
            if (!c.args.empty()) {
                err = c.name + "() takes no argument";
                return false;
            }
            out.kind = static_cast<uint8_t>(c.name == "show" ? TriggerAction::Show : TriggerAction::Hide);
            return true;
        }
        if (c.name == "reset") {
            // T-399 (08 §8.18): `reset(value)`; the element must have `bind:value` (emitElements()).
            if (c.args.size() != 1 || !c.args[0].key.empty() || c.args[0].isString ||
                c.args[0].value != "value") {
                err = "reset() takes value";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Reset);
            return true;
        }
        if (c.name == "consume") {
            // T-394 (08.15): only in `on:outsidepress` (checked by compile()).
            if (!c.args.empty()) {
                err = "consume() takes no argument";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Consume);
            return true;
        }
        if (c.name == "remove") {
            // T-177: only in `item-exit` (checked by compile()).
            if (!c.args.empty()) {
                err = "remove takes no argument";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Remove);
            return true;
        }
        if (c.name == "sound") {
            if (!argCount(1, 2) || !plain(0)) {
                return false;
            }
            float volume = 1.0f;
            if (c.args.size() == 2 && (!plain(1) || !parseNumberTriggerEmit(c.args[1].value, volume))) {
                err = "sound() volume must be a number";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Sound);
            out.arg0 = m_writer.intern(c.args[0].value);
            out.arg2 = volume;
            return true;
        }
        if (c.name == "haptic") {
            // T-194: `haptic(<cue>[, <strength>])`.
            if (!argCount(1, 2) || !plain(0)) {
                return false;
            }
            float strength = 1.0f;
            if (c.args.size() == 2 && (!plain(1) || !parseNumberTriggerEmit(c.args[1].value, strength))) {
                err = "haptic() strength must be a number";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Haptic);
            out.arg0 = m_writer.intern(c.args[0].value);
            out.arg2 = strength;
            return true;
        }
        if (c.name == "emit") {
            if (!argCount(1, 1) || !plain(0)) {
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Emit);
            out.arg0 = m_writer.intern(c.args[0].value);
            return true;
        }
        if (c.name == "layer") {
            // T-194 (27.2.3): `layer(overlay)` / `layer(normal)`.
            if (!argCount(1, 1) || !plain(0) ||
                (c.args[0].value != "overlay" && c.args[0].value != "normal")) {
                err = "layer() takes overlay or normal";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Layer);
            out.flags = static_cast<uint8_t>(c.args[0].value == "overlay" ? 1 : 0);
            return true;
        }
        if (c.name == "focus") {
            if (c.args.empty()) {
                // T-392 (27.3.5): `focus()` focuses the trigger's own element.
                out.kind = static_cast<uint8_t>(TriggerAction::Focus);
                out.arg0 = kInvalidString;
                return true;
            }
            if (!argCount(0, 1) || !plain(0)) {
                return false;
            }
            std::string id = c.args[0].value;
            if (!id.empty() && id[0] == '#') {
                id.erase(0, 1);
            }
            if (m_ids.count(id) == 0) {
                err = "focus() target \"" + id + "\" is not an element id";
                return false;
            }
            out.kind = static_cast<uint8_t>(TriggerAction::Focus);
            out.arg0 = m_writer.intern(id);
            return true;
        }
        if (c.name == "set") {
            return makeSet(c, out, err);
        }
        if (c.name == "spawn") {
            return makeSpawn(c, out, err);
        }
        // T-408 (43 §43.6): a registered action alias compiles as its replacement.
        if (const NameAlias* alias = findNameAlias(AliasKind::Action, c.name)) {
            m_sink.deprecated(file, deprecatedMessage(*alias), line, col);
            CallTriggerEmit renamed = c;
            renamed.name = alias->replacement;
            return makeAction(renamed, out, file, line, col, err);
        }
        err = "unknown action \"" + c.name + "\"";
        return false;
    }

    /// T-201: `spawn(<symbol>, at=<#id|pointer|self>, lifetime=<time>[, timeline=<name>])`. The
    /// pool (`extra`) and an `at` id are resolved per element (emitElements).
    bool makeSpawn(const CallTriggerEmit& c, IrTriggerAction& out, std::string& err) {
        if (c.args.empty() || !c.args[0].key.empty() || c.args[0].value.empty()) {
            err = "spawn() needs a symbol name";
            return false;
        }
        out.kind = static_cast<uint8_t>(TriggerAction::Spawn);
        out.flags = static_cast<uint8_t>(SpawnAt::Self);
        out.arg0 = kInvalidString;
        out.arg1 = kInvalidString;
        out.arg2 = -1.0f;
        for (size_t i = 1; i < c.args.size(); ++i) {
            const Arg& a = c.args[i];
            if (a.key == "at") {
                out.flags = static_cast<uint8_t>(a.value == "self"      ? SpawnAt::Self
                                                 : a.value == "pointer" ? SpawnAt::Pointer
                                                                        : SpawnAt::Element);
            } else if (a.key == "lifetime") {
                float t = 0;
                if (!parseSecondsTriggerEmit(a.value, t) || !(t > 0.0f)) {
                    err = "spawn() lifetime must be a positive time, got \"" + a.value + "\"";
                    return false;
                }
                out.arg2 = t;
            } else if (a.key == "timeline") {
                if (m_timelines.count(a.value) == 0) {
                    err = "unknown timeline \"" + a.value + "\" in spawn()";
                    return false;
                }
                out.arg1 = m_writer.intern(a.value);
            } else {
                err = "unknown spawn() option \"" + (a.key.empty() ? a.value : a.key + "=" + a.value) + "\"";
                return false;
            }
        }
        if (out.arg2 < 0.0f) {
            err = "spawn() needs lifetime=<time>";
            return false;
        }
        return true;
    }

    /// T-201: `#<id>` / `<id>` as seen from DOM node `element`: inside a symbol instance the
    /// instance's `<instance>/<id>` (or an already qualified path in it), from an instance root
    /// its own `<root>/<id>`, else (outside any instance) a document-level id. Empty when none.
    std::string scopedId(uint32_t element, std::string id) const {
        if (!id.empty() && id[0] == '#') {
            id.erase(0, 1);
        }
        auto idOf = [&](uint32_t n) -> std::string {
            const DomAttr* a = m_dom.nodes[n].attr("id");
            return a != nullptr ? a->value : std::string();
        };
        const uint32_t scope = element < m_ports.scopeOf.size() ? m_ports.scopeOf[element] : kNoPortScope;
        if (scope != kNoPortScope) {
            const std::string prefix = idOf(scope) + "/";
            if (m_ids.count(prefix + id) != 0) {
                return prefix + id;
            }
            if (id.rfind(prefix, 0) == 0 && m_ids.count(id) != 0) {
                return id;
            }
        }
        const std::string own = idOf(element);
        if (!own.empty() && m_ids.count(own + "/" + id) != 0) {
            return own + "/" + id;
        }
        if (scope == kNoPortScope && m_ids.count(id) != 0) {
            return id;
        }
        return std::string();
    }

    /// T-201: `set(<port>, <number | true | false | toggle | "string" | event.value [| conv...]>)`.
    /// The port is resolved per element (emitElements); `converters` receives the text after
    /// `event.value |`.
    bool makeSet(const CallTriggerEmit& c, IrTriggerAction& out, std::string& err) {
        std::string converters;
        return setValue(c, out, converters, err);
    }

    /// T-411: `sourceText` (may be null) receives a bind-source value (isSourceValue()), whose flags
    /// here are provisional (Port, Item, else Number); emitElements() compiles it per element.
    bool setValue(const CallTriggerEmit& c, IrTriggerAction& out, std::string& converters, std::string& err,
                  std::string* sourceText = nullptr) {
        if (c.args.size() != 2 || !c.args[0].key.empty() || c.args[0].value.empty() || c.args[0].isString) {
            err = "set() takes a port and a value";
            return false;
        }
        out.kind = static_cast<uint8_t>(TriggerAction::Set);
        out.arg0 = 0;
        out.arg1 = 0;
        out.extra = 0;
        const Arg& v = c.args[1];
        float number = 0;
        if (v.isString) {
            out.flags = static_cast<uint8_t>(SetSource::String);
            const uint32_t id = m_writer.intern(v.value);
            std::memcpy(&out.arg2, &id, sizeof(id));
        } else if (v.value == "toggle") {
            out.flags = static_cast<uint8_t>(SetSource::Toggle);
        } else if (v.value == "true" || v.value == "false") {
            out.flags = static_cast<uint8_t>(SetSource::Number);
            out.arg2 = v.value == "true" ? 1.0f : 0.0f;
        } else if (parseNumberTriggerEmit(v.value, number)) {
            out.flags = static_cast<uint8_t>(SetSource::Number);
            out.arg2 = number;
        } else if (v.value.rfind("pointer.", 0) == 0) {
            // T-386 (08.11): the element id (arg2) is resolved per element (emitElements).
            SetSource source = SetSource::PointerFractionX;
            std::string id;
            if (parsePointerValue(v.value, source, id, converters, err) != 1) {
                return false;
            }
            out.flags = static_cast<uint8_t>(source);
        } else if (v.value.rfind("event.value", 0) == 0 || isElementValue(v.value)) {
            // T-392 (27.3.5): `value` is the trigger element's text (checked per trigger event).
            const bool element = v.value.rfind("event.value", 0) != 0;
            const size_t head = element ? 5 : 11;
            out.flags = static_cast<uint8_t>(element ? SetSource::ElementValue : SetSource::EventValue);
            const std::string rest = trimCopyTriggerEmit(std::string_view(v.value).substr(head));
            if (!rest.empty()) {
                if (rest[0] != '|' || trimCopyTriggerEmit(std::string_view(rest).substr(1)).empty()) {
                    err = "set() value \"" + v.value + "\": expected '|' and a converter after " +
                          (element ? "value" : "event.value");
                    return false;
                }
                converters = trimCopyTriggerEmit(std::string_view(rest).substr(1));
            }
        } else if (isSourceValue(v.value)) {
            // T-411 (27 §27.2.3): port(<name>) / item(...) / repeat.* / a constant, then converters.
            const bool port = v.value.rfind("port(", 0) == 0;
            const bool item = !port && (v.value.rfind("item(", 0) == 0 || v.value.rfind("repeat.", 0) == 0 ||
                                        v.value.rfind("outer.", 0) == 0);
            out.flags =
                static_cast<uint8_t>(port ? SetSource::Port : (item ? SetSource::Item : SetSource::Number));
            if (sourceText != nullptr) {
                *sourceText = v.value;
            }
        } else {
            err =
                "set() value must be a number, true, false, toggle, a quoted string, event.value [| "
                "<converter> ...], value [| <converter> ...], pointer.fraction|local(#<id>).x|.y, "
                "port(<name>), item(<field>), item(), repeat.index or repeat.count [| <converter> ...], "
                "got \"" +
                v.value + "\"";
            return false;
        }
        return true;
    }

    /// T-390 (08.13): appends the kIfTerms steps of `terms` (headers, then each term's
    /// comparison) to `out.conditions`. Port names and comparison arguments are resolved per
    /// element (emitElements). False after reporting an error.
    bool emitTerms(const std::vector<CondTerm>& terms, const ParsedEvent& ev, CompiledTrigger& out,
                   const std::string& file, int line, int col) {
        for (const CondTerm& term : terms) {
            IrConverterStep head{};
            head.op = static_cast<uint8_t>(ConverterOp::Operands);
            head.args[0] = static_cast<float>(static_cast<uint8_t>(term.kind));
            head.args[1] = term.negate ? 1.0f : 0.0f;
            CallTriggerEmit headCall;  // Empty name: nothing to resolve per element.
            switch (term.kind) {
                case IfTerm::Compare:
                case IfTerm::Active:
                case IfTerm::External:
                case IfTerm::Dragging:  // T-394 (08.15).
                    break;
                case IfTerm::Button: {
                    if (!isPointerTriggerEvent(ev.event)) {
                        error(file,
                              "invalid actions for " + ev.canonical +
                                  ": if button() is only valid in pointer triggers (press, release, click, "
                                  "dblclick, hover, leave, pointermove, pointercancel, pointerdrag)",
                              line, col);
                        return false;
                    }
                    PointerButton b = PointerButton::Left;
                    pointerButtonFromName(term.operand, b);
                    head.args[2] = static_cast<float>(static_cast<uint8_t>(b));
                    break;
                }
                case IfTerm::State: {
                    uint16_t builtin = 0;
                    if (term.operand == "Disabled") {
                        builtin = kStateDisabled;
                    } else if (term.operand == "Hover") {
                        builtin = kStateHover;
                    } else if (term.operand == "Active") {
                        builtin = kStateActive;
                    } else if (term.operand == "Focus" || term.operand == "Focused") {
                        builtin = kStateFocus;  // T-392 (27.3.5): Focused is the built-in name too.
                    } else if (term.operand == "Checked") {
                        builtin = kStateChecked;
                    }
                    if (builtin != 0) {
                        uint32_t bit = 0;
                        while ((1u << bit) != builtin) {
                            ++bit;
                        }
                        head.args[2] = static_cast<float>(bit);
                        head.args[3] = 1.0f;
                    } else {
                        const int bit = stateBit(term.operand, file, line, col);
                        if (bit < 0) {
                            return false;
                        }
                        head.args[2] = static_cast<float>(bit);
                    }
                    break;
                }
                case IfTerm::Port:
                    head.argKinds = static_cast<uint16_t>(static_cast<uint16_t>(ConverterArg::Port) << 8u);
                    headCall.name = "#port";
                    headCall.args.push_back(Arg{std::string(), term.operand, false});
                    break;
            }
            const bool compares = term.kind == IfTerm::Compare || term.kind == IfTerm::Port;
            head.argCount = compares ? 1 : 0;
            out.conditions.push_back(head);
            out.condCalls.push_back(std::move(headCall));
            if (compares) {
                out.conditions.push_back(IrConverterStep{});  // Compiled per element (T-175).
                out.condCalls.push_back(term.cmp);
            }
        }
        return true;
    }

    /// Compiles one `<event>` + `<actions>` pair. Errors are reported at file/line/col.
    CompiledTrigger compile(const std::string& eventText, const std::string& actionText,
                            const std::string& file, int line, int col) {
        CompiledTrigger out;
        out.file = file;
        out.line = line;
        out.col = col;
        ParsedEvent ev;
        std::string err;
        if (!parseEvent(eventText, ev, err)) {
            error(file, err, line, col);
            return out;
        }
        if (ev.alias != nullptr) {  // T-408 (43 §43.6): compiles as its replacement.
            m_sink.deprecated(file, deprecatedMessage(*ev.alias), line, col);
        }
        out.canonical = ev.canonical;
        out.trigger.event = static_cast<uint8_t>(ev.event);
        out.trigger.button = ev.button;  // T-390 (08.13).
        out.trigger.arg0 = kInvalidString;
        out.trigger.arg1 = kInvalidString;
        switch (ev.event) {
            case TriggerEvent::Key:
                out.trigger.arg0 = ev.key;
                break;
            case TriggerEvent::Port:
                out.trigger.arg0 = m_writer.intern(ev.arg0);
                out.eventPort = ev.arg0;  // arg1 = PORT index + 1, resolved per element.
                break;
            case TriggerEvent::MediaEnd:
                // T-191: the id must name a `<video>`/`<sequence>` of this document.
                if (m_mediaIds.count(ev.arg0) == 0) {
                    error(file, "unknown video or sequence \"" + ev.arg0 + "\" in on:" + eventText, line,
                          col);
                    return out;
                }
                out.trigger.arg0 = m_writer.intern(ev.arg0);
                break;
            case TriggerEvent::TimelineEnd:
            case TriggerEvent::TimelineMarker: {
                auto it = m_timelines.find(ev.arg0);
                if (it == m_timelines.end()) {
                    error(file, "unknown timeline \"" + ev.arg0 + "\" in on:" + eventText, line, col);
                    return out;
                }
                out.trigger.arg0 = m_writer.intern(ev.arg0);
                if (ev.event == TriggerEvent::TimelineMarker) {
                    bool found = false;
                    for (const auto& m : it->second->markers) {
                        found = found || m.first == ev.arg1;
                    }
                    if (!found) {
                        error(file, "timeline \"" + ev.arg0 + "\" has no marker \"" + ev.arg1 + "\"", line,
                              col);
                        return out;
                    }
                    out.trigger.arg1 = m_writer.intern(ev.arg1);
                }
                break;
            }
            case TriggerEvent::StateEnter:
            case TriggerEvent::StateExit: {
                int bit = stateBit(ev.arg0, file, line, col);
                if (bit < 0) {
                    return out;
                }
                out.trigger.arg0 = static_cast<uint32_t>(bit);
                break;
            }
            // T-194: small codes (not StringIds); see TriggerEvent.
            case TriggerEvent::NavBlocked:
                out.trigger.arg0 = ev.key;
                break;
            case TriggerEvent::Gesture:
            case TriggerEvent::Player:
                out.trigger.arg0 = ev.key;
                out.trigger.arg1 = ev.arg1.empty()
                                       ? kInvalidString
                                       : static_cast<uint32_t>(std::strtoul(ev.arg1.c_str(), nullptr, 10));
                break;
            default:
                break;
        }

        ActionParser parser;
        parser.text = actionText;
        if (!lexActions(actionText, parser.toks, err)) {
            error(file, "invalid actions for " + ev.canonical + ": " + err, line, col);
            return out;
        }
        std::vector<Stmt> stmts;
        if (!parser.parseList(stmts)) {
            error(file, "invalid actions for " + ev.canonical + ": " + parser.err, line, col);
            return out;
        }
        if (stmts.empty()) {
            error(file, "no actions for " + ev.canonical, line, col);
            return out;
        }
        // Layout: statement heads first, then every continuation / branch.
        out.actions.resize(stmts.size());
        bool ok = true;
        auto emitSeq = [&](const std::vector<CallTriggerEmit>& seq, size_t headSlot) {
            // Writes seq[0] into `headSlot` and appends the rest, chained through `next`.
            size_t prev = headSlot;
            for (size_t k = 0; k < seq.size(); ++k) {
                IrTriggerAction act{};
                std::string aerr;
                if (!makeAction(seq[k], act, file, line, col, aerr)) {
                    if (aerr.empty()) {
                        aerr = "invalid arguments to " + seq[k].name + "()";
                    }
                    error(file, "invalid actions for " + ev.canonical + ": " + aerr, line, col);
                    ok = false;
                    return;
                }
                size_t slot = headSlot;
                if (k == 0) {
                    out.actions[headSlot] = act;
                } else {
                    out.actions.push_back(act);
                    size_t idx = out.actions.size() - 1;
                    out.actions[prev].next = static_cast<uint16_t>(idx + 1);
                    prev = idx;
                    slot = idx;
                }
                if (act.kind == static_cast<uint8_t>(TriggerAction::Emit)) {
                    out.emitPorts.emplace_back(slot, seq[k].args[0].value);  // Resolved per element.
                }
                if (act.kind == static_cast<uint8_t>(TriggerAction::Spawn)) {
                    CompiledTrigger::SpawnRef ref;
                    ref.slot = slot;
                    ref.symbol = seq[k].args[0].value;
                    for (const Arg& a : seq[k].args) {
                        if (a.key == "at" && act.flags == static_cast<uint8_t>(SpawnAt::Element)) {
                            ref.at = a.value;
                        }
                    }
                    out.spawns.push_back(std::move(ref));
                }
                if (act.kind == static_cast<uint8_t>(TriggerAction::State) &&
                    (act.flags & kStateTarget) != 0) {
                    for (const Arg& a : seq[k].args) {
                        if (a.key == "target") {
                            out.stateTargets.emplace_back(slot, a.value);
                        }
                    }
                }
                if (act.kind == static_cast<uint8_t>(TriggerAction::Set)) {
                    CompiledTrigger::SetPort sp;
                    sp.slot = slot;
                    sp.port = seq[k].args[0].value;
                    IrTriggerAction scratch{};
                    std::string ignored;
                    setValue(seq[k], scratch, sp.converters, ignored, &sp.source);
                    const bool sourceValue = !sp.source.empty();
                    out.setPorts.push_back(std::move(sp));
                    if (act.flags == static_cast<uint8_t>(SetSource::ElementValue)) {
                        out.usesElementValue = true;  // T-392 (27.3.5).
                    } else if (!sourceValue &&
                               act.flags >= static_cast<uint8_t>(SetSource::PointerFractionX) &&
                               act.flags <= static_cast<uint8_t>(SetSource::PointerLocalY)) {
                        SetSource source = SetSource::PointerFractionX;
                        std::string id;
                        std::string conv;
                        parsePointerValue(seq[k].args[1].value, source, id, conv, ignored);
                        out.pointerRefs.emplace_back(slot, id);  // Resolved per element.
                    }
                }
            }
        };
        for (size_t j = 0; j < stmts.size() && ok; ++j) {
            const Stmt& stmt = stmts[j];
            if (!stmt.isIf) {
                emitSeq(stmt.seq, j);
                continue;
            }
            IrTriggerAction ifAct{};
            ifAct.kind = static_cast<uint8_t>(TriggerAction::If);
            ifAct.arg0 = static_cast<uint32_t>(out.conditions.size());
            if (!stmt.terms.empty()) {
                ifAct.flags = kIfTerms;  // T-390 (08.13).
                if (!emitTerms(stmt.terms, ev, out, file, line, col)) {
                    ok = false;
                    break;
                }
                ifAct.arg1 = static_cast<uint32_t>(out.conditions.size()) - ifAct.arg0;
            } else if (stmt.ifActive) {
                ifAct.flags = kIfActive;  // T-386: no comparison steps (an empty range).
                ifAct.arg1 = 0;
            } else {
                ifAct.arg1 = 1;
                out.conditions.push_back(IrConverterStep{});  // Compiled per element (T-175).
                out.condCalls.push_back(stmt.condCall);
            }
            out.actions[j] = ifAct;
            out.actions.push_back(IrTriggerAction{});
            size_t trueSlot = out.actions.size() - 1;
            out.actions[j].next = static_cast<uint16_t>(trueSlot + 1);
            emitSeq(stmt.seq, trueSlot);
            if (ok && !stmt.alt.empty()) {
                out.actions.push_back(IrTriggerAction{});
                size_t altSlot = out.actions.size() - 1;
                out.actions[j].alt = static_cast<uint16_t>(altSlot + 1);
                emitSeq(stmt.alt, altSlot);
            }
        }
        if (!ok) {
            return out;
        }
        if (out.actions.size() > 0xFFFFu) {
            error(file, "too many actions for " + ev.canonical, line, col);
            return out;
        }
        // T-177: `remove` ends an `item-exit` chain; anywhere else it has nothing to remove.
        bool hasRemove = false;
        for (const IrTriggerAction& act : out.actions) {
            hasRemove = hasRemove || act.kind == static_cast<uint8_t>(TriggerAction::Remove);
        }
        if (hasRemove && ev.event != TriggerEvent::ItemExit) {
            error(file, "invalid actions for " + ev.canonical + ": remove is only valid in item-exit", line,
                  col);
            return out;
        }
        // T-394 (08.15): `consume()` takes the press of an OutsidePress; nowhere else.
        for (const IrTriggerAction& act : out.actions) {
            if (act.kind == static_cast<uint8_t>(TriggerAction::Consume) &&
                ev.event != TriggerEvent::OutsidePress) {
                error(file,
                      "invalid actions for " + ev.canonical + ": consume() is only valid in outsidepress",
                      line, col);
                return out;
            }
        }
        // T-386 (08.11): `pointer.fraction()` / `pointer.local()` read the firing pointer event.
        if (!out.pointerRefs.empty() && !isPointerTriggerEvent(ev.event)) {
            error(file,
                  "invalid actions for " + ev.canonical +
                      ": pointer.fraction() / pointer.local() are only valid in pointer triggers (press, "
                      "release, click, dblclick, hover, leave, pointermove, pointercancel, pointerdrag)",
                  line, col);
            return out;
        }
        // T-392 (27.3.5): `value` reads the element's text after an edit.
        if (out.usesElementValue && ev.event != TriggerEvent::Change && ev.event != TriggerEvent::Input) {
            error(
                file,
                "invalid actions for " + ev.canonical + ": value is only valid in change and input triggers",
                line, col);
            return out;
        }
        if (!hasRemove && ev.event == TriggerEvent::ItemExit) {
            m_sink.warn(
                file, "item-exit without `then remove`: the instance is removed as soon as the actions start",
                line, col);
        }

        out.trigger.actionCount = static_cast<uint16_t>(out.actions.size());
        out.trigger.headCount = static_cast<uint16_t>(stmts.size());
        out.ok = true;
        return out;
    }

    void compileRuleTriggers() {
        m_ruleTriggers.resize(m_rules.rules.size());
        for (size_t r = 0; r < m_rules.rules.size(); ++r) {
            const StyleRule& rule = m_rules.rules[r];
            for (const TriggerDecl& decl : rule.triggers) {
                m_ruleTriggers[r].push_back(compile(
                    decl.event, decl.actions, rule.file.empty() ? m_fileName : rule.file, decl.line, 0));
            }
        }
    }

    void emitElements(TriggerBuilder& out) {
        for (uint32_t element = 0; element < m_dom.nodes.size(); ++element) {
            const DomNode& node = m_dom.nodes[element];
            // canonical event -> (priority, compiled trigger). `.xt` attributes rank above all.
            std::map<std::string, std::pair<uint64_t, CompiledTrigger>> chosen;
            std::vector<std::string> order;
            uint64_t sourceOrder = 0;
            for (size_t r = 0; r < m_rules.rules.size(); ++r) {
                const StyleRule& rule = m_rules.rules[r];
                for (const Selector& sel : rule.selectors) {
                    ++sourceOrder;
                    if (rule.triggers.empty() || !selectorMatchesStatically(sel, m_dom, element)) {
                        continue;
                    }
                    uint64_t priority =
                        (static_cast<uint64_t>(sel.specificity) << 24) | (sourceOrder & 0xFFFFFFu);
                    for (const CompiledTrigger& ct : m_ruleTriggers[r]) {
                        if (!ct.ok) {
                            continue;
                        }
                        auto it = chosen.find(ct.canonical);
                        if (it == chosen.end()) {
                            order.push_back(ct.canonical);
                            chosen.emplace(ct.canonical, std::make_pair(priority, ct));
                        } else if (priority >= it->second.first) {
                            it->second = std::make_pair(priority, ct);
                        }
                    }
                }
            }
            for (const DomAttr& attr : node.attrs) {
                if (attr.name.size() <= 3 || attr.name.compare(0, 3, "on:") != 0) {
                    continue;
                }
                CompiledTrigger ct =
                    compile(attr.name.substr(3), attr.value, m_fileName, attr.line, attr.col);
                if (!ct.ok) {
                    continue;
                }
                const std::string key = ct.canonical;
                if (chosen.find(key) == chosen.end()) {
                    order.push_back(key);
                }
                chosen[key] = std::make_pair(~0ull, std::move(ct));
            }
            if (order.empty()) {
                continue;
            }
            IrTriggerTable table{};
            table.element = element;
            table.triggerOffset = static_cast<uint32_t>(out.triggers.size());
            for (const std::string& key : order) {
                const CompiledTrigger& ct = chosen[key].second;
                IrTrigger t = ct.trigger;
                // T-175: this element's view of the trigger's port names.
                int eventPort = -1;
                if (static_cast<TriggerEvent>(t.event) == TriggerEvent::Port) {
                    eventPort = m_ports.resolve(ct.eventPort, element);
                    t.arg1 = eventPort >= 0 ? static_cast<uint32_t>(eventPort) + 1u : kInvalidString;
                    if (eventPort < 0) {
                        m_sink.warn(ct.file,
                                    "on:port(" + ct.eventPort + "): no port \"" + ct.eventPort +
                                        "\" is declared here; the trigger never fires",
                                    ct.line, ct.col);
                    }
                }
                std::vector<IrTriggerAction> actions = ct.actions;
                for (const auto& [slot, name] : ct.emitPorts) {
                    int port = m_ports.resolve(name, element);
                    actions[slot].arg1 = port >= 0 ? static_cast<uint32_t>(port) + 1u : 0u;
                }
                std::vector<IrConverterStep> conditions = ct.conditions;
                bool condOk = true;
                int termPort = -1;  // T-390: the Port term whose comparison comes next.
                for (size_t i = 0; i < ct.condCalls.size(); ++i) {
                    const CallTriggerEmit& cc = ct.condCalls[i];
                    if (cc.name.empty()) {
                        continue;  // T-390: a kIfTerms header without names.
                    }
                    if (cc.name == "#port") {
                        termPort = m_ports.resolve(cc.args[0].value, element);
                        if (termPort < 0 ||
                            static_cast<PortDir>(m_ports.ports[static_cast<size_t>(termPort)].dir) ==
                                PortDir::Call) {
                            error(ct.file,
                                  "invalid actions for " + ct.canonical + ": if port(" + cc.args[0].value +
                                      "): no value port \"" + cc.args[0].value + "\" is declared here",
                                  ct.line, ct.col);
                            condOk = false;
                            termPort = -1;
                            ++i;  // Its comparison.
                            continue;
                        }
                        const uint32_t bits = static_cast<uint32_t>(termPort);
                        std::memcpy(&conditions[i].args[2], &bits, sizeof(bits));
                        continue;
                    }
                    const int cmpPort = termPort >= 0 ? termPort : eventPort;
                    termPort = -1;
                    std::string err;
                    if (!condition(cc, element, cmpPort, conditions[i], err)) {
                        error(ct.file, "invalid actions for " + ct.canonical + ": " + err, ct.line, ct.col);
                        condOk = false;
                    }
                }
                for (const CompiledTrigger::SpawnRef& ref : ct.spawns) {
                    // T-201: the element's spawn pool for the symbol (lowerSpawnPools() made one per
                    // element and symbol from its `on:` attributes; none for an `.xts` `on-*`).
                    const std::string where = "invalid actions for " + ct.canonical + ": ";
                    int ordinal = -1;
                    int seen = 0;
                    for (uint32_t child : m_dom.nodes[element].children) {
                        const DomAttr* spawnAttr = m_dom.nodes[child].attr("spawn");
                        if (m_dom.nodes[child].tag != "repeat" || spawnAttr == nullptr) {
                            continue;
                        }
                        if (spawnAttr->value == ref.symbol && ordinal < 0) {
                            ordinal = seen;
                        }
                        ++seen;
                    }
                    if (ordinal < 0) {
                        error(ct.file, where + "spawn() is only supported in on: attributes", ct.line,
                              ct.col);
                        condOk = false;
                        continue;
                    }
                    actions[ref.slot].extra = static_cast<uint16_t>(ordinal);
                    if (!ref.at.empty()) {
                        const std::string id = scopedId(element, ref.at);
                        if (id.empty()) {
                            error(ct.file,
                                  where + "spawn() at \"" + ref.at + "\" is not an element of this scope",
                                  ct.line, ct.col);
                            condOk = false;
                            continue;
                        }
                        actions[ref.slot].arg0 = m_writer.intern(id);
                    }
                }
                for (const IrTriggerAction& act : actions) {
                    // T-399 (08 §8.18): `reset(value)` re-applies this element's `bind:value`.
                    if (act.kind == static_cast<uint8_t>(TriggerAction::Reset) &&
                        m_dom.nodes[element].attr("bind:value") == nullptr) {
                        error(ct.file,
                              "invalid actions for " + ct.canonical +
                                  ": reset(value) needs bind:value on the element",
                              ct.line, ct.col);
                        condOk = false;
                        break;
                    }
                }
                for (const auto& [slot, text] : ct.stateTargets) {
                    // T-201: `#<id>` inside this element's symbol scope (scopedId()).
                    const std::string resolved = scopedId(element, text);
                    const std::string id = !text.empty() && text[0] == '#' ? text.substr(1) : text;
                    if (resolved.empty()) {
                        error(ct.file,
                              "invalid actions for " + ct.canonical + ": state() target \"#" + id +
                                  "\" is not an element of this symbol scope",
                              ct.line, ct.col);
                        condOk = false;
                        continue;
                    }
                    actions[slot].arg1 = m_writer.intern(resolved);
                }
                for (const auto& [slot, text] : ct.pointerRefs) {
                    // T-386 (08.11): `#<id>` inside this element's symbol scope, as state(target=).
                    const std::string resolved = scopedId(element, text);
                    if (resolved.empty()) {
                        error(ct.file,
                              "invalid actions for " + ct.canonical + ": pointer value element \"#" + text +
                                  "\" is not an element of this symbol scope",
                              ct.line, ct.col);
                        condOk = false;
                        continue;
                    }
                    const uint32_t sid = m_writer.intern(resolved);
                    std::memcpy(&actions[slot].arg2, &sid, sizeof(sid));
                }
                for (const CompiledTrigger::SetPort& sp : ct.setPorts) {
                    // T-201: the port as seen from this element; its converters after the conditions.
                    const std::string where = "invalid actions for " + ct.canonical + ": ";
                    const int port = m_ports.resolve(sp.port, element);
                    if (port < 0) {
                        error(ct.file, where + "set(): no port \"" + sp.port + "\" is declared here", ct.line,
                              ct.col);
                        condOk = false;
                        continue;
                    }
                    const IrPort& decl = m_ports.ports[static_cast<size_t>(port)];
                    IrTriggerAction& act = actions[sp.slot];
                    if (static_cast<PortDir>(decl.dir) == PortDir::Call ||
                        static_cast<PortValueType>(decl.type) == PortValueType::List ||
                        static_cast<PortValueType>(decl.type) == PortValueType::Struct) {
                        error(ct.file,
                              where + "set() cannot set the call, list or struct port \"" + sp.port + "\"",
                              ct.line, ct.col);
                        condOk = false;
                        continue;
                    }
                    if (act.flags == static_cast<uint8_t>(SetSource::Toggle) &&
                        static_cast<PortValueType>(decl.type) != PortValueType::Bool) {
                        error(ct.file, where + "set(" + sp.port + ", toggle) needs a bool port", ct.line,
                              ct.col);
                        condOk = false;
                        continue;
                    }
                    act.arg1 = static_cast<uint32_t>(port) + 1u;
                    if (!sp.source.empty()) {
                        // T-411 (27 §27.2.3): the bind-source value as seen from this element.
                        ActionValue value;
                        std::string err;
                        if (!compileActionValue(m_dom, m_ports, m_repeats, m_writer, element, sp.source,
                                                value, err) ||
                            value.steps.size() > 0xFFFFu) {
                            error(ct.file, where + "set(): " + (err.empty() ? "invalid value" : err), ct.line,
                                  ct.col);
                            condOk = false;
                            continue;
                        }
                        uint32_t bits = 0;
                        switch (value.head) {
                            case ActionValue::Head::Number:
                                act.flags = static_cast<uint8_t>(SetSource::Number);
                                act.arg2 = value.number;
                                break;
                            case ActionValue::Head::String:
                                act.flags = static_cast<uint8_t>(SetSource::String);
                                bits = m_writer.intern(value.text);
                                std::memcpy(&act.arg2, &bits, sizeof(bits));
                                break;
                            case ActionValue::Head::Port:
                                act.flags = static_cast<uint8_t>(SetSource::Port);
                                bits = static_cast<uint32_t>(value.port);
                                std::memcpy(&act.arg2, &bits, sizeof(bits));
                                break;
                            case ActionValue::Head::Item:
                                act.flags = static_cast<uint8_t>(SetSource::Item);
                                act.arg2 = 0;
                                break;
                        }
                        if (!value.steps.empty()) {
                            act.arg0 = static_cast<uint32_t>(conditions.size());
                            act.extra = static_cast<uint16_t>(value.steps.size());
                            conditions.insert(conditions.end(), value.steps.begin(), value.steps.end());
                        }
                    } else if (!sp.converters.empty()) {
                        std::vector<IrConverterStep> steps;
                        std::string err;
                        if (!compileConverterChain(m_dom, m_ports, m_writer, element, sp.converters, steps,
                                                   err) ||
                            steps.size() > 0xFFFFu) {
                            error(ct.file, where + "set(): " + (err.empty() ? "invalid converters" : err),
                                  ct.line, ct.col);
                            condOk = false;
                            continue;
                        }
                        act.arg0 = static_cast<uint32_t>(conditions.size());
                        act.extra = static_cast<uint16_t>(steps.size());
                        conditions.insert(conditions.end(), steps.begin(), steps.end());
                    }
                }
                if (!condOk) {
                    continue;
                }
                t.actionOffset = static_cast<uint32_t>(out.actions.size());
                uint32_t condBase = static_cast<uint32_t>(out.conditions.size());
                for (IrTriggerAction act : actions) {
                    if (act.kind == static_cast<uint8_t>(TriggerAction::If) ||
                        (act.kind == static_cast<uint8_t>(TriggerAction::Set) && act.extra != 0)) {
                        act.arg0 += condBase;
                    }
                    out.actions.push_back(act);
                }
                out.conditions.insert(out.conditions.end(), conditions.begin(), conditions.end());
                out.triggers.push_back(t);
            }
            table.triggerCount = static_cast<uint32_t>(out.triggers.size()) - table.triggerOffset;
            out.tables.push_back(table);
        }
    }

public:
    /// T-180 (validate.h): the action-list grammar of ActionParser plus each action's
    /// document-independent shape (name, argument count, modes, options). Timelines, markers,
    /// focus targets and ports are not resolved.
    static bool validateActionsSyntax(std::string_view text, std::string& err) {
        ActionParser parser;
        parser.text = text;
        if (!lexActions(text, parser.toks, err)) {
            return false;
        }
        std::vector<Stmt> stmts;
        if (!parser.parseList(stmts)) {
            err = parser.err;
            return false;
        }
        for (const Stmt& stmt : stmts) {
            for (const std::vector<CallTriggerEmit>* seq : {&stmt.seq, &stmt.alt}) {
                for (const CallTriggerEmit& call : *seq) {
                    if (!checkActionShape(call, err)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

private:
    static bool checkActionShape(const CallTriggerEmit& c, std::string& err) {
        auto argCount = [&](size_t lo, size_t hi) {
            if (c.args.size() < lo || c.args.size() > hi) {
                err = c.name + "() takes " +
                      (lo == hi ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi)) +
                      " argument(s)";
                return false;
            }
            return true;
        };
        auto plain = [&](size_t i) { return c.args[i].key.empty(); };
        auto mode = [&](size_t i) {
            if (i >= c.args.size()) {
                return true;
            }
            const std::string& v = c.args[i].value;
            if (!plain(i) || (v != "on" && v != "off" && v != "toggle")) {
                err = c.name + "() mode must be on, off or toggle";
                return false;
            }
            return true;
        };
        if (!c.hasParens && c.name != "show" && c.name != "hide") {
            err = "expected '(' after \"" + c.name + "\"";
            return false;
        }
        if (c.name == "play") {
            if (c.args.empty() || !plain(0)) {
                err = "play() needs a timeline name";
                return false;
            }
            for (size_t i = 1; i < c.args.size(); ++i) {
                const Arg& a = c.args[i];
                const bool option = a.key.empty() && (a.value == "reverse" || a.value == "loop" ||
                                                      a.value == "once" || a.value == "hold");
                if (a.key != "from" && !option) {
                    err =
                        "unknown play() option \"" + (a.key.empty() ? a.value : a.key + "=" + a.value) + "\"";
                    return false;
                }
            }
            return true;
        }
        if (c.name == "stop") {
            return argCount(1, 1) && plain(0);
        }
        if (c.name == "seek") {
            return argCount(2, 2) && plain(0) && plain(1);
        }
        if (c.name == "state") {
            for (size_t i = 0; i < c.args.size(); ++i) {
                if (c.args[i].key == "target") {  // T-201.
                    CallTriggerEmit own = c;
                    own.args.erase(own.args.begin() + static_cast<std::ptrdiff_t>(i));
                    return checkActionShape(own, err);
                }
            }
            if (!argCount(1, 2) || !plain(0) || !isStateName(c.args[0].value)) {
                if (err.empty()) {
                    err = "state() needs a state name";
                }
                return false;
            }
            return mode(1);
        }
        if (c.name == "class") {
            if (!argCount(1, 2) || !plain(0) || c.args[0].value.empty()) {
                if (err.empty()) {
                    err = "class() needs a class name";
                }
                return false;
            }
            return mode(1);
        }
        if (c.name == "reset") {  // T-399 (08 §8.18).
            if (c.args.size() != 1 || !c.args[0].key.empty() || c.args[0].isString ||
                c.args[0].value != "value") {
                err = "reset() takes value";
                return false;
            }
            return true;
        }
        if (c.name == "show" || c.name == "hide" || c.name == "consume") {  // T-394: consume().
            if (!c.args.empty()) {
                err = c.name + "() takes no argument";
                return false;
            }
            return true;
        }
        if (c.name == "sound") {
            if (!argCount(1, 2) || !plain(0)) {
                return false;
            }
            float volume = 1.0f;
            if (c.args.size() == 2 && (!plain(1) || !parseNumberTriggerEmit(c.args[1].value, volume))) {
                err = "sound() volume must be a number";
                return false;
            }
            return true;
        }
        if (c.name == "haptic") {  // T-194: optional strength.
            float strength = 1.0f;
            if (!argCount(1, 2) || !plain(0)) {
                return false;
            }
            if (c.args.size() == 2 && (!plain(1) || !parseNumberTriggerEmit(c.args[1].value, strength))) {
                err = "haptic() strength must be a number";
                return false;
            }
            return true;
        }
        if (c.name == "layer") {  // T-194.
            if (!argCount(1, 1) || !plain(0) ||
                (c.args[0].value != "overlay" && c.args[0].value != "normal")) {
                err = "layer() takes overlay or normal";
                return false;
            }
            return true;
        }
        if (c.name == "focus" && c.args.empty()) {
            return true;  // T-392 (27.3.5): the trigger's own element.
        }
        if (c.name == "emit" || c.name == "focus") {
            return argCount(1, 1) && plain(0);
        }
        if (c.name == "set") {  // T-201: the port and the value's form (converters not resolved).
            if (c.args.size() != 2 || !plain(0) || c.args[0].value.empty()) {
                err = "set() takes a port and a value";
                return false;
            }
            const Arg& v = c.args[1];
            float number = 0;
            if (v.isString || v.value == "toggle" || v.value == "true" || v.value == "false" ||
                parseNumberTriggerEmit(v.value, number) || v.value == "event.value" ||
                (v.value.rfind("event.value", 0) == 0 &&
                 trimCopyTriggerEmit(std::string_view(v.value).substr(11))[0] == '|') ||
                (!v.isString && isElementValue(v.value))) {  // T-392 (27.3.5).
                return true;
            }
            if (!v.isString && v.value.rfind("pointer.", 0) == 0) {  // T-386 (08.11).
                SetSource source = SetSource::PointerFractionX;
                std::string id;
                std::string converters;
                return parsePointerValue(v.value, source, id, converters, err) == 1;
            }
            if (!v.isString && isSourceValue(v.value)) {
                return true;  // T-411: checked against the scope when compiled.
            }
            err =
                "set() value must be a number, true, false, toggle, a quoted string, event.value [| "
                "<converter> ...], value [| <converter> ...], pointer.fraction|local(#<id>).x|.y, "
                "port(<name>), "
                "item(<field>), item(), repeat.index or repeat.count [| <converter> ...]";
            return false;
        }
        if (c.name == "spawn") {  // T-201: a symbol and at / lifetime / timeline options.
            if (c.args.empty() || !plain(0)) {
                err = "spawn() needs a symbol name";
                return false;
            }
            bool lifetime = false;
            for (size_t i = 1; i < c.args.size(); ++i) {
                const std::string& key = c.args[i].key;
                if (key != "at" && key != "lifetime" && key != "timeline") {
                    err = "unknown spawn() option \"" + (key.empty() ? c.args[i].value : key) + "\"";
                    return false;
                }
                lifetime = lifetime || key == "lifetime";
            }
            if (!lifetime) {
                err = "spawn() needs lifetime=<time>";
                return false;
            }
            return true;
        }
        err = "unknown action \"" + c.name + "\"";
        return false;
    }
};

}  // namespace

bool emitTriggers(const DomTree& dom, const RuleSet& ruleSet, const PortScopes& ports, IrWriter& writer,
                  std::string_view fileName, DiagnosticSink& sink, CustomStateBuilder& states,
                  TriggerBuilder& triggers, const RepeatScopes* repeats) {
    Emitter emitter(dom, ruleSet, ports, writer, fileName, sink, repeats);
    return emitter.run(states, triggers);
}

bool validateTriggerEvent(std::string_view event, std::string& error) {
    ParsedEvent parsed;
    return parseEvent(event, parsed, error);
}

bool validateTriggerActions(std::string_view actions, std::string& error) {
    return Emitter::validateActionsSyntax(actions, error);
}

}  // namespace xt::compile
