// NormForge_GPU.h
// Shared POD types between NormForge.cpp (host) and NormForge_CUDA.cu (device).
// Plain C-compatible structs only; no AE SDK or CUDA includes here.

#pragma once

#include <stddef.h>  // size_t

#define NF_GPU_MAX_LIGHTS 5

struct NF_Light {
    int   enabled;     // 0 or 1
    float x, y, z;     // comp space pixel coords (host: A_FpLong cast to float)
    float r, g, b;     // [0, 1]
    float intensity;   // AE intensity (~100 = 100%)
};

// Shading-section render params (G4-a, 0.6.5).
// Covers: Normal Map decode (channel/invert/strength/alpha), Diffuse,
// Specular, Incidence, AO Map, Specular Map. Display switch handles
// DIFFUSE / SPECULAR / INCIDENCE / ALL on GPU; other modes fall back to CPU
// via SmartPreRender gating. Rim / Toon / Gradient and full Blend modes
// arrive in G4-b / G4-c.
struct NF_ShadingParams {
    int width, height;        // output (and src) pixel dims
    int src_pitch;            // pixels per row (rowbytes / 16)
    int dst_pitch;
    int has_nm; int nm_pitch, nm_width, nm_height;
    int has_ao; int ao_pitch, ao_width, ao_height;
    int has_specmap; int specmap_pitch, specmap_width, specmap_height;

    // Display popup value (NormForge.h enum: DISPLAY_*)
    int display;

    // Normal Map decode
    int chan_x, chan_y, chan_z;       // 1=R, 2=G, 3=B, 4=ONE, 5=ZERO
    int inv_x, inv_y, inv_z;          // 0 / 1
    float normal_strength;            // 1.0 = identity
    int use_normal_alpha;             // 0 / 1

    // Shading section
    int enable_shading;
    float diff_r, diff_g, diff_b;     // diffuse_color  / 255
    float amb_r,  amb_g,  amb_b;      // ambient_color  / 255
    float ainc_r, ainc_g, ainc_b;     // amb * (incandescence / 255)
    float diffuse_amount;
    float shading_blend_amount;       // Shading Blend Amount (0..1, used for ALL composite)
    int   shading_blend;              // Shading Blend popup (1..30, G4-c)

    // Specular section
    int enable_specular;
    float spec_r, spec_g, spec_b;     // specular_color / 255
    float spec_amount;
    float shininess;                  // 5.0 / max(spread, 0.01)
    int   specular_blend;             // Specular Blend popup (G4-c)

    // Incidence section
    int enable_incidence;
    float inc_r, inc_g, inc_b;        // incidence_color / 255
    float inc_amount;
    float inc_falloff;
    int   incidence_blend;            // Incidence Blend popup (G4-c)

    // Rim Light section (G4-b)
    int   enable_rim;
    float rim_r, rim_g, rim_b;        // rim_color / 255
    float rim_amount;
    float rim_size;
    float rim_width;
    float rim_angle_deg;
    int   rim_blend;                  // Rim Blend popup (G4-c)

    // Toon section (G4-b)
    int   enable_toon;
    float toon_amount;
    float toon_smoothing;
    float toon_shadow_r, toon_shadow_g, toon_shadow_b;
    float toon_shadow_intensity;
    float toon_shadow_width;
    float toon_mid_r, toon_mid_g, toon_mid_b;
    float toon_hi_r, toon_hi_g, toon_hi_b;
    float toon_hi_intensity;
    float toon_hi_width;
    int   toon_blend;                 // Toon Blend popup (G4-c)

    // Gradient section (G4-b)
    int   enable_gradient;
    float gradient_r, gradient_g, gradient_b;  // gradient_color / 255
    float gradient_amount;
    int   gradient_blend;             // Gradient Blend popup (G4-c)

    // Bump section (G5-a)
    int   enable_bump;
    int   has_bump_map; int bump_pitch, bump_width, bump_height;
    float bump_intensity;
    int   bump_tile;                  // 1=None, 2=Edge, 3=Repeat, 4=Mirror
    int   bump_filter;                // 1=Bilinear, 2=Nearest
    int   bump_offset_x, bump_offset_y;
    float bump_scale;

    // Texture section (G5-b)
    int   enable_texture;
    int   has_uv_map;     int uv_pitch,  uv_width,  uv_height;
    int   has_tex_map;    int tex_pitch, tex_width, tex_height;
    float texture_amount;
    int   texture_tile;
    int   texture_uv_mirror;          // 1=Off, 2=X, 3=Y, 4=X+Y
    int   texture_filter;
    int   texture_offset_x, texture_offset_y;
    int   texture_blend;

    // Matcap section (G5-c)
    int   enable_matcap;
    int   has_matcap;     int matcap_pitch, matcap_width, matcap_height;
    float matcap_amount;
    int   matcap_offset_x, matcap_offset_y;
    int   matcap_mode;                // tile mode 1..4
    int   matcap_blur;                // blur radius (0 = no blur)
    int   matcap_blend;

    // Reflection section (G5-d)
    int   enable_reflection;
    int   has_envmap;     int env_pitch, env_width, env_height;
    float reflection_amount;
    int   reflection_env_mode;        // 1=Spherical, 2=Panorama
    float reflection_gamma;
    int   reflection_tile;
    int   reflection_blur;
    float refl_color_r, refl_color_g, refl_color_b;
    float reflection_inclination_deg;
    float reflection_azimuth_deg;
    float reflection_scale;
    float reflection_ior;
    float reflection_fresnel;
    float reflection_fresnel_depth;
    int   reflection_blend;

    // Refraction section (G5-e)
    int   enable_refraction;
    int   has_refrmap;    int refr_pitch, refr_width, refr_height;
    float refraction_amount;
    float refraction_ior;
    float refraction_strength;
    int   refraction_use_depth;       // 0/1
    float refraction_gamma;
    int   refraction_tile;
    int   refraction_blur;
    int   refraction_offset_x, refraction_offset_y;
    float refraction_scale;
    float refr_color_r, refr_color_g, refr_color_b;
    float refraction_fresnel;
    float refraction_fresnel_depth;
    int   refraction_blend;

    // Depth Map (G5-e, used by Refraction Use Depth and the Diffuse light
    // loop's pixel_z). When has_depthmap = 0, pixel_z is forced to 0.0 and
    // legacy behaviour is preserved.
    int   has_depthmap;   int depth_pitch, depth_width, depth_height;
    int   depth_invert;
    float depth_strength;

    // Common (Lights)
    float ev_global;                  // 2^exposure
    float inv_gamma;                  // 1 / gamma (clamped)
    int   falloff_mode;               // 1=Const, 2=Linear, 3=Quad, 4=Cubic
    float falloff_val;
    float sx, sy;                     // comp scale (downsample factor)

    int      num_lights;
    NF_Light lights[NF_GPU_MAX_LIGHTS];
};

#ifdef __cplusplus
extern "C" {
#endif

int NormForge_CUDA_GetDeviceCount();

int NormForge_CUDA_PassThrough(
    const void *src, void *dst,
    size_t src_pitch_bytes, size_t dst_pitch_bytes,
    size_t row_bytes, size_t height);

int NormForge_CUDA_RenderShading(
    const void *src_mem,
    const void *nm_mem,
    const void *ao_mem,
    const void *specmap_mem,
    const void *bump_mem,
    const void *uv_mem,
    const void *tex_mem,
    const void *matcap_mem,
    const void *env_mem,
    const void *refr_mem,
    const void *depth_mem,
    void *dst_mem,
    NF_ShadingParams params);

// G5-c: separable 3-pass box blur (Gaussian approximation) on a BGRA128
// layer. Apply H + V three times via ping-pong buffers (two tightly packed
// or rowbytes-aligned float4 buffers of the same dimensions).
//   direction: 0 = horizontal, 1 = vertical
//   src_pitch / dst_pitch: in float4 units (= rowbytes / 16)
//   skip_alpha (Phase 6-2 / 0.7.1): when nonzero, the alpha (w) component is
//     passed through from src to dst unchanged (no running-sum averaging).
//     Used for the Pre-Blur path with Blur Alpha = OFF; harmless for the
//     Matcap/Reflection/Refraction callers when set to 0.
int NormForge_CUDA_BoxBlur1D(
    const void *src, void *dst,
    int width, int height,
    int src_pitch, int dst_pitch,
    int radius, int direction, int skip_alpha);

#ifdef __cplusplus
}
#endif
