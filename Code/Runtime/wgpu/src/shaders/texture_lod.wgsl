// T-665 (Docs/design/10-backends.md section 10.11 item 7): Shading::Texture / TextureStraight with
// DrawCmd::mipLod -- trilinear at the explicit lod of gpu_frame.h TextureLodParams (lod 0, maxLevel
// 1, straight 2). Mirrors gl_shaders.h kTextureLodFrag.
fn levelSample(uv: vec2f, level: f32) -> vec4f {
    let s = textureSampleLevel(t0, smp, uv, level);
    if (pf(2u) != 0.0) {
        return vec4f(s.rgb * s.a, s.a);
    }
    return s;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let lod = pf(0u);
    let maxLevel = pf(1u);
    let lo = min(floor(lod), maxLevel);
    let hi = min(lo + 1.0, maxLevel);
    let t = select(lod - lo, 0.0, lo >= maxLevel);
    var s = levelSample(in.uv, lo);
    if (t > 0.0) {
        s = mix(s, levelSample(in.uv, hi), t);
    }
    return s * in.color;
}
