#include <cstdint>
#include <string_view>
#include <vector>

// SheenBidi's public headers do not guard their declarations with `extern "C"` (unlike
// libunibreak's), even though Source/SheenBidi.c is compiled as C; without this wrapper a C++
// translation unit would look up C++-mangled names that don't exist in the C-linkage library.
extern "C" {
#include <SheenBidi.h>
}

#include "xt/text_types.h"

/// SheenBidi wrapper: UTF-8 -> per-codepoint (replicated per UTF-8 byte) embedding levels. See
/// Docs/design/07-text.md §7.3 step 1.

namespace xt::text {

// Computes UAX#9 embedding levels for `utf8`, one entry per UTF-8 *byte* (all bytes of a given
// codepoint share that codepoint's level, so callers can index by a codepoint's byteStart).
// Multiple paragraphs (split on SheenBidi's own paragraph-separator detection, which includes
// '\n') are processed independently, each with the same explicit base level derived from
// `paragraphDirection` (0 = LTR, 1 = RTL; never SBLevelDefaultLTR/RTL "auto" detection, per
// Docs/tasks/T-29.md).
std::vector<uint8_t> computeBidiLevels(std::string_view utf8, TextDirection paragraphDirection) {
    std::vector<uint8_t> levels(utf8.size(), 0);
    if (utf8.empty()) return levels;

    const SBLevel baseLevel = paragraphDirection == TextDirection::Rtl ? 1 : 0;

    SBCodepointSequence sequence;
    sequence.stringEncoding = SBStringEncodingUTF8;
    sequence.stringBuffer = const_cast<void*>(static_cast<const void*>(utf8.data()));
    sequence.stringLength = static_cast<SBUInteger>(utf8.size());

    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (algorithm == nullptr) return levels;

    SBUInteger offset = 0;
    const SBUInteger length = static_cast<SBUInteger>(utf8.size());
    while (offset < length) {
        SBUInteger actualLength = 0;
        SBAlgorithmGetParagraphBoundary(algorithm, offset, length - offset, &actualLength, nullptr);
        if (actualLength == 0) break;  // defensive; SheenBidi never returns 0 here in practice.

        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, offset, actualLength, baseLevel);
        if (paragraph != nullptr) {
            const SBLevel* paraLevels = SBParagraphGetLevelsPtr(paragraph);
            const SBUInteger paraLength = SBParagraphGetLength(paragraph);
            for (SBUInteger i = 0; i < paraLength && offset + i < length; ++i) {
                levels[offset + i] = static_cast<uint8_t>(paraLevels[i]);
            }
            SBParagraphRelease(paragraph);
        }
        offset += actualLength;
    }

    SBAlgorithmRelease(algorithm);
    return levels;
}

}  // namespace xt::text
