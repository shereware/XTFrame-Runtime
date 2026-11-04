#include "section_sources.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/parse_xts.h"
#include "xt/compile/utf8_path.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.3, §54.5, §54.6, §54.8). See section_sources.h.

namespace xt::compile {

namespace {

/// One section in merge order.
struct OrderedSection {
    const XtSheetSection* section = nullptr;
    XtsSectionMode mode = XtsSectionMode::Full;
};

bool endsWithXts(std::string_view path) { return path.size() >= 4 && path.substr(path.size() - 4) == ".xts"; }

/// The `<symbol>` that encloses node `index` (its `name`), or "".
std::string enclosingSymbolName(const DomTree& tree, uint32_t index) {
    for (uint32_t p = tree.nodes[index].parent; p != 0xFFFFFFFFu && p < tree.nodes.size();
         p = tree.nodes[p].parent) {
        if (tree.nodes[p].tag == "symbol") {
            const DomAttr* name = tree.nodes[p].attr("name");
            return name != nullptr ? name->value : std::string();
        }
    }
    return {};
}

/// The layout lints over the rules `[begin, end)` that layout section `sectionIndex` produced.
void lintLayoutRules(const RuleSet& ruleSet, size_t begin, size_t end, size_t sectionIndex,
                     bool lintSubjectId, std::map<std::string, size_t>& subjects, DiagnosticSink& sink) {
    for (size_t r = begin; r < end; ++r) {
        const StyleRule& rule = ruleSet.rules[r];
        for (const Selector& selector : rule.selectors) {
            const std::string id = selector.parts.empty() ? std::string() : selector.parts[0].id;
            if (id.empty()) {
                if (lintSubjectId) {
                    sink.warn(rule.file, "the subject of this layout rule is not an #id", rule.line, 0,
                              "layout.subjectNotId",
                              "write the geometry in a rule whose subject is the element's #id");
                }
                continue;
            }
            const std::string key = rule.symbol + "|" + id;
            auto [it, inserted] = subjects.emplace(key, sectionIndex);
            if (!inserted && it->second != sectionIndex && it->second != static_cast<size_t>(-1)) {
                it->second = static_cast<size_t>(-1);  // Reported once per subject.
                sink.warn(rule.file, "#" + id + " is the subject of rules in more than one layout section",
                          rule.line, 0, "layout.duplicateSubject",
                          "keep the geometry of #" + id + " in one <layout> section");
            }
        }
    }
}

}  // namespace

std::string joinSectionHref(std::string_view base, std::string_view href) {
    const std::filesystem::path rel = pathFromUtf8(href);
    if (base.empty() || rel.is_absolute()) {
        return std::string(href);
    }
    return genericPathToUtf8((pathFromUtf8(base) / rel).lexically_normal());
}

void parseSheetSections(const DomTree& tree, const SectionOwner& owner, const FragmentReader& read,
                        bool lintSubjectId, DiagnosticSink& sink, RuleSet& ruleSet) {
    std::vector<OrderedSection> sections;
    for (const XtSheetSection& s : tree.layoutSheets) {
        sections.push_back({&s, XtsSectionMode::LayoutOnly});
    }
    for (const XtSheetSection& s : tree.styleSheets) {
        sections.push_back({&s, XtsSectionMode::StyleOnly});
    }
    for (const XtSheetSection& s : tree.timelineSheets) {
        sections.push_back({&s, XtsSectionMode::TimelinesOnly});
    }
    std::stable_sort(sections.begin(), sections.end(), [](const OrderedSection& a, const OrderedSection& b) {
        return a.section->order < b.section->order;
    });
    std::map<std::string, size_t> subjects;
    for (size_t k = 0; k < sections.size(); ++k) {
        const XtSheetSection& section = *sections[k].section;
        const size_t rulesBefore = ruleSet.rules.size();
        if (!section.src.empty()) {
            const std::string href = joinSectionHref(owner.baseHref, section.src);
            if (!endsWithXts(href)) {
                sink.error(owner.fileName, "a section fragment must be a .xts file (\"" + section.src + "\")",
                           section.lineOffset, section.colOffset);
                continue;
            }
            std::string text;
            std::string dir;
            std::string message;
            if (!read(href, text, dir, message)) {
                sink.error(owner.fileName, message, section.lineOffset, section.colOffset);
                continue;
            }
            parseXtsSection(text, href, 1, sections[k].mode, sink, ruleSet, dir, section.symbol);
        } else {
            // File columns (54 §54.13): the first line is padded to the column it starts at.
            const std::string text =
                std::string(static_cast<size_t>(section.colOffset > 1 ? section.colOffset - 1 : 0), ' ') +
                section.text;
            parseXtsSection(text, owner.fileName, section.lineOffset, sections[k].mode, sink, ruleSet,
                            owner.inlineSheetDir, section.symbol);
        }
        if (sections[k].mode == XtsSectionMode::LayoutOnly) {
            lintLayoutRules(ruleSet, rulesBefore, ruleSet.rules.size(), k, lintSubjectId, subjects, sink);
        }
    }
}

void lowerPathTriggers(DomTree& tree, std::string_view fileName, DiagnosticSink& sink, RuleSet& ruleSet) {
    const std::string file(fileName);
    for (uint32_t index = 0; index < tree.nodes.size(); ++index) {
        std::vector<DomAttr>& attrs = tree.nodes[index].attrs;
        for (size_t a = 0; a < attrs.size();) {
            const DomAttr& attr = attrs[a];
            const size_t at = attr.name.find('@');
            if (attr.name.compare(0, 3, "on:") != 0 || at == std::string::npos) {
                ++a;
                continue;
            }
            const std::string event = attr.name.substr(3, at - 3);
            const std::string path = attr.name.substr(at + 1);
            const DomAttr* id = tree.nodes[index].attr("id");
            RuleSet parsed;
            DiagnosticSink scratch;
            if (tree.nodes[index].tag != "use" || id == nullptr || id->value.empty()) {
                sink.error(file,
                           "\"" + attr.name + "\": on:<event>@<path> is only valid on a <use> with an id",
                           attr.line, attr.col);
            } else if (event.empty() || path.empty() ||
                       !parseXts("#" + id->value + "/" + path + " {}", file, attr.line, scratch, parsed) ||
                       parsed.rules.size() != 1) {
                sink.error(file, "\"" + attr.name + "\": invalid on:<event>@<path>", attr.line, attr.col);
            } else {
                StyleRule rule = std::move(parsed.rules.front());
                rule.triggers.push_back(TriggerDecl{event, attr.value, attr.line});
                rule.line = attr.line;
                rule.file = file;
                rule.symbol = enclosingSymbolName(tree, index);
                ruleSet.rules.push_back(std::move(rule));
            }
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(a));
        }
    }
}

}  // namespace xt::compile
