#pragma once

#include <cstddef>
#include <cstdint>

/// Minimal OpenGL 3.3 core function-pointer loader. No system gl.h/glext.h is included anywhere;
/// every GL type and constant used by xt_gl is defined here. See docs/tasks/T-19.md "GL 載入器".

namespace xt::gl {

// ---------------------------------------------------------------------------------------------
// GL types (subset needed by xt_gl; matches the official khrplatform/gl.h ABI on every desktop
// platform xt targets).
// ---------------------------------------------------------------------------------------------
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLuint64 = uint64_t;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLclampd = double;
using GLchar = char;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;
using GLvoid = void;

// ---------------------------------------------------------------------------------------------
// GL constants (only the ones xt_gl references).
// ---------------------------------------------------------------------------------------------
inline constexpr GLenum GL_FALSE = 0;
inline constexpr GLenum GL_TRUE = 1;

inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_FAN = 0x0006;

inline constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLenum GL_STENCIL_BUFFER_BIT = 0x00000400;
inline constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;

inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;

inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
inline constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;

inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;

inline constexpr GLenum GL_KEEP = 0x1E00;
inline constexpr GLenum GL_REPLACE = 0x1E01;
inline constexpr GLenum GL_INCR = 0x1E02;
inline constexpr GLenum GL_DECR = 0x1E03;
inline constexpr GLenum GL_INVERT = 0x150A;
inline constexpr GLenum GL_INCR_WRAP = 0x8507;
inline constexpr GLenum GL_DECR_WRAP = 0x8508;

inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;

inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;

inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
inline constexpr GLenum GL_SCISSOR_BOX = 0x0C10;
inline constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
inline constexpr GLenum GL_COLOR_CLEAR_VALUE = 0x0C22;
inline constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
inline constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
inline constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;
inline constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
inline constexpr GLenum GL_BLEND_EQUATION_RGB = 0x8009;
inline constexpr GLenum GL_BLEND_EQUATION_ALPHA = 0x883D;
inline constexpr GLenum GL_STENCIL_FUNC = 0x0B92;
inline constexpr GLenum GL_STENCIL_REF = 0x0B97;
inline constexpr GLenum GL_STENCIL_VALUE_MASK = 0x0B93;
inline constexpr GLenum GL_STENCIL_FAIL = 0x0B94;
inline constexpr GLenum GL_STENCIL_PASS_DEPTH_FAIL = 0x0B95;
inline constexpr GLenum GL_STENCIL_PASS_DEPTH_PASS = 0x0B96;
inline constexpr GLenum GL_STENCIL_WRITEMASK = 0x0B98;
inline constexpr GLenum GL_STENCIL_CLEAR_VALUE = 0x0B91;
inline constexpr GLenum GL_STENCIL_BACK_FUNC = 0x8800;
inline constexpr GLenum GL_STENCIL_BACK_FAIL = 0x8801;
inline constexpr GLenum GL_STENCIL_BACK_PASS_DEPTH_FAIL = 0x8802;
inline constexpr GLenum GL_STENCIL_BACK_PASS_DEPTH_PASS = 0x8803;
inline constexpr GLenum GL_STENCIL_BACK_REF = 0x8CA3;
inline constexpr GLenum GL_STENCIL_BACK_VALUE_MASK = 0x8CA4;
inline constexpr GLenum GL_STENCIL_BACK_WRITEMASK = 0x8CA5;
inline constexpr GLenum GL_READ_FRAMEBUFFER_BINDING = 0x8CAA;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
// T-343: timer queries (GL 3.3 core), for the tests' GPU-time measurement.
inline constexpr GLenum GL_TIME_ELAPSED = 0x88BF;
inline constexpr GLenum GL_QUERY_RESULT = 0x8866;
inline constexpr GLenum GL_QUERY_RESULT_AVAILABLE = 0x8867;
inline constexpr GLenum GL_SAMPLES = 0x80A9;
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
inline constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
inline constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
inline constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
inline constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
inline constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
inline constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
inline constexpr GLenum GL_TEXTURE_2D_MULTISAMPLE = 0x9100;
inline constexpr GLenum GL_TEXTURE_BINDING_2D_MULTISAMPLE = 0x9104;
inline constexpr GLenum GL_SAMPLER_BINDING = 0x8919;

inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;

inline constexpr GLenum GL_STENCIL_INDEX = 0x1901;
inline constexpr GLenum GL_RED = 0x1903;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_R8 = 0x8229;
inline constexpr GLenum GL_R32F = 0x822E;  ///< docs/tasks/T-54.md's Outline SDF target.
inline constexpr GLenum GL_RGBA8 = 0x8058;
// T-199: block-compressed image formats (EXT_texture_compression_s3tc, ARB_texture_compression_bptc).
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;
inline constexpr GLenum GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3;
inline constexpr GLenum GL_COMPRESSED_RGBA_BPTC_UNORM_ARB = 0x8E8C;
inline constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
inline constexpr GLenum GL_STENCIL_INDEX8 = 0x8D48;
inline constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
inline constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;

inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_TEXTURE1 = 0x84C1;
inline constexpr GLenum GL_TEXTURE2 = 0x84C2;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_REPEAT = 0x2901;

inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;

inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;

inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_INVALID_ENUM = 0x0500;
inline constexpr GLenum GL_INVALID_VALUE = 0x0501;
inline constexpr GLenum GL_INVALID_OPERATION = 0x0502;
inline constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;

inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;

inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
inline constexpr GLenum GL_UNPACK_SKIP_ROWS = 0x0CF3;
inline constexpr GLenum GL_UNPACK_SKIP_PIXELS = 0x0CF4;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;

// ---------------------------------------------------------------------------------------------
// Function table.
// ---------------------------------------------------------------------------------------------

/// XT_GL_FUNC(ret, name, params) lists every GL entry point xt_gl needs; the actual pointer is
/// exposed as a global named `name` (e.g. `xt::gl::glClear(...)`). See gl_loader.cpp.
#define XT_GL_FUNC_LIST(F)                                                                                  \
    F(void, glGetIntegerv, (GLenum pname, GLint * data))                                                    \
    F(void, glGetFloatv, (GLenum pname, GLfloat * data))                                                    \
    F(void, glGetBooleanv, (GLenum pname, GLboolean * data))                                                \
    F(const GLubyte*, glGetString, (GLenum name))                                                           \
    F(void, glEnable, (GLenum cap))                                                                         \
    F(void, glDisable, (GLenum cap))                                                                        \
    F(GLboolean, glIsEnabled, (GLenum cap))                                                                 \
    F(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))                                                  \
    F(void, glBlendFuncSeparate, (GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha))          \
    F(void, glBlendEquation, (GLenum mode))                                                                 \
    F(void, glBlendEquationSeparate, (GLenum modeRgb, GLenum modeAlpha))                                    \
    F(void, glStencilFunc, (GLenum func, GLint ref, GLuint mask))                                           \
    F(void, glStencilFuncSeparate, (GLenum face, GLenum func, GLint ref, GLuint mask))                      \
    F(void, glStencilOp, (GLenum sfail, GLenum dpfail, GLenum dppass))                                      \
    F(void, glStencilOpSeparate, (GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))                 \
    F(void, glStencilMask, (GLuint mask))                                                                   \
    F(void, glStencilMaskSeparate, (GLenum face, GLuint mask))                                              \
    F(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                              \
    F(void, glClear, (GLbitfield mask))                                                                     \
    F(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                     \
    F(void, glClearStencil, (GLint s))                                                                      \
    F(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                           \
    F(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                            \
    F(void, glGenBuffers, (GLsizei n, GLuint * buffers))                                                    \
    F(void, glBindBuffer, (GLenum target, GLuint buffer))                                                   \
    F(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                 \
    F(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))           \
    F(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                                            \
    F(void, glGenVertexArrays, (GLsizei n, GLuint * arrays))                                                \
    F(void, glBindVertexArray, (GLuint array))                                                              \
    F(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                        \
    F(void, glEnableVertexAttribArray, (GLuint index))                                                      \
    F(void, glVertexAttribPointer,                                                                          \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))   \
    F(GLuint, glCreateShader, (GLenum type))                                                                \
    F(void, glShaderSource,                                                                                 \
      (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))                     \
    F(void, glCompileShader, (GLuint shader))                                                               \
    F(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint * params))                                   \
    F(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog))       \
    F(void, glDeleteShader, (GLuint shader))                                                                \
    F(GLuint, glCreateProgram, ())                                                                          \
    F(void, glAttachShader, (GLuint program, GLuint shader))                                                \
    F(void, glLinkProgram, (GLuint program))                                                                \
    F(void, glGetProgramiv, (GLuint program, GLenum pname, GLint * params))                                 \
    F(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog))     \
    F(void, glUseProgram, (GLuint program))                                                                 \
    F(void, glDeleteProgram, (GLuint program))                                                              \
    F(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                                    \
    F(void, glUniform1i, (GLint location, GLint v0))                                                        \
    F(void, glUniform1f, (GLint location, GLfloat v0))                                                      \
    F(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                          \
    F(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                              \
    F(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                  \
    F(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat* value))                            \
    F(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat* value))                            \
    F(void, glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    F(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    F(void, glGenTextures, (GLsizei n, GLuint * textures))                                                  \
    F(void, glBindTexture, (GLenum target, GLuint texture))                                                 \
    F(void, glTexImage2D,                                                                                   \
      (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,       \
       GLenum format, GLenum type, const void* pixels))                                                     \
    F(void, glCompressedTexImage2D,                                                                         \
      (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,      \
       GLsizei imageSize, const void* data))                                                                \
    F(void, glTexSubImage2D,                                                                                \
      (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,             \
       GLenum format, GLenum type, const void* pixels))                                                     \
    F(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                                    \
    F(void, glGetTexParameteriv, (GLenum target, GLenum pname, GLint * params))                             \
    F(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                                          \
    F(void, glActiveTexture, (GLenum texture))                                                              \
    F(void, glBindSampler, (GLuint unit, GLuint sampler))                                                   \
    F(void, glGenFramebuffers, (GLsizei n, GLuint * framebuffers))                                          \
    F(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                         \
    F(void, glFramebufferTexture2D,                                                                         \
      (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))                    \
    F(void, glFramebufferRenderbuffer,                                                                      \
      (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))                   \
    F(GLenum, glCheckFramebufferStatus, (GLenum target))                                                    \
    F(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                                  \
    F(void, glBlitFramebuffer,                                                                              \
      (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,           \
       GLint dstY1, GLbitfield mask, GLenum filter))                                                        \
    F(void, glGenRenderbuffers, (GLsizei n, GLuint * renderbuffers))                                        \
    F(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                                       \
    F(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height))   \
    F(void, glRenderbufferStorageMultisample,                                                               \
      (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height))               \
    F(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* renderbuffers))                                \
    F(void, glTexImage2DMultisample,                                                                        \
      (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,                \
       GLboolean fixedsamplelocations))                                                                     \
    F(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                        \
    F(void, glDrawElementsBaseVertex,                                                                       \
      (GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex))                     \
    F(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instancecount))        \
    F(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                                          \
    F(void, glReadPixels,                                                                                   \
      (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels))          \
    F(void, glPixelStorei, (GLenum pname, GLint param))                                                     \
    F(GLenum, glGetError, ())                                                                               \
    F(void, glGenQueries, (GLsizei n, GLuint * ids))                                                        \
    F(void, glDeleteQueries, (GLsizei n, const GLuint* ids))                                                \
    F(void, glBeginQuery, (GLenum target, GLuint id))                                                       \
    F(void, glEndQuery, (GLenum target))                                                                    \
    F(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64 * params))                            \
    F(void, glFinish, ())

#define XT_GL_DECLARE_PTR(ret, name, params) extern ret(*name) params;
XT_GL_FUNC_LIST(XT_GL_DECLARE_PTR)
#undef XT_GL_DECLARE_PTR

/// Function pointer type matching SDL_GL_GetProcAddress / wglGetProcAddress / etc.
using GlLoadProc = void* (*)(const char* name);

/// Resolves every function above via `loadProc`. Returns true only if all of them resolved to a
/// non-null pointer; on the first failure, records the missing name (lastMissing()) and continues
/// resolving the rest (so subsequent calls don't crash on a null pointer left over from a partial
/// load attempt -- callers must still check the return value before using any of them).
bool loadGl(GlLoadProc loadProc);

/// Name of the first GL function that failed to resolve in the most recent loadGl() call, or ""
/// if the load succeeded (or loadGl has not been called yet).
const char* lastMissing();

}  // namespace xt::gl
