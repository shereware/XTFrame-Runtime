#include <vector>

#include "doctest.h"
#include "xt/context.h"
#include "xt/input.h"
#include "xt/types.h"

/// Context::pushInput/pendingInputCount/drainInput and the cursor()/wantsTextInput()/
/// textInputRect() placeholders. No SDL/window needed. See Docs/tasks/T-20.md "測試".

using namespace xt;

TEST_CASE("pushInput queues events in order, drainInput empties the queue") {
    ContextConfig config;
    Context ctx(config);
    CHECK(ctx.pendingInputCount() == 0);

    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{1, 2};
    ctx.pushInput(move);

    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{3, 4};
    down.button = PointerButton::Right;
    ctx.pushInput(down);

    InputEvent up;
    up.kind = InputEvent::PointerUp;
    up.pos = Vec2{5, 6};
    up.button = PointerButton::Right;
    ctx.pushInput(up);

    CHECK(ctx.pendingInputCount() == 3);

    std::vector<InputEvent> drained;
    ctx.drainInput(drained);

    REQUIRE(drained.size() == 3);
    CHECK(drained[0].kind == InputEvent::PointerMove);
    CHECK(drained[0].pos == Vec2{1, 2});
    CHECK(drained[1].kind == InputEvent::PointerDown);
    CHECK(drained[1].pos == Vec2{3, 4});
    CHECK(drained[2].kind == InputEvent::PointerUp);
    CHECK(drained[2].pos == Vec2{5, 6});

    CHECK(ctx.pendingInputCount() == 0);

    std::vector<InputEvent> drainedAgain;
    ctx.drainInput(drainedAgain);
    CHECK(drainedAgain.empty());
}

TEST_CASE("CompositionUpdate's composition string survives drainInput (pooled)") {
    ContextConfig config;
    Context ctx(config);

    InputEvent composition;
    composition.kind = InputEvent::CompositionUpdate;
    // A short string exercises std::string SSO, which is exactly the case that would break if
    // Context pooled these in a std::vector<std::string> (reallocation invalidates SSO buffers).
    std::string text = "ni";
    composition.composition = text;
    composition.compositionCursor = 2;
    composition.compositionSelLen = 0;
    ctx.pushInput(composition);

    // Mutate/destroy the host's original string: the queued copy must not depend on it.
    text.clear();

    std::vector<InputEvent> drained;
    ctx.drainInput(drained);
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].kind == InputEvent::CompositionUpdate);
    CHECK(drained[0].composition == "ni");
    CHECK(drained[0].compositionCursor == 2);
}

TEST_CASE("Context input placeholders") {
    ContextConfig config;
    Context ctx(config);
    CHECK(ctx.cursor() == Cursor::Default);
    CHECK_FALSE(ctx.wantsTextInput());
    CHECK(ctx.textInputRect() == Rect{});
}

TEST_CASE("KeyCode: T-143 keys are appended after Up with increasing values") {
    // Existing values are unchanged (public layout only grows, Docs/design/13 section 13.7).
    CHECK(static_cast<int>(KeyCode::Up) == 82);
    const KeyCode appended[] = {
        KeyCode::F1,
        KeyCode::F2,
        KeyCode::F3,
        KeyCode::F4,
        KeyCode::F5,
        KeyCode::F6,
        KeyCode::F7,
        KeyCode::F8,
        KeyCode::F9,
        KeyCode::F10,
        KeyCode::F11,
        KeyCode::F12,
        KeyCode::PageUp,
        KeyCode::PageDown,
        KeyCode::Insert,
        KeyCode::ShiftLeft,
        KeyCode::ShiftRight,
        KeyCode::ControlLeft,
        KeyCode::ControlRight,
        KeyCode::AltLeft,
        KeyCode::AltRight,
        KeyCode::MetaLeft,
        KeyCode::MetaRight,
        KeyCode::CapsLock,
        KeyCode::Minus,
        KeyCode::Equal,
        KeyCode::BracketLeft,
        KeyCode::BracketRight,
        KeyCode::Backslash,
        KeyCode::Semicolon,
        KeyCode::Quote,
        KeyCode::Backquote,
        KeyCode::Comma,
        KeyCode::Period,
        KeyCode::Slash,
        KeyCode::Numpad0,
        KeyCode::Numpad1,
        KeyCode::Numpad2,
        KeyCode::Numpad3,
        KeyCode::Numpad4,
        KeyCode::Numpad5,
        KeyCode::Numpad6,
        KeyCode::Numpad7,
        KeyCode::Numpad8,
        KeyCode::Numpad9,
        KeyCode::NumpadAdd,
        KeyCode::NumpadSubtract,
        KeyCode::NumpadMultiply,
        KeyCode::NumpadDivide,
        KeyCode::NumpadDecimal,
        KeyCode::NumpadEnter,
    };
    CHECK(sizeof(appended) / sizeof(appended[0]) == 51);
    int previous = static_cast<int>(KeyCode::Up);
    for (KeyCode key : appended) {
        CHECK(static_cast<int>(key) > previous);
        previous = static_cast<int>(key);
    }
    CHECK(static_cast<int>(KeyCode::F1) == 83);
    CHECK(static_cast<int>(KeyCode::NumpadEnter) == 133);
}
