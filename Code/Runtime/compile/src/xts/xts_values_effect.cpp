#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "css_values.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/style_model.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): effect and text-effect. T-413:
/// the built-in effect functions are core's css_values (18.3-18.4); this file keeps the sequence
/// rules with their diagnostics, custom() (compile-time only) and text-effect (07 §7.8).

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace xts_values_detail {

// `text-effect` (Docs/design/07-text.md §7.8, T-178): a comma-separated list of
// `outline <w> <color>` (-> Outline, params[0] = w), `glow <r> <color>` (-> Glow, params[0] = r,
// params[1] = intensity 1) and `shadow <dx> <dy> <r> <color>` (-> DropShadow, params[0..2] =
// dx, dy, r, params[4] = opacity 1), or `none`. Lengths are px/em (effect rule above). Radii
// above 8 px are clamped with a warning (07.8's glyph-level cost cap). Any other shape: a warning
// and the declaration is dropped.
bool parseTextEffectList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<EffectDeclEntry>& out) {
    if (toks.size() == 1 && isKeyword(toks[0], "none")) {
        return true;
    }
    auto invalid = [&]() {
        sink.warn(std::string(fileName), "invalid value for \"text-effect\"", line);
        return false;
    };
    constexpr float kMaxTextEffectRadius = 8.0f;
    bool clamped = false;
    auto clampRadius = [&](float& r) {
        if (r > kMaxTextEffectRadius) {
            r = kMaxTextEffectRadius;
            clamped = true;
        }
    };
    for (const std::vector<Tok>& item : splitByCommaTopLevel(toks)) {
        Cursor cur(item);
        if (cur.atEnd() || cur.peek().kind != TokKind::Ident) {
            return invalid();
        }
        const Tok& kindTok = cur.advance();
        EffectDeclEntry entry;
        bool percentError = false;
        float a = 0, b = 0, r = 0;
        const bool isOutline = isKeyword(kindTok, "outline");
        const bool isGlow = isKeyword(kindTok, "glow");
        if (isOutline || isGlow) {
            if (!css_values::readEffectLen(cur, kCompiler, r, percentError) || r < 0.0f) {
                return invalid();
            }
            clampRadius(r);
            entry.kind = isOutline ? EffectKind::Outline : EffectKind::Glow;
            entry.params[0] = r;
            if (isGlow) {
                entry.params[1] = 1;  // intensity (18.3's default).
            }
        } else if (isKeyword(kindTok, "shadow")) {
            if (!css_values::readEffectLen(cur, kCompiler, a, percentError) ||
                !css_values::readEffectLen(cur, kCompiler, b, percentError) ||
                !css_values::readEffectLen(cur, kCompiler, r, percentError) || r < 0.0f) {
                return invalid();
            }
            clampRadius(r);
            entry.kind = EffectKind::DropShadow;
            entry.params[0] = a;
            entry.params[1] = b;
            entry.params[2] = r;
            entry.params[4] = 1;  // opacity (18.3's default).
        } else {
            return invalid();
        }
        Color color;
        if (!parseColorCursor(cur, color) || !cur.atEnd()) {
            return invalid();
        }
        entry.colors[0] = color;
        out.push_back(std::move(entry));
        if (out.size() > css_values::kMaxEffects) {
            sink.error(std::string(fileName), "text-effect must have at most 16 entries", line);
            return false;
        }
    }
    if (clamped) {
        sink.warn(std::string(fileName), "text-effect radius clamped to 8px", line);
    }
    return true;
}

}  // namespace xts_values_detail

namespace {

// custom( "<asset-name>" [, <ident> <value>]* ) -> Custom (T-203, Docs/design/18-effects.md §18.4).
// `<value>` is one number (float), two numbers (vec2) or a color; the parameter's declared type is
// checked by style_emit.cpp against the asset's header. Every problem is an error (not the generic
// invalid-value warning), since a custom effect that silently drops out is hard to diagnose.
bool parseCustomFn(const std::vector<Tok>& inner, std::string_view fileName, int line, DiagnosticSink& sink,
                   EffectDeclEntry& out) {
    Cursor cur(inner);
    if (cur.peek().kind != TokKind::String || cur.peek().text.empty()) {
        sink.error(std::string(fileName),
                   "custom(): the first argument must be the .xtfx asset name in quotes", line);
        return false;
    }
    out.kind = EffectKind::Custom;
    out.customAsset = std::string(cur.advance().text);
    while (!cur.atEnd()) {
        if (cur.peek().kind != TokKind::Comma) {
            sink.error(std::string(fileName), "custom(): expected ',' before a parameter override", line);
            return false;
        }
        cur.advance();
        if (cur.peek().kind != TokKind::Ident) {
            sink.error(std::string(fileName), "custom(): expected '<param> <value>' after ','", line);
            return false;
        }
        CustomParamOverride ov;
        ov.name = std::string(cur.advance().text);
        ov.line = line;
        for (const CustomParamOverride& prev : out.customParams) {
            if (prev.name == ov.name) {
                sink.error(std::string(fileName), "custom(): parameter '" + ov.name + "' set twice", line);
                return false;
            }
        }
        Color color;
        if (cur.peek().kind == TokKind::Hash || cur.peek().kind == TokKind::Ident) {
            if (!parseColorCursor(cur, color)) {
                sink.error(std::string(fileName), "custom(): bad value for parameter '" + ov.name + "'",
                           line);
                return false;
            }
            ov.value[0] = color.r;
            ov.value[1] = color.g;
            ov.value[2] = color.b;
            ov.value[3] = color.a;
            ov.count = 4;
            ov.isColor = true;
        } else {
            while (cur.peek().kind == TokKind::Number && ov.count < 2) {
                const Tok& t = cur.peek();
                if (!(t.unit.empty() || t.unit == "px")) {
                    sink.error(std::string(fileName),
                               "custom(): parameter '" + ov.name + "' takes plain numbers (or px)", line);
                    return false;
                }
                ov.value[ov.count++] = t.num;
                cur.advance();
            }
            if (ov.count == 0) {
                sink.error(std::string(fileName),
                           "custom(): expected a number or color for parameter '" + ov.name + "'", line);
                return false;
            }
        }
        out.customParams.push_back(std::move(ov));
    }
    return true;
}

}  // namespace

namespace xts_values_detail {

bool parseEffectList(const std::vector<Tok>& toks, std::string_view fileName, int line, DiagnosticSink& sink,
                     std::vector<EffectDeclEntry>& out) {
    if (toks.size() == 1 && isKeyword(toks[0], "none")) {
        return true;  // Empty sequence (18.4: decl exists, count 0).
    }
    Cursor cur(toks);
    if (cur.atEnd()) {
        return false;
    }
    std::string_view fnName;
    std::vector<Tok> inner;
    while (!cur.atEnd()) {
        const css_values::EffectCall call = css_values::nextEffectCall(cur, fnName, inner);
        if (call == css_values::EffectCall::NotAFunction) {
            sink.warn(std::string(fileName), "invalid value for \"effect\"", line);
            return false;
        }
        const std::string fn(fnName);
        if (call == css_values::EffectCall::Unterminated) {
            sink.warn(std::string(fileName), "invalid value for \"effect\": unterminated \"" + fn + "(\"",
                      line);
            return false;
        }
        EffectDeclEntry entry;
        bool percentError = false;
        size_t diagCountBefore = sink.all().size();
        bool ok;
        if (css_values::iequals(fn, "custom")) {
            // T-203 (18.4, 0.3): custom("<asset>" [, <ident> <value>]*). The asset is resolved
            // and its parameter table applied by style_emit.cpp; here only the shape is checked.
            if (!parseCustomFn(inner, fileName, line, sink, entry)) {
                return false;  // parseCustomFn reported the error.
            }
            ok = true;
        } else {
            // 18.6: BackdropBlur reads what was drawn below the node before its layer was opened,
            // so it is only meaningful as the stack's input stage.
            if (css_values::iequals(fn, "backdrop-blur") && !out.empty()) {
                sink.error(std::string(fileName), "backdrop-blur must come first", line);
                return false;
            }
            SinkDiag diag(sink, fileName, line);
            EffectInstance parsed;
            std::vector<GradientStop> stops;
            const css_values::EffectFnResult result =
                css_values::parseEffectFn(fn, inner, kCompiler, &diag, percentError, parsed, stops);
            if (result == css_values::EffectFnResult::Unknown) {
                sink.warn(std::string(fileName),
                          "invalid value for \"effect\": unknown function \"" + fn + "\"", line);
                return false;
            }
            ok = result == css_values::EffectFnResult::Ok;
            entry.kind = parsed.kind;
            std::copy(std::begin(parsed.params), std::end(parsed.params), std::begin(entry.params));
            entry.colors[0] = parsed.colors[0];
            entry.colors[1] = parsed.colors[1];
            entry.stops = std::move(stops);
        }
        if (!ok) {
            if (percentError) {
                sink.error(std::string(fileName), "effect lengths do not accept percent", line);
            } else if (sink.all().size() == diagCountBefore) {
                sink.warn(std::string(fileName),
                          "invalid value for \"effect\": bad arguments to \"" + fn + "\"", line);
            }
            return false;
        }
        out.push_back(std::move(entry));
        if (out.size() > css_values::kMaxEffects) {
            sink.error(std::string(fileName), "effect sequence must have at most 16 effects", line);
            return false;
        }
    }
    return true;
}

}  // namespace xts_values_detail

}  // namespace xt::compile
