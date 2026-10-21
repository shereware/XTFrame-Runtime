#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "color_math.h"
#include "design_internal.h"
#include "xt/bundle/design_system.h"

/// The semantic diff of two design systems (docs/design/35-design-system.md §35.6;
/// docs/tasks/T-305.md). Items come in a fixed order: design.yaml, groups, swatches, variables,
/// type roles, texts, decisions, references; within a kind by id; within an entry by field.

namespace xt::bundle::design {

namespace {

using Op = DiffItem::Op;

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item;
    }
    return out;
}

void fieldDesignDiff(std::vector<DiffItem>& out, const std::string& entry, const char* name,
                     const std::string& before, const std::string& after) {
    if (before != after) {
        DiffItem item;
        item.entry = entry;
        item.op = Op::Changed;
        item.field = name;
        item.before = before;
        item.after = after;
        out.push_back(std::move(item));
    }
}

void whole(std::vector<DiffItem>& out, const std::string& entry, Op op) {
    DiffItem item;
    item.entry = entry;
    item.op = op;
    out.push_back(std::move(item));
}

std::string linkKey(const Link& link) { return link.rel + " " + link.to; }

void linksDesignDiff(std::vector<DiffItem>& out, const std::string& entry, const std::vector<Link>& a,
                     const std::vector<Link>& b) {
    std::set<std::string> before;
    std::set<std::string> after;
    for (const Link& l : a) {
        before.insert(linkKey(l));
    }
    for (const Link& l : b) {
        after.insert(linkKey(l));
    }
    for (const std::string& key : before) {
        if (after.count(key) == 0) {
            DiffItem item;
            item.entry = entry;
            item.op = Op::Removed;
            item.field = "link";
            item.before = key;
            out.push_back(std::move(item));
        }
    }
    for (const std::string& key : after) {
        if (before.count(key) == 0) {
            DiffItem item;
            item.entry = entry;
            item.op = Op::Added;
            item.field = "link";
            item.after = key;
            out.push_back(std::move(item));
        }
    }
}

/// The paragraphs of a Markdown body: runs of non-blank lines, joined with '\n'.
std::vector<std::string> paragraphs(std::string_view body) {
    std::vector<std::string> out;
    std::string current;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        std::string_view line = body.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const bool blank = line.find_first_not_of(" \t") == std::string_view::npos;
        if (blank) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            if (!current.empty()) {
                current += '\n';
            }
            current += line;
        }
        if (end == body.size()) {
            break;
        }
        start = end + 1;
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

/// Paragraph-level diff through a longest common subsequence: a removed paragraph followed by an
/// added one is one Changed item.
void bodyDiff(std::vector<DiffItem>& out, const std::string& entry, std::string_view a, std::string_view b) {
    const std::vector<std::string> pa = paragraphs(a);
    const std::vector<std::string> pb = paragraphs(b);
    const size_t n = pa.size();
    const size_t m = pb.size();
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            lcs[i][j] = pa[i] == pb[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    std::vector<std::string> removed;
    std::vector<std::string> added;
    const auto flush = [&]() {
        const size_t common = std::min(removed.size(), added.size());
        for (size_t k = 0; k < common; ++k) {
            DiffItem item;
            item.entry = entry;
            item.op = Op::Changed;
            item.field = "paragraph";
            item.before = removed[k];
            item.after = added[k];
            out.push_back(std::move(item));
        }
        for (size_t k = common; k < removed.size(); ++k) {
            DiffItem item;
            item.entry = entry;
            item.op = Op::Removed;
            item.field = "paragraph";
            item.before = removed[k];
            out.push_back(std::move(item));
        }
        for (size_t k = common; k < added.size(); ++k) {
            DiffItem item;
            item.entry = entry;
            item.op = Op::Added;
            item.field = "paragraph";
            item.after = added[k];
            out.push_back(std::move(item));
        }
        removed.clear();
        added.clear();
    };
    size_t i = 0;
    size_t j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && pa[i] == pb[j]) {
            flush();
            ++i;
            ++j;
        } else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
            added.push_back(pb[j++]);
        } else {
            removed.push_back(pa[i++]);
        }
    }
    flush();
}

/// Runs `same(a, b)` for ids in both lists and Added/Removed items for the others, by sorted id.
template <typename T, typename Same>
void byIdDesignDiff(std::vector<DiffItem>& out, EntryKind kind, const std::vector<T>& a,
                    const std::vector<T>& b, Same same, int (*affected)(const std::string& ref, const Graph*),
                    const Graph* graph) {
    std::map<std::string, const T*> ma;
    std::map<std::string, const T*> mb;
    for (const T& x : a) {
        ma.emplace(x.base.id, &x);
    }
    for (const T& x : b) {
        mb.emplace(x.base.id, &x);
    }
    std::set<std::string> ids;
    for (const auto& [id, p] : ma) {
        (void)p;
        ids.insert(id);
    }
    for (const auto& [id, p] : mb) {
        (void)p;
        ids.insert(id);
    }
    for (const std::string& id : ids) {
        const std::string ref = entryRef(kind, id);
        const size_t first = out.size();
        const auto ia = ma.find(id);
        const auto ib = mb.find(id);
        if (ib == mb.end()) {
            whole(out, ref, Op::Removed);
        } else if (ia == ma.end()) {
            whole(out, ref, Op::Added);
        } else {
            same(ref, *ia->second, *ib->second);
        }
        if (affected != nullptr) {
            const int count = affected(ref, graph);
            for (size_t k = first; k < out.size(); ++k) {
                out[k].affectedUis = count;
            }
        }
    }
}

int affectedUis(const std::string& ref, const Graph* graph) {
    return graph == nullptr ? 0 : static_cast<int>(usage(*graph, ref).uis.size());
}

}  // namespace

std::vector<DiffItem> diff(const DesignSystem& a, const DesignSystem& b, const Graph* graphOfB) {
    std::vector<DiffItem> out;
    fieldDesignDiff(out, "design", "name", a.info.name, b.info.name);
    fieldDesignDiff(out, "design", "status", a.info.status, b.info.status);
    fieldDesignDiff(out, "design", "principles", join(a.info.principles), join(b.info.principles));

    byIdDesignDiff(
        out, EntryKind::Group, a.groups, b.groups,
        [&out](const std::string& ref, const Group& x, const Group& y) {
            fieldDesignDiff(out, ref, "name", x.name.empty() ? x.base.name : x.name,
                            y.name.empty() ? y.base.name : y.name);
            fieldDesignDiff(out, ref, "swatches", join(x.swatches), join(y.swatches));
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);

    byIdDesignDiff(
        out, EntryKind::Swatch, a.swatches, b.swatches,
        [&out](const std::string& ref, const Swatch& x, const Swatch& y) {
            if (x.value != y.value) {
                DiffItem item;
                item.entry = ref;
                item.op = Op::Changed;
                item.field = "value";
                item.before = x.value;
                item.after = y.value;
                double dE = 0.0;
                if (bundle::detail::hexDeltaE2000(x.value, y.value, &dE)) {
                    item.deltaE = static_cast<float>(dE);
                }
                out.push_back(std::move(item));
            }
            fieldDesignDiff(out, ref, "name", x.base.name, y.base.name);
            fieldDesignDiff(out, ref, "role", x.role, y.role);
            fieldDesignDiff(out, ref, "group", x.group, y.group);
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            fieldDesignDiff(out, ref, "note", x.base.note, y.base.note);
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);

    byIdDesignDiff(
        out, EntryKind::Token, a.tokens, b.tokens,
        [&out](const std::string& ref, const TokenMeta& x, const TokenMeta& y) {
            fieldDesignDiff(out, ref, "category", x.category, y.category);
            fieldDesignDiff(out, ref, "description", x.description, y.description);
            fieldDesignDiff(out, ref, "from", x.from, y.from);
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            fieldDesignDiff(out, ref, "note", x.base.note, y.base.note);
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        &affectedUis, graphOfB);

    byIdDesignDiff(
        out, EntryKind::TypeRole, a.typeRoles, b.typeRoles,
        [&out](const std::string& ref, const TypeRole& x, const TypeRole& y) {
            fieldDesignDiff(out, ref, "family", x.family, y.family);
            fieldDesignDiff(out, ref, "weights", join(x.weights), join(y.weights));
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);

    byIdDesignDiff(
        out, EntryKind::Text, a.texts, b.texts,
        [&out](const std::string& ref, const TextEntry& x, const TextEntry& y) {
            fieldDesignDiff(out, ref, "title", x.title, y.title);
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            fieldDesignDiff(out, ref, "section", join(x.sections), join(y.sections));
            bodyDiff(out, ref, x.body, y.body);
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);

    byIdDesignDiff(
        out, EntryKind::Decision, a.decisions, b.decisions,
        [&out](const std::string& ref, const TextEntry& x, const TextEntry& y) {
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            fieldDesignDiff(out, ref, "title", x.title, y.title);
            bodyDiff(out, ref, x.body, y.body);
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);

    byIdDesignDiff(
        out, EntryKind::Reference, a.references, b.references,
        [&out](const std::string& ref, const ReferenceMeta& x, const ReferenceMeta& y) {
            fieldDesignDiff(out, ref, "content", x.contentHash, y.contentHash);
            fieldDesignDiff(out, ref, "path", x.path, y.path);
            fieldDesignDiff(out, ref, "title", x.title, y.title);
            fieldDesignDiff(out, ref, "license", x.license, y.license);
            fieldDesignDiff(out, ref, "status", detail::statusText(x.base), detail::statusText(y.base));
            linksDesignDiff(out, ref, x.base.links, y.base.links);
        },
        nullptr, nullptr);
    return out;
}

}  // namespace xt::bundle::design
