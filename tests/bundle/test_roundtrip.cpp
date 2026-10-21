#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <json.hpp>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/migrations.h"
#include "xt/bundle/project_edit.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xt.h"
#include "xt/flow/flow_graph.h"
#include "yaml_lite.h"

/// T-410 (docs/design/43-compatibility-and-versions.md §43.2, B5): cross-version round trips. Each
/// source format has one frozen fixture per historical version under tests/roundtrip/<format>/
/// (`v0` = the form before T-406, without a version key; `v1` = today's) plus `latest/`, the same
/// content as the latest writer writes it. Every version is read (migrated), written back and
/// compared with `latest` structurally (JSON / YAML parsed, the format version member removed);
/// where the writer is byte-stable the written bytes of every version must also be identical.
/// `.xt` is compared by its re-serialization to IR (the root's `version` never reaches it) and by
/// its DOM. XT_ROUNDTRIP_UPDATE=1 rewrites each `latest/` from the written `v1` (`.xt` excepted):
/// do that only when a format gets a new version, and review the diff.
/// The editor-level round trip (a whole old project opened and saved through the Editor Service)
/// is tests/editor_service/test_service_roundtrip.cpp.

using namespace xt;
using namespace xt::bundle;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

/// Every file is written by the writer of its version; the versions compared against `latest`.
constexpr const char* kOldVersionsT410[] = {"v0", "v1"};

fs::path fixtureDirT410(const std::string& format, const std::string& version) {
    return fs::path(XT_TEST_BUNDLE_DIR) / ".." / "roundtrip" / format / version;
}

bool updatingT410() {
    std::string update;
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "XT_ROUNDTRIP_UPDATE") == 0 && value != nullptr) {
        update = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv("XT_ROUNDTRIP_UPDATE")) {
        update = value;
    }
#endif
    return update == "1";
}

std::string readTextT410(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeTextT410(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// A fresh temp directory `xt_t410_<name>` holding a copy of fixture `format`/`version`.
fs::path workCopyT410(const std::string& format, const std::string& version) {
    const fs::path dir = xt::test::tempDir() / ("xt_t410_" + format + "_" + version);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::copy(fixtureDirT410(format, version), dir, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    return dir;
}

/// A minimal UI Project file in `dir` (the design / conventions writers need a named project).
UiProject projectInT410(const fs::path& dir) {
    writeTextT410(
        dir / "Roundtrip.xtui",
        R"({"version": 1, "name": "Roundtrip", "id": "roundtrip", "uis": [{"id": "hud", "doc": "ui/hud.xt"}]})");
    std::string error;
    Result<UiProject> project = UiProject::load(dir.string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return project.value();
}

/// The format version member of the file `rel`: `formatVersion` for design.yaml (whose `version`
/// is the design system's own, 35 §35.2), else `version`.
const char* versionKeyT410(const std::string& rel) {
    return rel.size() >= 11 && rel.compare(rel.size() - 11, 11, "design.yaml") == 0 ? "formatVersion"
                                                                                    : "version";
}

/// Parses a JSON text, checks its format version is the latest, and drops it.
json normalizeJsonT410(const std::string& rel, const std::string& text, int latest) {
    json doc = json::parse(text, nullptr, false);
    REQUIRE_MESSAGE(doc.is_object(), rel);
    CHECK_MESSAGE(doc.value("version", 0) == latest, rel);
    doc.erase("version");
    return doc;
}

/// Parses a YAML text (yaml_lite), checks its format version is the latest, and drops it.
json normalizeYamlT410(const std::string& rel, const std::string& text, int latest) {
    json doc;
    std::string error;
    REQUIRE_MESSAGE(bundle::detail::parseYamlLite(text, doc, &error), (rel + ": " + error));
    REQUIRE_MESSAGE(doc.is_object(), rel);
    const char* key = versionKeyT410(rel);
    CHECK_MESSAGE(doc.value(key, 0) == latest, rel);
    doc.erase(key);
    return doc;
}

using OutputsT410 = std::map<std::string, std::string>;  // fixture-relative path -> written text
using RoundTripT410 = std::function<OutputsT410(const std::string& version)>;
using NormalizeT410 = std::function<json(const std::string& rel, const std::string& text)>;

/// Reads, migrates and writes every version of `format`; each written file must equal its
/// `latest` twin structurally, and (`byteStable`) every version must write the same bytes.
void checkRoundTripT410(const std::string& format, const RoundTripT410& roundTrip,
                        const NormalizeT410& normalize, bool byteStable) {
    const OutputsT410 fromV1 = roundTrip("v1");
    if (updatingT410()) {
        for (const auto& [rel, text] : fromV1) {
            writeTextT410(fixtureDirT410(format, "latest") / rel, text);
        }
    }
    const OutputsT410 fromLatest = roundTrip("latest");
    for (const auto& [rel, text] : fromLatest) {
        INFO(format << "/latest/" << rel);
        const std::string frozen = readTextT410(fixtureDirT410(format, "latest") / rel);
        REQUIRE_FALSE(frozen.empty());
        CHECK(normalize(rel, text) == normalize(rel, frozen));
    }
    for (const char* version : kOldVersionsT410) {
        const OutputsT410 out = std::string(version) == "v1" ? fromV1 : roundTrip(version);
        CHECK(out.size() == fromLatest.size());
        for (const auto& [rel, text] : out) {
            INFO(format << "/" << version << "/" << rel);
            const std::string frozen = readTextT410(fixtureDirT410(format, "latest") / rel);
            REQUIRE_FALSE(frozen.empty());
            CHECK(normalize(rel, text) == normalize(rel, frozen));
            if (byteStable) {
                REQUIRE(fromLatest.count(rel) == 1);
                CHECK(text == fromLatest.at(rel));
            }
        }
    }
}

/// The DOM trees are equal apart from the root's `version` attribute.
bool sameDomT410(const compile::DomTree& a, const compile::DomTree& b) {
    if (a.nodes.size() != b.nodes.size()) {
        return false;
    }
    auto attrs = [](const compile::DomNode& node, bool root) {
        std::vector<std::pair<std::string, std::string>> out;
        for (const compile::DomAttr& attr : node.attrs) {
            if (!(root && attr.name == "version")) {
                out.emplace_back(attr.name, attr.value);
            }
        }
        return out;
    };
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const compile::DomNode& x = a.nodes[i];
        const compile::DomNode& y = b.nodes[i];
        if (x.tag != y.tag || x.text != y.text || x.hasText != y.hasText || x.children != y.children ||
            x.parent != y.parent || attrs(x, i == 0) != attrs(y, i == 0)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_SUITE("roundtrip") {
    TEST_CASE("T-410 round trip: .xtui (unknown keys kept at every level)") {
        checkRoundTripT410(
            "xtui",
            [](const std::string& version) {
                const fs::path dir = workCopyT410("xtui", version);
                std::string error;
                Result<UiProject> project = UiProject::load(dir.string(), &error);
                REQUIRE_MESSAGE(project.ok(), error);
                CHECK(project.value().version == kUiProjectFormatVersion);
                REQUIRE_MESSAGE(project.value().save(&error) == Status::kOk, error);
                const std::string text = readTextT410(dir / "Roundtrip.xtui");
                const json saved = json::parse(text, nullptr, false);
                CHECK(saved["futureTop"] == json{{"keep", true}});
                CHECK(saved["spec"]["futureSpec"] == json{{"a", 1}});
                CHECK(saved["spec"]["behavior"] == 1);
                CHECK(saved["shared"]["futureShared"] == "s");
                CHECK(saved["preview"]["futurePreview"] == json::array({1, 2}));
                CHECK(saved["publish"]["futurePublish"] == true);
                return OutputsT410{{"Roundtrip.xtui", text}};
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeJsonT410(rel, text, kUiProjectFormatVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: .xtflow") {
        checkRoundTripT410(
            "xtflow",
            [](const std::string& version) {
                const std::string text = readTextT410(fixtureDirT410("xtflow", version) / "hud.xtflow");
                flow::FlowGraph graph;
                std::vector<std::string> errors;
                REQUIRE(flow::FlowGraph::parse(text, graph, errors) == Status::kOk);
                CHECK(errors.empty());
                return OutputsT410{{"hud.xtflow", graph.toJson()}};
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeJsonT410(rel, text, kFlowFormatVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: asset .meta (unknown keys kept)") {
        checkRoundTripT410(
            "meta",
            [](const std::string& version) {
                const std::string text = readTextT410(fixtureDirT410("meta", version) / "icon.png.meta");
                assets::AssetMeta meta;
                std::string error;
                REQUIRE_MESSAGE(assets::parseMeta(text, meta, &error), error);
                const std::string out = assets::metaText(meta);
                CHECK(out.find("futureKey: 7\n") != std::string::npos);
                return OutputsT410{{"icon.png.meta", out}};
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeYamlT410(rel, text, kMetaFormatVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: manifest/assets.yaml") {
        checkRoundTripT410(
            "assets-manifest",
            [](const std::string& version) {
                const std::string text =
                    readTextT410(fixtureDirT410("assets-manifest", version) / "assets.yaml");
                AssetsManifest manifest;
                std::string error;
                REQUIRE_MESSAGE(AssetsManifest::parse(text, manifest, &error), error);
                CHECK_FALSE(manifest.hasUnsavableContent);
                return OutputsT410{{"assets.yaml", manifest.toYaml()}};
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeYamlT410(rel, text, kAssetsManifestVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: design/*.yaml and reference .meta (design.yaml formatVersion)") {
        checkRoundTripT410(
            "design",
            [](const std::string& version) {
                const fs::path dir = workCopyT410("design", version);
                const UiProject project = projectInT410(dir);
                design::DesignSystem ds;
                std::string error;
                REQUIRE_MESSAGE(design::load(project, ds, nullptr, &error) == Status::kOk, error);
                // design.yaml's own `version` is the design system's, kept as an ordinary key.
                CHECK(ds.info.extra["version"] == "0.3");
                CHECK_FALSE(ds.info.extra.contains("formatVersion"));
                REQUIRE_MESSAGE(design::save(ds, &error) == Status::kOk, error);
                OutputsT410 out;
                for (const char* rel : {"design/design.yaml", "design/palette.yaml", "design/tokens.yaml",
                                        "design/type.yaml", "design/references/moods/a.png.meta"}) {
                    out[rel] = readTextT410(dir / rel);
                }
                CHECK(out["design/design.yaml"].rfind("formatVersion: 1\nname: Roundtrip\n", 0) == 0);
                CHECK(out["design/design.yaml"].find("version: \"0.3\"") != std::string::npos);
                return out;
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeYamlT410(rel, text, kDesignYamlFormatVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: contracts/ and conventions/ YAML") {
        checkRoundTripT410(
            "conventions",
            [](const std::string& version) {
                const fs::path dir = workCopyT410("conventions", version);
                const UiProject project = projectInT410(dir);
                std::string error;
                // The conventions tab's motion entry edit (same value) and its file write-back.
                REQUIRE_MESSAGE(
                    putMotionSpecEntry(project, "text", json{{"min_px", 12}}, &error) == Status::kOk, error);
                json naming;
                REQUIRE(bundle::detail::parseYamlLite(readTextT410(dir / "conventions/naming.yaml"), naming,
                                                      &error));
                REQUIRE(migrateToLatest(SourceFormat::DesignYaml, naming, nullptr, &error) == Status::kOk);
                REQUIRE_MESSAGE(
                    writeConventionYaml(project, "conventions/naming.yaml", naming, &error) == Status::kOk,
                    error);
                OutputsT410 out;
                for (const char* rel : {"contracts/motion-spec.yaml", "conventions/naming.yaml"}) {
                    out[rel] = readTextT410(dir / rel);
                }
                CHECK(out["contracts/motion-spec.yaml"].rfind("# Motion spec\nversion: 1\n", 0) == 0);
                return out;
            },
            [](const std::string& rel, const std::string& text) {
                return normalizeYamlT410(rel, text, kDesignYamlFormatVersion);
            },
            true);
    }

    TEST_CASE("T-410 round trip: .xt (same IR and DOM with and without <xt version>)") {
        auto compileT410 = [](const std::string& version, compile::DomTree& dom) {
            const std::string text = readTextT410(fixtureDirT410("xt", version) / "hud.xt");
            REQUIRE_FALSE(text.empty());
            compile::DiagnosticSink sink;
            REQUIRE(compile::parseXt(text, "hud.xt", sink, dom));
            const compile::SourceFile files[] = {{"hud.xt", text}};
            compile::CompileOptions options;
            compile::CompileResult result = compile::compileDocumentFromStrings(files, "hud.xt", options);
            REQUIRE(result.ok());
            return result.ir;
        };
        compile::DomTree latestDom;
        const std::vector<uint8_t> latestIr = compileT410("latest", latestDom);
        for (const char* version : kOldVersionsT410) {
            INFO("xt/" << version);
            compile::DomTree dom;
            CHECK(compileT410(version, dom) == latestIr);
            CHECK(sameDomT410(dom, latestDom));
        }
        CHECK(readTextT410(fixtureDirT410("xt", "latest") / "hud.xt") ==
              readTextT410(fixtureDirT410("xt", "v1") / "hud.xt"));
    }
}
