#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/symbol_json.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/document.h"
#include "xt/ir.h"

/// T-741 (Docs/design/56-symbol-identity-style-scope.md 56.3 items 1, 6, 7): a library symbol whose
/// name the project already uses for another symbol is copied into `shared/symbols/<group id>/`
/// beside it, the symbols of its group it uses travel with it, the project index is keyed by the
/// qualified name, and the packer names namespaced symbols `<group id>/<Name>`.

using namespace xt;
using namespace xt::bundle;
namespace fs = std::filesystem;

namespace {

const fs::path kPresetDirT741 = fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols";

void writeT741(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readT741(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

fs::path freshT741(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

UiProject projectT741(const std::string& name) {
    const fs::path dir = freshT741(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(
        createUiProject(compile::genericPathToUtf8(dir), "T741", "t741", "", &project, &error) == Status::kOk,
        error);
    return project;
}

/// A button of `width` px (so a compiled instance says whose it is).
std::string buttonT741(int width) {
    return "<xt><head><symbol name=\"Button\"><div id=\"Bg\" style=\"width: " + std::to_string(width) +
           "px; height: 10px\"></div></symbol></head></xt>\n";
}

/// One group `id` of the library at `dir`: `symbols` are {name, file text}.
void groupT741(const fs::path& dir, const std::string& id,
               const std::vector<std::pair<std::string, std::string>>& symbols) {
    const fs::path g = dir / "groups" / id;
    writeT741(g / "group.json", "{ \"version\": 1, \"id\": \"" + id + "\", \"name\": \"Group " + id +
                                    "\", \"tokens\": \"tokens.xts\", \"source\": { \"kind\": \"manual\", "
                                    "\"set\": \"me/" +
                                    id + "\", \"setVersion\": \"1.0.0\" } }");
    std::string index = "{ \"version\": 1, \"tokens\": \"tokens.xts\", \"symbols\": [";
    for (size_t i = 0; i < symbols.size(); ++i) {
        index += std::string(i == 0 ? "" : ",") + " { \"name\": \"" + symbols[i].first + "\", \"file\": \"" +
                 symbols[i].first + ".xt\", \"tokens\": [], \"fonts\": [], \"textures\": [] }";
        writeT741(g / (symbols[i].first + ".xt"), symbols[i].second);
    }
    writeT741(g / "index.json", index + " ] }");
    writeT741(g / "tokens.xts", ":root { --x: 1px; }\n");
}

/// The library `lib` with the groups broadsheet-2026 (Button 10 px, Dialog) and survey-2026
/// (Button 20 px, a Dialog that uses Button, Tag), mounted.
struct TwoGroupsT741 {
    fs::path dir;
    LibraryMounts mounts;
    explicit TwoGroupsT741(const std::string& name) {
        dir = freshT741(name);
        writeT741(dir / "library.json", R"({ "version": 1, "id": "lib", "name": "Lib", "readOnly": false,
                                              "groups": ["broadsheet-2026", "survey-2026"] })");
        groupT741(
            dir, "broadsheet-2026",
            {{"Button", buttonT741(10)},
             {"Dialog", "<xt><head><symbol name=\"Dialog\"><div id=\"Box\"></div></symbol></head></xt>\n"}});
        groupT741(dir, "survey-2026",
                  {{"Button", buttonT741(20)},
                   {"Dialog",
                    "<xt><head><symbol name=\"Dialog\"><div id=\"Box\"><use symbol=\"Button\" "
                    "id=\"Ok\"></use></div></symbol></head></xt>\n"},
                   {"Tag", "<xt><head><symbol name=\"Tag\"><div id=\"T\"></div></symbol></head></xt>\n"}});
        REQUIRE(mount(mounts, dir) == Status::kOk);
    }
    fs::path file(const std::string& group, const std::string& name) const {
        return dir / "groups" / group / (name + ".xt");
    }
};

UseReport useT741(const LibraryMounts& mounts, const UiProject& project, std::string_view name,
                  std::string_view group) {
    const std::string_view names[] = {name};
    const UseReport report = useSymbols(mounts, project, names, group);
    REQUIRE(report.status == Status::kOk);
    return report;
}

bool hasT741(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

/// The width of the element `id` once `main` (with `files`) is compiled, loaded and laid out.
float widthT741(const std::vector<std::pair<std::string, std::string>>& files, const std::string& id) {
    std::vector<compile::SourceFile> sources;
    for (const auto& [name, text] : files) {
        sources.push_back(compile::SourceFile{name, text});
    }
    compile::CompileOptions options;
    options.emitMeta = false;
    options.rootDir = "t741-no-such-dir";
    const compile::CompileResult r = compile::compileDocumentFromStrings(sources, "main.xt", options);
    test::requireCompiled(r);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, true, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 100});
    doc->update(0.0f);
    return doc->byId(id).layoutBox().width;
}

std::string mainT741(const std::vector<std::string>& hrefs, const std::string& body) {
    std::string out = "<xt><head>";
    for (const std::string& href : hrefs) {
        out += "<link rel=\"symbols\" href=\"" + href + "\"/>";
    }
    return out + "</head><body id=\"Root\">" + body + "</body></xt>";
}

}  // namespace

TEST_CASE("T-741 broadsheet Button, then survey Dialog: Dialog and its Button go to survey-2026/ unchanged") {
    TwoGroupsT741 lib("xt_t741_order1_lib");
    const UiProject project = projectT741("xt_t741_order1");
    const fs::path symbols = compile::pathFromUtf8(project.rootDir) / "shared" / "symbols";
    useT741(lib.mounts, project, "Button", "broadsheet-2026");
    CHECK(readT741(symbols / "Button.xt") == buttonT741(10));

    // Planned first: Dialog's own name is free, but it uses survey's Button, whose name is taken.
    const UseTarget plan = planUse(lib.mounts, project, "Dialog", "survey-2026");
    CHECK(plan.clash == UseClash::Placed);
    CHECK(plan.qualified == "survey-2026/Dialog");
    CHECK(plan.path == "shared/symbols/survey-2026/Dialog.xt");
    CHECK(plan.because == "survey-2026/Button");
    CHECK(plan.existingLibrary.empty());

    const UseReport report = useT741(lib.mounts, project, "Dialog", "survey-2026");
    CHECK(hasT741(report.written, "shared/symbols/survey-2026/Dialog.xt"));
    CHECK(hasT741(report.written, "shared/symbols/survey-2026/Button.xt"));
    CHECK_FALSE(fs::exists(symbols / "Dialog.xt"));
    // Byte for byte the library's files: the bare `Button` inside Dialog resolves in its namespace.
    CHECK(readT741(symbols / "survey-2026" / "Dialog.xt") == readT741(lib.file("survey-2026", "Dialog")));
    CHECK(readT741(symbols / "survey-2026" / "Button.xt") == buttonT741(20));
    CHECK(readT741(symbols / "Button.xt") == buttonT741(10));  // Untouched.

    REQUIRE(report.symbols.size() == 2);
    CHECK(report.symbols[0].name == "Dialog");
    CHECK(report.symbols[0].qualified == "survey-2026/Dialog");
    CHECK_FALSE(report.symbols[0].dependency);
    CHECK(report.symbols[1].name == "Button");
    CHECK(report.symbols[1].qualified == "survey-2026/Button");
    CHECK(report.symbols[1].dependency);
    CHECK(report.symbols[1].group == "survey-2026");
    REQUIRE(report.clashes.size() == 2);  // Both are told about (56.3 item 1, the notice).
    CHECK(report.clashes[0].name == "Dialog");
    CHECK(report.clashes[0].reason == UseClash::Placed);
    CHECK(report.clashes[0].because == "survey-2026/Button");
    CHECK(report.clashes[1].name == "Button");
    CHECK(report.clashes[1].reason == UseClash::Placed);
    CHECK(report.clashes[1].placed == "survey-2026/Button");
    CHECK(report.clashes[1].existingLibrary == "lib");
    CHECK(report.clashes[1].existingGroup == "broadsheet-2026");
    const SymbolJson json = useReportJson(report);
    CHECK(json["clashes"][1]["placed"] == "survey-2026/Button");
    CHECK(json["clashes"][1]["existingSource"] == "lib/broadsheet-2026");
    CHECK(json["clashes"][0]["because"] == "survey-2026/Button");
    CHECK(json["symbols"][0]["qualified"] == "survey-2026/Dialog");
    CHECK(json["symbols"][1]["dependency"] == true);

    // The index: keyed by the qualified name, `name` the file's symbol name.
    CopySource source;
    REQUIRE(readCopySource(project, "survey-2026/Dialog", source));
    CHECK(source.group == "survey-2026");
    REQUIRE(readCopySource(project, "Button", source));
    CHECK(source.group == "broadsheet-2026");
    REQUIRE(readCopySource(project, "survey-2026/Button", source));
    CHECK(source.group == "survey-2026");
    CHECK_FALSE(readCopySource(project, "Dialog", source));
    const std::string index = readT741(symbols / "index.json");
    CHECK(index.find("\"qualified\": \"survey-2026/Button\"") != std::string::npos);
    CHECK(index.find("\"file\": \"shared/symbols/survey-2026/Button.xt\"") != std::string::npos);
    CHECK(copyStatus(lib.mounts, project, "survey-2026/Button").state == CopyState::UpToDate);
    CHECK(copyStatus(lib.mounts, project, "Button").state == CopyState::UpToDate);

    // Compiled: `<use symbol="survey-2026/Dialog">`'s Button is survey's; the bare one broadsheet's.
    const std::string main =
        mainT741({"shared/symbols/Button.xt", "shared/symbols/survey-2026/Button.xt",
                  "shared/symbols/survey-2026/Dialog.xt"},
                 R"(<use symbol="survey-2026/Dialog" id="D"/><use symbol="Button" id="B"/>)");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"main.xt", main},
        {"shared/symbols/Button.xt", readT741(symbols / "Button.xt")},
        {"shared/symbols/survey-2026/Button.xt", readT741(symbols / "survey-2026" / "Button.xt")},
        {"shared/symbols/survey-2026/Dialog.xt", readT741(symbols / "survey-2026" / "Dialog.xt")}};
    CHECK(widthT741(files, "D/Ok/Bg") == doctest::Approx(20.0f));
    CHECK(widthT741(files, "B/Bg") == doctest::Approx(10.0f));

    // Again: the same symbols, nothing written, nothing to tell (the dependency is no clash).
    const UseReport again = useT741(lib.mounts, project, "Dialog", "survey-2026");
    CHECK(again.written.empty());
    REQUIRE(again.clashes.size() == 1);
    CHECK(again.clashes[0].reason == UseClash::Same);
    CHECK(again.clashes[0].placed == "survey-2026/Dialog");
}

TEST_CASE("T-741 survey Dialog first, then broadsheet Button: survey stays flat, broadsheet's is placed") {
    TwoGroupsT741 lib("xt_t741_order2_lib");
    const UiProject project = projectT741("xt_t741_order2");
    const fs::path symbols = compile::pathFromUtf8(project.rootDir) / "shared" / "symbols";
    const UseReport first = useT741(lib.mounts, project, "Dialog", "survey-2026");
    CHECK(first.clashes.empty());
    CHECK(hasT741(first.written, "shared/symbols/Dialog.xt"));
    CHECK(hasT741(first.written, "shared/symbols/Button.xt"));  // The dependency, flat too.
    CHECK(readT741(symbols / "Button.xt") == buttonT741(20));

    const UseReport second = useT741(lib.mounts, project, "Button", "broadsheet-2026");
    REQUIRE(second.clashes.size() == 1);
    CHECK(second.clashes[0].reason == UseClash::Placed);
    CHECK(second.clashes[0].placed == "broadsheet-2026/Button");
    CHECK(second.clashes[0].existingGroup == "survey-2026");
    CHECK(readT741(symbols / "broadsheet-2026" / "Button.xt") == buttonT741(10));
    CHECK(readT741(symbols / "Button.xt") == buttonT741(20));
    CHECK_FALSE(fs::exists(symbols / "broadsheet-2026" / "Dialog.xt"));

    // The flat Dialog keeps meaning survey's Dialog with survey's Button (56.3 item 3: a second
    // Button changes no existing reference).
    const std::string main = mainT741(
        {"shared/symbols/Button.xt", "shared/symbols/Dialog.xt", "shared/symbols/broadsheet-2026/Button.xt"},
        R"(<use symbol="Dialog" id="D"/><use symbol="broadsheet-2026/Button" id="B"/>)");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"main.xt", main},
        {"shared/symbols/Button.xt", readT741(symbols / "Button.xt")},
        {"shared/symbols/Dialog.xt", readT741(symbols / "Dialog.xt")},
        {"shared/symbols/broadsheet-2026/Button.xt", readT741(symbols / "broadsheet-2026" / "Button.xt")}};
    CHECK(widthT741(files, "D/Ok/Bg") == doctest::Approx(20.0f));
    CHECK(widthT741(files, "B/Bg") == doctest::Approx(10.0f));
}

TEST_CASE("T-741 a free survey name goes flat; a clashing one later goes to the namespace with its users") {
    TwoGroupsT741 lib("xt_t741_later_lib");
    const UiProject project = projectT741("xt_t741_later");
    const fs::path symbols = compile::pathFromUtf8(project.rootDir) / "shared" / "symbols";
    const UseReport tag = useT741(lib.mounts, project, "Tag", "survey-2026");
    CHECK(tag.clashes.empty());
    REQUIRE(tag.symbols.size() == 1);
    CHECK(tag.symbols[0].qualified == "Tag");
    CHECK(fs::exists(symbols / "Tag.xt"));
    useT741(lib.mounts, project, "Button", "broadsheet-2026");
    const UseReport button = useT741(lib.mounts, project, "Button", "survey-2026");
    REQUIRE(button.clashes.size() == 1);
    CHECK(button.clashes[0].placed == "survey-2026/Button");
    CHECK(fs::exists(symbols / "Tag.xt"));  // Nothing moves (56.3: no silent migration).
    CHECK_FALSE(fs::exists(symbols / "survey-2026" / "Tag.xt"));

    // Dialog: its Button is in the namespace already (not copied again, no clash), so Dialog goes too.
    const UseReport dialog = useT741(lib.mounts, project, "Dialog", "survey-2026");
    CHECK(hasT741(dialog.written, "shared/symbols/survey-2026/Dialog.xt"));
    CHECK_FALSE(hasT741(dialog.written, "shared/symbols/survey-2026/Button.xt"));
    REQUIRE(dialog.clashes.size() == 1);
    CHECK(dialog.clashes[0].name == "Dialog");
    CHECK(dialog.clashes[0].reason == UseClash::Placed);
    CHECK(dialog.clashes[0].because == "survey-2026/Button");
    REQUIRE(dialog.symbols.size() == 2);
    CHECK(dialog.symbols[1].qualified == "survey-2026/Button");
    CHECK(dialog.symbols[1].dependency);
    // Both requested in one call: one row each, the requested one not marked as a dependency.
    const std::string_view both[] = {"Dialog", "Button"};
    const UseReport twice = useSymbols(lib.mounts, project, both, "survey-2026");
    REQUIRE(twice.symbols.size() == 2);
    CHECK_FALSE(twice.symbols[1].dependency);
}

TEST_CASE("T-741 index.json old and new entries; updateCopy, snapshot and saveAsGroup of a namespaced copy") {
    TwoGroupsT741 lib("xt_t741_index_lib");
    const UiProject project = projectT741("xt_t741_index");
    const fs::path root = compile::pathFromUtf8(project.rootDir);
    const fs::path symbols = root / "shared" / "symbols";
    // An entry written before T-741 (no `qualified`) is the flat copy.
    writeT741(symbols / "Button.xt", readT741(lib.file("broadsheet-2026", "Button")));
    writeT741(symbols / "index.json",
              R"({ "version": 1, "symbols": [ { "name": "Button", "file": "shared/symbols/Button.xt",
                   "source": { "library": "lib", "group": "broadsheet-2026", "setVersion": "1.0.0" } } ] })");
    CHECK(planUse(lib.mounts, project, "Button", "broadsheet-2026").clash == UseClash::Same);
    useT741(lib.mounts, project, "Button", "survey-2026");
    CopySource source;
    REQUIRE(readCopySource(project, "Button", source));
    CHECK(source.group == "broadsheet-2026");  // The old entry kept and still found.
    REQUIRE(readCopySource(project, "survey-2026/Button", source));
    CHECK(source.group == "survey-2026");
    CHECK_FALSE(readCopySource(project, "survey-2026", source));
    CHECK_FALSE(readCopySource(project, "a/b/Button", source));

    // The survey library's Button changes: only the namespaced copy is stale; the update rewrites
    // that file in place and its own entry.
    writeT741(lib.file("survey-2026", "Button"), buttonT741(30));
    CHECK(copyStatus(lib.mounts, project, "survey-2026/Button").state == CopyState::Stale);
    CHECK(isCopyStale(lib.mounts, project, "survey-2026/Button"));
    CHECK(copyStatus(lib.mounts, project, "Button").state == CopyState::UpToDate);
    const std::string_view names[] = {"survey-2026/Button"};
    const std::vector<ProjectFileState> snapshot = snapshotForUse(project, names);
    CHECK(std::any_of(snapshot.begin(), snapshot.end(), [](const ProjectFileState& s) {
        return s.path == "shared/symbols/survey-2026/Button.xt" && s.existed;
    }));
    const UseReport updated = updateCopy(lib.mounts, project, "survey-2026/Button");
    REQUIRE(updated.status == Status::kOk);
    CHECK(updated.written ==
          std::vector<std::string>{"shared/symbols/survey-2026/Button.xt", "shared/symbols/index.json"});
    CHECK(readT741(symbols / "survey-2026" / "Button.xt") == buttonT741(30));
    CHECK(readT741(symbols / "Button.xt") == readT741(lib.file("broadsheet-2026", "Button")));
    CHECK(copyStatus(lib.mounts, project, "survey-2026/Button").state == CopyState::UpToDate);
    REQUIRE(readCopySource(project, "Button", source));
    CHECK(source.group == "broadsheet-2026");
    CHECK(changesOfUse(snapshot, project, updated).size() == 2);

    // Changed on both sides: refused without overwriteLocal, nothing written.
    writeT741(lib.file("survey-2026", "Button"), buttonT741(40));
    writeT741(symbols / "survey-2026" / "Button.xt", buttonT741(31));
    CHECK(copyStatus(lib.mounts, project, "survey-2026/Button").state == CopyState::StaleAndModified);
    const UseReport refused = updateCopy(lib.mounts, project, "survey-2026/Button");
    CHECK(refused.status == Status::kUnsupported);
    CHECK(readT741(symbols / "survey-2026" / "Button.xt") == buttonT741(31));

    // saveAsGroup: qualified names; two symbols of one name cannot share a group.
    const fs::path mine = freshT741("xt_t741_index_mine");
    writeT741(mine / "library.json",
              R"({ "version": 1, "id": "mine", "name": "Mine", "readOnly": false, "groups": [] })");
    std::string error;
    const std::string_view twoButtons[] = {"Button", "survey-2026/Button"};
    CHECK(saveAsGroup(project, twoButtons, mine, "two", "Two", &error, &lib.mounts) ==
          Status::kInvalidArgument);
    CHECK(error.find("two symbols named 'Button'") != std::string::npos);
    const std::string_view one[] = {"survey-2026/Button"};
    REQUIRE_MESSAGE(saveAsGroup(project, one, mine, "saved", "Saved", &error, &lib.mounts) == Status::kOk,
                    error);
    CHECK(fs::exists(mine / "groups" / "saved" / "Button.xt"));
}

TEST_CASE("T-741 pack: manifest symbols are qualified; the implied group ignores namespaced file names") {
    TwoGroupsT741 lib("xt_t741_pack_lib");
    const UiProject created = projectT741("xt_t741_pack");
    const fs::path root = compile::pathFromUtf8(created.rootDir);
    const fs::path preset = kPresetDirT741 / "groups" / "broadsheet-2026";
    const std::string presetDir = compile::genericPathToUtf8(preset);
    // survey's Button only, flat name free: placed flat... so give the project its own Button first.
    writeT741(root / "shared" / "symbols" / "Button.xt", buttonT741(5));
    useT741(lib.mounts, created, "Button", "survey-2026");
    REQUIRE(fs::exists(root / "shared" / "symbols" / "survey-2026" / "Button.xt"));
    writeT741(root / "main" / "main.xt", mainT741({"../shared/symbols/survey-2026/Button.xt"},
                                                  R"(<use symbol="survey-2026/Button" id="S"/>)"));
    std::string error;
    Result<UiProject> loaded = UiProject::load(created.rootDir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    const UiProject& project = loaded.value();

    // survey-2026/Button.xt has a name the standard group has too; its recorded source decides.
    SymbolGroup group;
    CHECK_FALSE(detectImpliedGroup(project.rootDir, project.uis, presetDir, group, project.shared.symbols));
    CHECK(linkedSymbolGroups(project.rootDir, project.uis[0].doc, presetDir, project.shared.symbols) ==
          std::vector<std::string>{"lib/survey-2026"});

    PackOptions options;
    options.thumbnails = false;
    options.outDir = compile::genericPathToUtf8(root / "out");
    options.standardSymbolDir = presetDir;
    PackReport report = pack(project, options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        INFO(d.file << ": " << d.message);
    }
    REQUIRE(report.ok);
    CHECK(report.impliedGroupDir.empty());
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    std::vector<std::string> names;
    for (const SymbolContract& c : bundle.value()->symbolContracts()) {
        names.push_back(c.name);
    }
    CHECK(names == std::vector<std::string>{"Button", "survey-2026/Button"});  // Flat first, then namespaces.

    // A namespaced copy recorded from the standard group does imply it; a UI mixing two groups
    // with a namespaced copy is warned about, naming them in link order.
    LibraryMounts withPreset;
    REQUIRE(mount(withPreset, kPresetDirT741) == Status::kOk);
    const UseReport presetButton = useT741(withPreset, project, "Button", "broadsheet-2026");
    REQUIRE(presetButton.symbols.size() == 1);
    CHECK(presetButton.symbols[0].qualified == "broadsheet-2026/Button");
    writeT741(
        root / "main" / "main.xt",
        mainT741(
            {"../shared/symbols/broadsheet-2026/Button.xt", "../shared/symbols/survey-2026/Button.xt"},
            R"(<use symbol="survey-2026/Button" id="S"/><use symbol="broadsheet-2026/Button" id="P"/>)"));
    CHECK(detectImpliedGroup(project.rootDir, project.uis, presetDir, group, project.shared.symbols));
    CHECK(linkedSymbolGroups(project.rootDir, project.uis[0].doc, presetDir, project.shared.symbols) ==
          std::vector<std::string>{"preset/broadsheet-2026", "lib/survey-2026"});
    report = pack(project, options);
    bool warned = false;
    for (const compile::Diagnostic& d : report.diagnostics) {
        warned =
            warned || (d.level == compile::Diagnostic::Level::Warning &&
                       d.message.find("symbols from 2 groups (preset/broadsheet-2026, lib/survey-2026") !=
                           std::string::npos);
    }
    CHECK(warned);
}
