#include "doc_emit.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "compiled_assets.h"
#include "css_number.h"  // core/src: the CSS number (T-272).
#include "style_emit.h"
#include "svg_timeline.h"
#include "xt/asset_resolver.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/utf8_path.h"
#include "xts/xts_props.h"  // styleprop::BorderImageSource (T-199).

/// See docs/design/02-ir-format.md §2.9, docs/design/12-compiler.md §12.2 step 6, docs/tasks/T-23.md.

namespace xt::compile {

namespace {

namespace fs = std::filesystem;

/// Splits on ASCII whitespace, dropping empty tokens (leading/trailing/repeated spaces) and
/// duplicate class names within the same `class` attribute (first occurrence order kept).
std::vector<std::string> splitClassList(const std::string& value) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i > start) {
            std::string tok = value.substr(start, i - start);
            if (std::find(tokens.begin(), tokens.end(), tok) == tokens.end()) {
                tokens.push_back(std::move(tok));
            }
        }
    }
    return tokens;
}

/// Appends one (name, value) pair to the element's attr slice, tracking `attrOffset` as the
/// position of the first attribute pushed for this element (subsequent pushes for the same element
/// stay contiguous since doc.attrPool is only ever appended to, one element fully at a time).
void pushAttr(DocBuilder& doc, IrDocElement& el, StringId name, StringId value) {
    if (el.attrCount == 0) {
        el.attrOffset = static_cast<uint32_t>(doc.attrPool.size());
    }
    doc.attrPool.push_back(IrAttr{name, value});
    ++el.attrCount;
}

/// Parses `text` as a single decimal float (`<camera>`'s `zoom`/`near`/`far`/each `position`-
/// `target` component, docs/design/19-3d-space.md §19.4). Returns false (leaving `out` untouched)
/// unless the whole string is one valid float token (T-272: a finite CSS number, exponent included)
/// after optional leading whitespace.
bool parseFloatAttr(const std::string& text, float& out) {
    std::string_view rest(text);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())) != 0) {
        rest.remove_prefix(1);
    }
    float v = 0;
    size_t length = 0;
    if (!css_number::read(rest, v, length) || length != rest.size()) {
        return false;
    }
    out = v;
    return true;
}

/// Splits `text` on ASCII whitespace into its non-empty tokens.
std::vector<std::string> splitWs(const std::string& text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(text.substr(start, i - start));
        }
    }
    return tokens;
}

/// Parses `<camera>`'s `position`/`target` attribute ("x y z", 19.4). `allowOmitZ` accepts a
/// 2-token form (`position` only, "可省略 z -> 0"); `target` always requires all 3. Returns false
/// (leaving `out` untouched) on a malformed value (wrong token count, non-numeric component).
bool parseVec3Attr(const std::string& text, bool allowOmitZ, Vec3& out) {
    std::vector<std::string> parts = splitWs(text);
    if (parts.size() != 3 && !(allowOmitZ && parts.size() == 2)) {
        return false;
    }
    float x = 0, y = 0, z = 0;
    if (!parseFloatAttr(parts[0], x) || !parseFloatAttr(parts[1], y)) {
        return false;
    }
    if (parts.size() == 3 && !parseFloatAttr(parts[2], z)) {
        return false;
    }
    out = Vec3{x, y, z};
    return true;
}

/// T-191 (25.3.5b): a sequence frame file (the formats the image decoders read).
bool isSequenceFrameFile(const fs::path& path) {
    std::string ext = pathToUtf8(path.extension());
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

/// T-191 (25.3.5b): the frame file names of `<sequence src>` -- a `name_%04d.png` pattern
/// (numbered from 0, or from 1 when frame 0 is missing, until the first gap) or a folder (every
/// image file in it, sorted by name). Names are `src`-relative like any `<img src>`, so they
/// resolve (and are rewritten by a bundle) exactly the same way. Empty when nothing matches
/// (and for an atlas, which the caller handles).
std::vector<std::string> sequenceFrameNames(const CompileOptions& options, std::string_view xtDir,
                                            const std::string& src) {
    std::vector<std::string> out;
    const size_t pct = src.find('%');
    if (pct != std::string::npos) {
        size_t j = pct + 1;
        bool zeroPad = false;
        if (j < src.size() && src[j] == '0') {
            zeroPad = true;
            ++j;
        }
        size_t width = 0;
        while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j])) != 0) {
            width = width * 10 + static_cast<size_t>(src[j] - '0');
            ++j;
        }
        if (j >= src.size() || src[j] != 'd' || width > 9) {
            return out;
        }
        const std::string prefix = src.substr(0, pct);
        const std::string suffix = src.substr(j + 1);
        auto nameAt = [&](int index) {
            std::string digits = std::to_string(index);
            if (zeroPad && digits.size() < width) {
                digits.insert(0, width - digits.size(), '0');
            }
            return prefix + digits + suffix;
        };
        auto exists = [&](const std::string& name) {
            std::error_code ec;
            return fs::is_regular_file(resolveAsset(options, xtDir, name), ec);
        };
        const int first = exists(nameAt(0)) ? 0 : (exists(nameAt(1)) ? 1 : -1);
        for (int i = first; i >= 0 && i < first + 100000 && exists(nameAt(i)); ++i) {
            out.push_back(nameAt(i));
        }
        return out;
    }
    std::error_code ec;
    const fs::path dir = resolveAsset(options, xtDir, src);
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    std::vector<std::string> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && isSequenceFrameFile(it->path())) {
            files.push_back(pathToUtf8(it->path().filename()));
        }
    }
    std::sort(files.begin(), files.end());
    std::string base = src;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    for (const std::string& f : files) {
        out.push_back(base + "/" + f);
    }
    return out;
}

/// T-191: `WxH` with two positive integers (`<sequence frame-size>`).
bool parseFrameSize(const std::string& text, float& w, float& h) {
    const size_t x = text.find('x');
    if (x == std::string::npos) {
        return false;
    }
    float a = 0;
    float b = 0;
    if (!parseFloatAttr(text.substr(0, x), a) || !parseFloatAttr(text.substr(x + 1), b) || !(a >= 1.0f) ||
        !(b >= 1.0f)) {
        return false;
    }
    w = a;
    h = b;
    return true;
}

/// T-202 (03 §3.12.5): the `<particles>` attributes that are `particle-<name>` declarations.
bool isParticleValueAttr(std::string_view name) {
    static constexpr std::string_view kNames[] = {"rate",      "burst",  "lifetime", "speed",
                                                  "direction", "spread", "gravity",  "drag",
                                                  "size",      "color",  "rotation", "max"};
    for (std::string_view n : kNames) {
        if (name == n) {
            return true;
        }
    }
    return false;
}

/// T-179 (07.8): the attributes whose value may be an `@key`.
bool isLocalizableAttr(std::string_view name) {
    return name == "placeholder" || name == "alt" || name == "title";
}

/// Interns `key` into `l10n->keys` once, returning its 1-based index.
uint32_t keyIndex(IrWriter& writer, LocalizationBuilder& l10n,
                  std::unordered_map<std::string, uint32_t>& byKey, const std::string& key) {
    auto it = byKey.find(key);
    if (it != byKey.end()) {
        return it->second;
    }
    l10n.keys.push_back(writer.intern(key));
    uint32_t index = static_cast<uint32_t>(l10n.keys.size());
    byKey.emplace(key, index);
    return index;
}

std::string invalidKeyMessage(std::string_view text) {
    return "invalid localization key \"" + std::string(text) +
           "\" (a key is a letter or '_' followed by letters, digits, '_' or '-'; write \\@ for a literal "
           "'@')";
}

/// T-178 (07.8): the properties a rich-text run may override. T-324 (05 §5.9.8) adds
/// `text-decoration-line` / `-color`; T-331 `font-variant-numeric`.
bool isRunOverrideProp(uint16_t prop) {
    return prop == xt::prop::Color || prop == xt::prop::FontSize || prop == xt::prop::LetterSpacing ||
           prop == 173 /* font-weight */ || prop == 174 /* font-style */ || prop == 192 /* text-effect */ ||
           prop == 203 /* text-decoration-line */ || prop == xt::prop::TextDecorationColor ||
           prop == 257 /* font-variant-numeric */;
}

/// T-178: the declarations of the plain, unconditional `.a.b` rules (one compound, classes only,
/// no tag/id/state/condition, outside `@media` and `symbol()`) whose classes are all in
/// `classes`, ordered by specificity then source order (later wins at runtime).
std::vector<const StyleDecl*> classRuleDecls(const std::vector<StyleRule>* rules,
                                             const std::vector<std::string>& classes) {
    std::vector<const StyleDecl*> out;
    if (rules == nullptr || classes.empty()) {
        return out;
    }
    struct Hit {
        uint32_t specificity;
        size_t order;
        const StyleRule* rule;
    };
    std::vector<Hit> hits;
    size_t order = 0;
    for (const StyleRule& rule : *rules) {
        ++order;
        if (rule.media >= 0 || !rule.symbol.empty()) {
            continue;
        }
        for (const Selector& sel : rule.selectors) {
            if (sel.parts.size() != 1) {
                continue;
            }
            const SelectorPart& part = sel.parts[0];
            if (!part.tag.empty() || !part.id.empty() || part.stateMask != 0 || !part.conds.empty() ||
                part.classes.empty()) {
                continue;
            }
            bool all = true;
            for (const std::string& c : part.classes) {
                if (std::find(classes.begin(), classes.end(), c) == classes.end()) {
                    all = false;
                    break;
                }
            }
            if (all) {
                hits.push_back(Hit{sel.specificity, order, &rule});
                break;
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.specificity != b.specificity ? a.specificity < b.specificity : a.order < b.order;
    });
    for (const Hit& hit : hits) {
        for (const StyleDecl& d : hit.rule->decls) {
            out.push_back(&d);
        }
    }
    return out;
}

}  // namespace

LocalizedText parseLocalizedText(std::string_view text, std::string& out) {
    if (text.size() >= 2 && text[0] == '\\' && text[1] == '@') {
        out.assign(text.substr(1));
        return LocalizedText::Literal;
    }
    if (text.empty() || text[0] != '@') {
        out.assign(text);
        return LocalizedText::Literal;
    }
    std::string_view key = text.substr(1);
    auto isAlpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    bool valid = !key.empty() && (isAlpha(key[0]) || key[0] == '_');
    for (size_t i = 1; valid && i < key.size(); ++i) {
        char c = key[i];
        valid = isAlpha(c) || isDigit(c) || c == '_' || c == '-';
    }
    if (!valid) {
        out.assign(text);
        return LocalizedText::Invalid;
    }
    out.assign(key);
    return LocalizedText::Key;
}

bool emitDoc(const DomTree& dom, IrWriter& writer, StyleBuilder& style, DocBuilder& doc,
             const std::map<std::string, SceneId>& sceneBySrc,
             const std::unordered_map<std::string, TimelineId>& keyframesByName, std::string_view xtDir,
             std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink,
             const TokenScope* tokens, LocalizationBuilder* l10n, const std::vector<StyleRule>* rules) {
    bool ok = true;
    // T-179: key -> 1-based index into l10n->keys.
    std::unordered_map<std::string, uint32_t> keysByName;
    std::unordered_map<std::string, AssetId> imageAssetsBySrc;
    // T-273d: the `<video src>` paths outside the project already warned about.
    std::unordered_set<std::string> outsideVideos;
    // §19.4 (T-47a): "同一父下第二台 -> 警告 only the first camera is used". Keyed by DOM parent
    // index; dom.nodes is depth-first preorder (parent before child), so by the time a second
    // <camera> sibling is visited its parent is already in this set.
    std::unordered_set<uint32_t> camerasSeenForParent;
    float nan = std::numeric_limits<float>::quiet_NaN();

    // `img src` (and T-178's `<img inline src>`) -> ASST Image entry, one per distinct src.
    auto imageAssetFor = [&](const std::string& src, int line, int col) -> AssetId {
        auto cached = imageAssetsBySrc.find(src);
        if (cached != imageAssetsBySrc.end()) {
            return cached->second;
        }
        // T-199: one shared registration (style_emit.cpp) for `<img src>`, `background-image` and
        // `border-image-source`, so a name used by several of them gets one ASST entry.
        AssetId assetId = ensureImageAsset(writer, src, xtDir, fileName, line, col, options, sink);
        imageAssetsBySrc.emplace(src, assetId);
        return assetId;
    };

    // T-273b (CompileOptions::publishAssetNames): the text an attribute of `node` is written as.
    // An image reference outside the project (`<img src>`, an atlas `<sequence src>`,
    // `<particles texture>`) is registered here, ahead of the element's own lookup below (which
    // finds the same entry), and written as its ASST name, so both share one StringId.
    auto attrText = [&](const DomNode& node, const DomAttr& attr) -> std::string {
        if (!options.publishAssetNames || !assetLeavesProject(attr.value, xtDir, options.rootDir)) {
            return attr.value;
        }
        // T-273c: a frame-folder `<sequence src>` names no one asset (its frames are ASST entries
        // of their own), and an `<anim src>` / `<img src="*.svg">` is compiled into a scene (whose
        // name is the same text): written as its last path element.
        if ((node.tag == "sequence" && attr.name == "src" && node.attr("frame-size") == nullptr) ||
            (node.tag == "anim" && attr.name == "src") ||
            (node.tag == "img" && attr.name == "src" && sceneBySrc.find(attr.value) != sceneBySrc.end())) {
            return publishedReference(attr.value, xtDir, options);
        }
        AssetId assetId = kInvalidAsset;
        if ((node.tag == "img" && attr.name == "src" && sceneBySrc.find(attr.value) == sceneBySrc.end()) ||
            (node.tag == "sequence" && attr.name == "src" && node.attr("frame-size") != nullptr)) {
            assetId = imageAssetFor(attr.value, node.line, node.col);
        } else if (node.tag == "particles" && attr.name == "texture") {
            assetId = imageAssetFor(attr.value, attr.line, attr.col);
        }
        if (assetId == kInvalidAsset || !writer.assets()[assetId].referencedByName) {
            return attr.value;
        }
        return std::string(writer.stringAt(writer.assets()[assetId].name));
    };

    // T-178 (07.8): one text's rich-text runs -> DOCT run table (+ inline objects, links).
    static const TokenScope kNoRunTokens;
    auto emitTextRuns = [&](const DomNode& node, uint32_t textIndex) {
        const uint32_t linkBase = static_cast<uint32_t>(doc.textLinks.size());
        for (const std::string& onClick : node.textLinks) {
            doc.textLinks.push_back(IrTextLink{onClick.empty() ? kInvalidString : writer.intern(onClick)});
        }
        for (const DomTextRun& src : node.textRuns) {
            IrTextRun run{};
            run.text = textIndex;
            run.start = src.start;
            run.end = src.end;
            run.declOffset = static_cast<uint32_t>(doc.inlineDecls.size());
            run.declCount = 0;
            run.inlineObject = 0xFFFFFFFFu;
            run.link = src.link >= 0 ? linkBase + static_cast<uint32_t>(src.link) : 0xFFFFFFFFu;

            std::vector<StyleDecl> decls;
            for (const StyleDecl* d : classRuleDecls(rules, src.classes)) {
                decls.push_back(*d);
            }
            if (!src.style.empty()) {
                DiagnosticSink localSink;
                std::vector<StyleDecl> inlineDecls;
                if (!parseDeclarations(src.style, fileName, src.line, localSink, inlineDecls)) {
                    sink.warn(std::string(fileName), "invalid inline style in rich text, ignored", src.line,
                              src.col);
                } else {
                    decls.insert(decls.end(), inlineDecls.begin(), inlineDecls.end());
                }
            }
            resolveVarDecls(decls, tokens != nullptr ? *tokens : kNoRunTokens, /*keepRefs=*/false, fileName,
                            sink);
            bool dropped = false;
            for (const StyleDecl& d : decls) {
                if (!isRunOverrideProp(d.prop)) {
                    // T-324: the `text-decoration` shorthand's `auto` thickness is its reset, not
                    // something the author wrote; it goes without a warning.
                    const bool shorthandReset = d.prop == xt::prop::TextDecorationThickness &&
                                                d.value.kind == StyleValueKind::Length &&
                                                d.value.len.unit == LengthUnit::Auto;
                    dropped = dropped || !shorthandReset;
                    continue;
                }
                IrDecl decl{};
                if (!encodeDecl(writer, style, keyframesByName, d, fileName, sink, decl)) {
                    ok = false;
                    continue;
                }
                doc.inlineDecls.push_back(decl);
                ++run.declCount;
            }
            if (dropped) {
                sink.warn(
                    std::string(fileName),
                    "rich-text runs only take color, font-size, font-weight, font-style, letter-spacing, "
                    "text-effect, text-decoration-line, text-decoration-color and font-variant-numeric; "
                    "other "
                    "declarations ignored",
                    src.line, src.col);
            }

            if (src.inlineKind != DomInlineKind::None) {
                IrTextInline object{};
                float height = 0;
                if (!src.inlineHeight.empty()) {
                    std::string h = src.inlineHeight;
                    if (h.size() > 2 && h.compare(h.size() - 2, 2, "px") == 0) {
                        h.resize(h.size() - 2);
                    }
                    if (!parseFloatAttr(h, height) || height < 0.0f) {
                        sink.warn(std::string(fileName), "invalid \"height\" for an inline object, ignored",
                                  src.line, src.col);
                        height = 0;
                    }
                }
                object.height = height;
                if (src.inlineKind == DomInlineKind::Glyph) {
                    object.kind = 1;
                    object.ref = writer.intern(src.inlineRef);
                } else {
                    object.kind = 2;
                    object.ref = src.inlineRef.empty() ? kInvalidAsset
                                                       : imageAssetFor(src.inlineRef, src.line, src.col);
                    if (src.inlineRef.empty()) {
                        sink.warn(std::string(fileName), "<img inline> without src", src.line, src.col);
                    }
                }
                run.inlineObject = static_cast<uint32_t>(doc.textInlines.size());
                doc.textInlines.push_back(object);
            }
            doc.textRuns.push_back(run);
        }
    };

    doc.elements.resize(dom.nodes.size());

    for (uint32_t i = 0; i < dom.nodes.size(); ++i) {
        const DomNode& node = dom.nodes[i];
        IrDocElement el{};
        el.parent = node.parent;
        el.tag = writer.intern(node.tag);
        el.id = kInvalidString;
        el.classOffset = el.classCount = 0;
        el.attrOffset = el.attrCount = 0;
        el.inlineDeclOffset = el.inlineDeclCount = 0;
        el.scene = kInvalidScene;
        el.firstTextChild = kInvalidNode;
        el.cameraIndex = kInvalidNode;

        if (node.tag == "particles") {
            // T-202 (03 §3.12.5, 05 §5.9.1): the numeric attributes are the element's first inline
            // declarations (`rate="30"` = `particle-rate: 30`), so `style` (appended after them)
            // and run-time declarations win, and `@timeline` / `bind:` address the same PropIds.
            std::string text;
            int declLine = node.line;
            for (const DomAttr& attr : node.attrs) {
                if (isParticleValueAttr(attr.name)) {
                    text += "particle-" + attr.name + ": " + attr.value + ";";
                    declLine = attr.line;
                }
            }
            if (!text.empty()) {
                DiagnosticSink localSink;
                std::vector<StyleDecl> decls;
                if (!parseDeclarations(text, fileName, declLine, localSink, decls) ||
                    !localSink.all().empty()) {
                    sink.error(std::string(fileName), "invalid <particles> attribute value", node.line,
                               node.col);
                    ok = false;
                } else {
                    el.inlineDeclOffset = static_cast<uint32_t>(doc.inlineDecls.size());
                    for (const StyleDecl& d : decls) {
                        IrDecl decl{};
                        if (!encodeDecl(writer, style, keyframesByName, d, fileName, sink, decl)) {
                            ok = false;
                            continue;
                        }
                        doc.inlineDecls.push_back(decl);
                        ++el.inlineDeclCount;
                    }
                }
            }
        }

        for (const DomAttr& attr : node.attrs) {
            if (attr.name == "id") {
                el.id = writer.intern(attr.value);
            } else if (attr.name == "class") {
                std::vector<std::string> classes = splitClassList(attr.value);
                el.classOffset = static_cast<uint32_t>(doc.classPool.size());
                for (const std::string& c : classes) {
                    doc.classPool.push_back(writer.intern(c));
                }
                el.classCount = static_cast<uint32_t>(classes.size());
            } else if (attr.name == "style") {
                // A malformed inline style degrades to a warning and is entirely discarded (not
                // partially applied); the underlying parse errors go into a throwaway sink so they
                // don't fail the whole document (docs/tasks/T-23.md "失敗 -> warning 丟棄").
                DiagnosticSink localSink;
                std::vector<StyleDecl> decls;
                // T-335: a `linear()` easing's points go straight into the STYL pool.
                bool declsOk =
                    parseDeclarations(attr.value, fileName, attr.line, localSink, decls, &style.easingPoints);
                if (!declsOk) {
                    sink.warn(std::string(fileName), "invalid inline style, ignored", attr.line, attr.col);
                } else {
                    // T-170: `var()` in a `style` attribute resolves against the document's tokens.
                    static const TokenScope kNoTokens;
                    resolveVarDecls(decls, tokens != nullptr ? *tokens : kNoTokens, /*keepRefs=*/true,
                                    fileName, sink);
                    // T-203: `custom()` in a `style` attribute (18.7).
                    if (!resolveCustomEffects(decls, writer, xtDir, fileName, options, sink)) {
                        ok = false;
                    }
                    if (el.inlineDeclCount == 0) {  // T-202: after a `<particles>` element's own.
                        el.inlineDeclOffset = static_cast<uint32_t>(doc.inlineDecls.size());
                    }
                    for (const StyleDecl& d : decls) {
                        if ((d.prop == styleprop::BorderImageSource ||
                             d.prop == styleprop::BackgroundImage) &&
                            d.value.kind == StyleValueKind::String && d.value.str != "none") {
                            imageAssetFor(d.value.str, attr.line, attr.col);  // T-199: its ASST entry.
                        }
                        IrDecl decl{};
                        if (!encodeDecl(writer, style, keyframesByName, d, fileName, sink, decl)) {
                            ok = false;
                            continue;
                        }
                        doc.inlineDecls.push_back(decl);
                        ++el.inlineDeclCount;
                    }
                }
            } else if (l10n != nullptr && isLocalizableAttr(attr.name)) {
                // T-179 (07.8): `placeholder`/`alt`/`title` may name a localization key.
                std::string value;
                LocalizedText kind = parseLocalizedText(attr.value, value);
                uint32_t key = 0;
                if (kind == LocalizedText::Invalid) {
                    sink.error(std::string(fileName), invalidKeyMessage(attr.value), attr.line, attr.col);
                    ok = false;
                } else if (kind == LocalizedText::Key) {
                    key = keyIndex(writer, *l10n, keysByName, value);
                }
                pushAttr(doc, el, writer.intern(attr.name), writer.intern(value));
                l10n->attrKeys.resize(doc.attrPool.size(), 0);
                l10n->attrKeys.back() = key;
            } else if (attr.name.rfind("nav:", 0) == 0) {
                // T-194 (05.9.1): `nav:<dir>="#id"` is stored as the bare id, so the runtime
                // compares it with element ids by StringId.
                std::string_view target = attr.value;
                while (!target.empty() && (target.front() == ' ' || target.front() == '#')) {
                    target.remove_prefix(1);
                }
                while (!target.empty() && target.back() == ' ') {
                    target.remove_suffix(1);
                }
                pushAttr(doc, el, writer.intern(attr.name), writer.intern(target));
            } else if (attr.name == "key-activation" && node.tag == "body") {
                // T-379 (08.10): the document's keyboard activation switch, kept as a `<body>`
                // attribute (Document::keyboardActivation() reads it at load).
                if (attr.value != "auto" && attr.value != "none") {
                    sink.error(std::string(fileName),
                               "invalid key-activation \"" + attr.value + "\" (expected auto or none)",
                               attr.line, attr.col);
                    ok = false;
                }
                pushAttr(doc, el, writer.intern(attr.name), writer.intern(attr.value));
            } else {
                const std::string text = attrText(node, attr);  // T-273b
                pushAttr(doc, el, writer.intern(attr.name), writer.intern(text));
            }
        }

        if (node.tag == "anim") {
            const DomAttr* srcAttr = node.attr("src");
            if (srcAttr != nullptr) {
                auto it = sceneBySrc.find(srcAttr->value);
                if (it != sceneBySrc.end()) {
                    el.scene = it->second;
                }
                // Else: step 3 already reported an error for this src; leave kInvalidScene.
            }
        } else if (node.tag == "img") {
            const DomAttr* srcAttr = node.attr("src");
            auto svgScene = srcAttr != nullptr ? sceneBySrc.find(srcAttr->value) : sceneBySrc.end();
            if (svgScene != sceneBySrc.end()) {
                // T-110: `<img src="*.svg">` references a static scene exactly like `<anim>` does,
                // and carries no image asset (its intrinsic size comes from the scene, 12.2.1).
                el.scene = svgScene->second;
            } else if (srcAttr != nullptr) {
                AssetId assetId = imageAssetFor(srcAttr->value, node.line, node.col);
                // data-xt-asset = decimal AssetId, so a runtime can look it up before T-27 adds a
                // dedicated field (docs/tasks/T-23.md).
                pushAttr(doc, el, writer.intern("data-xt-asset"), writer.intern(std::to_string(assetId)));
            }
        } else if (node.tag == "video" || node.tag == "sequence" || node.tag == "external") {
            // T-191 (25.3.5b, 05 §5.9.1): external content. Drawn like an `<img>` whose image is
            // an `engine://` texture the host binds (video, scene view, engine/host texture) or
            // the current frame of an image sequence the runtime plays itself.
            const std::string file(fileName);
            if (!node.children.empty() || node.hasText) {
                sink.error(file, "<" + node.tag + "> must not have children", node.line, node.col);
                ok = false;
            }
            auto positiveAttr = [&](const char* name) {
                const DomAttr* a = node.attr(name);
                float v = 0;
                if (a != nullptr && (!parseFloatAttr(a->value, v) || !(v > 0.0f))) {
                    sink.warn(file, std::string("invalid \"") + name + "\" for <" + node.tag + ">, ignored",
                              a->line, a->col);
                }
            };
            positiveAttr("rate");
            const DomAttr* idAttr = node.attr("id");
            const DomAttr* srcAttr = node.attr("src");
            if (node.tag == "external") {
                const DomAttr* sourceAttr = node.attr("source");
                const std::string source = sourceAttr != nullptr ? sourceAttr->value : std::string();
                std::string assetName;
                if (isExternalAssetName(source) && source.size() > kExternalAssetScheme.size()) {
                    assetName = source;  // The same binding as `<img src="engine://...">`.
                } else if ((source.rfind("scene-view:", 0) == 0 && source.size() > 11) ||
                           (source.rfind("host:", 0) == 0 && source.size() > 5)) {
                    assetName = std::string(kExternalAssetScheme) + source;
                } else {
                    sink.error(file,
                               "<external> source must be scene-view:<camera>, engine://<name> or "
                               "host:<name>",
                               node.line, node.col);
                    ok = false;
                }
                if (!assetName.empty()) {
                    AssetId assetId = imageAssetFor(assetName, node.line, node.col);
                    pushAttr(doc, el, writer.intern("data-xt-asset"), writer.intern(std::to_string(assetId)));
                }
            } else if (srcAttr == nullptr || srcAttr->value.empty()) {
                sink.error(file, "<" + node.tag + "> without src", node.line, node.col);
                ok = false;
            } else if (node.tag == "video") {
                // The host binds the decoded frames to this element's own `engine://video:<id>`
                // texture (by src when the element has no id).
                const std::string key =
                    idAttr != nullptr && !idAttr->value.empty() ? idAttr->value : srcAttr->value;
                AssetId assetId =
                    imageAssetFor(std::string(kExternalAssetScheme) + "video:" + key, node.line, node.col);
                pushAttr(doc, el, writer.intern("data-xt-asset"), writer.intern(std::to_string(assetId)));
                // T-273d: a published build does not carry video files, so an outside `src` (and
                // the key above, when it holds the same path) stays as written: say so, once.
                if (options.publishAssetNames && assetLeavesProject(srcAttr->value, xtDir, options.rootDir) &&
                    outsideVideos.insert(srcAttr->value).second) {
                    sink.warn(std::string(fileName),
                              "video \"" + srcAttr->value + std::string(kVideoOutsideProjectTail),
                              srcAttr->line, srcAttr->col);
                }
            } else {
                positiveAttr("fps");
                const DomAttr* frameSizeAttr = node.attr("frame-size");
                if (frameSizeAttr != nullptr) {
                    // An atlas: one image, frames cut out of it row-major.
                    float fw = 0;
                    float fh = 0;
                    if (!parseFrameSize(frameSizeAttr->value, fw, fh)) {
                        sink.error(file, "invalid \"frame-size\" for <sequence>, expected WxH",
                                   frameSizeAttr->line, frameSizeAttr->col);
                        ok = false;
                    }
                    positiveAttr("frames");
                    AssetId assetId = imageAssetFor(srcAttr->value, node.line, node.col);
                    pushAttr(doc, el, writer.intern("data-xt-asset"), writer.intern(std::to_string(assetId)));
                } else {
                    const std::vector<std::string> frames =
                        sequenceFrameNames(options, xtDir, srcAttr->value);
                    if (frames.empty()) {
                        sink.error(file, "sequence src \"" + srcAttr->value + "\" has no frames", node.line,
                                   node.col);
                        ok = false;
                    } else {
                        std::string ids;
                        for (const std::string& frame : frames) {
                            if (!ids.empty()) {
                                ids += ',';
                            }
                            ids += std::to_string(imageAssetFor(frame, node.line, node.col));
                        }
                        pushAttr(doc, el, writer.intern("data-xt-frames"), writer.intern(ids));
                    }
                }
            }
        } else if (node.tag == "svg") {
            // T-190 (11.9): an inline <svg> references its own scene (step 3b of compileDocument).
            auto svgScene = sceneBySrc.find(inlineSvgSceneKey(i));
            if (svgScene != sceneBySrc.end()) {
                el.scene = svgScene->second;
            }
        } else if (node.tag == "particles") {
            // T-202 (03 §3.12.5, 05 §5.9.1): an emitter; its box is the emitter area. The keyword
            // attributes stay attributes (read at load time); the texture gets an ASST entry the
            // runtime finds through `data-xt-texture` (not `data-xt-asset`: the element draws no
            // image and has no intrinsic size).
            const std::string file(fileName);
            if (!node.children.empty() || node.hasText) {
                sink.error(file, "<particles> must not have children", node.line, node.col);
                ok = false;
            }
            auto keywordAttr = [&](const char* name, std::initializer_list<const char*> allowed) {
                const DomAttr* a = node.attr(name);
                if (a == nullptr) {
                    return;
                }
                for (const char* v : allowed) {
                    if (a->value == v) {
                        return;
                    }
                }
                sink.error(file,
                           std::string("invalid \"") + name + "\" for <particles>: \"" + a->value + "\"",
                           a->line, a->col);
                ok = false;
            };
            keywordAttr("shape", {"point", "rect", "circle", "edge"});
            keywordAttr("blend", {"normal", "additive"});
            keywordAttr("simulate-in-editor", {"true", "false"});
            if (const DomAttr* tex = node.attr("texture"); tex != nullptr && !tex->value.empty()) {
                AssetId assetId = imageAssetFor(tex->value, tex->line, tex->col);
                pushAttr(doc, el, writer.intern("data-xt-texture"), writer.intern(std::to_string(assetId)));
            }
        } else if (node.tag == "camera") {
            // §19.4 (T-47a): "<camera> 不得有子節點".
            if (!node.children.empty()) {
                sink.error(std::string(fileName), "<camera> must not have children", node.line, node.col);
                ok = false;
            }
            if (!camerasSeenForParent.insert(node.parent).second) {
                sink.warn(std::string(fileName), "only the first camera is used", node.line, node.col);
            } else {
                IrCameraData cam{};
                cam.kind = 0;       // perspective (default).
                cam.useTarget = 1;  // <camera> exposes position/target only, never rotation (19.4).
                cam.zoom = nan;
                cam.position = Vec3{nan, nan, nan};
                cam.target = Vec3{nan, nan, nan};
                cam.nearZ = nan;
                cam.farZ = nan;

                const DomAttr* typeAttr = node.attr("type");
                if (typeAttr != nullptr) {
                    if (typeAttr->value == "perspective") {
                        cam.kind = 0;
                    } else if (typeAttr->value == "orthographic") {
                        cam.kind = 1;
                    } else {
                        sink.warn(std::string(fileName),
                                  "invalid \"type\" for <camera>, using \"perspective\"", typeAttr->line,
                                  typeAttr->col);
                    }
                }
                const DomAttr* positionAttr = node.attr("position");
                if (positionAttr != nullptr) {
                    Vec3 pos{};
                    if (parseVec3Attr(positionAttr->value, /*allowOmitZ=*/true, pos)) {
                        cam.position = pos;
                    } else {
                        sink.warn(std::string(fileName), "invalid \"position\" for <camera>, ignored",
                                  positionAttr->line, positionAttr->col);
                    }
                }
                const DomAttr* targetAttr = node.attr("target");
                if (targetAttr != nullptr) {
                    Vec3 tgt{};
                    if (parseVec3Attr(targetAttr->value, /*allowOmitZ=*/false, tgt)) {
                        cam.target = tgt;
                    } else {
                        sink.warn(std::string(fileName), "invalid \"target\" for <camera>, ignored",
                                  targetAttr->line, targetAttr->col);
                    }
                }
                const DomAttr* zoomAttr = node.attr("zoom");
                if (zoomAttr != nullptr) {
                    float v = 0;
                    if (parseFloatAttr(zoomAttr->value, v)) {
                        cam.zoom = v;
                    } else {
                        sink.warn(std::string(fileName), "invalid \"zoom\" for <camera>, ignored",
                                  zoomAttr->line, zoomAttr->col);
                    }
                }
                const DomAttr* nearAttr = node.attr("near");
                if (nearAttr != nullptr) {
                    float v = 0;
                    if (parseFloatAttr(nearAttr->value, v)) {
                        cam.nearZ = v;
                    } else {
                        sink.warn(std::string(fileName), "invalid \"near\" for <camera>, ignored",
                                  nearAttr->line, nearAttr->col);
                    }
                }
                const DomAttr* farAttr = node.attr("far");
                if (farAttr != nullptr) {
                    float v = 0;
                    if (parseFloatAttr(farAttr->value, v)) {
                        cam.farZ = v;
                    } else {
                        sink.warn(std::string(fileName), "invalid \"far\" for <camera>, ignored",
                                  farAttr->line, farAttr->col);
                    }
                }
                el.cameraIndex = writer.addDocCamera(doc, cam);
            }
        }

        if (node.hasText) {
            el.firstTextChild = static_cast<uint32_t>(doc.texts.size());
            IrDocText text{};
            text.parent = i;
            uint32_t key = 0;
            if (l10n != nullptr) {
                // T-179 (07.8): `@key` text. An editable `<input>`'s text is its initial value,
                // which the runtime never re-resolves, so a key there stays literal text.
                std::string value;
                LocalizedText kind = parseLocalizedText(node.text, value);
                if (kind == LocalizedText::Invalid) {
                    sink.error(std::string(fileName), invalidKeyMessage(node.text), node.line, node.col);
                    ok = false;
                } else if (kind == LocalizedText::Key && node.tag == "input") {
                    sink.warn(std::string(fileName), "localization key in an <input>'s text is kept as text",
                              node.line, node.col);
                    value = node.text;
                } else if (kind == LocalizedText::Key) {
                    key = keyIndex(writer, *l10n, keysByName, value);
                    if (!node.textRuns.empty()) {
                        // T-187: runs are byte offsets into the source text, not the translation.
                        sink.warn(std::string(fileName), "translation invalidates run offsets", node.line,
                                  node.col);
                    }
                }
                text.text = writer.intern(value);
            } else {
                text.text = writer.intern(node.text);
            }
            doc.texts.push_back(text);
            if (l10n != nullptr) {
                l10n->textKeys.push_back(key);
            }
            if (!node.textRuns.empty()) {
                emitTextRuns(node, el.firstTextChild);  // T-178.
            }
        }

        doc.elements[i] = el;
    }

    if (l10n != nullptr) {
        if (l10n->keys.empty()) {
            l10n->textKeys.clear();
            l10n->attrKeys.clear();
        } else {
            l10n->attrKeys.resize(doc.attrPool.size(), 0);
        }
    }
    return ok;
}

}  // namespace xt::compile
