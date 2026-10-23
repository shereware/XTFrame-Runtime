// xt_wgpu copy blit (T-438): a full-target triangle copying the texels of `src` offset by uBlit.xy
// (source texel = target texel + offset), limited by the scissor to the copied rectangle. Stands in
// for gl_device.cpp's copy-pass glBlitFramebuffer(GL_NEAREST) where copyTextureToTexture cannot be
// used (a multisampled destination, two formats, the host's surface view), and loads the host's
// surface into the 4x surface attachment. Vertex stage = gl_shaders.h kResolveVert's triangle.
// Standalone module (no prelude).
@group(0) @binding(0) var<uniform> uBlit: vec4f;
@group(0) @binding(1) var src: texture_2d<f32>;

@vertex
fn vs_blit(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((vi << 1u) & 2u), f32(vi & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_blit(@builtin(position) fc: vec4f) -> @location(0) vec4f {
    let p = vec2i(floor(fc.xy)) + vec2i(uBlit.xy);
    return textureLoad(src, p, 0);
}
