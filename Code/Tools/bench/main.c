/*
 * xtframe-bench -- frame-time benchmark of one UI over the C ABI (Docs/design/52-web-wasm.md
 * §52.11). Pure C99 on xt/capi/xtframe.h, so the very same source runs natively and as the
 * Node.js/wasm build (NODERAWFS): the two numbers are comparable because the measured code path
 * is identical -- Bundle, Context, flows, HostPorts and the software renderer behind the ABI.
 *
 * Usage: xtframe-bench <bundle dir> <ui id> [--size WxH] [--frames N] [--warmup N] [--dt S]
 *                      [--pointer] [--json]
 *   --size    output pixels (default the UI's design size from the manifest)
 *   --frames  frames measured after the warm-up (default 300)
 *   --warmup  frames run and discarded first (default 60)
 *   --dt      seconds per frame handed to xt_surface_update (default 1/60)
 *   --pointer move the pointer across the surface every frame (hover states, hit tests)
 *   --json    one JSON object instead of the text summary
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

static int usage(void) {
    fprintf(stderr,
            "usage: xtframe-bench <bundle dir> <ui id> [--size WxH] [--frames N] [--warmup N] [--dt S] "
            "[--pointer] [--json]\n");
    return 2;
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
    int pointer = 0, json = 0;
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
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else {
            return usage();
        }
    }
    if (frames <= 0 || warmup < 0 || !(dt > 0.0)) {
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
    desc.system_fonts = 0;
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
        printf(
            "{\"bundle\":\"%s\",\"ui\":\"%s\",\"width\":%d,\"height\":%d,\"frames\":%ld,\"warmup\":%ld,"
            "\"dt\":%.6f,\"pointer\":%d,\"changed\":%ld,\"full\":%ld,\"draws\":%u,\"total_ms\":%.3f,"
            "\"pixels_fnv\":\"%016llx\",",
            dir, uiId, width, height, frames, warmup, dt, pointer, changed, full, draws, total,
            (unsigned long long)sum);
        printStats("update_ms", &su, 1, 0);
        printStats("render_ms", &sr, 1, 0);
        printStats("wall_update_ms", &swu, 1, 0);
        printStats("wall_render_ms", &swr, 1, 0);
        printStats("wall_frame_ms", &sw, 1, 1);
        printf("}\n");
    } else {
        printf("xtframe-bench %s / %s  %dx%d  %ld frames (+%ld warm-up), dt %.4f s%s\n", dir, uiId, width,
               height, frames, warmup, dt, pointer ? ", pointer" : "");
        printf("  changed %ld, full %ld, draws %u, last frame fnv %016llx, total %.1f ms\n", changed, full,
               draws, (unsigned long long)sum, total);
        printStats("update", &su, 0, 0);
        printStats("render", &sr, 0, 0);
        printStats("wall upd", &swu, 0, 0);
        printStats("wall rnd", &swr, 0, 0);
        printStats("wall frm", &sw, 0, 1);
    }

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
