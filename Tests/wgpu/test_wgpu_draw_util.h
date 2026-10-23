#pragma once

#include <webgpu/webgpu.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "doctest.h"
#include "golden_common.h"
#include "test_wgpu_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"
#include "xt/wgpu/wgpu_device.h"
#include "xt/wgpu/wgpu_renderer.h"

/// Shared helpers of the T-438 draw cases (Tests/wgpu/test_wgpu_draw.cpp, test_wgpu_effects.cpp):
/// CommandList builders in the style of Tests/gl, a WgpuRenderer session drawing into a readable
/// target, and the GPU/SW cross-check of 15 §15.3 (channel 16, 3 % of the pixels).

namespace xt::wgpu::test {

/// A render target the cases draw into and read back (also sampleable: Direct mode, 4x loads).
struct DrawTarget {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    int w = 0, h = 0;
    DrawTarget(int width, int height) : w(width), h(height) {
        WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
        d.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst |
                  WGPUTextureUsage_TextureBinding;
        d.size = WGPUExtent3D{static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        d.format = WGPUTextureFormat_RGBA8Unorm;
        texture = wgpuDeviceCreateTexture(sharedInit().device, &d);
        view = wgpuTextureCreateView(texture, nullptr);
    }
    ~DrawTarget() {
        wgpuTextureViewRelease(view);
        wgpuTextureDestroy(texture);
        wgpuTextureRelease(texture);
    }
    DrawTarget(const DrawTarget&) = delete;
    DrawTarget& operator=(const DrawTarget&) = delete;
    /// Replaces the whole target with `rgba` (top-down RGBA8).
    void upload(const std::vector<uint8_t>& rgba) const {
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = texture;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = static_cast<uint32_t>(w) * 4u;
        layout.rowsPerImage = static_cast<uint32_t>(h);
        const WGPUExtent3D size{static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        wgpuQueueWriteTexture(sharedInit().queue, &dst, rgba.data(), rgba.size(), &layout, &size);
    }
};

/// One renderer kept across renders (pipeline and custom-program caches live in it).
struct Session {
    WgpuRenderer renderer;
    DrawTarget target;
    /// lastError()/lastWarning() of the last render (readback() clears the renderer's own).
    std::string error, warning;
    explicit Session(int w, int h, uint32_t samples = 1) : target(w, h) {
        WgpuInit in = sharedInit();
        in.sampleCount = samples;
        in.error.clear();
        REQUIRE(renderer.init(in));
        renderer.resize(w, h);
    }
    std::vector<uint8_t> read() {
        std::vector<uint8_t> px(static_cast<size_t>(target.w) * static_cast<size_t>(target.h) * 4);
        REQUIRE_MESSAGE(renderer.readback(target.texture, px), renderer.lastError());
        return px;
    }
    /// Clears to `clear` (straight colour, premultiplied here as SwRenderer::clear does), draws, reads.
    std::vector<uint8_t> render(const CommandList& cl, Color clear = Color{0, 0, 0, 1},
                                AssetResolver* assets = nullptr, const IrView* ir = nullptr,
                                PerfTracker* perf = nullptr, const DamageList* damage = nullptr) {
        renderer.setClearColor(clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a);
        renderer.render(cl, nullptr, assets, target.view, ir, perf, damage);
        error = renderer.lastError();
        warning = renderer.lastWarning();
        return read();
    }
};

inline std::vector<uint8_t> renderSw(const CommandList& cl, int w, int h, Color clear = Color{0, 0, 0, 1},
                                     AssetResolver* assets = nullptr, const IrView* ir = nullptr) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{buf.data(), w, h, w * 4};
    renderer.setTarget(target);
    renderer.clear(clear);
    renderer.render(cl, nullptr, assets, ir);
    return buf;
}

/// 15 §15.3's GPU/SW tolerance; prints the numbers for the report.
inline bool matches(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& sw, int w, int h,
                    const char* what, float ratio = 0.03f) {
    golden::GoldenResult r = golden::compareRgba(gpu.data(), sw.data(), w, h, 16, ratio);
    std::printf("wgpu vs sw %s: %u px differ, max channel diff %u\n", what, r.diffPixels, r.maxChannelDiff);
    return r.pass;
}

inline const uint8_t* pixel(const std::vector<uint8_t>& rgba, int w, int x, int y) {
    return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
}

inline CommandList makeList(int w, int h) {
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    return cl;
}

/// A closed polygon path.
inline PathRef addPolygon(CommandList& cl, std::initializer_list<Vec2> pts) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    bool first = true;
    for (Vec2 p : pts) {
        cl.verbs.push_back(first ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(p.x);
        cl.points.push_back(p.y);
        first = false;
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
    return ref;
}

/// Appends a regular `n`-gon contour (center `c`, radius `r`) to the path `ref` (same verb/point run).
inline void appendNgon(CommandList& cl, PathRef& ref, Vec2 c, float r, int n) {
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(c.x + r * std::cos(a));
        cl.points.push_back(c.y + r * std::sin(a));
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(cl.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(cl.points.size()) - ref.pointOffset;
}

inline PathRef emptyPath(const CommandList& cl) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    return ref;
}

inline void addFill(CommandList& cl, PathRef path, Color color, FillRule rule = FillRule::NonZero,
                    Mat2x3 xf = Mat2x3{}) {
    FillCmd f{};
    f.xf = xf;
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = rule;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

inline void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    addFill(cl, addPolygon(cl, {Vec2{x, y}, Vec2{x + w, y}, Vec2{x + w, y + h}, Vec2{x, y + h}}), color);
}

inline void addPushClipRect(CommandList& cl, Rect r, Mat2x3 xf = Mat2x3{}) {
    ClipRectCmd c{};
    c.xf = xf;
    c.rect = r;
    cl.clipRects.push_back(c);
    cl.commands.push_back(Command{CmdKind::PushClipRect, {}, static_cast<uint32_t>(cl.clipRects.size() - 1)});
}

inline void addPopClip(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopClip, {}, 0}); }

inline void addPushLayer(CommandList& cl, float opacity, BlendMode blend, Rect hint, bool sceneRead = false) {
    LayerCmd l{};
    l.opacity = opacity;
    l.blend = blend;
    l.boundsHint = hint;
    cl.layers.push_back(l);
    Command c{CmdKind::PushLayer, {}, static_cast<uint32_t>(cl.layers.size() - 1)};
    if (sceneRead) c.flags[0] = kCommandSceneRead;
    cl.commands.push_back(c);
}

inline void addPopLayer(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0}); }

inline void addPushEffect(CommandList& cl, std::initializer_list<EffectInstance> effects, Rect boundsHint,
                          Rect contentBounds, bool sceneRead = false) {
    EffectCmd c{};
    c.effectOffset = static_cast<uint32_t>(cl.effects.size());
    c.effectCount = static_cast<uint32_t>(effects.size());
    for (const EffectInstance& e : effects) cl.effects.push_back(e);
    c.xf = Mat2x3{};
    c.boundsHint = boundsHint;
    c.contentBounds = contentBounds;
    cl.effectCmds.push_back(c);
    Command cmd{CmdKind::PushEffect, {}, static_cast<uint32_t>(cl.effectCmds.size() - 1)};
    if (sceneRead) cmd.flags[0] = kCommandSceneRead;
    cl.commands.push_back(cmd);
}

inline void addPopEffect(CommandList& cl) { cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0}); }

inline EffectInstance effect(EffectKind kind, std::initializer_list<float> params,
                             Color color = Color{0, 0, 0, 1}) {
    EffectInstance e{};
    e.kind = kind;
    int i = 0;
    for (float p : params) e.params[i++] = p;
    e.colors[0] = color;
    return e;
}

}  // namespace xt::wgpu::test
