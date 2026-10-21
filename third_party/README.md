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

FreeType and HarfBuzz are not vendored here; they are fetched and built from source by
`cmake/xt_text_deps.cmake` (FetchContent, pinned tags), see docs/tasks/T-29.md.
