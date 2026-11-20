// T-652 (Docs/design/50-c-abi.md §50.1 "跨 bundle 的執行緒"): one thread per bundle. Two different
// bundles, one surface each, each driven by its own std::thread through 500 frames of input + update +
// render, at the same time. Every frame's pixels are folded into a hash; the threaded run must give the
// same hashes as the same two scenes run one after the other on the calling thread (a race that does
// not crash still shows up as different pixels). XTAtrium's report: two such threads crashed in
// xt_surface_update (0xC0000005), from the function-local `static` scratch vectors of the style engine,
// the world-transform pass and the painter's child sort that every Context shared.
// T-797 (Docs/design/23-host-integration.md §23.ai rule 8): cases 4 and 5 hand one bundle (with its
// surface) from thread A to thread B and back, each hand-over through an event, while another bundle runs
// on a third thread; every frame must equal the serial run.

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

constexpr int kFrames = 500;
constexpr float kDt = 1.0f / 60.0f;

/// One surface of one bundle, opened, driven and closed on whichever thread calls run().
struct Scene {
    const char* bundleDir;
    const char* ui;
    int width;
    int height;
    float dpr;
    int runFlow;
    int systemFonts;
    const char* activePort;  // set to "1" before the first frame, or null
    bool clicks;             // a click at the centre every 40 frames (flip toggles `flipped`)
};

/// What one run of a scene gives: the hash of every frame's pixels folded together, and the first
/// failure (empty when none). doctest's assertions are not used off the main thread.
struct Outcome {
    uint64_t hash = 1469598103934665603ull;
    std::string error;
};

void fold(uint64_t& hash, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
}

xt_input_event pointerEvent(int kind, float x, float y) {
    xt_input_event e{};
    e.struct_size = sizeof(e);
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.clicks = 1;
    return e;
}

bool push(xt_surface* s, int kind, float x, float y, Outcome& out) {
    const xt_input_event e = pointerEvent(kind, x, y);
    if (xt_surface_push_input(s, &e) != XT_STATUS_OK) {
        out.error = std::string("push_input: ") + xt_capi_last_error();
        return false;
    }
    return true;
}

/// Pointer input of frame `f`: a sweep over the surface (hover), a wheel notch every 25 frames and,
/// with `clicks`, a click at the centre every 40.
bool frameInput(const Scene& scene, xt_surface* s, int f, Outcome& out) {
    const float w = static_cast<float>(scene.width) / scene.dpr;
    const float h = static_cast<float>(scene.height) / scene.dpr;
    const float x = w * static_cast<float>(f % 50) / 50.0f;
    const float y = h * static_cast<float>((f / 50) % 10) / 10.0f;
    if (!push(s, XT_INPUT_POINTER_MOVE, x, y, out)) {
        return false;
    }
    if (f % 25 == 12) {
        xt_input_event wheel = pointerEvent(XT_INPUT_WHEEL, x, y);
        wheel.wheel_y = (f / 25) % 2 == 0 ? 1.0f : -1.0f;
        if (xt_surface_push_input(s, &wheel) != XT_STATUS_OK) {
            out.error = std::string("push_input wheel: ") + xt_capi_last_error();
            return false;
        }
    }
    if (scene.clicks && f % 40 == 20) {
        return push(s, XT_INPUT_POINTER_MOVE, w * 0.5f, h * 0.5f, out) &&
               push(s, XT_INPUT_POINTER_DOWN, w * 0.5f, h * 0.5f, out) &&
               push(s, XT_INPUT_POINTER_UP, w * 0.5f, h * 0.5f, out);
    }
    return true;
}

/// One scene's bundle and surface, opened, stepped and closed by whichever thread calls each method --
/// the unit a host hands from one thread to another (50 §50.1, 23 §23.ai rule 8).
class Driver {
public:
    explicit Driver(const Scene& scene) : m_scene(scene) {}
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /// Opens the bundle and the surface and sets the active port; false (with out.error) on failure.
    bool open() {
        m_bundle = xt_bundle_open(m_scene.bundleDir);
        if (m_bundle == nullptr) {
            m_out.error = std::string("bundle_open: ") + xt_capi_last_error();
            return false;
        }
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = m_scene.width;
        desc.height = m_scene.height;
        desc.device_pixel_ratio = m_scene.dpr;
        desc.premultiplied = 1;
        desc.run_flow = m_scene.runFlow;
        desc.system_fonts = m_scene.systemFonts;
        m_surface = xt_surface_create(m_bundle, m_scene.ui, &desc);
        if (m_surface == nullptr) {
            m_out.error = std::string("surface_create: ") + xt_capi_last_error();
            return false;
        }
        if (m_scene.activePort != nullptr) {
            const uint32_t id = xt_surface_port_lookup(m_surface, m_scene.activePort);
            if (id == XT_INVALID_PORT || xt_surface_port_set_text(m_surface, id, "1") != XT_STATUS_OK) {
                m_out.error = std::string("port ") + m_scene.activePort;
                return false;
            }
        }
        m_rgba.assign(static_cast<size_t>(m_scene.width) * 4 * static_cast<size_t>(m_scene.height), 0);
        return true;
    }

    /// Frames [m_frame, last): input, update, render, each frame's pixels folded into the hash. Does
    /// nothing after a failure.
    void frames(int last) {
        const int stride = m_scene.width * 4;
        for (; m_surface != nullptr && m_out.error.empty() && m_frame < last; ++m_frame) {
            xt_damage damage{};
            if (!frameInput(m_scene, m_surface, m_frame, m_out)) {
                return;
            }
            if (xt_surface_update(m_surface, m_frame == 0 ? 0.0f : kDt) != XT_STATUS_OK) {
                m_out.error = std::string("update: ") + xt_capi_last_error();
                return;
            }
            if (xt_surface_render(m_surface, m_rgba.data(), stride, &damage) != XT_STATUS_OK) {
                m_out.error = std::string("render: ") + xt_capi_last_error();
                return;
            }
            fold(m_out.hash, m_rgba.data(), m_rgba.size());
            m_frameHashes.push_back(m_out.hash);
        }
    }

    /// Destroys the surface and closes the bundle.
    void close() {
        if (m_surface != nullptr) {
            xt_surface_destroy(m_surface);
            m_surface = nullptr;
        }
        if (m_bundle != nullptr) {
            if (xt_bundle_close(m_bundle) != XT_STATUS_OK && m_out.error.empty()) {
                m_out.error = std::string("bundle_close: ") + xt_capi_last_error();
            }
            m_bundle = nullptr;
        }
    }

    const Outcome& outcome() const { return m_out; }
    /// The running hash after each frame (T-797: the first differing frame of a hand-over run).
    const std::vector<uint64_t>& frameHashes() const { return m_frameHashes; }

private:
    const Scene& m_scene;
    xt_bundle* m_bundle = nullptr;
    xt_surface* m_surface = nullptr;
    std::vector<uint8_t> m_rgba;
    std::vector<uint64_t> m_frameHashes;
    int m_frame = 0;
    Outcome m_out;
};

Outcome run(const Scene& scene) {
    Driver d(scene);
    if (d.open()) {
        d.frames(kFrames);
    }
    d.close();
    return d.outcome();
}

/// Runs `a` and `b` one after the other on this thread, then each on its own thread at the same time,
/// and checks both threaded runs against the serial ones.
void checkPair(const Scene& a, const Scene& b) {
    const Outcome serialA = run(a);
    const Outcome serialB = run(b);
    REQUIRE_MESSAGE(serialA.error.empty(), serialA.error);
    REQUIRE_MESSAGE(serialB.error.empty(), serialB.error);
    Outcome threadedA;
    Outcome threadedB;
    std::thread ta([&] { threadedA = run(a); });
    std::thread tb([&] { threadedB = run(b); });
    ta.join();
    tb.join();
    CHECK_MESSAGE(threadedA.error.empty(), threadedA.error);
    CHECK_MESSAGE(threadedB.error.empty(), threadedB.error);
    CHECK(threadedA.hash == serialA.hash);
    CHECK(threadedB.hash == serialB.hash);
}

/// A Debug (and ASan) SW frame is tens of times slower than a Release one: npdo renders at a fifth of
/// its design size there (the same layout), at a quarter in Release.
#ifdef NDEBUG
constexpr int kNpdoW = 480;
constexpr int kNpdoH = 270;
constexpr float kNpdoDpr = 0.25f;
#else
constexpr int kNpdoW = 384;
constexpr int kNpdoH = 216;
constexpr float kNpdoDpr = 0.2f;
#endif

const Scene kFlip{XT_CAPI_TEST_FLIP_BUNDLE, "flip", 64, 64, 1.0f, 1, 0, nullptr, true};
const Scene kState{XT_CAPI_TEST_STATE_BUNDLE, "state", 64, 64, 1.0f, 0, 0, nullptr, false};
const Scene kNpdoSettings{
    XT_CAPI_TEST_NPDO_BUNDLE, "settings", kNpdoW, kNpdoH, kNpdoDpr, 1, 1, "active", true};
const Scene kNpdoHud{XT_CAPI_TEST_NPDO_BUNDLE, "hud", kNpdoW, kNpdoH, kNpdoDpr, 1, 1, "active", false};

/// T-797: a thread that runs the jobs handed to it, one at a time; post() returns when the job has
/// finished, so the mutex and condition variable order the job before whatever the caller does next --
/// the hand-over edge of 23 §23.ai rule 2.
class Worker {
public:
    Worker() : m_thread([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> job) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_job = std::move(job);
        m_done = false;
        m_wake.notify_all();
        m_finished.wait(lock, [this] { return m_done; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [this] { return m_quit || m_job != nullptr; });
            if (m_job == nullptr) {
                return;
            }
            std::function<void()> job = std::move(m_job);
            m_job = nullptr;
            lock.unlock();
            job();
            lock.lock();
            m_done = true;
            m_finished.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_finished;
    std::function<void()> m_job;
    bool m_done = false;
    bool m_quit = false;
    std::thread m_thread;
};

/// The first frame whose running hash differs, or -1.
int firstDifference(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    for (size_t f = 0; f < a.size() && f < b.size(); ++f) {
        if (a[f] != b[f]) {
            return static_cast<int>(f);
        }
    }
    return a.size() == b.size() ? -1 : static_cast<int>(a.size() < b.size() ? a.size() : b.size());
}

/// T-797 (23 §23.ai rule 8): `handed` -- its bundle with its surface -- is opened and driven on thread A,
/// handed to thread B, then back to A, which closes it; `parallel` runs on a third thread meanwhile.
/// Both must give, frame for frame, the hashes of the serial runs.
void checkHandover(const Scene& handed, const Scene& parallel) {
    Driver serialHanded(handed);
    REQUIRE_MESSAGE(serialHanded.open(), serialHanded.outcome().error);
    serialHanded.frames(kFrames);
    serialHanded.close();
    REQUIRE_MESSAGE(serialHanded.outcome().error.empty(), serialHanded.outcome().error);
    Driver serialParallel(parallel);
    REQUIRE_MESSAGE(serialParallel.open(), serialParallel.outcome().error);
    serialParallel.frames(kFrames);
    serialParallel.close();
    REQUIRE_MESSAGE(serialParallel.outcome().error.empty(), serialParallel.outcome().error);

    Driver threadedHanded(handed);
    Driver threadedParallel(parallel);
    std::thread third([&] {
        if (threadedParallel.open()) {
            threadedParallel.frames(kFrames);
        }
        threadedParallel.close();
    });
    {
        Worker a;
        Worker b;
        bool opened = false;
        a.post([&] {
            opened = threadedHanded.open();
            threadedHanded.frames(kFrames / 3);
        });
        b.post([&] { threadedHanded.frames(2 * kFrames / 3); });
        a.post([&] {
            threadedHanded.frames(kFrames);
            threadedHanded.close();
        });
        CHECK(opened);
    }
    third.join();
    CHECK_MESSAGE(threadedHanded.outcome().error.empty(), threadedHanded.outcome().error);
    CHECK_MESSAGE(threadedParallel.outcome().error.empty(), threadedParallel.outcome().error);
    const int handedDiff = firstDifference(threadedHanded.frameHashes(), serialHanded.frameHashes());
    const int parallelDiff = firstDifference(threadedParallel.frameHashes(), serialParallel.frameHashes());
    CHECK_MESSAGE(handedDiff == -1, "handed bundle: first differing frame " << handedDiff);
    CHECK_MESSAGE(parallelDiff == -1, "parallel bundle: first differing frame " << parallelDiff);
    CHECK(threadedHanded.outcome().hash == serialHanded.outcome().hash);
    CHECK(threadedParallel.outcome().hash == serialParallel.outcome().hash);
}

}  // namespace

TEST_CASE(
    "capi threads 1 (T-652): flip and state bundles on two threads, 500 frames each, equal the serial run") {
    checkPair(kFlip, kState);
}

TEST_CASE(
    "capi threads 2 (T-652): two npdo bundles (settings / hud, text and images) on two threads equal the "
    "serial run") {
    checkPair(kNpdoSettings, kNpdoHud);
}

TEST_CASE("capi threads 3 (T-652): npdo settings and flip on two threads equal the serial run") {
    checkPair(kNpdoSettings, kFlip);
}

TEST_CASE(
    "capi threads 4 (T-797): an npdo settings bundle handed A -> B -> A between threads while an npdo hud "
    "bundle runs on a third thread equals the serial run, frame for frame") {
    checkHandover(kNpdoSettings, kNpdoHud);
}

TEST_CASE("capi threads 5 (T-797): flip handed A -> B -> A while npdo settings runs on a third thread") {
    checkHandover(kFlip, kNpdoSettings);
}
