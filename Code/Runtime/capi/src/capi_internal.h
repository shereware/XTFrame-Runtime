#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/bundle/bundle.h"
#include "xt/capi/xtframe.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/flow/bundle_context.h"
#include "xt/flow/host_ports.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

#ifdef XT_CAPI_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

/// Internals of the C ABI shared library (Docs/design/50-c-abi.md §50.2-50.3, Docs/tasks/T-504.md):
/// the objects behind the opaque `xt_bundle` / `xt_surface` handles, the thread-local last error,
/// the log sink and the exception guard every exported function runs its body in.

namespace xt::capi {

/// Strings handed out by one object: a ring of kScratchSlots, so a pointer stays valid at least
/// until the next call on that object (50.2 "strings").
inline constexpr size_t kScratchSlots = 8;

class Scratch {
public:
    /// Copies `text` into the next slot and returns its NUL-terminated copy.
    const char* put(std::string_view text) {
        std::string& slot = m_slots[m_next];
        m_next = (m_next + 1) % kScratchSlots;
        slot.assign(text.data(), text.size());
        return slot.c_str();
    }

private:
    std::array<std::string, kScratchSlots> m_slots;
    size_t m_next = 0;
};

/// This thread's last failure message (50.2 "errors"); never cleared by a successful call.
void setLastError(std::string message);
const char* lastError();

/// The xt_capi_set_log sink (level 0 info, 1 warning, 2 error); a no-op while none is set.
void log(int level, const std::string& message);

/// Sets the last error and returns `status` (for the `return fail(...)` of an exported function).
inline xt_status fail(xt_status status, std::string message) {
    setLastError(std::move(message));
    return status;
}

/// xt::Status -> xt_status (same values, 50.3).
inline xt_status toCapi(Status status) { return static_cast<xt_status>(status); }

/// Runs `body` and turns any exception into `failValue` plus a last error naming `function`.
template <class R, class F>
R guard(const char* function, R failValue, F&& body) noexcept {
    try {
        return body();
    } catch (...) {
        try {
            setLastError(std::string("unexpected exception in ") + function);
        } catch (...) {
        }
        return failValue;
    }
}

/// The void form of guard().
template <class F>
void guardVoid(const char* function, F&& body) noexcept {
    try {
        body();
    } catch (...) {
        try {
            setLastError(std::string("unexpected exception in ") + function);
        } catch (...) {
        }
    }
}

/// Runs a callback body handed to the C++ runtime: an exception must never unwind through xt_core
/// (built without exceptions), so it is logged and dropped here.
template <class F>
void callbackGuard(const char* where, F&& body) noexcept {
    try {
        body();
    } catch (...) {
        try {
            log(2, std::string("unexpected exception in ") + where);
        } catch (...) {
        }
    }
}

/// Shortest fixed text of a number, as Document::portText() writes a Number port (up to 6
/// decimals, trailing zeros and a bare '.' trimmed, "-0" -> "0", non-finite -> "0"). The core's
/// own function (binding_runtime_detail::numText) is not public; this mirrors it through the
/// locale-independent xt::formatFixed.
std::string numberText(double value);

/// Decodes `size` bytes of UTF-8 into code points' byte ranges (offset, length), rejecting
/// overlong forms, surrogates, values above U+10FFFF, stray continuation bytes and truncation.
bool splitUtf8(const char* text, size_t size, std::vector<std::pair<size_t, uint8_t>>& out);

}  // namespace xt::capi

/// Exception guards for the exported functions (Docs/tasks/T-504.md "capi_internal.h"): the body is
/// a braced block, run in a lambda. XT_CAPI_GUARD returns its xt_status (XT_STATUS_INTERNAL on an
/// exception); XT_CAPI_GUARD_OR returns `type` (`fail` on an exception); XT_CAPI_GUARD_VOID
/// returns nothing.
#define XT_CAPI_GUARD(function, ...) \
    return ::xt::capi::guard<xt_status>(function, XT_STATUS_INTERNAL, [&]() -> xt_status __VA_ARGS__)
#define XT_CAPI_GUARD_OR(function, type, failValue, ...) \
    return ::xt::capi::guard<type>(function, failValue, [&]() -> type __VA_ARGS__)
#define XT_CAPI_GUARD_VOID(function, ...) ::xt::capi::guardVoid(function, [&]() -> void __VA_ARGS__)
/// The NULL check a surface entry point returning xt_status starts with (inside its guard).
#define XT_CAPI_REQUIRE_SURFACE(s, function)                                                              \
    if ((s) == nullptr) {                                                                                 \
        return ::xt::capi::fail(XT_STATUS_INVALID_ARGUMENT, std::string(function) + ": surface is NULL"); \
    }

/// The object behind `xt_bundle*` (50.3 `xt_surface_create`): the opened Bundle, whose
/// FileAssetResolver every surface of it shares, and the shaper they share, built lazily.
struct xt_bundle {
    std::unique_ptr<xt::bundle::Bundle> bundle;
    /// Built by the first xt_surface_create(); FtTextShaper with XT_BUILD_TEXT, else NullTextShaper.
    std::unique_ptr<xt::TextShaper> shaper;
#ifdef XT_CAPI_HAS_TEXT
    /// `shaper` as the FreeType shaper (for addSystemFonts()); null until it is built.
    xt::text::FtTextShaper* ftShaper = nullptr;
#endif
    bool systemFontsAdded = false;
    /// Bundle::reload()'s result, for xt_bundle_changed_ui_id().
    std::vector<std::string> changedIds;
    /// Surfaces created on this bundle and not destroyed yet.
    uint32_t surfaceCount = 0;
    /// Callback dispatches running on any of its surfaces (xt_bundle_close refuses inside one).
    uint32_t dispatchDepth = 0;
    xt::capi::Scratch scratch;
};

/// The object behind `xt_surface*` (50.1: one Context + one Document + one SwRenderer + one
/// BundleContext + one HostPorts). It is also the ContextListener that reports onShown/onHidden.
struct xt_surface : public xt::ContextListener {
    xt_bundle* owner = nullptr;
    std::string uiId;
    xt::bundle::BundleSpec spec;
    // Teardown order (50.3): ports -> bundle context (with its flows) -> context; see teardown().
    std::unique_ptr<xt::Context> ctx;
    xt::DocumentHandle handle = 0;
    std::unique_ptr<xt::flow::BundleContext> bundleCtx;
    std::unique_ptr<xt::flow::HostPorts> ports;
    xt::sw::SwRenderer renderer;
    /// The target last given to renderer.setTarget(); `rgba == nullptr` = none yet.
    xt::sw::SwTarget target;
    xt::DamageList damage;
    xt::CommandList list;
    uint32_t timelineListener = 0;
    /// HostPorts::addInputListener ids, one per declared in-port of the bound Document.
    std::vector<uint32_t> inputListeners;
    /// connection_watch ids and the names they watch.
    std::vector<std::pair<uint32_t, std::string>> watches;
    /// HostPorts::portTable() of the bound Document (refreshed by bind / rebind).
    std::vector<xt::PortRecord> portTable;
    bool runFlow = false;
    bool globalFlowLoaded = false;
    // Viewport and device.
    int width = 0;
    int height = 0;
    float dpr = 1.0f;
    bool premultiplied = false;
    /// The device xt_surface_create starts from (its safe area); afterwards the Context's own DeviceInfo
    /// (`ctx->device()`) is the one every call changes (T-552, capi review K1).
    xt::DeviceInfo device;
    float uiScaleOverride = 0.0f;
    // Callbacks.
    xt_port_event_fn portFn = nullptr;
    void* portUser = nullptr;
    xt_host_event_fn hostFn = nullptr;
    void* hostUser = nullptr;
    xt_timeline_event_fn timelineFn = nullptr;
    void* timelineUser = nullptr;
    xt_visibility_fn visibilityFn = nullptr;
    void* visibilityUser = nullptr;
    /// T-561 (50.3 xt_surface_set_update_hook): called from onBeforeDocumentUpdate.
    xt_update_hook_fn updateHookFn = nullptr;
    void* updateHookUser = nullptr;
    /// The dt of the xt_surface_update() running now (handed to the update hook).
    float updateDt = 0.0f;
    /// Calls running that may call back (50.3: destroy / hot_reload / bundle_close refuse inside).
    uint32_t dispatchDepth = 0;
    uint32_t debugInputCount = 0;
    xt::capi::Scratch scratch;

    ~xt_surface() override;
    /// Takes everything down in the 50.3 order; safe to call twice.
    void teardown();

    void onShown(xt::DocumentHandle document) override;
    void onHidden(xt::DocumentHandle document) override;
    void onBeforeDocumentUpdate(xt::Context& context) override;
};

namespace xt::capi {

/// Counts a call that may call back (on the surface and its bundle) for its whole scope.
class DispatchScope {
public:
    explicit DispatchScope(xt_surface& surface) : m_surface(surface) {
        ++m_surface.dispatchDepth;
        ++m_surface.owner->dispatchDepth;
    }
    ~DispatchScope() {
        --m_surface.dispatchDepth;
        --m_surface.owner->dispatchDepth;
    }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;

private:
    xt_surface& m_surface;
};

/// The bundle's shared shaper, built on first use (bundle fonts first, then the system fonts once
/// when `systemFonts` asks for them; 50.3 and Docs/tasks/T-504.md item 2).
TextShaper* ensureShaper(xt_bundle& bundle, bool systemFonts);

/// Reads `<bundle dir>/<relative>` through xt_bundle's own file reader (the setBundleFileReader
/// hook applies). False when it cannot be read.
bool readBundleFile(const bundle::Bundle& bundle, std::string_view relative, std::string& out);

/// (Re)registers the surface's HostPorts listeners on the bound Document: the in-port listeners
/// (one per declared `dir == In` port) and the cached port table. Called after bind / rebind.
void attachPortListeners(xt_surface& surface);
/// Registers the emit listener (once; HostPorts keeps it across rebind()).
void attachEmitListener(xt_surface& surface);

/// Applies the uiscale of the current viewport: the override when > 0, else the spec rule.
void applyUiScale(xt_surface& surface);

}  // namespace xt::capi
