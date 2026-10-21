#pragma once

#include <string>

#include "gl_loader.h"

/// Shader compile/link helpers shared by gl_device.cpp and gl_effects.cpp (T-147).

namespace xt::gl {

/// Compiles one shader stage; returns 0 (with `err` set) on failure.
GLuint compileShader(GLenum type, const char* src, std::string& err);

/// Links a vertex + fragment program; returns 0 (with `err` set) on failure.
GLuint linkProgram(const char* vsSrc, const char* fsSrc, std::string& err);

}  // namespace xt::gl
