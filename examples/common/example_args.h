#pragma once

#include <cstdlib>
#include <string>
#include <vector>

/// Shared command-line parsing for the examples/ programs. See docs/tasks/T-40.md.
///
/// All three examples accept the same four flags (`--headless`, `--frames N`, `--file <path>`,
/// `--width W`, `--height H`); `lottie_player`'s usage additionally documents a positional
/// `<file.json|file.xtir>` argument instead of `--file`, so `parseCommonArgs` also accepts the
/// first non-flag argument as the file path when `--file` was not given. Anything this function
/// does not recognize is appended to `remaining` (in order) for the caller to parse itself (e.g.
/// `engine_integration`'s `--simplify`).

namespace xt::examples {

struct CommonArgs {
    bool headless = false;
    uint32_t frames = 60;
    std::string file;
    int width = 800;
    int height = 600;
};

/// Parses `argv[1..argc)` into `out`, appending every unrecognized argument to `remaining`.
/// Returns false when a flag that requires a value (`--frames`/`--file`/`--width`/`--height`) is
/// the last argument.
inline bool parseCommonArgs(int argc, char** argv, CommonArgs& out, std::vector<std::string>& remaining) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--headless") {
            out.headless = true;
        } else if (arg == "--frames") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.frames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--file") {
            if (!needValue(out.file)) {
                return false;
            }
        } else if (arg == "--width") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.width = std::atoi(v.c_str());
        } else if (arg == "--height") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.height = std::atoi(v.c_str());
        } else if (out.file.empty() && !arg.empty() && arg[0] != '-') {
            out.file = arg;
        } else {
            remaining.push_back(arg);
        }
    }
    return true;
}

}  // namespace xt::examples
