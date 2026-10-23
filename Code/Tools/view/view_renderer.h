#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// T-439 (Docs/design/10-backends.md §10.10.4, 45 §45.7): `xtframe-view --renderer sw|gl|wgpu`.
/// The default stays what the build's shell presents with (XT_SHELL_GL: gl, else sw). Choosing
/// another renderer that is built in draws the frames through Code/Backends' renderer files instead
/// of xt_shell: offscreen under --headless (gl: a hidden SDL window's GL context and an FBO; wgpu:
/// a headless device and a texture), in an SDL window otherwise (the headless frame loop driven by
/// the platform's events -- a viewer without the shell's extras). xt_shell itself is unchanged.

namespace xt::view {

enum class Renderer { Sw, Gl, Wgpu };

const char* rendererName(Renderer renderer);
/// "sw gl wgpu" -- the renderers built into this executable.
std::string builtRenderers();
/// The renderer of a run without --renderer: xt_sw under --headless, else what the shell presents
/// with (XT_SHELL_SW: sw, else gl).
Renderer defaultRenderer(bool headless);
/// `text` (the --renderer value; empty = the default) as a built-in renderer. An unknown name or a
/// renderer this build lacks prints one line to stderr and gives the default.
Renderer resolveRenderer(std::string_view text, bool headless);

struct FrameTargetConfig {
    int width = 0;  ///< logical pixels (= pixels offscreen)
    int height = 0;
    bool windowed = false;
    bool hidden = false;
    const char* title = "XTFrame";
    Color clear{1, 1, 1, 1};  ///< straight alpha
    uint32_t sampleCount = 4;
};

/// Where the view's frame loop draws when it is not the shell or the plain xt_sw buffer.
class FrameTarget {
public:
    virtual ~FrameTarget() = default;
    virtual bool windowed() const = 0;
    /// Windowed: the platform's events into `context` (also its viewport); false once closed.
    virtual bool pump(Context& context) = 0;
    virtual void render(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
                        PerfTracker* perf, const DamageList* damage) = 0;
    /// The last frame as straight-alpha RGBA8, row 0 = top (call before present() for a window).
    virtual bool read(std::vector<uint8_t>& rgba, int& width, int& height) = 0;
    virtual void present() = 0;
    virtual double seconds() const = 0;
#ifndef XT_BACKEND_NO_TEXT
    /// Glyph atlas source of the GPU renderers.
    virtual void setTextShaper(text::FtTextShaper* shaper) = 0;
#endif
    virtual std::string description() const = 0;
};

/// The gl / wgpu / sw target of `config`; null with `error` when it cannot be created (sw is only
/// windowed: offscreen sw is the view's own buffer).
std::unique_ptr<FrameTarget> createFrameTarget(Renderer renderer, const FrameTargetConfig& config,
                                               std::string& error);

}  // namespace xt::view
