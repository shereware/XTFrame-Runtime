#include <string>
#include <string_view>

#include "hlsl_subset.h"
#include "xt/compile/xtfx.h"

/// HLSL subset AST -> GLSL 330 module (docs/design/18-effects.md §18.7's translation table,
/// docs/tasks/T-203.md item 1):
///   float2/3/4 -> vec2/3/4          lerp -> mix            frac -> fract
///   saturate(x) -> clamp(x, 0.0, 1.0)   atan2(y, x) -> atan(y, x)
///   fmod(a, b) -> xtFmod(a, b) = a - b * trunc(a / b)   (helper overloads, HLSL semantics)
///   xtSample(uv) -> xtSample(uv), a helper around texture(uLayer, xtTexCoord(uv)) (the
///   backend's wrapper defines xtTexCoord: GL flips y for its bottom-up FBOs) that returns 0 outside
///   [0,1]^2 and un-premultiplies (the layer target is premultiplied; §18.7 says xtSample is
///   straight RGBA).
///   Integer literals in float context get ".0"; int variables get float(); a scalar where a
///   vector is needed gets vecN().

namespace xt::compile::xtfx {

namespace {

class Emitter {
public:
    Emitter(std::string& out) : m_out(out) {}

    void program(const Program& p, std::span<const XtfxParam> params) {
        m_out += "// Translated from the .xtfx HLSL subset (docs/design/18-effects.md 18.7).\n";
        m_out += "uniform sampler2D uLayer;\n";
        m_out += "uniform float xtTime;\n";
        m_out += "uniform vec2 uTexelSize;\n";
        m_out += "uniform vec2 uLayerSize;\n";
        for (const XtfxParam& prm : params) {
            m_out += "uniform ";
            m_out += prm.type == 0 ? "float " : (prm.type == 1 ? "vec2 " : "vec4 ");
            m_out += prm.name;
            m_out += ";\n";
        }
        m_out += "struct XtInput { vec2 uv; vec2 texelSize; vec2 layerSize; };\n";
        m_out +=
            "vec4 xtSample(vec2 uv) {\n"
            "    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0);\n"
            "    vec4 c = texture(uLayer, xtTexCoord(uv));\n"
            "    return c.a > 1e-5 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);\n"
            "}\n"
            "float xtFmod(float a, float b) { return a - b * trunc(a / b); }\n"
            "vec2 xtFmod(vec2 a, vec2 b) { return a - b * trunc(a / b); }\n"
            "vec3 xtFmod(vec3 a, vec3 b) { return a - b * trunc(a / b); }\n"
            "vec4 xtFmod(vec4 a, vec4 b) { return a - b * trunc(a / b); }\n";
        m_out += "vec4 xtMain(XtInput ";
        m_out += p.inputName;
        m_out += ") {\n";
        for (const auto& s : p.body) {
            stmt(*s, 1);
        }
        m_out += "}\n";
    }

private:
    void indent(int level) {
        for (int i = 0; i < level; ++i) m_out += "    ";
    }

    void stmt(const Stmt& s, int level) {
        switch (s.kind) {
            case Stmt::Kind::Block:
                if (s.body.empty()) {
                    return;  // empty statement
                }
                indent(level);
                m_out += "{\n";
                for (const auto& c : s.body) stmt(*c, level + 1);
                indent(level);
                m_out += "}\n";
                return;
            case Stmt::Kind::VarDecl:
                indent(level);
                m_out += glslTypeName(s.declType);
                m_out += " ";
                m_out += s.name;
                if (s.expr) {
                    m_out += " = ";
                    expr(*s.expr, s.declType);
                }
                m_out += ";\n";
                return;
            case Stmt::Kind::ExprStmt:
                indent(level);
                expr(*s.expr, s.expr->type);
                m_out += ";\n";
                return;
            case Stmt::Kind::Return:
                indent(level);
                m_out += "return ";
                expr(*s.expr, Ty::Float4);
                m_out += ";\n";
                return;
            case Stmt::Kind::If:
                indent(level);
                m_out += "if (";
                expr(*s.expr, Ty::Bool);
                m_out += ")\n";
                nested(*s.body.front(), level);
                if (s.hasElse) {
                    indent(level);
                    m_out += "else\n";
                    nested(*s.elseBody.front(), level);
                }
                return;
            case Stmt::Kind::For:
                indent(level);
                m_out += "for (int ";
                m_out += s.forInit->name;
                m_out += " = ";
                expr(*s.forInit->expr, Ty::Int);
                m_out += "; ";
                expr(*s.expr, Ty::Bool);
                m_out += "; ";
                expr(*s.forStep, s.forStep->type);
                m_out += ")\n";
                nested(*s.body.front(), level);
                return;
        }
    }

    /// The body of an if/else/for: always braced so a nested single statement reads clearly.
    void nested(const Stmt& s, int level) {
        if (s.kind == Stmt::Kind::Block) {
            if (s.body.empty()) {
                indent(level);
                m_out += "{ }\n";
                return;
            }
            stmt(s, level);
            return;
        }
        indent(level);
        m_out += "{\n";
        stmt(s, level + 1);
        indent(level);
        m_out += "}\n";
    }

    static const char* mapCall(const std::string& name) {
        if (name == "lerp") return "mix";
        if (name == "frac") return "fract";
        if (name == "atan2") return "atan";
        if (name == "fmod") return "xtFmod";
        return name.c_str();
    }

    /// Recomputes checkCall's unified argument type (see hlsl_subset.cpp).
    static Ty unifiedArgType(const Expr& call) {
        Ty unified = Ty::Int;
        bool anyFloat = false;
        for (const auto& a : call.args) {
            if (a->type != Ty::Int) anyFloat = true;
            if (isVectorType(a->type)) unified = a->type;
        }
        if (!isVectorType(unified)) {
            bool intOnly = !anyFloat && (call.text == "abs" || call.text == "min" || call.text == "max" ||
                                         call.text == "clamp");
            unified = intOnly ? Ty::Int : Ty::Float;
        }
        return unified;
    }

    /// Emits `e` converted to `want` (int -> float, scalar -> vector); `want == e.type` emits it
    /// as is.
    void expr(const Expr& e, Ty want) {
        if (want == e.type || want == Ty::Void || want == Ty::Bool || e.type == Ty::Bool) {
            raw(e);
            return;
        }
        if (e.type == Ty::Int && want == Ty::Float) {
            if (e.kind == Expr::Kind::IntLit) {
                m_out += e.text;
                m_out += ".0";
            } else if (e.kind == Expr::Kind::Unary && e.text == "-" &&
                       e.args[0]->kind == Expr::Kind::IntLit) {
                m_out += "-";
                m_out += e.args[0]->text;
                m_out += ".0";
            } else {
                m_out += "float(";
                raw(e);
                m_out += ")";
            }
            return;
        }
        if (isVectorType(want) && componentCount(e.type) == 1) {
            m_out += glslTypeName(want);
            m_out += "(";
            expr(e, Ty::Float);
            m_out += ")";
            return;
        }
        raw(e);
    }

    void raw(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::IntLit:
            case Expr::Kind::BoolLit:
                m_out += e.text;
                return;
            case Expr::Kind::FloatLit: {
                std::string t = e.text;
                if (t.find('.') == std::string::npos && t.find('e') == std::string::npos &&
                    t.find('E') == std::string::npos) {
                    t += ".0";
                } else if (t.back() == '.') {
                    t += "0";
                } else if (t.front() == '.') {
                    t = "0" + t;
                }
                m_out += t;
                return;
            }
            case Expr::Kind::Ident:
                m_out += e.text;
                return;
            case Expr::Kind::Member:
                raw(*e.args[0]);
                m_out += ".";
                m_out += e.text;
                return;
            case Expr::Kind::Ctor: {
                Ty t;
                t = e.text == "float"    ? Ty::Float
                    : e.text == "float2" ? Ty::Float2
                    : e.text == "float3" ? Ty::Float3
                    : e.text == "float4" ? Ty::Float4
                    : e.text == "int"    ? Ty::Int
                                         : Ty::Bool;
                m_out += glslTypeName(t);
                m_out += "(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i > 0) m_out += ", ";
                    // Components of a float vector are floats; scalar casts keep the argument.
                    if (isVectorType(t)) {
                        expr(*e.args[i], componentCount(e.args[i]->type) == 1 ? Ty::Float : e.args[i]->type);
                    } else {
                        raw(*e.args[i]);
                    }
                }
                m_out += ")";
                return;
            }
            case Expr::Kind::Call: {
                if (e.text == "saturate") {
                    m_out += "clamp(";
                    expr(*e.args[0], e.type);
                    m_out += ", 0.0, 1.0)";
                    return;
                }
                if (e.text == "xtSample") {
                    m_out += "xtSample(";
                    expr(*e.args[0], Ty::Float2);
                    m_out += ")";
                    return;
                }
                const Ty unified = unifiedArgType(e);
                m_out += mapCall(e.text);
                m_out += "(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i > 0) m_out += ", ";
                    expr(*e.args[i], unified);
                }
                m_out += ")";
                return;
            }
            case Expr::Kind::Unary:
                m_out += e.text;
                paren(*e.args[0]);
                return;
            case Expr::Kind::Binary: {
                // Arithmetic between int and float promotes the int side; scalar/vector mixes are
                // native GLSL.
                Ty l = e.args[0]->type, r = e.args[1]->type;
                Ty wantL = l, wantR = r;
                if (isFloatType(e.type) || e.type == Ty::Bool) {
                    if (l == Ty::Int && r != Ty::Int) wantL = Ty::Float;
                    if (r == Ty::Int && l != Ty::Int) wantR = Ty::Float;
                }
                parenExpr(*e.args[0], wantL);
                m_out += " ";
                m_out += e.text;
                m_out += " ";
                parenExpr(*e.args[1], wantR);
                return;
            }
            case Expr::Kind::Assign:
                raw(*e.args[0]);
                m_out += " ";
                m_out += e.text;
                m_out += " ";
                expr(*e.args[1], e.args[0]->type);
                return;
            case Expr::Kind::IncDec:
                if (e.prefix) {
                    m_out += e.text;
                    raw(*e.args[0]);
                } else {
                    raw(*e.args[0]);
                    m_out += e.text;
                }
                return;
        }
    }

    static bool needsParens(const Expr& e) {
        return e.kind == Expr::Kind::Binary || e.kind == Expr::Kind::Assign || e.kind == Expr::Kind::Unary;
    }
    void paren(const Expr& e) { parenExpr(e, e.type); }
    void parenExpr(const Expr& e, Ty want) {
        bool p = needsParens(e);
        if (p) m_out += "(";
        expr(e, want);
        if (p) m_out += ")";
    }

    std::string& m_out;
};

}  // namespace

void emitGlsl(const Program& program, std::span<const XtfxParam> params, std::string& out) {
    out.clear();
    Emitter em(out);
    em.program(program, params);
}

}  // namespace xt::compile::xtfx

namespace xt::compile {

bool translateHlslSubset(std::string_view body, int firstLine, std::span<const XtfxParam> params,
                         std::string& glsl, XtfxError& err) {
    xtfx::Program program;
    if (!xtfx::parseProgram(body, firstLine, params, program, err)) {
        glsl.clear();
        return false;
    }
    xtfx::emitGlsl(program, params, glsl);
    return true;
}

}  // namespace xt::compile
