// T-668: Shading::BoxShadow (gpu_frame.h BoxShadowParams: x0, y0, x1, y1 at pf(0..3), the corner
// radii tl, tr, br, bl at pf(4..7), sigma at pf(8), the premultiplied colour at pf(12..15)).
// T-680: pf(9) = the number of extra signed terms (0..2), pf(10..11) their signs, their shapes at
// pf(16..23) and pf(24..31) (laid out like pf(0..7)); coverage = clamp(sum of sign x S, 0, 1). The
// coverage formula of Code/Runtime/core/src/box_shadow.cpp (Docs/design/10-backends.md §10.12
// item 2) at the local position in.uv: a 16-point Gauss-Legendre rule over y in
// [max(y0, p.y - 3 sigma), min(y1, p.y + 3 sigma)] of the Gaussian times the erf difference of the
// shape's span at that y. Constants as xt/box_shadow.h (A&S Table 25.4 n = 16; A&S 7.1.26).
// Mirrors gl_shaders.h kBoxShadowFrag.

fn bsErf(x: f32) -> f32 {
    let ax = abs(x);
    let t = 1.0 / (1.0 + 0.3275911 * ax);
    let poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    let y = 1.0 - poly * exp(-ax * ax);
    return select(y, -y, x < 0.0);
}

// The horizontal extent at height y (x0, x1) of the shape at pf(base..base+7); x1 <= x0 when empty.
fn bsSpan(base: u32, y: f32) -> vec2f {
    let sx0 = pf(base);
    let sy0 = pf(base + 1u);
    let sx1 = pf(base + 2u);
    let sy1 = pf(base + 3u);
    if (y < sy0 || y > sy1) {
        return vec2f(0.0, 0.0);
    }
    let rtl = pf(base + 4u);
    let rtr = pf(base + 5u);
    let rbr = pf(base + 6u);
    let rbl = pf(base + 7u);
    var x0 = sx0;
    if (y < sy0 + rtl) {
        let d = sy0 + rtl - y;
        x0 = sx0 + rtl - sqrt(max(0.0, rtl * rtl - d * d));
    } else if (y > sy1 - rbl) {
        let d = y - (sy1 - rbl);
        x0 = sx0 + rbl - sqrt(max(0.0, rbl * rbl - d * d));
    }
    var x1 = sx1;
    if (y < sy0 + rtr) {
        let d = sy0 + rtr - y;
        x1 = sx1 - rtr + sqrt(max(0.0, rtr * rtr - d * d));
    } else if (y > sy1 - rbr) {
        let d = y - (sy1 - rbr);
        x1 = sx1 - rbr + sqrt(max(0.0, rbr * rbr - d * d));
    }
    return vec2f(x0, x1);
}

// One abscissa: weight x Gaussian x the erf difference (0 when the span is empty).
fn bsTerm(base: u32, y: f32, w: f32, p: vec2f, invErf: f32, invTwoSigma2: f32) -> f32 {
    let s = bsSpan(base, y);
    if (!(s.y > s.x)) {
        return 0.0;
    }
    let t = y - p.y;
    let wg = w * exp(-t * t * invTwoSigma2);
    if (wg == 0.0) {
        return 0.0;
    }
    return wg * (0.5 * (bsErf((s.y - p.x) * invErf) - bsErf((s.x - p.x) * invErf)));
}

// The clamped coverage at p of the shape at pf(base..base+7) (box_shadow.cpp boxShadowCoverage).
fn bsCoverage(base: u32, p: vec2f, sigma: f32) -> f32 {
    let reach = 3.0 * sigma;
    let a = max(pf(base + 1u), p.y - reach);
    let b = min(pf(base + 3u), p.y + reach);
    if (!(a < b)) {
        return 0.0;
    }
    let h = 0.5 * (b - a);
    let m = 0.5 * (a + b);
    let invErf = 1.0 / (sigma * 1.41421356237309505);
    let k = 1.0 / (2.0 * sigma * sigma);
    var sum = 0.0;
    sum += bsTerm(base, m - h * 0.0950125098376374, 0.1894506104550685, p, invErf, k);
    sum += bsTerm(base, m + h * 0.0950125098376374, 0.1894506104550685, p, invErf, k);
    sum += bsTerm(base, m - h * 0.2816035507792589, 0.1826034150449236, p, invErf, k);
    sum += bsTerm(base, m + h * 0.2816035507792589, 0.1826034150449236, p, invErf, k);
    sum += bsTerm(base, m - h * 0.4580167776572274, 0.1691565193950025, p, invErf, k);
    sum += bsTerm(base, m + h * 0.4580167776572274, 0.1691565193950025, p, invErf, k);
    sum += bsTerm(base, m - h * 0.6178762444026438, 0.1495959888165767, p, invErf, k);
    sum += bsTerm(base, m + h * 0.6178762444026438, 0.1495959888165767, p, invErf, k);
    sum += bsTerm(base, m - h * 0.7554044083550030, 0.1246289712555339, p, invErf, k);
    sum += bsTerm(base, m + h * 0.7554044083550030, 0.1246289712555339, p, invErf, k);
    sum += bsTerm(base, m - h * 0.8656312023878318, 0.0951585116824928, p, invErf, k);
    sum += bsTerm(base, m + h * 0.8656312023878318, 0.0951585116824928, p, invErf, k);
    sum += bsTerm(base, m - h * 0.9445750230732326, 0.0622535239386479, p, invErf, k);
    sum += bsTerm(base, m + h * 0.9445750230732326, 0.0622535239386479, p, invErf, k);
    sum += bsTerm(base, m - h * 0.9894009349916499, 0.0271524594117541, p, invErf, k);
    sum += bsTerm(base, m + h * 0.9894009349916499, 0.0271524594117541, p, invErf, k);
    return clamp(sum * h * 0.398942280401432678 / sigma, 0.0, 1.0);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let p = in.uv;
    let sigma = pf(8u);
    var coverage = bsCoverage(0u, p, sigma);
    let extra = pf(9u);
    if (extra > 0.5) {
        coverage = coverage + pf(10u) * bsCoverage(16u, p, sigma);
        if (extra > 1.5) {
            coverage = coverage + pf(11u) * bsCoverage(24u, p, sigma);
        }
        coverage = clamp(coverage, 0.0, 1.0);
    }
    return vec4f(pf(12u), pf(13u), pf(14u), pf(15u)) * coverage;
}
