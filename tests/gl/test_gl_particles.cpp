#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "gl_loader.h"
#include "golden_common.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/gl/gl_renderer.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"

/// T-202 (docs/tasks/T-202.md items 5 and 8): the golden particle fixture
/// (tests/golden/ui/ui_particles.xt at 0.5 s: additive solid sparks, textured snow, a 4-stop ramp,
/// a capped burst) drawn by xt::gl::GlRenderer's instanced DrawKind::Particles path and by
/// xt::sw::SwRenderer, compared with docs/design/15-testing.md §15.3's tolerance. Skips (does not
/// fail) when no GL context is available, per test_gl_main.cpp's rule.

using namespace xt;

namespace {

constexpr int kW = 256;
constexpr int kH = 256;

struct Target {
    gl::GLuint fbo = 0, color = 0, rbo = 0;

    void init() {
        using namespace xt::gl;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kW, kH);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }

    void clear() {
        using namespace xt::gl;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kW, kH);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(1, 1, 1, 1);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    std::vector<uint8_t> readTopDown() const {
        using namespace xt::gl;
        std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        std::vector<uint8_t> out(pixels.size());
        const size_t row = static_cast<size_t>(kW) * 4;
        for (int y = 0; y < kH; ++y) {
            std::copy(pixels.begin() + static_cast<long>(static_cast<size_t>(kH - 1 - y) * row),
                      pixels.begin() + static_cast<long>(static_cast<size_t>(kH - y) * row),
                      out.begin() + static_cast<long>(static_cast<size_t>(y) * row));
        }
        return out;
    }

    void destroy() {
        using namespace xt::gl;
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (rbo != 0) glDeleteRenderbuffers(1, &rbo);
        if (color != 0) glDeleteTextures(1, &color);
        fbo = rbo = color = 0;
    }
};

void* sdlLoadProc(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

}  // namespace

TEST_CASE("gl: DrawParticles (ui_particles.xt at 0.5 s) matches the software renderer") {
    const std::string dir = XT_GOLDEN_UI_DIR;
    std::ifstream in(dir + "/ui_particles.xt", std::ios::binary);
    REQUIRE(in.good());
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    compile::CompileOptions options;
    options.emitMeta = false;
    options.rootDir = dir;
    std::vector<compile::SourceFile> files{{"ui_particles.xt", source}};
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "ui_particles.xt", options);
    REQUIRE(result.ok());
    FileAssetResolver assets(dir, sw::decodeImageBytes);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, false, nullptr, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, kW, kH});
    doc->update(0);
    doc->update(0.5f);
    CommandList list;
    doc->paint(list);
    list.viewport = Rect{0, 0, kW, kH};
    list.devicePixelRatio = 1;
    size_t instances = 0;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::DrawParticles) {
            instances += list.particles[c.index].count;
        }
    }
    CHECK(list.particles.size() == 4);
    CHECK(instances > 40);

    std::vector<uint8_t> swPixels(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer sw;
    sw::SwTarget swTarget{swPixels.data(), kW, kH, kW * 4};
    sw.setTarget(swTarget);
    sw.clear(Color{1, 1, 1, 1});
    sw.render(list, nullptr, &assets, &doc->ir());

    static bool loaded = false;
    if (!loaded) {
        REQUIRE(gl::loadGl(sdlLoadProc));
        loaded = true;
    }
    Target target;
    target.init();
    gl::GlRenderer renderer;
    REQUIRE(renderer.init(sdlLoadProc));
    renderer.resize(kW, kH);
    target.clear();
    renderer.render(list, nullptr, &assets, &doc->ir());
    std::vector<uint8_t> glPixels = target.readTopDown();
    renderer.shutdown();
    target.destroy();

    golden::GoldenResult cmp = golden::compareRgba(glPixels.data(), swPixels.data(), kW, kH, 16, 0.03f);
    std::printf("gl particles: diffPixels=%d maxChannelDiff=%d\n", static_cast<int>(cmp.diffPixels),
                static_cast<int>(cmp.maxChannelDiff));
    CHECK(cmp.pass);
}
