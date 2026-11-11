/*
 * xtframe.h -- the C ABI of the XTFrame runtime (shared library `xtframe`: xtframe.dll /
 * libxtframe.so / libxtframe.dylib). See Docs/design/50-c-abi.md (50.1-50.5), Docs/tasks/T-504.md.
 *
 * Pure C99: no C++ and no other XTFrame header, so FFI hosts (Unity P/Invoke, Godot, Python,
 * Rust) can bind it directly. Every function wraps an existing C++ call of the runtime
 * (Docs/design/13, 22 and 23); the host rules of those chapters apply unchanged.
 *
 * Conventions (50.2):
 *  - Calling convention cdecl. The library starts no thread. Callbacks run inside xt_surface_update /
 *    xt_surface_port_set_* / xt_surface_hot_reload (and the other calls that drive the document), on
 *    the caller's thread.
 *  - Threads (50.1, T-652): one thread per bundle. The calls on one bundle and on the surfaces created
 *    from it are never made in parallel; different bundles, each with its own surfaces, may be driven
 *    on different threads at the same time. xt_capi_abi_version, xt_capi_version_string and
 *    xt_capi_last_error may be called from any thread at any time. The log callback of
 *    xt_capi_set_log is process-wide and may be called from every thread that drives a bundle, at the
 *    same time. An xt_gpu_target belongs to no bundle: the calls that use one target are never made in
 *    parallel.
 *  - Strings passed in are UTF-8, NUL-terminated and only need to live for the call (the library
 *    copies them). A `const char*` handed out is owned by the object it came from and stays valid
 *    until the next call on that object; copy it to keep it.
 *  - Functions returning xt_status give the values of xt::Status. When a call fails,
 *    xt_capi_last_error() names the reason (thread-local; a successful call does not clear it).
 *  - Structures that start with `struct_size` must have it set to sizeof(the structure).
 *  - XT_CAPI_ABI_VERSION changes when a function's meaning changes; functions are only ever added.
 *    A host compares xt_capi_abi_version() with the value it was built against at start-up.
 */
#ifndef XT_CAPI_XTFRAME_H
#define XT_CAPI_XTFRAME_H

#include <stddef.h>
#include <stdint.h>

/* XT_CAPI_STATIC: the static library of the Emscripten build (libxtframe.a, Docs/design/52-web-wasm.md
   52.9); no import / export attribute. */
#if defined(XT_CAPI_STATIC)
#define XT_CAPI
#elif defined(_WIN32)
#if defined(XT_CAPI_BUILD)
#define XT_CAPI __declspec(dllexport)
#else
#define XT_CAPI __declspec(dllimport)
#endif
#else
#define XT_CAPI __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The declarations below are the 50.3 contract, kept in its layout. */
/* clang-format off */

#define XT_CAPI_ABI_VERSION 1

typedef enum xt_status {            /* = xt::Status */
    XT_STATUS_OK = 0, XT_STATUS_INVALID_ARGUMENT, XT_STATUS_INVALID_IR, XT_STATUS_NOT_FOUND,
    XT_STATUS_OUT_OF_RANGE, XT_STATUS_UNSUPPORTED, XT_STATUS_IO_ERROR, XT_STATUS_INTERNAL,
    XT_STATUS_INCOMPATIBLE,
    XT_STATUS_BUSY                  /* T-636 (50.z, appended; no xt::Status counterpart): still in use */
} xt_status;

typedef struct xt_bundle xt_bundle;      /* opaque */
typedef struct xt_surface xt_surface;    /* opaque */

/* --- Library --- */
XT_CAPI uint32_t    xt_capi_abi_version(void);
XT_CAPI const char* xt_capi_version_string(void);          /* xt::versionString() */
XT_CAPI const char* xt_capi_last_error(void);              /* thread-local; "" = none */
/* Flow errors, rebind warnings and load diagnostics: level 0 info, 1 warning, 2 error. Dropped while
   no function is set. */
typedef void (*xt_log_fn)(int level, const char* message, void* user);
XT_CAPI void        xt_capi_set_log(xt_log_fn fn, void* user);

/* --- Bundle (a published directory bundle, Docs/design/22; read-only) --- */
/* xt_bundle_open: a manifest with `strings` also loads its string tables (T-617); every surface then
   starts in `spec.defaultLanguage` (else the first strings language) with its `@key` text resolved. */
XT_CAPI xt_bundle*  xt_bundle_open(const char* dir_utf8);  /* NULL + last_error */
XT_CAPI xt_status   xt_bundle_close(xt_bundle* b);         /* surfaces still open -> INVALID_ARGUMENT, not closed */
XT_CAPI const char* xt_bundle_id(const xt_bundle* b);
XT_CAPI const char* xt_bundle_name(const xt_bundle* b);
XT_CAPI const char* xt_bundle_dir(const xt_bundle* b);

typedef struct xt_ui_info {
    const char* id;  const char* name;
    float design_width, design_height;   /* UiInfo::size */
    const char* fit;                     /* manifest uis[].fit: native|stretch|contain|cover|center */
    int has_flow;                        /* UiInfo::flow is not empty */
    uint32_t port_count;                 /* rows of manifest uis[].ports (connection-point contract) */
    const char* contract;                /* contract hash, may be "" */
} xt_ui_info;
XT_CAPI uint32_t    xt_bundle_ui_count(const xt_bundle* b);
XT_CAPI xt_status   xt_bundle_ui_info(const xt_bundle* b, uint32_t index, xt_ui_info* out);
XT_CAPI int32_t     xt_bundle_find_ui(const xt_bundle* b, const char* ui_id); /* index or -1 */

typedef struct xt_port_info {            /* one row of manifest uis[].ports (PortInfo) */
    const char* name;                    /* "#fire.Click" / "document.clock" */
    const char* type;                    /* pulse|bool|number|string */
    int is_input;
} xt_port_info;
XT_CAPI xt_status   xt_bundle_ui_port_info(const xt_bundle* b, uint32_t ui_index, uint32_t port_index,
                                           xt_port_info* out);

typedef struct xt_bundle_spec {          /* BundleSpec; the defaults when the manifest has no spec */
    float logical_height;                /* 1080 */
    int   uiscale_rule;                  /* UiScaleRule: 0 height 1 width 2 shortEdge 3 fit */
    float safe_area_default, safe_area_console;
    int   uiscale_clamped; float uiscale_clamp_min, uiscale_clamp_max, uiscale_dpi_floor;
} xt_bundle_spec;
XT_CAPI xt_status   xt_bundle_get_spec(const xt_bundle* b, xt_bundle_spec* out);

/* Hot reload: re-reads the manifest and returns the number of changed UIs; the ids stay valid until
   the next reload. Then call xt_surface_hot_reload on every affected surface. */
XT_CAPI uint32_t    xt_bundle_reload(xt_bundle* b);
XT_CAPI const char* xt_bundle_changed_ui_id(const xt_bundle* b, uint32_t index);

/* --- Surface ---
   Threads (50.1, T-652): one thread per bundle -- the calls on a surface and on the bundle it came from
   (and its other surfaces) are never made in parallel; surfaces of different bundles may run on
   different threads at the same time. The library takes no lock on them and starts no thread. */
typedef struct xt_surface_desc {
    uint32_t struct_size;                /* = sizeof(xt_surface_desc) */
    int   width, height;                 /* output pixels (device pixels) */
    float device_pixel_ratio;            /* 0 -> 1 */
    int   premultiplied;                 /* SwTarget::premultiplied */
    int   run_flow;                      /* 1: load the UI's .xtflow and bundle.xtflow (BundleContext) */
    int   system_fonts;                  /* 1: the bundle's shaper adds the system fonts (once per bundle) */
    uint32_t instance_id;                /* Context::setInstanceId / HostPorts::setInstanceId */
} xt_surface_desc;
XT_CAPI xt_surface* xt_surface_create(xt_bundle* b, const char* ui_id, const xt_surface_desc* desc);
XT_CAPI void        xt_surface_destroy(xt_surface* s);     /* not from inside a callback: the host defers it */
XT_CAPI const char* xt_surface_ui_id(const xt_surface* s);

/* Viewport: logical size = (width, height) / dpr; uiscale follows the bundle spec's rule
   (bundle::uiScaleFor(rule, logicalW, logicalH, logicalHeight)) until set_ui_scale overrides it. */
XT_CAPI xt_status   xt_surface_resize(xt_surface* s, int width, int height, float device_pixel_ratio);
XT_CAPI xt_status   xt_surface_set_ui_scale(xt_surface* s, float scale);      /* override; <= 0 back to automatic */
XT_CAPI float       xt_surface_ui_scale(const xt_surface* s);
XT_CAPI xt_status   xt_surface_set_safe_area(xt_surface* s, float top, float right, float bottom, float left);
XT_CAPI xt_status   xt_surface_set_input_kind(xt_surface* s, int kind);     /* InputKind 0 mouse 1 gamepad 2 touch */

/* Input (Docs/design/08; the value ranges of xt::InputEvent; kind = the InputEvent::Kind order) */
typedef enum xt_input_kind {
    XT_INPUT_POINTER_MOVE = 0, XT_INPUT_POINTER_DOWN, XT_INPUT_POINTER_UP, XT_INPUT_WHEEL,
    XT_INPUT_KEY_DOWN, XT_INPUT_KEY_UP, XT_INPUT_TEXT_INPUT, XT_INPUT_POINTER_LEAVE,
    XT_INPUT_COMPOSITION_START, XT_INPUT_COMPOSITION_UPDATE, XT_INPUT_COMPOSITION_END,
    XT_INPUT_GAMEPAD_BUTTON_DOWN, XT_INPUT_GAMEPAD_BUTTON_UP, XT_INPUT_GAMEPAD_AXIS_MOTION,
    XT_INPUT_GESTURE, XT_INPUT_POINTER_CANCEL
} xt_input_kind;
typedef struct xt_input_event {
    uint32_t struct_size;
    int32_t  kind;                       /* xt_input_kind */
    float    x, y;                       /* logical pixels */
    int32_t  button;                     /* PointerButton 0 left 1 right 2 middle */
    float    wheel_x, wheel_y;           /* notches; 08.11 */
    uint16_t key;                        /* xt::KeyCode value */
    uint8_t  modifiers, repeat;          /* Modifiers bits; auto-repeat */
    const char* text; uint32_t text_len; /* TEXT_INPUT: UTF-8 of any length, split by the library into one
                                            event per code point; COMPOSITION_UPDATE: the composition */
    uint32_t composition_cursor, composition_sel_len;
    uint32_t pointer_id;
    uint8_t  gamepad_button, gamepad_axis, player, clicks;
    float    axis_value;
    uint8_t  gesture, direction; float gesture_scale;
} xt_input_event;
XT_CAPI xt_status   xt_surface_push_input(xt_surface* s, const xt_input_event* e);
XT_CAPI void        xt_surface_cancel_pointer(xt_surface* s);
XT_CAPI int         xt_surface_cursor(const xt_surface* s);                /* xt::Cursor */
XT_CAPI int         xt_surface_wants_text_input(const xt_surface* s);
XT_CAPI xt_status   xt_surface_text_input_rect(const xt_surface* s, float* x, float* y, float* w, float* h);

/* Every frame: update (Context::update -> BundleContext::tick -> HostPorts::tick; the callbacks run
   here), then render into the host's buffer. After the first frame / a resize / a reload damage.full = 1. */
XT_CAPI xt_status   xt_surface_update(xt_surface* s, float dt);
typedef struct xt_rect { int x, y, w, h; } xt_rect;      /* device pixels, top-left origin */
typedef struct xt_damage {
    int full;                            /* the whole buffer is dirty */
    int changed;                         /* 0: no pixel changed this frame, the host need not upload */
    uint32_t rect_count; xt_rect rects[16];   /* kMaxDamageRects */
} xt_damage;
/* rgba: width x height RGBA8, rows stride_bytes apart; stride_bytes >= width * 4 (computed in size_t: a
   negative stride, or a width whose row does not fit in an int, is XT_STATUS_INVALID_ARGUMENT). */
XT_CAPI xt_status   xt_surface_render(xt_surface* s, void* rgba, int stride_bytes, xt_damage* out_damage);

/* Lifecycle and appearance */
XT_CAPI xt_status   xt_surface_request_show(xt_surface* s);
XT_CAPI xt_status   xt_surface_request_hide(xt_surface* s);
XT_CAPI xt_status   xt_surface_set_visible(xt_surface* s, int visible);    /* no intro/outro */
XT_CAPI int         xt_surface_is_visible(const xt_surface* s);
XT_CAPI xt_status   xt_surface_set_theme(xt_surface* s, const char* name);  /* "" clears */
/* set_language: Context::setLanguage (every `@key` text and `localize` binding re-resolved); with a
   strings bundle the library then posts the system event languageChanged = lang to the surface's
   flows itself (the host need not; posting it again is harmless). Without strings: language only. */
XT_CAPI xt_status   xt_surface_set_language(xt_surface* s, const char* lang);
XT_CAPI xt_status   xt_surface_post_system_event(xt_surface* s, const char* kind, const char* value);
XT_CAPI xt_status   xt_surface_hot_reload(xt_surface* s);   /* reloadUi + flow rebind + ports rebind */

typedef struct xt_play_options { uint32_t struct_size; int reverse, loop, once, hold; const char* from_marker; float rate; } xt_play_options;
XT_CAPI xt_status   xt_surface_play_timeline(xt_surface* s, const char* scope_path, const char* name,
                                             const xt_play_options* opt);   /* opt may be NULL */
XT_CAPI xt_status   xt_surface_stop_timeline(xt_surface* s, const char* scope_path, const char* name);
XT_CAPI xt_status   xt_surface_seek_timeline(xt_surface* s, const char* scope_path, const char* name, float t);
XT_CAPI int         xt_surface_is_timeline_playing(const xt_surface* s, const char* scope_path, const char* name);

/* Declared ports (27.3 `port:`; the ids equal the constants of `xtframe-pack --gen-ports cs`) */
#define XT_INVALID_PORT 0u
typedef struct xt_port_record {
    uint32_t id; const char* name; const char* qualified_name; const char* element_id;
    int value_type;                      /* PortValueType */
    int dir;                             /* PortDir 0 in 1 out 2 call */
    int document_level;
} xt_port_record;
XT_CAPI uint32_t    xt_surface_port_count(const xt_surface* s);
XT_CAPI xt_status   xt_surface_port_record(const xt_surface* s, uint32_t index, xt_port_record* out);
XT_CAPI uint32_t    xt_surface_port_lookup(const xt_surface* s, const char* name);
XT_CAPI xt_status   xt_surface_port_set_number(xt_surface* s, uint32_t port, double value);
XT_CAPI xt_status   xt_surface_port_set_bool(xt_surface* s, uint32_t port, int value);
XT_CAPI xt_status   xt_surface_port_set_text(xt_surface* s, uint32_t port, const char* text); /* Document::setPortText: any type, struct/list take JSON */
XT_CAPI xt_status   xt_surface_port_pulse(xt_surface* s, uint32_t port);   /* Event ports: only fires port(<name>) */
XT_CAPI double      xt_surface_port_get_number(const xt_surface* s, uint32_t port);
XT_CAPI int         xt_surface_port_get_bool(const xt_surface* s, uint32_t port);
XT_CAPI const char* xt_surface_port_get_text(const xt_surface* s, uint32_t port);  /* Document::portText */

/* Connection points (17.6; the names of manifest uis[].ports; HostPorts::setInput / addOutputListener) */
XT_CAPI xt_status   xt_surface_connection_set_number(xt_surface* s, const char* port, double value, const char* param);
XT_CAPI xt_status   xt_surface_connection_set_bool(xt_surface* s, const char* port, int value, const char* param);
XT_CAPI xt_status   xt_surface_connection_set_text(xt_surface* s, const char* port, const char* text, const char* param);
XT_CAPI xt_status   xt_surface_connection_pulse(xt_surface* s, const char* port, const char* param);
XT_CAPI xt_status   xt_surface_connection_watch(xt_surface* s, const char* port, uint32_t* out_listener_id);
XT_CAPI void        xt_surface_connection_unwatch(xt_surface* s, uint32_t listener_id);

/* Callbacks (one of each per surface; NULL removes it) */
typedef enum xt_port_event_kind { XT_PORT_EMIT = 0, XT_PORT_INPUT_CHANGED = 1, XT_PORT_CONNECTION = 2 } xt_port_event_kind;
typedef struct xt_port_event {
    int kind;                            /* xt_port_event_kind */
    uint32_t port;                       /* EMIT / INPUT_CHANGED: PortId; CONNECTION: 0 */
    uint32_t listener_id;                /* CONNECTION: the id connection_watch gave; otherwise 0 */
    const char* name;                    /* port name (EMIT: qualified_name; CONNECTION: the name watched) */
    int value_type;                      /* EMIT / INPUT_CHANGED: PortValueType; CONNECTION: PortType 0 pulse 1 bool 2 number 3 string */
    double number; int boolean; const char* text;   /* filled by type; text always (numbers / bools as text too) */
    uint32_t instance_id, item_index;    /* PortEvent::instanceId / itemIndex (0xFFFFFFFF = none) */
} xt_port_event;
typedef void (*xt_port_event_fn)(xt_surface* s, const xt_port_event* e, void* user);
XT_CAPI xt_status   xt_surface_set_port_listener(xt_surface* s, xt_port_event_fn fn, void* user);

typedef struct xt_host_event {           /* the flow's FlowHostEvent: sound / haptic / spawn / emit ... */
    int kind;                            /* FlowHostEventKind */
    const char* name; float volume; const char* ui_id; uint32_t instance_id; const char* node_id;
} xt_host_event;
typedef void (*xt_host_event_fn)(xt_surface* s, const xt_host_event* e, void* user);
XT_CAPI xt_status   xt_surface_set_host_event_listener(xt_surface* s, xt_host_event_fn fn, void* user);

typedef struct xt_timeline_event { int kind; const char* timeline; const char* marker; } xt_timeline_event; /* TimelineEventKind */
typedef void (*xt_timeline_event_fn)(xt_surface* s, const xt_timeline_event* e, void* user);
XT_CAPI xt_status   xt_surface_set_timeline_listener(xt_surface* s, xt_timeline_event_fn fn, void* user);

typedef void (*xt_visibility_fn)(xt_surface* s, int visible, void* user);   /* onShown / onHidden */
XT_CAPI xt_status   xt_surface_set_visibility_listener(xt_surface* s, xt_visibility_fn fn, void* user);

/* World anchors (23.10 item 5; Context::setWorldAnchor) */
XT_CAPI xt_status   xt_surface_set_world_anchor(xt_surface* s, uint32_t port, float x, float y, float depth, int on_screen);

/* Performance (Docs/design/21; PerfTracker is off by default, the host turns it on) */
typedef struct xt_frame_stats { float update_ms, render_ms; uint32_t executed_draws, skipped_draws; } xt_frame_stats;
XT_CAPI void        xt_surface_set_perf_enabled(xt_surface* s, int enabled);
XT_CAPI xt_status   xt_surface_frame_stats(const xt_surface* s, xt_frame_stats* out);

/* clang-format on */

/* Test-only (Docs/design/50-c-abi.md 50.4 case 7): the number of xt::InputEvents this surface has
   handed to its Context so far (a TEXT_INPUT event counts once per code point). Not for hosts. */
XT_CAPI uint32_t xt_debug_surface_input_count(const xt_surface* s);

/* T-561 (Docs/design/50-c-abi.md 50.3; appended, ABI version unchanged): called once inside
   xt_surface_update, after input dispatch (and xt_flow's pulse edges when run_flow = 1) and before
   style resolution, so writes made here (port_*, connection_*, play_timeline) take effect this frame.
   A write after xt_surface_update returns takes effect at the next update. destroy / hot_reload
   refuse inside it like in any callback. NULL removes it. */
typedef void (*xt_update_hook_fn)(xt_surface* s, float dt, void* user);
XT_CAPI xt_status xt_surface_set_update_hook(xt_surface* s, xt_update_hook_fn fn, void* user);

/* T-635 (Docs/design/50-c-abi.md 50.y; appended, ABI version unchanged): the runtime state snapshot
   (Document::snapshotState / restoreState): every port value (event ports excepted), the custom
   states, input text and selection, focus and scroll position of the elements with an id, and the
   active @timeline groups. Not included: the IR, resources and flow node state; restore on the same
   bundle version (another version only applies ports and custom states best-effort).
   xt_surface_save_state: the snapshot as JSON. The string is owned by the surface and stays valid
   until the next xt_surface_save_state, xt_surface_destroy or xt_surface_hot_reload on that surface
   (other calls do not invalidate it); copy it to keep it. NULL + last_error on failure.
   xt_surface_load_state: applies a save_state text, after create and before the first
   xt_surface_update or at any time; ports are written as a host write (listeners notified,
   port(<name>) triggers fired), id'd elements the snapshot does not list lose their custom states
   and scroll offset, timeline groups not in it are released. NULL or not a snapshot (unparsable
   JSON, not an object, another version) -> XT_STATUS_INVALID_ARGUMENT with nothing applied; from
   inside a callback -> XT_STATUS_UNSUPPORTED (as hot_reload).
   T-650 (50.y addendum): version 2 snapshots add the runtime classes of the id'd elements, the
   document clock and whether the initial show was done; version 1 snapshots still load. Loaded
   before the first xt_surface_update, a snapshot of a shown surface skips the initial show: no
   xt_visibility_fn "shown", no intro started by it -- the first frame is the saved one.
   T-651: version 3 snapshots add the CSS `animation` items, put back at their saved position;
   versions 1 and 2 still load. */
XT_CAPI const char* xt_surface_save_state(xt_surface* s);
XT_CAPI xt_status xt_surface_load_state(xt_surface* s, const char* json);

/* T-636 (Docs/design/50-c-abi.md 50.z; appended, ABI version unchanged): the GPU target, drawn with
   xt_wgpu (wgpu-native, shipped next to the library). The library owns the WebGPU instance, adapter,
   device and swapchain (or offscreen texture) of a target. Frame order:
     xt_gpu_target_begin_frame -> xt_surface_render_gpu (any number, any surfaces) -> xt_gpu_target_end_frame.
   Surfaces are composited inside the target in call order (a later call is drawn over an earlier one,
   premultiplied source-over); `dest` is a rectangle in target pixels (NULL = the whole target). A
   surface renders at its own size (xt_surface_resize) and is scaled linearly into `dest` when the sizes
   differ (the host computes any fit rectangle).
   Attachment: the first xt_surface_render_gpu of a surface attaches it to that target (its own renderer,
   glyph atlas and texture cache on the target's device). A surface attached to one target given to
   another -> XT_STATUS_INVALID_ARGUMENT ("attached to another target"); a surface that has used
   xt_surface_render cannot use xt_surface_render_gpu, nor the reverse -> XT_STATUS_UNSUPPORTED, until
   xt_surface_detach_gpu (which also releases the GPU resources; destroying the surface does too).
   xt_gpu_target_destroy with surfaces still attached -> XT_STATUS_BUSY, nothing destroyed.
   Resize, device pixel ratio and input of a surface are unchanged. xt_frame_stats.render_ms of a GPU
   frame is the CPU time of building and submitting it (no present wait).
   A build without wgpu (XT_BUILD_WGPU=OFF): xt_gpu_target_create returns NULL with last_error
   "built without wgpu"; every other function here returns XT_STATUS_UNSUPPORTED.
   T-666 (behaviour change, ABI version unchanged): xt_gpu_target_desc.sample_count 0 -- and an older,
   shorter desc without the field -- means 1 from this version on; it meant 4 before. A host that wants
   4x MSAA passes 4. */
/* clang-format off */
typedef struct xt_gpu_target xt_gpu_target;  /* opaque */

typedef enum xt_window_kind {
    XT_WINDOW_NONE = 0,  /* no window: an offscreen RGBA8 texture (headless, conformance, readback) */
    XT_WINDOW_WIN32,
    XT_WINDOW_COCOA,
    XT_WINDOW_X11,
    XT_WINDOW_WAYLAND
} xt_window_kind;

typedef struct xt_native_window {
    int kind;  /* xt_window_kind */
    union {
        struct { void* hwnd; } win32;                          /* HWND */
        struct { void* ns_window; } cocoa;                     /* NSWindow*; call create on the main thread */
        struct { void* display; uint64_t window; } x11;        /* Display*, Window */
        struct { void* display; void* surface; } wayland;      /* wl_display*, wl_surface* */
    } u;
} xt_native_window;

typedef struct xt_gpu_target_desc {
    uint32_t struct_size;     /* = sizeof(xt_gpu_target_desc) */
    xt_native_window window;
    int width, height;        /* pixels (> 0) */
    int vsync;                /* 1: Fifo; 0: Immediate, else Mailbox, else Fifo (windows only) */
    const char* backend;      /* NULL or "" = the XT_WGPU_BACKEND rule; else vulkan|d3d12|metal|gl */
    int sample_count;         /* surface rendering MSAA: 0 = 1 (the default; T-666, it was 4), 1 or 4;
                                 other values fail. Read only when struct_size covers it: an older,
                                 shorter desc means 0 */
} xt_gpu_target_desc;

/* NULL + last_error when no adapter / device can be had (the host falls back to xt_surface_render),
   the window gives no surface, the backend name is unknown, or the build has no wgpu. */
XT_CAPI xt_gpu_target* xt_gpu_target_create(const xt_gpu_target_desc* desc);
/* Surfaces still attached -> XT_STATUS_BUSY (destroy them or xt_surface_detach_gpu first). NULL -> OK. */
XT_CAPI xt_status xt_gpu_target_destroy(xt_gpu_target* t);
/* New size in pixels (swapchain reconfigured / offscreen texture recreated); not inside a frame. */
XT_CAPI xt_status xt_gpu_target_resize(xt_gpu_target* t, int width, int height);
/* Starts a frame: acquires the swapchain texture and clears it to `clear_rgba` (straight alpha, each
   0..1; NULL = transparent). A window whose texture cannot be had this frame (minimized) still returns
   OK: the surfaces still render, nothing is composited or presented. */
XT_CAPI xt_status xt_gpu_target_begin_frame(xt_gpu_target* t, const float clear_rgba[4]);
/* Presents the frame (XT_WINDOW_NONE: only submits). */
XT_CAPI xt_status xt_gpu_target_end_frame(xt_gpu_target* t);
/* Blocking copy of the target into `rgba` (premultiplied RGBA8, row 0 = top, `stride` bytes per row,
   >= width * 4). XT_WINDOW_NONE: any time after end_frame (or inside a frame); a window: only between
   begin_frame and end_frame, and only when its swapchain allows copies (else UNSUPPORTED). For tests
   and conformance. */
XT_CAPI xt_status xt_gpu_target_readback(xt_gpu_target* t, void* rgba, int stride);
/* Renders `s` (after xt_surface_update, as xt_surface_render) and composites it into `dest` of the
   frame `t` has begun (outside begin_frame / end_frame -> XT_STATUS_INVALID_ARGUMENT). */
XT_CAPI xt_status xt_surface_render_gpu(xt_surface* s, xt_gpu_target* t, const xt_rect* dest);
/* Releases the GPU resources of `s` and detaches it from its target; then either render path may be
   used. A surface that is not attached -> OK (and it may use either path again). */
XT_CAPI xt_status xt_surface_detach_gpu(xt_surface* s);

/* ---- Hidden surfaces and memory (50 §50.aa, T-641) ------------------------------------------------
   xt_surface_trim: frees the surface's frame scratch now -- the software renderer's layer pool,
   effect buffers and masks; with a GPU target, all of its GPU resources (it stays attached; its
   textures go back to the target's pools, 50 §50.ac). Any time, inside callbacks too. A visible
   surface repaints whole on its next render.
   xt_surface_set_trim_on_hide: 1 (the default) = the first xt_surface_update after the surface became
   hidden (xt_surface_set_visible(s, 0), or a request_hide outro that ended) does the same for the
   software scratch; with a GPU target it gives back only its textures (the surface texture and its
   effect-cache targets, to the target's pools, which a surface shown later takes them from) and keeps
   its renderer (T-675). The frame that shows it again renders the same pixels. 0 keeps the scratch
   while hidden (a HUD toggled every few frames). */
XT_CAPI xt_status xt_surface_trim(xt_surface* s);
XT_CAPI xt_status xt_surface_set_trim_on_hide(xt_surface* s, int enabled);

/* ---- GPU target memory (50 §50.ab, T-656; appended, ABI version unchanged) -------------------------
   A target keeps one texture per bundle image content for all its surfaces. A texture no surface uses
   (its surfaces were destroyed, detached, trimmed or hidden) stays on the target's idle list, so a
   surface shown again does not upload it again; once the idle textures together exceed the idle
   budget (default 64 MiB), the ones left unused longest are destroyed. Without wgpu every function
   here returns XT_STATUS_UNSUPPORTED. */
/* Sets the idle budget of `t`'s image textures in bytes and applies it now (the oldest idle textures
   are destroyed until the rest fit). 0 = a texture is destroyed as soon as no surface uses it.
   NULL target -> XT_STATUS_INVALID_ARGUMENT. */
XT_CAPI xt_status xt_gpu_target_set_image_budget(xt_gpu_target* t, uint64_t idle_bytes);
/* Destroys every idle image texture of `t` now, and every texture of the target's pools that no
   surface is drawing with (idle 1x surface textures, attachment sets, transient and effect-cache
   targets), then polls the device once without waiting so the driver can reclaim the memory (call it
   when the host changes level / scene). Textures in use are kept. Inside a frame (between begin_frame and end_frame) or NULL target -> XT_STATUS_INVALID_ARGUMENT,
   nothing destroyed. */
XT_CAPI xt_status xt_gpu_target_trim(xt_gpu_target* t);

/* Memory the library accounts for itself (bytes from formats and sizes, not the driver's committed).
   The numbers are content bytes (a stencil attachment at the size the backend creates it with: 4 bytes
   per texel on D3D12); they leave out the allocator's alignment, each texture's minimum allocation and
   the slack of its memory blocks. */
typedef struct xt_gpu_memory_stats {
    uint32_t struct_size;          /* = sizeof(xt_gpu_memory_stats), set by the caller */
    uint32_t image_textures;       /* shared image textures alive (used or idle) */
    uint64_t image_bytes;          /* their memory */
    uint32_t image_idle_textures;  /* of image_textures, those no surface uses */
    uint32_t attached_surfaces;    /* surfaces attached to the target */
    uint64_t image_idle_bytes;     /* memory of the idle textures */
    uint64_t image_evictions;      /* idle textures destroyed so far (budget or xt_gpu_target_trim) */
    uint64_t surface_bytes;        /* the 1x textures the attached surfaces hold now */
    uint64_t transient_bytes;      /* the target's pools now: transient targets, released effect-cache
                                      targets, the MSAA color + stencil attachment sets (one per surface
                                      size, shared by the surfaces) and the idle 1x surface textures */
    uint64_t atlas_bytes;          /* the surfaces' glyph and coverage atlas pages (R8) */
} xt_gpu_memory_stats;
/* Fills `out` with `t`'s counters: only the fields `out->struct_size` covers are written (an older,
   shorter struct gets its prefix; struct_size itself is not written). NULL target or `out` ->
   XT_STATUS_INVALID_ARGUMENT. */
XT_CAPI xt_status xt_gpu_target_memory_stats(const xt_gpu_target* t, xt_gpu_memory_stats* out);
/* clang-format on */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XT_CAPI_XTFRAME_H */
