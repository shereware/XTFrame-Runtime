#include "xt/image_format.h"

#include <cstring>

/// T-821: see xt/image_format.h. Signatures as the formats' own specifications (and stb_image's
/// stbi__*_test functions, for the decodable ones) define them.

namespace xt {

namespace {

bool startsWith(const uint8_t* head, size_t size, size_t at, const char* magic, size_t length) {
    return size >= at + length && std::memcmp(head + at, magic, length) == 0;
}

}  // namespace

ImageFormat imageFormatOf(const uint8_t* head, size_t size) {
    if (head == nullptr) {
        return ImageFormat::Unknown;
    }
    if (startsWith(head, size, 0, "\x89PNG\r\n\x1a\n", 8)) {
        return ImageFormat::Png;
    }
    if (startsWith(head, size, 0, "\xFF\xD8", 2)) {
        return ImageFormat::Jpeg;
    }
    if (startsWith(head, size, 0, "GIF87a", 6) || startsWith(head, size, 0, "GIF89a", 6)) {
        return ImageFormat::Gif;
    }
    if (startsWith(head, size, 0, "DDS ", 4)) {
        return ImageFormat::Dds;
    }
    if (startsWith(head, size, 0, "8BPS", 4)) {
        return ImageFormat::Psd;
    }
    if (startsWith(head, size, 0, "#?RADIANCE\n", 11) || startsWith(head, size, 0, "#?RGBE\n", 7)) {
        return ImageFormat::Hdr;
    }
    if (startsWith(head, size, 0, "\x53\x80\xF6\x34", 4) && startsWith(head, size, 88, "PICT", 4)) {
        return ImageFormat::Pic;
    }
    if (startsWith(head, size, 0, "P5", 2) || startsWith(head, size, 0, "P6", 2)) {
        return ImageFormat::Pnm;  // stb_image reads the binary grey / RGB maps only.
    }
    if (startsWith(head, size, 0, "RIFF", 4) && startsWith(head, size, 8, "WEBP", 4)) {
        return ImageFormat::WebP;
    }
    if (startsWith(head, size, 4, "ftyp", 4)) {
        if (startsWith(head, size, 8, "avif", 4) || startsWith(head, size, 8, "avis", 4)) {
            return ImageFormat::Avif;
        }
        static constexpr const char* kHeif[] = {"heic", "heix", "hevc", "hevx",
                                                "heim", "heis", "mif1", "msf1"};
        for (const char* brand : kHeif) {
            if (startsWith(head, size, 8, brand, 4)) {
                return ImageFormat::Heic;
            }
        }
    }
    if (startsWith(head, size, 0, "\xFF\x0A", 2) ||
        startsWith(head, size, 0, "\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A", 12)) {
        return ImageFormat::Jxl;
    }
    if (startsWith(head, size, 0, "II*\x00", 4) || startsWith(head, size, 0, "MM\x00*", 4)) {
        return ImageFormat::Tiff;
    }
    if (startsWith(head, size, 0, "qoif", 4)) {
        return ImageFormat::Qoi;
    }
    if (startsWith(head, size, 0, "BM", 2)) {
        return ImageFormat::Bmp;  // Last: the weakest signature.
    }
    return ImageFormat::Unknown;
}

std::string_view imageFormatName(ImageFormat format) {
    switch (format) {
        case ImageFormat::Png:
            return "PNG";
        case ImageFormat::Jpeg:
            return "JPEG";
        case ImageFormat::Gif:
            return "GIF";
        case ImageFormat::Bmp:
            return "BMP";
        case ImageFormat::Psd:
            return "PSD";
        case ImageFormat::Hdr:
            return "HDR";
        case ImageFormat::Pic:
            return "PIC";
        case ImageFormat::Pnm:
            return "PNM";
        case ImageFormat::Dds:
            return "DDS";
        case ImageFormat::WebP:
            return "WebP";
        case ImageFormat::Avif:
            return "AVIF";
        case ImageFormat::Heic:
            return "HEIF";
        case ImageFormat::Jxl:
            return "JPEG XL";
        case ImageFormat::Tiff:
            return "TIFF";
        case ImageFormat::Qoi:
            return "QOI";
        case ImageFormat::Unknown:
            break;
    }
    return "unknown";
}

bool isDecodableImageFormat(ImageFormat format) {
    switch (format) {
        case ImageFormat::Png:
        case ImageFormat::Jpeg:
        case ImageFormat::Gif:
        case ImageFormat::Bmp:
        case ImageFormat::Psd:
        case ImageFormat::Hdr:
        case ImageFormat::Pic:
        case ImageFormat::Pnm:
        case ImageFormat::Dds:
            return true;
        default:
            return false;
    }
}

std::string undecodableImageMessage(std::string_view name, ImageFormat format) {
    std::string out = "image asset \"";
    out += name;
    out += "\": format ";
    out += imageFormatName(format);
    out += " cannot be decoded (supported: ";
    out += kDecodableImageFormats;
    out += ")";
    return out;
}

}  // namespace xt
