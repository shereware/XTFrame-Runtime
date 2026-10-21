#include "gl_loader.h"

#include <cstring>

namespace xt::gl {

#define XT_GL_DEFINE_PTR(ret, name, params) ret(*name) params = nullptr;
XT_GL_FUNC_LIST(XT_GL_DEFINE_PTR)
#undef XT_GL_DEFINE_PTR

namespace {
const char* g_lastMissing = "";
}  // namespace

bool loadGl(GlLoadProc loadProc) {
    g_lastMissing = "";
    bool allOk = true;

#define XT_GL_LOAD_PTR(ret, name, params)                    \
    name = reinterpret_cast<ret(*) params>(loadProc(#name)); \
    if (name == nullptr) {                                   \
        if (allOk) {                                         \
            g_lastMissing = #name;                           \
        }                                                    \
        allOk = false;                                       \
    }
    XT_GL_FUNC_LIST(XT_GL_LOAD_PTR)
#undef XT_GL_LOAD_PTR

    return allOk;
}

const char* lastMissing() { return g_lastMissing; }

}  // namespace xt::gl
