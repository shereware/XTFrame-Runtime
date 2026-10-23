// EffectProgram::MaskApply (gpu_frame.h MaskApplyParams: mode 0). Mirrors gl_shaders.h
// kFxMaskApplyFrag; t1 = the mask.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let mask = tex1(in.uv);
    let mode = pi(0u);
    var m: f32;
    if (mode == 1) {
        m = 1.0 - mask.a;
    } else if (mode == 2) {
        m = lum(mask.rgb);
    } else if (mode == 3) {
        m = 1.0 - lum(mask.rgb);
    } else {
        m = mask.a;
    }
    return tex0(in.uv) * m;
}
