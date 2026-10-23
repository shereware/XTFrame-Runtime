#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <json.hpp>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bind_emit.h"
#include "compiled_assets.h"
#include "doc_emit.h"
#include "file_bytes.h"
#include "json_bounded.h"
#include "repeat_emit.h"
#include "style_emit.h"
#include "svg_timeline.h"
#include "symbol_expand.h"
#include "trigger_emit.h"
#include "xt/asset_resolver.h"
#include "xt/charconv.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/compile/parse_svg.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"
#include "xt/version.h"

/// `compileDocument`: the full `.xt` document pipeline (Docs/design/12-compiler.md §12.2). See
/// Docs/tasks/T-23.md.
///
/// Every relative path this pipeline resolves (`<link href>`, `<anim src>`, `<img src>`,
/// `@font-face src`) is resolved against the `.xt` file's own directory, per T-23's spec text
/// ("相對 .xt 所在目錄" repeated at every step). `CompileOptions::rootDir` (used by compileLottie for
/// the Lottie JSON's own directory) is intentionally NOT used for this purpose here -- 12.1's
/// comment describes it generically ("解析相對路徑"), but T-23's pipeline section never mentions it;
/// this is the conservative reading (see report).

namespace xt::compile {

// Implemented in xts/xts_parser.cpp (T-172): one `@timeline` key body and a free-standing easing.
bool parseTimelineKeyDeclarations(std::string_view text, std::string_view fileName, int line,
                                  DiagnosticSink& sink, TimelineKey& key);
bool parseEasingText(std::string_view text, Easing& out, std::vector<EasingPoint>* easingPoints = nullptr);

namespace {

namespace fs = std::filesystem;

bool readFileText(const fs::path& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!detail::readFileBytes(path, bytes)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

fs::path resolveCompileDocument(const std::string& dir, const std::string& rel) {
    return dir.empty() ? pathFromUtf8(rel) : pathFromUtf8(dir) / pathFromUtf8(rel);
}

/// True when `src` ends in `.svg` (case-insensitively): the marker that routes an `<img>` through
/// the static vector importer instead of the image-asset path (12.2.1, T-110).
bool hasSvgExtension(std::string_view src) {
    if (src.size() < 4) {
        return false;
    }
    std::string_view ext = src.substr(src.size() - 4);
    return (ext[0] == '.') && (ext[1] == 's' || ext[1] == 'S') && (ext[2] == 'v' || ext[2] == 'V') &&
           (ext[3] == 'g' || ext[3] == 'G');
}

/// Strips `name`'s directory component (the "去掉目錄的檔名" fallback match, Docs/tasks/T-59.md).
std::string basenameOf(std::string_view name) { return pathToUtf8(pathFromUtf8(name).filename()); }

/// Resolves a `<link href>` value against `files` first -- exact `name` match, then a match on
/// `name`s with their directory stripped -- and only then falls back to `rootDir / name` on disk
/// (Docs/design/17-editor.md §17.7, Docs/tasks/T-59.md's `compileDocumentFromStrings`). `files` is
/// empty for `compileDocument()`, which always falls through to the disk read below (its previous,
/// unchanged behavior).
bool resolveLinkedText(std::span<const SourceFile> files, const std::string& rootDir, const std::string& name,
                       std::string& outText) {
    for (const SourceFile& f : files) {
        if (f.name == name) {
            outText.assign(f.text);
            return true;
        }
    }
    std::string base = basenameOf(name);
    for (const SourceFile& f : files) {
        if (basenameOf(f.name) == base) {
            outText.assign(f.text);
            return true;
        }
    }
    return readFileText(resolveCompileDocument(rootDir, name), outText);
}

/// T-235: the `<link rel="symbols" href="xt-standard:<Name>">` scheme (CompileOptions::
/// standardSymbolDir).
constexpr std::string_view kStandardSymbolScheme = "xt-standard:";

bool isStandardSymbolHref(std::string_view href) { return href.rfind(kStandardSymbolScheme, 0) == 0; }

/// A file stem without path separators, dots or colons (so `xt-standard:` never leaves its dir).
/// T-342 (03 §3.13): true for an element carrying the `editor-only` attribute.
bool isEditorOnly(const DomNode& node) { return node.attr("editor-only") != nullptr; }

/// T-342 (03 §3.13, 22 §22.15, CompileOptions::stripEditorOnly): removes every `editor-only`
/// element of `tree` with its subtree and renumbers the rest (children, parent, `body`,
/// `symbolDefs`; DomTree node index == DOCT element index). The `id` of each removed element is
/// appended to `removedIds`, the `name` of each removed `<symbol>` to `removedSymbols`.
/// Drops every node `removed` marks (with nothing else checked: a caller marks whole subtrees)
/// and renumbers the rest of `tree`. Shared by stripEditorOnlyNodes() and keepEditorOnlyNodes().
void removeMarkedNodes(DomTree& tree, const std::vector<uint8_t>& removed);

void stripEditorOnlyNodes(DomTree& tree, std::vector<std::string>& removedIds,
                          std::vector<std::string>& removedSymbols) {
    constexpr uint32_t kNoneCompileDocument = 0xFFFFFFFFu;
    const size_t count = tree.nodes.size();
    std::vector<uint8_t> removed(count, 0);
    bool any = false;
    // Depth-first from every root (the `<xt>` root and the detached `<head>` symbols), in
    // document order.
    std::vector<std::pair<uint32_t, bool>> stack;
    for (size_t i = count; i-- > 0;) {
        if (tree.nodes[i].parent == kNoneCompileDocument) {
            stack.emplace_back(static_cast<uint32_t>(i), false);
        }
    }
    while (!stack.empty()) {
        const auto [index, inherited] = stack.back();
        stack.pop_back();
        if (index >= count || removed[index] != 0) {
            continue;
        }
        const DomNode& node = tree.nodes[index];
        const bool strip = inherited || isEditorOnly(node);
        if (strip) {
            removed[index] = 1;
            any = true;
            if (const DomAttr* id = node.attr("id"); id != nullptr && !id->value.empty()) {
                removedIds.push_back(id->value);
            }
            if (node.tag == "symbol") {
                if (const DomAttr* name = node.attr("name"); name != nullptr && !name->value.empty()) {
                    removedSymbols.push_back(name->value);
                }
            }
        }
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.emplace_back(*it, strip);
        }
    }
    if (!any) {
        return;
    }
    removeMarkedNodes(tree, removed);
}

/// T-344 (CompileOptions::editorOnlyAsScene, 17 §17.31): keeps only the `editor-only` subtrees of
/// the document's own tree and the ancestors that place them; every other element of that tree
/// goes with its subtree. The root, `<body>` and the detached `<head>` symbol definitions stay.
void keepEditorOnlyNodes(DomTree& tree) {
    constexpr uint32_t kNoneCompileDocument = 0xFFFFFFFFu;
    const size_t count = tree.nodes.size();
    // 1 = inside an editor-only subtree, 2 = an ancestor of one; 0 = not kept (yet).
    std::vector<uint8_t> keep(count, 0);
    std::vector<std::pair<uint32_t, bool>> stack;
    for (size_t i = count; i-- > 0;) {
        if (tree.nodes[i].parent == kNoneCompileDocument) {
            stack.emplace_back(static_cast<uint32_t>(i), false);
        }
    }
    while (!stack.empty()) {
        const auto [index, inherited] = stack.back();
        stack.pop_back();
        if (index >= count) {
            continue;
        }
        const DomNode& node = tree.nodes[index];
        const bool inside = inherited || isEditorOnly(node);
        if (inside) {
            keep[index] = 1;
            for (uint32_t up = node.parent; up < count && keep[up] == 0; up = tree.nodes[up].parent) {
                keep[up] = 2;
            }
        }
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            stack.emplace_back(*it, inside);
        }
    }
    // The detached symbol definitions stay whole.
    for (uint32_t def : tree.symbolDefs) {
        std::vector<uint32_t> walk{def};
        while (!walk.empty()) {
            const uint32_t at = walk.back();
            walk.pop_back();
            if (at < count && keep[at] != 1) {
                keep[at] = 1;
                walk.insert(walk.end(), tree.nodes[at].children.begin(), tree.nodes[at].children.end());
            }
        }
    }
    std::vector<uint8_t> removed(count, 0);
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        const bool root = tree.nodes[i].parent == kNoneCompileDocument;
        if (keep[i] == 0 && !root && i != tree.body) {
            removed[i] = 1;
            any = true;
        }
    }
    if (any) {
        removeMarkedNodes(tree, removed);
    }
}

void removeMarkedNodes(DomTree& tree, const std::vector<uint8_t>& removed) {
    constexpr uint32_t kNoneCompileDocument = 0xFFFFFFFFu;
    const size_t count = tree.nodes.size();
    std::vector<uint32_t> remap(count, kNoneCompileDocument);
    uint32_t next = 0;
    for (size_t i = 0; i < count; ++i) {
        if (removed[i] == 0) {
            remap[i] = next++;
        }
    }
    std::vector<DomNode> kept;
    kept.reserve(next);
    for (size_t i = 0; i < count; ++i) {
        if (removed[i] != 0) {
            continue;
        }
        DomNode node = std::move(tree.nodes[i]);
        std::vector<uint32_t> children;
        std::vector<uint32_t> childPos;
        const bool parallel = node.rawChildPos.size() == node.children.size();
        for (size_t c = 0; c < node.children.size(); ++c) {
            const uint32_t child = node.children[c];
            if (child < count && remap[child] != kNoneCompileDocument) {
                children.push_back(remap[child]);
                if (parallel) {
                    childPos.push_back(node.rawChildPos[c]);
                }
            }
        }
        node.children = std::move(children);
        if (parallel) {
            node.rawChildPos = std::move(childPos);
        }
        node.parent = node.parent < count ? remap[node.parent] : kNoneCompileDocument;
        kept.push_back(std::move(node));
    }
    tree.nodes = std::move(kept);
    tree.body = tree.body < count ? remap[tree.body] : kNoneCompileDocument;
    std::vector<uint32_t> defs;
    for (uint32_t def : tree.symbolDefs) {
        if (def < count && remap[def] != kNoneCompileDocument) {
            defs.push_back(remap[def]);
        }
    }
    tree.symbolDefs = std::move(defs);
}

/// T-342 (03 §3.13 lint): warns about each reference `tree` still makes to a removed element: an
/// attribute value naming `#<id>` (a trigger action, a binding), or a `<use>` of a removed symbol.
void warnEditorOnlyReferences(const DomTree& tree, const std::string& file,
                              const std::vector<std::string>& removedIds,
                              const std::vector<std::string>& removedSymbols, DiagnosticSink& sink) {
    auto isIdChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_' || c == '/';
    };
    for (const DomNode& node : tree.nodes) {
        if (node.tag == "use") {
            if (const DomAttr* symbol = node.attr("symbol"); symbol != nullptr) {
                for (const std::string& name : removedSymbols) {
                    if (symbol->value == name) {
                        sink.warn(file, "editor-only element '" + name + "' is referenced by <use>",
                                  node.line, node.col);
                    }
                }
            }
        }
        for (const DomAttr& attr : node.attrs) {
            for (const std::string& id : removedIds) {
                const std::string needle = "#" + id;
                for (size_t at = attr.value.find(needle); at != std::string::npos;
                     at = attr.value.find(needle, at + 1)) {
                    const size_t end = at + needle.size();
                    if (end == attr.value.size() || !isIdChar(attr.value[end])) {
                        sink.warn(file,
                                  "editor-only element '" + id + "' is referenced by <" + node.tag +
                                      "> attribute '" + attr.name + "'",
                                  attr.line, attr.col);
                        break;
                    }
                }
            }
        }
    }
}

bool isPlainStem(std::string_view stem) {
    return !stem.empty() && stem.find_first_of("/\\.:") == std::string_view::npos;
}

/// T-201 (26.6, 30.9): a `<link rel="symbols" href>` or stylesheet `<link href>` resolves by path
/// only -- `files` entries named exactly `href`, or naming the same normalized path (both relative
/// to `rootDir`), then `rootDir / href` on disk. Unlike resolveLinkedText() there is no file-name
/// fallback (T-138's search order stays for assets), so a library never resolves to a same-named
/// file elsewhere, least of all the linking document itself. `self` is set when `href` names
/// `mainName` (a document may not link itself).
bool resolveLinkPath(std::span<const SourceFile> files, const std::string& rootDir, std::string_view mainName,
                     const std::string& href, std::string& outText, bool& self) {
    auto normal = [&](std::string_view name) {
        return genericPathToUtf8(resolveCompileDocument(rootDir, std::string(name)).lexically_normal());
    };
    const std::string target = normal(href);
    self = href == mainName || target == normal(mainName);
    if (self) {
        return false;
    }
    for (const SourceFile& f : files) {
        if (f.name == href) {
            outText.assign(f.text);
            return true;
        }
    }
    for (const SourceFile& f : files) {
        if (f.name != mainName && normal(f.name) == target) {
            outText.assign(f.text);
            return true;
        }
    }
    return readFileText(resolveCompileDocument(rootDir, href), outText);
}

/// Warns once per member of `obj` that is not in `known` (Docs/tasks/T-172.md: `.xtanim.json`
/// supports only 11.9's fields; unknown ones are a warning).
void warnUnknownFields(const nlohmann::json& obj, std::initializer_list<const char*> known,
                       const std::string& file, const std::string& where, DiagnosticSink& sink) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool ok = false;
        for (const char* k : known) {
            if (it.key() == k) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            sink.warn(file, "unknown field \"" + it.key() + "\" in " + where + ", ignored");
        }
    }
}

/// Docs/design/11-lottie-mapping.md §11.9 / 12-compiler.md §12.7 (T-172): loads the
/// `@timeline <name> from "<file>#<name>"` reference of `def` from its `.xtanim.json` and merges it
/// into `def` (the file's tracks first, then any inline tracks; an inline `duration` wins; markers
/// are concatenated). Every key goes through the inline key parser, so the result is exactly what
/// the equivalent inline `@timeline` would produce. Returns false (after reporting an error) when
/// the file or the named timeline cannot be read.
bool mergeXtanimTimeline(std::span<const SourceFile> files, const std::string& xtDir, TimelineDef& def,
                         DiagnosticSink& sink, std::vector<EasingPoint>& easingPoints) {
    using nlohmann::json;
    const std::string& file = def.fromFile;
    std::string text;
    if (!resolveLinkedText(files, xtDir, file, text)) {
        sink.error(def.fileName, "failed to read file \"" + file + "\"", def.line);
        return false;
    }
    std::string depthError;  // T-262c
    json root = detail::parseBounded(text, detail::kMaxJsonDepth, depthError);
    if (!depthError.empty()) {
        sink.error(file, "invalid JSON: " + depthError);
        return false;
    }
    if (root.is_discarded()) {
        // Not JSON (and not too deep): parse again with exceptions for nlohmann's message.
        try {
            // Only the exception message is wanted; the parsed value is discarded.
            const json probe = json::parse(text);
            (void)probe;
            sink.error(file, "invalid JSON");
        } catch (const std::exception& e) {
            sink.error(file, std::string("invalid JSON: ") + e.what());
        }
        return false;
    }
    if (!root.is_object() || !root.contains("timelines") || !root["timelines"].is_array()) {
        sink.error(file, "not an .xtanim.json file (missing \"timelines\")");
        return false;
    }
    warnUnknownFields(root, {"v", "timelines"}, file, "the file", sink);
    if (!root.contains("v") || !root["v"].is_number_integer() || root["v"].get<int>() != 1) {
        sink.warn(file, "unsupported or missing \"v\" (expected 1)");
    }
    const json* found = nullptr;
    for (const json& tl : root["timelines"]) {
        if (tl.is_object() && tl.contains("name") && tl["name"].is_string() &&
            tl["name"].get<std::string>() == def.fromName) {
            found = &tl;
            break;
        }
    }
    if (found == nullptr) {
        sink.error(def.fileName, "\"" + file + "\" has no timeline \"" + def.fromName + "\"", def.line);
        return false;
    }
    const json& tl = *found;
    std::string where = "timeline \"" + def.fromName + "\"";
    warnUnknownFields(tl, {"name", "duration", "markers", "tracks"}, file, where, sink);

    TimelineDef merged;
    merged.name = def.name;
    merged.fileName = def.fileName;
    merged.line = def.line;
    merged.symbol = def.symbol;  // T-190: a symbol-scoped timeline stays one after the merge.
    merged.scopePath = def.scopePath;
    merged.duration = def.duration;
    if (merged.duration < 0 && tl.contains("duration") && tl["duration"].is_number()) {
        merged.duration = tl["duration"].get<float>();
    }
    bool ok = true;
    if (tl.contains("markers") && tl["markers"].is_array()) {
        for (const json& m : tl["markers"]) {
            if (!m.is_array() || m.size() != 2 || !m[0].is_number() || !m[1].is_string()) {
                sink.error(file, where + ": a marker must be [t, name]");
                ok = false;
                continue;
            }
            merged.markers.emplace_back(m[1].get<std::string>(), m[0].get<float>());
        }
    }
    if (tl.contains("tracks") && tl["tracks"].is_array()) {
        for (const json& tr : tl["tracks"]) {
            if (!tr.is_object() || !tr.contains("layer") || !tr["layer"].is_string() ||
                !tr.contains("prop") || !tr["prop"].is_string() || !tr.contains("keys") ||
                !tr["keys"].is_array()) {
                sink.error(file, where + ": a track needs \"layer\", \"prop\" and \"keys\"");
                ok = false;
                continue;
            }
            warnUnknownFields(tr, {"layer", "prop", "keys"}, file, where + " track", sink);
            TimelineTrackDef track;
            track.path = tr["layer"].get<std::string>();
            track.line = def.line;
            std::string prop = tr["prop"].get<std::string>();
            for (const json& k : tr["keys"]) {
                if (!k.is_array() || k.size() < 2 || k.size() > 4 || !k[0].is_number() ||
                    k[0].get<float>() < 0) {
                    sink.error(file, where + ": a key must be [t, value, easing?] with t >= 0");
                    ok = false;
                    continue;
                }
                TimelineKey key;
                key.time = k[0].get<float>();
                key.line = def.line;
                std::string valueText;
                if (k[1].is_number()) {
                    char buf[48];
                    valueText.assign(buf, xt::formatFloat(buf, sizeof(buf), k[1].get<double>(), 9));
                } else if (k[1].is_string()) {
                    valueText = k[1].get<std::string>();
                } else {
                    sink.error(file, where + ": a key value must be a number or a CSS value string");
                    ok = false;
                    continue;
                }
                if (k.size() >= 3 && !k[2].is_null() &&
                    !(k[2].is_string() && k[2].get<std::string>().empty())) {
                    if (!k[2].is_string() ||
                        !parseEasingText(k[2].get<std::string>(), key.easing, &easingPoints)) {
                        sink.error(file, where + ": invalid easing");
                        ok = false;
                        continue;
                    }
                    key.hasEasing = true;
                }
                if (k.size() == 4) {
                    // T-228: the spatial handles, `[inX, inY, outX, outY]` (px).
                    const json& m = k[3];
                    bool good = m.is_array() && m.size() == 4;
                    for (size_t i = 0; good && i < 4; ++i) {
                        good = m[i].is_number();
                    }
                    if (!good) {
                        sink.error(file, where + ": motion handles must be [inX, inY, outX, outY]");
                        ok = false;
                        continue;
                    }
                    key.hasMotion = true;
                    key.motionIn = Vec2{m[0].get<float>(), m[1].get<float>()};
                    key.motionOut = Vec2{m[2].get<float>(), m[3].get<float>()};
                }
                size_t before = sink.all().size();
                parseTimelineKeyDeclarations(prop + ": " + valueText, file, 0, sink, key);
                for (size_t i = before; i < sink.all().size(); ++i) {
                    if (sink.all()[i].level == Diagnostic::Level::Error) {
                        ok = false;
                    }
                }
                track.keys.push_back(std::move(key));
            }
            merged.tracks.push_back(std::move(track));
        }
    }
    if (!ok) {
        return false;
    }
    for (auto& m : def.markers) {
        merged.markers.push_back(std::move(m));
    }
    for (auto& t : def.tracks) {
        merged.tracks.push_back(std::move(t));
    }
    for (auto& snd : def.sounds) {
        merged.sounds.push_back(std::move(snd));
    }
    def = std::move(merged);
    return true;
}

/// Shared pipeline body for `compileDocument()` and `compileDocumentFromStrings()` (Docs/tasks/
/// T-59.md): `mainText`/`mainName` are the `.xt` document itself; `files` supplies in-memory
/// `<link>` targets (empty for `compileDocument()`, which only ever has the one file already read
/// into `mainText`); `xtDir` is the base directory every relative path (`<link href>` disk
/// fallback, `<anim src>`, `<img src>`, `@font-face src`) resolves against -- the `.xt` file's own
/// directory for `compileDocument()`, `options.rootDir` for `compileDocumentFromStrings()` (see its
/// doc comment). Mirrors Docs/design/12-compiler.md §12.2-§12.3's pipeline exactly; see
/// `compileDocument()`'s own (unmoved) doc comment for the step-by-step description.
CompileResult compileDocumentImpl(std::string_view mainText, std::string_view mainName,
                                  std::span<const SourceFile> files, const std::string& xtDir,
                                  const CompileOptions& options) {
    CompileResult result;
    DiagnosticSink sink;
    sink.setReportDeprecated(options.strict || options.reportDeprecated);  // T-408
    std::string path(mainName);
    std::string fileName = basenameOf(mainName);

    // Step 1: parseXt.
    DomTree dom;
    if (!parseXt(mainText, path, sink, dom)) {
        result.diagnostics = sink.all();
        return result;
    }
    // Step 1a' (T-342, 03 §3.13): the publish view drops `editor-only` subtrees before anything
    // else sees them; the symbol libraries get the same treatment as they are read below.
    std::vector<std::string> strippedSymbols;
    if (options.stripEditorOnly) {
        stripEditorOnlyNodes(dom, result.strippedIds, strippedSymbols);
    } else if (options.editorOnlyAsScene) {
        keepEditorOnlyNodes(dom);  // T-344 (17 §17.31): the Demo space's scene.
    }

    IrWriter writer;

    // Step 2: implied stylesheets, then <link href>, then <style> -> one RuleSet, source order =
    // concatenation order. `options.impliedStylesheets` (a bundle's `shared.styles`,
    // Docs/design/22-bundle.md §22.2) behaves exactly as if it were listed before the document's
    // own <link>s, so the document still wins on equal specificity.
    RuleSet ruleSet;
    for (const std::string& implied : options.impliedStylesheets) {
        std::string impliedText;
        // T-368: an in-memory file of exactly this name wins over the disk (the editor's unsaved
        // copy of a shared token sheet), as in-memory files do for `<link href>`.
        bool inMemory = false;
        for (const SourceFile& f : files) {
            if (f.name == implied) {
                impliedText.assign(f.text);
                inMemory = true;
                break;
            }
        }
        if (inMemory) {
            parseXts(impliedText, implied, 1, sink, ruleSet);
            continue;
        }
        fs::path impliedPath = pathFromUtf8(implied).is_absolute()
                                   ? pathFromUtf8(implied)
                                   : resolveCompileDocument(options.rootDir, implied);
        if (!readFileText(impliedPath, impliedText)) {
            sink.error(implied, "failed to read file \"" + implied + "\"");
            continue;
        }
        parseXts(impliedText, implied, 1, sink, ruleSet);
    }
    // T-171: symbol libraries (`<link rel="symbols" href>`), resolved like stylesheet links. Their
    // own `<style>` blocks (where their `symbol(Name){}` rules live) come before the document's
    // stylesheets, so the document still wins on equal specificity.
    std::vector<SymbolLibraryFile> symbolLibraries;
    for (const std::string& href : dom.symbolHrefs) {
        std::string libText;
        bool self = false;
        if (isStandardSymbolHref(href)) {
            // T-235: `xt-standard:<Name>` -> `<options.standardSymbolDir>/<Name>.xt`.
            const std::string stem = href.substr(kStandardSymbolScheme.size());
            if (!isPlainStem(stem)) {
                sink.error(href, "invalid standard symbol name in \"" + href + "\"");
                continue;
            }
            if (options.standardSymbolDir.empty()) {
                sink.error(href, "no standard symbol library to resolve \"" + href + "\"");
                continue;
            }
            // T-241: the library root, then its `mobile/` subset (23 §23.11.2); T-382: `list/`.
            const fs::path standardDir = pathFromUtf8(options.standardSymbolDir);
            if (!readFileText(standardDir / pathFromUtf8(stem + ".xt"), libText) &&
                !readFileText(standardDir / "mobile" / pathFromUtf8(stem + ".xt"), libText) &&
                !readFileText(standardDir / "list" / pathFromUtf8(stem + ".xt"), libText)) {
                sink.error(href, "standard symbol \"" + stem + "\" not found in \"" +
                                     options.standardSymbolDir + "\"");
                continue;
            }
        } else if (!resolveLinkPath(files, xtDir, mainName, href, libText, self)) {
            sink.error(href, self ? "a document cannot link itself (\"" + href + "\")"
                                  : "failed to read file \"" + href + "\"");
            continue;
        }
        SymbolLibraryFile lib;
        lib.name = href;
        if (!parseXtSymbolLibrary(libText, href, sink, lib.tree)) {
            continue;
        }
        if (options.stripEditorOnly) {
            std::vector<std::string> libIds;  // T-342: a library's ids are not the document's.
            stripEditorOnlyNodes(lib.tree, libIds, strippedSymbols);
        }
        for (const auto& [styleText, startLine] : lib.tree.inlineStyles) {
            parseXts(styleText, href, startLine, sink, ruleSet);
        }
        symbolLibraries.push_back(std::move(lib));
    }
    for (const std::string& href : dom.linkHrefs) {
        std::string linkText;
        bool self = false;
        if (!resolveLinkPath(files, xtDir, mainName, href, linkText, self)) {
            sink.error(href, self ? "a document cannot link itself (\"" + href + "\")"
                                  : "failed to read file \"" + href + "\"");
            continue;
        }
        parseXts(linkText, href, 1, sink, ruleSet);
    }
    for (const auto& [styleText, startLine] : dom.inlineStyles) {
        parseXts(styleText, path, startLine, sink, ruleSet);
    }

    if (!result.strippedIds.empty() || !strippedSymbols.empty()) {
        warnEditorOnlyReferences(dom, path, result.strippedIds, strippedSymbols, sink);
        for (const SymbolLibraryFile& lib : symbolLibraries) {
            warnEditorOnlyReferences(lib.tree, lib.name, {}, strippedSymbols, sink);
        }
    }

    // Step 1b (T-172, 12.7): `@timeline ... from "<file>#<name>"` merges its `.xtanim.json` first.
    // T-190: before symbol expansion, so a symbol-scoped timeline's file tracks are rewritten per
    // instance like its inline ones.
    for (TimelineDef& def : ruleSet.timelines) {
        if (!def.fromFile.empty()) {
            mergeXtanimTimeline(files, xtDir, def, sink, ruleSet.easingPoints);
        }
    }

    // Step 2a (T-171, 12.7): symbol expansion -- after parsing, before style compilation.
    std::vector<ExpandedSymbolInstance> symbolInstances;
    {
        // T-201: `spawn()` actions -> spawn pools (a `<repeat spawn>` per element and symbol),
        // before expansion so a spawn inside a symbol definition gets one pool per instance.
        uint32_t spawnCounter = 0;
        lowerSpawnPools(dom, spawnCounter);
        for (SymbolLibraryFile& lib : symbolLibraries) {
            lowerSpawnPools(lib.tree, spawnCounter);
        }
    }
    if (!expandSymbols(dom, symbolLibraries, ruleSet, path, sink, symbolInstances)) {
        result.diagnostics = sink.all();
        return result;
    }
    // Step 2a' (T-177, 12.7): `<repeat>` templates; item-enter/exit become template-root triggers.
    std::vector<RepeatDecl> repeatDecls;
    RepeatScopes repeatScopes;
    {
        std::vector<uint32_t> instanceRoots;  // T-380: symbol-owned ports inside templates.
        instanceRoots.reserve(symbolInstances.size());
        for (const ExpandedSymbolInstance& inst : symbolInstances) {
            instanceRoots.push_back(inst.instanceNode);
        }
        prepareRepeats(dom, path, sink, repeatDecls, repeatScopes, instanceRoots);
    }

    // Step 2b (T-170, 12.7): design tokens -> VARS; `var()` references are kept (or substituted
    // where a reference cannot be kept) now that every stylesheet's tokens are known.
    TokenScope tokens;
    emitTokens(ruleSet, writer, sink, tokens);
    resolveRuleSetVars(ruleSet, tokens, path, sink);
    // T-203 (18.7): `custom("<asset>")` effects load their `.xtfx` (ASST kind 2 + SHDR) and get
    // their parameter values before anything is encoded.
    resolveCustomEffects(ruleSet, writer, xtDir, path, options, sink);

    // Step 3: <anim src> -> parseLottie, deduplicated by src.
    std::map<std::string, SceneId> sceneBySrc;
    for (const DomNode& node : dom.nodes) {
        if (node.tag != "anim") {
            continue;
        }
        const DomAttr* srcAttr = node.attr("src");
        if (srcAttr == nullptr || sceneBySrc.count(srcAttr->value) != 0) {
            continue;
        }
        const std::string& src = srcAttr->value;
        fs::path jsonPath = resolveAsset(options, xtDir, src);
        std::vector<uint8_t> jsonBytes;
        if (!readAssetFileBytes(options, jsonPath, jsonBytes)) {  // T-452
            sink.error(path, "anim src \"" + src + "\" not found", node.line, node.col);
            continue;
        }
        LottieOptions lopts;
        lopts.strict = options.strict;
        lopts.sourceName = publishedReference(src, xtDir, options);  // T-273c: the scene's name.
        lopts.rootDir = pathToUtf8(jsonPath.parent_path());
        lopts.embedImages = options.embedImages;
        LottieResult lr =
            parseLottie(std::span<const uint8_t>(jsonBytes.data(), jsonBytes.size()), lopts, writer, sink);
        if (lr.ok) {
            sceneBySrc.emplace(src, lr.scene);
        }
    }

    // Step 3a (T-110): <img src="*.svg"> -> parseSvg, deduplicated by src the same way. The scene
    // is static, but an empty Timeline is added alongside it so the SceneId <-> TimelineId identity
    // `Document::sceneTimeline()` relies on keeps holding for the Lottie scenes around it.
    for (const DomNode& node : dom.nodes) {
        if (node.tag != "img") {
            continue;
        }
        const DomAttr* srcAttr = node.attr("src");
        if (srcAttr == nullptr || !hasSvgExtension(srcAttr->value) || sceneBySrc.count(srcAttr->value) != 0 ||
            isExternalAssetName(srcAttr->value)) {
            continue;  // T-150: `engine://x.svg` is an external texture, not a vector file.
        }
        const std::string& src = srcAttr->value;
        const fs::path svgPath = resolveAsset(options, xtDir, src);
        std::vector<uint8_t> svgBytes;
        if (!readAssetFileBytes(options, svgPath, svgBytes)) {  // T-452
            sink.error(path, "img src \"" + src + "\" not found", node.line, node.col);
            continue;
        }
        const std::string svgText(svgBytes.begin(), svgBytes.end());
        // T-273c: the scene and its timeline are named by the published reference.
        const std::string sceneName = publishedReference(src, xtDir, options);
        SceneBuilder svgScene;
        // T-273d: an `<xt-mesh href>` of the file is registered as the document's own are
        // (ensureImageAsset(): resolved, renamed by `assetNameRewrite`, packed by a bundle), but
        // resolved against the SVG file's own directory.
        CompileOptions svgOptions = options;
        svgOptions.searchDirs.clear();
        const std::string svgDir = pathToUtf8(svgPath.parent_path());
        SvgParseExtras svgExtras;
        svgExtras.imageAsset = [&](const std::string& href, int line) {
            return ensureImageAsset(writer, href, svgDir, src, line, 0, svgOptions, sink);
        };
        if (parseSvg(svgText, sceneName, sink, writer, svgScene, nullptr, &svgExtras) != Status::kOk) {
            continue;
        }
        TimelineBuilder emptyTimeline;
        emptyTimeline.name = writer.intern(sceneName);
        SceneId sceneId = writer.addScene(std::move(svgScene));
        writer.addTimeline(std::move(emptyTimeline));
        sceneBySrc.emplace(src, sceneId);
    }

    // Step 3b (T-190, 11.9): inline <svg> -> parseSvg + the scene's own Timeline from the
    // @timeline tracks that address its nodes (`#<svg id>/<id>`).
    // T-218: then warn about scene-target rules naming a node the <svg> does not have.
    std::map<std::string, std::vector<std::string>> svgInnerIds;
    // T-236: the inline <svg>s first, so a scene target's last segment is matched against their
    // ids before it is taken as a tag name (emitInlineSvgs only reads the @timeline tracks).
    // T-243 (32 §32.2.4/§32.4): `<xt-mesh href>` images go through the `<img src>` registration;
    // the content's `bind:` attributes are compiled with the others in step 5a.
    InlineSvgRig svgRig;
    svgRig.imageAsset = [&](const std::string& href, int line) {
        return ensureImageAsset(writer, href, xtDir, path, line, 0, options, sink);
    };
    emitInlineSvgs(dom, ruleSet, writer, path, sink, sceneBySrc, &svgInnerIds, &svgRig);
    resolveSceneTargets(ruleSet, dom, path, sink, &svgInnerIds);
    warnUnknownSceneTargets(ruleSet, svgInnerIds, path, sink);

    StyleBuilder style;
    style.easingPoints = ruleSet.easingPoints;  // T-335: the same offsets index both.

    // Step 4: @keyframes -> Timeline.
    std::unordered_map<std::string, TimelineId> keyframesByName;
    emitKeyframesTimelines(ruleSet, writer, style, path, sink, keyframesByName);

    // Step 4a' (T-344, CompileOptions::editorOnlyAsScene): the scene keeps only the `editor-only`
    // subtrees, so a `@timeline` track naming an element that is not among them is dropped (with
    // nothing reported) instead of failing the compile.
    if (options.editorOnlyAsScene && !options.stripEditorOnly) {
        std::unordered_set<std::string_view> ids;
        for (const DomNode& node : dom.nodes) {
            if (const DomAttr* id = node.attr("id"); id != nullptr && !id->value.empty()) {
                ids.insert(id->value);
            }
        }
        for (TimelineDef& def : ruleSet.timelines) {
            std::erase_if(def.tracks,
                          [&](const TimelineTrackDef& track) { return ids.count(track.path) == 0; });
        }
    }

    // Step 4a (T-172, 12.7): every `@timeline` -> per-track keyframes + a TGRP group (the
    // `.xtanim.json` references were merged in step 1b).
    TimelineGroupBuilder timelineGroups;
    emitTimelineGroups(ruleSet, dom, writer, style, keyframesByName, sink, timelineGroups);

    // Step 5: RuleSet -> StyleBuilder (rules/decls/@font-face).
    emitStyle(ruleSet, writer, style, keyframesByName, xtDir, path, options, sink);

    // Step 5a (T-174, T-175, 12.7): declared ports (`PORT`), custom states (`STAT`), trigger
    // tables (`TRIG`, which resolve port names) and bindings (`BIND`, which use both).
    {
        PortBuilder ports;
        PortScopes portScopes;
        normalizeBindAliases(dom, path, sink);  // T-385: `bind:state.Disabled` -> `bind:disabled`.
        emitPorts(dom, symbolInstances, writer, path, sink, ports, portScopes, &svgRig.boneIds);
        applyUseLiterals(dom, symbolInstances, portScopes, writer, path, sink, ports,
                         options.strict);  // T-385
        resolveRepeatPorts(repeatDecls, portScopes, ports, writer, path, sink, repeatScopes);
        CustomStateBuilder customStates;
        TriggerBuilder triggerTables;
        emitTriggers(dom, ruleSet, portScopes, writer, path, sink, customStates, triggerTables,
                     &repeatScopes);  // T-411: item() / repeat.* set() values.
        BindingBuilder bindings;
        emitBindings(dom, symbolInstances, portScopes, customStates, writer, path, sink, bindings,
                     &repeatScopes, &svgRig.binds);
        // T-177: `RPT ` rows (the template roots' item-enter/exit triggers are in TRIG now).
        std::vector<IrRepeat> repeats;
        emitRepeats(dom, repeatDecls, repeatScopes, triggerTables, path, sink, repeats);
        writer.setCustomStates(std::move(customStates));
        writer.setTriggers(std::move(triggerTables));
        writer.setPorts(std::move(ports));
        writer.setBindings(std::move(bindings));
        writer.setRepeats(std::move(repeats));
    }

    // Step 6: DomTree -> DocBuilder (elements/text/attrs/classes/inline decls/anim scene/img asset).
    // T-179 (07.8, 12.7): `@key` text/attributes -> STRS keys, alongside DOCT.
    DocBuilder doc;
    LocalizationBuilder l10n;
    emitDoc(dom, writer, style, doc, sceneBySrc, keyframesByName, xtDir, path, options, sink, &tokens, &l10n,
            &ruleSet.rules);

    // T-255: the runtime rejects an element at depth kMaxElementDepth or deeper. One file nests at
    // most 256 levels, but symbol expansion stacks instance bodies, so the expanded tree can go deeper.
    {
        std::vector<uint32_t> depths(dom.nodes.size(), 0);
        for (size_t i = 0; i < dom.nodes.size(); ++i) {
            const DomNode& node = dom.nodes[i];
            if (node.parent >= i) {
                continue;  // The root (dom.nodes is preorder: a parent always comes first).
            }
            depths[i] = depths[node.parent] + 1;
            if (depths[i] >= kMaxElementDepth) {
                sink.error(path,
                           "element nesting too deep after symbol expansion (the runtime allows at most " +
                               std::to_string(kMaxElementDepth) + " levels)",
                           node.line, node.col);
                break;
            }
        }
    }

    // Step 6a (T-179): `@font-fallback` chains -> STRS; a later block for the same language (by
    // exact tag) replaces an earlier one, keeping the earlier one's position.
    {
        std::vector<const FontFallbackDef*> chains;
        for (const FontFallbackDef& def : ruleSet.fontFallbacks) {
            bool replaced = false;
            for (const FontFallbackDef*& existing : chains) {
                if (existing->lang == def.lang) {
                    existing = &def;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                chains.push_back(&def);
            }
        }
        for (const FontFallbackDef* def : chains) {
            IrFontFallback fb{};
            fb.lang = writer.intern(def->lang);
            fb.familyOffset = static_cast<uint32_t>(l10n.fallbackFamilies.size());
            fb.familyCount = static_cast<uint32_t>(def->families.size());
            for (const std::string& family : def->families) {
                l10n.fallbackFamilies.push_back(writer.intern(family));
            }
            l10n.fallbacks.push_back(fb);
        }
    }

    writer.setStyle(std::move(style));
    writer.setDoc(std::move(doc));
    writer.setLocalization(std::move(l10n));
    writer.setTimelineGroups(std::move(timelineGroups));

    // SYMB (T-171): the symbol source table; DomTree node index == DOCT element index.
    {
        std::vector<SymbolInstanceBuilder> symbols;
        symbols.reserve(symbolInstances.size());
        for (const ExpandedSymbolInstance& inst : symbolInstances) {
            SymbolInstanceBuilder sib;
            sib.symbol = writer.intern(inst.symbol);
            sib.instanceElement = inst.instanceNode;
            for (const auto& [pathId, element] : inst.paths) {
                sib.paths.push_back(IrSymbolPath{writer.intern(pathId), element});
            }
            symbols.push_back(std::move(sib));
        }
        writer.setSymbols(std::move(symbols));
    }

    // SRCM: file name table = the main file, then every `<link href>` (its literal href text, in
    // document order) the DomTree actually references (Docs/design/17-editor.md §17.7) -- driven
    // by `dom.linkHrefs` rather than the caller's raw `files` list so `compileDocument()` and
    // `compileDocumentFromStrings()` agree byte-for-byte when `files` is exactly "the main file
    // plus its <link>ed files in order" (Docs/tasks/T-59.md's test). Then one entry per DomTree
    // node (== DocBuilder::elements, same order/count) followed by one entry per text-bearing node
    // (== DocBuilder::texts, same order/count); every element/text entry points at fileId 0, the
    // main file (T-42c: style declarations aren't individually source-mapped).
    {
        StringId mainFileId = writer.intern(fileName);
        std::vector<StringId> srcmFiles{mainFileId};
        for (const std::string& href : dom.linkHrefs) {
            // T-273c: a published build names a stylesheet outside the project by its file name; it
            // is compiled in, so nothing reads the path again.
            srcmFiles.push_back(writer.intern(publishedReference(href, xtDir, options)));
        }
        std::vector<IrSourceEntry> srcmEntries;
        srcmEntries.reserve(dom.nodes.size());
        for (const DomNode& node : dom.nodes) {
            srcmEntries.push_back(
                IrSourceEntry{0, static_cast<uint32_t>(node.line), static_cast<uint32_t>(node.col)});
        }
        for (const DomNode& node : dom.nodes) {
            if (node.hasText) {
                srcmEntries.push_back(
                    IrSourceEntry{0, static_cast<uint32_t>(node.line), static_cast<uint32_t>(node.col)});
            }
        }
        writer.setSourceMap(std::move(srcmFiles), std::move(srcmEntries));
    }

    // Step 7 (animation-name / transform) is already enforced by steps 4-6 above (encodeDeclValue /
    // T-22's parser respectively).

    if (options.emitMeta) {
        StringId sourceFileId = writer.intern(fileName);
        StringId versionId = writer.intern(std::string("xtframe-compile ") + versionString());
        // <ctime> instead of <chrono>: libstdc++ 14's <chrono> pulls in <format>, which clang 15
        // cannot compile (same reasoning as compile_lottie.cpp).
        uint64_t unixTime = static_cast<uint64_t>(std::time(nullptr));
        writer.setMeta(sourceFileId, versionId, unixTime, options.behavior);  // T-407
    }

    result.assets = collectCompiledAssets(writer);

    // T-408: a deprecated alias is reported under strict but does not fail it.
    bool failed = sink.hasErrors() || (options.strict && sink.warningCount() > sink.deprecatedCount());
    if (!failed) {
        result.ir = writer.finish();
    }
    result.diagnostics = sink.all();
    return result;
}

}  // namespace

CompileResult compileDocument(std::string_view xtPath, const CompileOptions& options) {
    std::string path(xtPath);
    std::string text;
    if (!readFileText(pathFromUtf8(path), text)) {
        DiagnosticSink sink;
        sink.error(path, "failed to read file");
        CompileResult result;
        result.diagnostics = sink.all();
        return result;
    }

    fs::path xtFsPath = pathFromUtf8(path);
    std::string xtDir = pathToUtf8(xtFsPath.parent_path());
    std::string fileName = pathToUtf8(xtFsPath.filename());

    SourceFile main{fileName, text};
    return compileDocumentImpl(text, fileName, std::span<const SourceFile>(&main, 1), xtDir, options);
}

CompileResult compileDocumentFromStrings(std::span<const SourceFile> files, std::string_view mainName,
                                         const CompileOptions& options) {
    std::string_view mainText;
    bool found = false;
    for (const SourceFile& f : files) {
        if (f.name == mainName) {
            mainText = f.text;
            found = true;
            break;
        }
    }
    if (!found) {
        DiagnosticSink sink;
        std::string mainNameStr(mainName);
        sink.error(mainNameStr, "main file \"" + mainNameStr + "\" not found in files");
        CompileResult result;
        result.diagnostics = sink.all();
        return result;
    }
    return compileDocumentImpl(mainText, mainName, files,
                               options.xtDir.empty() ? options.rootDir : options.xtDir, options);
}

}  // namespace xt::compile
