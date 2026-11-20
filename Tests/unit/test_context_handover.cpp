#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text/ft_text_shaper.h"
#include "xt/timeline.h"

/// T-797 (Docs/design/23-host-integration.md §23.ai): a Context is not used by two threads at once,
/// but it may be handed from one thread to another between calls. One Context (text, an image, an
/// endless CSS animation, a looping timeline, port bindings, pointer input, perf recording on) is
/// driven by thread A, handed to thread B, then back to A -- every hand-over through an event (a
/// mutex and a condition variable) -- while a second Context, sharing only the FtTextShaper (rule 4)
/// and with its own AssetResolver and SwRenderer (rule 3), runs on a third thread the whole time.
/// Every frame's SW pixels are hashed; both Contexts must give, frame for frame, the hashes of the
/// same two runs made one after the other on the calling thread. The visual-generation check stays on
/// (XT_VERIFY_VISUAL_GENERATION, set for the test suites). A failure here means the investigation
/// behind §23.ai missed shared state: reduce it and report, do not add a lock.

using namespace xt;

namespace {

constexpr int kFrames = 240;
constexpr int kHandAt1 = 80;   // A -> B before this frame.
constexpr int kHandAt2 = 160;  // B -> A before this frame.
constexpr int kWidth = 160;
constexpr int kHeight = 100;
constexpr float kDt = 1.0f / 60.0f;

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

/// A small HUD: a text bound to a string port, a bar whose width is bound to a number port, an image,
/// an endless @keyframes rotation, a hover style and a timeline on the image's opacity.
std::string hudSource(const char* spinDuration, const char* barColor) {
    return std::string(
               "<xt><head><style>"
               "body { width: 160px; height: 100px; background-color: #202830; font-family: \"Noto Sans\";"
               " font-size: 14px; color: #ffffff; }"
               "#title { position: absolute; left: 4px; top: 4px; }"
               "#bar { position: absolute; left: 4px; top: 30px; width: 10px; height: 10px; "
               "background-color: ") +
           barColor +
           "; }"
           "#bar:hover { background-color: #00ccff; }"
           "#pic { position: absolute; left: 100px; top: 40px; width: 48px; height: 48px; }"
           "#spin { position: absolute; left: 10px; top: 56px; width: 30px; height: 30px;"
           " background-color: #33aa55; animation: spin " +
           spinDuration +
           " linear infinite; }"
           "@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }"
           "@timeline pulse { #pic { 0s { opacity: 1 } 1s { opacity: 0.3 } 2s { opacity: 1 } } }"
           "</style></head><body id=\"Root\">"
           "<div id=\"hud\" port:label=\"string\" port:hp=\"number\"></div>"
           "<span id=\"title\" bind:text=\"label\">start</span>"
           "<div id=\"bar\" bind:width=\"hp\"></div>"
           "<img id=\"pic\" src=\"field_checker.png\"/>"
           "<div id=\"spin\"></div>"
           "</body></xt>";
}

std::vector<uint8_t> compileHud(const char* spinDuration, const char* barColor) {
    compile::CompileOptions options;
    options.emitMeta = false;
    const std::string source = hudSource(spinDuration, barColor);
    std::vector<compile::SourceFile> files{{"main.xt", source}};
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
    test::requireCompiled(result);
    return result.ir;
}

void fold(uint64_t& hash, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
}

/// One Context with everything that must travel with it (§23.ai rule 3): its own resolver and SW
/// renderer. Only `shaper` is shared. Every member is used only by the thread that drives it now.
class Scene {
public:
    Scene(const std::vector<uint8_t>& ir, text::FtTextShaper& shaper, int seed) : m_ir(ir), m_seed(seed) {
        m_rgba.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0);
        m_assets = std::make_unique<FileAssetResolver>(std::string(XT_TEST_XT_DIR), sw::decodeImageBytes);
        ContextConfig config;
        config.shaper = &shaper;
        config.assets = m_assets.get();
        m_ctx = std::make_unique<Context>(config);
        m_ctx->perf().setEnabled(true);
        m_ctx->setViewport(Rect{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)}, 1.0f);
        Result<DocumentHandle> handle = m_ctx->loadDocument(IrBlob{m_ir.data(), m_ir.size()}, false);
        if (!handle.ok()) {
            m_error = "loadDocument failed";
            return;
        }
        m_handle = handle.value();
        Document* doc = m_ctx->document(m_handle);
        m_label = doc->lookupPort("label");
        m_hp = doc->lookupPort("hp");
        if (m_label == kInvalidPort || m_hp == kInvalidPort) {
            m_error = "ports label / hp not found";
            return;
        }
        PlayOptions loop;
        loop.loop = true;
        if (m_ctx->playTimeline("", "pulse", loop) != Status::kOk) {
            m_error = "playTimeline pulse failed";
        }
        m_renderer.setTarget(sw::SwTarget{m_rgba.data(), kWidth, kHeight, kWidth * 4});
    }

    /// Frames [first, last): input, port writes, update, render, SW raster, hash.
    void run(int first, int last) {
        for (int f = first; f < last && m_error.empty(); ++f) {
            frame(f);
        }
    }

    const std::string& error() const { return m_error; }
    const std::vector<uint64_t>& hashes() const { return m_hashes; }
    /// Frames whose list drew glyphs / an image (the test really covers text and images).
    int glyphFrames() const { return m_glyphFrames; }
    int imageFrames() const { return m_imageFrames; }

private:
    void frame(int f) {
        InputEvent move;
        move.kind = InputEvent::PointerMove;
        move.pos =
            Vec2{static_cast<float>((f * 3 + m_seed * 17) % kWidth), static_cast<float>((f / 4) % kHeight)};
        m_ctx->pushInput(move);
        if (f % 10 == 0) {
            Document* doc = m_ctx->document(m_handle);
            const std::string hp = std::to_string(20 + (f * (m_seed + 1)) % 120) + ".5";
            const std::string label = "frame " + std::to_string(f) + " / " + std::to_string(m_seed);
            if (doc->setPortText(m_hp, hp) != Status::kOk ||
                doc->setPortText(m_label, label) != Status::kOk) {
                m_error = "setPortText failed at frame " + std::to_string(f);
                return;
            }
        }
        m_ctx->update(f == 0 ? 0.0f : kDt);
        m_ctx->render(m_list, nullptr);
        m_glyphFrames += m_list.glyphRuns.empty() ? 0 : 1;
        m_imageFrames += m_list.images.empty() ? 0 : 1;
        m_renderer.clear(Color{0, 0, 0, 1});
        m_renderer.render(m_list, m_ctx->textShaper(), m_assets.get(), nullptr, &m_ctx->perf());
        uint64_t hash = 1469598103934665603ull;
        fold(hash, m_rgba.data(), m_rgba.size());
        m_hashes.push_back(hash);
        if (m_ctx->visualCheck().mismatches != 0) {
            m_error = "visual-generation mismatch: " + m_ctx->visualCheck().first;
        }
    }

    const std::vector<uint8_t>& m_ir;
    int m_seed;
    std::unique_ptr<FileAssetResolver> m_assets;
    std::unique_ptr<Context> m_ctx;
    DocumentHandle m_handle = 0;
    PortId m_label = kInvalidPort;
    PortId m_hp = kInvalidPort;
    CommandList m_list;
    sw::SwRenderer m_renderer;
    std::vector<uint8_t> m_rgba;
    std::vector<uint64_t> m_hashes;
    int m_glyphFrames = 0;
    int m_imageFrames = 0;
    std::string m_error;
};

/// A thread that runs the jobs handed to it, one at a time. post() returns when the job has finished:
/// the mutex and condition variable give the caller -- and the next thread it hands the work to -- a
/// happens-before edge with everything the job did (§23.ai rule 2).
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

    std::thread::id id() const { return m_thread.get_id(); }

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

}  // namespace

TEST_CASE(
    "T-797 Context hand-over: one Context driven A -> B -> A while another shares the FtTextShaper on a "
    "third thread, every frame equal to the serial run") {
    // Fonts are added before any Context runs and never during the run (§23.ai rule 4).
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    const std::vector<uint8_t> irHanded = compileHud("1.5s", "#ff8800");
    const std::vector<uint8_t> irParallel = compileHud("0.9s", "#8844ff");

    // The reference: both scenes on this thread, one after the other.
    std::vector<uint64_t> serialHanded;
    std::vector<uint64_t> serialParallel;
    {
        Scene handed(irHanded, shaper, 1);
        REQUIRE_MESSAGE(handed.error().empty(), handed.error());
        handed.run(0, kFrames);
        REQUIRE_MESSAGE(handed.error().empty(), handed.error());
        serialHanded = handed.hashes();
        CHECK(handed.glyphFrames() == kFrames);
        CHECK(handed.imageFrames() == kFrames);
        Scene parallel(irParallel, shaper, 2);
        REQUIRE_MESSAGE(parallel.error().empty(), parallel.error());
        parallel.run(0, kFrames);
        REQUIRE_MESSAGE(parallel.error().empty(), parallel.error());
        serialParallel = parallel.hashes();
    }
    REQUIRE(serialHanded.size() == static_cast<size_t>(kFrames));
    REQUIRE(serialParallel.size() == static_cast<size_t>(kFrames));
    // The two scenes really differ (a race between them would show as one taking the other's pixels).
    CHECK(serialHanded != serialParallel);

    // The threaded run. The handed scene is built on A and destroyed on this thread after the last
    // hand-over back; the parallel one lives entirely on its own thread.
    std::unique_ptr<Scene> handed;
    std::vector<uint64_t> threadedParallel;
    std::string parallelError;
    std::thread third([&] {
        Scene parallel(irParallel, shaper, 2);
        parallel.run(0, kFrames);
        threadedParallel = parallel.hashes();
        parallelError = parallel.error();
    });
    std::thread::id onA;
    std::thread::id onB;
    std::thread::id onAAgain;
    {
        Worker a;
        Worker b;
        a.post([&] {
            handed = std::make_unique<Scene>(irHanded, shaper, 1);
            handed->run(0, kHandAt1);
            onA = std::this_thread::get_id();
        });
        b.post([&] {
            handed->run(kHandAt1, kHandAt2);
            onB = std::this_thread::get_id();
        });
        a.post([&] {
            handed->run(kHandAt2, kFrames);
            onAAgain = std::this_thread::get_id();
        });
        CHECK(onA == a.id());
        CHECK(onB == b.id());
        CHECK(onAAgain == a.id());
    }
    third.join();

    REQUIRE(handed != nullptr);
    CHECK_MESSAGE(handed->error().empty(), handed->error());
    CHECK_MESSAGE(parallelError.empty(), parallelError);
    CHECK(onA != onB);
    REQUIRE(handed->hashes().size() == serialHanded.size());
    REQUIRE(threadedParallel.size() == serialParallel.size());
    int firstHanded = -1;
    int firstParallel = -1;
    for (int f = 0; f < kFrames; ++f) {
        if (firstHanded < 0 &&
            handed->hashes()[static_cast<size_t>(f)] != serialHanded[static_cast<size_t>(f)]) {
            firstHanded = f;
        }
        if (firstParallel < 0 &&
            threadedParallel[static_cast<size_t>(f)] != serialParallel[static_cast<size_t>(f)]) {
            firstParallel = f;
        }
    }
    CHECK_MESSAGE(firstHanded == -1, "handed Context: first differing frame " << firstHanded);
    CHECK_MESSAGE(firstParallel == -1, "parallel Context: first differing frame " << firstParallel);
    handed.reset();
}
