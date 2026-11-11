// T-671 (Docs/design/50-c-abi.md §50.ab addendum 2, item 3): xt_gpu_mem_probe -- process and GPU memory of
// one offscreen GPU target while a host switches scenes, through the C ABI only. One CSV row per sample
// point: start, bundle_open, target_create, one `scene` row per UI shown (after its frames), teardown
// (surfaces gone, target trimmed) and end (target destroyed, bundle closed). Driven by
// Tools/Scripts/gpu_mem_matrix.py.
//
//   xt_gpu_mem_probe --bundle <dir> --out <csv> [--sequence <ui,ui,...|all>] [--rounds N] [--frames-per-scene
//   N]
//                    [--width W] [--height H] [--msaa 1|4] [--backend <name>] [--power low|high]
//                    [--mode hide|destroy] [--trim-each-scene 0|1] [--image-budget <MiB|default>]
//                    [--prewarm-hidden N] [--no-system-fonts] [--hold <ui> <frames>]
//
// T-683 (50 §50.ad): --prewarm-hidden N creates every surface of the sequence first, hidden, and runs N
// frames (each updating every surface) before the first one is shown -- the host flow, where every surface
// exists and updates from start-up and a scene switch only changes visibility; a `prewarm_hidden` row follows
// those frames.
// --no-system-fonts creates the surfaces with `system_fonts` = 0 (the bundle's own fonts only, as a host
// does). The time xt_gpu_target_create took is printed as "target_create <ms> ms".
// --power sets XT_WGPU_POWER for this process (unset: the environment's). The library's log lines (adapter,
// wgpu-native version, memory hints) go to stdout, and before each scene a line "scene <round> <n> <ui>"
// (T-675). T-685: --hold <ui> <frames> shows the sequence in order up to <ui> (round 1), then keeps it shown
// for <frames> more frames and stops (teardown follows); every 60 of those frames it prints a line "hold <ui>
// <frame> textures created <n> pool textures <n> transient_bytes <n> max_frame_ms <ms> max_begin_ms <ms>
// frames_over_33ms <n>" (textures created and pool textures from the library's last shared-cache line; the
// maxima and the count over that interval; begin = the xt_gpu_target_begin_frame call), then a `hold` CSV
// row. Exit codes: 0 ok, 2 bad arguments, 1 a C ABI failure; "SKIP: no WebGPU adapter" (exit 0) when no
// target can be created for want of an adapter.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "xt/capi/xtframe.h"

namespace {

struct Options {
    std::string bundle;
    std::string out;
    std::string sequence = "all";
    int rounds = 3;
    int framesPerScene = 60;
    int width = 1920;
    int height = 1080;
    int msaa = 4;
    std::string backend;
    std::string power;
    bool destroyMode = false;
    bool trimEachScene = false;
    int64_t imageBudgetMiB = -1;  ///< -1 = the library default
    int prewarmHidden = 0;        ///< T-683: hidden frames of every surface before the first show (0 = none)
    bool systemFonts = true;      ///< T-683: false = --no-system-fonts
    std::string holdUi;           ///< T-685: --hold <ui> <frames> (empty = no hold)
    int holdFrames = 0;
};

bool parseInt(const std::string& text, int lo, int hi, int& out) {
    int v = 0;
    const char* end = text.data() + text.size();
    const auto r = std::from_chars(text.data(), end, v);
    if (r.ec != std::errc() || r.ptr != end || v < lo || v > hi) {
        return false;
    }
    out = v;
    return true;
}

/// Parses argv into `o`; false (with a message on stderr) for a missing or bad value.
bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--no-system-fonts") {  // T-683: the one option without a value
            o.systemFonts = false;
            continue;
        }
        if (key == "--hold") {  // T-685: the one option with two values
            if (i + 2 >= argc || !parseInt(argv[i + 2], 1, 10000000, o.holdFrames)) {
                std::fprintf(stderr, "--hold needs <ui> <frames>\n");
                return false;
            }
            o.holdUi = argv[i + 1];
            i += 2;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value after %s\n", key.c_str());
            return false;
        }
        const std::string value = argv[++i];
        bool ok = true;
        if (key == "--bundle") {
            o.bundle = value;
        } else if (key == "--out") {
            o.out = value;
        } else if (key == "--sequence") {
            o.sequence = value;
        } else if (key == "--rounds") {
            ok = parseInt(value, 1, 1000, o.rounds);
        } else if (key == "--frames-per-scene") {
            ok = parseInt(value, 1, 100000, o.framesPerScene);
        } else if (key == "--width") {
            ok = parseInt(value, 1, 16384, o.width);
        } else if (key == "--height") {
            ok = parseInt(value, 1, 16384, o.height);
        } else if (key == "--msaa") {
            ok = (value == "1" || value == "4") && parseInt(value, 1, 4, o.msaa);
        } else if (key == "--backend") {
            o.backend = value;
        } else if (key == "--power") {
            ok = value == "low" || value == "high";
            o.power = value;
        } else if (key == "--mode") {
            ok = value == "hide" || value == "destroy";
            o.destroyMode = value == "destroy";
        } else if (key == "--trim-each-scene") {
            ok = value == "0" || value == "1";
            o.trimEachScene = value == "1";
        } else if (key == "--prewarm-hidden") {
            ok = parseInt(value, 0, 100000, o.prewarmHidden);
        } else if (key == "--image-budget") {
            int mib = 0;
            if (value == "default") {
                o.imageBudgetMiB = -1;
            } else if (parseInt(value, 0, 1 << 20, mib)) {
                o.imageBudgetMiB = mib;
            } else {
                ok = false;
            }
        } else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return false;
        }
        if (!ok) {
            std::fprintf(stderr, "bad value for %s: %s\n", key.c_str(), value.c_str());
            return false;
        }
    }
    if (o.bundle.empty() || o.out.empty()) {
        std::fprintf(stderr, "--bundle and --out are required\n");
        return false;
    }
    return true;
}

/// Process memory and the DXGI usage of the adapter the target runs on (Windows; -1 = unknown).
struct MemSample {
    int64_t privateBytes = -1;
    int64_t workingSet = -1;
    int64_t dxgiLocal = -1;
    int64_t dxgiNonLocal = -1;
};

std::string g_adapterName;  ///< from the library's "xt_gpu_target: adapter '<name>'" log line
/// T-685: "textures created" and "pools <n> texture(s)" of the library's last shared-cache line.
unsigned long g_texturesCreated = 0;
unsigned long g_poolTextures = 0;

unsigned long numberAfter(const char* message, const char* key) {
    const char* p = std::strstr(message, key);
    return p != nullptr ? std::strtoul(p + std::strlen(key), nullptr, 10) : 0ul;
}

void onLog(int level, const char* message, void*) {
    std::printf("log%d %s\n", level, message);
    std::fflush(stdout);
    const char* key = "xt_gpu_target: adapter '";
    if (std::strncmp(message, key, std::strlen(key)) == 0) {
        const char* name = message + std::strlen(key);
        const char* q = std::strchr(name, '\'');
        g_adapterName.assign(name, q != nullptr ? static_cast<size_t>(q - name) : std::strlen(name));
    }
    if (std::strncmp(message, "xt_gpu_target: shared image textures ", 37) == 0) {  // T-685
        g_texturesCreated = numberAfter(message, "textures created ");
        g_poolTextures = numberAfter(message, "; pools ");
    }
}

#if defined(_WIN32)
IDXGIAdapter3* g_dxgi = nullptr;

/// The DXGI adapter whose description equals the adapter the library logged; else none (columns stay empty).
void pickDxgiAdapter() {
    if (g_dxgi != nullptr || g_adapterName.empty()) {
        return;
    }
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return;
    }
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; g_dxgi == nullptr && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        char name[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        if (g_adapterName == name) {
            a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_dxgi));
        }
        a->Release();
    }
    factory->Release();
}

MemSample sample() {
    MemSample m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        m.privateBytes = static_cast<int64_t>(pmc.PrivateUsage);
        m.workingSet = static_cast<int64_t>(pmc.WorkingSetSize);
    }
    if (g_dxgi != nullptr) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(g_dxgi->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            m.dxgiLocal = static_cast<int64_t>(info.CurrentUsage);
        }
        if (SUCCEEDED(g_dxgi->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
            m.dxgiNonLocal = static_cast<int64_t>(info.CurrentUsage);
        }
    }
    return m;
}

void setEnv(const char* name, const std::string& value) { _putenv_s(name, value.c_str()); }
#else
void pickDxgiAdapter() {}

/// Linux: RssAnon (private) and VmRSS (working set) of /proc/self/status; elsewhere unknown.
MemSample sample() {
    MemSample m;
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        long kb = 0;
        if (std::sscanf(line.c_str(), "RssAnon: %ld kB", &kb) == 1) {
            m.privateBytes = static_cast<int64_t>(kb) * 1024;
        } else if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
            m.workingSet = static_cast<int64_t>(kb) * 1024;
        }
    }
    return m;
}

void setEnv(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }
#endif

std::FILE* openForWrite(const std::string& path) {
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    return fopen_s(&f, path.c_str(), "wb") == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

const char* kHeader =
    "round,scene,phase,ui,private_bytes,working_set_bytes,dxgi_local_bytes,dxgi_nonlocal_bytes,"
    "image_textures,image_bytes,image_idle_textures,image_idle_bytes,image_evictions,attached_surfaces,"
    "surface_bytes,transient_bytes,atlas_bytes,first_frame_ms,steady_frame_ms,elapsed_ms";

/// One CSV row; unknown numbers (-1) and a missing target leave their cells empty.
void writeRow(std::FILE* f, int round, int scene, const char* phase, const std::string& ui,
              const xt_gpu_target* t, double firstMs, double steadyMs, double startMs) {
    const MemSample m = sample();
    auto num = [&](int64_t v) {
        if (v >= 0) {
            std::fprintf(f, ",%lld", static_cast<long long>(v));
        } else {
            std::fprintf(f, ",");
        }
    };
    std::fprintf(f, "%d,%d,%s,%s", round, scene, phase, ui.c_str());
    num(m.privateBytes);
    num(m.workingSet);
    num(m.dxgiLocal);
    num(m.dxgiNonLocal);
    xt_gpu_memory_stats s{};
    s.struct_size = sizeof s;
    if (t != nullptr && xt_gpu_target_memory_stats(t, &s) == XT_STATUS_OK) {
        std::fprintf(f, ",%u,%llu,%u,%llu,%llu,%u,%llu,%llu,%llu", s.image_textures,
                     static_cast<unsigned long long>(s.image_bytes), s.image_idle_textures,
                     static_cast<unsigned long long>(s.image_idle_bytes),
                     static_cast<unsigned long long>(s.image_evictions), s.attached_surfaces,
                     static_cast<unsigned long long>(s.surface_bytes),
                     static_cast<unsigned long long>(s.transient_bytes),
                     static_cast<unsigned long long>(s.atlas_bytes));
    } else {
        std::fprintf(f, ",,,,,,,,,");
    }
    if (firstMs >= 0) {
        std::fprintf(f, ",%.3f,%.3f", firstMs, steadyMs);
    } else {
        std::fprintf(f, ",,");
    }
    std::fprintf(f, ",%.1f\n", nowMs() - startMs);
    std::fflush(f);
}

/// The UI ids of `sequence` ("all" = every UI of the bundle in manifest order); false for an unknown id.
bool resolveSequence(xt_bundle* b, const std::string& sequence, std::vector<std::string>& ids) {
    std::vector<std::string> all;
    const uint32_t n = xt_bundle_ui_count(b);
    for (uint32_t i = 0; i < n; ++i) {
        xt_ui_info info{};
        if (xt_bundle_ui_info(b, i, &info) == XT_STATUS_OK && info.id != nullptr) {
            all.emplace_back(info.id);
        }
    }
    if (sequence == "all") {
        ids = all;
        return !ids.empty();
    }
    size_t at = 0;
    while (at <= sequence.size()) {
        const size_t comma = std::min(sequence.find(',', at), sequence.size());
        const std::string id = sequence.substr(at, comma - at);
        bool known = false;
        for (const std::string& a : all) {
            known = known || a == id;
        }
        if (!known) {
            std::fprintf(stderr, "UI '%s' is not in the bundle\n", id.c_str());
            return false;
        }
        ids.push_back(id);
        at = comma + 1;
    }
    return !ids.empty();
}

/// One frame of every live surface (hidden ones included, as a host loops over all of them); wall time in ms.
/// T-685: `beginMs`, when given, receives the time xt_gpu_target_begin_frame took.
double frame(xt_gpu_target* t, std::vector<xt_surface*>& surfaces, double* beginMs = nullptr) {
    const double t0 = nowMs();
    xt_gpu_target_begin_frame(t, nullptr);
    if (beginMs != nullptr) *beginMs = nowMs() - t0;
    for (xt_surface* s : surfaces) {
        if (s != nullptr) {
            xt_surface_update(s, 1.0f / 60.0f);
            xt_surface_render_gpu(s, t, nullptr);
        }
    }
    xt_gpu_target_end_frame(t);
    return nowMs() - t0;
}

/// T-685: --hold -- `o.holdFrames` more frames of the shown scene, a line every 60 (see the file comment).
void hold(const Options& o, xt_gpu_target* t, std::vector<xt_surface*>& surfaces) {
    double maxFrame = 0, maxBegin = 0;
    int over = 0;
    for (int i = 1; i <= o.holdFrames; ++i) {
        double beginMs = 0;
        const double ms = frame(t, surfaces, &beginMs);
        maxFrame = std::max(maxFrame, ms);
        maxBegin = std::max(maxBegin, beginMs);
        if (ms > 33.0) ++over;
        if (i % 60 == 0 || i == o.holdFrames) {
            xt_gpu_memory_stats s{};
            s.struct_size = sizeof s;
            const bool haveStats = xt_gpu_target_memory_stats(t, &s) == XT_STATUS_OK;
            std::printf(
                "hold %s %d textures created %lu pool textures %lu transient_bytes %llu max_frame_ms %.2f "
                "max_begin_ms %.2f frames_over_33ms %d\n",
                o.holdUi.c_str(), i, g_texturesCreated, g_poolTextures,
                haveStats ? static_cast<unsigned long long>(s.transient_bytes) : 0ull, maxFrame, maxBegin,
                over);
            std::fflush(stdout);
            maxFrame = maxBegin = 0;
            over = 0;
        }
    }
}

int run(const Options& o) {
    const double start = nowMs();
    std::FILE* f = openForWrite(o.out);
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    std::fprintf(f, "%s\n", kHeader);
    writeRow(f, 0, 0, "start", "", nullptr, -1, 0, start);
    xt_bundle* b = xt_bundle_open(o.bundle.c_str());
    if (b == nullptr) {
        std::fprintf(stderr, "xt_bundle_open: %s\n", xt_capi_last_error());
        std::fclose(f);
        return 1;
    }
    std::vector<std::string> ids;
    if (!resolveSequence(b, o.sequence, ids) ||
        (!o.holdUi.empty() && std::find(ids.begin(), ids.end(), o.holdUi) == ids.end())) {
        if (!ids.empty()) std::fprintf(stderr, "--hold: UI '%s' is not in the sequence\n", o.holdUi.c_str());
        xt_bundle_close(b);
        std::fclose(f);
        return 2;
    }
    writeRow(f, 0, 0, "bundle_open", "", nullptr, -1, 0, start);
    xt_gpu_target_desc td{};
    td.struct_size = sizeof td;
    td.window.kind = XT_WINDOW_NONE;
    td.width = o.width;
    td.height = o.height;
    td.backend = o.backend.c_str();
    td.sample_count = o.msaa;
    const double createStart = nowMs();
    xt_gpu_target* t = xt_gpu_target_create(&td);
    const double createMs = nowMs() - createStart;
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        xt_bundle_close(b);
        std::fclose(f);
        if (error.find("no WebGPU adapter") != std::string::npos) {
            std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
            return 0;
        }
        std::fprintf(stderr, "xt_gpu_target_create: %s\n", error.c_str());
        return 1;
    }
    std::printf("target_create %.1f ms\n", createMs);
    std::fflush(stdout);
    pickDxgiAdapter();
    if (o.imageBudgetMiB >= 0) {
        xt_gpu_target_set_image_budget(t, static_cast<uint64_t>(o.imageBudgetMiB) << 20);
    }
    writeRow(f, 0, 0, "target_create", "", t, -1, 0, start);

    std::vector<xt_surface*> surfaces(ids.size(), nullptr);  // hide mode keeps them across rounds
    auto makeSurface = [&](size_t k) -> xt_surface* {
        xt_surface_desc d{};
        d.struct_size = sizeof d;
        d.width = o.width;
        d.height = o.height;
        d.device_pixel_ratio = 1.0f;
        d.premultiplied = 1;
        d.run_flow = 1;
        d.system_fonts = o.systemFonts ? 1 : 0;
        d.instance_id = static_cast<uint32_t>(k + 1);
        xt_surface* s = xt_surface_create(b, ids[k].c_str(), &d);
        if (s == nullptr) {
            std::fprintf(stderr, "xt_surface_create(%s): %s\n", ids[k].c_str(), xt_capi_last_error());
        }
        return s;
    };
    int previous = -1;
    int sceneIndex = 0;
    int status = 0;
    if (o.prewarmHidden > 0) {  // T-683: the host flow -- every surface exists, hidden, before the first show
        for (size_t k = 0; k < ids.size() && status == 0; ++k) {
            surfaces[k] = makeSurface(k);
            if (surfaces[k] == nullptr) {
                status = 1;
            } else {
                xt_surface_set_visible(surfaces[k], 0);
            }
        }
        const double p0 = nowMs();
        for (int i = 0; i < o.prewarmHidden && status == 0; ++i) {
            frame(t, surfaces);
        }
        std::printf("prewarm_hidden %d frames %.1f ms\n", o.prewarmHidden, nowMs() - p0);
        std::fflush(stdout);
        writeRow(f, 0, 0, "prewarm_hidden", "", t, -1, 0, start);
    }
    for (int round = 1; round <= o.rounds && status == 0; ++round) {
        for (size_t k = 0; k < ids.size() && status == 0; ++k) {
            ++sceneIndex;
            if (previous >= 0 && surfaces[previous] != nullptr) {  // leave the previous scene
                if (o.destroyMode) {
                    xt_surface_destroy(surfaces[previous]);
                    surfaces[previous] = nullptr;
                } else {
                    xt_surface_set_visible(surfaces[previous], 0);
                }
                if (o.trimEachScene) {
                    xt_gpu_target_trim(t);
                }
            }
            if (surfaces[k] == nullptr) {
                surfaces[k] = makeSurface(k);
                if (surfaces[k] == nullptr) {
                    status = 1;
                    break;
                }
            }
            // T-675: marks the scene in the log, so the library's "textures created N" lines can be
            // counted per scene switch.
            std::printf("scene %d %d %s\n", round, sceneIndex, ids[k].c_str());
            std::fflush(stdout);
            xt_surface_set_visible(surfaces[k], 1);
            previous = static_cast<int>(k);
            const double firstMs = frame(t, surfaces);
            double rest = 0;
            for (int i = 1; i < o.framesPerScene; ++i) {
                rest += frame(t, surfaces);
            }
            const double steadyMs = o.framesPerScene > 1 ? rest / (o.framesPerScene - 1) : firstMs;
            writeRow(f, round, sceneIndex, "scene", ids[k], t, firstMs, steadyMs, start);
            if (!o.holdUi.empty() && ids[k] == o.holdUi) {
                hold(o, t, surfaces);
                writeRow(f, round, sceneIndex, "hold", ids[k], t, -1, 0, start);
                round = o.rounds;  // stop after the hold
                break;
            }
        }
    }
    for (xt_surface*& s : surfaces) {
        if (s != nullptr) {
            xt_surface_destroy(s);
            s = nullptr;
        }
    }
    xt_gpu_target_trim(t);
    std::vector<xt_surface*> none;
    for (int i = 0; i < 3; ++i) {
        frame(t, none);  // end_frame polls the device: deferred destruction completes
    }
    writeRow(f, 0, 0, "teardown", "", t, -1, 0, start);
    xt_gpu_target_destroy(t);
    xt_bundle_close(b);
    writeRow(f, 0, 0, "end", "", nullptr, -1, 0, start);
    std::fclose(f);
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) {
        return 2;
    }
    if (!o.power.empty()) {
        setEnv("XT_WGPU_POWER", o.power);
    }
    xt_capi_set_log(&onLog, nullptr);
    const int rc = run(o);
    xt_capi_set_log(nullptr, nullptr);
    return rc;
}
