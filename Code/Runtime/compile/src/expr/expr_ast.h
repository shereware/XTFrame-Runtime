#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// The syntax tree of one timeline expression (Docs/design/58-value-functions-and-drivers.md §58.2,
/// Docs/tasks/T-766.md): an arena of nodes addressed by index, built by expr_parser.cpp with every
/// name already resolved to a variable slot, a built-in function or a local function.

namespace xt::compile::expr {

/// The built-in functions and run-time leaves a Call node can name (58 §58.2's table, §58.5).
enum class Builtin : uint8_t {
    None = 0,
    Wiggle,
    LoopOut,
    LoopIn,
    ValueAtTime,
    Linear,
    Ease,
    EaseIn,
    EaseOut,
    Clamp,
    Random,
    SeedRandom,
    Noise,
    PosterizeTime,
    DegreesToRadians,
    RadiansToDegrees,
    Length,
    MathSin,
    MathCos,
    MathTan,
    MathAsin,
    MathAcos,
    MathAtan,
    MathAtan2,
    MathAbs,
    MathRound,
    MathFloor,
    MathCeil,
    MathMin,
    MathMax,
    MathPow,
    MathSqrt,
    MathExp,
    MathLog,
    // Run-time leaves (58 §58.5): they parse, and lowerExpression() answers NeedsRuntime.
    Port,           ///< port("name"): a number the game sends in.
    RandomPerPlay,  ///< randomPerPlay(), randomPerPlay(min, max): one draw per play.
};

/// What a Node is. Expression kinds first, statement kinds after.
enum class NodeKind : uint8_t {
    Number,       ///< `number`.
    String,       ///< `slot` = index into Program::strings (only as an argument of a few calls).
    Var,          ///< A variable: `slot`, `flags` & kFlagLocal for a function local.
    Time,         ///< `time`.
    Value,        ///< `value`: this row at `time`.
    ValueVec,     ///< `valueVec`: [x, y] of the property's rows at `time`.
    Index,        ///< `index`.
    Duration,     ///< `thisComp.duration`.
    Unary,        ///< `op` = UnaryOp, `a` = operand.
    Binary,       ///< `op` = BinaryOp, `a` `b`.
    Logical,      ///< `op` = LogicalOp, `a` `b` (short-circuit, JS semantics: yields an operand).
    Conditional,  ///< `a ? b : c`.
    Call,         ///< `op` = Builtin, arguments in the list.
    LocalCall,    ///< `slot` = Program::functions index, arguments in the list.
    Array,        ///< `[e0, e1, ...]` (up to 4 numbers), elements in the list.
    Element,      ///< `a[b]`.
    ArrayLength,  ///< `a.length`.
    Assign,       ///< `op` = AssignOp, `a` = target (Var or Element), `b` = value.
    IncDec,       ///< `op` = IncDecOp, `a` = target (Var or Element).
    LayerProp,    ///< `layer(a).<strings[slot]>`: another row (T-770).
    LayerPropAt,  ///< `layer(a).<strings[slot]>.valueAtTime(b)`.
    LayerLayout,  ///< `layer(a).width/height/left/top`, `op` = LayoutWhich (run-time leaf).
    PortAt,       ///< `port(a).valueAtTime(b)` (run-time leaf).
    // Statements.
    VarDecl,   ///< `var <slot> = a;` (`a` = -1 without an initialiser).
    ExprStmt,  ///< `a;`
    If,        ///< `if (a) b else c` (`c` = -1 without else).
    For,       ///< `for (a; b; c) d` (any of a, b, c may be -1).
    While,     ///< `while (a) d`.
    Block,     ///< `{ ... }`, statements in the list.
    Break,     ///< `break;`
    Continue,  ///< `continue;`
    Return,    ///< `return a;` (`a` = -1 for a bare return).
    Function,  ///< A function declaration (`slot` = Program::functions index); no effect when run.
    Empty,     ///< `;`
};

/// Unary operators.
enum class UnaryOp : uint8_t { Neg, Plus, Not };
/// Binary operators (`===` / `!==` are Eq / Ne: there are only numbers).
enum class BinaryOp : uint8_t { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };
/// `&&` and `||`.
enum class LogicalOp : uint8_t { And, Or };
/// `=` and the compound assignments.
enum class AssignOp : uint8_t { Set, Add, Sub, Mul, Div, Mod };
/// `++x`, `--x`, `x++`, `x--`.
enum class IncDecOp : uint8_t { PreInc, PreDec, PostInc, PostDec };
/// Which layout result a LayerLayout node reads.
enum class LayoutWhich : uint8_t { Left = 0, Top = 1, Width = 2, Height = 3 };

/// Node::flags bit: a Var node names a slot of the enclosing function's frame, not a global.
inline constexpr uint8_t kFlagLocal = 1;

/// One node of the arena. Unused child links are -1.
struct Node {
    NodeKind kind = NodeKind::Empty;
    uint8_t op = 0;     ///< Operator, Builtin or LayoutWhich, by kind.
    uint8_t flags = 0;  ///< kFlagLocal.
    int32_t a = -1;     ///< First child.
    int32_t b = -1;     ///< Second child.
    int32_t c = -1;     ///< Third child.
    int32_t d = -1;     ///< Fourth child (loop bodies).
    int32_t first = 0;  ///< Start of the child list in Program::lists.
    int32_t count = 0;  ///< Length of the child list.
    double number = 0;  ///< Number literal.
    int32_t slot = -1;  ///< Variable slot, string index or function index, by kind.
    uint32_t site = 0;  ///< Call site number (Call nodes; the seed of wiggle / noise / random).
    int32_t line = 1;   ///< 1-based source line.
    int32_t col = 1;    ///< 1-based source column.
};

/// A local function: `function name(params) { body }`.
struct Function {
    std::string name;
    int32_t body = -1;       ///< Block node.
    int32_t paramCount = 0;  ///< Parameters are locals 0 .. paramCount - 1.
    int32_t localCount = 0;  ///< Parameters plus declared and implicit locals.
    int32_t line = 1;
};

/// A parsed, resolved expression.
struct Program {
    std::vector<Node> nodes;
    std::vector<int32_t> lists;        ///< Child lists (Call arguments, Array elements, Block statements).
    std::vector<std::string> strings;  ///< String literals and property names.
    std::vector<Function> functions;
    std::vector<std::string> globalNames;  ///< One per global slot.
    int32_t root = -1;                     ///< The top-level Block.
    uint32_t siteCount = 0;                ///< Call sites numbered so far.
    bool hasRuntimeLeaf = false;  ///< port(), randomPerPlay(), layer().width/..., port().valueAtTime().
    bool hasLayerRef = false;     ///< layer(...).<row> (T-770).
    bool hasPosterize = false;    ///< posterizeTime(...) appears.
    std::string text;             ///< The source text (kept for diagnostics).

    /// The child list of `n`.
    const int32_t* list(const Node& n) const { return lists.data() + n.first; }
};

}  // namespace xt::compile::expr
