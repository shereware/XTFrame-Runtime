#include "color_math.h"

#include <cmath>
#include <cstdio>

/// See color_math.h.

namespace xt::bundle::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

double linearize(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

double labF(double t) {
    constexpr double kEpsilon = 216.0 / 24389.0;
    constexpr double kKappa = 24389.0 / 27.0;
    return t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16.0) / 116.0;
}

double degrees(double radians) { return radians * 180.0 / kPi; }
double radians(double degrees) { return degrees * kPi / 180.0; }

/// The hue angle in degrees, 0..360 (0 for a neutral colour).
double hueAngle(double b, double ap) {
    if (b == 0.0 && ap == 0.0) {
        return 0.0;
    }
    double h = degrees(std::atan2(b, ap));
    if (h < 0.0) {
        h += 360.0;
    }
    return h;
}

}  // namespace

bool parseHexColor(std::string_view text, float rgba[4]) {
    if (text.empty() || text[0] != '#') {
        return false;
    }
    text.remove_prefix(1);
    const size_t n = text.size();
    if (n != 3 && n != 4 && n != 6 && n != 8) {
        return false;
    }
    for (char c : text) {
        if (hexDigit(c) < 0) {
            return false;
        }
    }
    float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (n == 3 || n == 4) {
        for (size_t i = 0; i < n; ++i) {
            const int v = hexDigit(text[i]);
            out[i] = static_cast<float>(v * 17) / 255.0f;
        }
    } else {
        for (size_t i = 0; i < n / 2; ++i) {
            const int v = hexDigit(text[2 * i]) * 16 + hexDigit(text[2 * i + 1]);
            out[i] = static_cast<float>(v) / 255.0f;
        }
    }
    for (int i = 0; i < 4; ++i) {
        rgba[i] = out[i];
    }
    return true;
}

Lab srgbToLab(double r, double g, double b) {
    const double lr = linearize(r);
    const double lg = linearize(g);
    const double lb = linearize(b);
    // sRGB (IEC 61966-2-1) to XYZ, D65.
    const double x = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    const double y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    const double z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
    const double fx = labF(x / 0.95047);
    const double fy = labF(y / 1.0);
    const double fz = labF(z / 1.08883);
    return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double deltaE2000(const Lab& x, const Lab& y) {
    const double pow25to7 = 6103515625.0;  // 25^7
    const double c1 = std::sqrt(x.a * x.a + x.b * x.b);
    const double c2 = std::sqrt(y.a * y.a + y.b * y.b);
    const double cBar = (c1 + c2) / 2.0;
    const double cBar7 = std::pow(cBar, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(cBar7 / (cBar7 + pow25to7)));
    const double a1p = (1.0 + g) * x.a;
    const double a2p = (1.0 + g) * y.a;
    const double c1p = std::sqrt(a1p * a1p + x.b * x.b);
    const double c2p = std::sqrt(a2p * a2p + y.b * y.b);
    const double h1p = hueAngle(x.b, a1p);
    const double h2p = hueAngle(y.b, a2p);

    const double dLp = y.l - x.l;
    const double dCp = c2p - c1p;
    double dhp = 0.0;
    if (c1p * c2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0) {
            dhp -= 360.0;
        } else if (dhp < -180.0) {
            dhp += 360.0;
        }
    }
    const double dHp = 2.0 * std::sqrt(c1p * c2p) * std::sin(radians(dhp / 2.0));

    const double lBarP = (x.l + y.l) / 2.0;
    const double cBarP = (c1p + c2p) / 2.0;
    double hBarP = h1p + h2p;
    if (c1p * c2p != 0.0) {
        if (std::fabs(h1p - h2p) <= 180.0) {
            hBarP = (h1p + h2p) / 2.0;
        } else if (h1p + h2p < 360.0) {
            hBarP = (h1p + h2p + 360.0) / 2.0;
        } else {
            hBarP = (h1p + h2p - 360.0) / 2.0;
        }
    }
    const double t = 1.0 - 0.17 * std::cos(radians(hBarP - 30.0)) + 0.24 * std::cos(radians(2.0 * hBarP)) +
                     0.32 * std::cos(radians(3.0 * hBarP + 6.0)) -
                     0.20 * std::cos(radians(4.0 * hBarP - 63.0));
    const double dTheta = 30.0 * std::exp(-((hBarP - 275.0) / 25.0) * ((hBarP - 275.0) / 25.0));
    const double cBarP7 = std::pow(cBarP, 7.0);
    const double rc = 2.0 * std::sqrt(cBarP7 / (cBarP7 + pow25to7));
    const double lm50 = (lBarP - 50.0) * (lBarP - 50.0);
    const double sl = 1.0 + 0.015 * lm50 / std::sqrt(20.0 + lm50);
    const double sc = 1.0 + 0.045 * cBarP;
    const double sh = 1.0 + 0.015 * cBarP * t;
    const double rt = -std::sin(radians(2.0 * dTheta)) * rc;
    const double tl = dLp / sl;
    const double tc = dCp / sc;
    const double th = dHp / sh;
    return std::sqrt(tl * tl + tc * tc + th * th + rt * tc * th);
}

bool hexDeltaE2000(std::string_view a, std::string_view b, double* out) {
    float ca[4];
    float cb[4];
    if (!parseHexColor(a, ca) || !parseHexColor(b, cb)) {
        return false;
    }
    const Lab la = srgbToLab(ca[0], ca[1], ca[2]);
    const Lab lb = srgbToLab(cb[0], cb[1], cb[2]);
    if (out != nullptr) {
        *out = deltaE2000(la, lb);
    }
    return true;
}

namespace {

double delinearize(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

}  // namespace

Oklab srgbToOklab(double r, double g, double b) {
    const double lr = linearize(r);
    const double lg = linearize(g);
    const double lb = linearize(b);
    const double l = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
    const double m = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
    const double s = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
    return Oklab{0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

void oklabToSrgb(const Oklab& lab, double rgb[3]) {
    const double l1 = lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m1 = lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s1 = lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l1 * l1 * l1;
    const double m = m1 * m1 * m1;
    const double s = s1 * s1 * s1;
    const double lr = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    // Keep the sign so an out-of-gamut component stays outside 0..1.
    rgb[0] = lr < 0.0 ? -delinearize(-lr) : delinearize(lr);
    rgb[1] = lg < 0.0 ? -delinearize(-lg) : delinearize(lg);
    rgb[2] = lb < 0.0 ? -delinearize(-lb) : delinearize(lb);
}

double relativeLuminance(double r, double g, double b) {
    return 0.2126 * linearize(r) + 0.7152 * linearize(g) + 0.0722 * linearize(b);
}

std::string formatHex(double r, double g, double b) {
    const auto byte = [](double c) {
        const double v = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
        return static_cast<int>(std::lround(v * 255.0));
    };
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", byte(r), byte(g), byte(b));
    return std::string(buffer);
}

}  // namespace xt::bundle::detail
