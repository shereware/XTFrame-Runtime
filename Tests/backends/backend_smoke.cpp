#include <cstdio>
#include <cstring>
#include <string>

#include "xt/command_list.h"
#include "xt/context.h"
#include "xt_backend.h"

/// T-439 item 8: one combination library (`xt_backend_<platform>_<renderer>`) linked alone, as an
/// example would, and driven through xt_backend.h: create() a hidden window, pump, one frame of a
/// Context with no document, present, destroy. A missing WebGPU adapter (or a video driver without
/// native windows, e.g. SDL's offscreen) is a SKIP (exit 0); any other create() failure exits 1.
/// Built once per combination by Tests/backends/CMakeLists.txt (XT_SMOKE_NAME = the combination).

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    xt::backend::Backend backend;
    xt::backend::BackendOptions options;
    options.title = "xt_backend_smoke";
    options.width = 160;
    options.height = 120;
    options.hidden = true;
    options.vsync = false;
    if (!backend.create(options)) {
        const std::string why = backend.lastError();
        if (why.find("no WebGPU adapter") != std::string::npos ||
            why.find("no native handle") != std::string::npos) {
            std::printf("SKIP: backend %s: %s\n", XT_SMOKE_NAME, why.c_str());
            return 0;
        }
        std::printf("backend %s: create failed: %s\n", XT_SMOKE_NAME, why.c_str());
        return 1;
    }
    xt::Context context(xt::ContextConfig{});
    int frames = 0;
    for (; frames < 3; ++frames) {
        if (!backend.pumpEvents(context)) break;
        context.update(frames == 0 ? 0.0f : 1.0f / 60.0f);
        xt::CommandList list;
        context.render(list);
        backend.beginFrame();
        backend.render(list, nullptr, nullptr);
        backend.present();
    }
    std::printf("backend %s: %dx%d logical, dpr %.2f, %d frames, error '%s'\n", backend.name(),
                backend.width(), backend.height(), static_cast<double>(backend.devicePixelRatio()), frames,
                backend.lastError());
    const bool ok = frames == 3 && std::strcmp(backend.name(), XT_SMOKE_NAME) == 0 && backend.width() > 0 &&
                    backend.lastError()[0] == 0;
    backend.destroy();
    std::printf("%s\n", ok ? "smoke ok" : "smoke FAILED");
    return ok ? 0 : 1;
}
