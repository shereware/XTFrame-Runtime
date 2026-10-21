#include "xt/version.h"

#define XT_STRINGIFY_IMPL(x) #x
#define XT_STRINGIFY(x) XT_STRINGIFY_IMPL(x)

namespace xt {

const char* versionString() {
    static const char* const kVersion =
        XT_STRINGIFY(XT_VERSION_MAJOR) "." XT_STRINGIFY(XT_VERSION_MINOR) "." XT_STRINGIFY(XT_VERSION_PATCH);
    return kVersion;
}

}  // namespace xt
