#include <cmath>
#include <string>

#include "capi_internal.h"
#include "xt/charconv.h"
#include "xt/version.h"

/// Library-level part of the C ABI (Docs/design/50-c-abi.md §50.3 "library"): the ABI version,
/// the version string, the thread-local last error and the log sink.

namespace xt::capi {

namespace {

thread_local std::string g_lastError;

// The log sink. One thread per surface (50.1); the sink itself is process-wide, set by the host
// before it opens anything.
xt_log_fn g_logFn = nullptr;
void* g_logUser = nullptr;

}  // namespace

void setLastError(std::string message) { g_lastError = std::move(message); }

const char* lastError() { return g_lastError.c_str(); }

void log(int level, const std::string& message) {
    if (g_logFn != nullptr) {
        g_logFn(level, message.c_str(), g_logUser);
    }
}

std::string numberText(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    char buffer[64];
    std::string text(buffer, formatFixed(buffer, sizeof(buffer), value, 6));
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text;
}

bool splitUtf8(const char* text, size_t size, std::vector<std::pair<size_t, uint8_t>>& out) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        size_t length = 0;
        uint32_t cp = 0;
        uint32_t minimum = 0;
        if (lead < 0x80) {
            length = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            cp = lead & 0x1Fu;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            cp = lead & 0x0Fu;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            cp = lead & 0x07u;
            minimum = 0x10000;
        } else {
            return false;  // A continuation byte, or 0xF8..0xFF.
        }
        if (size - i < length) {
            return false;  // Truncated.
        }
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = bytes[i + k];
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3Fu);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;  // Overlong, out of range or a surrogate.
        }
        out.emplace_back(i, static_cast<uint8_t>(length));
        i += length;
    }
    return true;
}

}  // namespace xt::capi

extern "C" {

XT_CAPI uint32_t xt_capi_abi_version(void) {
    XT_CAPI_GUARD_OR("xt_capi_abi_version", uint32_t, XT_CAPI_ABI_VERSION, { return XT_CAPI_ABI_VERSION; });
}

XT_CAPI const char* xt_capi_version_string(void) {
    XT_CAPI_GUARD_OR("xt_capi_version_string", const char*, "", { return xt::versionString(); });
}

XT_CAPI const char* xt_capi_last_error(void) {
    XT_CAPI_GUARD_OR("xt_capi_last_error", const char*, "", { return xt::capi::lastError(); });
}

XT_CAPI void xt_capi_set_log(xt_log_fn fn, void* user) {
    XT_CAPI_GUARD_VOID("xt_capi_set_log", {
        xt::capi::g_logFn = fn;
        xt::capi::g_logUser = user;
    });
}

}  // extern "C"
