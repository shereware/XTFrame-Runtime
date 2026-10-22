#include "xt/sw/sw_png.h"

#define STB_IMAGE_IMPLEMENTATION
// T-258a: stb_image refuses wider or taller images itself (decodeImageBytes checks the same limit,
// and a pixel count, before decoding). T-258b: the number lives once, in sw_png.h.
#define STBI_MAX_DIMENSIONS (xt::sw::kMaxImageDimension)
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <cstdio>
#include <string>

#include "file_open_utf8.h"  // Code/Runtime/core/src (T-261e)
#include "stb_image_write.h"

/// stb_image / stb_image_write wrapper. See Docs/tasks/T-11.md "sw_png.h".

namespace xt::sw {

namespace {

/// stb_image_write's output callback: appends to the FILE* in `context`, remembering a short write.
struct PngFileSink {
    std::FILE* file = nullptr;
    bool failed = false;
};

void writeToFile(void* context, void* data, int size) {
    auto* sink = static_cast<PngFileSink*>(context);
    if (size > 0 &&
        std::fwrite(data, 1, static_cast<size_t>(size), sink->file) != static_cast<size_t>(size)) {
        sink->failed = true;
    }
}

}  // namespace

bool writePng(std::string_view path, const uint8_t* rgba, int width, int height, int strideBytes) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    // T-261e: `path` is UTF-8, opened through the wide API on Windows (openFileUtf8).
    std::string pathStr(path);
    PngFileSink sink;
    sink.file = openFileUtf8(pathStr.c_str(), "wb");
    if (sink.file == nullptr) {
        return false;
    }
    int ok = stbi_write_png_to_func(writeToFile, &sink, width, height, 4, rgba, strideBytes);
    const bool closed = std::fclose(sink.file) == 0;
    return ok != 0 && !sink.failed && closed;
}

bool encodePng(const uint8_t* rgba, int width, int height, int strideBytes, std::vector<uint8_t>& out) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    int length = 0;
    unsigned char* png = stbi_write_png_to_mem(rgba, strideBytes, width, height, 4, &length);
    if (png == nullptr || length <= 0) {
        STBIW_FREE(png);
        return false;
    }
    out.assign(png, png + length);
    STBIW_FREE(png);
    return true;
}

bool readPng(std::string_view path, std::vector<uint8_t>& rgba, int& width, int& height) {
    std::string pathStr(path);
    int w = 0;
    int h = 0;
    int channels = 0;
    std::FILE* file = openFileUtf8(pathStr.c_str(), "rb");  // T-261e: `path` is UTF-8.
    if (file == nullptr) {
        return false;
    }
    unsigned char* data = stbi_load_from_file(file, &w, &h, &channels, 4);
    std::fclose(file);
    if (data == nullptr) {
        return false;
    }
    rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    width = w;
    height = h;
    stbi_image_free(data);
    return true;
}

}  // namespace xt::sw
