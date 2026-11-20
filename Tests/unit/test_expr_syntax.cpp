#include <string>
#include <vector>

#include "doctest.h"
#include "expr_test_util.h"

/// T-766 (Docs/tasks/T-766.md, Docs/design/58-value-functions-and-drivers.md §58.2): the syntax of
/// timeline expressions -- every construct of the subset parses, constructs outside it get a
/// diagnostic that names them, the result is the last expression statement run.

using namespace expr_test;

namespace {

const std::vector<xe::ExprKey> kFlat = {key(0, 10)};

double at0(const std::string& text) { return eval(text, context(kFlat, 2), 0.0); }

}  // namespace

TEST_CASE("expr syntax: numbers, operators and precedence") {
    CHECK(at0("1.5e2") == doctest::Approx(150));
    CHECK(at0(".5 + 0.25") == doctest::Approx(0.75));
    CHECK(at0("1 + 2 * 3") == doctest::Approx(7));
    CHECK(at0("(1 + 2) * 3") == doctest::Approx(9));
    CHECK(at0("10 - 4 - 3") == doctest::Approx(3));
    CHECK(at0("12 / 3 / 2") == doctest::Approx(2));
    CHECK(at0("7 % 3") == doctest::Approx(1));
    CHECK(at0("-3 + +2") == doctest::Approx(-1));
    CHECK(at0("!0 + !5") == doctest::Approx(1));
    CHECK(at0("(2 < 3) + (3 <= 3) + (4 > 5) + (5 >= 6)") == doctest::Approx(2));
    CHECK(at0("(1 == 1) + (1 === 1) + (1 != 2) + (1 !== 1)") == doctest::Approx(3));
    CHECK(at0("0 || 7") == doctest::Approx(7));
    CHECK(at0("3 && 4") == doctest::Approx(4));
    CHECK(at0("0 && 1 / 0") == doctest::Approx(0));  // short-circuit: no division
    CHECK(at0("1 ? 2 : 3") == doctest::Approx(2));
    CHECK(at0("0 ? 2 : 1 ? 4 : 5") == doctest::Approx(4));
    CHECK(at0("true + false") == doctest::Approx(1));
    CHECK(at0("Math.PI") == doctest::Approx(3.14159265358979));
    CHECK(eval("thisComp.duration", context(kFlat, 7.5), 0) == doctest::Approx(7.5));
    CHECK(eval("index * 2", context(kFlat, 1, 4), 0) == doctest::Approx(8));
    CHECK(eval("value", context(kFlat, 1), 0) == doctest::Approx(10));
    CHECK(eval("time", context(kFlat, 3), 1.25) == doctest::Approx(1.25));
}

TEST_CASE("expr syntax: statements, comments and line breaks") {
    CHECK(at0("var a = 2; let b = 3; const c = 4; a * b + c") == doctest::Approx(10));
    CHECK(at0("var a = 1, b = 2\nb - a") == doctest::Approx(1));  // ASI at the line break
    CHECK(at0("x = 5; x += 2; x -= 1; x *= 3; x /= 2; x") == doctest::Approx(9));
    CHECK(at0("var y = 7; y %= 4; y") == doctest::Approx(3));
    CHECK(at0("var i = 1; i++; ++i; i") == doctest::Approx(3));
    CHECK(at0("var i = 5; var j = i--; j * 10 + i") == doctest::Approx(54));
    CHECK(at0("var i = 5; --i") == doctest::Approx(4));
    CHECK(at0("// a comment\nvar a = 1; /* block\ncomment */ a + 1") == doctest::Approx(2));
    CHECK(at0("if (1) { 10 } else { 20 }") == doctest::Approx(10));
    CHECK(at0("if (0) 10; else 20;") == doctest::Approx(20));
    CHECK(at0("var r = 0; if (0) { r = 1 } else if (1) { r = 2 } else { r = 3 } r") == doctest::Approx(2));
    CHECK(at0("var s = 0; for (var i = 0; i < 5; i++) { if (i == 1) continue; if (i == 4) break; s += i; } "
              "s") == doctest::Approx(5));
    CHECK(at0("var n = 0; while (n < 10) n += 3; n") == doctest::Approx(12));
    CHECK(at0("var k = 0; for (;;) { k++; if (k > 2) break; } k") == doctest::Approx(3));
    CHECK(at0("function sq(x) { return x * x; } sq(3) + sq(4)") == doctest::Approx(25));
    CHECK(at0("twice(5); function twice(v) { var t = v * 2; return t; }") == doctest::Approx(10));
    CHECK(at0("var g = 3; function addG(x) { return x + g; } addG(1)") == doctest::Approx(4));
    CHECK(at0("var g = 3; function setG() { g = 9; } setG(); g") == doctest::Approx(9));
    CHECK(at0("function a(x) { return b(x) + 1; } function b(x) { return x * 2; } a(3)") ==
          doctest::Approx(7));
    CHECK(at0(";;; 4") == doctest::Approx(4));
}

TEST_CASE("expr syntax: multi-line expression with var and if; the last expression is the result") {
    const std::vector<xe::ExprKey> keys = {key(0, 0), key(2, 20)};
    const std::string text =
        "var d = (index - 1) * 0.5;\n"
        "var v = valueAtTime(time - d);\n"
        "if (v > 5) {\n"
        "  v * 2\n"
        "} else {\n"
        "  v\n"
        "}\n";
    CHECK(eval(text, context(keys, 2, 2), 1.5) == doctest::Approx(20));  // v = 10 -> 20
    CHECK(eval(text, context(keys, 2, 2), 0.75) == doctest::Approx(2.5));
    CHECK(at0("1; 2; 3") == doctest::Approx(3));
    CHECK(at0("var a = 1; a + 1; a = 7") == doctest::Approx(7));  // an assignment statement is a result
    CHECK(at0("5; posterizeTime(12)") == doctest::Approx(5));     // posterizeTime has no value
    CHECK(at0("5; seedRandom(1, true)") == doctest::Approx(5));
    CHECK(at0("var r = 0; for (var i = 0; i < 3; i++) { i * 10 } ") == doctest::Approx(20));
}

TEST_CASE("expr syntax: arrays") {
    CHECK(at0("[1, 2, 3][1]") == doctest::Approx(2));
    CHECK(at0("var v = [3, 4]; v[0] * v[1]") == doctest::Approx(12));
    CHECK(at0("var v = [1, 2, 3, 4]; v.length") == doctest::Approx(4));
    CHECK(at0("([1, 2] + [10, 20])[1]") == doctest::Approx(22));
    CHECK(at0("([10, 20] - [1, 2])[0]") == doctest::Approx(9));
    CHECK(at0("([1, 2] * 3)[1]") == doctest::Approx(6));
    CHECK(at0("(2 * [1, 2])[0]") == doctest::Approx(2));
    CHECK(at0("([4, 8] / 4)[1]") == doctest::Approx(2));
    CHECK(at0("length([3, 4])") == doctest::Approx(5));
    CHECK(at0("length([1, 1], [4, 5])") == doctest::Approx(5));
    CHECK(at0("var v = [0, 0]; v[1] = 5; v[1]") == doctest::Approx(5));
    CHECK(at0("(-[1, 2])[1]") == doctest::Approx(-2));
    xe::ExprContext ctx = context(kFlat, 1);
    ctx.component = 1;
    CHECK(eval("[7, 8]", ctx, 0) == doctest::Approx(8));
}

TEST_CASE("expr syntax: run-time leaves parse and are marked") {
    for (const char* text :
         {"port(\"hp\") * 2", "layer(\"Label\").width + 24", "layer(\"Label\").height",
          "layer(\"A\").left + layer(\"A\").top", "randomPerPlay(0, 10)", "randomPerPlay()",
          "port(\"hp\").valueAtTime(time - 0.5)", "wiggle(port(\"f\"), 6)"}) {
        const xe::Program p = parse(text);
        CHECK_MESSAGE(p.hasRuntimeLeaf, text);
    }
    for (const char* text :
         {"wiggle(2, 6)", "layer(\"Logo\").y", "layer(index - 1).rotation.valueAtTime(time - 0.2)"}) {
        const xe::Program p = parse(text);
        CHECK_FALSE_MESSAGE(p.hasRuntimeLeaf, text);
    }
    CHECK(parse("layer(\"Logo\").y").hasLayerRef);
    CHECK(parse("posterizeTime(12); wiggle(2, 6)").hasPosterize);
}

TEST_CASE("expr syntax: constructs outside the subset name themselves") {
    struct Case {
        const char* text;
        const char* code;
        const char* contains;
    };
    const Case cases[] = {
        {"var o = {a: 1}; 1", "expr.unsupported", "objects"},
        {"new Array(3)", "expr.unsupported", "'new'"},
        {"switch (1) { }", "expr.unsupported", "switch"},
        {"try { 1 } catch (e) { 2 }", "expr.unsupported", "try"},
        {"do { 1 } while (0)", "expr.unsupported", "do ... while"},
        {"\"a\" + 1", "expr.unsupported", "strings can only"},
        {"var s = 'x'; 1", "expr.unsupported", "strings can only"},
        {"function f(x) { return f(x); } f(1)", "expr.recursion", "recursion"},
        {"function f(x) { return g(x); } function g(x) { return f(x); } f(1)", "expr.recursion", "recursion"},
        {"foo + 1", "expr.name", "'foo' is not defined"},
        {"bar(1)", "expr.name", "'bar' is not a function"},
        {"effect(\"Slider Control\")(\"Slider\")", "expr.unsupported", "port(\"name\")"},
        {"sourceRectAtTime().width", "expr.unsupported", "layer(\"name\").width"},
        {"layer(\"A\").sourceRectAtTime()", "expr.unsupported", "layer(\"name\").width"},
        {"thisComp.layer(\"Logo\").transform.position[1]", "expr.unsupported", "short form"},
        {"thisComp.width", "expr.unsupported", "thisComp.duration"},
        {"[1, 2, 3, 4, 5]", "expr.unsupported", "at most 4"},
        {"1 & 2", "expr.unsupported", "bitwise"},
        {"2 ** 3", "expr.unsupported", "Math.pow"},
        {"var f = (x) => x; 1", "expr.unsupported", "arrow"},
        {"if (1) { function g() { return 1; } } 1", "expr.unsupported", "top level"},
        {"Math.hypot(3, 4)", "expr.unsupported", "Math.hypot"},
        {"loopOut(\"bounce\")", "expr.syntax", "\"cycle\""},
        {"linear(1, 2, 3, 4)", "expr.syntax", "3 or 5 arguments"},
        {"wiggle(1)", "expr.syntax", "2 to 5 arguments"},
        {"Math.sin()", "expr.syntax", "1 argument"},
        {"layer(\"A\")", "expr.syntax", "row name"},
        {"return 5", "expr.syntax", "outside a function"},
        {"break", "expr.syntax", "outside a loop"},
        {"(1 + 2", "expr.syntax", "')'"},
        {"1 +", "expr.syntax", "end of the expression"},
        {"var time = 3; time", "expr.syntax", "built-in name"},
        {"value = 3", "expr.syntax", "left side"},
        {"1 = 2", "expr.syntax", "left side"},
        {"`x`", "expr.unsupported", "template strings"},
        {"0x10", "expr.unsupported", "hexadecimal"},
        {"thisProperty.valueAtTime(0)", "expr.unsupported", "valueAtTime"},
        {"3 4", "expr.syntax", "';'"},
        {"1, 2", "expr.unsupported", "comma"},
        {"for (var k in [1]) {}", "expr.unsupported", "for ... in"},
        {"port(5)", "expr.syntax", "in quotes"},
        {"randomPerPlay(1)", "expr.syntax", "0 or 2 arguments"},
        {"\"unterminated", "expr.syntax", "unterminated"},
        {"1 # 2", "expr.syntax", "unexpected character"},
        {"function f(a, a) { return a; } 1", "expr.syntax", "duplicate parameter"},
        {"function f(a) { return a; } f(1, 2)", "expr.syntax", "takes 1 argument"},
    };
    for (const Case& c : cases) {
        const xt::compile::Diagnostic d = parseError(c.text);
        INFO("expression: " << c.text << " -> " << d.code << ": " << d.message);
        CHECK(d.code == c.code);
        CHECK(d.message.find(c.contains) != std::string::npos);
        CHECK(d.line >= 1);
        CHECK(d.level == xt::compile::Diagnostic::Level::Warning);
    }
}

TEST_CASE("expr syntax: diagnostics carry the line and column") {
    const xt::compile::Diagnostic d = parseError("var a = 1;\nvar b = a +* 2;");
    CHECK(d.line == 2);
    CHECK(d.col == 12);
}

TEST_CASE("expr syntax: nesting limits are diagnostics, not crashes") {
    std::string deep(200, '(');
    deep += "1";
    deep += std::string(200, ')');
    CHECK(parseError(deep).code == "expr.limit");
    std::string longSum = "1";
    for (int i = 0; i < 400; ++i) {
        longSum += " + 1";
    }
    CHECK(parseError(longSum).code == "expr.limit");
    std::string ifs;
    for (int i = 0; i < 100; ++i) {
        ifs += "if (1) ";
    }
    ifs += "1";
    CHECK(parseError(ifs).code == "expr.limit");
    CHECK(parseError(std::string(70 * 1024, ' ') + "1").code == "expr.limit");
}
