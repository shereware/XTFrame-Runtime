// The headless test of Docs/intro/for-programmers.md ("Test it headless"): compiles hud.xt in
// process, drives the port and checks what the bindings did. No window, no GPU, no fonts. The
// block between the snippet markers is copied into the intro; intro_snippets checks it.

#include <xt/compile/compile.h>
#include <xt/xt.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef XT_INTRO_HUD
#error "XT_INTRO_HUD (the path of Tests/intro/hud/hud.xt) must be defined by Tests/intro/CMakeLists.txt"
#endif

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "intro_headless_test: FAILED: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    // snippet:begin
    xt::compile::CompileResult compiled = xt::compile::compileDocument(XT_INTRO_HUD, {});
    for (const xt::compile::Diagnostic& d : compiled.diagnostics) {
        std::fprintf(stderr, "%s\n", d.message.c_str());
    }
    if (!compiled.ok()) {
        return 1;
    }

    xt::Context ctx(xt::ContextConfig{});
    xt::DocumentHandle handle =
        ctx.loadDocument(xt::IrBlob{compiled.ir.data(), compiled.ir.size()}, /*takeOwnership=*/true).value();
    xt::Document* doc = ctx.document(handle);
    ctx.setViewport(xt::Rect{0, 0, 1280, 720}, 1.0f);
    ctx.update(0.0f);

    xt::ElementRef hpText = doc->byId("HpText");
    check(hpText.text() == "100", "the port default shows");

    doc->setPort(doc->lookupPort("hp"), xt::Value::ofFloat(17.4f));
    ctx.update(1.0f / 60.0f);
    check(hpText.text() == "17", "bind:text rounds the port");
    check(hpText.hasClass("low"), "bind:class.low turns on below 30");
    // snippet:end

    if (failures == 0) {
        std::printf("intro_headless_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
