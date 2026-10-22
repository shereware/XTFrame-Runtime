#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"

/// Tests for the `.xt` document parser. See Docs/design/05-style-and-css.md §5.1-5.2,
/// Docs/tasks/T-21.md.

using namespace xt::compile;

namespace {

std::string loadFixture(const std::string& name) {
    std::string path = std::string(XT_TEST_XT_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool hasErrorContaining(const DiagnosticSink& sink, const std::string& needle) {
    for (const Diagnostic& d : sink.all()) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasWarningContaining(const DiagnosticSink& sink, const std::string& needle) {
    for (const Diagnostic& d : sink.all()) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const Diagnostic* firstErrorContaining(const DiagnosticSink& sink, const std::string& needle) {
    for (const Diagnostic& d : sink.all()) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return &d;
        }
    }
    return nullptr;
}

const Diagnostic* firstWarningContaining(const DiagnosticSink& sink, const std::string& needle) {
    for (const Diagnostic& d : sink.all()) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("parseXt: minimal document") {
    std::string text = loadFixture("min.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "min.xt", sink, tree));
    CHECK_FALSE(sink.hasErrors());
    REQUIRE(tree.nodes.size() == 2);
    CHECK(tree.nodes[0].tag == "xt");
    CHECK(tree.nodes[1].tag == "body");
    CHECK(tree.body == 1);
    CHECK(tree.nodes[1].parent == 0);
}

TEST_CASE("parseXt: 5.1 example document") {
    std::string text = loadFixture("sample.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "sample.xt", sink, tree));
    CHECK_FALSE(sink.hasErrors());

    REQUIRE(tree.nodes.size() == 8);
    CHECK(tree.body == 1);
    CHECK(tree.nodes[0].tag == "xt");
    CHECK(tree.nodes[1].tag == "body");
    CHECK(tree.nodes[2].tag == "div");
    CHECK(tree.nodes[3].tag == "span");
    CHECK(tree.nodes[4].tag == "img");
    CHECK(tree.nodes[5].tag == "anim");
    CHECK(tree.nodes[6].tag == "input");
    CHECK(tree.nodes[7].tag == "button");

    CHECK(tree.linkHrefs.size() == 1);
    CHECK(tree.linkHrefs[0] == "theme.xts");
    REQUIRE(tree.inlineStyles.size() == 1);
    CHECK(tree.inlineStyles[0].first.find("color: #fff") != std::string::npos);
    CHECK(tree.inlineStyles[0].second == 4);
    CHECK(tree.title == "Demo");

    const DomNode& div = tree.nodes[2];
    REQUIRE(div.attrs.size() == 3);
    CHECK(div.attr("id")->value == "root");
    CHECK(div.attr("class")->value == "panel large");
    CHECK(div.attr("style")->value == "width: 200px");
    CHECK(div.children.size() == 5);

    const DomNode& span = tree.nodes[3];
    CHECK(span.hasText);
    CHECK(span.text == "Hello");

    const DomNode& img = tree.nodes[4];
    CHECK(img.attr("src")->value == "images/logo.png");

    const DomNode& anim = tree.nodes[5];
    CHECK(anim.attr("src")->value == "anims/loading.json");
    CHECK(anim.attr("autoplay")->value == "true");
    CHECK(anim.attr("loop")->value == "true");

    const DomNode& input = tree.nodes[6];
    CHECK(input.attr("type")->value == "text");
    CHECK(input.attr("value")->value == "");

    const DomNode& button = tree.nodes[7];
    CHECK(button.attr("disabled")->value == "true");
    CHECK(button.hasText);
    CHECK(button.text == "OK");
}

TEST_CASE("parseXt: entity decoding") {
    std::string text = "<xt><body><p>&lt;b&gt; &amp; &quot;q&quot; &#65;&#x42;</p></body></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "<mem>", sink, tree));
    CHECK_FALSE(sink.hasErrors());
    const DomNode& p = tree.nodes[2];
    CHECK(p.tag == "p");
    CHECK(p.hasText);
    CHECK(p.text == "<b> & \"q\" AB");
}

TEST_CASE("parseXt: unknown entity warns and keeps original text") {
    std::string text = "<xt><body><p>a&nope;b</p></body></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "<mem>", sink, tree));
    CHECK(hasWarningContaining(sink, "unknown entity"));
    CHECK(tree.nodes[2].text == "a&nope;b");
}

TEST_CASE("parseXt: whitespace folding") {
    std::string text = "<xt><body><p>  a \n\t b  </p></body></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "<mem>", sink, tree));
    CHECK(tree.nodes[2].hasText);
    CHECK(tree.nodes[2].text == "a b");
}

TEST_CASE("parseXt: pure whitespace text is not text") {
    std::string text = "<xt><body><p>   \n\t  </p></body></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "<mem>", sink, tree));
    CHECK_FALSE(tree.nodes[2].hasText);
    CHECK(tree.nodes[2].text.empty());
}

TEST_CASE("parseXt: self-closing and explicit closing are equivalent") {
    std::string a = loadFixture("self_close_a.xt");
    std::string b = loadFixture("self_close_b.xt");
    DiagnosticSink sinkA, sinkB;
    DomTree treeA, treeB;
    REQUIRE(parseXt(a, "a.xt", sinkA, treeA));
    REQUIRE(parseXt(b, "b.xt", sinkB, treeB));
    REQUIRE(treeA.nodes.size() == treeB.nodes.size());
    for (size_t i = 0; i < treeA.nodes.size(); ++i) {
        const DomNode& na = treeA.nodes[i];
        const DomNode& nb = treeB.nodes[i];
        CHECK(na.tag == nb.tag);
        CHECK(na.text == nb.text);
        CHECK(na.hasText == nb.hasText);
        CHECK(na.children == nb.children);
        CHECK(na.parent == nb.parent);
        REQUIRE(na.attrs.size() == nb.attrs.size());
        for (size_t a2 = 0; a2 < na.attrs.size(); ++a2) {
            CHECK(na.attrs[a2].name == nb.attrs[a2].name);
            CHECK(na.attrs[a2].value == nb.attrs[a2].value);
        }
    }
}

TEST_CASE("parseXt: error - mismatched closing tag") {
    std::string text = loadFixture("err_mismatch.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_mismatch.xt", sink, tree));
    REQUIRE(sink.hasErrors());
    const Diagnostic* d = firstErrorContaining(sink, "mismatched closing tag </span>, expected </div>");
    REQUIRE(d != nullptr);
    CHECK(d->line == 3);
}

TEST_CASE("parseXt: error - unclosed tag at EOF") {
    std::string text = loadFixture("err_unclosed.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_unclosed.xt", sink, tree));
    const Diagnostic* d = firstErrorContaining(sink, "unclosed tag <div>");
    REQUIRE(d != nullptr);
    CHECK(d->line == 3);
}

TEST_CASE("parseXt: error - unquoted attribute value") {
    std::string text = loadFixture("err_unquoted_attr.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_unquoted_attr.xt", sink, tree));
    const Diagnostic* d = firstErrorContaining(sink, "attribute value must be quoted");
    REQUIRE(d != nullptr);
    CHECK(d->line == 3);
}

TEST_CASE("parseXt: error - missing body") {
    std::string text = loadFixture("err_missing_body.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_missing_body.xt", sink, tree));
    CHECK(hasErrorContaining(sink, "missing <body> element"));
}

TEST_CASE("parseXt: error - root element must be xt") {
    std::string text = loadFixture("err_root_not_xt.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_root_not_xt.xt", sink, tree));
    const Diagnostic* d = firstErrorContaining(sink, "root element must be <xt>");
    REQUIRE(d != nullptr);
    CHECK(d->line == 1);
}

TEST_CASE("parseXt: error - mixes text and child elements") {
    std::string text = loadFixture("err_mixed_text_child.xt");
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "err_mixed_text_child.xt", sink, tree));
    const Diagnostic* d = firstErrorContaining(sink, "mixes text and child elements");
    REQUIRE(d != nullptr);
    CHECK(d->line == 3);
}

TEST_CASE("parseXt: warning - unknown tag rewritten to div") {
    std::string text = loadFixture("warn_unknown_tag.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "warn_unknown_tag.xt", sink, tree));
    const Diagnostic* w = firstWarningContaining(sink, "unknown tag <foo>, treated as div");
    REQUIRE(w != nullptr);
    CHECK(w->line == 3);

    const DomNode& node = tree.nodes[2];
    CHECK(node.tag == "div");
    const DomAttr* rewritten = node.attr("data-xt-tag");
    REQUIRE(rewritten != nullptr);
    CHECK(rewritten->value == "foo");
    CHECK(node.attr("bar")->value == "1");
}

TEST_CASE("parseXt: warning - illegal tag inside head is ignored") {
    std::string text = loadFixture("warn_head_illegal.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "warn_head_illegal.xt", sink, tree));
    const Diagnostic* w = firstWarningContaining(sink, "illegal tag <meta> in <head>");
    REQUIRE(w != nullptr);
    CHECK(w->line == 3);
    // <head> never produces a node; only <xt> and <body> should exist.
    REQUIRE(tree.nodes.size() == 2);
}

TEST_CASE("parseXt: warning - duplicate attribute keeps last value") {
    std::string text = loadFixture("warn_dup_attr.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "warn_dup_attr.xt", sink, tree));
    const Diagnostic* w = firstWarningContaining(sink, "duplicate attribute \"id\"");
    REQUIRE(w != nullptr);
    CHECK(w->line == 3);

    const DomNode& div = tree.nodes[2];
    REQUIRE(div.attrs.size() == 1);
    CHECK(div.attrs[0].value == "b");
}

TEST_CASE("parseXt: warning - DOCTYPE is an ignored declaration") {
    std::string text = loadFixture("warn_doctype.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "warn_doctype.xt", sink, tree));
    const Diagnostic* w = firstWarningContaining(sink, "ignored declaration");
    REQUIRE(w != nullptr);
    CHECK(w->line == 1);
}

TEST_CASE("parseXt: comments spanning lines with '>' and raw <style> content") {
    std::string text = loadFixture("comment_style.xt");
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "comment_style.xt", sink, tree));
    CHECK_FALSE(sink.hasErrors());

    REQUIRE(tree.inlineStyles.size() == 1);
    const std::string& css = tree.inlineStyles[0].first;
    CHECK(css.find(".a > .b") != std::string::npos);
    CHECK(css.find("a < b") != std::string::npos);

    REQUIRE(tree.nodes.size() == 3);
    CHECK(tree.nodes[1].tag == "body");
    CHECK(tree.nodes[1].children.size() == 1);
    CHECK(tree.nodes[tree.nodes[1].children[0]].tag == "div");
    CHECK(tree.nodes[tree.nodes[1].children[0]].text == "ok");
}

TEST_CASE("parseXt: nesting depth over 256 is an error") {
    std::string text = "<xt><body>";
    for (int i = 0; i < 300; ++i) {
        text += "<div>";
    }
    for (int i = 0; i < 300; ++i) {
        text += "</div>";
    }
    text += "</body></xt>";

    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "<mem>", sink, tree));
    CHECK(hasErrorContaining(sink, "maximum nesting depth"));
}

TEST_CASE("parseXt: 10000 sibling elements parse quickly") {
    std::string text = "<xt><body>";
    for (int i = 0; i < 10000; ++i) {
        text += "<div/>";
    }
    text += "</body></xt>";

    DiagnosticSink sink;
    DomTree tree;
    std::clock_t start = std::clock();
    REQUIRE(parseXt(text, "<mem>", sink, tree));
    std::clock_t elapsed = std::clock() - start;
    double ms = 1000.0 * static_cast<double>(elapsed) / static_cast<double>(CLOCKS_PER_SEC);
    // A guard against a quadratic parse, not a benchmark: the bound is wide and only asserted in
    // Release, because a Debug run on a loaded machine has exceeded a tight one.
#ifdef NDEBUG
    CHECK(ms < 2000.0);
#else
    static_cast<void>(ms);
#endif
    CHECK(tree.nodes[1].children.size() == 10000);
}

TEST_CASE("T-406 <xt version>: missing = 1, newer or malformed is an error, never in the IR") {
    const char* body = "<head></head><body><div id=\"a\">x</div></body></xt>";
    for (const char* root : {"<xt>", "<xt version=\"1\">"}) {
        DiagnosticSink sink;
        DomTree tree;
        CHECK(parseXt(std::string(root) + body, "v.xt", sink, tree));
    }
    {
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(parseXt(std::string("<xt version=\"2\">") + body, "v.xt", sink, tree));
        CHECK(hasErrorContaining(sink, "a newer XTFrame is needed"));
    }
    for (const char* bad :
         {"<xt version=\"0\">", "<xt version=\"one\">", "<xt version=\"\">", "<xt version=\"-1\">"}) {
        DiagnosticSink sink;
        DomTree tree;
        CHECK_FALSE(parseXt(std::string(bad) + body, "v.xt", sink, tree));
        CHECK(hasErrorContaining(sink, "is not a positive integer"));
    }
    // The same document compiles to the same bytes with and without the attribute (padded to the
    // same length: the IR keeps source positions).
    const std::string plainText = std::string("<xt            >") + body;
    const std::string versionedText = std::string("<xt version=\"1\">") + body;
    const SourceFile plain{"v.xt", plainText};
    const SourceFile versioned{"v.xt", versionedText};
    CompileOptions options;
    options.emitMeta = false;  // its timestamp differs between two runs
    const CompileResult a = compileDocumentFromStrings({&plain, 1}, "v.xt", options);
    const CompileResult b = compileDocumentFromStrings({&versioned, 1}, "v.xt", options);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.ir == b.ir);
    CHECK(kXtFormatVersion == 1);
}
