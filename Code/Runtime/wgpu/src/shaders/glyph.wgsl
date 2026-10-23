// Shading::Glyph (gpu_frame.h GlyphParams: gamma, effect, radius, rect x0 y0 x1 y1, texSize at
// pf(0..7)). Mirrors gl_shaders.h kGlyphFrag; t0 = the R8 atlas page.
fn cov(t: vec2i) -> f32 {
    if (t.x < i32(pf(3u)) || t.y < i32(pf(4u)) || t.x >= i32(pf(5u)) || t.y >= i32(pf(6u))) {
        return 0.0;
    }
    return textureLoad(t0, t, 0).r;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let effect = pi(1u);
    if (effect == 0) {
        let coverage = pow(tex0(in.uv).r, pf(0u));
        return in.color * coverage;
    }
    let radius = pf(2u);
    let p = vec2i(floor(in.uv * pf(7u)));
    var m = 0.0;
    if (effect == 1) {
        let k = select(0, i32(ceil(radius)), radius > 0.0);
        for (var dy = -k; dy <= k; dy++) {
            for (var dx = -k; dx <= k; dx++) {
                let wd = clamp(radius + 1.0 - sqrt(f32(dx * dx + dy * dy)), 0.0, 1.0);
                if (wd > 0.0) {
                    m = max(m, cov(p + vec2i(dx, dy)) * wd);
                }
            }
        }
    } else {
        let sigma = radius * 0.5;
        if (sigma <= 0.0) {
            m = cov(p);
        } else {
            let k = i32(ceil(3.0 * sigma));
            var sum = 0.0;
            var acc = 0.0;
            for (var dy = -k; dy <= k; dy++) {
                let wy = exp(-f32(dy * dy) / (2.0 * sigma * sigma));
                for (var dx = -k; dx <= k; dx++) {
                    let w = wy * exp(-f32(dx * dx) / (2.0 * sigma * sigma));
                    acc += w * cov(p + vec2i(dx, dy));
                    sum += w;
                }
            }
            m = acc / sum;
        }
    }
    return in.color * min(m, 1.0);
}
