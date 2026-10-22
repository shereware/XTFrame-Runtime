# Third-party vendored files

| Library | Files | Version | License |
|---|---|---|---|
| doctest | `doctest/doctest.h` | v2.4.11 | MIT |
| stb | `stb/stb_image.h`, `stb/stb_image_write.h` | commit `5c205738c191bcb0abc65c4febfa9bd25ff35234` | Public Domain / MIT |
| nlohmann/json | `nlohmann/json.hpp` | v3.11.3 | MIT |
| SheenBidi | `sheenbidi/Headers/`, `sheenbidi/Source/` | v2.8 | Apache License 2.0 |
| libunibreak | `libunibreak/src/` (subset, see `libunibreak/VERSION.txt`) | libunibreak_6_1 | zlib |
| bc7enc_rdo | `bc7enc/bc7enc.{h,cpp}`, `bc7enc/bc7decomp.{h,cpp}`, `bc7enc/rgbcx.{h,cpp}` | commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` | MIT / Public Domain (dual) |

Each subdirectory has a `VERSION.txt` file recording the exact version/commit and source URL.

FreeType, HarfBuzz, SDL2, Clipper2, cpp-httplib and miniz are not vendored here; they are fetched
and built from source by `Tools/CMake/xt_text_deps.cmake`, `Tools/CMake/xt_sdl2.cmake`, `Tools/CMake/xt_clipper2.cmake`,
`Tools/CMake/xt_httplib.cmake` and `Tools/CMake/xt_miniz.cmake` (FetchContent), see Docs/tasks/T-29.md.

`versions.cmake` lists every component above and every fetched one (version, source, hash or tag,
licence); it is the single source of `NOTICE.md` and `Legal/THIRD-PARTY.md`
(`python Tools/Scripts/gen_third_party.py`). Fetched sources go to `_fetched/<name>/` (git-ignored), shared
by every build tree; to update one, edit its line in `versions.cmake` (the next configure fetches
it). `-DXT_SDK_OFFLINE=ON` never downloads: copy the sources into `_fetched/<name>/` beforehand.
