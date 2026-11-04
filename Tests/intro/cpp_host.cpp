// The C++ host of Docs/intro/for-programmers.md ("Drive it from C++"). The block between the
// snippet markers is copied into the intro word for word; intro_snippets checks that it still is.
// Usage: intro_cpp_host <hud.xtir>  (the file xtframe-compile wrote)

#include <xt/xt.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: intro_cpp_host <hud.xtir>\n");
        return 2;
    }
    const char* xtirPath = argv[1];

    // snippet:begin
    xt::Context ctx(xt::ContextConfig{});  // no shaper, no assets: built-in defaults
    xt::Result<xt::DocumentHandle> loaded = ctx.loadDocumentFile(xtirPath);
    if (!loaded.ok()) {
        std::fprintf(stderr, "load failed: %s\n", xt::statusName(loaded.status()));
        return 1;
    }
    xt::Document* doc = ctx.document(loaded.value());
    ctx.setViewport(xt::Rect{0, 0, 1280, 720}, /*devicePixelRatio=*/1.0f);

    const xt::PortId hp = doc->lookupPort("hp");  // the `port:hp` of hud.xt

    xt::CommandList list;
    for (int frame = 0; frame < 3; ++frame) {
        // Game state -> UI. Written before update(), so it shows this frame.
        doc->setPort(hp, xt::Value::ofFloat(42.0f));

        xt::InputEvent move;  // 1. forward your window's input
        move.kind = xt::InputEvent::PointerMove;
        move.pos = xt::Vec2{100, 40};
        ctx.pushInput(move);

        ctx.update(1.0f / 60.0f);  // 2. triggers, bindings, animation, layout
        list.clear();
        ctx.render(list);  // 3. draw commands for your renderer
    }
    // snippet:end

    const std::string text(doc->byId("HpText").text());
    std::printf("intro_cpp_host: HpText=%s commands=%zu\n", text.c_str(), list.commands.size());
    return text == "42" && !list.commands.empty() ? 0 : 1;
}
