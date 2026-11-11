// T-660b: Shading::Gradient with DrawCmd::coverage (gpu_frame.h CoverageGradientParams: kind, p0,
// p1, rx, ry, alpha at pf(0..7), uvToLocal a b c d e f at pf(8..13)). gradient.wgsl evaluated at
// the path-local position uvToLocal(uv), times the coverage. t0 = the 256x1 straight-color LUT,
// t1 = the R8 coverage page (both LinearClamp). Mirrors gl_shaders.h kCoverageGradientFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let kind = pi(0u);
    let p0 = vec2f(pf(1u), pf(2u));
    let p1 = vec2f(pf(3u), pf(4u));
    let radialAxes = vec2f(pf(5u), pf(6u));
    let paintAlpha = pf(7u);
    let local = vec2f(pf(8u) * in.uv.x + pf(10u) * in.uv.y + pf(12u),
                      pf(9u) * in.uv.x + pf(11u) * in.uv.y + pf(13u));
    let lutWidth = 256.0;
    let d = p1 - p0;
    let lenSq = dot(d, d);
    var t: f32;
    if (lenSq <= 0.0) {
        t = 1.0;
    } else {
        let v = local - p0;
        if (kind == 4) {
            t = atan2(d.x * v.y - d.y * v.x, dot(d, v)) * 0.15915494309;
            t = select(t, t + 1.0, t < 0.0);
        } else {
            t = select(dot(v, d) / lenSq, length(v / radialAxes), kind == 3);
        }
        t = clamp(t, 0.0, 1.0);
    }
    let u = (t * (lutWidth - 1.0) + 0.5) / lutWidth;
    let c = tex0(vec2f(u, 0.5));
    let a = c.a * paintAlpha * in.color.a * tex1(in.uv).r;
    return vec4f(c.rgb * a, a);
}
