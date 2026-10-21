#include <cstdio>
#include <string>

#include "xt/layout.h"
#include "xt/node.h"

/// Test-only layout snapshot: one JSON-ish line per Element, preorder. See docs/tasks/T-25.md
/// "快照格式".

namespace xt {

namespace {

/// Appends `v` formatted like `%.2f` with trailing zeros (and a trailing '.') stripped, e.g.
/// 100 -> "100", 33.5 -> "33.5", 33.33 -> "33.33" (never trailing ".00"/".0").
void appendTrimmedFloat(std::string& out, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s(buf);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) {
            last -= 1;  // Strip the '.' too when nothing follows it.
        }
        s.erase(last + 1);
    }
    out += s;
}

/// Escapes `s` minimally for embedding in a JSON string literal (quote + backslash only; layout
/// snapshot tags/ids never contain control characters in this project's test fixtures).
void appendJsonString(std::string& out, std::string_view s) {
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
}

}  // namespace

std::string LayoutEngine::snapshot(const NodeArena& arena, NodeId root) const {
    std::string out;
    if (root == kInvalidNode || !arena.alive(root)) {
        return out;
    }
    arena.traversePreorder(root, [&](NodeId node) {
        if (arena.kind(node) != NodeKind::Element) {
            return true;
        }
        const ElementData& data = arena.element(node);
        out += "{\"tag\":\"";
        appendJsonString(out, string(data.tag));
        out += "\"";
        if (data.id != kInvalidString) {
            out += ",\"id\":\"";
            appendJsonString(out, string(data.id));
            out += "\"";
        }
        out += ",\"x\":";
        appendTrimmedFloat(out, data.box.x);
        out += ",\"y\":";
        appendTrimmedFloat(out, data.box.y);
        out += ",\"w\":";
        appendTrimmedFloat(out, data.box.width);
        out += ",\"h\":";
        appendTrimmedFloat(out, data.box.height);
        out += "}\n";
        return true;
    });
    return out;
}

}  // namespace xt
