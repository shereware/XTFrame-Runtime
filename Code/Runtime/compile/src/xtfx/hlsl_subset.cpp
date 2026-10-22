#include "hlsl_subset.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/// HLSL subset lexer + recursive-descent parser + type checker (Docs/design/18-effects.md §18.7,
/// Docs/tasks/T-203.md item 1). One pass: every expression is typed as it is built, so the
/// emitter (glsl_emit.cpp) only inserts the int -> float / scalar -> vector promotions.
///
/// Not covered by the design (conservative choices, listed in the T-203 report):
///   - Only `xtMain` may be defined: helper functions are not in the listed subset, and without
///     them recursion is impossible.
///   - The ternary operator `?:`, `while`/`do`/`switch`, `break`/`continue` are rejected (not
///     listed).
///   - Comparisons are scalar-only (HLSL's component-wise vector comparison is not in the
///     subset's operator list; GLSL would need lessThan() anyway).
///   - The `for` counter cannot be written in the loop body (that would defeat the constant
///     bound the design requires).
///   - Identifiers that are GLSL keywords/builtins (`mix`, `fract`, `in`, `texture`, ...) or the
///     wrapper's own names (`uLayer`, `vUV`, `fragColor`, ...) are rejected so the translation
///     can keep parameter uniforms and locals under their HLSL names.

namespace xt::compile::xtfx {

const char* hlslTypeName(Ty t) {
    switch (t) {
        case Ty::Void:
            return "void";
        case Ty::Bool:
            return "bool";
        case Ty::Int:
            return "int";
        case Ty::Float:
            return "float";
        case Ty::Float2:
            return "float2";
        case Ty::Float3:
            return "float3";
        case Ty::Float4:
            return "float4";
        case Ty::XtInput:
            return "XtInput";
    }
    return "?";
}

const char* glslTypeName(Ty t) {
    switch (t) {
        case Ty::Void:
            return "void";
        case Ty::Bool:
            return "bool";
        case Ty::Int:
            return "int";
        case Ty::Float:
            return "float";
        case Ty::Float2:
            return "vec2";
        case Ty::Float3:
            return "vec3";
        case Ty::Float4:
            return "vec4";
        case Ty::XtInput:
            return "XtInput";
    }
    return "?";
}

namespace {

// -------------------------------------------------------------------------------------------
// Lexer
// -------------------------------------------------------------------------------------------

enum class Tok : uint8_t { End, Ident, Int, Float, Punct, Bad };

struct TokenHlslSubset {
    Tok kind = Tok::End;
    std::string text;
    int line = 1;
    int col = 1;
};

class LexerHlslSubset {
public:
    LexerHlslSubset(std::string_view src, int firstLine) : m_src(src), m_line(firstLine) {}

    TokenHlslSubset next() {
        skipSpace();
        TokenHlslSubset t;
        t.line = m_line;
        t.col = m_col;
        if (m_pos >= m_src.size()) {
            t.kind = Tok::End;
            return t;
        }
        char c = m_src[m_pos];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = m_pos;
            while (m_pos < m_src.size() &&
                   (std::isalnum(static_cast<unsigned char>(m_src[m_pos])) || m_src[m_pos] == '_')) {
                advance();
            }
            t.kind = Tok::Ident;
            t.text = std::string(m_src.substr(start, m_pos - start));
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && m_pos + 1 < m_src.size() &&
             std::isdigit(static_cast<unsigned char>(m_src[m_pos + 1])))) {
            size_t start = m_pos;
            bool isFloat = false;
            while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                advance();
            }
            if (m_pos < m_src.size() && m_src[m_pos] == '.') {
                isFloat = true;
                advance();
                while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                    advance();
                }
            }
            if (m_pos < m_src.size() && (m_src[m_pos] == 'e' || m_src[m_pos] == 'E')) {
                size_t save = m_pos;
                int saveCol = m_col;
                advance();
                if (m_pos < m_src.size() && (m_src[m_pos] == '+' || m_src[m_pos] == '-')) {
                    advance();
                }
                if (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                    isFloat = true;
                    while (m_pos < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_pos]))) {
                        advance();
                    }
                } else {
                    m_pos = save;
                    m_col = saveCol;
                }
            }
            if (m_pos < m_src.size() && (m_src[m_pos] == 'f' || m_src[m_pos] == 'F')) {
                isFloat = true;
                t.text = std::string(m_src.substr(start, m_pos - start));
                advance();
            } else {
                t.text = std::string(m_src.substr(start, m_pos - start));
            }
            if (m_pos < m_src.size() &&
                (std::isalnum(static_cast<unsigned char>(m_src[m_pos])) || m_src[m_pos] == '_')) {
                t.kind = Tok::Bad;
                t.text = "malformed number";
                return t;
            }
            t.kind = isFloat ? Tok::Float : Tok::Int;
            return t;
        }
        // Punctuation: longest match first.
        static const char* const kTwo[] = {
            "++", "--", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||"};
        if (m_pos + 1 < m_src.size()) {
            std::string_view two = m_src.substr(m_pos, 2);
            for (const char* p : kTwo) {
                if (two == p) {
                    t.kind = Tok::Punct;
                    t.text = std::string(two);
                    advance();
                    advance();
                    return t;
                }
            }
        }
        static const char kOne[] = "+-*/%=<>!(){};,.?:[]&|^~";
        for (const char* p = kOne; *p != 0; ++p) {
            if (c == *p) {
                t.kind = Tok::Punct;
                t.text = std::string(1, c);
                advance();
                return t;
            }
        }
        t.kind = Tok::Bad;
        t.text = std::string("unexpected character '") + c + "'";
        return t;
    }

private:
    void advance() {
        if (m_src[m_pos] == '\n') {
            ++m_line;
            m_col = 1;
        } else {
            ++m_col;
        }
        ++m_pos;
    }
    void skipSpace() {
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '/') {
                while (m_pos < m_src.size() && m_src[m_pos] != '\n') {
                    advance();
                }
            } else if (c == '/' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '*') {
                advance();
                advance();
                while (m_pos < m_src.size() &&
                       !(m_src[m_pos] == '*' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '/')) {
                    advance();
                }
                if (m_pos < m_src.size()) {
                    advance();
                    advance();
                }
            } else {
                break;
            }
        }
    }

    std::string_view m_src;
    size_t m_pos = 0;
    int m_line = 1;
    int m_col = 1;
};

// -------------------------------------------------------------------------------------------
// Name tables
// -------------------------------------------------------------------------------------------

bool typeFromName(std::string_view name, Ty& out) {
    if (name == "float")
        out = Ty::Float;
    else if (name == "float2")
        out = Ty::Float2;
    else if (name == "float3")
        out = Ty::Float3;
    else if (name == "float4")
        out = Ty::Float4;
    else if (name == "int")
        out = Ty::Int;
    else if (name == "bool")
        out = Ty::Bool;
    else
        return false;
    return true;
}

/// Types HLSL has but the subset does not, for a pointed error message.
bool isForbiddenTypeName(std::string_view n) {
    static const char* const kNames[] = {
        "half",       "double",     "uint",     "int2",     "int3",     "int4",     "uint2",  "uint3",
        "uint4",      "bool2",      "bool3",    "bool4",    "half2",    "half3",    "half4",  "float2x2",
        "float3x3",   "float4x4",   "float2x3", "float3x2", "float3x4", "float4x3", "matrix", "vector",
        "min16float", "min10float", "double2",  "double3",  "double4",  "int1",     "float1"};
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

bool isTextureTypeName(std::string_view n) {
    static const char* const kNames[] = {"Texture2D",
                                         "Texture1D",
                                         "Texture3D",
                                         "TextureCube",
                                         "Texture2DArray",
                                         "SamplerState",
                                         "SamplerComparisonState",
                                         "sampler",
                                         "sampler2D",
                                         "Buffer",
                                         "StructuredBuffer",
                                         "RWTexture2D",
                                         "RWStructuredBuffer",
                                         "ByteAddressBuffer",
                                         "cbuffer",
                                         "tbuffer"};
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

/// GLSL keywords, GLSL-only builtins and the wrapper's names: an HLSL identifier spelled like
/// one of these cannot keep its name in the translation, so it is rejected up front.
bool isGlslReserved(std::string_view n) {
    static const char* const kNames[] = {
        // GLSL keywords / reserved words.
        "attribute",
        "const",
        "uniform",
        "varying",
        "buffer",
        "shared",
        "coherent",
        "volatile",
        "restrict",
        "readonly",
        "writeonly",
        "layout",
        "centroid",
        "flat",
        "smooth",
        "noperspective",
        "patch",
        "sample",
        "break",
        "continue",
        "do",
        "for",
        "while",
        "switch",
        "case",
        "default",
        "if",
        "else",
        "subroutine",
        "in",
        "out",
        "inout",
        "float",
        "double",
        "int",
        "void",
        "bool",
        "true",
        "false",
        "invariant",
        "precise",
        "discard",
        "return",
        "mat2",
        "mat3",
        "mat4",
        "dmat2",
        "dmat3",
        "dmat4",
        "mat2x2",
        "mat2x3",
        "mat2x4",
        "mat3x2",
        "mat3x3",
        "mat3x4",
        "mat4x2",
        "mat4x3",
        "mat4x4",
        "vec2",
        "vec3",
        "vec4",
        "ivec2",
        "ivec3",
        "ivec4",
        "bvec2",
        "bvec3",
        "bvec4",
        "dvec2",
        "dvec3",
        "dvec4",
        "uint",
        "uvec2",
        "uvec3",
        "uvec4",
        "lowp",
        "mediump",
        "highp",
        "precision",
        "sampler1D",
        "sampler2D",
        "sampler3D",
        "samplerCube",
        "sampler1DShadow",
        "sampler2DShadow",
        "samplerCubeShadow",
        "sampler1DArray",
        "sampler2DArray",
        "sampler1DArrayShadow",
        "sampler2DArrayShadow",
        "isampler1D",
        "isampler2D",
        "isampler3D",
        "isamplerCube",
        "isampler1DArray",
        "isampler2DArray",
        "usampler1D",
        "usampler2D",
        "usampler3D",
        "usamplerCube",
        "usampler1DArray",
        "usampler2DArray",
        "sampler2DRect",
        "sampler2DRectShadow",
        "isampler2DRect",
        "usampler2DRect",
        "samplerBuffer",
        "isamplerBuffer",
        "usamplerBuffer",
        "sampler2DMS",
        "isampler2DMS",
        "usampler2DMS",
        "sampler2DMSArray",
        "isampler2DMSArray",
        "usampler2DMSArray",
        "samplerCubeArray",
        "samplerCubeArrayShadow",
        "isamplerCubeArray",
        "usamplerCubeArray",
        "struct",
        "common",
        "partition",
        "active",
        "asm",
        "class",
        "union",
        "enum",
        "typedef",
        "template",
        "this",
        "resource",
        "goto",
        "inline",
        "noinline",
        "public",
        "static",
        "extern",
        "external",
        "interface",
        "long",
        "short",
        "half",
        "fixed",
        "unsigned",
        "superp",
        "input",
        "output",
        "hvec2",
        "hvec3",
        "hvec4",
        "fvec2",
        "fvec3",
        "fvec4",
        "sampler3DRect",
        "filter",
        "sizeof",
        "cast",
        "namespace",
        "using",
        "main",
        // GLSL builtin functions the translation emits or that would shadow a builtin.
        "mix",
        "fract",
        "texture",
        "trunc",
        "mod",
        "inversesqrt",
        "radians",
        "degrees",
        "atan",
        "asin",
        "acos",
        "sinh",
        "cosh",
        "tanh",
        "exp2",
        "log2",
        "round",
        "roundEven",
        "sign",
        "isnan",
        "isinf",
        "lessThan",
        "greaterThan",
        "equal",
        "notEqual",
        "any",
        "all",
        "not",
        "dFdx",
        "dFdy",
        "fwidth",
        "textureSize",
        "texelFetch",
        "textureLod",
        "textureGrad",
        "reflect",
        "refract",
        "faceforward",
        "outerProduct",
        "transpose",
        "determinant",
        "inverse",
        "matrixCompMult",
        "floatBitsToInt",
        "intBitsToFloat",
        "packUnorm2x16",
        "unpackUnorm2x16",
        "modf",
        // The wrapper's own names (Code/Runtime/gl/src/gl_effects.cpp).
        "uLayer",
        "uNdc",
        "vUV",
        "fragColor",
        "uTexelSize",
        "uLayerSize",
        "xtFmod",
        "xtSample",
        "xtMain",
        "xtTexCoord",
        "xtIn",
        "XtInput",
        "xtTime",
    };
    if (n.rfind("gl_", 0) == 0 || n.rfind("__", 0) == 0) {
        return true;
    }
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

/// HLSL keywords that are not part of the subset, each with its own message.
const char* forbiddenKeywordMessage(std::string_view n) {
    if (n == "struct") return "custom structs are not allowed";
    if (n == "discard") return "discard is not allowed";
    if (n == "while" || n == "do") return "only 'for' loops with constant bounds are allowed";
    if (n == "switch" || n == "case") return "switch is not allowed";
    if (n == "break" || n == "continue") return "break/continue are not allowed";
    if (n == "static" || n == "uniform" || n == "extern" || n == "groupshared")
        return "global variables are not allowed";
    if (n == "typedef" || n == "class" || n == "interface") return "type declarations are not allowed";
    if (n == "cbuffer" || n == "tbuffer")
        return "constant buffers are not allowed (parameters come from the header)";
    if (n == "register" || n == "packoffset") return "register/packoffset are not allowed";
    if (n == "goto") return "goto is not allowed";
    return nullptr;
}

struct BuiltinSig {
    const char* name;
    int arity;
    /// 0: result = the unified argument type; 1: result is Float (length, distance, dot);
    /// 2: float3 x float3 -> float3 (cross); 3: xtSample (float2 -> float4).
    int resultRule;
};

const BuiltinSig* builtinByName(std::string_view n) {
    static const BuiltinSig kSigs[] = {
        {"abs", 1, 0},      {"sin", 1, 0},      {"cos", 1, 0},       {"tan", 1, 0},  {"atan2", 2, 0},
        {"exp", 1, 0},      {"log", 1, 0},      {"pow", 2, 0},       {"sqrt", 1, 0}, {"floor", 1, 0},
        {"ceil", 1, 0},     {"frac", 1, 0},     {"fmod", 2, 0},      {"min", 2, 0},  {"max", 2, 0},
        {"clamp", 3, 0},    {"saturate", 1, 0}, {"lerp", 3, 0},      {"step", 2, 0}, {"smoothstep", 3, 0},
        {"length", 1, 1},   {"distance", 2, 1}, {"normalize", 1, 0}, {"dot", 2, 1},  {"cross", 2, 2},
        {"xtSample", 1, 3},
    };
    for (const BuiltinSig& s : kSigs) {
        if (n == s.name) return &s;
    }
    return nullptr;
}

/// True for an HLSL intrinsic the subset leaves out (so the message can say so, rather than
/// "unknown function").
bool isKnownHlslIntrinsic(std::string_view n) {
    static const char* const kNames[] = {
        "mul",         "asin",      "acos",        "atan",        "sinh",   "cosh",    "tanh",    "exp2",
        "log2",        "log10",     "rsqrt",       "round",       "trunc",  "sign",    "mad",     "fma",
        "any",         "all",       "ddx",         "ddy",         "fwidth", "reflect", "refract", "transpose",
        "determinant", "isnan",     "isinf",       "asfloat",     "asint",  "asuint",  "tex2D",   "tex2Dlod",
        "clip",        "modf",      "ldexp",       "frexp",       "noise",  "radians", "degrees", "rcp",
        "sincos",      "countbits", "reversebits", "firstbithigh"};
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

// -------------------------------------------------------------------------------------------
// Parser + checker
// -------------------------------------------------------------------------------------------

struct Symbol {
    Ty type = Ty::Void;
    bool readOnly = false;
    bool loopCounter = false;
};

/// T-257: the deepest expression or statement nesting the recursive descent accepts.
constexpr int kMaxNesting = 64;
/// T-257: the most terms one left-associative operator chain or one postfix chain may have, so
/// the GLSL emitter and the tree's destructor never recurse deeper than that per chain.
constexpr int kMaxChainTerms = 256;
/// T-257: the tallest expression tree (Expr::depth) a body may build -- what bounds the emitter's
/// and the destructor's recursion when chains are nested inside chains.
constexpr int kMaxTreeDepth = 256;

/// Counts one level of nesting for the lifetime of the scope.
class NestingScope {
public:
    explicit NestingScope(int& depth) : m_depth(depth) { ++m_depth; }
    ~NestingScope() { --m_depth; }
    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;
    bool tooDeep() const { return m_depth > kMaxNesting; }

private:
    int& m_depth;
};

class Parser {
public:
    Parser(std::string_view body, int firstLine, std::span<const XtfxParam> params, XtfxError& err)
        : m_lexer(body, firstLine), m_params(params), m_err(err) {
        m_tok = m_lexer.next();
        m_peekValid = false;
    }

    bool parse(Program& out) {
        // Globals: the parameters and xtTime.
        m_scopes.emplace_back();
        for (const XtfxParam& p : m_params) {
            Symbol s;
            s.type = p.type == 0 ? Ty::Float : (p.type == 1 ? Ty::Float2 : Ty::Float4);
            s.readOnly = true;
            if (isGlslReserved(p.name)) {
                return fail("parameter name '" + p.name + "' is reserved in the translation", 1, 1);
            }
            m_scopes.back()[p.name] = s;
        }
        {
            Symbol s;
            s.type = Ty::Float;
            s.readOnly = true;
            m_scopes.back()["xtTime"] = s;
        }
        // The one and only function: `float4 xtMain(XtInput <name>)`.
        if (m_tok.kind == Tok::End) {
            return fail("body must define float4 xtMain(XtInput i)");
        }
        if (!(m_tok.kind == Tok::Ident && m_tok.text == "float4")) {
            if (m_tok.kind == Tok::Ident) {
                if (const char* msg = forbiddenKeywordMessage(m_tok.text)) {
                    return fail(msg);
                }
                if (isTextureTypeName(m_tok.text)) {
                    return fail("texture objects are not allowed (use xtSample)");
                }
                Ty t;
                if (typeFromName(m_tok.text, t) || isForbiddenTypeName(m_tok.text)) {
                    // Could be a global variable or another function: both are out.
                    return fail("only float4 xtMain(XtInput i) may be defined at global scope");
                }
            }
            return fail("body must start with float4 xtMain(XtInput i)");
        }
        next();
        if (!(m_tok.kind == Tok::Ident && m_tok.text == "xtMain")) {
            return fail("only xtMain may be defined at global scope (helper functions are not allowed)");
        }
        next();
        if (!expectPunct("(")) return false;
        if (!(m_tok.kind == Tok::Ident && m_tok.text == "XtInput")) {
            return fail("xtMain must take exactly one XtInput parameter");
        }
        next();
        if (m_tok.kind != Tok::Ident) {
            return fail("expected the XtInput parameter name");
        }
        if (!checkName(m_tok.text)) return false;
        out.inputName = m_tok.text;
        next();
        if (!expectPunct(")")) return false;
        m_scopes.emplace_back();
        {
            Symbol s;
            s.type = Ty::XtInput;
            s.readOnly = true;
            m_scopes.back()[out.inputName] = s;
        }
        if (!parseBlockInto(out.body)) return false;
        m_scopes.pop_back();
        if (m_tok.kind != Tok::End) {
            if (m_tok.kind == Tok::Ident) {
                return fail("only xtMain may be defined (no other functions or globals)");
            }
            return fail("unexpected token '" + m_tok.text + "' after xtMain");
        }
        if (!m_sawReturn) {
            return fail("xtMain must return a float4", m_lastLine, m_lastCol);
        }
        return true;
    }

private:
    // -- token stream --
    void next() {
        m_lastLine = m_tok.line;
        m_lastCol = m_tok.col;
        if (m_peekValid) {
            m_tok = m_peek;
            m_peekValid = false;
        } else {
            m_tok = m_lexer.next();
        }
    }
    const TokenHlslSubset& peekTok() {
        if (!m_peekValid) {
            m_peek = m_lexer.next();
            m_peekValid = true;
        }
        return m_peek;
    }
    bool isPunct(const char* p) const { return m_tok.kind == Tok::Punct && m_tok.text == p; }
    bool acceptPunct(const char* p) {
        if (isPunct(p)) {
            next();
            return true;
        }
        return false;
    }
    bool expectPunct(const char* p) {
        if (!isPunct(p)) {
            if (m_tok.kind == Tok::Bad) return fail(m_tok.text);
            return fail(std::string("expected '") + p + "'");
        }
        next();
        return true;
    }
    bool fail(std::string msg) { return fail(std::move(msg), m_tok.line, m_tok.col); }
    bool fail(std::string msg, int line, int col) {
        if (m_err.message.empty()) {
            m_err.message = std::move(msg);
            m_err.line = line;
            m_err.col = col;
        }
        return false;
    }

    // -- scopes --
    Symbol* lookup(const std::string& name) {
        for (size_t i = m_scopes.size(); i-- > 0;) {
            auto it = m_scopes[i].find(name);
            if (it != m_scopes[i].end()) return &it->second;
        }
        return nullptr;
    }
    bool checkName(const std::string& name) {
        Ty t;
        if (typeFromName(name, t) || isForbiddenTypeName(name) || isTextureTypeName(name)) {
            return fail("'" + name + "' is a type name");
        }
        if (forbiddenKeywordMessage(name) != nullptr || name == "return" || name == "if" || name == "else" ||
            name == "for" || name == "true" || name == "false" || name == "const") {
            return fail("'" + name + "' is a keyword");
        }
        if (isGlslReserved(name)) {
            return fail("identifier '" + name + "' is reserved in the translation; rename it");
        }
        if (builtinByName(name) != nullptr || isKnownHlslIntrinsic(name)) {
            return fail("'" + name + "' is a function name");
        }
        if (m_scopes.back().count(name) != 0) {
            return fail("'" + name + "' is already declared in this scope");
        }
        return true;
    }

    // -- statements --
    bool parseBlockInto(std::vector<std::unique_ptr<Stmt>>& out) {
        if (!expectPunct("{")) return false;
        while (!isPunct("}")) {
            if (m_tok.kind == Tok::End) return fail("expected '}'");
            std::unique_ptr<Stmt> s;
            if (!parseStatement(s)) return false;
            out.push_back(std::move(s));
        }
        next();
        return true;
    }

    bool parseStatement(std::unique_ptr<Stmt>& out) {
        // Every nested statement (a block, an if/else branch, a for body) comes through here.
        const NestingScope nesting(m_blockDepth);
        if (nesting.tooDeep()) return fail("block nesting too deep (max 64)");
        out = std::make_unique<Stmt>();
        out->line = m_tok.line;
        out->col = m_tok.col;
        if (m_tok.kind == Tok::Bad) return fail(m_tok.text);
        if (isPunct("{")) {
            out->kind = Stmt::Kind::Block;
            m_scopes.emplace_back();
            bool ok = parseBlockInto(out->body);
            m_scopes.pop_back();
            return ok;
        }
        if (isPunct(";")) {
            next();
            out->kind = Stmt::Kind::Block;  // empty statement
            return true;
        }
        if (m_tok.kind == Tok::Ident) {
            const std::string& w = m_tok.text;
            if (const char* msg = forbiddenKeywordMessage(w)) return fail(msg);
            if (isTextureTypeName(w)) return fail("texture objects are not allowed (use xtSample)");
            if (w == "return") {
                next();
                out->kind = Stmt::Kind::Return;
                if (isPunct(";")) return fail("xtMain must return a float4 value");
                if (!parseExpr(out->expr)) return false;
                if (out->expr->type != Ty::Float4) {
                    return fail(
                        std::string("return value must be float4, got ") + hlslTypeName(out->expr->type),
                        out->line, out->col);
                }
                m_sawReturn = true;
                return expectPunct(";");
            }
            if (w == "if") return parseIf(*out);
            if (w == "for") return parseFor(*out);
            if (w == "else") return fail("'else' without 'if'");
            bool isConst = false;
            if (w == "const") {
                isConst = true;
                next();
            }
            Ty t;
            if (m_tok.kind == Tok::Ident && typeFromName(m_tok.text, t)) {
                return parseVarDecl(*out, t, isConst);
            }
            if (m_tok.kind == Tok::Ident && isForbiddenTypeName(m_tok.text)) {
                return fail("type '" + m_tok.text +
                            "' is not in the subset (float, float2, float3, float4, int, bool)");
            }
            if (isConst) return fail("expected a type after 'const'");
        }
        out->kind = Stmt::Kind::ExprStmt;
        if (!parseExpr(out->expr)) return false;
        Expr::Kind k = out->expr->kind;
        if (k != Expr::Kind::Assign && k != Expr::Kind::IncDec) {
            return fail("expression statement has no effect (only assignments and ++/-- are allowed)",
                        out->line, out->col);
        }
        return expectPunct(";");
    }

    bool parseVarDecl(Stmt& out, Ty t, bool isConst) {
        (void)isConst;  // `const` is accepted and dropped: GLSL const needs a constant initializer.
        out.kind = Stmt::Kind::VarDecl;
        out.declType = t;
        next();  // type
        if (m_tok.kind != Tok::Ident) return fail("expected a variable name");
        if (!checkName(m_tok.text)) return false;
        out.name = m_tok.text;
        next();
        if (isPunct("[")) return fail("arrays are not allowed");
        if (acceptPunct("=")) {
            if (isPunct("{")) return fail("brace initializers are not allowed (use a constructor)");
            if (!parseExpr(out.expr)) return false;
            if (!convertible(out.expr->type, t)) {
                return fail(std::string("cannot initialize ") + hlslTypeName(t) + " '" + out.name +
                                "' from " + hlslTypeName(out.expr->type),
                            out.line, out.col);
            }
        }
        if (isPunct(",")) return fail("declare one variable per statement");
        if (!expectPunct(";")) return false;
        Symbol s;
        s.type = t;
        m_scopes.back()[out.name] = s;
        return true;
    }

    bool parseIf(Stmt& out) {
        out.kind = Stmt::Kind::If;
        next();
        if (!expectPunct("(")) return false;
        if (!parseExpr(out.expr)) return false;
        if (out.expr->type != Ty::Bool) {
            return fail(std::string("if condition must be bool, got ") + hlslTypeName(out.expr->type),
                        out.line, out.col);
        }
        if (!expectPunct(")")) return false;
        m_scopes.emplace_back();
        std::unique_ptr<Stmt> thenStmt;
        bool ok = parseStatement(thenStmt);
        m_scopes.pop_back();
        if (!ok) return false;
        out.body.push_back(std::move(thenStmt));
        if (m_tok.kind == Tok::Ident && m_tok.text == "else") {
            next();
            out.hasElse = true;
            m_scopes.emplace_back();
            std::unique_ptr<Stmt> elseStmt;
            ok = parseStatement(elseStmt);
            m_scopes.pop_back();
            if (!ok) return false;
            out.elseBody.push_back(std::move(elseStmt));
        }
        return true;
    }

    static bool intConstant(const Expr& e, long& out) {
        if (e.kind == Expr::Kind::IntLit) {
            out = std::strtol(e.text.c_str(), nullptr, 10);
            return true;
        }
        if (e.kind == Expr::Kind::Unary && e.text == "-" && e.args[0]->kind == Expr::Kind::IntLit) {
            out = -std::strtol(e.args[0]->text.c_str(), nullptr, 10);
            return true;
        }
        return false;
    }

    bool parseFor(Stmt& out) {
        out.kind = Stmt::Kind::For;
        next();
        if (!expectPunct("(")) return false;
        // init: `int <name> = <const>`
        Ty t;
        if (!(m_tok.kind == Tok::Ident && typeFromName(m_tok.text, t) && t == Ty::Int)) {
            return fail("for: the loop counter must be declared as 'int <name> = <integer constant>'");
        }
        m_scopes.emplace_back();
        out.forInit = std::make_unique<Stmt>();
        out.forInit->line = m_tok.line;
        out.forInit->col = m_tok.col;
        // parseVarDecl consumes the ';'.
        if (!parseVarDecl(*out.forInit, Ty::Int, false)) {
            m_scopes.pop_back();
            return false;
        }
        long start = 0;
        if (out.forInit->expr == nullptr || !intConstant(*out.forInit->expr, start)) {
            m_scopes.pop_back();
            return fail("for: the loop counter must start at an integer constant", out.forInit->line,
                        out.forInit->col);
        }
        const std::string counter = out.forInit->name;
        m_scopes.back()[counter].loopCounter = true;
        // cond: `<name> <|<=|>|>= <const>`
        int condLine = m_tok.line, condCol = m_tok.col;
        if (!parseExpr(out.expr)) {
            m_scopes.pop_back();
            return false;
        }
        const Expr& cond = *out.expr;
        long bound = 0;
        std::string op;
        if (cond.kind == Expr::Kind::Binary && cond.args[0]->kind == Expr::Kind::Ident &&
            cond.args[0]->text == counter && intConstant(*cond.args[1], bound) &&
            (cond.text == "<" || cond.text == "<=" || cond.text == ">" || cond.text == ">=")) {
            op = cond.text;
        } else {
            m_scopes.pop_back();
            return fail("for: the condition must be '<counter> < <integer constant>' (or <=, >, >=)",
                        condLine, condCol);
        }
        if (!expectPunct(";")) {
            m_scopes.pop_back();
            return false;
        }
        // step: `i++`, `++i`, `i--`, `--i`, `i += c`, `i -= c`
        int stepLine = m_tok.line, stepCol = m_tok.col;
        m_inForStep = true;
        bool stepOk = parseExpr(out.forStep);
        m_inForStep = false;
        if (!stepOk) {
            m_scopes.pop_back();
            return false;
        }
        long step = 0;
        const Expr& st = *out.forStep;
        if (st.kind == Expr::Kind::IncDec && st.args[0]->kind == Expr::Kind::Ident &&
            st.args[0]->text == counter) {
            step = st.text == "++" ? 1 : -1;
        } else if (st.kind == Expr::Kind::Assign && (st.text == "+=" || st.text == "-=") &&
                   st.args[0]->kind == Expr::Kind::Ident && st.args[0]->text == counter &&
                   intConstant(*st.args[1], step)) {
            if (st.text == "-=") step = -step;
        } else {
            m_scopes.pop_back();
            return fail("for: the step must be '<counter>++', '<counter>--', '<counter> += c' or '-= c'",
                        stepLine, stepCol);
        }
        if (step == 0) {
            m_scopes.pop_back();
            return fail("for: the step must not be zero", stepLine, stepCol);
        }
        if (!expectPunct(")")) {
            m_scopes.pop_back();
            return false;
        }
        // Iteration count by simulation (bounded).
        long i = start;
        int iterations = 0;
        auto continues = [&](long v) {
            if (op == "<") return v < bound;
            if (op == "<=") return v <= bound;
            if (op == ">") return v > bound;
            return v >= bound;
        };
        while (continues(i) && iterations <= 64) {
            ++iterations;
            i += step;
        }
        if (iterations > 64) {
            m_scopes.pop_back();
            return fail("for: the loop runs more than 64 times", out.line, out.col);
        }
        std::unique_ptr<Stmt> bodyStmt;
        bool ok = parseStatement(bodyStmt);
        m_scopes.pop_back();
        if (!ok) return false;
        out.body.push_back(std::move(bodyStmt));
        return true;
    }

    // -- expressions (precedence climbing) --
    static bool convertible(Ty from, Ty to) {
        if (from == to) return true;
        if (from == Ty::Int && to == Ty::Float) return true;
        return false;
    }

    bool parseExpr(std::unique_ptr<Expr>& out) { return parseAssign(out); }

    bool parseAssign(std::unique_ptr<Expr>& out) {
        // Every nested expression (parentheses, call arguments, a right-hand side) starts here.
        const NestingScope nesting(m_exprDepth);
        if (nesting.tooDeep()) return fail("expression nesting too deep (max 64)");
        std::unique_ptr<Expr> lhs;
        if (!parseOr(lhs)) return false;
        if (m_tok.kind == Tok::Punct && (m_tok.text == "=" || m_tok.text == "+=" || m_tok.text == "-=" ||
                                         m_tok.text == "*=" || m_tok.text == "/=")) {
            std::string op = m_tok.text;
            int line = m_tok.line, col = m_tok.col;
            next();
            if (!isLvalue(*lhs)) return fail("left side of '" + op + "' is not assignable", line, col);
            if (!checkWritable(*lhs, line, col)) return false;
            std::unique_ptr<Expr> rhs;
            if (!parseAssign(rhs)) return false;
            if (op == "=") {
                if (!convertible(rhs->type, lhs->type)) {
                    return fail(std::string("cannot assign ") + hlslTypeName(rhs->type) + " to " +
                                    hlslTypeName(lhs->type),
                                line, col);
                }
            } else {
                Ty r;
                if (!arithmeticResult(lhs->type, rhs->type, r) || !convertible(r, lhs->type)) {
                    return fail(std::string("cannot apply '") + op + "' to " + hlslTypeName(lhs->type) +
                                    " and " + hlslTypeName(rhs->type),
                                line, col);
                }
            }
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Assign;
            e->text = op;
            e->type = lhs->type;
            e->line = line;
            e->col = col;
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            if (!setTreeDepth(*e)) return false;
            out = std::move(e);
            return true;
        }
        if (isPunct("?")) return fail("the ternary operator is not in the subset (use if/else)");
        out = std::move(lhs);
        return true;
    }

    static bool isLvalue(const Expr& e) {
        if (e.kind == Expr::Kind::Ident) return true;
        if (e.kind == Expr::Kind::Member && e.args[0]->kind == Expr::Kind::Ident) {
            // A swizzle without repeated components.
            for (size_t i = 0; i < e.text.size(); ++i) {
                for (size_t j = i + 1; j < e.text.size(); ++j) {
                    if (e.text[i] == e.text[j]) return false;
                }
            }
            return e.args[0]->type != Ty::XtInput;
        }
        return false;
    }

    bool checkWritable(const Expr& lv, int line, int col) {
        const Expr& base = lv.kind == Expr::Kind::Member ? *lv.args[0] : lv;
        Symbol* s = lookup(base.text);
        if (s == nullptr) return fail("unknown identifier '" + base.text + "'", line, col);
        if (s->readOnly) {
            return fail("'" + base.text + "' is read-only (parameters and xtTime cannot be assigned)", line,
                        col);
        }
        if (s->loopCounter && !m_inForStep) {
            return fail("the loop counter '" + base.text + "' cannot be modified in the loop body", line,
                        col);
        }
        return true;
    }

    /// Arithmetic result of `a op b`: int op int = int; any float involved = float; a scalar and
    /// a vector give the vector; two vectors must match.
    static bool arithmeticResult(Ty a, Ty b, Ty& out) {
        if (!isNumericType(a) || !isNumericType(b)) return false;
        if (a == Ty::Int && b == Ty::Int) {
            out = Ty::Int;
            return true;
        }
        int ca = componentCount(a), cb = componentCount(b);
        if (ca == 1 && cb == 1) {
            out = Ty::Float;
            return true;
        }
        if (ca == 1) {
            out = b;
            return true;
        }
        if (cb == 1) {
            out = a;
            return true;
        }
        if (a != b) return false;
        out = a;
        return true;
    }

    /// T-257: sets `e.depth` from its (already attached) arguments; false past kMaxTreeDepth.
    bool setTreeDepth(Expr& e) {
        int deepest = 0;
        for (const std::unique_ptr<Expr>& a : e.args) {
            deepest = std::max(deepest, a->depth);
        }
        e.depth = deepest + 1;
        if (e.depth > kMaxTreeDepth) return fail("expression too deep (max 256 levels)", e.line, e.col);
        return true;
    }

    bool makeBinary(std::unique_ptr<Expr>& lhs, const std::string& op, int line, int col,
                    std::unique_ptr<Expr> rhs) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Binary;
        e->text = op;
        e->line = line;
        e->col = col;
        if (op == "&&" || op == "||") {
            if (lhs->type != Ty::Bool || rhs->type != Ty::Bool) {
                return fail("'" + op + "' needs bool operands", line, col);
            }
            e->type = Ty::Bool;
        } else if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            bool okTypes;
            if (op == "==" || op == "!=") {
                okTypes = (lhs->type == Ty::Bool && rhs->type == Ty::Bool) ||
                          (isNumericType(lhs->type) && isNumericType(rhs->type) &&
                           componentCount(lhs->type) == 1 && componentCount(rhs->type) == 1);
            } else {
                okTypes = isNumericType(lhs->type) && isNumericType(rhs->type) &&
                          componentCount(lhs->type) == 1 && componentCount(rhs->type) == 1;
            }
            if (!okTypes) {
                return fail("'" + op + "' compares scalars only (got " + hlslTypeName(lhs->type) + " and " +
                                hlslTypeName(rhs->type) + ")",
                            line, col);
            }
            e->type = Ty::Bool;
        } else if (op == "%") {
            if (lhs->type != Ty::Int || rhs->type != Ty::Int) {
                return fail("'%' needs int operands (use fmod for floats)", line, col);
            }
            e->type = Ty::Int;
        } else {
            Ty r;
            if (!arithmeticResult(lhs->type, rhs->type, r)) {
                return fail("cannot apply '" + op + "' to " + hlslTypeName(lhs->type) + " and " +
                                hlslTypeName(rhs->type),
                            line, col);
            }
            e->type = r;
        }
        e->args.push_back(std::move(lhs));
        e->args.push_back(std::move(rhs));
        if (!setTreeDepth(*e)) return false;
        lhs = std::move(e);
        return true;
    }

    template <class Sub>
    bool parseLeftAssoc(std::unique_ptr<Expr>& out, Sub sub, std::initializer_list<const char*> ops) {
        if (!sub(out)) return false;
        int terms = 1;  // T-257: a left-associative chain is as deep a tree as it is long
        for (;;) {
            bool matched = false;
            for (const char* op : ops) {
                if (isPunct(op)) {
                    if (++terms > kMaxChainTerms) return fail("expression too long (max 256 terms)");
                    std::string o = op;
                    int line = m_tok.line, col = m_tok.col;
                    next();
                    std::unique_ptr<Expr> rhs;
                    if (!sub(rhs)) return false;
                    if (!makeBinary(out, o, line, col, std::move(rhs))) return false;
                    matched = true;
                    break;
                }
            }
            if (!matched) return true;
        }
    }

    bool parseOr(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseAnd(o); }, {"||"});
    }
    bool parseAnd(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseEquality(o); }, {"&&"});
    }
    bool parseEquality(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseRelational(o); },
                              {"==", "!="});
    }
    bool parseRelational(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseAdditive(o); },
                              {"<=", ">=", "<", ">"});
    }
    bool parseAdditive(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseMultiplicative(o); },
                              {"+", "-"});
    }
    bool parseMultiplicative(std::unique_ptr<Expr>& out) {
        return parseLeftAssoc(out, [this](std::unique_ptr<Expr>& o) { return parseUnary(o); },
                              {"*", "/", "%"});
    }

    bool parseUnary(std::unique_ptr<Expr>& out) {
        if (m_tok.kind == Tok::Punct && (m_tok.text == "-" || m_tok.text == "!" || m_tok.text == "+")) {
            std::string op = m_tok.text;
            int line = m_tok.line, col = m_tok.col;
            next();
            const NestingScope nesting(m_exprDepth);
            if (nesting.tooDeep()) return fail("expression nesting too deep (max 64)");
            std::unique_ptr<Expr> operand;
            if (!parseUnary(operand)) return false;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Unary;
            e->text = op;
            e->line = line;
            e->col = col;
            if (op == "!") {
                if (operand->type != Ty::Bool) return fail("'!' needs a bool operand", line, col);
                e->type = Ty::Bool;
            } else {
                if (!isNumericType(operand->type))
                    return fail("'" + op + "' needs a numeric operand", line, col);
                e->type = operand->type;
            }
            e->args.push_back(std::move(operand));
            if (!setTreeDepth(*e)) return false;
            out = std::move(e);
            return true;
        }
        if (m_tok.kind == Tok::Punct && (m_tok.text == "++" || m_tok.text == "--")) {
            std::string op = m_tok.text;
            int line = m_tok.line, col = m_tok.col;
            next();
            const NestingScope nesting(m_exprDepth);
            if (nesting.tooDeep()) return fail("expression nesting too deep (max 64)");
            std::unique_ptr<Expr> operand;
            if (!parseUnary(operand)) return false;
            return makeIncDec(out, std::move(operand), op, true, line, col);
        }
        if (m_tok.kind == Tok::Punct && (m_tok.text == "~" || m_tok.text == "&" || m_tok.text == "*")) {
            return fail("operator '" + m_tok.text + "' is not in the subset");
        }
        return parsePostfix(out);
    }

    bool makeIncDec(std::unique_ptr<Expr>& out, std::unique_ptr<Expr> operand, const std::string& op,
                    bool prefix, int line, int col) {
        if (!isLvalue(*operand) || !isNumericType(operand->type) || componentCount(operand->type) != 1) {
            return fail("'" + op + "' needs a scalar variable", line, col);
        }
        if (!checkWritable(*operand, line, col)) return false;
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::IncDec;
        e->text = op;
        e->type = operand->type;
        e->prefix = prefix;
        e->line = line;
        e->col = col;
        e->args.push_back(std::move(operand));
        if (!setTreeDepth(*e)) return false;
        out = std::move(e);
        return true;
    }

    bool parsePostfix(std::unique_ptr<Expr>& out) {
        if (!parsePrimary(out)) return false;
        int terms = 1;  // T-257: every postfix step wraps the tree one level deeper
        for (;;) {
            if ((isPunct(".") || isPunct("[") || isPunct("++") || isPunct("--")) &&
                ++terms > kMaxChainTerms) {
                return fail("expression too long (max 256 terms)");
            }
            if (isPunct(".")) {
                int line = m_tok.line, col = m_tok.col;
                next();
                if (m_tok.kind != Tok::Ident) return fail("expected a member name after '.'");
                std::string member = m_tok.text;
                next();
                if (isPunct("(")) {
                    if (member == "Sample" || member == "SampleLevel" || member == "Load") {
                        return fail("texture Sample is not allowed (use xtSample)", line, col);
                    }
                    return fail("method calls are not in the subset", line, col);
                }
                if (!makeMember(out, member, line, col)) return false;
                continue;
            }
            if (isPunct("[")) return fail("arrays and indexing are not allowed");
            if (m_tok.kind == Tok::Punct && (m_tok.text == "++" || m_tok.text == "--")) {
                std::string op = m_tok.text;
                int line = m_tok.line, col = m_tok.col;
                next();
                if (!makeIncDec(out, std::move(out), op, false, line, col)) return false;
                continue;
            }
            return true;
        }
    }

    bool makeMember(std::unique_ptr<Expr>& out, const std::string& member, int line, int col) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Member;
        e->text = member;
        e->line = line;
        e->col = col;
        if (out->type == Ty::XtInput) {
            if (member == "uv" || member == "texelSize" || member == "layerSize") {
                e->type = Ty::Float2;
            } else {
                return fail("XtInput has no member '" + member + "' (uv, texelSize, layerSize)", line, col);
            }
        } else if (isFloatType(out->type)) {
            int n = componentCount(out->type);
            if (n == 1) {
                // T-208: GLSL 330 has no scalar swizzle, so `f.x` would only fail on the GPU.
                return fail("swizzle '." + member + "' on a scalar float (GLSL 330 has no scalar swizzle)",
                            line, col);
            }
            if (member.empty() || member.size() > 4) return fail("bad swizzle '." + member + "'", line, col);
            bool xyzw = true, rgba = true;
            for (char c : member) {
                int idx = -1;
                if (c == 'x' || c == 'r')
                    idx = 0;
                else if (c == 'y' || c == 'g')
                    idx = 1;
                else if (c == 'z' || c == 'b')
                    idx = 2;
                else if (c == 'w' || c == 'a')
                    idx = 3;
                if (idx < 0 || idx >= n)
                    return fail("bad swizzle '." + member + "' on " + hlslTypeName(out->type), line, col);
                if (c == 'r' || c == 'g' || c == 'b' || c == 'a')
                    xyzw = false;
                else
                    rgba = false;
            }
            if (!xyzw && !rgba) return fail("swizzle '." + member + "' mixes xyzw and rgba", line, col);
            e->type = floatVector(static_cast<int>(member.size()));
        } else {
            return fail(std::string("'.") + member + "' on a " + hlslTypeName(out->type), line, col);
        }
        e->args.push_back(std::move(out));
        if (!setTreeDepth(*e)) return false;
        out = std::move(e);
        return true;
    }

    bool parseArgs(std::vector<std::unique_ptr<Expr>>& args) {
        if (!expectPunct("(")) return false;
        if (acceptPunct(")")) return true;
        for (;;) {
            std::unique_ptr<Expr> a;
            if (!parseExpr(a)) return false;
            args.push_back(std::move(a));
            if (acceptPunct(",")) continue;
            return expectPunct(")");
        }
    }

    bool parsePrimary(std::unique_ptr<Expr>& out) {
        auto e = std::make_unique<Expr>();
        e->line = m_tok.line;
        e->col = m_tok.col;
        switch (m_tok.kind) {
            case Tok::Bad:
                return fail(m_tok.text);
            case Tok::End:
                return fail("unexpected end of body");
            case Tok::Int:
                e->kind = Expr::Kind::IntLit;
                e->type = Ty::Int;
                e->text = m_tok.text;
                next();
                out = std::move(e);
                return true;
            case Tok::Float:
                e->kind = Expr::Kind::FloatLit;
                e->type = Ty::Float;
                e->text = m_tok.text;
                next();
                out = std::move(e);
                return true;
            case Tok::Punct:
                if (isPunct("(")) {
                    next();
                    if (!parseExpr(out)) return false;
                    return expectPunct(")");
                }
                if (isPunct("{")) return fail("brace initializers are not allowed (use a constructor)");
                return fail("unexpected '" + m_tok.text + "'");
            case Tok::Ident:
                break;
        }
        const std::string name = m_tok.text;
        if (name == "true" || name == "false") {
            e->kind = Expr::Kind::BoolLit;
            e->type = Ty::Bool;
            e->text = name;
            next();
            out = std::move(e);
            return true;
        }
        if (const char* msg = forbiddenKeywordMessage(name)) return fail(msg);
        if (isTextureTypeName(name)) return fail("texture objects are not allowed (use xtSample)");
        Ty t;
        if (typeFromName(name, t)) {
            next();
            if (!isPunct("(")) return fail("expected '(' after type '" + name + "' (constructor)");
            e->kind = Expr::Kind::Ctor;
            e->text = name;
            if (!parseArgs(e->args)) return false;
            if (!setTreeDepth(*e)) return false;
            if (!checkCtor(*e, t)) return false;
            out = std::move(e);
            return true;
        }
        if (isForbiddenTypeName(name)) {
            return fail("type '" + name +
                        "' is not in the subset (float, float2, float3, float4, int, bool)");
        }
        // Function call?
        next();
        if (isPunct("(")) {
            const BuiltinSig* sig = builtinByName(name);
            if (sig == nullptr) {
                if (isKnownHlslIntrinsic(name))
                    return fail("'" + name + "' is not in the allowed function list", e->line, e->col);
                if (lookup(name) != nullptr)
                    return fail("'" + name + "' is a variable, not a function", e->line, e->col);
                return fail("unknown function '" + name + "'", e->line, e->col);
            }
            e->kind = Expr::Kind::Call;
            e->text = name;
            if (!parseArgs(e->args)) return false;
            if (!setTreeDepth(*e)) return false;
            if (!checkCall(*e, *sig)) return false;
            out = std::move(e);
            return true;
        }
        Symbol* s = lookup(name);
        if (s == nullptr) {
            if (builtinByName(name) != nullptr)
                return fail("'" + name + "' is a function; call it", e->line, e->col);
            return fail("unknown identifier '" + name + "'", e->line, e->col);
        }
        e->kind = Expr::Kind::Ident;
        e->text = name;
        e->type = s->type;
        out = std::move(e);
        return true;
    }

    bool checkCtor(Expr& e, Ty t) {
        int n = componentCount(t);
        if (e.args.empty()) return fail("constructor needs arguments", e.line, e.col);
        if (t == Ty::Int || t == Ty::Bool || t == Ty::Float) {
            if (e.args.size() != 1 || componentCount(e.args[0]->type) != 1 ||
                (!isNumericType(e.args[0]->type) && e.args[0]->type != Ty::Bool)) {
                return fail(std::string(hlslTypeName(t)) + "() takes one scalar", e.line, e.col);
            }
            e.type = t;
            return true;
        }
        int total = 0;
        for (const auto& a : e.args) {
            if (!isNumericType(a->type))
                return fail(std::string(hlslTypeName(t)) + "() arguments must be numeric", e.line, e.col);
            total += componentCount(a->type);
        }
        if (!(total == n || (e.args.size() == 1 && total == 1))) {
            return fail(std::string(hlslTypeName(t)) + "() needs " + std::to_string(n) + " components (got " +
                            std::to_string(total) + ")",
                        e.line, e.col);
        }
        e.type = t;
        return true;
    }

    bool checkCall(Expr& e, const BuiltinSig& sig) {
        if (static_cast<int>(e.args.size()) != sig.arity) {
            return fail(std::string(sig.name) + "() takes " + std::to_string(sig.arity) + " argument(s)",
                        e.line, e.col);
        }
        if (sig.resultRule == 3) {
            if (e.args[0]->type != Ty::Float2) return fail("xtSample(uv) needs a float2", e.line, e.col);
            e.type = Ty::Float4;
            return true;
        }
        // Unify: every argument numeric; vectors must agree; scalars broadcast.
        Ty unified = Ty::Int;
        bool anyFloat = false;
        for (const auto& a : e.args) {
            if (!isNumericType(a->type)) {
                return fail(std::string(sig.name) + "() arguments must be numeric", e.line, e.col);
            }
            if (a->type != Ty::Int) anyFloat = true;
            if (isVectorType(a->type)) {
                if (isVectorType(unified) && unified != a->type) {
                    return fail(std::string(sig.name) + "() vector arguments must have the same type", e.line,
                                e.col);
                }
                unified = a->type;
            }
        }
        if (!isVectorType(unified)) {
            bool intOnly =
                !anyFloat && (std::string_view(sig.name) == "abs" || std::string_view(sig.name) == "min" ||
                              std::string_view(sig.name) == "max" || std::string_view(sig.name) == "clamp");
            unified = intOnly ? Ty::Int : Ty::Float;
        }
        if (sig.resultRule == 2) {
            if (unified != Ty::Float3) return fail("cross() needs two float3", e.line, e.col);
        }
        // The emitter recomputes the unified type from the argument types (a pure function of
        // them, same rule as here) and promotes each argument to it.
        e.type = sig.resultRule == 1 ? Ty::Float : unified;
        return true;
    }

    LexerHlslSubset m_lexer;
    TokenHlslSubset m_tok;
    TokenHlslSubset m_peek;
    bool m_peekValid = false;
    std::span<const XtfxParam> m_params;
    XtfxError& m_err;
    std::vector<std::unordered_map<std::string, Symbol>> m_scopes;
    bool m_sawReturn = false;
    bool m_inForStep = false;
    int m_lastLine = 1;
    int m_lastCol = 1;
    /// T-257: current expression nesting (parentheses, unary operators, call arguments,
    /// assignment right-hand sides) and statement nesting, each capped at kMaxNesting.
    int m_exprDepth = 0;
    int m_blockDepth = 0;
};

}  // namespace

bool parseProgram(std::string_view body, int firstLine, std::span<const XtfxParam> params, Program& out,
                  XtfxError& err) {
    err = XtfxError{};
    out = Program{};
    Parser p(body, firstLine, params, err);
    if (!p.parse(out)) {
        if (err.message.empty()) err.message = "invalid body";
        return false;
    }
    return true;
}

}  // namespace xt::compile::xtfx
