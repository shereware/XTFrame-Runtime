#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_value.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xts_lexer.h"
#include "xts_props.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): the grid subset.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

// -------------------------------------------------------------------------------------------
// T-338 (06 §6.15): the grid subset.
// -------------------------------------------------------------------------------------------

/// One track: `<px> | <%> | <n>fr | auto` (a bare 0 is 0px).
bool parseGridTrackTok(const Tok& t, GridTrack& out, GridReject& why) {
    if (t.kind == TokKind::Ident) {
        if (t.text == "auto") {
            out = GridTrack{GridTrackKind::Auto, 0};
            return true;
        }
        if (t.text == "min-content" || t.text == "max-content" || t.text == "minmax" ||
            t.text == "fit-content" || t.text == "subgrid" || t.text == "masonry") {
            why = GridReject::Unsupported;
        }
        return false;
    }
    if (t.kind == TokKind::LBracket) {
        why = GridReject::Unsupported;  // A named line.
        return false;
    }
    if (t.kind != TokKind::Number || !std::isfinite(t.num) || t.num < 0.0f) {
        return false;
    }
    if (t.unit == "px" || (t.unit.empty() && t.num == 0.0f)) {
        out = GridTrack{GridTrackKind::Px, t.num};
    } else if (t.unit == "%") {
        out = GridTrack{GridTrackKind::Percent, t.num};
    } else if (t.unit == "fr") {
        out = GridTrack{GridTrackKind::Fr, t.num};
    } else {
        return false;
    }
    return true;
}

}  // namespace

namespace xts_values_detail {

/// `none | [<track> | repeat(<n>, <track>+)]+`, `repeat()` expanded; `none` is an empty list.
bool parseGridTrackList(const std::vector<Tok>& toks, std::vector<GridTrack>& out, GridReject& why) {
    why = GridReject::Invalid;
    out.clear();
    if (toks.size() == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        return true;
    }
    for (size_t i = 0; i < toks.size(); ++i) {
        const Tok& t = toks[i];
        if (t.kind == TokKind::Ident && i + 1 < toks.size() && toks[i + 1].kind == TokKind::LParen) {
            if (t.text != "repeat") {
                why = (t.text == "minmax" || t.text == "fit-content") ? GridReject::Unsupported : why;
                return false;
            }
            size_t close = i + 2;
            while (close < toks.size() && toks[close].kind != TokKind::RParen) {
                if (toks[close].kind == TokKind::LParen) {
                    why = GridReject::Unsupported;  // minmax() / fit-content() inside repeat().
                    return false;
                }
                ++close;
            }
            if (close >= toks.size() || close < i + 5 || toks[i + 3].kind != TokKind::Comma) {
                return false;
            }
            const Tok& count = toks[i + 2];
            if (count.kind == TokKind::Ident && (count.text == "auto-fill" || count.text == "auto-fit")) {
                why = GridReject::Unsupported;
                return false;
            }
            if (count.kind != TokKind::Number || !count.unit.empty() || !(count.num >= 1.0f) ||
                count.num > static_cast<float>(kMaxGridTracks) || std::trunc(count.num) != count.num) {
                return false;
            }
            std::vector<GridTrack> pattern;
            for (size_t k = i + 4; k < close; ++k) {
                GridTrack track;
                if (!parseGridTrackTok(toks[k], track, why)) {
                    return false;
                }
                pattern.push_back(track);
            }
            const size_t n = static_cast<size_t>(count.num);
            if (out.size() + n * pattern.size() > kMaxGridTracks) {
                return false;
            }
            for (size_t r = 0; r < n; ++r) {
                out.insert(out.end(), pattern.begin(), pattern.end());
            }
            i = close;
            continue;
        }
        GridTrack track;
        if (!parseGridTrackTok(t, track, why)) {
            return false;
        }
        out.push_back(track);
        if (out.size() > kMaxGridTracks) {
            return false;
        }
    }
    return !out.empty();
}

}  // namespace xts_values_detail

namespace {

/// An area name: CSS name code points (letters, digits, `-`, `_`, anything non-ASCII).
bool isGridAreaNameChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0 || c == '-' || c == '_';
}

}  // namespace

namespace xts_values_detail {

/// `none | <string>+`: one string per row, cells separated by whitespace; a run of `.` is an
/// empty cell. Every row must have the same cell count and every name must cover a rectangle,
/// or the whole value is rejected (06 §6.15).
bool parseGridAreasValue(const std::vector<Tok>& toks, StyleValue& out, GridReject& why) {
    why = GridReject::Invalid;
    out.kind = StyleValueKind::GridAreas;
    out.gridAreas.clear();
    out.gridAreaRows = 0;
    out.gridAreaCols = 0;
    if (toks.size() == 1 && toks[0].kind == TokKind::Ident && toks[0].text == "none") {
        return true;
    }
    if (toks.empty() || toks.size() > kMaxGridTracks) {
        return false;
    }
    std::vector<std::vector<std::string>> cells;
    for (const Tok& t : toks) {
        if (t.kind != TokKind::String) {
            return false;
        }
        std::vector<std::string> row;
        const std::string_view text = t.text;
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
                ++i;
                continue;
            }
            if (text[i] == '.') {
                while (i < text.size() && text[i] == '.') {
                    ++i;
                }
                if (i < text.size() && isGridAreaNameChar(text[i])) {
                    return false;  // `.a` is neither a null cell nor a name.
                }
                row.emplace_back();
                continue;
            }
            const size_t start = i;
            while (i < text.size() && isGridAreaNameChar(text[i])) {
                ++i;
            }
            if (i == start) {
                return false;  // A character that is neither a name nor whitespace nor `.`.
            }
            row.emplace_back(text.substr(start, i - start));
        }
        if (row.empty() || row.size() > kMaxGridTracks || (!cells.empty() && row.size() != cells[0].size())) {
            return false;
        }
        cells.push_back(std::move(row));
    }
    const size_t rows = cells.size();
    const size_t cols = cells[0].size();
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const std::string& name = cells[r][c];
            if (name.empty()) {
                continue;
            }
            const auto known = std::find_if(out.gridAreas.begin(), out.gridAreas.end(),
                                            [&name](const GridAreaDecl& a) { return a.name == name; });
            if (known != out.gridAreas.end()) {
                continue;  // Its first (top-left) cell built it; the check below covers the rest.
            }
            size_t c1 = c;
            while (c1 < cols && cells[r][c1] == name) {
                ++c1;
            }
            size_t r1 = r;
            while (r1 < rows && cells[r1][c] == name) {
                ++r1;
            }
            GridAreaDecl area;
            area.name = name;
            area.rowStart = static_cast<uint16_t>(r);
            area.rowEnd = static_cast<uint16_t>(r1);
            area.colStart = static_cast<uint16_t>(c);
            area.colEnd = static_cast<uint16_t>(c1);
            out.gridAreas.push_back(std::move(area));
        }
    }
    // Rectangle check: every cell holds the name of exactly the area that covers it.
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const std::string& name = cells[r][c];
            for (const GridAreaDecl& a : out.gridAreas) {
                const bool inside = r >= a.rowStart && r < a.rowEnd && c >= a.colStart && c < a.colEnd;
                if (inside != (name == a.name)) {
                    why = GridReject::NotRectangle;
                    out.gridAreas.clear();
                    return false;
                }
            }
        }
    }
    out.gridAreaRows = static_cast<uint16_t>(rows);
    out.gridAreaCols = static_cast<uint16_t>(cols);
    return true;
}

/// One grid line `toks[begin, end)`: `auto | <integer> | span <integer>` into the kGridLine*
/// encoding. Negative lines and named lines are outside the subset.
bool parseGridLine(const std::vector<Tok>& toks, size_t begin, size_t end, uint32_t& out, GridReject& why) {
    bool unsupported = false;
    const bool ok = css_values::parseGridLine(toks.data() + begin, end - begin, out, unsupported);
    if (unsupported) {
        why = GridReject::Unsupported;
    }
    return ok;
}

/// `grid-column` / `grid-row`: `<line> [/ <line>]` -> the start / end longhands (a missing end
/// is `auto`).
bool parseGridLineShorthand(const std::vector<Tok>& toks, uint16_t startProp, int line,
                            std::vector<StyleDecl>& out, GridReject& why) {
    size_t slash = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
        if (isSlash(toks[i])) {
            slash = i;
            break;
        }
    }
    uint32_t start = kGridLineAuto;
    uint32_t end = kGridLineAuto;
    if (slash == 0 || !parseGridLine(toks, 0, slash, start, why)) {
        return false;
    }
    if (slash < toks.size() &&
        (slash + 1 >= toks.size() || !parseGridLine(toks, slash + 1, toks.size(), end, why))) {
        return false;
    }
    out.push_back(enumDecl(startProp, start, line));
    out.push_back(enumDecl(static_cast<uint16_t>(startProp + 1), end, line));
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
