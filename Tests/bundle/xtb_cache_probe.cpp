// T-807 test helper: opens one `.xtb` through Bundle::open in its own process, with the system temp
// directory pointed at a directory the test owns, and writes the outcome to a file.
//   xt_xtb_cache_probe <temp root> <file.xtb> <result file> [NAME=VALUE]...
// TMP, TEMP and TMPDIR are set to <temp root>; XTFRAME_BUNDLE_CACHE_MAX_ENTRIES and
// XTFRAME_BUNDLE_CACHE_MAX_MB are cleared, then every NAME=VALUE is set. The result file holds
// "ok\n<unpacked dir>\n<bundle id>\n" or "error\n<message>\n". Exit code 0 when the open succeeded,
// 1 when it failed, 2 on bad arguments or an unwritable result file.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "xt/bundle/bundle.h"

namespace {

void setEnvProbe(const std::string& name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value.c_str());  // an empty value removes the variable
#else
    if (value.empty()) {
        unsetenv(name.c_str());
    } else {
        setenv(name.c_str(), value.c_str(), 1);
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: xt_xtb_cache_probe <temp root> <file.xtb> <result file> [NAME=VALUE]...\n");
        return 2;
    }
    for (const char* name : {"TMP", "TEMP", "TMPDIR"}) {
        setEnvProbe(name, argv[1]);
    }
    setEnvProbe("XTFRAME_BUNDLE_CACHE_MAX_ENTRIES", "");
    setEnvProbe("XTFRAME_BUNDLE_CACHE_MAX_MB", "");
    for (int i = 4; i < argc; ++i) {
        const std::string pair = argv[i];
        const size_t eq = pair.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::fprintf(stderr, "xt_xtb_cache_probe: '%s' is not NAME=VALUE\n", argv[i]);
            return 2;
        }
        setEnvProbe(pair.substr(0, eq), pair.substr(eq + 1));
    }

    std::string error;
    xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
        xt::bundle::Bundle::open(argv[2], nullptr, &error);
    const std::string text = opened.ok() ? "ok\n" + std::string(opened.value()->dir()) + "\n" +
                                               std::string(opened.value()->id()) + "\n"
                                         : "error\n" + error + "\n";
    std::FILE* out = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&out, argv[3], "wb") != 0) {
        out = nullptr;
    }
#else
    out = std::fopen(argv[3], "wb");
#endif
    if (out == nullptr) {
        std::fprintf(stderr, "xt_xtb_cache_probe: cannot write %s\n", argv[3]);
        return 2;
    }
    std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
    return opened.ok() ? 0 : 1;
}
