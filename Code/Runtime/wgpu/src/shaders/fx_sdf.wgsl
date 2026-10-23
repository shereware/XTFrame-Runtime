// EffectProgram::Sdf (gpu_frame.h SdfParams: texel 0-1, searchRadius 2). Mirrors gl_shaders.h
// kFxSdfFrag; writes an R32Float target.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let texel = vec2f(pf(0u), pf(1u));
    let searchRadius = pf(2u);
    let inside = tex0(in.uv).a >= 0.5;
    var minDist = searchRadius + 1.0;
    var dirs = array<vec2f, 8>(vec2f(1.0, 0.0), vec2f(-1.0, 0.0), vec2f(0.0, 1.0), vec2f(0.0, -1.0),
                               vec2f(0.70710678, 0.70710678), vec2f(-0.70710678, 0.70710678),
                               vec2f(0.70710678, -0.70710678), vec2f(-0.70710678, -0.70710678));
    let maxSteps = i32(ceil(searchRadius));
    for (var d = 0; d < 8; d++) {
        let dir = dirs[d];
        for (var s = 1; s <= 64; s++) {
            if (s > maxSteps) {
                break;
            }
            let uv = in.uv + dir * f32(s) * texel;
            if (!insideUnit(uv)) {
                break;
            }
            let otherInside = tex0(uv).a >= 0.5;
            if (otherInside != inside) {
                minDist = min(minDist, f32(s));
                break;
            }
        }
    }
    let sdf = select(minDist, -minDist, inside);
    return vec4f(sdf, sdf, sdf, sdf);
}
