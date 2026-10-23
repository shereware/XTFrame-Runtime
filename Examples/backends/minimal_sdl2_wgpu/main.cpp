// minimal_sdl2_wgpu: the smallest host on Code/Backends (45 §45.7, T-439) -- an SDL2 window drawn
// with WebGPU (xt_backend_sdl2_wgpu), one UI of a published Bundle, and the frame loop.
//   minimal_sdl2_wgpu <bundle dir> <ui id> [--frames N]   (N > 0: hidden window, N frames, exit)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "xt/bundle/bundle.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/sw/sw_png.h"
#include "xt_backend.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: minimal_sdl2_wgpu <bundle dir> <ui id> [--frames N]\n");
        return 2;
    }
    const int frames = argc > 4 && std::strcmp(argv[3], "--frames") == 0 ? std::atoi(argv[4]) : 0;
    std::string error;
    auto bundle = xt::bundle::Bundle::open(argv[1], xt::sw::decodeImageBytes, &error);
    if (!bundle.ok()) {
        std::fprintf(stderr, "minimal_sdl2_wgpu: cannot open bundle '%s': %s\n", argv[1], error.c_str());
        return 1;
    }
    xt::backend::Backend backend;
    xt::backend::BackendOptions options;
    options.title = "XTFrame - minimal_sdl2_wgpu";
    options.hidden = frames > 0;
    if (!backend.create(options)) {
        std::fprintf(stderr, "minimal_sdl2_wgpu: %s\n", backend.lastError());
        return frames > 0 ? 0 : 1;  // a CI run without a GPU has nothing to show
    }
    xt::Context context(xt::ContextConfig{nullptr, &bundle.value()->assets()});
    auto ui = xt::bundle::loadUi(context, *bundle.value(), argv[2]);
    if (!ui.ok()) {
        std::fprintf(stderr, "minimal_sdl2_wgpu: cannot load ui '%s' (%s)\n", argv[2],
                     xt::statusName(ui.status()));
        return 1;
    }
    xt::CommandList list;
    double last = backend.seconds();
    for (int frame = 0; (frames <= 0 || frame < frames) && backend.pumpEvents(context); ++frame) {
        const double now = backend.seconds();
        context.update(static_cast<float>(now - last));
        last = now;
        context.render(list);
        backend.beginFrame();
        backend.render(list, nullptr, &bundle.value()->assets(), &context.document(ui.value())->ir());
        backend.present();
    }
    std::printf("minimal_sdl2_wgpu: done (%s)\n", backend.name());
    return 0;
}
