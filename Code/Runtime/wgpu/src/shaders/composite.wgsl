// Shading::Composite (gpu_frame.h CompositeParams: mode, opacity at pf(0..1)). Mirrors gl_shaders.h
// kCompositeFrag: t0 = the layer (uSrc), t1 = the backdrop copy (uDst). Mode 0 (Normal) outputs
// src * opacity for the hardware SrcOver; every other mode computes the W3C result itself.
fn clipColor(cIn: vec3f) -> vec3f {
    var c = cIn;
    let l = lum(c);
    let n = min(min(c.r, c.g), c.b);
    let x = max(max(c.r, c.g), c.b);
    if (n < 0.0) {
        c = l + (c - l) * (l / max(l - n, 1e-5));
    }
    if (x > 1.0) {
        c = l + (c - l) * ((1.0 - l) / max(x - l, 1e-5));
    }
    return c;
}

fn setLum(c: vec3f, l: f32) -> vec3f {
    return clipColor(c + (l - lum(c)));
}

fn satOf(c: vec3f) -> f32 {
    return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
}

fn setSat(c: vec3f, s: f32) -> vec3f {
    let cmax = max(max(c.r, c.g), c.b);
    let cmin = min(min(c.r, c.g), c.b);
    var result = vec3f(0.0);
    if (cmax > cmin) {
        result = (c - cmin) * s / (cmax - cmin);
    }
    return result;
}

fn overlayCh(cb: f32, cs: f32) -> f32 {
    return select(1.0 - 2.0 * (1.0 - cb) * (1.0 - cs), 2.0 * cb * cs, cb <= 0.5);
}

fn dodgeCh(cb: f32, cs: f32) -> f32 {
    if (cb <= 0.0) {
        return 0.0;
    }
    if (cs >= 1.0) {
        return 1.0;
    }
    return min(1.0, cb / (1.0 - cs));
}

fn burnCh(cb: f32, cs: f32) -> f32 {
    if (cb >= 1.0) {
        return 1.0;
    }
    if (cs <= 0.0) {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - cb) / cs);
}

fn softCh(cb: f32, cs: f32) -> f32 {
    let d = select(sqrt(cb), ((16.0 * cb - 12.0) * cb + 4.0) * cb, cb <= 0.25);
    return select(cb + (2.0 * cs - 1.0) * (d - cb), cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb), cs <= 0.5);
}

fn blendChannel(cb: vec3f, cs: vec3f, mode: i32) -> vec3f {
    switch (mode) {
        case 0: { return cs; }                                              // Normal
        case 1: { return cb * cs; }                                         // Multiply
        case 2: { return cb + cs - cb * cs; }                               // Screen
        case 3: { return vec3f(overlayCh(cb.r, cs.r), overlayCh(cb.g, cs.g), overlayCh(cb.b, cs.b)); }
        case 4: { return min(cb, cs); }                                     // Darken
        case 5: { return max(cb, cs); }                                     // Lighten
        case 6: { return vec3f(dodgeCh(cb.r, cs.r), dodgeCh(cb.g, cs.g), dodgeCh(cb.b, cs.b)); }
        case 7: { return vec3f(burnCh(cb.r, cs.r), burnCh(cb.g, cs.g), burnCh(cb.b, cs.b)); }
        case 8: { return vec3f(overlayCh(cs.r, cb.r), overlayCh(cs.g, cb.g), overlayCh(cs.b, cb.b)); }  // HardLight
        case 9: { return vec3f(softCh(cb.r, cs.r), softCh(cb.g, cs.g), softCh(cb.b, cs.b)); }
        case 10: { return abs(cb - cs); }                                   // Difference
        case 11: { return cb + cs - 2.0 * cb * cs; }                        // Exclusion
        case 12: { return setLum(setSat(cs, satOf(cb)), lum(cb)); }         // Hue
        case 13: { return setLum(setSat(cb, satOf(cs)), lum(cb)); }         // Saturation
        case 14: { return setLum(cs, lum(cb)); }                            // Color
        case 15: { return setLum(cb, lum(cs)); }                            // Luminosity
        case 16: { return min(cb + cs, vec3f(1.0)); }                       // Add
        default: { return cs; }
    }
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let mode = pi(0u);
    let opacity = pf(1u);
    let src = tex0(in.uv);
    if (mode == 0) {
        return src * opacity;
    }
    let dst = tex1(in.uv);
    let as_ = src.a * opacity;
    let ab = dst.a;
    let cs = select(vec3f(0.0), src.rgb / src.a, src.a > 1e-5);
    let cb = select(vec3f(0.0), dst.rgb / dst.a, dst.a > 1e-5);
    let blended = blendChannel(cb, cs, mode);
    let rgb = (1.0 - ab) * as_ * cs + ab * as_ * blended + (1.0 - as_) * dst.rgb;
    let outA = as_ + ab * (1.0 - as_);
    return vec4f(rgb, outA);
}
