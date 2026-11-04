#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/shell/app.h"
#include "xt/shell/source_watcher.h"
#include "xt/text_shaper.h"

/// T-595 (Docs/design/54-source-format-v2.md §54.14 S5, 13 §13.8.3): `--watch` hot reload. A saved
/// source change is recompiled and hot-reloaded on the next poll (the next App frame); a save that
/// does not compile prints its diagnostics in xtframe-compile's format and keeps the previous
/// Document.

using namespace xt;
using namespace xt::shell;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSheet = "#label { width: 40px; height: 10px; }\n";

std::string documentText(const char* label) {
    return std::string("<xt><head><link href=\"main.xts\"/></head><body id=\"Root\"><span id=\"label\">") +
           label + "</span></body></xt>\n";
}

/// Writes `text` to `path` and moves its modification time forward by a whole second per call, so
/// a rewrite within the file system's timestamp resolution still reads as a change.
void writeSource(const fs::path& path, const std::string& text) {
    static int bump = 0;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(++bump), ec);
    REQUIRE(!ec);
}

/// A fresh UI directory `name` under the per-process temp root, holding main.xt + main.xts.
fs::path makeUi(const char* name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    writeSource(dir / "main.xt", documentText("one"));
    writeSource(dir / "main.xts", kSheet);
    return dir;
}

std::string mainPath(const fs::path& dir) { return compile::pathToUtf8(dir / "main.xt"); }

compile::CompileOptions options() {
    compile::CompileOptions o;
    o.emitMeta = false;
    return o;
}

DocumentHandle loadUi(Context& context, const fs::path& dir) {
    compile::CompileResult r = compile::compileDocument(mainPath(dir), options());
    REQUIRE(r.ok());
    Result<DocumentHandle> h = context.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, /*takeOwnership=*/true);
    REQUIRE(h.ok());
    return h.value();
}

std::string labelText(Context& context, DocumentHandle handle) {
    Document* doc = context.document(handle);
    REQUIRE(doc != nullptr);
    return std::string(doc->byId("label").text());
}

}  // namespace

TEST_CASE("T-595: SourceWatcher hot-reloads a saved change on the next poll") {
    const fs::path dir = makeUi("t595_reload");
    NullTextShaper shaper;
    Context context(ContextConfig{&shaper, nullptr});
    const DocumentHandle handle = loadUi(context, dir);
    context.update(0.0f);
    CHECK(labelText(context, handle) == "one");

    SourceWatcher watcher;
    std::vector<std::string> lines;
    std::vector<DocumentHandle> reloaded;
    watcher.setDiagnosticSink([&](const std::string& line) { lines.push_back(line); });
    watcher.setReloadedHandler([&](DocumentHandle h) { reloaded.push_back(h); });
    watcher.watch(mainPath(dir), handle, options());
    CHECK(watcher.active());
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Unchanged);

    // The UI file itself.
    const uint32_t serial = context.documentSerial(handle);
    writeSource(dir / "main.xt", documentText("two"));
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Reloaded);
    CHECK(context.documentSerial(handle) != serial);
    CHECK(labelText(context, handle) == "two");
    CHECK(reloaded == std::vector<DocumentHandle>{handle});
    CHECK(lines.empty());
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Unchanged);

    // A linked stylesheet and a flow graph next to it count too.
    writeSource(dir / "main.xts", "#label { width: 80px; height: 10px; }\n");
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Reloaded);
    writeSource(dir / "main.xtflow", "{}\n");
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Reloaded);
    CHECK(reloaded.size() == 3);

    // poll() snapshots at most once per interval.
    writeSource(dir / "main.xt", documentText("three"));
    const auto now = std::chrono::steady_clock::now();
    CHECK(watcher.poll(context, now - std::chrono::hours(1)) == SourceWatcher::PollResult::Unchanged);
    CHECK(labelText(context, handle) == "two");
    CHECK(watcher.poll(context, now + SourceWatcher::kDefaultInterval) ==
          SourceWatcher::PollResult::Reloaded);
    CHECK(labelText(context, handle) == "three");

    watcher.stop();
    writeSource(dir / "main.xt", documentText("four"));
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Unchanged);
    CHECK(labelText(context, handle) == "three");
}

TEST_CASE("T-595: a save that does not compile keeps the previous Document and prints one error line") {
    const fs::path dir = makeUi("t595_error");
    NullTextShaper shaper;
    Context context(ContextConfig{&shaper, nullptr});
    const DocumentHandle handle = loadUi(context, dir);

    SourceWatcher watcher;
    std::vector<std::string> lines;
    int reloads = 0;
    watcher.setDiagnosticSink([&](const std::string& line) { lines.push_back(line); });
    watcher.setReloadedHandler([&](DocumentHandle) { ++reloads; });
    watcher.watch(mainPath(dir), handle, options());

    const uint32_t serial = context.documentSerial(handle);
    writeSource(dir / "main.xts", "#label { width: 40px; height: 10px;\n");
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Failed);
    CHECK(context.documentSerial(handle) == serial);
    CHECK(labelText(context, handle) == "one");
    CHECK(reloads == 0);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("main.xts") != std::string::npos);
    CHECK(lines[0].find(": error: ") != std::string::npos);
    // Not retried until the next save.
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Unchanged);
    CHECK(lines.size() == 1);

    // The next save that compiles goes through.
    writeSource(dir / "main.xts", kSheet);
    CHECK(watcher.pollNow(context) == SourceWatcher::PollResult::Reloaded);
    CHECK(reloads == 1);
}

TEST_CASE("T-595: formatCompileDiagnostic matches xtframe-compile's lines") {
    compile::Diagnostic positioned{compile::Diagnostic::Level::Error, "a.xt", 3, 7, "bad thing"};
    CHECK(formatCompileDiagnostic(positioned) == "a.xt:3:7: error: bad thing");
    compile::Diagnostic bare{compile::Diagnostic::Level::Warning, "", 0, 0, "careful"};
    CHECK(formatCompileDiagnostic(bare) == "<input>: warning: careful");
}

TEST_CASE("T-595: App with installWatch shows a saved change on the next frame") {
    WindowConfig config;
    config.width = 160;
    config.height = 120;
    config.hidden = true;
    config.vsync = false;
    config.msaa = 0;
    SourceWatcher watcher;  // T-598: outlives the App's frame poll.
    App app(config);
    if (!app.ok()) {
        MESSAGE("skipped: no window could be created (" << app.lastError() << ")");
        return;
    }
    const fs::path dir = makeUi("t595_app");
    const DocumentHandle handle = loadUi(app.context(), dir);
    app.setCurrentDocument(handle);
    app.syncViewport();
    watcher.setInterval(std::chrono::milliseconds(0));
    watcher.setDiagnosticSink([](const std::string&) {});
    int reloads = 0;
    watcher.setReloadedHandler([&](DocumentHandle) { ++reloads; });
    watcher.watch(mainPath(dir), handle, options());
    installWatch(app, watcher);

    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(reloads == 0);
    writeSource(dir / "main.xt", documentText("two"));
    CHECK(app.runFrame([](float) {}, 0.016f, /*swap=*/false));
    CHECK(reloads == 1);
    CHECK(labelText(app.context(), handle) == "two");
}
