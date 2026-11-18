/*
 * xtframe-bench -- frame-time benchmark of one UI over the C ABI (Docs/design/52-web-wasm.md
 * §52.11). Pure C99 on xt/capi/xtframe.h, so the very same source runs natively and as the
 * Node.js/wasm build (NODERAWFS): the two numbers are comparable because the measured code path
 * is identical -- Bundle, Context, flows, HostPorts and the software renderer behind the ABI.
 *
 * Usage: xtframe-bench <bundle dir> <ui id> [--size WxH] [--frames N] [--warmup N] [--dt S]
 *                      [--pointer] [--system-fonts] [--event "<spec>"]... [--json]
 *   --size    output pixels (default the UI's design size from the manifest)
 *   --frames  frames measured after the warm-up (default 300)
 *   --warmup  frames run and discarded first (default 60)
 *   --dt      seconds per frame handed to xt_surface_update (default 1/60)
 *   --pointer move the pointer across the surface every frame (hover states, hit tests)
 *   --system-fonts  (T-713) create the surface with system_fonts = 1 (the bundle's shaper adds the
 *             system fonts); default off, as before: a UI whose bundle carries no font draws no text
 *   --event   (T-713, repeatable) xtframe-view's --event syntax, the forms the C ABI can drive,
 *             applied before that frame's update (frames count from 0, the warm-up included):
 *               "<frame> port <name> <value>"          xt_surface_port_set_text
 *               "[<frame>] system <kind> [<value>]"    xt_surface_post_system_event
 *               "[<frame>] pointer <move|down|up> <x> <y> [button=left|right|middle]"
 *                                                      xt_surface_push_input
 *               "<frame> #<id>.Focus"                  connection point "#<id>.focus"
 *               "<frame> #<id>.Change=<value>"         connection point "#<id>.setValue"
 *             #<id>.Click / PointerEnter / PointerLeave / Blur / state(...), key and press are
 *             refused (exit 2): the C ABI gives a host no element box to aim a pointer at, and no
 *             key or gamepad name table; click with "pointer down" + "pointer up" at a position.
 *   --json    one JSON object instead of the text summary (T-713: strings escaped)
 * Exit code 0; 1 on a bundle / surface error (the message on stderr); 2 on a usage error.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c99 */
#else
#define _CRT_SECURE_NO_WARNINGS /* sscanf: the C99 source stays the same on every platform */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "xt/capi/xtframe.h"

static double nowMs(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static int cmpDouble(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct Stats {
    double mean, median, p95, min, max;
} Stats;

/* Sorts `v` in place. */
static Stats stats(double* v, size_t n) {
    Stats s = {0, 0, 0, 0, 0};
    if (n == 0) {
        return s;
    }
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += v[i];
    }
    qsort(v, n, sizeof(double), cmpDouble);
    s.mean = sum / (double)n;
    s.median = v[n / 2];
    s.p95 = v[(size_t)((double)(n - 1) * 0.95)];
    s.min = v[0];
    s.max = v[n - 1];
    return s;
}

static void printStats(const char* name, const Stats* s, int json, int last) {
    if (json) {
        printf("\"%s\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f,\"min\":%.4f,\"max\":%.4f}%s", name,
               s->mean, s->median, s->p95, s->min, s->max, last ? "" : ",");
    } else {
        printf("  %-10s mean %8.3f  median %8.3f  p95 %8.3f  min %8.3f  max %8.3f ms\n", name, s->mean,
               s->median, s->p95, s->min, s->max);
    }
}

/* T-713: `text` as a JSON string (quotes included): '"' and '\\' escaped, control characters as
   \u00XX, every other byte as it is (UTF-8 stays UTF-8). */
static void printJsonString(const char* text) {
    putchar('"');
    for (const unsigned char* p = (const unsigned char*)text; *p != 0; ++p) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 0x20) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

static int usage(void) {
    fprintf(stderr,
            "usage: xtframe-bench <bundle dir> <ui id> [--size WxH] [--frames N] [--warmup N] [--dt S] "
            "[--pointer] [--system-fonts] [--event \"<spec>\"]... [--json]\n");
    return 2;
}

/* --- T-713: --event --- */

enum { EV_PORT, EV_SYSTEM, EV_POINTER, EV_FOCUS, EV_CHANGE };

typedef struct Event {
    long frame;
    int kind;         /* EV_* */
    char name[256];   /* port name, system kind, connection point ("#id.focus" / "#id.setValue") */
    char value[1024]; /* port / system / Change value */
    int pointerKind;  /* XT_INPUT_POINTER_* */
    int button;       /* 0 left 1 right 2 middle */
    float x, y;
} Event;

static int eventError(const char* spec, const char* reason) {
    fprintf(stderr, "xtframe-bench: cannot parse --event '%s': %s\n", spec, reason);
    return 0;
}

/* Up to `max` whitespace-separated words of `spec`, each copied into words[i] (at most 1023
   bytes); *rest receives where the word after the third starts (for a port value with spaces). */
static int splitWords(const char* spec, char words[][1024], int max, const char** rest) {
    int n = 0;
    const char* p = spec;
    *rest = NULL;
    while (*p != 0 && n < max) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == 0) break;
        if (n == 3) *rest = p;
        const char* start = p;
        while (*p != 0 && *p != ' ' && *p != '\t') ++p;
        size_t len = (size_t)(p - start);
        if (len > 1023) len = 1023;
        memcpy(words[n], start, len);
        words[n][len] = 0;
        ++n;
    }
    return n;
}

static int isFrame(const char* w, long* out) {
    if (*w == 0) return 0;
    for (const char* p = w; *p != 0; ++p) {
        if (*p < '0' || *p > '9') return 0;
    }
    *out = strtol(w, NULL, 10);
    return 1;
}

static int isNumber(const char* w, float* out) {
    char* end = NULL;
    if (*w == 0) return 0;
    *out = (float)strtod(w, &end);
    return end != NULL && *end == 0;
}

static void copyText(char* dst, size_t cap, const char* src) {
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* Parses one --event spec into `e`; 0 (and a message) when it is malformed or not drivable. */
static int parseEvent(const char* spec, Event* e) {
    char words[6][1024];
    const char* rest = NULL;
    memset(e, 0, sizeof(*e));
    const int n = splitWords(spec, words, 6, &rest);
    int at = 0;
    if (n > 0 && isFrame(words[0], &e->frame)) at = 1;
    if (n > at && strcmp(words[at], "system") == 0) {
        if (n < at + 2) return eventError(spec, "a system event is \"[<frame>] system <kind> [<value>]\"");
        e->kind = EV_SYSTEM;
        copyText(e->name, sizeof(e->name), words[at + 1]);
        /* The value is the rest of the spec after the kind (it may hold spaces). */
        const char* v = strstr(spec, words[at + 1]) + strlen(words[at + 1]);
        while (*v == ' ' || *v == '\t') ++v;
        copyText(e->value, sizeof(e->value), v);
        return 1;
    }
    if (n > at && strcmp(words[at], "pointer") == 0) {
        e->kind = EV_POINTER;
        const char* k = n > at + 1 ? words[at + 1] : "";
        e->pointerKind = strcmp(k, "move") == 0   ? XT_INPUT_POINTER_MOVE
                         : strcmp(k, "down") == 0 ? XT_INPUT_POINTER_DOWN
                         : strcmp(k, "up") == 0   ? XT_INPUT_POINTER_UP
                                                  : -1;
        int ok = e->pointerKind >= 0 && n >= at + 4 && n <= at + 5 && isNumber(words[at + 2], &e->x) &&
                 isNumber(words[at + 3], &e->y);
        if (ok && n == at + 5) {
            const char* b = words[at + 4];
            e->button = strcmp(b, "button=left") == 0     ? 0
                        : strcmp(b, "button=right") == 0  ? 1
                        : strcmp(b, "button=middle") == 0 ? 2
                                                          : -1;
            ok = e->button >= 0 && e->pointerKind != XT_INPUT_POINTER_MOVE;
        }
        if (!ok) {
            return eventError(spec,
                              "a pointer event is \"[<frame>] pointer <move|down|up> <x> <y> "
                              "[button=left|right|middle]\" (button only with down/up)");
        }
        return 1;
    }
    if (n > at && (strcmp(words[at], "key") == 0 || strcmp(words[at], "press") == 0)) {
        return eventError(spec, "key and press events are not supported by xtframe-bench (C ABI only)");
    }
    if (at == 1 && n >= 4 && strcmp(words[1], "port") == 0) {
        e->kind = EV_PORT;
        copyText(e->name, sizeof(e->name), words[2]);
        const char* v = rest != NULL ? rest : "";
        size_t len = strlen(v);
        /* One pair of matching outer quotes is removed, as xtframe-view does. */
        if (len >= 2 && (v[0] == '\'' || v[0] == '"') && v[len - 1] == v[0]) {
            const size_t inner = len - 2 < sizeof(e->value) - 1 ? len - 2 : sizeof(e->value) - 1;
            memcpy(e->value, v + 1, inner);
            e->value[inner] = 0;
        } else {
            copyText(e->value, sizeof(e->value), v);
        }
        return 1;
    }
    if (n != 2 || at != 1) return eventError(spec, "expected \"<frame> #<id>.<Event>[=value]\"");
    const char* target = words[1];
    const char* dot = strchr(target, '.');
    if (target[0] != '#' || dot == NULL || dot == target + 1) {
        return eventError(spec, "the target is not '#<id>.<Event>'");
    }
    const size_t idLen = (size_t)(dot - target - 1);
    const char* ev = dot + 1;
    const char* eq = strchr(ev, '=');
    /* 246: "#" + id + ".setValue" always fits e->name (256), which GCC checks (format-truncation). */
    char id[246];
    copyText(id, sizeof(id) < idLen + 1 ? sizeof(id) : idLen + 1, target + 1);
    if (strcmp(ev, "Focus") == 0) {
        e->kind = EV_FOCUS;
        snprintf(e->name, sizeof(e->name), "#%s.focus", id);
        return 1;
    }
    if (eq != NULL && (size_t)(eq - ev) == 6 && strncmp(ev, "Change", 6) == 0) {
        e->kind = EV_CHANGE;
        snprintf(e->name, sizeof(e->name), "#%s.setValue", id);
        copyText(e->value, sizeof(e->value), eq + 1);
        return 1;
    }
    return eventError(spec,
                      "only #<id>.Focus and #<id>.Change=<value> of the element forms can go through the C "
                      "ABI (it gives a host no element box to aim a pointer at); click with "
                      "\"<frame> pointer down <x> <y>\" and \"<frame> pointer up <x> <y>\"");
}

/* Applies the events of `frame` to `s`; a failure is reported on stderr and the run goes on. */
static void applyEvents(xt_surface* s, const Event* events, int count, long frame) {
    for (int i = 0; i < count; ++i) {
        const Event* e = &events[i];
        if (e->frame != frame) continue;
        xt_status st = XT_STATUS_OK;
        if (e->kind == EV_PORT) {
            const uint32_t port = xt_surface_port_lookup(s, e->name);
            st = port == XT_INVALID_PORT ? XT_STATUS_INVALID_ARGUMENT
                                         : xt_surface_port_set_text(s, port, e->value);
        } else if (e->kind == EV_SYSTEM) {
            st = xt_surface_post_system_event(s, e->name, e->value);
        } else if (e->kind == EV_POINTER) {
            xt_input_event in;
            memset(&in, 0, sizeof(in));
            in.struct_size = (uint32_t)sizeof(in);
            in.kind = e->pointerKind;
            in.x = e->x;
            in.y = e->y;
            in.button = e->button;
            st = xt_surface_push_input(s, &in);
        } else if (e->kind == EV_FOCUS) {
            st = xt_surface_connection_pulse(s, e->name, NULL);
        } else {
            st = xt_surface_connection_set_text(s, e->name, e->value, NULL);
        }
        if (st != XT_STATUS_OK) {
            fprintf(stderr, "xtframe-bench: --event at frame %ld (%s): %s\n", frame, e->name,
                    e->kind == EV_PORT && st == XT_STATUS_INVALID_ARGUMENT ? "no such port"
                                                                           : xt_capi_last_error());
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const char* dir = argv[1];
    const char* uiId = argv[2];
    int width = 0, height = 0;
    long frames = 300, warmup = 60;
    double dt = 1.0 / 60.0;
    int pointer = 0, json = 0, systemFonts = 0;
    Event* events = NULL;
    int eventCount = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
                return usage();
            }
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dt") == 0 && i + 1 < argc) {
            dt = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--pointer") == 0) {
            pointer = 1;
        } else if (strcmp(argv[i], "--system-fonts") == 0) {
            systemFonts = 1;
        } else if (strcmp(argv[i], "--event") == 0 && i + 1 < argc) {
            Event* grown = (Event*)realloc(events, sizeof(Event) * (size_t)(eventCount + 1));
            if (grown == NULL) {
                fprintf(stderr, "xtframe-bench: out of memory\n");
                return 1;
            }
            events = grown;
            if (!parseEvent(argv[++i], &events[eventCount])) {
                free(events);
                return 2;
            }
            ++eventCount;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else {
            free(events);
            return usage();
        }
    }
    if (frames <= 0 || warmup < 0 || !(dt > 0.0)) {
        free(events);
        return usage();
    }

    xt_bundle* bundle = xt_bundle_open(dir);
    if (bundle == NULL) {
        fprintf(stderr, "xtframe-bench: cannot open %s: %s\n", dir, xt_capi_last_error());
        return 1;
    }
    const int32_t uiIndex = xt_bundle_find_ui(bundle, uiId);
    if (uiIndex < 0) {
        fprintf(stderr, "xtframe-bench: no UI '%s' in %s\n", uiId, dir);
        xt_bundle_close(bundle);
        return 1;
    }
    xt_ui_info info;
    memset(&info, 0, sizeof(info));
    if (xt_bundle_ui_info(bundle, (uint32_t)uiIndex, &info) != XT_STATUS_OK) {
        fprintf(stderr, "xtframe-bench: ui_info: %s\n", xt_capi_last_error());
        xt_bundle_close(bundle);
        return 1;
    }
    if (width == 0) {
        width = (int)(info.design_width + 0.5f);
        height = (int)(info.design_height + 0.5f);
    }
    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }

    xt_surface_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.width = width;
    desc.height = height;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    desc.system_fonts = systemFonts;
    xt_surface* surface = xt_surface_create(bundle, uiId, &desc);
    if (surface == NULL) {
        fprintf(stderr, "xtframe-bench: cannot create surface: %s\n", xt_capi_last_error());
        xt_bundle_close(bundle);
        return 1;
    }
    xt_surface_set_perf_enabled(surface, 1);

    const size_t stride = (size_t)width * 4;
    uint8_t* rgba = (uint8_t*)calloc(stride, (size_t)height);
    double* updateMs = (double*)calloc((size_t)frames, sizeof(double));
    double* renderMs = (double*)calloc((size_t)frames, sizeof(double));
    double* wallMs = (double*)calloc((size_t)frames, sizeof(double));
    double* wallUpdate = (double*)calloc((size_t)frames, sizeof(double));
    double* wallRender = (double*)calloc((size_t)frames, sizeof(double));
    if (rgba == NULL || updateMs == NULL || renderMs == NULL || wallMs == NULL || wallUpdate == NULL ||
        wallRender == NULL) {
        fprintf(stderr, "xtframe-bench: out of memory\n");
        return 1;
    }

    long changed = 0, full = 0;
    uint32_t draws = 0;
    const double t0 = nowMs();
    for (long frame = 0; frame < warmup + frames; ++frame) {
        if (pointer) {
            xt_input_event e;
            memset(&e, 0, sizeof(e));
            e.struct_size = (uint32_t)sizeof(e);
            e.kind = XT_INPUT_POINTER_MOVE;
            const double phase = (double)frame / 120.0;
            e.x = (float)((0.5 + 0.45 * cos(phase)) * (double)width);
            e.y = (float)((0.5 + 0.45 * sin(phase * 1.3)) * (double)height);
            xt_surface_push_input(surface, &e);
        }
        applyEvents(surface, events, eventCount, frame);
        const double a = nowMs();
        if (xt_surface_update(surface, (float)dt) != XT_STATUS_OK) {
            fprintf(stderr, "xtframe-bench: update: %s\n", xt_capi_last_error());
            return 1;
        }
        const double b = nowMs();
        xt_damage damage;
        if (xt_surface_render(surface, rgba, (int)stride, &damage) != XT_STATUS_OK) {
            fprintf(stderr, "xtframe-bench: render: %s\n", xt_capi_last_error());
            return 1;
        }
        const double c = nowMs();
        if (frame < warmup) {
            continue;
        }
        const long k = frame - warmup;
        xt_frame_stats st;
        memset(&st, 0, sizeof(st));
        (void)xt_surface_frame_stats(surface, &st);
        updateMs[k] = st.update_ms;
        renderMs[k] = st.render_ms;
        wallUpdate[k] = b - a;
        wallRender[k] = c - b;
        wallMs[k] = c - a;
        changed += damage.changed ? 1 : 0;
        full += damage.full ? 1 : 0;
        draws = st.executed_draws;
    }
    const double total = nowMs() - t0;

    /* A checksum of the last frame, so two builds can be compared for identical pixels. */
    uint64_t sum = 1469598103934665603ULL;
    for (size_t i = 0; i < stride * (size_t)height; ++i) {
        sum = (sum ^ rgba[i]) * 1099511628211ULL;
    }

    const Stats su = stats(updateMs, (size_t)frames);
    const Stats sr = stats(renderMs, (size_t)frames);
    const Stats swu = stats(wallUpdate, (size_t)frames);
    const Stats swr = stats(wallRender, (size_t)frames);
    const Stats sw = stats(wallMs, (size_t)frames);
    if (json) {
        printf("{\"bundle\":");
        printJsonString(dir);
        printf(",\"ui\":");
        printJsonString(uiId);
        printf(
            ",\"width\":%d,\"height\":%d,\"frames\":%ld,\"warmup\":%ld,"
            "\"dt\":%.6f,\"pointer\":%d,",
            width, height, frames, warmup, dt, pointer);
        if (systemFonts) {
            printf("\"system_fonts\":1,"); /* T-713: only when on, so the default object is unchanged */
        }
        if (eventCount > 0) {
            printf("\"events\":%d,", eventCount);
        }
        printf("\"changed\":%ld,\"full\":%ld,\"draws\":%u,\"total_ms\":%.3f,\"pixels_fnv\":\"%016llx\",",
               changed, full, draws, total, (unsigned long long)sum);
        printStats("update_ms", &su, 1, 0);
        printStats("render_ms", &sr, 1, 0);
        printStats("wall_update_ms", &swu, 1, 0);
        printStats("wall_render_ms", &swr, 1, 0);
        printStats("wall_frame_ms", &sw, 1, 1);
        printf("}\n");
    } else {
        printf("xtframe-bench %s / %s  %dx%d  %ld frames (+%ld warm-up), dt %.4f s%s%s", dir, uiId, width,
               height, frames, warmup, dt, pointer ? ", pointer" : "", systemFonts ? ", system fonts" : "");
        if (eventCount > 0) {
            printf(", %d event(s)", eventCount);
        }
        printf("\n");
        printf("  changed %ld, full %ld, draws %u, last frame fnv %016llx, total %.1f ms\n", changed, full,
               draws, (unsigned long long)sum, total);
        printStats("update", &su, 0, 0);
        printStats("render", &sr, 0, 0);
        printStats("wall upd", &swu, 0, 0);
        printStats("wall rnd", &swr, 0, 0);
        printStats("wall frm", &sw, 0, 1);
    }

    free(events);
    free(wallRender);
    free(wallUpdate);
    free(wallMs);
    free(renderMs);
    free(updateMs);
    free(rgba);
    xt_surface_destroy(surface);
    xt_bundle_close(bundle);
    return 0;
}
