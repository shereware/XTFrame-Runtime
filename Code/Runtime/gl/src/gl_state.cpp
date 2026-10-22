#include "gl_state.h"

namespace xt::gl {

GlSavedState saveState() {
    GlSavedState s;
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFramebuffer);

    s.blendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEquationAlpha);

    s.stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_STENCIL_FUNC, &s.stencilFront.func);
    glGetIntegerv(GL_STENCIL_REF, &s.stencilFront.ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &s.stencilFront.valueMask);
    glGetIntegerv(GL_STENCIL_FAIL, &s.stencilFront.fail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s.stencilFront.passDepthFail);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s.stencilFront.passDepthPass);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s.stencilFront.writeMask);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &s.stencilBack.func);
    glGetIntegerv(GL_STENCIL_BACK_REF, &s.stencilBack.ref);
    glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK, &s.stencilBack.valueMask);
    glGetIntegerv(GL_STENCIL_BACK_FAIL, &s.stencilBack.fail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL, &s.stencilBack.passDepthFail);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &s.stencilBack.passDepthPass);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &s.stencilBack.writeMask);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &s.stencilClearValue);

    s.depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    s.cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    s.scissorTestEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
    glGetIntegerv(GL_VIEWPORT, s.viewport);

    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s.clearColor);

    // E5: a binding query answers for the active unit only, so each unit xt_gl binds on is made
    // active in turn (unit 0 last: its multisample binding is the one xt_gl touches).
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    for (int unit = GlSavedState::kTextureUnits - 1; unit >= 0; --unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.boundTexture2D[unit]);
        glGetIntegerv(GL_SAMPLER_BINDING, &s.boundSampler[unit]);  // T-263c: per unit as well
    }
    glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &s.boundTexture2DMs);
    glActiveTexture(static_cast<GLenum>(s.activeTexture));

    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s.unpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s.unpackSkipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s.unpackSkipPixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s.pixelUnpackBuffer);
    return s;
}

void setUploadUnpackState() {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void unbindSamplers() {
    for (int unit = 0; unit < GlSavedState::kTextureUnits; ++unit) {
        glBindSampler(static_cast<GLuint>(unit), 0);
    }
}

void restoreState(const GlSavedState& s) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s.drawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFramebuffer));

    glUseProgram(static_cast<GLuint>(s.program));
    glBindVertexArray(static_cast<GLuint>(s.vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s.arrayBuffer));

    if (s.blendEnabled != 0) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    glBlendFuncSeparate(static_cast<GLenum>(s.blendSrcRgb), static_cast<GLenum>(s.blendDstRgb),
                        static_cast<GLenum>(s.blendSrcAlpha), static_cast<GLenum>(s.blendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(s.blendEquationRgb),
                            static_cast<GLenum>(s.blendEquationAlpha));

    if (s.stencilEnabled != 0) {
        glEnable(GL_STENCIL_TEST);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    const GLenum faces[2] = {GL_FRONT, GL_BACK};
    const GlSavedState::StencilFace* stencil[2] = {&s.stencilFront, &s.stencilBack};
    for (int i = 0; i < 2; ++i) {
        const GlSavedState::StencilFace& f = *stencil[i];
        glStencilFuncSeparate(faces[i], static_cast<GLenum>(f.func), f.ref, static_cast<GLuint>(f.valueMask));
        glStencilOpSeparate(faces[i], static_cast<GLenum>(f.fail), static_cast<GLenum>(f.passDepthFail),
                            static_cast<GLenum>(f.passDepthPass));
        glStencilMaskSeparate(faces[i], static_cast<GLuint>(f.writeMask));
    }
    glClearStencil(s.stencilClearValue);

    if (s.depthTestEnabled != 0) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (s.cullFaceEnabled != 0) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (s.scissorTestEnabled != 0) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);

    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    glClearColor(s.clearColor[0], s.clearColor[1], s.clearColor[2], s.clearColor[3]);

    for (int unit = GlSavedState::kTextureUnits - 1; unit >= 0; --unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.boundTexture2D[unit]));
        glBindSampler(static_cast<GLuint>(unit), static_cast<GLuint>(s.boundSampler[unit]));
    }
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, static_cast<GLuint>(s.boundTexture2DMs));
    glActiveTexture(static_cast<GLenum>(s.activeTexture));

    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s.unpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, s.unpackSkipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, s.unpackSkipPixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(s.pixelUnpackBuffer));
}

}  // namespace xt::gl
