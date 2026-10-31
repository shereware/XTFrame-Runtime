#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "capi_internal.h"
#include "xt/input.h"
#include "xt/timeline.h"

/// `xt_surface_*` (Docs/design/50-c-abi.md §50.3 "Surface"): creation and teardown, viewport,
/// input, update / render, lifecycle, timelines, world anchors and frame statistics. Ports,
/// connection points and the callbacks are in capi_ports.cpp.

xt_surface::~xt_surface() { teardown(); }

void xt_surface::teardown() {
    // 50.3: ports -> bundle context (its flows) -> context.
    ports.reset();
    bundleCtx.reset();
    if (ctx != nullptr) {
        ctx->removeListener(this);
        if (timelineListener != 0) {
            ctx->removeTimelineListener(timelineListener);
        }
        // The resolver (bundle.assets()) outlives this Context and is shared with the other
        // surfaces: unload first so it releases this Document's IR (Context::~Context, T-263f).
        if (handle != 0) {
            ctx->unloadDocument(handle);
        }
        ctx.reset();
    }
    handle = 0;
    timelineListener = 0;
}

void xt_surface::onShown(xt::DocumentHandle document) {
    if (document != handle || visibilityFn == nullptr) {
        return;
    }
    xt::capi::callbackGuard("the visibility callback", [&] { visibilityFn(this, 1, visibilityUser); });
}

void xt_surface::onHidden(xt::DocumentHandle document) {
    if (document != handle || visibilityFn == nullptr) {
        return;
    }
    xt::capi::callbackGuard("the visibility callback", [&] { visibilityFn(this, 0, visibilityUser); });
}

void xt_surface::onBeforeDocumentUpdate(xt::Context& context) {
    // T-561 (50.3): inside xt_surface_update (its DispatchScope is open, so destroy / hot_reload refuse).
    if (&context != ctx.get() || updateHookFn == nullptr) {
        return;
    }
    xt::capi::callbackGuard("the update hook", [&] { updateHookFn(this, updateDt, updateHookUser); });
}

namespace xt::capi {

void applyUiScale(xt_surface& s) {
    const float logicalW = static_cast<float>(s.width) / s.dpr;
    const float logicalH = static_cast<float>(s.height) / s.dpr;
    const float scale = s.uiScaleOverride > 0.0f ? s.uiScaleOverride
                                                 : bundle::uiScaleFor(s.spec.uiscaleRule, logicalW, logicalH,
                                                                      s.spec.logicalHeight);
    (void)s.ctx->setUiScale(scale);
}

namespace {

/// Viewport + device (DPR) for the surface's current size, then the uiscale. T-552 (capi review K1): the
/// DeviceInfo is the Context's own (`ctx->device()`), so what was set since -- `reducedMotion` from
/// xt_surface_post_system_event, the safe area, the input kind -- is kept; `seed` (xt_surface_create only)
/// is the device the surface starts from.
Status applyViewport(xt_surface& s, const DeviceInfo* seed = nullptr) {
    s.ctx->setViewport(Rect{0, 0, static_cast<float>(s.width) / s.dpr, static_cast<float>(s.height) / s.dpr},
                       s.dpr);
    DeviceInfo device = seed != nullptr ? *seed : s.ctx->device();
    device.dpr = s.dpr;
    const Status status = s.ctx->setDevice(device);
    applyUiScale(s);
    return status;
}

/// Loads one flow file of the bundle into `load(json, errors)`; read and load errors go to the log
/// (level 2) and are not a failure of the caller (Docs/tasks/T-504.md item 1).
template <class Load>
bool loadFlowFile(xt_surface& s, std::string_view file, const char* what, Load&& load) {
    std::string json;
    if (!readBundleFile(*s.owner->bundle, file, json)) {
        log(2, std::string(what) + ": cannot read '" + std::string(file) + "'");
        return false;
    }
    std::vector<std::string> errors;
    const Status status = load(json, errors);
    for (const std::string& error : errors) {
        log(2, std::string(what) + " '" + std::string(file) + "': " + error);
    }
    if (status != Status::kOk) {
        log(2, std::string(what) + " '" + std::string(file) + "': " + statusName(status));
        return false;
    }
    return true;
}

/// The global flow (when the bundle has one) and the UI's own flow (when it has one), against
/// the UIs registered on the surface's BundleContext.
void loadFlows(xt_surface& s) {
    const std::string_view global = s.owner->bundle->globalFlow();
    if (!global.empty()) {
        s.globalFlowLoaded = loadFlowFile(s, global, "global flow", [&](std::string_view json, auto& errors) {
            return s.bundleCtx->loadGlobalFlow(json, errors);
        });
    }
    const bundle::UiInfo* info = s.owner->bundle->ui(s.uiId);
    if (info != nullptr && !info->flow.empty()) {
        loadFlowFile(s, info->flow, "ui flow", [&](std::string_view json, auto& errors) {
            return s.bundleCtx->loadUiFlow(s.uiId, json, errors);
        });
    }
}

bool validDpr(float dpr) { return std::isfinite(dpr) && dpr >= 0.0f; }

xt_surface* fromStatus(xt_status status, std::string message) {
    fail(status, std::move(message));
    return nullptr;
}

/// xt_input_event -> xt::InputEvent for every kind but TEXT_INPUT. False (with the last error set)
/// for a value outside xt::InputEvent's range.
bool convertInput(const xt_input_event& e, InputEvent& out) {
    if (e.button < 0 || e.button > 2 || e.gamepad_button > 14 || e.gamepad_axis > 5 || e.gesture > 4 ||
        e.direction > 3 || e.player >= kMaxPlayers) {
        fail(XT_STATUS_INVALID_ARGUMENT,
             "xt_surface_push_input: a field is outside the xt::InputEvent range");
        return false;
    }
    out.kind = static_cast<InputEvent::Kind>(e.kind);
    out.pos = Vec2{e.x, e.y};
    out.button = static_cast<PointerButton>(e.button);
    out.wheelDelta = Vec2{e.wheel_x, e.wheel_y};
    out.key = static_cast<KeyCode>(e.key);
    out.modifiers = e.modifiers;
    out.repeat = e.repeat;
    out.compositionCursor = e.composition_cursor;
    out.compositionSelLen = e.composition_sel_len;
    out.pointerId = e.pointer_id;
    out.gamepadButton = static_cast<GamepadButton>(e.gamepad_button);
    out.gamepadAxis = static_cast<GamepadAxis>(e.gamepad_axis);
    out.axisValue = e.axis_value;
    out.player = e.player;
    out.gesture = static_cast<GestureKind>(e.gesture);
    out.direction = static_cast<NavDirection>(e.direction);
    out.gestureScale = e.gesture_scale;
    out.clicks = e.clicks;
    if (e.kind == XT_INPUT_COMPOSITION_UPDATE && e.text_len > 0) {
        out.composition = std::string_view(e.text, e.text_len);  // Context::pushInput copies it.
    }
    return true;
}

}  // namespace

}  // namespace xt::capi

using xt::capi::fail;
using xt::capi::toCapi;

extern "C" {

XT_CAPI xt_surface* xt_surface_create(xt_bundle* b, const char* ui_id, const xt_surface_desc* desc) {
    using xt::capi::fromStatus;
    XT_CAPI_GUARD_OR("xt_surface_create", xt_surface*, nullptr, {
        if (b == nullptr || ui_id == nullptr || desc == nullptr) {
            return fromStatus(XT_STATUS_INVALID_ARGUMENT, "xt_surface_create: bundle, ui_id or desc is NULL");
        }
        if (desc->struct_size < sizeof(xt_surface_desc)) {
            return fromStatus(XT_STATUS_INVALID_ARGUMENT,
                              "xt_surface_create: desc->struct_size is smaller than sizeof(xt_surface_desc)");
        }
        if (desc->width <= 0 || desc->height <= 0 || !xt::capi::validDpr(desc->device_pixel_ratio)) {
            return fromStatus(XT_STATUS_INVALID_ARGUMENT,
                              "xt_surface_create: width / height must be > 0 and device_pixel_ratio >= 0");
        }
        if (b->bundle->ui(ui_id) == nullptr) {
            return fromStatus(XT_STATUS_NOT_FOUND,
                              std::string("xt_surface_create: the bundle has no UI '") + ui_id + "'");
        }
        auto s = std::make_unique<xt_surface>();
        s->owner = b;
        s->uiId = ui_id;
        const xt::bundle::BundleSpec* spec = b->bundle->spec();
        s->spec = spec != nullptr ? *spec : xt::bundle::BundleSpec{};
        s->width = desc->width;
        s->height = desc->height;
        s->dpr = desc->device_pixel_ratio > 0.0f ? desc->device_pixel_ratio : 1.0f;
        s->premultiplied = desc->premultiplied != 0;
        s->runFlow = desc->run_flow != 0;
        const float inset = s->spec.safeAreaDefault;
        s->device.safeArea = xt::Insets{inset, inset, inset, inset};

        // Load order (50.3, Docs/tasks/T-504.md item 1).
        xt::TextShaper* shaper = xt::capi::ensureShaper(*b, desc->system_fonts != 0);
        s->ctx = std::make_unique<xt::Context>(xt::ContextConfig{shaper, &b->bundle->assets()});
        xt::Result<xt::DocumentHandle> loaded = xt::bundle::loadUi(*s->ctx, *b->bundle, s->uiId);
        if (!loaded.ok()) {
            return fromStatus(toCapi(loaded.status()), "xt_surface_create: cannot load UI '" + s->uiId +
                                                           "': " + xt::statusName(loaded.status()));
        }
        s->handle = loaded.value();
        (void)s->ctx->setInstanceId(s->handle, desc->instance_id);
        s->ctx->addListener(s.get());
        xt_surface* raw = s.get();
        s->timelineListener = s->ctx->addTimelineListener([raw](const xt::TimelineEvent& event) {
            if (raw->timelineFn == nullptr) {
                return;
            }
            xt::capi::callbackGuard("the timeline callback", [&] {
                const std::string timeline(event.timeline);
                const std::string marker(event.marker);
                xt_timeline_event e{};
                e.kind = static_cast<int>(event.kind);
                e.timeline = timeline.c_str();
                e.marker = marker.c_str();
                raw->timelineFn(raw, &e, raw->timelineUser);
            });
        });
        s->bundleCtx = std::make_unique<xt::flow::BundleContext>(*s->ctx);
        s->bundleCtx->setHostEventListener([raw](const xt::flow::FlowHostEvent& event) {
            if (raw->hostFn == nullptr) {
                return;
            }
            xt::capi::callbackGuard("the host event callback", [&] {
                xt_host_event e{};
                e.kind = static_cast<int>(event.kind);
                e.name = event.name.c_str();
                e.volume = event.volume;
                e.ui_id = event.uiId.c_str();
                e.instance_id = event.instanceId;
                e.node_id = event.nodeId.c_str();
                raw->hostFn(raw, &e, raw->hostUser);
            });
        });
        const xt::Status added = s->bundleCtx->addUi(s->uiId, s->handle);
        if (added != xt::Status::kOk) {
            return fromStatus(toCapi(added), "xt_surface_create: BundleContext::addUi failed");
        }
        if (s->runFlow) {
            xt::capi::loadFlows(*s);
        }
        s->ports = std::make_unique<xt::flow::HostPorts>();
        xt::Document* doc = s->ctx->document(s->handle);
        if (doc == nullptr) {
            return fromStatus(XT_STATUS_INTERNAL, "xt_surface_create: the loaded Document is gone");
        }
        (void)s->ports->bind(*doc);
        (void)s->ports->setInstanceId(desc->instance_id);
        xt::capi::attachEmitListener(*s);
        xt::capi::attachPortListeners(*s);
        if (xt::bundle::loadThemes(*s->ctx, *b->bundle) != xt::Status::kOk) {
            xt::capi::log(1, "xt_surface_create: a theme file of the bundle could not be read");
        }
        const xt::bundle::UiScaleLimits& limits = s->spec.uiscaleLimits;
        (void)s->ctx->setUiScaleLimits(limits.clampMin, limits.clampMax, limits.dpiFloor);
        const xt::Status viewport = xt::capi::applyViewport(*s, &s->device);
        if (viewport != xt::Status::kOk) {
            return fromStatus(toCapi(viewport), "xt_surface_create: Context::setDevice rejected the device");
        }
        ++b->surfaceCount;
        return s.release();
    });
}

XT_CAPI void xt_surface_destroy(xt_surface* s) {
    XT_CAPI_GUARD_VOID("xt_surface_destroy", {
        if (s == nullptr) {
            return;
        }
        if (s->dispatchDepth > 0) {
            fail(XT_STATUS_UNSUPPORTED, "xt_surface_destroy: called from inside a callback; not destroyed");
            return;
        }
        xt_bundle* owner = s->owner;
        s->teardown();
        delete s;
        --owner->surfaceCount;
    });
}

XT_CAPI const char* xt_surface_ui_id(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_ui_id", const char*, "", { return s == nullptr ? "" : s->uiId.c_str(); });
}

XT_CAPI xt_status xt_surface_resize(xt_surface* s, int width, int height, float device_pixel_ratio) {
    XT_CAPI_GUARD("xt_surface_resize", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_resize");
        if (width <= 0 || height <= 0 || !xt::capi::validDpr(device_pixel_ratio)) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_resize: width / height must be > 0 and device_pixel_ratio >= 0");
        }
        xt::capi::DispatchScope scope(*s);
        s->width = width;
        s->height = height;
        s->dpr = device_pixel_ratio > 0.0f ? device_pixel_ratio : 1.0f;
        return toCapi(xt::capi::applyViewport(*s));
    });
}

XT_CAPI xt_status xt_surface_set_ui_scale(xt_surface* s, float scale) {
    XT_CAPI_GUARD("xt_surface_set_ui_scale", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_ui_scale");
        if (!std::isfinite(scale)) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_set_ui_scale: scale is not finite");
        }
        xt::capi::DispatchScope scope(*s);
        s->uiScaleOverride = scale > 0.0f ? scale : 0.0f;
        xt::capi::applyUiScale(*s);
        return XT_STATUS_OK;
    });
}

XT_CAPI float xt_surface_ui_scale(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_ui_scale", float, 0.0f, { return s == nullptr ? 0.0f : s->ctx->uiScale(); });
}

XT_CAPI xt_status xt_surface_set_safe_area(xt_surface* s, float top, float right, float bottom, float left) {
    XT_CAPI_GUARD("xt_surface_set_safe_area", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_safe_area");
        xt::capi::DispatchScope scope(*s);
        xt::DeviceInfo device = s->ctx->device();  // T-552 (K1): keeps reducedMotion and the input kind.
        device.safeArea = xt::Insets{top, right, bottom, left};
        const xt::Status status = s->ctx->setDevice(device);
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), "xt_surface_set_safe_area: each inset must be in [0, 0.5]");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_input_kind(xt_surface* s, int kind) {
    XT_CAPI_GUARD("xt_surface_set_input_kind", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_input_kind");
        if (kind < 0 || kind > 2) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_set_input_kind: kind must be 0, 1 or 2");
        }
        xt::capi::DispatchScope scope(*s);
        return toCapi(s->ctx->setInputDevice(static_cast<xt::InputKind>(kind)));
    });
}

XT_CAPI xt_status xt_surface_push_input(xt_surface* s, const xt_input_event* e) {
    XT_CAPI_GUARD("xt_surface_push_input", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_push_input");
        if (e == nullptr || e->struct_size < sizeof(xt_input_event)) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_push_input: event is NULL or its struct_size is too small");
        }
        if (e->kind < XT_INPUT_POINTER_MOVE || e->kind > XT_INPUT_POINTER_CANCEL) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_push_input: kind is outside 0..15");
        }
        if (e->text_len > 0 && e->text == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_push_input: text is NULL with text_len > 0");
        }
        xt::InputEvent event;
        if (!xt::capi::convertInput(*e, event)) {
            return XT_STATUS_INVALID_ARGUMENT;
        }
        if (e->kind != XT_INPUT_TEXT_INPUT) {
            s->ctx->pushInput(event);
            ++s->debugInputCount;
            return XT_STATUS_OK;
        }
        // 50.3: one InputEvent::TextInput per code point; invalid UTF-8 sends nothing.
        std::vector<std::pair<size_t, uint8_t>> points;
        if (!xt::capi::splitUtf8(e->text, e->text_len, points)) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_push_input: text is not valid UTF-8");
        }
        for (const auto& [offset, length] : points) {
            xt::InputEvent one = event;
            std::memset(one.utf8, 0, sizeof(one.utf8));
            std::memcpy(one.utf8, e->text + offset, length);
            one.utf8Len = length;
            s->ctx->pushInput(one);
        }
        s->debugInputCount += static_cast<uint32_t>(points.size());
        return XT_STATUS_OK;
    });
}

XT_CAPI void xt_surface_cancel_pointer(xt_surface* s) {
    XT_CAPI_GUARD_VOID("xt_surface_cancel_pointer", {
        if (s != nullptr) {
            s->ctx->cancelPointer();
        }
    });
}

XT_CAPI int xt_surface_cursor(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_cursor", int, 0,
                     { return s == nullptr ? 0 : static_cast<int>(s->ctx->cursor()); });
}

XT_CAPI int xt_surface_wants_text_input(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_wants_text_input", int, 0,
                     { return s != nullptr && s->ctx->wantsTextInput() ? 1 : 0; });
}

XT_CAPI xt_status xt_surface_text_input_rect(const xt_surface* s, float* x, float* y, float* w, float* h) {
    XT_CAPI_GUARD("xt_surface_text_input_rect", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_text_input_rect");
        if (x == nullptr || y == nullptr || w == nullptr || h == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_text_input_rect: an out pointer is NULL");
        }
        const xt::Rect rect = s->ctx->textInputRect();
        *x = rect.x;
        *y = rect.y;
        *w = rect.w;
        *h = rect.h;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_update(xt_surface* s, float dt) {
    XT_CAPI_GUARD("xt_surface_update", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_update");
        if (!std::isfinite(dt) || dt < 0.0f) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_update: dt must be finite and >= 0");
        }
        xt::capi::DispatchScope scope(*s);
        s->updateDt = dt;  // T-561: for the update hook, called inside ctx->update().
        s->ctx->update(dt);
        if (s->bundleCtx != nullptr) {
            s->bundleCtx->tick(dt);
        }
        if (s->ports != nullptr) {
            s->ports->tick(dt);
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_render(xt_surface* s, void* rgba, int stride_bytes, xt_damage* out_damage) {
    XT_CAPI_GUARD("xt_surface_render", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_render");
        // T-552 (capi review K4): `width * 4` in size_t, so a large width cannot wrap past the check; a row
        // wider than an int stride can describe is refused too.
        const size_t rowBytes = static_cast<size_t>(s->width) * 4u;
        if (rgba == nullptr || stride_bytes < 0 || static_cast<size_t>(s->width) > SIZE_MAX / 4u ||
            rowBytes > static_cast<size_t>(INT32_MAX) || static_cast<size_t>(stride_bytes) < rowBytes) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_surface_render: rgba is NULL or stride_bytes < width * 4");
        }
        xt::capi::DispatchScope scope(*s);
        xt::sw::SwTarget target;
        target.rgba = static_cast<uint8_t*>(rgba);
        target.width = s->width;
        target.height = s->height;
        target.strideBytes = stride_bytes;
        target.premultiplied = s->premultiplied;
        const bool newTarget = target.rgba != s->target.rgba || target.width != s->target.width ||
                               target.height != s->target.height ||
                               target.strideBytes != s->target.strideBytes ||
                               target.premultiplied != s->target.premultiplied;
        if (newTarget) {
            s->renderer.setTarget(target);
            s->target = target;
        }
        s->ctx->render(s->list, &s->damage);
        // A buffer this renderer has not drawn into holds no previous frame to keep: repaint it all.
        const bool full = s->damage.full || newTarget;
        const xt::Color clear{0, 0, 0, 0};
        if (full) {
            s->renderer.clear(clear);
        } else {
            s->renderer.clearRects(clear, s->damage.rects);
        }
        s->renderer.render(s->list, s->owner->shaper.get(), &s->owner->bundle->assets(), nullptr,
                           &s->ctx->perf(), full ? nullptr : &s->damage);
        if (out_damage != nullptr) {
            std::memset(out_damage, 0, sizeof(*out_damage));
            out_damage->full = full ? 1 : 0;
            out_damage->changed = full || !s->damage.rects.empty() ? 1 : 0;
            if (!full) {
                const size_t n = std::min<size_t>(s->damage.rects.size(), 16);
                for (size_t i = 0; i < n; ++i) {
                    const xt::Rect& r = s->damage.rects[i];
                    const int x0 = std::clamp(static_cast<int>(std::floor(r.x)), 0, s->width);
                    const int y0 = std::clamp(static_cast<int>(std::floor(r.y)), 0, s->height);
                    const int x1 = std::clamp(static_cast<int>(std::ceil(r.x + r.w)), 0, s->width);
                    const int y1 = std::clamp(static_cast<int>(std::ceil(r.y + r.h)), 0, s->height);
                    out_damage->rects[i] = xt_rect{x0, y0, x1 - x0, y1 - y0};
                }
                out_damage->rect_count = static_cast<uint32_t>(n);
            }
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_request_show(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_request_show", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_request_show");
        xt::capi::DispatchScope scope(*s);
        return toCapi(s->ctx->requestShow(s->handle));
    });
}

XT_CAPI xt_status xt_surface_request_hide(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_request_hide", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_request_hide");
        xt::capi::DispatchScope scope(*s);
        return toCapi(s->ctx->requestHide(s->handle));
    });
}

XT_CAPI xt_status xt_surface_set_visible(xt_surface* s, int visible) {
    XT_CAPI_GUARD("xt_surface_set_visible", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_visible");
        xt::capi::DispatchScope scope(*s);
        return toCapi(s->ctx->setVisible(s->handle, visible != 0));
    });
}

XT_CAPI int xt_surface_is_visible(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_surface_is_visible", int, 0,
                     { return s != nullptr && s->ctx->isVisible(s->handle) ? 1 : 0; });
}

XT_CAPI xt_status xt_surface_set_theme(xt_surface* s, const char* name) {
    XT_CAPI_GUARD("xt_surface_set_theme", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_theme");
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->ctx->setTheme(name != nullptr ? name : "");
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), std::string("xt_surface_set_theme: no theme '") +
                                            (name != nullptr ? name : "") + "' in the bundle");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_set_language(xt_surface* s, const char* lang) {
    XT_CAPI_GUARD("xt_surface_set_language", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_language");
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->ctx->setLanguage(lang != nullptr ? lang : "");
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), "xt_surface_set_language: not a language tag");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_post_system_event(xt_surface* s, const char* kind, const char* value) {
    XT_CAPI_GUARD("xt_surface_post_system_event", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_post_system_event");
        if (kind == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_post_system_event: kind is NULL");
        }
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->bundleCtx->postSystemEvent(kind, value != nullptr ? value : "");
        if (status != xt::Status::kOk) {
            return fail(toCapi(status),
                        std::string("xt_surface_post_system_event: unknown kind '") + kind + "'");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_hot_reload(xt_surface* s) {
    XT_CAPI_GUARD("xt_surface_hot_reload", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_hot_reload");
        if (s->dispatchDepth > 0) {
            return fail(XT_STATUS_UNSUPPORTED, "xt_surface_hot_reload: called from inside a callback");
        }
        xt::capi::DispatchScope scope(*s);
        xt::Result<xt::DocumentHandle> reloaded =
            xt::bundle::reloadUi(*s->ctx, *s->owner->bundle, s->handle, s->uiId);
        if (!reloaded.ok()) {
            return fail(toCapi(reloaded.status()), "xt_surface_hot_reload: cannot reload UI '" + s->uiId +
                                                       "': " + xt::statusName(reloaded.status()));
        }
        xt::Document* doc = s->ctx->document(s->handle);
        if (doc == nullptr) {
            return fail(XT_STATUS_INTERNAL, "xt_surface_hot_reload: the reloaded Document is gone");
        }
        // 23.x items 2-3: deregister and register the UI again, then load its flows again, here
        // (outside any callback). The global flow is loaded again too: after removeUi() its edges
        // to this UI reach nothing (BundleContext::removeUi).
        (void)s->bundleCtx->removeUi(s->uiId);
        const xt::Status added = s->bundleCtx->addUi(s->uiId, s->handle);
        if (added != xt::Status::kOk) {
            return fail(toCapi(added), "xt_surface_hot_reload: BundleContext::addUi failed");
        }
        if (s->runFlow) {
            xt::capi::loadFlows(*s);
        }
        std::vector<std::string> warnings;
        (void)s->ports->rebind(*doc, warnings);
        for (const std::string& warning : warnings) {
            xt::capi::log(1, "xt_surface_hot_reload: " + warning);
        }
        xt::capi::attachPortListeners(*s);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_play_timeline(xt_surface* s, const char* scope_path, const char* name,
                                           const xt_play_options* opt) {
    XT_CAPI_GUARD("xt_surface_play_timeline", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_play_timeline");
        if (name == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_play_timeline: name is NULL");
        }
        xt::PlayOptions options;
        if (opt != nullptr) {
            if (opt->struct_size < sizeof(xt_play_options)) {
                return fail(XT_STATUS_INVALID_ARGUMENT,
                            "xt_surface_play_timeline: opt->struct_size is too small");
            }
            options.reverse = opt->reverse != 0;
            options.loop = opt->loop != 0;
            options.once = opt->once != 0;
            options.hold = opt->hold != 0;
            options.fromMarker = opt->from_marker != nullptr ? opt->from_marker : "";
            options.rate = opt->rate;
        }
        xt::capi::DispatchScope scope(*s);
        const xt::Status status =
            s->ctx->playTimeline(scope_path != nullptr ? scope_path : "", name, options);
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), std::string("xt_surface_play_timeline: cannot play '") + name + "'");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_stop_timeline(xt_surface* s, const char* scope_path, const char* name) {
    XT_CAPI_GUARD("xt_surface_stop_timeline", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_stop_timeline");
        if (name == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_stop_timeline: name is NULL");
        }
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->ctx->stopTimeline(scope_path != nullptr ? scope_path : "", name);
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), std::string("xt_surface_stop_timeline: no timeline '") + name + "'");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_surface_seek_timeline(xt_surface* s, const char* scope_path, const char* name, float t) {
    XT_CAPI_GUARD("xt_surface_seek_timeline", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_seek_timeline");
        if (name == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_seek_timeline: name is NULL");
        }
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->ctx->seekTimeline(scope_path != nullptr ? scope_path : "", name, t);
        if (status != xt::Status::kOk) {
            return fail(toCapi(status), std::string("xt_surface_seek_timeline: no timeline '") + name + "'");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI int xt_surface_is_timeline_playing(const xt_surface* s, const char* scope_path, const char* name) {
    XT_CAPI_GUARD_OR("xt_surface_is_timeline_playing", int, 0, {
        if (s == nullptr || name == nullptr) {
            return 0;
        }
        const xt::Document* doc = s->ctx->document(s->handle);
        return doc != nullptr && doc->isTimelinePlaying(scope_path != nullptr ? scope_path : "", name) ? 1
                                                                                                       : 0;
    });
}

XT_CAPI xt_status xt_surface_set_world_anchor(xt_surface* s, uint32_t port, float x, float y, float depth,
                                              int on_screen) {
    XT_CAPI_GUARD("xt_surface_set_world_anchor", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_set_world_anchor");
        xt::capi::DispatchScope scope(*s);
        const xt::Status status = s->ctx->setWorldAnchor(s->handle, port, x, y, depth, on_screen != 0);
        if (status != xt::Status::kOk) {
            return fail(toCapi(status),
                        "xt_surface_set_world_anchor: not a vec2/vec3 port or not a finite point");
        }
        return XT_STATUS_OK;
    });
}

XT_CAPI void xt_surface_set_perf_enabled(xt_surface* s, int enabled) {
    XT_CAPI_GUARD_VOID("xt_surface_set_perf_enabled", {
        if (s != nullptr) {
            s->ctx->perf().setEnabled(enabled != 0);
        }
    });
}

XT_CAPI xt_status xt_surface_frame_stats(const xt_surface* s, xt_frame_stats* out) {
    XT_CAPI_GUARD("xt_surface_frame_stats", {
        XT_CAPI_REQUIRE_SURFACE(s, "xt_surface_frame_stats");
        if (out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_surface_frame_stats: out is NULL");
        }
        const xt::FrameStats& frame = s->ctx->perf().current();
        out->update_ms = frame.updateMs;
        out->render_ms = frame.renderMs;
        out->executed_draws = s->renderer.lastExecutedDrawCommands();
        out->skipped_draws = s->renderer.lastSkippedDrawCommands();
        return XT_STATUS_OK;
    });
}

XT_CAPI uint32_t xt_debug_surface_input_count(const xt_surface* s) {
    XT_CAPI_GUARD_OR("xt_debug_surface_input_count", uint32_t, 0u,
                     { return s == nullptr ? 0u : s->debugInputCount; });
}

}  // extern "C"
