// EffectProgram::Shine (gpu_frame.h ShineParams: dir 0-1, center 2, edge0 3, edge1 4, rgb 5-7,
// intensity 8, target size 9-10). Mirrors gl_shaders.h kFxShineFrag (top-down window px).
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let screenPx = vec2f(in.uv.x * pf(9u), in.uv.y * pf(10u));
    let u = screenPx.x * pf(0u) + screenPx.y * pf(1u);
    let band = 1.0 - smoothstepGuarded(pf(3u), pf(4u), abs(u - pf(2u)));
    let c = tex0(in.uv);
    let s = select(vec3f(0.0), c.rgb / c.a, c.a > 1e-5);
    let color = vec3f(pf(5u), pf(6u), pf(7u));
    let outRgb = clamp(s + color * band * pf(8u) * c.a, vec3f(0.0), vec3f(1.0));
    return vec4f(outRgb * c.a, c.a);
}
