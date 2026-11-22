#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/validate.h"

/// T-844 (Docs/design/27-interaction.md §27.2): `validateTriggerActions()` (the editor's check)
/// reaches the same verdict as a compile for every action form whose names the document resolves,
/// and the compiler's event / action tables describe exactly what it accepts.

using namespace xt;
using namespace xt::compile;

namespace {

/// Where an action list is compiled: some actions are only valid in one event (context, not shape).
enum class Where { Click, Change, OutsidePress, ItemExit };

/// A document that defines every name the corpus refers to (timeline `t` with marker `m`, media `v`,
/// ports `p` / `b` / `s` / `e`, element `A`, symbol `Pop`), so only an action's shape can fail.
std::string actionDoc(std::string_view actions, Where where) {
    const std::string list(actions);
    const char* event = where == Where::Change         ? "change"
                        : where == Where::OutsidePress ? "outsidepress"
                                                       : "click";
    const std::string onA =
        where == Where::ItemExit ? std::string() : " on:" + std::string(event) + "=\"" + list + "\"";
    const std::string exit = where == Where::ItemExit ? " item-exit=\"" + list + "\"" : std::string();
    return R"X(<xt><head>
  <symbol name="Pop"><div id="Box"></div></symbol>
  <style>
    #A { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
    @timeline t { duration: 1s; marker: m 0.5s; #A { 0s { opacity: 0 } 1s { opacity: 1 } } }
  </style>
</head><body id="Root" port:p="number in" port:b="bool in" port:s="string in" port:e="event out" port:items="list<struct{n: number}> in">
  <input id="A" bind:value="s")X" +
           onA + R"X(/>
  <video id="v" src="clip.mp4"></video>
  <repeat port="items" id="R")X" +
           exit + R"X(><div id="Cell"></div></repeat>
</body></xt>)X";
}

/// True when the compile reports no error (warnings, e.g. a missing media file, do not count).
bool compiles(std::string_view actions, Where where, std::string& firstError) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string text = actionDoc(actions, where);  // SourceFile only views it.
    std::vector<SourceFile> files{{"main.xt", text}};
    const CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error) {
            firstError = d.message;
            return false;
        }
    }
    return r.ok();
}

struct Case {
    const char* actions;
    Where where;
    bool valid;  ///< The expected verdict of both the compile and validateTriggerActions().
};

/// Every action of 27.2.3 in its accepted forms (with and without parentheses, each argument and
/// option form) and the shape errors around them.
const Case kCases[] = {
    // play / stop / seek / pause / resume (timeline and media targets).
    {"play(t)", Where::Click, true},
    {"play(t, reverse)", Where::Click, true},
    {"play(t, loop)", Where::Click, true},
    {"play(t, once)", Where::Click, true},
    {"play(t, hold)", Where::Click, true},
    {"play(t, from=m)", Where::Click, true},
    {"play(t, reverse, loop, from=m)", Where::Click, true},
    {"play(v)", Where::Click, true},
    {"play(v, loop)", Where::Click, true},
    {"play", Where::Click, false},
    {"play()", Where::Click, false},
    {"play(t, sideways)", Where::Click, false},
    {"play(t, speed=2)", Where::Click, false},
    {"stop(t)", Where::Click, true},
    {"stop(v)", Where::Click, true},
    {"stop", Where::Click, false},
    {"stop()", Where::Click, false},
    {"stop(t, t)", Where::Click, false},
    {"seek(t, 0.5s)", Where::Click, true},
    {"seek(t, 250ms)", Where::Click, true},
    {"seek(t, m)", Where::Click, true},
    {"seek(v, 1s)", Where::Click, true},
    {"seek(t)", Where::Click, false},
    {"seek(t, 1s, 2s)", Where::Click, false},
    {"pause(t)", Where::Click, true},
    {"resume(t)", Where::Click, true},
    {"pause()", Where::Click, false},
    {"resume(t, t)", Where::Click, false},
    // state / class.
    {"state(Open)", Where::Click, true},
    {"state(Open, on)", Where::Click, true},
    {"state(Open, off)", Where::Click, true},
    {"state(Open, toggle)", Where::Click, true},
    {"state(Open, toggle, target=#A)", Where::Click, true},
    {"state(Disabled)", Where::Click, true},
    {"state()", Where::Click, false},
    {"state", Where::Click, false},
    {"state(Open, maybe)", Where::Click, false},
    {"state(Open, target='A')", Where::Click, false},
    {"class(big)", Where::Click, true},
    {"class(big, toggle)", Where::Click, true},
    {"class()", Where::Click, false},
    {"class(big, maybe)", Where::Click, false},
    // show / hide / consume / reset / remove.
    {"show", Where::Click, true},
    {"hide", Where::Click, true},
    {"show()", Where::Click, true},
    {"hide()", Where::Click, true},
    {"show(x)", Where::Click, false},
    {"hide(1)", Where::Click, false},
    {"consume()", Where::OutsidePress, true},
    {"consume", Where::OutsidePress, false},
    {"consume(x)", Where::OutsidePress, false},
    {"reset(value)", Where::Click, true},
    {"reset", Where::Click, false},
    {"reset()", Where::Click, false},
    {"reset(other)", Where::Click, false},
    {"remove", Where::ItemExit, true},
    {"remove()", Where::ItemExit, true},
    {"show then remove", Where::ItemExit, true},
    {"hide; show then remove", Where::ItemExit, true},
    {"remove(x)", Where::ItemExit, false},
    // sound / haptic / layer / emit / focus.
    {"sound(click)", Where::Click, true},
    {"sound(click, 0.5)", Where::Click, true},
    {"sound", Where::Click, false},
    {"sound()", Where::Click, false},
    {"sound(click, loud)", Where::Click, false},
    {"haptic(buzz)", Where::Click, true},
    {"haptic(buzz, 0.3)", Where::Click, true},
    {"haptic(buzz, strong)", Where::Click, false},
    {"layer(overlay)", Where::Click, true},
    {"layer(normal)", Where::Click, true},
    {"layer(top)", Where::Click, false},
    {"layer()", Where::Click, false},
    {"emit(e)", Where::Click, true},
    {"emit()", Where::Click, false},
    {"emit(e, e)", Where::Click, false},
    {"focus()", Where::Click, true},
    {"focus(A)", Where::Click, true},
    {"focus(#A)", Where::Click, true},
    {"focus(A, A)", Where::Click, false},
    {"focus", Where::Click, false},
    // set.
    {"set(p, 1)", Where::Click, true},
    {"set(p, true)", Where::Click, true},
    {"set(p, false)", Where::Click, true},
    {"set(b, toggle)", Where::Click, true},
    {"set(s, 'x')", Where::Click, true},
    {"set(p, pointer.fraction(#A).x)", Where::Click, true},
    {"set(p, pointer.local(#A).y | scale(2))", Where::Click, true},
    {"set(p, 5 | scale(2))", Where::Click, true},
    {"set(p, port(p))", Where::Click, true},
    {"set(s, value)", Where::Change, true},
    {"set(s, value | upper)", Where::Change, true},
    {"set(p)", Where::Click, false},
    {"set(p, 1, 2)", Where::Click, false},
    {"set('p', 1)", Where::Click, false},
    {"set(p, nope)", Where::Click, false},
    {"set(p, pointer.fraction(A).x)", Where::Click, false},
    {"set(p, pointer.fraction(#A).x |)", Where::Click, false},
    {"set(s, value |)", Where::Change, false},
    {"set(s, valuex)", Where::Change, false},
    // spawn.
    {"spawn(Pop, lifetime=1s)", Where::Click, true},
    {"spawn(Pop, at=self, lifetime=250ms)", Where::Click, true},
    {"spawn(Pop, at=pointer, lifetime=0.5)", Where::Click, true},
    {"spawn()", Where::Click, false},
    {"spawn(Pop)", Where::Click, false},
    {"spawn('', lifetime=1s)", Where::Click, false},
    {"spawn(Pop, lifetime=0s)", Where::Click, false},
    {"spawn(Pop, lifetime=fast)", Where::Click, false},
    {"spawn(Pop, lifetime=1s, speed=2)", Where::Click, false},
    {"spawn(Pop, pointer, lifetime=1s)", Where::Click, false},
    // Unknown names and sequences.
    {"bogus()", Where::Click, false},
    {"bogus", Where::Click, false},
    {"play(t) then hide; sound(click), show", Where::Click, true},
};

}  // namespace

TEST_CASE("T-844: validateTriggerActions accepts remove like the compiler") {
    std::string error;
    CHECK(validateTriggerActions("remove", error));
    CHECK(validateTriggerActions("remove()", error));
    CHECK(validateTriggerActions("play(item-out) then remove", error));
    CHECK_FALSE(validateTriggerActions("remove(x)", error));
}

TEST_CASE("T-844: validateTriggerActions and the compile agree on every action form") {
    for (const Case& c : kCases) {
        const std::string actions = c.actions;
        CAPTURE(actions);
        std::string compileError;
        const bool compiled = compiles(c.actions, c.where, compileError);
        CAPTURE(compileError);
        CHECK(compiled == c.valid);
        std::string validateError;
        const bool validated = validateTriggerActions(c.actions, validateError);
        CAPTURE(validateError);
        CHECK(validated == c.valid);
    }
}

namespace {

/// Sample arguments that name things actionDoc() defines (or need no document).
std::string sampleEvent(std::string pattern) {
    const auto sub = [&](const char* placeholder, const char* value) {
        const size_t at = pattern.find(placeholder);
        if (at != std::string::npos) {
            pattern.replace(at, 3, value);
        }
    };
    const auto starts = [&](const char* prefix) { return pattern.rfind(prefix, 0) == 0; };
    if (starts("press(") || starts("release(") || starts("click(") || starts("dblclick(") ||
        starts("pointermove(") || starts("pointerdown(") || starts("pointerup(")) {
        sub("{0}", "left");
    } else if (starts("key(")) {
        sub("{0}", "Enter");
    } else if (starts("input(")) {
        sub("{0}", "pad.a");  // T-873.
    } else if (starts("port(")) {
        sub("{0}", "p");
    } else if (starts("timeline(")) {
        sub("{0}", "t");
        sub("{1}", "m");
    } else if (starts("state")) {
        sub("{0}", "Open");
    } else if (starts("media(")) {
        sub("{0}", "v");
    } else if (starts("nav.blocked(")) {
        sub("{0}", "up");
    } else if (starts("gesture(")) {
        sub("{0}", "tap");
    } else if (starts("player(")) {
        sub("{0}", "0");
        sub("{1}", "press");
    }
    return pattern;
}

}  // namespace

TEST_CASE("T-844: every entry of the event table is an event the compiler accepts") {
    const std::span<const TriggerEventInfo> table = triggerEventTable();
    REQUIRE(!table.empty());
    for (const TriggerEventInfo& e : table) {
        const std::string pattern = e.pattern;
        CAPTURE(pattern);
        // `args` matches the placeholders of the pattern.
        const int placeholders = (pattern.find("{0}") != std::string::npos ? 1 : 0) +
                                 (pattern.find("{1}") != std::string::npos ? 1 : 0);
        CHECK(placeholders == e.args);
        const std::string sample = sampleEvent(pattern);
        CAPTURE(sample);
        CHECK(sample.find('{') == std::string::npos);
        std::string error;
        CAPTURE(error);
        CHECK(validateTriggerEvent(sample, error));
        // No pattern is listed twice.
        int seen = 0;
        for (const TriggerEventInfo& other : table) {
            seen += pattern == other.pattern ? 1 : 0;
        }
        CHECK(seen == 1);
    }
    // Spellings outside the table stay unknown.
    std::string error;
    CHECK_FALSE(validateTriggerEvent("tap", error));
    CHECK_FALSE(validateTriggerEvent("pointerover", error));
    CHECK_FALSE(validateTriggerEvent("hover(left)", error));
}

TEST_CASE("T-844: every action of the action table is known to the validator, bare as the table says") {
    for (const TriggerActionInfo& a : triggerActionTable()) {
        const std::string name = a.name;
        CAPTURE(name);
        std::string error;
        validateTriggerActions(name + "()", error);
        CHECK(error.find("unknown action") == std::string::npos);
        error.clear();
        const bool bareOk = validateTriggerActions(name, error);
        CAPTURE(error);
        if (a.bare) {
            CHECK(bareOk);
        } else {
            CHECK_FALSE(bareOk);
            CHECK(error.find("expected '('") != std::string::npos);
        }
    }
    std::string error;
    CHECK_FALSE(validateTriggerActions("bogus()", error));
    CHECK(error.find("unknown action") != std::string::npos);
}
