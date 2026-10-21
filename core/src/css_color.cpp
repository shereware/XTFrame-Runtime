#include "css_color.h"

#include <cmath>

/// CSS Color Level 4 named colours (https://www.w3.org/TR/css-color-4/#named-colors) and the
/// `hsl()` conversion of the same specification (section 7.1, "Converting HSL Colors to sRGB").

namespace xt::css_color {

namespace {

struct NamedColor {
    const char* name;
    uint32_t rgb;  ///< 0xRRGGBB.
};

// Sorted by name (byte order) for findNamedColor()'s binary search.
// clang-format off
constexpr NamedColor kNamedColors[] = {
    {"aliceblue", 0xF0F8FFU},
    {"antiquewhite", 0xFAEBD7U},
    {"aqua", 0x00FFFFU},
    {"aquamarine", 0x7FFFD4U},
    {"azure", 0xF0FFFFU},
    {"beige", 0xF5F5DCU},
    {"bisque", 0xFFE4C4U},
    {"black", 0x000000U},
    {"blanchedalmond", 0xFFEBCDU},
    {"blue", 0x0000FFU},
    {"blueviolet", 0x8A2BE2U},
    {"brown", 0xA52A2AU},
    {"burlywood", 0xDEB887U},
    {"cadetblue", 0x5F9EA0U},
    {"chartreuse", 0x7FFF00U},
    {"chocolate", 0xD2691EU},
    {"coral", 0xFF7F50U},
    {"cornflowerblue", 0x6495EDU},
    {"cornsilk", 0xFFF8DCU},
    {"crimson", 0xDC143CU},
    {"cyan", 0x00FFFFU},
    {"darkblue", 0x00008BU},
    {"darkcyan", 0x008B8BU},
    {"darkgoldenrod", 0xB8860BU},
    {"darkgray", 0xA9A9A9U},
    {"darkgreen", 0x006400U},
    {"darkgrey", 0xA9A9A9U},
    {"darkkhaki", 0xBDB76BU},
    {"darkmagenta", 0x8B008BU},
    {"darkolivegreen", 0x556B2FU},
    {"darkorange", 0xFF8C00U},
    {"darkorchid", 0x9932CCU},
    {"darkred", 0x8B0000U},
    {"darksalmon", 0xE9967AU},
    {"darkseagreen", 0x8FBC8FU},
    {"darkslateblue", 0x483D8BU},
    {"darkslategray", 0x2F4F4FU},
    {"darkslategrey", 0x2F4F4FU},
    {"darkturquoise", 0x00CED1U},
    {"darkviolet", 0x9400D3U},
    {"deeppink", 0xFF1493U},
    {"deepskyblue", 0x00BFFFU},
    {"dimgray", 0x696969U},
    {"dimgrey", 0x696969U},
    {"dodgerblue", 0x1E90FFU},
    {"firebrick", 0xB22222U},
    {"floralwhite", 0xFFFAF0U},
    {"forestgreen", 0x228B22U},
    {"fuchsia", 0xFF00FFU},
    {"gainsboro", 0xDCDCDCU},
    {"ghostwhite", 0xF8F8FFU},
    {"gold", 0xFFD700U},
    {"goldenrod", 0xDAA520U},
    {"gray", 0x808080U},
    {"green", 0x008000U},
    {"greenyellow", 0xADFF2FU},
    {"grey", 0x808080U},
    {"honeydew", 0xF0FFF0U},
    {"hotpink", 0xFF69B4U},
    {"indianred", 0xCD5C5CU},
    {"indigo", 0x4B0082U},
    {"ivory", 0xFFFFF0U},
    {"khaki", 0xF0E68CU},
    {"lavender", 0xE6E6FAU},
    {"lavenderblush", 0xFFF0F5U},
    {"lawngreen", 0x7CFC00U},
    {"lemonchiffon", 0xFFFACDU},
    {"lightblue", 0xADD8E6U},
    {"lightcoral", 0xF08080U},
    {"lightcyan", 0xE0FFFFU},
    {"lightgoldenrodyellow", 0xFAFAD2U},
    {"lightgray", 0xD3D3D3U},
    {"lightgreen", 0x90EE90U},
    {"lightgrey", 0xD3D3D3U},
    {"lightpink", 0xFFB6C1U},
    {"lightsalmon", 0xFFA07AU},
    {"lightseagreen", 0x20B2AAU},
    {"lightskyblue", 0x87CEFAU},
    {"lightslategray", 0x778899U},
    {"lightslategrey", 0x778899U},
    {"lightsteelblue", 0xB0C4DEU},
    {"lightyellow", 0xFFFFE0U},
    {"lime", 0x00FF00U},
    {"limegreen", 0x32CD32U},
    {"linen", 0xFAF0E6U},
    {"magenta", 0xFF00FFU},
    {"maroon", 0x800000U},
    {"mediumaquamarine", 0x66CDAAU},
    {"mediumblue", 0x0000CDU},
    {"mediumorchid", 0xBA55D3U},
    {"mediumpurple", 0x9370DBU},
    {"mediumseagreen", 0x3CB371U},
    {"mediumslateblue", 0x7B68EEU},
    {"mediumspringgreen", 0x00FA9AU},
    {"mediumturquoise", 0x48D1CCU},
    {"mediumvioletred", 0xC71585U},
    {"midnightblue", 0x191970U},
    {"mintcream", 0xF5FFFAU},
    {"mistyrose", 0xFFE4E1U},
    {"moccasin", 0xFFE4B5U},
    {"navajowhite", 0xFFDEADU},
    {"navy", 0x000080U},
    {"oldlace", 0xFDF5E6U},
    {"olive", 0x808000U},
    {"olivedrab", 0x6B8E23U},
    {"orange", 0xFFA500U},
    {"orangered", 0xFF4500U},
    {"orchid", 0xDA70D6U},
    {"palegoldenrod", 0xEEE8AAU},
    {"palegreen", 0x98FB98U},
    {"paleturquoise", 0xAFEEEEU},
    {"palevioletred", 0xDB7093U},
    {"papayawhip", 0xFFEFD5U},
    {"peachpuff", 0xFFDAB9U},
    {"peru", 0xCD853FU},
    {"pink", 0xFFC0CBU},
    {"plum", 0xDDA0DDU},
    {"powderblue", 0xB0E0E6U},
    {"purple", 0x800080U},
    {"rebeccapurple", 0x663399U},
    {"red", 0xFF0000U},
    {"rosybrown", 0xBC8F8FU},
    {"royalblue", 0x4169E1U},
    {"saddlebrown", 0x8B4513U},
    {"salmon", 0xFA8072U},
    {"sandybrown", 0xF4A460U},
    {"seagreen", 0x2E8B57U},
    {"seashell", 0xFFF5EEU},
    {"sienna", 0xA0522DU},
    {"silver", 0xC0C0C0U},
    {"skyblue", 0x87CEEBU},
    {"slateblue", 0x6A5ACDU},
    {"slategray", 0x708090U},
    {"slategrey", 0x708090U},
    {"snow", 0xFFFAFAU},
    {"springgreen", 0x00FF7FU},
    {"steelblue", 0x4682B4U},
    {"tan", 0xD2B48CU},
    {"teal", 0x008080U},
    {"thistle", 0xD8BFD8U},
    {"tomato", 0xFF6347U},
    {"turquoise", 0x40E0D0U},
    {"violet", 0xEE82EEU},
    {"wheat", 0xF5DEB3U},
    {"white", 0xFFFFFFU},
    {"whitesmoke", 0xF5F5F5U},
    {"yellow", 0xFFFF00U},
    {"yellowgreen", 0x9ACD32U},
};
// clang-format on

static_assert(sizeof(kNamedColors) / sizeof(kNamedColors[0]) == kNamedColorCount);

/// ASCII lower-case of one character.
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// Compares `entry` (all lower case) with `name` case-insensitively: <0, 0, >0 like strcmp.
int compareName(const char* entry, std::string_view name) {
    size_t i = 0;
    for (; entry[i] != '\0' && i < name.size(); ++i) {
        const char a = entry[i];
        const char b = lowerAscii(name[i]);
        if (a != b) {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b) ? -1 : 1;
        }
    }
    if (entry[i] == '\0') {
        return i == name.size() ? 0 : -1;
    }
    return 1;
}

float channel(uint32_t rgb, int shift) { return static_cast<float>((rgb >> shift) & 0xFFU) / 255.0f; }

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

bool findNamedColor(std::string_view name, Color& out) {
    size_t lo = 0;
    size_t hi = kNamedColorCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = compareName(kNamedColors[mid].name, name);
        if (cmp == 0) {
            const uint32_t rgb = kNamedColors[mid].rgb;
            out = Color{channel(rgb, 16), channel(rgb, 8), channel(rgb, 0), 1.0f};
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool namedColorAt(size_t index, std::string_view& name, uint32_t& rgb) {
    if (index >= kNamedColorCount) {
        return false;
    }
    name = kNamedColors[index].name;
    rgb = kNamedColors[index].rgb;
    return true;
}

Color hslToColor(float hueDeg, float sat, float light, float alpha) {
    double h = std::isfinite(hueDeg) ? std::fmod(static_cast<double>(hueDeg), 360.0) : 0.0;
    if (h < 0.0) {
        h += 360.0;
    }
    const double s = clamp01(std::isfinite(sat) ? sat : 0.0f);
    const double l = clamp01(std::isfinite(light) ? light : 0.0f);
    // CSS Color 4 section 7.1: f(n) = l - a * max(-1, min(k - 3, 9 - k, 1)), k = (n + h / 30) mod 12.
    const double a = s * (l < 0.5 ? l : 1.0 - l);
    auto f = [&](double n) {
        const double k = std::fmod(n + h / 30.0, 12.0);
        double m = k - 3.0;
        m = m < 9.0 - k ? m : 9.0 - k;
        m = m < 1.0 ? m : 1.0;
        m = m > -1.0 ? m : -1.0;
        const double v = l - a * m;
        return static_cast<float>(std::round(v * 255.0) / 255.0);
    };
    return Color{f(0.0), f(8.0), f(4.0), clamp01(std::isfinite(alpha) ? alpha : 1.0f)};
}

}  // namespace xt::css_color
