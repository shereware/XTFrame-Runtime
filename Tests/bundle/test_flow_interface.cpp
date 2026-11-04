#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/gen_ports.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/text_shaper.h"

/// T-580 (Docs/design/22-bundle.md §22.15, 53-flow-language.md §53.101): the manifest's flow
/// interface -- `uis[].flowInterface`, the top-level `flowInterface`, `ports.out[].origin` -- as
/// pack() writes it and Bundle reads it back, the contract hash over it, and the `Flow` block of
/// `--gen-ports`.

using namespace xt;
using namespace xt::bundle;
using json = nlohmann::json;

namespace {

namespace fs = std::filesystem;

std::string readTextT580(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeTextT580(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

fs::path freshT580(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t580_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

fs::path copyT580(const fs::path& from, const std::string& name) {
    const fs::path dir = freshT580(name);
    std::error_code ec;
    fs::copy(from, dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return dir;
}

std::unique_ptr<Document> documentT580(const std::string& text) {
    const compile::SourceFile file{"hud.xt", text};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings({&file, 1}, "hud.xt", options);
    REQUIRE(result.ok());
    static NullTextShaper shaper;
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    return doc;
}

const json* uiEntry(const json& manifest, const std::string& id) {
    for (const json& ui : manifest["uis"]) {
        if (ui["id"] == id) {
            return &ui;
        }
    }
    return nullptr;
}

const PortInfo* outPort(const UiInfo& ui, const std::string& name) {
    for (const PortInfo& port : ui.ports) {
        if (!port.isInput && port.name == name) {
            return &port;
        }
    }
    return nullptr;
}

constexpr const char* kHudDoc = R"X(<xt><body port:hp="number in" port:score="number out">
  <div id="Card" port:power="number in" port:done="number out"></div>
  <button id="pick" on:click="emit(opened); emit(hint)">P</button>
  <button id="open">O</button><span id="label">L</span>
</body></xt>)X";

constexpr const char* kHudFlow = R"({"version": 1,
  "nodes": [
    {"id": "c", "type": "constant", "params": {"value": 3}},
    {"id": "e1", "type": "emit", "params": {"port": "clue_pick"}},
    {"id": "e2", "type": "emit", "params": {"port": "opened"}},
    {"id": "e3", "type": "emit", "params": {"port": "echo"}},
    {"id": "e4", "type": "emit", "params": {"port": "score"}},
    {"id": "sys", "type": "system"},
    {"id": "cnt", "type": "counter"},
    {"id": "ts", "type": "toString"}
  ],
  "edges": [
    {"from": "#pick.Click", "to": "e1.trigger"},
    {"from": "c.out", "to": "e1.value"},
    {"from": "#open.Click", "to": "e2.trigger"},
    {"from": "#open.Click", "to": "e3.trigger"},
    {"from": "#document.score", "to": "e3.value"},
    {"from": "#open.Click", "to": "e4.trigger"},
    {"from": "sys.back", "to": "cnt.increment"},
    {"from": "sys.paused", "to": "cnt.reset"},
    {"from": "cnt.value", "to": "#document.hp"},
    {"from": "cnt.value", "to": "#Card.power"},
    {"from": "#Card.done", "to": "ts.in"},
    {"from": "ts.out", "to": "#label.setText"}
  ]})";

constexpr const char* kPauseDoc =
    R"X(<xt><body port:active="number in" port:back_out="event out"><button id="b">B</button></body></xt>)X";

constexpr const char* kGlobalFlow = R"({"version": 1,
  "nodes": [
    {"id": "r", "type": "radio", "params": {"count": 2}},
    {"id": "e", "type": "emit", "params": {"port": "screen"}},
    {"id": "sys", "type": "system"}
  ],
  "edges": [
    {"from": "ui#pause.back_out", "to": "r.set1"},
    {"from": "sys.loading", "to": "r.set0"},
    {"from": "r.index", "to": "ui#pause.active"},
    {"from": "ui#pause.b.Click", "to": "e.trigger"},
    {"from": "r.index", "to": "e.value"},
    {"from": "ui#hud.score", "to": "r.set0"}
  ]})";

/// The sample project with kHudDoc / kHudFlow / kPauseDoc / kGlobalFlow.
fs::path flowSample(const std::string& name) {
    const fs::path src = copyT580(fs::path(XT_TEST_BUNDLE_DIR) / "fixtures" / "sample", name);
    writeTextT580(src / "hud" / "hud.xt", kHudDoc);
    writeTextT580(src / "hud" / "hud.xtflow", kHudFlow);
    writeTextT580(src / "menus" / "pause.xt", kPauseDoc);
    writeTextT580(src / "bundle.xtflow", kGlobalFlow);
    return src;
}

}  // namespace

TEST_CASE("T-580: game-ui demo -- hud's flowInterface field by field, every ports.out origin") {
    const fs::path src =
        copyT580(fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Examples" / "demos" / "game-ui", "game_src");
    std::string error;
    Result<UiProject> project = UiProject::load(src.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    PackOptions options;
    options.outDir = freshT580("game_out").generic_string();
    options.thumbnails = false;
    const PackReport report = pack(project.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(report.ok);
    CHECK(report.infos.empty());

    const json manifest = json::parse(readTextT580(fs::path(options.outDir) / "manifest.json"));
    CHECK(manifest["version"] == 1);
    CHECK(manifest.find("flowInterface") == manifest.end());  // No global flow.
    const json* hud = uiEntry(manifest, "hud");
    REQUIRE(hud != nullptr);
    CHECK((*hud)["flow"] == "ui/hud.xtflow");  // The path is untouched.
    REQUIRE(hud->contains("flowInterface"));
    const json& face = (*hud)["flowInterface"];
    // hud.xtflow wires element ports only: no emit, no declared port, no system node.
    CHECK(face.size() == 4);
    CHECK(face["emits"] == json::array());
    CHECK(face["writes"] == json::array());
    CHECK(face["reads"] == json::array());
    CHECK(face["system"] == json::array());
    const std::string text = readTextT580(fs::path(options.outDir) / "manifest.json");
    CHECK(
        text.find(
            "\"flowInterface\": {\n        \"emits\": [],\n        \"writes\": [],\n        \"reads\": [],\n"
            "        \"system\": []\n      },") != std::string::npos);
    CHECK(text.find("},\n      \"flowInterface\"") != std::string::npos);  // Right after `ports`.
    for (const char* id : {"hud", "pause"}) {
        const json* ui = uiEntry(manifest, id);
        REQUIRE(ui != nullptr);
        for (const json& port : (*ui)["ports"]["out"]) {
            CAPTURE(port.dump());
            CHECK(port["origin"] == "document");  // game-ui has no undeclared trigger / flow emits.
        }
        for (const json& port : (*ui)["ports"]["in"]) {
            CHECK(port.find("origin") == port.end());
        }
    }
    const json* pause = uiEntry(manifest, "pause");
    REQUIRE(pause != nullptr);
    CHECK(pause->find("flowInterface") == pause->end());  // No flow.

    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(options.outDir);
    REQUIRE(bundle.ok());
    const UiInfo* hudInfo = bundle.value()->ui("hud");
    REQUIRE(hudInfo != nullptr);
    CHECK(hudInfo->flowInterface.present);
    CHECK(hudInfo->flowInterface.empty());
    CHECK_FALSE(bundle.value()->ui("pause")->flowInterface.present);
    CHECK_FALSE(bundle.value()->globalFlowInterface().present);
    // An empty interface leaves the contract as it was before T-580.
    REQUIRE(report.portTables.size() == 3);
    CHECK(report.portTables[0].uiId == "hud");
    CHECK(report.portTables[0].flow.present);
    CHECK(report.portTables[0].contract == hudInfo->contract);
}

TEST_CASE("T-580: pack reflects emits, writes, reads and system of the UI flows and the global flow") {
    const fs::path src = flowSample("full_src");
    std::string error;
    Result<ProjectFile> project = ProjectFile::load(src.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    PackOptions options;
    options.outDir = freshT580("full_out").generic_string();
    options.thumbnails = false;
    const PackReport report = pack(project.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(report.ok);
    // `opened` is a trigger emit and a flow emit: the trigger came first.
    REQUIRE(report.infos.size() == 1);
    CHECK(report.infos[0] ==
          "hud: port 'document.opened' (flow emit) is already listed (origin trigger); the first is kept");

    const json manifest = json::parse(readTextT580(fs::path(options.outDir) / "manifest.json"));
    const json* hud = uiEntry(manifest, "hud");
    REQUIRE(hud != nullptr);
    const json expectedHud = json::parse(R"({
      "emits": [ {"name": "clue_pick", "type": "number"}, {"name": "opened", "type": "pulse"},
                 {"name": "echo", "type": "number"}, {"name": "score", "type": "pulse"} ],
      "writes": [ "#hud.hp", "#hud.Card.power" ],
      "reads": [ "#hud.score", "#hud.Card.done" ],
      "system": [ "paused", "back" ] })");
    CHECK((*hud)["flowInterface"] == expectedHud);
    const json* pause = uiEntry(manifest, "pause");
    REQUIRE(pause != nullptr);
    // pause has no flow of its own; the global flow's `ui#pause` addresses land here.
    CHECK((*pause)["flowInterface"] == json::parse(R"({"emits": [], "writes": ["#pause.active"],
                                                       "reads": ["#pause.back_out"], "system": []})"));
    CHECK(manifest["flowInterface"] == json::parse(R"({"emits": [{"name": "screen", "type": "number"}],
                                                       "writes": [], "reads": [], "system": ["loading"]})"));
    CHECK(manifest["flows"]["global"] == "bundle.xtflow");

    // ports.out: declared ports and element events "document", then the undeclared emits.
    const auto originOf = [&](const char* name) -> std::string {
        for (const json& port : (*hud)["ports"]["out"]) {
            if (port["name"] == name) {
                return port["origin"].get<std::string>() + " " + port["type"].get<std::string>();
            }
        }
        return "missing";
    };
    CHECK(originOf("#pick.Click") == "document pulse");
    CHECK(originOf("document.opened") == "trigger pulse");
    CHECK(originOf("document.hint") == "trigger pulse");
    CHECK(originOf("document.clue_pick") == "flow number");
    CHECK(originOf("document.echo") == "flow number");
    CHECK(originOf("document.score") == "document number");  // Declared on <body> (no id).
    size_t scoreRows = 0;
    for (const json& port : (*hud)["ports"]["out"]) {
        scoreRows +=
            port["name"].get<std::string>().find(".score") != std::string::npos ? size_t{1} : size_t{0};
    }
    CHECK(scoreRows == 1);  // The flow emit of the declared `score` adds no row.

    // Bundle reads the same.
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(options.outDir);
    REQUIRE(bundle.ok());
    const UiInfo* hudInfo = bundle.value()->ui("hud");
    REQUIRE(hudInfo != nullptr);
    REQUIRE(hudInfo->flowInterface.present);
    REQUIRE(hudInfo->flowInterface.emits.size() == 4);
    CHECK(hudInfo->flowInterface.emits[0].name == "clue_pick");
    CHECK(hudInfo->flowInterface.emits[0].type == "number");
    CHECK(hudInfo->flowInterface.writes == std::vector<std::string>{"#hud.hp", "#hud.Card.power"});
    CHECK(hudInfo->flowInterface.reads == std::vector<std::string>{"#hud.score", "#hud.Card.done"});
    CHECK(hudInfo->flowInterface.system == std::vector<std::string>{"paused", "back"});
    REQUIRE(outPort(*hudInfo, "document.clue_pick") != nullptr);
    CHECK(outPort(*hudInfo, "document.clue_pick")->origin == "flow");
    CHECK(outPort(*hudInfo, "document.hint")->origin == "trigger");
    CHECK(outPort(*hudInfo, "#pick.Click")->origin == "document");
    const FlowInterface& global = bundle.value()->globalFlowInterface();
    CHECK(global.present);
    REQUIRE(global.emits.size() == 1);
    CHECK(global.emits[0].name == "screen");
    CHECK(global.system == std::vector<std::string>{"loading"});

    // The contract covers the interface: the documents alone hash differently.
    const std::unique_ptr<Document> hudDoc = documentT580(kHudDoc);
    CHECK(hudInfo->contract != contractHash(*hudDoc));
    CHECK(hudInfo->contract ==
          contractHashOfJson(contractJsonWithFlow(contractJson(*hudDoc), hudInfo->flowInterface)));
    const std::unique_ptr<Document> pauseDoc = documentT580(kPauseDoc);
    const UiInfo* pauseInfo = bundle.value()->ui("pause");
    REQUIRE(pauseInfo != nullptr);
    CHECK(pauseInfo->contract != contractHash(*pauseDoc));  // Changed by the global flow's write.
    CHECK(pauseInfo->contract ==
          contractHashOfJson(contractJsonWithFlow(contractJson(*pauseDoc), pauseInfo->flowInterface)));

    // --gen-ports: the tables carry the final contract and the interface.
    REQUIRE(report.portTables.size() == 2);
    CHECK(report.portTables[0].contract == hudInfo->contract);
    CHECK(report.portTables[1].contract == pauseInfo->contract);
    const std::string cpp = generatePortHeader(PortsLanguage::Cpp, report.portTables);
    CHECK(cpp.find("namespace Flow {\n\n"
                   "inline constexpr char clue_pick[] = \"clue_pick\";  // emit, number\n"
                   "inline constexpr char opened[] = \"opened\";  // emit, pulse\n"
                   "inline constexpr char echo[] = \"echo\";  // emit, number\n"
                   "inline constexpr char score[] = \"score\";  // emit, pulse\n"
                   "inline constexpr std::array<const char*, 2> kSystemEvents = {\"paused\", \"back\"};\n\n"
                   "}  // namespace Flow\n") != std::string::npos);
    CHECK(cpp.find("inline constexpr std::array<const char*, 0> kSystemEvents = {};") != std::string::npos);
    const std::string cs = generatePortHeader(PortsLanguage::CSharp, report.portTables);
    CHECK(cs.find("    public static class Flow\n    {\n"
                  "        /// <summary>emit, number</summary>\n"
                  "        public const string CluePick = \"clue_pick\";\n") != std::string::npos);
    CHECK(cs.find("        public static readonly string[] SystemEvents = { \"paused\", \"back\" };\n") !=
          std::string::npos);
    CHECK(cs.find("        public static readonly string[] SystemEvents = {};\n") != std::string::npos);

    // An incremental run keeps the interface and the contract of the UI it does not repack.
    options.onlyUis = {"pause"};
    REQUIRE(pack(project.value(), options).ok);
    Result<std::unique_ptr<Bundle>> kept = Bundle::open(options.outDir);
    REQUIRE(kept.ok());
    CHECK(kept.value()->ui("hud")->flowInterface.emits.size() == 4);
    CHECK(kept.value()->ui("hud")->contract == hudInfo->contract);
    CHECK(outPort(*kept.value()->ui("hud"), "document.clue_pick")->origin == "flow");
    CHECK(kept.value()->ui("pause")->contract == pauseInfo->contract);
}

TEST_CASE("T-580: a manifest without flowInterface or origin still loads -- empty and \"document\"") {
    Result<std::unique_ptr<Bundle>> bundle =
        Bundle::open(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/published");
    REQUIRE(bundle.ok());
    REQUIRE(!bundle.value()->uis().empty());
    size_t outputs = 0;
    for (const UiInfo& ui : bundle.value()->uis()) {
        CHECK_FALSE(ui.flowInterface.present);
        CHECK(ui.flowInterface.empty());
        for (const PortInfo& port : ui.ports) {
            CHECK(port.origin == "document");
            outputs += port.isInput ? 0 : 1;
        }
    }
    CHECK(outputs > 0);
    CHECK_FALSE(bundle.value()->globalFlowInterface().present);
}

TEST_CASE("T-580: gen-ports Flow block; a table without a flow is unchanged") {
    UiPortTable table;
    table.uiId = "hud";
    const std::string before = generatePortHeader(PortsLanguage::Cpp, {&table, 1});
    CHECK(before.find("Flow") == std::string::npos);
    table.flow.present = true;
    table.flow.emits = {FlowEmit{"clue_pick", "number"}, FlowEmit{"class", "pulse"},
                        FlowEmit{"kSystemEvents", "pulse"}};
    table.flow.system = {"paused", "back"};
    const std::string cpp = generatePortHeader(PortsLanguage::Cpp, {&table, 1});
    CHECK(cpp ==
          "// Generated by `xtframe-pack --gen-ports cpp` from the bundle's declared ports. Do not edit.\n"
          "// The PortId values belong to this build's .xtir files; Document::lookupPort() resolves the\n"
          "// same names at run time.\n"
          "#pragma once\n\n#include <array>\n#include <cstdint>\n#include <string>\n\n"
          "namespace xtui {\n\nusing PortId = std::uint32_t;\n\nnamespace hud {\n\n"
          "// The UI's flow interface (manifest uis[].flowInterface, T-580).\nnamespace Flow {\n\n"
          "inline constexpr char clue_pick[] = \"clue_pick\";  // emit, number\n"
          "inline constexpr char class_[] = \"class\";  // emit, pulse\n"
          "inline constexpr char kSystemEvents_2[] = \"kSystemEvents\";  // emit, pulse\n"
          "inline constexpr std::array<const char*, 2> kSystemEvents = {\"paused\", \"back\"};\n\n"
          "}  // namespace Flow\n\n}  // namespace hud\n\n}  // namespace xtui\n");
    const std::string cs = generatePortHeader(PortsLanguage::CSharp, {&table, 1});
    CHECK(cs ==
          "// <auto-generated>\n"
          "// Generated by `xtframe-pack --gen-ports cs` from the bundle's declared ports. Do not edit.\n"
          "// The port ids belong to this build's .xtir files; Document::lookupPort() resolves the same\n"
          "// names at run time.\n"
          "// </auto-generated>\n\n"
          "namespace Xtui.Hud\n{\n    public static class Ports\n    {\n    }\n\n"
          "    public static class Flow\n    {\n"
          "        /// <summary>emit, number</summary>\n"
          "        public const string CluePick = \"clue_pick\";\n"
          "        /// <summary>emit, pulse</summary>\n"
          "        public const string Class = \"class\";\n"
          "        /// <summary>emit, pulse</summary>\n"
          "        public const string KSystemEvents = \"kSystemEvents\";\n"
          "        /// <summary>The system events the flow is wired to.</summary>\n"
          "        public static readonly string[] SystemEvents = { \"paused\", \"back\" };\n"
          "    }\n}\n");
}
