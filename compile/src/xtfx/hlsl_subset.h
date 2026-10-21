#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/xtfx.h"

/// The HLSL subset of docs/design/18-effects.md §18.7 (docs/tasks/T-203.md item 1): a typed AST
/// built by a hand-written recursive-descent parser (hlsl_subset.cpp) and consumed by the GLSL
/// emitter (glsl_emit.cpp). Internal to xt_compile; the public entry is xt/compile/xtfx.h.

namespace xt::compile::xtfx {

/// Every type the subset knows. `XtInput` is only ever the type of xtMain's parameter.
enum class Ty : uint8_t { Void, Bool, Int, Float, Float2, Float3, Float4, XtInput };

inline int componentCount(Ty t) {
    switch (t) {
        case Ty::Float2:
            return 2;
        case Ty::Float3:
            return 3;
        case Ty::Float4:
            return 4;
        default:
            return 1;
    }
}
inline bool isFloatType(Ty t) {
    return t == Ty::Float || t == Ty::Float2 || t == Ty::Float3 || t == Ty::Float4;
}
inline bool isVectorType(Ty t) { return t == Ty::Float2 || t == Ty::Float3 || t == Ty::Float4; }
inline bool isNumericType(Ty t) { return t == Ty::Int || isFloatType(t); }
inline Ty floatVector(int n) {
    switch (n) {
        case 2:
            return Ty::Float2;
        case 3:
            return Ty::Float3;
        case 4:
            return Ty::Float4;
        default:
            return Ty::Float;
    }
}
const char* hlslTypeName(Ty t);
const char* glslTypeName(Ty t);

struct Expr {
    enum class Kind : uint8_t {
        IntLit,    ///< text = digits
        FloatLit,  ///< text = source spelling (normalized to contain a '.' or exponent)
        BoolLit,   ///< text = "true"/"false"
        Ident,     ///< text = name
        Member,    ///< args[0].text -- a swizzle or an XtInput field (text = member)
        Call,      ///< text = builtin name; args = arguments
        Ctor,      ///< text = HLSL type name (float, float2, ..., int, bool); args = components
        Unary,     ///< text = "-", "!", "+"; args[0]
        Binary,    ///< text = operator; args[0] op args[1]
        Assign,    ///< text = "=", "+=", "-=", "*=", "/="; args[0] = lvalue, args[1] = rhs
        IncDec,    ///< text = "++"/"--"; args[0] = lvalue; `prefix` says which side
    };
    Kind kind = Kind::IntLit;
    Ty type = Ty::Void;
    int line = 0;
    int col = 0;
    std::string text;
    std::vector<std::unique_ptr<Expr>> args;
    bool prefix = false;
    /// T-257: the height of this node's tree (1 + the deepest argument), capped by the parser.
    int depth = 1;
};

struct Stmt {
    enum class Kind : uint8_t { Block, VarDecl, ExprStmt, If, For, Return };
    Kind kind = Kind::Block;
    int line = 0;
    int col = 0;
    /// VarDecl: the declared type and name; `expr` = the initializer (may be null).
    Ty declType = Ty::Void;
    std::string name;
    /// ExprStmt/Return: the expression; If/For: the condition; VarDecl: the initializer.
    std::unique_ptr<Expr> expr;
    /// For: the init statement (a VarDecl) and the increment expression.
    std::unique_ptr<Stmt> forInit;
    std::unique_ptr<Expr> forStep;
    /// Block: the statements; If: `body` = then, `elseBody` = else (may be empty); For: the body.
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    bool hasElse = false;
};

/// `float4 xtMain(XtInput <inputName>) { body }`.
struct Program {
    std::string inputName;
    std::vector<std::unique_ptr<Stmt>> body;
};

/// Parses and type-checks `body`; false with `err` set on the first violation.
bool parseProgram(std::string_view body, int firstLine, std::span<const XtfxParam> params, Program& out,
                  XtfxError& err);

/// Writes the GLSL 330 module (uniforms, XtInput, helpers, xtMain) for a checked program.
void emitGlsl(const Program& program, std::span<const XtfxParam> params, std::string& out);

}  // namespace xt::compile::xtfx
