// XTFrame reference backend -- MIT License (see LICENSE). Copy this file, one platform pair, one
// renderer pair and the matching xt_backend_<platform>_<renderer>.cpp into your project and edit them
// there (Code/Backends/README.md, Docs/design/45-repository-layout.md §45.7).
// Host API: none (the common interface every combination file implements).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/types.h"

namespace xt {
class TextShaper;
class AssetResolver;
}  // namespace xt

#ifndef XT_BACKEND_NO_TEXT
namespace xt::text {
class FtTextShaper;
}  // namespace xt::text
#endif

/// The small interface the samples and `xtframe-view` drive a window with (45 §45.7): create a
/// window and a renderer, pump the platform's events into a `Context`, then per frame
/// `beginFrame()` -> `render(list, ...)` -> `present()`. Every `xt_backend_<platform>_<renderer>.cpp`
/// implements this one class, so an executable links exactly one combination (RmlUi's rule; two
/// combination libraries in one executable would define `Backend` twice).
///
/// The host duties of Docs/design/23-host-integration.md map onto it like this: `pumpEvents` turns
/// window, mouse, keyboard, wheel, text and IME events into `InputEvent`s and keeps the Context's
/// viewport equal to the window (logical pixels, device pixel ratio); `seconds()` is the clock the
/// caller derives `dt` from; `render` takes the `DamageList` of `Context::render` (renderers that
/// cannot repaint partially redraw the whole surface).

namespace xt::backend {

/// What `Backend::create` opens.
struct BackendOptions {
    /// Window title (UTF-8).
    const char* title = "XTFrame";
    /// Client size in logical pixels.
    int width = 1280;
    int height = 720;
    bool resizable = true;
    /// Created hidden (tests, automation).
    bool hidden = false;
    /// Present synchronized to the display (Fifo / swap interval 1).
    bool vsync = true;
    /// GPU renderers: 1 or 4 (4x MSAA, the default of the shell and the editor preview).
    uint32_t sampleCount = 4;
    /// Straight-alpha colour every frame starts from.
    Color clearColor{0.1f, 0.1f, 0.1f, 1.0f};
};

class Backend {
public:
    Backend();
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    /// Opens the window and initializes the renderer on it. False (and `lastError()`) when either
    /// fails; whatever was created is destroyed again.
    bool create(const BackendOptions& options);
    /// Destroys the renderer and the window. Safe to call twice or after a failed create.
    void destroy();

    /// Takes every pending platform event: input goes to `context.pushInput`, a resize updates
    /// `context.setViewport` (also called on the first pump), text input follows
    /// `context.wantsTextInput()`. False once the window was asked to close.
    bool pumpEvents(Context& context);
    /// Starts a frame: picks up a size change, acquires the target and clears it.
    void beginFrame();
    /// Draws `list` into the frame `beginFrame` started (see each renderer for `damage`).
    void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                const IrView* ir = nullptr, PerfTracker* perf = nullptr, const DamageList* damage = nullptr);
    /// Shows the frame (swap, surface present or blit).
    void present();

#ifndef XT_BACKEND_NO_TEXT
    /// GPU renderers rasterize glyphs through an atlas built from `shaper` (null: from outlines
    /// through the `shaper` given to `render`). Not owned. The software renderer ignores it.
    void setTextShaper(text::FtTextShaper* shaper);
#endif

    /// Client size in logical pixels.
    int width() const;
    int height() const;
    /// Pixels per logical pixel.
    float devicePixelRatio() const;
    /// Monotonic seconds since create().
    double seconds() const;
    /// The platform clipboard as UTF-8 (empty when it holds no text).
    std::string clipboardText() const;
    void setClipboardText(std::string_view utf8);

    /// "sdl2_wgpu", "win32_wgpu", ...
    const char* name() const;
    /// Why the last create() failed, or the renderer's last error; empty otherwise.
    const char* lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace xt::backend
