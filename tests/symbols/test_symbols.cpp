#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "golden_common.h"
#include "json.hpp"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"

/// T-198 (docs/tasks/T-198.md goal 4): the standard symbol library (`editor/symbols`,
/// docs/design/30-ui-project.md 30.4) and the starter UI Project (`examples/ui-project-starter`).
///   - index.json lists exactly the library's files and repeats each symbol's contract;
///   - every symbol compiles with --strict (its `requires` contract is checked by the compiler)
///     and a symbol that breaks its contract does not;
///   - no literal colors outside the token files;
///   - every state of every symbol matches its SW golden (tests/symbols/manifest.txt; set
///     XT_SYMBOLS_UPDATE=1 to rewrite tests/symbols/expected/) and differs from the normal state;
///   - every symbol timeline plays through the host API (playTimeline(<instance id>, <name>));
///   - timelines stay inside contracts/motion-spec.yaml (at most 500 ms, transform / opacity /
///     text-reveal only; the host-driven `cooldown` progress timeline is exempt);
///   - the starter UI Project packs with --strict and no diagnostics, pulling in exactly the
///     standard symbols its UIs link, never over a project file, and every packed UI renders.

using namespace xt;

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// XT_SYMBOLS_UPDATE=1 in the environment rewrites tests/symbols/expected/.
bool updateGoldens() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_SYMBOLS_UPDATE") != 0 || v == nullptr) {
        return false;
    }
    const bool on = v[0] == '1';
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_SYMBOLS_UPDATE");
    return v != nullptr && v[0] == '1';
#endif
}

const json& symbolIndex() {
    static const json index = json::parse(readText(fs::path(XT_SYMBOLS_DIR) / "index.json"));
    return index;
}

std::vector<std::string> symbolNames() {
    std::vector<std::string> names;
    for (const json& entry : symbolIndex()["symbols"]) {
        names.push_back(entry["name"].get<std::string>());
    }
    return names;
}

fs::path fixturePath(const std::string& name) {
    return fs::path(XT_SYMBOLS_TEST_DIR) / "fixtures" / (name + ".preview.xt");
}

compile::CompileResult compileStrict(const fs::path& xt, std::vector<std::string> implied = {}) {
    compile::CompileOptions options;
    options.rootDir = xt.parent_path().string();
    options.strict = true;
    options.impliedStylesheets = std::move(implied);
    return compile::compileDocument(xt.string(), options);
}

std::string diagnosticsText(const compile::CompileResult& result) {
    std::string text;
    for (const compile::Diagnostic& d : result.diagnostics) {
        text += d.file + ":" + std::to_string(d.line) + ": " + d.message + "\n";
    }
    return text;
}

/// The id of the first `<use symbol="<name>">` in `text`.
std::string firstInstanceId(const std::string& text, const std::string& name) {
    const std::regex use("<use symbol=\"" + name + "\" id=\"([A-Za-z0-9_]+)\"");
    std::smatch match;
    return std::regex_search(text, match, use) ? match[1].str() : std::string();
}

/// The `<style>` bodies and `style="..."` attributes of an `.xt`, or the whole text of an `.xts`.
std::vector<std::string> styleRegions(const fs::path& path) {
    const std::string text = readText(path);
    if (path.extension() == ".xts") {
        return {text};
    }
    std::vector<std::string> out;
    const std::regex style("<style>([\\s\\S]*?)</style>|style=\"([^\"]*)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), style), end; it != end; ++it) {
        out.push_back((*it)[1].matched ? (*it)[1].str() : (*it)[2].str());
    }
    return out;
}

/// Declaration values in a stylesheet region with a literal color: `#hex`, `rgb[a]()`, `hsl()` or
/// one of 05 §5.3's named colors.
std::vector<std::string> literalColors(const std::string& css) {
    static const std::regex comment("/\\*[\\s\\S]*?\\*/");
    static const std::regex decl("([A-Za-z-]+)\\s*:\\s*([^;{}]+)");
    static const std::regex hex("#[0-9a-fA-F]{3,8}\\b|\\brgba?\\(|\\bhsla?\\(");
    static const std::regex named(
        "(^|[\\s,(])(black|silver|gray|white|maroon|red|purple|fuchsia|green|lime|olive|yellow|navy|blue|"
        "teal|"
        "aqua|orange)($|[\\s,)])");
    const std::string clean = std::regex_replace(css, comment, " ");
    std::vector<std::string> out;
    for (std::sregex_iterator it(clean.begin(), clean.end(), decl), end; it != end; ++it) {
        const std::string value = (*it)[2].str();
        if (std::regex_search(value, hex) || std::regex_search(value, named)) {
            out.push_back((*it)[1].str() + ": " + value);
        }
    }
    return out;
}

struct TimelineInfo {
    std::string name;
    float lastTime = 0;
    std::set<std::string> props;
};

/// Every `@timeline` block of a stylesheet text: its last key time and animated properties.
std::vector<TimelineInfo> timelines(const std::string& text) {
    std::vector<TimelineInfo> out;
    static const std::regex head("@timeline\\s+([A-Za-z0-9_-]+)\\s*\\{");
    static const std::regex time("(^|[\\s{};])([0-9]+(\\.[0-9]+)?)(ms|s)\\b");
    static const std::regex prop("([a-z-]+)\\s*:");
    for (std::sregex_iterator it(text.begin(), text.end(), head), end; it != end; ++it) {
        TimelineInfo info;
        info.name = (*it)[1].str();
        size_t pos = static_cast<size_t>(it->position() + it->length());
        int depth = 1;
        const size_t start = pos;
        while (pos < text.size() && depth > 0) {
            depth += text[pos] == '{' ? 1 : text[pos] == '}' ? -1 : 0;
            ++pos;
        }
        const std::string body = text.substr(start, pos - start);
        for (std::sregex_iterator t(body.begin(), body.end(), time), tend; t != tend; ++t) {
            const float value = std::stof((*t)[2].str()) * ((*t)[4].str() == "ms" ? 0.001f : 1.0f);
            info.lastTime = std::max(info.lastTime, value);
        }
        for (std::sregex_iterator p(body.begin(), body.end(), prop), pend; p != pend; ++p) {
            const std::string name = (*p)[1].str();
            if (name != "duration" && name != "marker") {
                info.props.insert(name);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

/// T-241: every `.xt` of the library: the root and its `mobile/` subset (T-382: and `list/`).
std::vector<fs::path> libraryFiles() {
    std::vector<fs::path> files;
    for (const fs::path& dir :
         {fs::path(XT_SYMBOLS_DIR), fs::path(XT_SYMBOLS_DIR) / "mobile", fs::path(XT_SYMBOLS_DIR) / "list"}) {
        for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".xt") {
                files.push_back(e.path());
            }
        }
    }
    return files;
}

/// A fresh copy of the starter UI Project without pack output or pulled-in symbols.
fs::path copiedStarter(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_symbols_starter_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(XT_STARTER_DIR, dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    fs::remove_all(dir / "out", ec);
    for (fs::directory_iterator it(dir / "shared" / "symbols", ec), end; it != end; it.increment(ec)) {
        if (it->path().extension() == ".xt") {
            fs::remove(it->path(), ec);
        }
    }
    return dir;
}

/// The standard symbol names the starter's UIs link (`shared/symbols/<Name>.xt`).
std::set<std::string> linkedSymbols(const fs::path& root) {
    std::set<std::string> names;
    const std::regex link("<link rel=\"symbols\" href=\"\\.\\./\\.\\./shared/symbols/([A-Za-z]+)\\.xt\"");
    for (const char* ui : {"hud/hud.xt", "main-menu/main-menu.xt", "settings/settings.xt"}) {
        const std::string text = readText(root / "ui" / ui);
        for (std::sregex_iterator it(text.begin(), text.end(), link), end; it != end; ++it) {
            names.insert((*it)[1].str());
        }
    }
    return names;
}

}  // namespace

TEST_CASE("symbols: index.json lists every library file and repeats its contract") {
    const json& index = symbolIndex();
    CHECK(index["version"] == 1);
    CHECK(index["tokens"] == "tokens.xts");

    std::set<std::string> files;
    for (const fs::path& file : libraryFiles()) {
        files.insert(file.stem().string());
    }
    const std::vector<std::string> names = symbolNames();
    CHECK(names.size() == 26 + 11 + 1);  // T-241: 11 in mobile/; T-382: ListItem in list/.
    CHECK(std::set<std::string>(names.begin(), names.end()) == files);

    for (const json& entry : index["symbols"]) {
        const std::string name = entry["name"];
        CAPTURE(name);
        const std::string text = readText(fs::path(XT_SYMBOLS_DIR) / entry["file"].get<std::string>());
        CHECK(text.find("<symbol name=\"" + name + "\"") != std::string::npos);
        CHECK(fs::exists(fs::path(XT_SYMBOLS_DIR) / entry["thumbnail"].get<std::string>()));
        CHECK(fs::exists(fixturePath(name)));
        const std::string category = entry["category"];
        CHECK((category == "form" || category == "hud" || category == "input" || category == "mobile" ||
               category == "list"));
        CHECK(index["categories"].contains(category));
        // T-241: the mobile subset lives in mobile/, the rest at the library root (T-382: list/ too).
        CHECK((entry["file"].get<std::string>().rfind("mobile/", 0) == 0) == (category == "mobile"));
        CHECK((entry["file"].get<std::string>().rfind("list/", 0) == 0) == (category == "list"));
        CHECK(!entry["description"].get<std::string>().empty());

        // The contract in index.json is the one the symbol declares.
        std::smatch match;
        REQUIRE(std::regex_search(text, match, std::regex("requires=\"([^\"]*)\"")));
        std::string expected;
        const json& states = entry["requires"]["states"];
        const json& slotList = entry["requires"]["slots"];
        auto join = [](const json& list) {
            std::string out;
            for (const json& item : list) {
                out += (out.empty() ? "" : ", ") + item.get<std::string>();
            }
            return out;
        };
        if (!states.empty()) {
            expected = "states: " + join(states);
        }
        if (!slotList.empty()) {
            expected += (expected.empty() ? "" : "; ") + std::string("slots: ") + join(slotList);
        }
        CHECK(match[1].str() == expected);
        for (const json& port : entry["ports"]) {
            std::string type = port["type"];
            type = std::regex_replace(type, std::regex("<"), "&lt;");
            type = std::regex_replace(type, std::regex(">"), "&gt;");
            const std::string decl = "port:" + port["name"].get<std::string>() + "=\"" + type + " " +
                                     port["dir"].get<std::string>() + "\"";
            CHECK_MESSAGE(text.find(decl) != std::string::npos, decl);
        }
        for (const json& timeline : entry["timelines"]) {
            CHECK(text.find("@timeline " + timeline.get<std::string>() + " ") != std::string::npos);
        }
        for (const json& token : entry["tokens"]) {
            CHECK(text.find("var(--" + token.get<std::string>() + ")") != std::string::npos);
        }
    }
}

TEST_CASE("symbols: every symbol compiles with --strict and keeps its contract") {
    for (const std::string& name : symbolNames()) {
        CAPTURE(name);
        const compile::CompileResult result = compileStrict(fixturePath(name));
        CHECK_MESSAGE(result.ok(), diagnosticsText(result));
        CHECK_MESSAGE(result.diagnostics.empty(), diagnosticsText(result));
        if (!result.ok()) {
            continue;
        }
        Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
        REQUIRE(view.ok());
        bool instanced = false;
        for (const IrSymbolInstance& instance : view.value().symbols().instances()) {
            instanced = instanced || view.value().string(instance.symbol) == name;
        }
        CHECK(instanced);
    }
}

TEST_CASE("symbols: the mobile subset has touch sizes, a timeline and a full contract (T-241)") {
    const std::string tokens = readText(fs::path(XT_SYMBOLS_DIR) / "tokens.xts");
    CHECK(tokens.find("--touch-min: 44px;") != std::string::npos);
    CHECK(tokens.find("--touch-lg: 88px;") != std::string::npos);
    CHECK(symbolIndex()["categories"].contains("mobile"));
    const std::set<std::string> expected{"VirtualJoystick", "TouchButton", "DPad",         "SwipeCard",
                                         "BottomSheet",     "TabBar",      "TopBar",       "PullIndicator",
                                         "ToggleTouch",     "SliderTouch", "PinchZoomView"};
    std::set<std::string> mobile;
    for (const json& entry : symbolIndex()["symbols"]) {
        if (entry["category"] != "mobile") {
            continue;
        }
        const std::string name = entry["name"];
        CAPTURE(name);
        mobile.insert(name);
        CHECK(entry["file"] == "mobile/" + name + ".xt");
        CHECK(entry["thumbnail"] == "mobile/" + name + ".png");
        CHECK(!entry["timelines"].empty());
        CHECK(!entry["notes"].get<std::string>().empty());
        CHECK((!entry["requires"]["states"].empty() || !entry["requires"]["slots"].empty()));
        const std::string text = readText(fs::path(XT_SYMBOLS_DIR) / entry["file"].get<std::string>());
        // Every mobile symbol sizes its hit areas with the touch tokens (or is a bar that holds them).
        const bool touch = text.find("var(--touch-min)") != std::string::npos ||
                           text.find("var(--touch-lg)") != std::string::npos;
        CHECK_MESSAGE((touch || name == "DPad" || name == "SwipeCard" || name == "PinchZoomView"), name);
    }
    CHECK(mobile == expected);
    // The named ports of the 23.11.2 contract.
    auto hasPort = [](const std::string& name, const std::string& port, const std::string& type,
                      const std::string& dir) {
        for (const json& entry : symbolIndex()["symbols"]) {
            if (entry["name"] != name) {
                continue;
            }
            for (const json& p : entry["ports"]) {
                if (p["name"] == port && p["type"] == type && p["dir"] == dir) {
                    return true;
                }
            }
        }
        return false;
    };
    CHECK(hasPort("VirtualJoystick", "axis", "vec2", "out"));
    CHECK(hasPort("VirtualJoystick", "dead", "number", "in"));
    CHECK(hasPort("TouchButton", "pressed", "event", "out"));
    CHECK(hasPort("TouchButton", "held", "bool", "out"));
    CHECK(hasPort("TouchButton", "repeat", "event", "out"));
    CHECK(hasPort("TouchButton", "cooldown", "number", "in"));
    CHECK(hasPort("DPad", "dir", "event", "out"));
    CHECK(hasPort("DPad", "axis", "vec2", "out"));
    CHECK(hasPort("SwipeCard", "swipe", "event", "out"));
    CHECK(hasPort("BottomSheet", "open", "bool", "in"));
    CHECK(hasPort("TabBar", "value", "number", "in"));
    CHECK(hasPort("TabBar", "select", "event", "out"));
    CHECK(hasPort("PullIndicator", "progress", "number", "in"));
    CHECK(hasPort("PinchZoomView", "scale", "number", "in"));
    CHECK(hasPort("PinchZoomView", "min", "number", "in"));
    CHECK(hasPort("PinchZoomView", "max", "number", "in"));
}

TEST_CASE("symbols: a symbol that drops a required state fails its contract") {
    // Button without any :active rule no longer provides the Pressed state it requires.
    std::string button = readText(fs::path(XT_SYMBOLS_DIR) / "Button.xt");
    button = std::regex_replace(button, std::regex(":active"), ".was-active");
    const std::string tokens = readText(fs::path(XT_SYMBOLS_DIR) / "tokens.xts");
    const std::string doc =
        "<xt><head><link href=\"tokens.xts\"/><link rel=\"symbols\" href=\"Button.xt\"/></head>"
        "<body><use symbol=\"Button\" id=\"B\"/></body></xt>";
    std::vector<compile::SourceFile> files{{"doc.xt", doc}, {"Button.xt", button}, {"tokens.xts", tokens}};
    compile::CompileOptions options;
    options.rootDir = XT_SYMBOLS_DIR;
    const compile::CompileResult result = compile::compileDocumentFromStrings(files, "doc.xt", options);
    CHECK_FALSE(result.ok());
    CHECK(diagnosticsText(result).find("requires state \"Pressed\"") != std::string::npos);
}

TEST_CASE("symbols: no literal colors outside the token files") {
    std::vector<fs::path> files = libraryFiles();
    const fs::path starter(XT_STARTER_DIR);
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(starter / "ui")) {
        if (e.path().extension() == ".xt" || e.path().extension() == ".xts") {
            files.push_back(e.path());
        }
    }
    for (const fs::directory_entry& e : fs::directory_iterator(starter / "conventions" / "layouts")) {
        files.push_back(e.path());
    }
    CHECK(files.size() >= 37 + 6 + 3);
    for (const fs::path& file : files) {
        CAPTURE(file.string());
        for (const std::string& region : styleRegions(file)) {
            const std::vector<std::string> found = literalColors(region);
            CHECK_MESSAGE(found.empty(), (found.empty() ? std::string() : found.front()));
        }
    }
    // The scanner itself finds what it must.
    CHECK(literalColors("a { color: #fff; }").size() == 1);
    CHECK(literalColors("a { border: 1px solid red; background-color: rgba(0, 0, 0, 0.5); }").size() == 2);
    CHECK(literalColors("#Box { color: var(--color-text); border-color: transparent; }").empty());
}

TEST_CASE("symbols: every state matches its golden and differs from the normal state") {
    const std::string dir = XT_SYMBOLS_TEST_DIR;
    const std::vector<golden::UiFixture> fixtures = golden::loadUiManifest(dir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);  // T-267a: no fixture dropped by a bad manifest line.
    const bool update = updateGoldens();
    std::map<std::string, std::vector<uint8_t>> normal;
    std::set<std::string> covered;
    for (const golden::UiFixture& fx : fixtures) {
        CAPTURE(fx.name);
        const std::vector<uint8_t> actual = golden::renderUiFixture(fx, dir);
        REQUIRE(!actual.empty());
        const std::string symbol = fx.name.substr(4, fx.name.rfind('_') - 4);  // sym_<Name>_<state>
        covered.insert(symbol);
        const fs::path expectedPath = fs::path(dir) / "expected" / (fx.name + ".png");
        if (update) {
            fs::create_directories(expectedPath.parent_path());
            CHECK(sw::writePng(expectedPath.string(), actual.data(), fx.width, fx.height, fx.width * 4));
        } else {
            std::vector<uint8_t> expected;
            int width = 0;
            int height = 0;
            REQUIRE_MESSAGE(sw::readPng(expectedPath.string(), expected, width, height),
                            "missing golden (run with XT_SYMBOLS_UPDATE=1)");
            REQUIRE(width == fx.width);
            REQUIRE(height == fx.height);
            // T-267a (review L6): SW against an SW golden, so no pixel may differ by more than 2/255.
            // T-267d: XT_GOLDEN_CHANNEL / XT_GOLDEN_RATIO override that for one run.
            const golden::SwTolerance tolerance = golden::swTolerance();
            const golden::GoldenResult result = golden::compareRgba(
                actual.data(), expected.data(), fx.width, fx.height, tolerance.channel, tolerance.ratio);
            if (result.pass && golden::goldenVerbose() && result.maxChannelDiff > 0) {
                MESSAGE(golden::describeDiff("[DIFF]", fx.name, result, tolerance.ratio, tolerance.channel));
            }
            if (!result.pass) {
                const fs::path out(XT_SYMBOLS_OUT_DIR);
                fs::create_directories(out);
                sw::writePng((out / (fx.name + ".actual.png")).string(), actual.data(), fx.width, fx.height,
                             fx.width * 4);
                golden::writeDiffPng((out / (fx.name + ".diff.png")).string(), actual.data(), expected.data(),
                                     fx.width, fx.height, tolerance.channel);
            }
            CHECK_MESSAGE(result.pass, golden::describeDiff("[FAIL]", fx.name, result, tolerance.ratio,
                                                            tolerance.channel));
        }
        if (fx.name.size() > 7 && fx.name.compare(fx.name.size() - 7, 7, "_normal") == 0) {
            normal[symbol] = actual;
        } else {
            REQUIRE(normal.count(symbol) == 1);  // The manifest lists `normal` first.
            const golden::GoldenResult same =
                golden::compareRgba(actual.data(), normal[symbol].data(), fx.width, fx.height, 2, 0.0f);
            CHECK_MESSAGE(!same.pass, "state renders exactly like the normal state");
        }
    }
    const std::vector<std::string> names = symbolNames();
    CHECK(covered == std::set<std::string>(names.begin(), names.end()));
}

TEST_CASE("symbols: every timeline plays through the host API") {
    NullTextShaper shaper;
    for (const json& entry : symbolIndex()["symbols"]) {
        const std::string name = entry["name"];
        if (entry["timelines"].empty()) {
            continue;
        }
        CAPTURE(name);
        const compile::CompileResult result = compileStrict(fixturePath(name));
        REQUIRE(result.ok());
        const std::string instance = firstInstanceId(readText(fixturePath(name)), name);
        REQUIRE(!instance.empty());
        for (const json& timeline : entry["timelines"]) {
            const std::string tl = timeline;
            CAPTURE(tl);
            Context ctx(ContextConfig{&shaper, nullptr});
            ctx.setViewport(Rect{0, 0, 480, 320}, 1.0f);
            Result<DocumentHandle> handle =
                ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/false);
            REQUIRE(handle.ok());
            Document* doc = ctx.document(handle.value());
            REQUIRE(doc != nullptr);
            ctx.update(0.0f);
            REQUIRE(doc->playTimeline(instance, tl) == Status::kOk);
            CHECK(doc->isTimelinePlaying(instance, tl));
            ctx.update(0.05f);
            CHECK(doc->timelineTime(instance, tl) > 0.0f);
        }
    }
}

TEST_CASE("symbols: timelines stay inside the motion spec") {
    std::vector<fs::path> files = libraryFiles();
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(fs::path(XT_STARTER_DIR) / "ui")) {
        if (e.path().extension() == ".xts") {
            files.push_back(e.path());
        }
    }
    size_t count = 0;
    for (const fs::path& file : files) {
        for (const TimelineInfo& t : timelines(readText(file))) {
            CAPTURE(file.string());
            CAPTURE(t.name);
            ++count;
            if (t.name == "cooldown") {
                continue;  // A progress timeline the host seeks, not motion.
            }
            CHECK(t.lastTime <= 0.5f);  // motion-spec.yaml duration.slow upper bound.
            for (const std::string& prop : t.props) {
                CAPTURE(prop);
                CHECK((prop == "transform" || prop == "opacity" || prop == "text-reveal"));
            }
        }
    }
    CHECK(count >= 15);
}

TEST_CASE("symbols: conventions/layouts templates compile with the starter tokens") {
    const fs::path starter(XT_STARTER_DIR);
    const std::string tokens = (starter / "shared" / "tokens.xts").string();
    for (const char* layout : {"hud.xt", "main-menu.xt", "inventory.xt"}) {
        CAPTURE(layout);
        const compile::CompileResult result =
            compileStrict(starter / "conventions" / "layouts" / layout, {tokens});
        CHECK_MESSAGE(result.ok(), diagnosticsText(result));
        CHECK_MESSAGE(result.diagnostics.empty(), diagnosticsText(result));
    }
}

TEST_CASE("symbols: the starter UI Project packs strictly and pulls in only the symbols it uses") {
    const fs::path root = copiedStarter("pack");
    // A project symbol overrides the standard one of the same name: pack must not replace it.
    const std::string counter =
        "<!-- project override -->\n" + readText(fs::path(XT_SYMBOLS_DIR) / "Counter.xt");
    {
        std::ofstream out(root / "shared" / "symbols" / "Counter.xt", std::ios::binary);
        out << counter;
    }

    std::string error;
    Result<bundle::UiProject> project = bundle::UiProject::load(root.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = (root / "out").string();
    options.strict = true;
    options.standardSymbolDir = XT_SYMBOLS_DIR;
    const bundle::PackReport report = bundle::pack(project.value(), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    REQUIRE_MESSAGE(report.ok, diagnostics);
    CHECK_MESSAGE(report.diagnostics.empty(), diagnostics);
    CHECK(report.packedUis == std::vector<std::string>{"hud", "main-menu", "settings"});

    // Exactly the linked standard symbols were copied, byte for byte, except the override.
    std::set<std::string> expected = linkedSymbols(root);
    CHECK(expected.size() >= 20);
    expected.erase("Counter");
    std::set<std::string> copied;
    for (const std::string& path : report.standardSymbolsCopied) {
        CHECK(path.rfind("shared/symbols/", 0) == 0);
        copied.insert(fs::path(path).stem().string());
    }
    CHECK(copied == expected);
    for (const std::string& name : copied) {
        CHECK(readText(root / "shared" / "symbols" / (name + ".xt")) ==
              readText(fs::path(XT_SYMBOLS_DIR) / (name + ".xt")));
    }
    CHECK(readText(root / "shared" / "symbols" / "Counter.xt") == counter);
    for (const std::string unused : {"Toast", "Tooltip"}) {
        if (expected.count(unused) == 0) {
            CHECK_FALSE(fs::exists(root / "shared" / "symbols" / (unused + ".xt")));
        }
    }

    // The manifest carries the theme, both string tables and the motion spec.
    const json manifest = json::parse(readText(root / "out" / "manifest.json"));
    CHECK(manifest["themes"].size() == 1);
    CHECK(manifest["themes"][0]["name"] == "dark");
    CHECK(manifest["strings"].contains("en"));
    CHECK(manifest["strings"].contains("zh-TW"));
    CHECK(manifest.contains("motionSpec"));
    CHECK(!manifest["tokens"].empty());

    // Every packed UI loads and draws something (what `xtframe-view out/ui/<id>.xtir` shows).
    NullTextShaper shaper;
    FileAssetResolver assets((root / "out").generic_string(), sw::decodeImageBytes);
    for (const char* id : {"hud", "main-menu", "settings"}) {
        CAPTURE(id);
        const std::string ir = readText(root / "out" / "ui" / (std::string(id) + ".xtir"));
        REQUIRE(!ir.empty());
        Context ctx(ContextConfig{&shaper, &assets});
        // Before the load: loadDocument() applies the Context viewport to the new Document
        // (13 §13.8.14, T-236); the starter's roots are sized in vw/vh since T-210, so a 0x0
        // viewport would draw nothing.
        ctx.setViewport(Rect{0, 0, 1280, 720}, 1.0f);
        Result<DocumentHandle> handle = ctx.loadDocument(
            IrBlob{reinterpret_cast<const uint8_t*>(ir.data()), ir.size()}, /*takeOwnership=*/false);
        REQUIRE(handle.ok());
        ctx.update(0.0f);
        CommandList list;
        ctx.render(list);
        std::vector<uint8_t> pixels(1280u * 720u * 4u, 0);
        sw::SwRenderer renderer;
        renderer.setTarget(sw::SwTarget{pixels.data(), 1280, 720, 1280 * 4});
        renderer.clear(Color{0, 0, 0, 0});
        renderer.render(list, &shaper, &assets, &ctx.document(handle.value())->ir());
        size_t drawn = 0;
        for (size_t i = 3; i < pixels.size(); i += 4) {
            drawn += pixels[i] != 0 ? 1u : 0u;
        }
        CHECK(drawn > 1000);
    }

    // A second run finds every symbol in place and copies nothing.
    const bundle::PackReport again = bundle::pack(project.value(), options);
    CHECK(again.ok);
    CHECK(again.standardSymbolsCopied.empty());
}

TEST_CASE("symbols: pack pulls nothing without a standard symbol directory") {
    const fs::path root = copiedStarter("nodir");
    std::string error;
    Result<bundle::UiProject> project = bundle::UiProject::load(root.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = (root / "out").string();
    const bundle::PackReport report = bundle::pack(project.value(), options);
    CHECK_FALSE(report.ok);  // The links point at files that do not exist.
    CHECK(report.standardSymbolsCopied.empty());
    CHECK_FALSE(fs::exists(root / "shared" / "symbols" / "Button.xt"));
}

TEST_CASE("symbols: the mobile-hud demo packs strictly with the mobile symbols and draws portrait (T-241)") {
    const fs::path dir = xt::test::tempDir() / "xt_symbols_mobile_hud";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(XT_MOBILE_HUD_DIR, dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    fs::remove_all(dir / "out", ec);
    for (fs::directory_iterator it(dir / "shared" / "symbols", ec), end; it != end; it.increment(ec)) {
        if (it->path().extension() == ".xt") {
            fs::remove(it->path(), ec);
        }
    }

    std::string error;
    Result<bundle::UiProject> project = bundle::UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    CHECK(project.value().spec.uiscaleRule == bundle::UiScaleRule::ShortEdge);
    CHECK(project.value().spec.logicalHeight == doctest::Approx(1080.0f));
    REQUIRE(project.value().spec.platforms.size() == 1);
    CHECK(project.value().spec.platforms[0].name == "mobile");
    // The project's tokens are the standard ones.
    const std::string standard = readText(fs::path(XT_SYMBOLS_DIR) / "tokens.xts");
    const std::string own = readText(dir / "shared" / "tokens.xts");
    CHECK(own.substr(own.find(":root")) == standard.substr(standard.find(":root")));

    bundle::PackOptions options;
    options.outDir = (dir / "out").string();
    options.strict = true;
    options.standardSymbolDir = XT_SYMBOLS_DIR;
    const bundle::PackReport report = bundle::pack(project.value(), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    REQUIRE_MESSAGE(report.ok, diagnostics);
    CHECK_MESSAGE(report.diagnostics.empty(), diagnostics);
    CHECK(report.packedUis == std::vector<std::string>{"hud", "settings"});
    // The mobile symbols came from the library's mobile/ subset.
    std::set<std::string> copied;
    for (const std::string& path : report.standardSymbolsCopied) {
        copied.insert(fs::path(path).stem().string());
    }
    for (const char* name : {"VirtualJoystick", "TouchButton", "TopBar", "BottomSheet", "TabBar",
                             "ToggleTouch", "SliderTouch", "HealthBar", "Counter"}) {
        CAPTURE(name);
        CHECK(copied.count(name) == 1);
    }
    CHECK(readText(dir / "shared" / "symbols" / "TouchButton.xt") ==
          readText(fs::path(XT_SYMBOLS_DIR) / "mobile" / "TouchButton.xt"));

    const json manifest = json::parse(readText(dir / "out" / "manifest.json"));
    CHECK(manifest["spec"]["uiscaleRule"] == "shortEdge");
    CHECK(manifest["spec"]["logicalHeight"] == 1080);

    // Both UIs load at the portrait 1080x2400 viewport and draw.
    NullTextShaper shaper;
    FileAssetResolver assets((dir / "out").generic_string(), sw::decodeImageBytes);
    for (const char* id : {"hud", "settings"}) {
        CAPTURE(id);
        const std::string ir = readText(dir / "out" / "ui" / (std::string(id) + ".xtir"));
        REQUIRE(!ir.empty());
        Context ctx(ContextConfig{&shaper, &assets});
        ctx.setViewport(Rect{0, 0, 1080, 2400}, 1.0f);
        ctx.setUiScale(bundle::uiScaleFor(bundle::UiScaleRule::ShortEdge, 1080.0f, 2400.0f, 1080.0f));
        Result<DocumentHandle> handle = ctx.loadDocument(
            IrBlob{reinterpret_cast<const uint8_t*>(ir.data()), ir.size()}, /*takeOwnership=*/false);
        REQUIRE(handle.ok());
        ctx.update(0.0f);
        ctx.update(0.6f);
        CommandList list;
        ctx.render(list);
        std::vector<uint8_t> pixels(1080u * 2400u * 4u, 0);
        sw::SwRenderer renderer;
        renderer.setTarget(sw::SwTarget{pixels.data(), 1080, 2400, 1080 * 4});
        renderer.clear(Color{0, 0, 0, 0});
        renderer.render(list, &shaper, &assets, &ctx.document(handle.value())->ir());
        size_t drawn = 0;
        for (size_t i = 3; i < pixels.size(); i += 4) {
            drawn += pixels[i] != 0 ? 1u : 0u;
        }
        CHECK(drawn > 100000);
    }
}

TEST_CASE("symbols: ListItem shows the three data paths and its preview list binds per entry (T-382)") {
    // 30 §30.13: in-ports for values, a slot for free content, bind:state for states.
    const std::string text = readText(fs::path(XT_SYMBOLS_DIR) / "list" / "ListItem.xt");
    CHECK(text.find("port:title=\"string in\"") != std::string::npos);
    CHECK(text.find("port:selected=\"bool in\"") != std::string::npos);
    CHECK(text.find("<slot name=\"trailing\">") != std::string::npos);
    CHECK(text.find("bind:state.Selected=\"selected\"") != std::string::npos);
    CHECK(text.find("on:dblclick=\"emit(dblactivate)\"") != std::string::npos);
    CHECK(symbolIndex()["categories"].contains("list"));

    // The preview: a <repeat> whose template instance takes bind:title="item(title)".
    const std::string preview = readText(fixturePath("ListItem"));
    CHECK(preview.find("<repeat port=\"items\"") != std::string::npos);
    CHECK(preview.find("bind:title=\"item(title)\"") != std::string::npos);
    const compile::CompileResult result = compileStrict(fixturePath("ListItem"));
    REQUIRE_MESSAGE(result.ok(), diagnosticsText(result));
    CHECK_MESSAGE(result.diagnostics.empty(), diagnosticsText(result));

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setViewport(Rect{0, 0, 340, 220}, 1.0f);
    Result<DocumentHandle> handle =
        ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/false);
    REQUIRE(handle.ok());
    Document& doc = *ctx.document(handle.value());
    const PortId items = doc.lookupPort("List.items");
    REQUIRE(items != kInvalidPort);
    CHECK(doc.setPortText(items, R"([{"title":"Save 1","subtitle":"a","badge":"3h","selected":false},)"
                                 R"({"title":"Save 2","subtitle":"b","badge":"5h","selected":true}])") ==
          Status::kOk);
    ctx.update(0.0f);
    // Each entry has its own copy of the instance's in-ports (27 §27.3.4, T-380).
    CHECK(doc.portText(doc.lookupPort("Rows/0/Item.title")) == "Save 1");
    CHECK(doc.portText(doc.lookupPort("Rows/1/Item.title")) == "Save 2");
    CHECK(doc.portText(doc.lookupPort("Rows/1/Item.selected")) == "true");
}

TEST_CASE("symbols: xtframe-pack and xt-standard: find the list/ subset (T-382)") {
    const fs::path root = xt::test::tempDir() / "xt_symbols_list_pack";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "ui" / "list", ec);
    fs::create_directories(root / "shared" / "symbols", ec);
    {
        std::ofstream out(root / "List.xtui", std::ios::binary);
        out << R"({ "version": 1, "name": "List", "id": "list",
  "uis": [ { "id": "list", "doc": "ui/list/list.xt", "size": [400, 300] } ],
  "shared": { "styles": [], "tokens": "shared/tokens.xts", "symbols": "shared/symbols" } })";
    }
    {
        std::ofstream out(root / "shared" / "tokens.xts", std::ios::binary);
        out << readText(fs::path(XT_SYMBOLS_DIR) / "tokens.xts");
    }
    {
        std::ofstream out(root / "ui" / "list" / "list.xt", std::ios::binary);
        out << "<xt><head><link rel=\"symbols\" href=\"../../shared/symbols/ListItem.xt\"/></head>"
               "<body><use symbol=\"ListItem\" id=\"Row\"/></body></xt>";
    }
    std::string error;
    Result<bundle::UiProject> project = bundle::UiProject::load(root.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = (root / "out").string();
    options.standardSymbolDir = XT_SYMBOLS_DIR;
    const bundle::PackReport report = bundle::pack(project.value(), options);
    std::string diagnostics;
    for (const compile::Diagnostic& d : report.diagnostics) {
        diagnostics += d.file + ": " + d.message + "\n";
    }
    REQUIRE_MESSAGE(report.ok, diagnostics);
    CHECK(report.standardSymbolsCopied == std::vector<std::string>{"shared/symbols/ListItem.xt"});
    CHECK(readText(root / "shared" / "symbols" / "ListItem.xt") ==
          readText(fs::path(XT_SYMBOLS_DIR) / "list" / "ListItem.xt"));

    // `xt-standard:ListItem` resolves in list/ as well.
    const std::string doc =
        "<xt><head><link href=\"tokens.xts\"/><link rel=\"symbols\" href=\"xt-standard:ListItem\"/></head>"
        "<body><use symbol=\"ListItem\" id=\"Row\"/></body></xt>";
    const std::string tokens = readText(fs::path(XT_SYMBOLS_DIR) / "tokens.xts");  // SourceFile is a view.
    std::vector<compile::SourceFile> files{{"doc.xt", doc}, {"tokens.xts", tokens}};
    compile::CompileOptions compileOptions;
    compileOptions.rootDir = XT_SYMBOLS_DIR;
    compileOptions.standardSymbolDir = XT_SYMBOLS_DIR;
    const compile::CompileResult compiled =
        compile::compileDocumentFromStrings(files, "doc.xt", compileOptions);
    CHECK_MESSAGE(compiled.ok(), diagnosticsText(compiled));
}
