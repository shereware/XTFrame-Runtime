#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#ifdef XT_GOLDEN_TEXT
#include "xt/text/ft_text_shaper.h"
#endif
#include "xt/types.h"

/// UI (`.xt` document) golden fixture loader/runner. See Docs/tasks/T-27.md "黃金圖 UI fixture".
///
/// Not covered by Docs/tasks/T-27.md (conservative choices made here, mirroring golden_lottie.cpp's
/// documented choices for the analogous Lottie loader):
///   - Manifest lines starting with '#' (after trimming leading whitespace) are treated as
///     comments and skipped, as are blank lines; a line with too few fields is skipped with a
///     message to stdout rather than aborting the whole manifest load.
///   - `clearColor` hex parsing accepts a bare 6-digit "RRGGBB" string (no leading '#'); defaults
///     to "ffffff" (opaque white) when omitted, matching the fixture table's own white background.
///   - A compileDocument() warning diagnostic is printed to stdout but does not fail the fixture;
///     only `!result.ok()` (no IR produced) does.
///   - T-30: text fixtures need a real shaper, so this file uses FtTextShaper when xt_text is part
///     of the build (XT_GOLDEN_TEXT) and NullTextShaper otherwise; in the latter case the
///     `ui_text_*` manifest entries are skipped entirely (they would all render as blank pages).
///
/// Event scripts (Docs/tasks/T-33b.md "事件腳本文法"; T-36 uses the same grammar). Not covered
/// by the sheet (conservative choices made here):
///   - The sheet calls the script the manifest's 6th column, but that column already held the
///     optional clear color, so either of the last two columns is accepted: a token ending in
///     ".events" is the script, anything else is the clear color.
///   - `pointerdown`/`pointerup`'s button argument is optional and defaults to `left`; an
///     unknown button name is a script error, like an unknown command.
///   - `key`'s optional `shift`/`ctrl` words may appear in either order; an unknown word is a
///     script error.
///   - `textinput` takes the rest of the line verbatim after exactly one separating space
///     (trailing CR stripped), so it can carry spaces; an empty remainder is a script error.
///   - An `.events` file that cannot be opened fails its fixture, like a script error.
///   - T-170: `theme <name> <file.xts>` reads the `.xts` file (relative to the script's directory),
///     registers its `--name: value` declarations with Context::defineTheme(name) and activates it
///     with Context::setTheme(name), then runs one update(0) like every other command.
///   - T-176: `device <safe> <platform> <input>` calls Context::setDevice() with a uniform safe
///     area (a fraction of the viewport) and runs one update(0).
///   - T-345: `uiscale <factor>` calls Context::setUiScale() and runs one update(0).
///   - T-179: `string <lang> <key> <text...>` adds one entry to the harness's localization table
///     (the text is the rest of the line after the key, verbatim, UTF-8); `lang <tag>` binds that
///     table with Context::setLocalizationProvider(), calls Context::setLanguage(tag) and runs one
///     update(0). The table is emptied before every fixture.
///   - T-194: `pad <player> <A|B|LB|RB|Up|Down|Left|Right>` presses (down + up) a gamepad
///     button of that local player, one update(0) after each half.
///   - T-174: `state <id> <Name> [on|off]` sets a custom state (Context::setCustomState, default
///     `on`) on the element with that id, then runs one update(0).
///   - T-178: `glyph <action> <asset>` maps `<glyph key="action">` to the IR image asset named
///     `asset` (for every input device) in a map-backed IGlyphProvider installed with
///     Context::setGlyphProvider(), and runs one update(0).
///   - T-175: `port <name> <value>` sets a declared port from text (Document::lookupPort +
///     setPortText; the value is the rest of the line), then runs one update(0).
///   - T-177: `port <list> [{...}, ...]` sets a list port from a JSON array (setPortText);
///     `listinsert <list> <index> {"field": value, ...}` inserts one entry (a flat JSON object:
///     strings, numbers, true/false; a color field takes "#rrggbb") and `listremove <list> <index>`
///     removes one, each followed by one update(0).

namespace xt::golden {

namespace {

namespace fs = std::filesystem;

xt::Color parseClearColorHex(const std::string& hex) {
    unsigned int value = 0;
    std::istringstream iss(hex);
    iss >> std::hex >> value;
    const float r = static_cast<float>((value >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((value >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(value & 0xFFu) / 255.0f;
    return xt::Color{r, g, b, 1.0f};
}

/// T-178: the event scripts' `glyph` command provider: action -> asset name, any input device.
struct ScriptGlyphProvider : xt::IGlyphProvider {
    std::map<std::string, std::string, std::less<>> map;
    xt::AssetRef resolve(std::string_view action, xt::InputKind) override {
        auto it = map.find(action);
        return it != map.end() ? xt::AssetRef{it->second} : xt::AssetRef{};
    }
};

ScriptGlyphProvider& scriptGlyphProvider() {
    static ScriptGlyphProvider provider;
    return provider;
}

bool isBlankOrComment(const std::string& line) {
    const size_t start = line.find_first_not_of(" \t\r\n");
    return start == std::string::npos || line[start] == '#';
}

// -------------------------------------------------------------------------------------------
// Event script (Docs/tasks/T-33b.md "事件腳本文法").
// -------------------------------------------------------------------------------------------

/// T-179: the `string`/`lang` commands' table. Static so it outlives every fixture's Context;
/// renderUiFixture() empties it first.
class ScriptLocalization : public xt::ILocalizationProvider {
public:
    std::map<std::pair<std::string, std::string>, std::string> table;

    std::string_view resolve(std::string_view key, std::string_view lang) override {
        auto it = table.find({std::string(lang), std::string(key)});
        return it == table.end() ? std::string_view{} : std::string_view(it->second);
    }
};

ScriptLocalization& scriptLocalization() {
    static ScriptLocalization instance;
    return instance;
}

/// One parsed script line: the command word, its whitespace-separated arguments, and the raw
/// remainder after the command (which `textinput` uses verbatim).
struct ScriptLine {
    int number = 0;
    std::string command;
    std::vector<std::string> args;
    std::string rest;
};

struct KeyName {
    const char* name;
    xt::KeyCode code;
};

/// Every KeyCode enumerator of Code/Runtime/core/include/xt/input.h, by its own name.
constexpr KeyName kKeyNames[] = {
    {"Unknown", xt::KeyCode::Unknown}, {"KeyA", xt::KeyCode::KeyA},
    {"KeyB", xt::KeyCode::KeyB},       {"KeyC", xt::KeyCode::KeyC},
    {"KeyD", xt::KeyCode::KeyD},       {"KeyE", xt::KeyCode::KeyE},
    {"KeyF", xt::KeyCode::KeyF},       {"KeyG", xt::KeyCode::KeyG},
    {"KeyH", xt::KeyCode::KeyH},       {"KeyI", xt::KeyCode::KeyI},
    {"KeyJ", xt::KeyCode::KeyJ},       {"KeyK", xt::KeyCode::KeyK},
    {"KeyL", xt::KeyCode::KeyL},       {"KeyM", xt::KeyCode::KeyM},
    {"KeyN", xt::KeyCode::KeyN},       {"KeyO", xt::KeyCode::KeyO},
    {"KeyP", xt::KeyCode::KeyP},       {"KeyQ", xt::KeyCode::KeyQ},
    {"KeyR", xt::KeyCode::KeyR},       {"KeyS", xt::KeyCode::KeyS},
    {"KeyT", xt::KeyCode::KeyT},       {"KeyU", xt::KeyCode::KeyU},
    {"KeyV", xt::KeyCode::KeyV},       {"KeyW", xt::KeyCode::KeyW},
    {"KeyX", xt::KeyCode::KeyX},       {"KeyY", xt::KeyCode::KeyY},
    {"KeyZ", xt::KeyCode::KeyZ},       {"Key1", xt::KeyCode::Key1},
    {"Key2", xt::KeyCode::Key2},       {"Key3", xt::KeyCode::Key3},
    {"Key4", xt::KeyCode::Key4},       {"Key5", xt::KeyCode::Key5},
    {"Key6", xt::KeyCode::Key6},       {"Key7", xt::KeyCode::Key7},
    {"Key8", xt::KeyCode::Key8},       {"Key9", xt::KeyCode::Key9},
    {"Key0", xt::KeyCode::Key0},       {"Enter", xt::KeyCode::Enter},
    {"Escape", xt::KeyCode::Escape},   {"Backspace", xt::KeyCode::Backspace},
    {"Tab", xt::KeyCode::Tab},         {"Space", xt::KeyCode::Space},
    {"Delete", xt::KeyCode::Delete},   {"Home", xt::KeyCode::Home},
    {"End", xt::KeyCode::End},         {"Right", xt::KeyCode::Right},
    {"Left", xt::KeyCode::Left},       {"Down", xt::KeyCode::Down},
    {"Up", xt::KeyCode::Up},
};

bool lookupKey(const std::string& name, xt::KeyCode& out) {
    for (const KeyName& entry : kKeyNames) {
        if (name == entry.name) {
            out = entry.code;
            return true;
        }
    }
    return false;
}

bool lookupButton(const std::string& name, xt::PointerButton& out) {
    if (name == "left") {
        out = xt::PointerButton::Left;
        return true;
    }
    if (name == "right") {
        out = xt::PointerButton::Right;
        return true;
    }
    if (name == "middle") {
        out = xt::PointerButton::Middle;
        return true;
    }
    return false;
}

bool parseFloatArg(const std::string& text, float& out) {
    std::istringstream iss(text);
    iss >> out;
    return !iss.fail() && iss.eof();
}

/// Pushes `event` and immediately runs one zero-length update, so the event is dispatched in its
/// own frame (Docs/tasks/T-33b.md: "每個非 update 行都 pushInput 後立刻 update(0)").
void pushAndStep(xt::Context& ctx, const xt::InputEvent& event) {
    ctx.pushInput(event);
    ctx.update(0.0f);
}

void pushPointer(xt::Context& ctx, xt::InputEvent::Kind kind, float x, float y, xt::PointerButton button) {
    xt::InputEvent ev;
    ev.kind = kind;
    ev.pos = xt::Vec2{x, y};
    ev.button = button;
    pushAndStep(ctx, ev);
}

/// Splits `line` into a ScriptLine. Returns false for a blank/comment line.
bool parseScriptLine(const std::string& line, int number, ScriptLine& out) {
    std::string text = line;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos || text[start] == '#') {
        return false;
    }
    size_t end = text.find_first_of(" \t", start);
    out.number = number;
    out.command = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    out.args.clear();
    out.rest.clear();
    if (end == std::string::npos) {
        return true;
    }
    out.rest = text.substr(end + 1);
    std::istringstream iss(text.substr(end));
    std::string token;
    while (iss >> token) {
        out.args.push_back(token);
    }
    return true;
}

/// Runs one parsed line. `err` gets a bare message (no file/line prefix) on failure. `scriptDir` is
/// the script's own directory (T-170's `theme` command resolves its file against it).
/// T-177: `{"a": "text", "b": 1.5, "c": true, "d": "#rrggbb"}` -> struct field names and values,
/// typed by the list port's entry fields (a flat object only).
bool flatJsonEntry(xt::Document& doc, xt::PortId port, const std::string& text,
                   std::vector<xt::StringId>& names, std::vector<xt::Value>& values) {
    const xt::PortRecord info = doc.portInfo(port);
    size_t i = 0;
    auto ws = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
    };
    auto str = [&](std::string& out) {
        ws();
        if (i >= text.size() || text[i] != '"') {
            return false;
        }
        size_t end = text.find('"', i + 1);
        if (end == std::string::npos) {
            return false;
        }
        out = text.substr(i + 1, end - i - 1);
        i = end + 1;
        return true;
    };
    ws();
    if (i >= text.size() || text[i++] != '{') {
        return false;
    }
    for (;;) {
        std::string key;
        if (!str(key)) {
            return false;
        }
        ws();
        if (i >= text.size() || text[i++] != ':') {
            return false;
        }
        ws();
        xt::PortValueType type = xt::PortValueType::Number;
        bool known = false;
        for (const auto& [name, t] : info.fields) {
            if (name == key) {
                type = t;
                known = true;
            }
        }
        xt::Value v;
        if (i < text.size() && text[i] == '"') {
            std::string s;
            if (!str(s)) {
                return false;
            }
            if (type == xt::PortValueType::Color) {
                char* hexEnd = nullptr;
                const unsigned long rgb =
                    s.size() == 7 && s[0] == '#' ? std::strtoul(s.c_str() + 1, &hexEnd, 16) : 0;
                if (hexEnd != s.c_str() + s.size()) {
                    return false;
                }
                v = xt::Value::ofColor(xt::Color{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                                                 static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                                                 static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f});
            } else {
                v = xt::Value::ofStringId(doc.intern(s));
            }
        } else if (text.compare(i, 4, "true") == 0 || text.compare(i, 5, "false") == 0) {
            const bool on = text.compare(i, 4, "true") == 0;
            i += on ? 4 : 5;
            v = xt::Value::ofFloat(on ? 1.0f : 0.0f);
        } else {
            char* end = nullptr;
            const float f = std::strtof(text.c_str() + i, &end);
            if (end == text.c_str() + i) {
                return false;
            }
            i = static_cast<size_t>(end - text.c_str());
            v = xt::Value::ofFloat(f);
        }
        if (known) {
            names.push_back(doc.intern(key));
            values.push_back(v);
        }
        ws();
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        return i < text.size() && text[i] == '}';
    }
}

bool runScriptLine(xt::Context& ctx, xt::Document& doc, const ScriptLine& line, const fs::path& scriptDir,
                   std::string& err) {
    const std::vector<std::string>& args = line.args;
    const std::string& cmd = line.command;

    auto wrongArity = [&err, &cmd]() {
        err = "wrong number of arguments for '" + cmd + "'";
        return false;
    };
    auto badNumber = [&err, &cmd]() {
        err = "'" + cmd + "': expected a number";
        return false;
    };

    if (cmd == "focus") {
        if (args.size() != 1) {
            return wrongArity();
        }
        std::vector<xt::ElementRef> hits;
        if (doc.query(args[0], hits) != xt::Status::kOk || hits.empty()) {
            err = "focus: no element matches '" + args[0] + "'";
            return false;
        }
        hits[0].focus();
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "blur") {
        if (!args.empty()) {
            return wrongArity();
        }
        doc.setFocus(xt::ElementRef{});
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "pointermove" || cmd == "pointerdown" || cmd == "pointerup") {
        bool moveOnly = cmd == "pointermove";
        size_t maxArgs = moveOnly ? 2u : 3u;
        if (args.size() < 2 || args.size() > maxArgs) {
            return wrongArity();
        }
        float x = 0;
        float y = 0;
        if (!parseFloatArg(args[0], x) || !parseFloatArg(args[1], y)) {
            return badNumber();
        }
        xt::PointerButton button = xt::PointerButton::Left;
        if (args.size() == 3 && !lookupButton(args[2], button)) {
            err = "unknown pointer button '" + args[2] + "'";
            return false;
        }
        xt::InputEvent::Kind kind = moveOnly               ? xt::InputEvent::PointerMove
                                    : cmd == "pointerdown" ? xt::InputEvent::PointerDown
                                                           : xt::InputEvent::PointerUp;
        pushPointer(ctx, kind, x, y, button);
        return true;
    }
    if (cmd == "click") {
        if (args.size() != 2) {
            return wrongArity();
        }
        float x = 0;
        float y = 0;
        if (!parseFloatArg(args[0], x) || !parseFloatArg(args[1], y)) {
            return badNumber();
        }
        pushPointer(ctx, xt::InputEvent::PointerMove, x, y, xt::PointerButton::Left);
        pushPointer(ctx, xt::InputEvent::PointerDown, x, y, xt::PointerButton::Left);
        pushPointer(ctx, xt::InputEvent::PointerUp, x, y, xt::PointerButton::Left);
        return true;
    }
    if (cmd == "wheel") {
        if (args.size() != 4) {
            return wrongArity();
        }
        float v[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < 4; ++i) {
            if (!parseFloatArg(args[i], v[i])) {
                return badNumber();
            }
        }
        xt::InputEvent ev;
        ev.kind = xt::InputEvent::Wheel;
        ev.pos = xt::Vec2{v[0], v[1]};
        ev.wheelDelta = xt::Vec2{v[2], v[3]};
        pushAndStep(ctx, ev);
        return true;
    }
    if (cmd == "key") {
        if (args.empty() || args.size() > 3) {
            return wrongArity();
        }
        xt::KeyCode code = xt::KeyCode::Unknown;
        if (!lookupKey(args[0], code)) {
            err = "unknown key name '" + args[0] + "'";
            return false;
        }
        uint8_t modifiers = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "shift") {
                modifiers = static_cast<uint8_t>(modifiers | xt::kShift);
            } else if (args[i] == "ctrl") {
                modifiers = static_cast<uint8_t>(modifiers | xt::kCtrl);
            } else {
                err = "unknown key modifier '" + args[i] + "'";
                return false;
            }
        }
        xt::InputEvent down;
        down.kind = xt::InputEvent::KeyDown;
        down.key = code;
        down.modifiers = modifiers;
        pushAndStep(ctx, down);
        xt::InputEvent up;
        up.kind = xt::InputEvent::KeyUp;
        up.key = code;
        up.modifiers = modifiers;
        pushAndStep(ctx, up);
        return true;
    }
    if (cmd == "textinput") {
        if (line.rest.empty()) {
            return wrongArity();
        }
        // One TextInput event per code point (the grammar's "每個 code point 一個 TextInput 事件").
        const std::string& utf8 = line.rest;
        size_t i = 0;
        while (i < utf8.size()) {
            size_t len = 1;
            while (i + len < utf8.size() && (static_cast<unsigned char>(utf8[i + len]) & 0xC0u) == 0x80u) {
                ++len;
            }
            xt::InputEvent ev;
            ev.kind = xt::InputEvent::TextInput;
            std::memcpy(ev.utf8, utf8.data() + i, len);
            ev.utf8Len = static_cast<uint8_t>(len);
            pushAndStep(ctx, ev);
            i += len;
        }
        return true;
    }
    if (cmd == "compstart") {
        if (!args.empty()) {
            return wrongArity();
        }
        xt::InputEvent ev;
        ev.kind = xt::InputEvent::CompositionStart;
        pushAndStep(ctx, ev);
        return true;
    }
    if (cmd == "compupdate") {
        if (args.size() != 3) {
            return wrongArity();
        }
        float cursor = 0;
        float selLen = 0;
        if (!parseFloatArg(args[1], cursor) || !parseFloatArg(args[2], selLen)) {
            return badNumber();
        }
        xt::InputEvent ev;
        ev.kind = xt::InputEvent::CompositionUpdate;
        ev.composition = args[0];  // Context::pushInput copies it into its own pool.
        ev.compositionCursor = static_cast<uint32_t>(cursor);
        ev.compositionSelLen = static_cast<uint32_t>(selLen);
        pushAndStep(ctx, ev);
        return true;
    }
    if (cmd == "compend") {
        if (!args.empty()) {
            return wrongArity();
        }
        xt::InputEvent ev;
        ev.kind = xt::InputEvent::CompositionEnd;
        pushAndStep(ctx, ev);
        return true;
    }
    if (cmd == "placeholders") {
        // T-191: unbound videos / external textures draw their placeholder (xt_shell's mode).
        if (!args.empty()) {
            return wrongArity();
        }
        ctx.setExternalPlaceholders(true);
        return true;
    }
    if (cmd == "play") {
        // T-172: `play <timeline>` starts a root-scope @timeline (no update of its own; the
        // fixture's `time` then advances it).
        if (args.size() != 1) {
            return wrongArity();
        }
        if (ctx.playTimeline("", args[0]) != xt::Status::kOk) {
            err = "'play': no @timeline '" + args[0] + "'";
            return false;
        }
        return true;
    }
    if (cmd == "theme") {
        if (args.size() != 2) {
            return wrongArity();
        }
        std::ifstream themeFile(scriptDir / args[1], std::ios::binary);
        if (!themeFile.good()) {
            err = "'theme': could not open '" + args[1] + "'";
            return false;
        }
        std::stringstream text;
        text << themeFile.rdbuf();
        xt::compile::DiagnosticSink sink;
        xt::compile::RuleSet rules;
        xt::compile::parseXts(text.str(), args[1], 1, sink, rules);
        std::vector<xt::TokenDef> tokens;
        for (const xt::compile::TokenDecl& token : rules.tokens) {
            tokens.push_back(xt::TokenDef{token.name, token.value});
        }
        if (ctx.defineTheme(args[0], tokens) != xt::Status::kOk || ctx.setTheme(args[0]) != xt::Status::kOk) {
            err = "'theme': could not activate '" + args[0] + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "string") {
        // T-179: `string <lang> <key> <text...>`.
        if (args.size() < 3) {
            return wrongArity();
        }
        const size_t langAt = line.rest.find(args[0]);
        const size_t keyAt = line.rest.find(args[1], langAt + args[0].size());
        size_t textAt = line.rest.find_first_not_of(" \t", keyAt + args[1].size());
        scriptLocalization().table[{args[0], args[1]}] = line.rest.substr(textAt);
        return true;
    }
    if (cmd == "lang") {
        // T-179: `lang <tag>`.
        if (args.size() != 1) {
            return wrongArity();
        }
        ctx.setLocalizationProvider(&scriptLocalization());
        if (ctx.setLanguage(args[0]) != xt::Status::kOk) {
            err = "'lang': invalid language '" + args[0] + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "device") {
        // T-176: `device <safe fraction> <desktop|console|handheld> <mouse|gamepad|touch>` calls
        // Context::setDevice() (the aspect comes from the fixture's own size) and runs update(0).
        if (args.size() != 3) {
            return wrongArity();
        }
        float safe = 0;
        if (!parseFloatArg(args[0], safe)) {
            return badNumber();
        }
        xt::DeviceInfo device;
        device.safeArea = xt::Insets{safe, safe, safe, safe};
        const char* platforms[] = {"desktop", "console", "handheld"};
        const char* inputs[] = {"mouse", "gamepad", "touch"};
        bool platformOk = false;
        bool inputOk = false;
        for (int i = 0; i < 3; ++i) {
            if (args[1] == platforms[i]) {
                device.platform = static_cast<xt::DeviceKind>(i);
                platformOk = true;
            }
            if (args[2] == inputs[i]) {
                device.input = static_cast<xt::InputKind>(i);
                inputOk = true;
            }
        }
        if (!platformOk || !inputOk || ctx.setDevice(device) != xt::Status::kOk) {
            err = "'device': invalid platform, input or safe area";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "uiscale") {
        // T-345: `uiscale <factor>` calls Context::setUiScale() and runs update(0).
        float scale = 0;
        if (args.size() != 1) {
            return wrongArity();
        }
        if (!parseFloatArg(args[0], scale) || ctx.setUiScale(scale) != xt::Status::kOk) {
            return badNumber();
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "pad") {
        // T-194: `pad <player> <button>` (see the file comment).
        static const std::pair<const char*, xt::GamepadButton> kButtons[] = {
            {"A", xt::GamepadButton::A},
            {"B", xt::GamepadButton::B},
            {"LB", xt::GamepadButton::LeftShoulder},
            {"RB", xt::GamepadButton::RightShoulder},
            {"Up", xt::GamepadButton::DpadUp},
            {"Down", xt::GamepadButton::DpadDown},
            {"Left", xt::GamepadButton::DpadLeft},
            {"Right", xt::GamepadButton::DpadRight},
        };
        float player = 0;
        if (args.size() != 2 || !parseFloatArg(args[0], player) || player < 0 || player > 7) {
            return wrongArity();
        }
        for (const auto& b : kButtons) {
            if (args[1] == b.first) {
                xt::InputEvent ev;
                ev.kind = xt::InputEvent::GamepadButtonDown;
                ev.gamepadButton = b.second;
                ev.player = static_cast<uint8_t>(player);
                pushAndStep(ctx, ev);
                ev.kind = xt::InputEvent::GamepadButtonUp;
                pushAndStep(ctx, ev);
                return true;
            }
        }
        err = "'pad': unknown button '" + args[1] + "'";
        return false;
    }
    if (cmd == "state") {
        if (args.size() < 2 || args.size() > 3 || (args.size() == 3 && args[2] != "on" && args[2] != "off")) {
            return wrongArity();
        }
        xt::ElementRef ref = doc.byId(args[0]);
        if (!ref.valid() ||
            ctx.setCustomState(ref, args[1], args.size() < 3 || args[2] == "on") != xt::Status::kOk) {
            err = "'state': cannot set '" + args[1] + "' on '#" + args[0] + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "glyph") {
        // T-178: `glyph <action> <asset>` (see the file comment).
        if (args.size() != 2) {
            return wrongArity();
        }
        scriptGlyphProvider().map[args[0]] = args[1];
        ctx.setGlyphProvider(&scriptGlyphProvider());
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "port") {
        if (args.size() < 2) {
            return wrongArity();
        }
        std::string value = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            value += " " + args[i];
        }
        xt::PortId port = doc.lookupPort(args[0]);
        if (port == xt::kInvalidPort || doc.setPortText(port, value) != xt::Status::kOk) {
            err = "'port': cannot set '" + args[0] + "' to '" + value + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "listinsert" || cmd == "listremove") {
        // T-177 (see the file comment).
        if (args.size() < (cmd == "listinsert" ? 3u : 2u)) {
            return wrongArity();
        }
        xt::PortId port = doc.lookupPort(args[0]);
        float indexF = 0;
        if (!parseFloatArg(args[1], indexF) || indexF < 0) {
            return badNumber();
        }
        const uint32_t index = static_cast<uint32_t>(indexF);
        xt::Status status = xt::Status::kNotFound;
        if (cmd == "listremove") {
            status = doc.listRemove(port, index);
        } else {
            std::string text = args[2];
            for (size_t i = 3; i < args.size(); ++i) {
                text += " " + args[i];
            }
            std::vector<xt::StringId> names;
            std::vector<xt::Value> values;
            if (!flatJsonEntry(doc, port, text, names, values)) {
                err = "'listinsert': bad entry '" + text + "'";
                return false;
            }
            status = doc.listInsert(
                port, index,
                xt::Value::ofStruct(names.data(), values.data(), static_cast<uint32_t>(values.size())));
        }
        if (status != xt::Status::kOk) {
            err = "'" + cmd + "': failed on '" + args[0] + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (cmd == "update") {
        if (args.size() != 1) {
            return wrongArity();
        }
        float seconds = 0;
        if (!parseFloatArg(args[0], seconds)) {
            return badNumber();
        }
        ctx.update(seconds);
        return true;
    }
    err = "unknown command '" + cmd + "'";
    return false;
}

}  // namespace

bool runEventScript(xt::Context& ctx, xt::Document& doc, const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in.good()) {
        err = path + ": could not open event script";
        return false;
    }
    std::string raw;
    int number = 0;
    while (std::getline(in, raw)) {
        ++number;
        ScriptLine line;
        if (!parseScriptLine(raw, number, line)) {
            continue;
        }
        std::string message;
        if (!runScriptLine(ctx, doc, line, fs::path(path).parent_path(), message)) {
            err = path + ":" + std::to_string(number) + ": " + message;
            return false;
        }
    }
    return true;
}

namespace {}  // namespace

std::vector<UiFixture> loadUiManifest(const std::string& manifestPath) {
    std::vector<UiFixture> out;
    std::ifstream in(manifestPath);
    if (!in.good()) {
        report("xt_golden: could not open UI manifest '%s'\n", manifestPath.c_str());
        noteManifestError();
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (isBlankOrComment(line)) {
            continue;
        }
        std::istringstream iss(line);
        UiFixture fx;
        if (!(iss >> fx.name >> fx.xtPath >> fx.width >> fx.height >> fx.time)) {
            report("xt_golden: malformed UI manifest line: '%s'\n", line.c_str());
            noteManifestError();
            continue;
        }
#ifndef XT_GOLDEN_TEXT
        if (fx.name.rfind("ui_text_", 0) == 0 || fx.name.rfind("ui_ime_", 0) == 0) {
            continue;  // No text backend in this build: see the file header.
        }
#endif
        fx.clearColor = xt::Color{1, 1, 1, 1};
        std::string token;
        while (iss >> token) {
            if (token == "gl-only") {
                fx.glOnly = true;  // T-203
            } else if (token.size() > 7 && token.compare(token.size() - 7, 7, ".events") == 0) {
                fx.eventsPath = token;
            } else {
                fx.clearColor = parseClearColorHex(token);
            }
        }
        out.push_back(std::move(fx));
    }
    return out;
}

std::vector<uint8_t> renderUiFixture(const UiFixture& fx, const std::string& uiDir) {
    return renderUiFixture(fx, uiDir,
                           [](const UiFixture& f, const xt::CommandList& cmds, xt::TextShaper* shaper,
                              xt::AssetResolver* assets, const xt::IrView* ir) {
                               std::vector<uint8_t> buf(
                                   static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 4, 0);
                               xt::sw::SwRenderer renderer;
                               xt::sw::SwTarget target{buf.data(), f.width, f.height, f.width * 4};
                               renderer.setTarget(target);
                               renderer.clear(f.clearColor);
                               renderer.render(cmds, shaper, assets, ir);
                               return buf;
                           });
}

std::vector<uint8_t> renderUiFixture(const UiFixture& fx, const std::string& uiDir,
                                     const UiBackend& backend) {
    const fs::path xtPath = fs::path(uiDir) / fx.xtPath;

    xt::compile::CompileOptions options;
    options.rootDir = xtPath.parent_path().string();
    options.strict = false;

    xt::compile::CompileResult result = xt::compile::compileDocument(xtPath.string(), options);
    for (const xt::compile::Diagnostic& d : result.diagnostics) {
        const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
        report("xt_golden: ui fixture '%s': %s: %s\n", fx.name.c_str(), level, d.message.c_str());
    }
    if (!result.ok()) {
        report("xt_golden: ui fixture '%s' failed to compile ('%s')\n", fx.name.c_str(),
               xtPath.string().c_str());
        return {};
    }

#ifdef XT_GOLDEN_TEXT
    xt::text::FtTextShaper shaper;
#else
    xt::NullTextShaper shaper;
#endif
    scriptLocalization().table.clear();  // T-179.
    xt::FileAssetResolver resolver(xtPath.parent_path().string(), xt::sw::decodeImageBytes);
    xt::Context ctx(xt::ContextConfig{&shaper, &resolver});
    ctx.setViewport(xt::Rect{0, 0, static_cast<float>(fx.width), static_cast<float>(fx.height)}, 1.0f);

    xt::Result<xt::DocumentHandle> handle =
        ctx.loadDocument(xt::IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
    if (!handle.ok()) {
        report("xt_golden: ui fixture '%s' failed to load (status=%s)\n", fx.name.c_str(),
               xt::statusName(handle.status()));
        return {};
    }

    xt::Document* doc = ctx.document(handle.value());
    if (doc == nullptr) {
        report("xt_golden: ui fixture '%s': loaded document not found\n", fx.name.c_str());
        return {};
    }

    // Docs/tasks/T-33b.md "事件腳本文法": update(0) -> script -> update(time) -> render. With no
    // script and time == 0 this is the single update(0) every pre-T-33b fixture already ran.
    ctx.update(0.0f);
    if (!fx.eventsPath.empty()) {
        std::string err;
        if (!runEventScript(ctx, *doc, (fs::path(uiDir) / fx.eventsPath).string(), err)) {
            report("xt_golden: %s\n", err.c_str());
            return {};
        }
    }
    if (fx.time != 0.0f) {
        ctx.update(fx.time);
    }

    xt::CommandList cmds;
    ctx.render(cmds);
    return backend(fx, cmds, &shaper, &resolver, &doc->ir());
}

}  // namespace xt::golden
