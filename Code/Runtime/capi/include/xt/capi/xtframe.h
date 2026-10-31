/*
 * xtframe.h -- the C ABI of the XTFrame runtime (shared library `xtframe`: xtframe.dll /
 * libxtframe.so / libxtframe.dylib). See Docs/design/50-c-abi.md (50.1-50.5), Docs/tasks/T-504.md.
 *
 * Pure C99: no C++ and no other XTFrame header, so FFI hosts (Unity P/Invoke, Godot, Python,
 * Rust) can bind it directly. Every function wraps an existing C++ call of the runtime
 * (Docs/design/13, 22 and 23); the host rules of those chapters apply unchanged.
 *
 * Conventions (50.2):
 *  - Calling convention cdecl. One thread per surface; the library starts no thread. Callbacks run
 *    inside xt_surface_update / xt_surface_port_set_* / xt_surface_hot_reload (and the other calls
 *    that drive the document), on the caller's thread.
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

#if defined(_WIN32)
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
    XT_STATUS_INCOMPATIBLE
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
   Threads (50.1, T-552 / capi review K2): every call on one surface -- and on the bundle it came from --
   is made on one and the same thread; the library takes no lock and starts no thread. */
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XT_CAPI_XTFRAME_H */
