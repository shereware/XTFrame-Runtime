#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "example_args.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/scene_instance.h"
#include "xt/shell/app.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// lottie_player: plays a Lottie `.json` (compiled at runtime) or an already-compiled `.xtir`
/// through `xt_shell::App`, with playback controls driven by `Document::play/pause/resume/seek`
/// (Docs/design/13-api.md §13.2) on the document's `defaultAnimation()` instance. See
/// README.md for the full walkthrough.
///
/// Usage: lottie_player <file.json|file.xtir> [--headless] [--frames N] [--width W] [--height H]
///
/// Keys (interactive mode only): Space pause/resume, Left/Right step one frame back/forward,
/// R reload (recompile/reread the file into a fresh Document, replacing the old one), Escape
/// quits. The window title shows the current frame index and time.
///
/// Exit codes: 0 success (prints "done"), 1 a load/compile error, 2 argument error. With
/// `--headless`, or when `App` cannot create a window/GL context (e.g. no display), this renders
/// `--frames` frames off-screen with `xt_sw::SwRenderer` instead of opening a window.

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: lottie_player <file.json|file.xtir> [--headless] [--frames N] "
                 "[--width W] [--height H]\n");
}

void printDiagnostic(const xt::compile::Diagnostic& d) {
    const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
    std::fprintf(stderr, "lottie_player: %s: %s\n", level, d.message.c_str());
}

/// Compiles (`.json`) or reads (`.xtir`) `path` into `ctx`, replacing `handle` (unloaded first, if
/// valid). Returns false on failure, leaving `handle`/`ctx` untouched. Mirrors xtframe-view's
/// `loadInto` (Docs/tasks/T-20.md "xtframe-view").
bool loadInto(xt::Context& ctx, const std::string& path, const std::string& rootDir,
              xt::DocumentHandle& handle) {
    xt::Result<xt::DocumentHandle> result{xt::Result<xt::DocumentHandle>::error(xt::Status::kInternal)};
    std::vector<uint8_t> compiled;

    fs::path inputPath(path);
    if (inputPath.extension() == ".json") {
        xt::compile::CompileOptions options;
        options.rootDir = rootDir;
        xt::compile::CompileResult compileResult = xt::compile::compileLottie(path, options);
        for (const xt::compile::Diagnostic& d : compileResult.diagnostics) {
            printDiagnostic(d);
        }
        if (!compileResult.ok()) {
            std::fprintf(stderr, "lottie_player: failed to compile '%s'\n", path.c_str());
            return false;
        }
        compiled = std::move(compileResult.ir);
        result = ctx.loadDocument(xt::IrBlob{compiled.data(), compiled.size()}, /*takeOwnership=*/true);
    } else {
        result = ctx.loadDocumentFile(path);
    }

    if (!result.ok()) {
        std::fprintf(stderr, "lottie_player: failed to load '%s' (%s)\n", path.c_str(),
                     xt::statusName(result.status()));
        return false;
    }

    if (handle != 0) {
        ctx.unloadDocument(handle);
    }
    handle = result.value();
    return true;
}

/// Every Document element with a non-zero defaultAnimation() instance (there is exactly one for a
/// compileLottie() output, Docs/design/12-compiler.md §12.2; a hand-authored `.xt`/`.xtir` may
/// have more).
std::vector<xt::ElementRef> animatedElements(xt::Document& doc) {
    std::vector<xt::ElementRef> out;
    for (uint32_t i = 0;; ++i) {
        xt::ElementRef ref = doc.elementAt(i);
        if (!ref.valid()) {
            break;
        }
        if (doc.defaultAnimation(ref) != 0) {
            out.push_back(ref);
        }
    }
    return out;
}

float frameRateFor(xt::Document& doc, xt::ElementRef anim) {
    const xt::SceneInstance* inst = doc.sceneInstance(anim);
    if (inst == nullptr) {
        return 30.0f;
    }
    xt::TimelineId timelineId = doc.sceneTimeline(inst->scene);
    const xt::Timeline* timeline = doc.animator().timeline(timelineId);
    if (timeline == nullptr || timeline->frameRate <= 0) {
        return 30.0f;
    }
    return timeline->frameRate;
}

/// Off-screen path (Docs/tasks/T-40.md "--headless --frames N 用 SW 渲染 N 幀不開窗"): no window,
/// no keyboard controls, just `--frames` update/render/rasterize iterations.
int runHeadless(const std::string& file, const std::string& rootDir, int width, int height, uint32_t frames) {
    xt::NullTextShaper shaper;  // Lottie shapes are vector paths; no text is involved.
    xt::FileAssetResolver assets(rootDir, xt::sw::decodeImageBytes);
    xt::Context ctx(xt::ContextConfig{&shaper, &assets});

    xt::DocumentHandle handle = 0;
    if (!loadInto(ctx, file, rootDir, handle)) {
        return 1;
    }
    ctx.setViewport(xt::Rect{0, 0, static_cast<float>(width), static_cast<float>(height)}, 1.0f);

    xt::sw::SwRenderer renderer;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    xt::sw::SwTarget target{pixels.data(), width, height, width * 4};

    const xt::Document* doc = ctx.document(handle);
    const xt::IrView* irView = doc != nullptr ? &doc->ir() : nullptr;

    const float dt = 1.0f / 60.0f;
    xt::CommandList list;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ctx.update(frame == 0 ? 0.0f : dt);
        list.clear();
        ctx.render(list);
        renderer.setTarget(target);
        renderer.clear(xt::Color{0, 0, 0, 1});
        renderer.render(list, &shaper, &assets, irView);
    }

    std::printf("lottie_player: rendered %u headless frame(s) at %dx%d\n", frames, width, height);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    xt::examples::CommonArgs args;
    args.width = 512;
    args.height = 512;
    std::vector<std::string> remaining;
    if (!xt::examples::parseCommonArgs(argc, argv, args, remaining) || !remaining.empty() ||
        args.file.empty()) {
        printUsage();
        return 2;
    }

    fs::path inputPath(args.file);
    std::string rootDir = inputPath.has_parent_path() ? inputPath.parent_path().string() : ".";

    if (!args.headless) {
        xt::shell::WindowConfig windowConfig;
        windowConfig.width = args.width;
        windowConfig.height = args.height;
        windowConfig.title = "XTFrame lottie_player";
        xt::shell::App app(windowConfig);
        if (app.ok()) {
            app.setAssetRoot(rootDir);
            xt::DocumentHandle handle = 0;
            if (!loadInto(app.context(), args.file, rootDir, handle)) {
                return 1;
            }
            app.setCurrentDocument(handle);
            app.syncViewport();
            app.setTitle(("XTFrame lottie_player - " + inputPath.filename().string()).c_str());

            bool paused = false;
            char titleBuf[256];

            auto onFrame = [&](float /*dt*/) {
                std::vector<xt::InputEvent> events;
                app.context().drainInput(events);
                xt::Document* doc = app.context().document(handle);
                if (doc == nullptr) {
                    return;
                }
                for (const xt::InputEvent& event : events) {
                    if (event.kind != xt::InputEvent::KeyDown) {
                        continue;
                    }
                    switch (event.key) {
                        case xt::KeyCode::Space:
                            paused = !paused;
                            for (xt::ElementRef anim : animatedElements(*doc)) {
                                uint32_t id = doc->defaultAnimation(anim);
                                if (paused) {
                                    doc->pause(id);
                                } else {
                                    doc->resume(id);
                                }
                            }
                            break;
                        case xt::KeyCode::Left:
                        case xt::KeyCode::Right: {
                            float sign = event.key == xt::KeyCode::Right ? 1.0f : -1.0f;
                            for (xt::ElementRef anim : animatedElements(*doc)) {
                                uint32_t id = doc->defaultAnimation(anim);
                                float frameRate = frameRateFor(*doc, anim);
                                float localT = doc->animator().localTime(id, app.context().clock());
                                doc->seek(id, localT + sign / frameRate);
                            }
                            break;
                        }
                        case xt::KeyCode::KeyR: {
                            xt::DocumentHandle reloaded = handle;
                            if (loadInto(app.context(), args.file, rootDir, reloaded)) {
                                handle = reloaded;
                                app.setCurrentDocument(handle);
                                app.syncViewport();
                                paused = false;
                            }
                            break;
                        }
                        case xt::KeyCode::Escape:
                            app.quit();
                            break;
                        default:
                            break;
                    }
                }

                // Title bar shows the first animated element's current frame/time.
                std::vector<xt::ElementRef> anims = animatedElements(*doc);
                if (!anims.empty()) {
                    uint32_t id = doc->defaultAnimation(anims[0]);
                    float frameRate = frameRateFor(*doc, anims[0]);
                    float localT = doc->animator().localTime(id, app.context().clock());
                    std::snprintf(titleBuf, sizeof(titleBuf), "XTFrame lottie_player - frame %d t=%.3fs%s",
                                  static_cast<int>(localT * frameRate + 0.5f), static_cast<double>(localT),
                                  paused ? " [paused]" : "");
                    app.setTitle(titleBuf);
                }
            };

            app.run(onFrame);
            std::printf("done\n");
            return 0;
        }
        // No display / GL context available: fall through to the headless render path below,
        // same as desktop_app (Docs/tasks/T-40.md).
    }

    int status = runHeadless(args.file, rootDir, args.width, args.height, args.frames == 0 ? 1 : args.frames);
    if (status != 0) {
        return status;
    }
    std::printf("done\n");
    return 0;
}
