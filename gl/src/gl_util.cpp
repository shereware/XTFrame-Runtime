#include "gl_util.h"

namespace xt::gl {

GLuint compileShader(GLenum type, const char* src, std::string& err) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
        if (len > 0) {
            glGetShaderInfoLog(shader, len, nullptr, log.data());
        }
        err = "shader compile error: " + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc, std::string& err) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
    if (vs == 0) {
        return 0;
    }
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
        if (len > 0) {
            glGetProgramInfoLog(prog, len, nullptr, log.data());
        }
        err = "program link error: " + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

}  // namespace xt::gl
