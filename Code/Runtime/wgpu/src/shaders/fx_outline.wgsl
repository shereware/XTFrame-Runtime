// EffectProgram::Outline (gpu_frame.h OutlineParams: radius 0, softness 1, position 2, rgba 3-6).
// Mirrors gl_shaders.h kFxOutlineFrag; t1 = the R32Float distance field, read with textureLoad (GL
// samples it GL_NEAREST at texel centers; R32Float is not filterable in WebGPU).
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let dims = vec2i(textureDimensions(t1));
    let tc = clamp(vec2i(floor(in.uv * vec2f(dims))), vec2i(0), dims - vec2i(1));
    let sdf = textureLoad(t1, tc, 0).r;
    let radius = pf(0u);
    let softness = pf(1u);
    let position = pi(2u);
    var m: f32;
    var lineOnTop: bool;
    if (position == 0) {
        m = select(0.0, 1.0 - smoothstepGuarded(radius, radius + softness, sdf), sdf > 0.0);
        lineOnTop = false;
    } else if (position == 1) {
        m = 1.0 - smoothstepGuarded(radius * 0.5, radius * 0.5 + softness, abs(sdf));
        lineOnTop = false;
    } else {
        m = select(0.0, 1.0 - smoothstepGuarded(radius, radius + softness, -sdf), sdf <= 0.0);
        lineOnTop = true;
    }
    let lineA = m * pf(6u);
    let line = vec4f(vec3f(pf(3u), pf(4u), pf(5u)) * lineA, lineA);
    let src = tex0(in.uv);
    let bottom = select(line, src, lineOnTop);
    let top = select(src, line, lineOnTop);
    return vec4f(top.rgb + bottom.rgb * (1.0 - top.a), top.a + bottom.a * (1.0 - top.a));
}
