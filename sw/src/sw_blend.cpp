#include "sw_blend.h"

/// 17 blend modes (W3C Compositing and Blending Level 1). See docs/tasks/T-12.md "混合（sw_blend）".

namespace xt::sw {

namespace {

float multiplyF(float cb, float cs) { return cb * cs; }
float screenF(float cb, float cs) { return cb + cs - cb * cs; }

float hardLightF(float cb, float cs) {
    return cs <= 0.5f ? multiplyF(cb, 2.0f * cs) : screenF(cb, 2.0f * cs - 1.0f);
}

// Overlay(Cb, Cs) = HardLight(Cs, Cb).
float overlayF(float cb, float cs) { return hardLightF(cs, cb); }

float darkenF(float cb, float cs) { return std::min(cb, cs); }
float lightenF(float cb, float cs) { return std::max(cb, cs); }

float colorDodgeF(float cb, float cs) {
    if (cb <= 0.0f) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return std::min(1.0f, cb / (1.0f - cs));
}

float colorBurnF(float cb, float cs) {
    if (cb >= 1.0f) {
        return 1.0f;
    }
    if (cs <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - std::min(1.0f, (1.0f - cb) / cs);
}

float softLightDF(float cb) { return cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb); }

float softLightF(float cb, float cs) {
    return cs <= 0.5f ? cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb)
                      : cb + (2.0f * cs - 1.0f) * (softLightDF(cb) - cb);
}

float differenceF(float cb, float cs) { return std::fabs(cb - cs); }
float exclusionF(float cb, float cs) { return cb + cs - 2.0f * cb * cs; }
float addF(float cb, float cs) { return std::min(1.0f, cb + cs); }

// -------------------------------------------------------------------------------------------
// Non-separable modes (W3C §9.1.4): Lum/ClipColor/SetLum/Sat/SetSat operate on the whole RGB
// triple. Weights are the W3C-specified luminosity coefficients (distinct from the Rec. 709
// weights used by MaskMode::Luma).
// -------------------------------------------------------------------------------------------

float lumOf(const Color& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

Color clipColor(Color c) {
    float l = lumOf(c);
    float n = std::min({c.r, c.g, c.b});
    float x = std::max({c.r, c.g, c.b});
    if (n < 0.0f && l != n) {
        c.r = l + (c.r - l) * l / (l - n);
        c.g = l + (c.g - l) * l / (l - n);
        c.b = l + (c.b - l) * l / (l - n);
    }
    if (x > 1.0f && x != l) {
        c.r = l + (c.r - l) * (1.0f - l) / (x - l);
        c.g = l + (c.g - l) * (1.0f - l) / (x - l);
        c.b = l + (c.b - l) * (1.0f - l) / (x - l);
    }
    return c;
}

Color setLum(Color c, float l) {
    float d = l - lumOf(c);
    c.r += d;
    c.g += d;
    c.b += d;
    return clipColor(c);
}

float satOf(const Color& c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

Color setSat(Color c, float s) {
    // Work on (value, channel-pointer) pairs so the "max/mid/min" algorithm can write back to the
    // right channel regardless of which one is largest/smallest.
    float* channels[3] = {&c.r, &c.g, &c.b};
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return *channels[a] < *channels[b]; });
    float* cmin = channels[order[0]];
    float* cmid = channels[order[1]];
    float* cmax = channels[order[2]];
    if (*cmax > *cmin) {
        *cmid = (*cmid - *cmin) * s / (*cmax - *cmin);
        *cmax = s;
    } else {
        *cmid = 0.0f;
        *cmax = 0.0f;
    }
    *cmin = 0.0f;
    return c;
}

Color hueBlend(const Color& cb, const Color& cs) { return setLum(setSat(cs, satOf(cb)), lumOf(cb)); }
Color saturationBlend(const Color& cb, const Color& cs) { return setLum(setSat(cb, satOf(cs)), lumOf(cb)); }
Color colorBlend(const Color& cb, const Color& cs) { return setLum(cs, lumOf(cb)); }
Color luminosityBlend(const Color& cb, const Color& cs) { return setLum(cb, lumOf(cs)); }

/// B(Cb, Cs): the blend function itself, ignoring alpha (both colors treated as fully opaque).
Color applyBlendFunc(BlendMode mode, const Color& cb, const Color& cs) {
    switch (mode) {
        case BlendMode::Normal:
            return cs;
        case BlendMode::Multiply:
            return Color{multiplyF(cb.r, cs.r), multiplyF(cb.g, cs.g), multiplyF(cb.b, cs.b), 1};
        case BlendMode::Screen:
            return Color{screenF(cb.r, cs.r), screenF(cb.g, cs.g), screenF(cb.b, cs.b), 1};
        case BlendMode::Overlay:
            return Color{overlayF(cb.r, cs.r), overlayF(cb.g, cs.g), overlayF(cb.b, cs.b), 1};
        case BlendMode::Darken:
            return Color{darkenF(cb.r, cs.r), darkenF(cb.g, cs.g), darkenF(cb.b, cs.b), 1};
        case BlendMode::Lighten:
            return Color{lightenF(cb.r, cs.r), lightenF(cb.g, cs.g), lightenF(cb.b, cs.b), 1};
        case BlendMode::ColorDodge:
            return Color{colorDodgeF(cb.r, cs.r), colorDodgeF(cb.g, cs.g), colorDodgeF(cb.b, cs.b), 1};
        case BlendMode::ColorBurn:
            return Color{colorBurnF(cb.r, cs.r), colorBurnF(cb.g, cs.g), colorBurnF(cb.b, cs.b), 1};
        case BlendMode::HardLight:
            return Color{hardLightF(cb.r, cs.r), hardLightF(cb.g, cs.g), hardLightF(cb.b, cs.b), 1};
        case BlendMode::SoftLight:
            return Color{softLightF(cb.r, cs.r), softLightF(cb.g, cs.g), softLightF(cb.b, cs.b), 1};
        case BlendMode::Difference:
            return Color{differenceF(cb.r, cs.r), differenceF(cb.g, cs.g), differenceF(cb.b, cs.b), 1};
        case BlendMode::Exclusion:
            return Color{exclusionF(cb.r, cs.r), exclusionF(cb.g, cs.g), exclusionF(cb.b, cs.b), 1};
        case BlendMode::Hue:
            return hueBlend(cb, cs);
        case BlendMode::Saturation:
            return saturationBlend(cb, cs);
        case BlendMode::Color:
            return colorBlend(cb, cs);
        case BlendMode::Luminosity:
            return luminosityBlend(cb, cs);
        case BlendMode::Add:
            return Color{addF(cb.r, cs.r), addF(cb.g, cs.g), addF(cb.b, cs.b), 1};
    }
    return cs;
}

}  // namespace

Color blendPixel(BlendMode mode, Color src, Color dst) {
    float as = src.a;
    float ab = dst.a;
    Color cs{src.r, src.g, src.b, 1};
    Color cb{dst.r, dst.g, dst.b, 1};
    Color b = applyBlendFunc(mode, cb, cs);

    // Co = (1 - ab) * Cs + ab * B(Cb, Cs) (docs/tasks/T-12.md "混合（sw_blend）" general formula).
    Color co{
        (1.0f - ab) * cs.r + ab * b.r,
        (1.0f - ab) * cs.g + ab * b.g,
        (1.0f - ab) * cs.b + ab * b.b,
        1,
    };

    float ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) {
        return Color{0, 0, 0, 0};
    }
    return Color{
        (as * co.r + ab * cb.r * (1.0f - as)) / ao,
        (as * co.g + ab * cb.g * (1.0f - as)) / ao,
        (as * co.b + ab * cb.b * (1.0f - as)) / ao,
        ao,
    };
}

Color blendPixelPremul(BlendMode mode, const Color& src, const Color& dst) {
    if (mode == BlendMode::Normal) {
        return blendSrcOverPremul(src, dst);
    }
    return premultiply(blendPixel(mode, unpremultiply(src), unpremultiply(dst)));
}

}  // namespace xt::sw
