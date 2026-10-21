#pragma once

#include "gl_loader.h"

/// saveState/restoreState (docs/design/10-backends.md §10.3.5): GlRenderer::render() must leave
/// the GL context's externally-visible state exactly as it found it, so a host embedding xt_gl
/// into a larger GL scene isn't disturbed.
///
/// Not covered by docs/tasks/T-19.md verbatim (the sheet just says "10.3.5 清單" without listing
/// it in the file set given to this task); this is a conservative, reasonably complete list of
/// state xt_gl itself touches: current program, VAO/array-buffer bindings, framebuffer bindings,
/// blend/stencil/depth/cull/scissor enable flags and their parameters, viewport, color mask,
/// clear color, active texture unit and its bound 2D texture, and pixel store alignment.
/// T-263b (E5): also the 2D texture bound on each of units 0..2, both stencil faces and the alpha
/// blend equation. A host (`engine://`) texture's filter is put back by GlDevice right after the
/// draw that changed it. T-263b (E9): the unpack row length, skips and pixel unpack buffer.
/// T-263c: the sampler object bound on each of units 0..2.

namespace xt::gl {

struct GlSavedState {
    GLint program = 0;
    GLint vao = 0;
    GLint arrayBuffer = 0;
    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;

    GLboolean blendEnabled = 0;
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint blendEquationRgb = 0;
    GLint blendEquationAlpha = 0;  ///< T-263b E5: the host may use another one for alpha

    /// The front-face stencil state, and (T-263b E5) the back-face one: xt_gl sets two-sided
    /// ops for its winding fills, and a host may keep the faces different.
    struct StencilFace {
        GLint func = 0, ref = 0, valueMask = 0;
        GLint fail = 0, passDepthFail = 0, passDepthPass = 0;
        GLint writeMask = 0;
    };
    GLboolean stencilEnabled = 0;
    StencilFace stencilFront;
    StencilFace stencilBack;
    GLint stencilClearValue = 0;

    GLboolean depthTestEnabled = 0;
    GLboolean cullFaceEnabled = 0;
    GLboolean scissorTestEnabled = 0;
    GLint scissorBox[4] = {0, 0, 0, 0};
    GLint viewport[4] = {0, 0, 0, 0};

    GLboolean colorMask[4] = {1, 1, 1, 1};
    GLfloat clearColor[4] = {0, 0, 0, 0};

    /// Texture units xt_gl binds on (GlStateCache::bindTexture: source, auxiliary, lookup table).
    static constexpr int kTextureUnits = 3;
    GLint activeTexture = 0;
    /// T-263b E5: the GL_TEXTURE_2D binding of each of those units, whichever one was active.
    GLint boundTexture2D[kTextureUnits] = {0, 0, 0};
    /// GL_TEXTURE_2D_MULTISAMPLE binding of unit 0, the only unit xt_gl binds one on (T-161).
    GLint boundTexture2DMs = 0;
    /// T-263c: the sampler object bound on each of those units (0 = none). A bound sampler
    /// replaces the texture's own filter and wrap parameters, which are the ones xt_gl sets.
    GLint boundSampler[kTextureUnits] = {0, 0, 0};

    GLint unpackAlignment = 4;
    /// T-263b E9: the rest of the unpack state an upload from client memory depends on.
    GLint unpackRowLength = 0;
    GLint unpackSkipRows = 0;
    GLint unpackSkipPixels = 0;
    GLint pixelUnpackBuffer = 0;
};

/// Captures every field above from the current GL context.
GlSavedState saveState();

/// T-263b E9: the unpack state xt_gl's uploads assume -- rows as long as the upload is wide, no
/// skipped rows or pixels, no pixel unpack buffer bound (with one, a data pointer is an offset
/// into it). A host that streams textures leaves any of these set. Alignment is set per upload.
void setUploadUnpackState();

/// T-263c: unbinds the sampler object of every unit xt_gl binds on (GlSavedState::kTextureUnits),
/// so the filter and wrap parameters of its textures apply for the render. An engine host
/// usually leaves its own samplers bound; restoreState() binds them again.
void unbindSamplers();

/// Restores every field captured by saveState().
void restoreState(const GlSavedState& state);

}  // namespace xt::gl
