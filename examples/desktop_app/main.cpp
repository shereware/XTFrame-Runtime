#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "example_args.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/shell/app.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

#ifndef XT_DESKTOP_APP_UI_DIR
#error "XT_DESKTOP_APP_UI_DIR must be defined by examples/CMakeLists.txt"
#endif

/// desktop_app: a small interactive `xt_shell::App` host -- a button counter, a text input whose
/// Change event is echoed, a checkbox that toggles a `dark` class on the root element, and a
/// `:hover` transition on the button. See ui/app.xt / ui/app.xts for the document and
/// docs/design/13-api.md §13.5 for the `App` API this example drives.
///
/// Usage: desktop_app [--headless] [--frames N] [--width W] [--height H]
///
/// Without `--headless`, this opens a real window and runs until it is closed (Escape or the
/// window's close button), exiting 0. When `--headless` is given, or when `App` fails to create a
/// window/GL context (e.g. no display available, docs/tasks/T-20.md "app.h"), this falls back to
/// driving a plain `xt::Context` for `--frames` update/render iterations with no window at all,
/// simulating the same button click / checkbox toggle / text entry through `pushInput` and the
/// element API so the whole UI wiring below still runs. Exit codes: 0 success (prints "done"), 1
/// UI compile error, 2 argument error.

namespace {

void printUsage() {
    std::fprintf(stderr, "usage: desktop_app [--headless] [--frames N] [--width W] [--height H]\n");
}

/// Wires this document's event listeners (docs/design/08-events.md §8.3): the counter button, the
/// text input's echo, and the dark-mode checkbox. Shared between the interactive (`App`) and
/// headless (`Context`) code paths below since `Document`'s API is identical either way.
void wireUi(xt::Document& doc) {
    xt::ElementRef root = doc.byId("root");
    xt::ElementRef incButton = doc.byId("inc");
    xt::ElementRef countSpan = doc.byId("count");
    xt::ElementRef nameInput = doc.byId("name");
    xt::ElementRef echoSpan = doc.byId("echo");
    xt::ElementRef darkToggle = doc.byId("darkToggle");

    if (incButton.valid() && countSpan.valid()) {
        auto counter = std::make_shared<int>(0);
        doc.addListener(incButton, xt::EventType::Click, [countSpan, counter](xt::Event&) mutable {
            ++*counter;
            countSpan.setText(std::to_string(*counter));
        });
    }

    if (nameInput.valid() && echoSpan.valid()) {
        doc.addListener(nameInput, xt::EventType::Change,
                        [nameInput, echoSpan](xt::Event&) mutable { echoSpan.setText(nameInput.value()); });
    }

    if (darkToggle.valid() && root.valid()) {
        doc.addListener(darkToggle, xt::EventType::Change,
                        [root](xt::Event&) mutable { root.toggleClass("dark"); });
    }
}

/// Compiles ui/app.xt (at XT_DESKTOP_APP_UI_DIR, set by CMakeLists.txt to this source directory)
/// into an in-memory `.xtir` blob. Returns an empty vector on failure (diagnostics already
/// printed to stderr).
std::vector<uint8_t> compileUi() {
    const std::string uiDir = XT_DESKTOP_APP_UI_DIR;
    xt::compile::CompileOptions options;
    options.rootDir = uiDir;
    xt::compile::CompileResult result = xt::compile::compileDocument(uiDir + "/app.xt", options);
    for (const xt::compile::Diagnostic& d : result.diagnostics) {
        const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
        std::fprintf(stderr, "desktop_app: %s: %s\n", level, d.message.c_str());
    }
    if (!result.ok()) {
        std::fprintf(stderr, "desktop_app: failed to compile ui/app.xt\n");
        return {};
    }
    return result.ir;
}

/// The `--headless` (or App-creation-failed) fallback: drives a plain Context directly, no
/// window, simulating one click on #inc, one checkbox toggle and one text entry so the wiring in
/// wireUi() above is exercised even without a real display, then runs the remaining frames.
int runHeadless(const std::vector<uint8_t>& ir, int width, int height, uint32_t frames) {
    xt::NullTextShaper shaper;
    xt::FileAssetResolver assets(std::string(XT_DESKTOP_APP_UI_DIR), xt::sw::decodeImageBytes);
    xt::Context ctx(xt::ContextConfig{&shaper, &assets});

    xt::Result<xt::DocumentHandle> loadResult =
        ctx.loadDocument(xt::IrBlob{ir.data(), ir.size()}, /*takeOwnership=*/true);
    if (!loadResult.ok()) {
        std::fprintf(stderr, "desktop_app: failed to load compiled ui (%s)\n",
                     xt::statusName(loadResult.status()));
        return 1;
    }
    xt::DocumentHandle handle = loadResult.value();
    ctx.setViewport(xt::Rect{0, 0, static_cast<float>(width), static_cast<float>(height)}, 1.0f);

    xt::Document* doc = ctx.document(handle);
    if (doc == nullptr) {
        return 1;
    }
    wireUi(*doc);

    // A first update() lays out the document so boundingBox() below is meaningful.
    ctx.update(0.0f);

    xt::ElementRef incButton = doc->byId("inc");
    xt::ElementRef darkToggle = doc->byId("darkToggle");
    xt::ElementRef nameInput = doc->byId("name");

    auto click = [&](xt::ElementRef target) {
        if (!target.valid()) {
            return;
        }
        xt::Rect box = target.boundingBox();
        xt::Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
        xt::InputEvent down;
        down.kind = xt::InputEvent::PointerDown;
        down.pos = center;
        down.button = xt::PointerButton::Left;
        ctx.pushInput(down);
        xt::InputEvent up = down;
        up.kind = xt::InputEvent::PointerUp;
        ctx.pushInput(up);
    };

    click(incButton);
    click(darkToggle);
    if (nameInput.valid()) {
        // Directly set the value rather than simulating individual key events: setValue() itself
        // dispatches the same (bubbling) Change event a real keystroke sequence would end with
        // (docs/design/13-api.md §13.3).
        nameInput.setValue("headless");
    }

    const float dt = 1.0f / 60.0f;
    xt::CommandList list;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ctx.update(frame == 0 ? 0.0f : dt);
        list.clear();
        ctx.render(list);
    }

    std::printf("desktop_app: headless run complete, count=%s, echo=%s\n",
                std::string(doc->byId("count").text()).c_str(), std::string(nameInput.value()).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    xt::examples::CommonArgs args;
    args.width = 640;
    args.height = 480;
    std::vector<std::string> remaining;
    if (!xt::examples::parseCommonArgs(argc, argv, args, remaining) || !remaining.empty()) {
        printUsage();
        return 2;
    }
    if (args.frames == 0) {
        args.frames = 1;
    }

    std::vector<uint8_t> ir = compileUi();
    if (ir.empty()) {
        return 1;
    }

    if (!args.headless) {
        xt::shell::WindowConfig windowConfig;
        windowConfig.width = args.width;
        windowConfig.height = args.height;
        windowConfig.title = "XTFrame desktop_app example";
        xt::shell::App app(windowConfig);
        if (app.ok()) {
            app.setAssetRoot(XT_DESKTOP_APP_UI_DIR);
            app.addSystemFonts();  // Text needs at least one registered font (07.2).
            xt::Result<xt::DocumentHandle> loadResult =
                app.context().loadDocument(xt::IrBlob{ir.data(), ir.size()}, /*takeOwnership=*/true);
            if (!loadResult.ok()) {
                std::fprintf(stderr, "desktop_app: failed to load compiled ui (%s)\n",
                             xt::statusName(loadResult.status()));
                return 1;
            }
            xt::DocumentHandle handle = loadResult.value();
            app.setCurrentDocument(handle);
            app.syncViewport();
            xt::Document* doc = app.context().document(handle);
            if (doc != nullptr) {
                wireUi(*doc);
            }
            // App::run() itself polls SDL events, pushes input, calls Context::update()/render()
            // and swaps the window each iteration (docs/design/13-api.md §13.5); the UI's
            // reactions all happen through the listeners wired above, so onFrame has nothing to
            // add.
            app.run([](float /*dt*/) {});
            std::printf("done\n");
            return 0;
        }
        // NOTE: App reports why it could not create a window/GL context in app.lastError(); this
        // example does not print it since falling back to the headless path is the documented,
        // expected behavior on a machine with no display (docs/tasks/T-40.md).
    }

    int status = runHeadless(ir, args.width, args.height, args.frames);
    if (status != 0) {
        return status;
    }
    std::printf("done\n");
    return 0;
}
