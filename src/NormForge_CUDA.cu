// NormForge_CUDA.cu
// Phase 5: CUDA bridge for NormForge.
// Compiled by nvcc via VS CUDA Build Customization.

#include <cuda_runtime.h>
#include <cstddef>

#include "NormForge_GPU.h"

// Display enum values mirrored from NormForge.h (kept in sync with the host
// side; if those values change, update both files).
#define NF_DISPLAY_OFF                1
#define NF_DISPLAY_ADJUSTED_NORMALS   3
#define NF_DISPLAY_ORIGINAL_NORMALS   4
#define NF_DISPLAY_DEPTH              5
#define NF_DISPLAY_DIFFUSE            7
#define NF_DISPLAY_SPECULAR           8
#define NF_DISPLAY_INCIDENCE          9
#define NF_DISPLAY_RIM                10
#define NF_DISPLAY_TOON               11
#define NF_DISPLAY_GRADIENT           12
#define NF_DISPLAY_MATCAP             13
#define NF_DISPLAY_REFLECTION         14
#define NF_DISPLAY_REFRACTION         15
#define NF_DISPLAY_TEXTURE            16
#define NF_DISPLAY_BUMP               17
#define NF_DISPLAY_ALL                19

// Channel selector mirrored from NormForge.h
#define NF_CHAN_R    1
#define NF_CHAN_G    2
#define NF_CHAN_B    3
#define NF_CHAN_ONE  4
#define NF_CHAN_ZERO 5

__global__ void NormForge_TestKernel(int* d_out)
{
    if (threadIdx.x == 0) {
        *d_out = 42;
    }
}

extern "C" int NormForge_CUDA_GetDeviceCount()
{
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) return -1;
    return count;
}

// Step G1 (0.6.0): GPU pass-through helper. Used by the SmartFX GPU path
// for input pass-through (Display = OFF) and as a fallback before the
// shading kernel was wired up.
extern "C" int NormForge_CUDA_PassThrough(
    const void *src, void *dst,
    size_t src_pitch_bytes, size_t dst_pitch_bytes,
    size_t row_bytes, size_t height)
{
    cudaError_t err = cudaMemcpy2D(
        dst, dst_pitch_bytes,
        src, src_pitch_bytes,
        row_bytes, height,
        cudaMemcpyDeviceToDevice);
    return (err == cudaSuccess) ? 0 : (int)err;
}

// ---- Helpers ----------------------------------------------------------------

// BGRA128 layout: float4.x=B, .y=G, .z=R, .w=A.
// CHAN selector (1=R, 2=G, 3=B, 4=ONE, 5=ZERO) mirrors GetChannel<Pixel>().
__device__ __forceinline__
float nf_get_channel(float4 p, int sel)
{
    if (sel == NF_CHAN_R)    return p.z;
    if (sel == NF_CHAN_G)    return p.y;
    if (sel == NF_CHAN_B)    return p.x;
    if (sel == NF_CHAN_ONE)  return 1.0f;
    return 0.0f;  // CHAN_ZERO or unknown
}

__device__ __forceinline__
float nf_decode_normal(float raw, int invert)
{
    if (invert) raw = 1.0f - raw;
    return raw * 2.0f - 1.0f;
}

__device__ __forceinline__
float nf_luma(float r, float g, float b)
{
    return r * 0.21267f + g * 0.71516f + b * 0.072169f;
}

// ---- Layer sampling helpers (G5) -------------------------------------------
// Mirrors host TileWrap (NormForge.cpp): 1=None (-1 OOB), 2=Edge (clamp),
// 3=Repeat (modulo), 4=Mirror (modulo on 2W with reflect).
__device__ __forceinline__
int nf_tile_wrap(int mode, int max_idx, int v)
{
    if (v >= 0 && v <= max_idx) return v;
    if (mode == 1) return -1;
    if (mode == 2) return (v < 0) ? 0 : max_idx;
    int w = max_idx + 1;
    if (mode == 3) {
        v = v % w;
        if (v < 0) v += w;
        return v;
    }
    if (mode == 4) {
        int w2 = w * 2;
        v = v % w2;
        if (v < 0) v += w2;
        if (v >= w) v = w2 - 1 - v;
        return v;
    }
    return (v < 0) ? 0 : max_idx;
}

// Nearest sample of a BGRA128 layer at integer (u, v). Returns float4 with
// .x=B, .y=G, .z=R, .w=A. If the wrap mode is None and the coordinate falls
// outside the layer, returns zero.
__device__ __forceinline__
float4 nf_sample_nearest(const float4 *layer, int pitch, int w, int h,
                         int tile_mode, int u, int v)
{
    int uw = nf_tile_wrap(tile_mode, w - 1, u);
    int vw = nf_tile_wrap(tile_mode, h - 1, v);
    if (uw < 0 || vw < 0) {
        return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return layer[vw * pitch + uw];
}

// Bilinear sample at fractional (u_pix, v_pix). The four taps are wrapped by
// nf_tile_wrap; out-of-bounds taps contribute zero (matches host behavior
// where the four corners individually fall back to None).
__device__ __forceinline__
float4 nf_sample_bilinear(const float4 *layer, int pitch, int w, int h,
                          int tile_mode, float u_pix, float v_pix)
{
    int u0 = (int)floorf(u_pix);
    int v0 = (int)floorf(v_pix);
    float fu = u_pix - (float)u0;
    float fv = v_pix - (float)v0;
    int u0w = nf_tile_wrap(tile_mode, w - 1, u0);
    int v0w = nf_tile_wrap(tile_mode, h - 1, v0);
    int u1w = nf_tile_wrap(tile_mode, w - 1, u0 + 1);
    int v1w = nf_tile_wrap(tile_mode, h - 1, v0 + 1);

    auto tap = [&] (int xc, int yc) -> float4 {
        if (xc < 0 || yc < 0) return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        return layer[yc * pitch + xc];
    };
    float4 p00 = tap(u0w, v0w);
    float4 p10 = tap(u1w, v0w);
    float4 p01 = tap(u0w, v1w);
    float4 p11 = tap(u1w, v1w);

    float ufm = 1.0f - fu;
    float vfm = 1.0f - fv;
    float4 r;
    r.x = vfm * (ufm * p00.x + fu * p10.x) + fv * (ufm * p01.x + fu * p11.x);
    r.y = vfm * (ufm * p00.y + fu * p10.y) + fv * (ufm * p01.y + fu * p11.y);
    r.z = vfm * (ufm * p00.z + fu * p10.z) + fv * (ufm * p01.z + fu * p11.z);
    r.w = vfm * (ufm * p00.w + fu * p10.w) + fv * (ufm * p01.w + fu * p11.w);
    return r;
}

// ---- Box Blur (G5-c) ------------------------------------------------------
// Separable 3-pass box blur on BGRA128 (float4). One thread per row (H pass)
// or one thread per column (V pass) maintains a running sum + count, mirroring
// the host BoxBlur1D_H / BoxBlur1D_V code. Edge handling is clamp via the
// running average (cnt < 2*radius+1 near borders).

__global__ void NormForge_BoxBlur1D_H_Kernel(
    const float4 *src, float4 *dst,
    int w, int h, int src_pitch, int dst_pitch, int radius)
{
    int y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= h) return;

    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f, sum_w = 0.0f;
    int cnt = 0;
    int lim = (radius < (w - 1)) ? radius : (w - 1);
    for (int x = 0; x <= lim; ++x) {
        float4 p = src[y * src_pitch + x];
        sum_x += p.x; sum_y += p.y; sum_z += p.z; sum_w += p.w;
        ++cnt;
    }

    for (int x = 0; x < w; ++x) {
        float invc = (cnt > 0) ? 1.0f / (float)cnt : 0.0f;
        float4 r;
        r.x = sum_x * invc;
        r.y = sum_y * invc;
        r.z = sum_z * invc;
        r.w = sum_w * invc;
        dst[y * dst_pitch + x] = r;

        int add = x + radius + 1;
        int rem = x - radius;
        if (add < w) {
            float4 p = src[y * src_pitch + add];
            sum_x += p.x; sum_y += p.y; sum_z += p.z; sum_w += p.w;
            ++cnt;
        }
        if (rem >= 0) {
            float4 p = src[y * src_pitch + rem];
            sum_x -= p.x; sum_y -= p.y; sum_z -= p.z; sum_w -= p.w;
            --cnt;
        }
    }
}

__global__ void NormForge_BoxBlur1D_V_Kernel(
    const float4 *src, float4 *dst,
    int w, int h, int src_pitch, int dst_pitch, int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= w) return;

    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f, sum_w = 0.0f;
    int cnt = 0;
    int lim = (radius < (h - 1)) ? radius : (h - 1);
    for (int y = 0; y <= lim; ++y) {
        float4 p = src[y * src_pitch + x];
        sum_x += p.x; sum_y += p.y; sum_z += p.z; sum_w += p.w;
        ++cnt;
    }

    for (int y = 0; y < h; ++y) {
        float invc = (cnt > 0) ? 1.0f / (float)cnt : 0.0f;
        float4 r;
        r.x = sum_x * invc;
        r.y = sum_y * invc;
        r.z = sum_z * invc;
        r.w = sum_w * invc;
        dst[y * dst_pitch + x] = r;

        int add = y + radius + 1;
        int rem = y - radius;
        if (add < h) {
            float4 p = src[add * src_pitch + x];
            sum_x += p.x; sum_y += p.y; sum_z += p.z; sum_w += p.w;
            ++cnt;
        }
        if (rem >= 0) {
            float4 p = src[rem * src_pitch + x];
            sum_x -= p.x; sum_y -= p.y; sum_z -= p.z; sum_w -= p.w;
            --cnt;
        }
    }
}

extern "C" int NormForge_CUDA_BoxBlur1D(
    const void *src, void *dst,
    int width, int height,
    int src_pitch, int dst_pitch,
    int radius, int direction)
{
    if (radius <= 0) {
        // No-op: caller should skip in this case, but if we land here just
        // copy via cudaMemcpy2D so the destination is at least populated.
        cudaError_t err = cudaMemcpy2D(
            dst, (size_t)dst_pitch * 16,
            src, (size_t)src_pitch * 16,
            (size_t)width * 16, (size_t)height,
            cudaMemcpyDeviceToDevice);
        return (err == cudaSuccess) ? 0 : (int)err;
    }

    const int block_size = 128;
    if (direction == 0 /* horizontal */) {
        int blocks = (height + block_size - 1) / block_size;
        NormForge_BoxBlur1D_H_Kernel<<<blocks, block_size>>>(
            (const float4 *)src, (float4 *)dst,
            width, height, src_pitch, dst_pitch, radius);
    } else /* vertical */ {
        int blocks = (width + block_size - 1) / block_size;
        NormForge_BoxBlur1D_V_Kernel<<<blocks, block_size>>>(
            (const float4 *)src, (float4 *)dst,
            width, height, src_pitch, dst_pitch, radius);
    }

    cudaError_t err = cudaPeekAtLastError();
    return (err == cudaSuccess) ? 0 : (int)err;
}

// ---- Blend modes (G4-c) ---------------------------------------------------
// Direct port of the host BlendChannel switch in NormForge.cpp. Modes
// 1..30 cover the standard transfer-mode set. HSL modes (26..29) fall back
// to Add on the host as well; do the same here so CPU/GPU parity holds.

__device__ __forceinline__
float nf_blend_channel(int mode, float strength, float src, float dst)
{
    float u = 1.0f - strength;
    switch (mode) {
        case 1:  return strength * src + u * dst;                       // Normal
        case 2:  return (dst + src) * 0.5f * strength + dst * u;        // Average
        case 3:  return ((dst < src) ? dst : src) * strength + dst * u; // Darken
        case 4:  return dst * src * strength + dst * u;                 // Multiply
        case 5: { // Color Burn
            if (src == 0.0f) return dst;
            return (1.0f - (1.0f - dst) / src) * strength + dst * u;
        }
        case 6: { // Inverse Color Burn
            if (dst == 0.0f) return dst;
            return (1.0f - (1.0f - src) / dst) * strength + dst * u;
        }
        case 7:  return ((dst + src) - 1.0f) * strength + dst * u;      // Subtract
        case 8:  return (src < dst) ? (u * dst + src * strength) : dst; // Darker
        case 9:  return ((dst > src) ? dst : src) * strength + dst * u; // Lighten
        case 10: return dst + src * strength;                           // Add
        case 11: return (1.0f - (1.0f - src) * (1.0f - dst)) * strength + dst * u; // Screen
        case 12: return (dst / fmaxf(1.0f - src, 1e-6f)) * strength + dst * u;     // Color Dodge
        case 13: return (src / fmaxf(1.0f - dst, 1e-6f)) * strength + dst * u;     // Inverse Color Dodge
        case 14: return (src > dst) ? (u * dst + src * strength) : dst; // Lighter
        case 15: { // Divide
            if (src == 0.0f) return 1.0f;
            return u * dst + (dst * strength) / src;
        }
        case 16: { // Overlay
            float r = (dst >= 0.5f)
                ? (1.0f - (1.0f - dst) * 2.0f * (1.0f - src))
                : (dst * src);
            return r * strength + dst * u;
        }
        case 17: { // Soft Light
            float r;
            if (src >= 0.5f) {
                r = sqrtf(fmaxf(dst, 0.0f)) * (src * 2.0f - 1.0f) + dst * 2.0f * (1.0f - src);
            } else {
                float d2 = dst * dst;
                r = d2 * (1.0f - src * 2.0f) + dst * 2.0f * src;
            }
            return r * strength + dst * u;
        }
        case 18: { // Hard Light
            float r = (src >= 0.5f)
                ? (1.0f - (1.0f - dst) * 2.0f * (1.0f - src))
                : (dst * 2.0f * src);
            return r * strength + dst * u;
        }
        case 19: { // Reflect
            float r = (src == 1.0f) ? (u * dst + strength)
                                    : ((dst * dst) / fmaxf(1.0f - src, 1e-6f) * strength + dst * u);
            return r;
        }
        case 20: { // Glow
            float r = (dst == 1.0f) ? (u * dst + strength)
                                    : ((src * src) / fmaxf(1.0f - dst, 1e-6f) * strength + dst * u);
            return r;
        }
        case 21: return fabsf(dst - src) * strength + dst * u;          // Difference
        case 22: return ((src + dst) - dst * 2.0f * src) * strength + dst * u; // Exclusion
        case 23: return ((dst + src) - 0.5f) * strength + dst * u;      // Grain Merge
        case 24: return powf(fmaxf(dst, 0.0f), 1.0f / fmaxf(src, 1e-6f)) * strength + dst * u; // Exp A^B
        case 25: return powf(fmaxf(src, 0.0f), 1.0f / fmaxf(dst, 1e-6f)) * strength + dst * u; // Exp B^A
        case 26: case 27: case 28: case 29:
            // HSL (Hue/Saturation/Color/Luminosity): not implemented on either
            // side. Both fall back to Add for parity.
            return dst + src * strength;
        case 30: { // Phoenix
            float lo = fminf(dst, src);
            float hi = fmaxf(dst, src);
            return ((lo - hi) + 1.0f) * strength + dst * u;
        }
        default: return dst + src * strength;
    }
}

__device__ __forceinline__
void nf_apply_blend(int mode, float strength,
                    float src_r, float src_g, float src_b,
                    float &dst_r, float &dst_g, float &dst_b)
{
    dst_r = nf_blend_channel(mode, strength, src_r, dst_r);
    dst_g = nf_blend_channel(mode, strength, src_g, dst_g);
    dst_b = nf_blend_channel(mode, strength, src_b, dst_b);
}

// ---- Diffuse kernel (deprecated path, kept for back-compat in case we ----
// ---- need to fall back to the leaner G3-b path during debugging). The ----
// ---- shading kernel below subsumes its functionality. ---------------------

__global__ void NormForge_DiffuseKernel(
    const float4 *src, const float4 *nm, float4 *dst,
    NF_ShadingParams p)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || y >= p.height) return;

    float Nx = 0.0f, Ny = 0.0f, Nz = 1.0f;
    if (p.has_nm) {
        int nm_xc = (x < p.nm_width)  ? x : (p.nm_width  - 1);
        int nm_yc = (y < p.nm_height) ? y : (p.nm_height - 1);
        float4 px = nm[nm_yc * p.nm_pitch + nm_xc];
        Nx = px.z * 2.0f - 1.0f;
        Ny = px.y * 2.0f - 1.0f;
        Nz = px.x * 2.0f - 1.0f;
        float nl = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
        if (nl > 1e-5f) { Nx /= nl; Ny /= nl; Nz /= nl; }
    }

    float comp_x = (float)x * p.sx;
    float comp_y = (float)y * p.sy;

    float dr = 0.0f, dg = 0.0f, db = 0.0f;

    for (int i = 0; i < p.num_lights; ++i) {
        if (!p.lights[i].enabled) continue;
        float lx = p.lights[i].x - comp_x;
        float ly = p.lights[i].y - comp_y;
        float lz = -p.lights[i].z;
        float dist = sqrtf(lx*lx + ly*ly + lz*lz);
        if (dist < 0.001f) continue;
        float Lx = lx / dist, Ly = ly / dist, Lz = lz / dist;
        float NdotL = Nx*Lx + Ny*Ly + Nz*Lz;
        if (NdotL < 0.0f) NdotL = 0.0f;

        float falloff_coeff = 1.0f;
        if (p.falloff_mode != 1) {
            float eff = (p.falloff_val > 1e-3f) ? p.falloff_val : 1e-3f;
            float s = dist * 0.01f / eff + 1e-4f;
            if      (p.falloff_mode == 2) falloff_coeff = 1.0f / s;
            else if (p.falloff_mode == 3) falloff_coeff = 1.0f / (s * s);
            else if (p.falloff_mode == 4) falloff_coeff = 1.0f / (s * s * s);
        }

        float intensity_scale = p.lights[i].intensity * 0.01f * falloff_coeff;
        float dc = NdotL * intensity_scale * p.diffuse_amount;
        dr += dc * p.lights[i].r;
        dg += dc * p.lights[i].g;
        db += dc * p.lights[i].b;
    }

    float fr = dr * p.diff_r * p.amb_r * 2.0f + p.ainc_r;
    float fg = dg * p.diff_g * p.amb_g * 2.0f + p.ainc_g;
    float fb = db * p.diff_b * p.amb_b * 2.0f + p.ainc_b;

    fr = powf(fmaxf(fr * p.ev_global, 0.0f), p.inv_gamma);
    fg = powf(fmaxf(fg * p.ev_global, 0.0f), p.inv_gamma);
    fb = powf(fmaxf(fb * p.ev_global, 0.0f), p.inv_gamma);

    float4 out;
    out.x = fb;
    out.y = fg;
    out.z = fr;
    out.w = 1.0f;
    dst[y * p.dst_pitch + x] = out;
}

// ---- Step G4-a (0.6.5): Shading kernel ------------------------------------
// Replicates the Shading + Specular + Incidence sub-passes of
// ShadingRowFnT<PF_PixelFloat> with full Channel/Invert/Strength config and
// AO Map / Specular Map per-pixel modulation. Display = DIFFUSE / SPECULAR /
// INCIDENCE / ALL / OFF supported on GPU; other displays are gated off in
// SmartPreRender so AE falls back to CPU.

__global__ void NormForge_ShadingKernel(
    const float4 *src,
    const float4 *nm,
    const float4 *ao,
    const float4 *specmap,
    const float4 *bump,
    const float4 *uvmap,
    const float4 *texmap,
    const float4 *matcap,
    const float4 *envmap,
    const float4 *refrmap,
    const float4 *depthmap,
    float4       *dst,
    NF_ShadingParams p)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || y >= p.height) return;

    // ---- Normal Map sample with channel/invert/strength config ----
    // Nx/Ny/Nz are normalized for shading. Nx_raw/Ny_raw/Nz_raw are the
    // post-invert [0,1] values used by Rim Light and Gradient (G4-b).
    float Nx = 0.0f, Ny = 0.0f, Nz = 1.0f;
    float Nx_raw = 0.5f, Ny_raw = 0.5f, Nz_raw = 1.0f;
    float N_alpha = 1.0f;
    if (p.has_nm) {
        int nm_xc = (x < p.nm_width)  ? x : (p.nm_width  - 1);
        int nm_yc = (y < p.nm_height) ? y : (p.nm_height - 1);
        float4 px = nm[nm_yc * p.nm_pitch + nm_xc];
        float rx = nf_get_channel(px, p.chan_x);
        float ry = nf_get_channel(px, p.chan_y);
        float rz = nf_get_channel(px, p.chan_z);
        Nx = nf_decode_normal(rx, p.inv_x);
        Ny = nf_decode_normal(ry, p.inv_y);
        Nz = nf_decode_normal(rz, p.inv_z);
        float nl = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
        if (nl > 1e-5f) { Nx /= nl; Ny /= nl; Nz /= nl; }
        Nx_raw = p.inv_x ? (1.0f - rx) : rx;
        Ny_raw = p.inv_y ? (1.0f - ry) : ry;
        Nz_raw = p.inv_z ? (1.0f - rz) : rz;
        if (p.use_normal_alpha) N_alpha = px.w;
    }

    // Normal Strength: scale tangent (X/Y) deviation, keep Nz, renormalize.
    if (p.has_nm && fabsf(p.normal_strength - 1.0f) > 1e-5f) {
        Nx *= p.normal_strength;
        Ny *= p.normal_strength;
        float ns_l = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
        if (ns_l > 1e-5f) { Nx /= ns_l; Ny /= ns_l; Nz /= ns_l; }
    }

    // Bump (G5-a): sample Bump Map and add to N (then renormalize).
    if (p.enable_bump && p.has_bump_map &&
        p.bump_intensity != 0.0f && p.bump_scale > 1e-5f &&
        p.bump_width > 0 && p.bump_height > 0)
    {
        float scale_inv = 1.0f / p.bump_scale;
        float u_pix = ((float)x - (float)p.bump_offset_x) * scale_inv;
        float v_pix = ((float)y - (float)p.bump_offset_y) * scale_inv;

        float br = 0.0f, bg = 0.0f, bb = 0.0f;
        bool sampled = false;

        if (p.bump_filter == 2 /* Nearest */) {
            int u = (int)floorf(u_pix);
            int v = (int)floorf(v_pix);
            int uw = nf_tile_wrap(p.bump_tile, p.bump_width  - 1, u);
            int vw = nf_tile_wrap(p.bump_tile, p.bump_height - 1, v);
            if (uw >= 0 && vw >= 0) {
                float4 bp = bump[vw * p.bump_pitch + uw];
                br = bp.z;  // R
                bg = bp.y;  // G
                bb = bp.x;  // B
                sampled = true;
            }
        } else /* Bilinear */ {
            float4 bp = nf_sample_bilinear(bump, p.bump_pitch,
                                            p.bump_width, p.bump_height,
                                            p.bump_tile, u_pix, v_pix);
            br = bp.z; bg = bp.y; bb = bp.x;
            // Tile = None can yield zeros for out-of-bounds taps; that's
            // fine - the host treats the same case as no contribution via
            // bilinear-with-None. A fully-OOB pixel (all four taps failed)
            // would produce 0,0,0 and the grayscale check below would set
            // r_n=g_n=1 with bb=0, contributing nothing. Effectively `sampled
            // = (any tap valid)`; we mark sampled=true to mirror the host
            // bilinear path (which only skips on initial wrap rejection).
            sampled = true;
        }

        if (sampled) {
            float r_n, g_n;
            if (br == bg && bb == bg) {
                // Grayscale source: tangent contribution zero, only Z.
                r_n = 1.0f;
                g_n = 1.0f;
            } else {
                r_n = br * 2.0f - 1.0f;
                g_n = bg * 2.0f - 1.0f;
            }
            float b_n = (bb * 2.0f - 1.0f) * p.bump_intensity;
            Nx += r_n * p.bump_intensity;
            Ny += g_n * p.bump_intensity;
            Nz += b_n;
            float bn_l = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
            if (bn_l > 1e-5f) { Nx /= bn_l; Ny /= bn_l; Nz /= bn_l; }
        }
    }

    // ---- Depth Map sampling (G5-e) ----
    // Bilinearly samples Depth Map at output (col, row) scaled to depth dims,
    // mirrors CPU PreprocessDepthMapT (BT.709 luminance, optional invert,
    // multiplied by depth_strength). When depth map is unconnected, pixel_z
    // stays at 0.0 (preserves legacy lighting behavior).
    float pixel_z = 0.0f;
    if (p.has_depthmap && p.depth_strength != 0.0f &&
        p.depth_width > 0 && p.depth_height > 0)
    {
        float fw = (float)p.depth_width;
        float fh = (float)p.depth_height;
        float u_f = (p.width  > 1) ? ((float)x * (fw - 1.0f) / (float)(p.width  - 1)) : 0.0f;
        float v_f = (p.height > 1) ? ((float)y * (fh - 1.0f) / (float)(p.height - 1)) : 0.0f;
        int u0 = (int)floorf(u_f);
        int v0 = (int)floorf(v_f);
        int u1 = (u0 + 1 < p.depth_width)  ? (u0 + 1) : (p.depth_width  - 1);
        int v1 = (v0 + 1 < p.depth_height) ? (v0 + 1) : (p.depth_height - 1);
        float fu = u_f - (float)u0;
        float fv = v_f - (float)v0;
        if (u0 < 0) { u0 = 0; fu = 0.0f; }
        if (v0 < 0) { v0 = 0; fv = 0.0f; }
        float4 d00 = depthmap[v0 * p.depth_pitch + u0];
        float4 d10 = depthmap[v0 * p.depth_pitch + u1];
        float4 d01 = depthmap[v1 * p.depth_pitch + u0];
        float4 d11 = depthmap[v1 * p.depth_pitch + u1];
        // BT.709 luminance (CPU precision, not nf_luma's higher-precision)
        auto lum = [](float r, float g, float b) {
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        };
        float l00 = lum(d00.z, d00.y, d00.x);
        float l10 = lum(d10.z, d10.y, d10.x);
        float l01 = lum(d01.z, d01.y, d01.x);
        float l11 = lum(d11.z, d11.y, d11.x);
        float a = l00 * (1.0f - fu) + l10 * fu;
        float b = l01 * (1.0f - fu) + l11 * fu;
        float d = a * (1.0f - fv) + b * fv;
        if (p.depth_invert) d = 1.0f - d;
        pixel_z = d * p.depth_strength;
    }

    // ---- Per-light loop: accumulate Diffuse and Specular ----
    float comp_x = (float)x * p.sx;
    float comp_y = (float)y * p.sy;

    float dr = 0.0f, dg = 0.0f, db = 0.0f;
    float sr = 0.0f, sg = 0.0f, sb = 0.0f;
    float toon_factor = 0.0f;  // max NdotL across enabled lights (Toon, G4-b)

    for (int i = 0; i < p.num_lights; ++i) {
        if (!p.lights[i].enabled) continue;
        float lx = p.lights[i].x - comp_x;
        float ly = p.lights[i].y - comp_y;
        // CPU formula: lz = -li.z + pixel_z (AE Z away-from-camera flipped).
        float lz = -p.lights[i].z + pixel_z;
        float dist = sqrtf(lx*lx + ly*ly + lz*lz);
        if (dist < 0.001f) continue;
        float Lx = lx / dist;
        float Ly = ly / dist;
        float Lz = lz / dist;
        float NdotL = Nx*Lx + Ny*Ly + Nz*Lz;
        if (NdotL < 0.0f) NdotL = 0.0f;
        if (NdotL > toon_factor) toon_factor = NdotL;

        float falloff_coeff = 1.0f;
        if (p.falloff_mode != 1) {
            float eff = (p.falloff_val > 1e-3f) ? p.falloff_val : 1e-3f;
            float s = dist * 0.01f / eff + 1e-4f;
            if      (p.falloff_mode == 2) falloff_coeff = 1.0f / s;
            else if (p.falloff_mode == 3) falloff_coeff = 1.0f / (s * s);
            else if (p.falloff_mode == 4) falloff_coeff = 1.0f / (s * s * s);
        }

        float intensity_scale = p.lights[i].intensity * 0.01f * falloff_coeff;

        if (p.enable_shading) {
            float dc = NdotL * intensity_scale * p.diffuse_amount;
            dr += dc * p.lights[i].r;
            dg += dc * p.lights[i].g;
            db += dc * p.lights[i].b;
        }

        if (p.enable_specular) {
            // Cross product specular: spec_raw = 1 - |N x L|. Powered by
            // shininess. NormForge-specific (not standard Blinn-Phong).
            float cx = Ny*Lz - Nz*Ly;
            float cy = Nz*Lx - Nx*Lz;
            float cz = Nx*Ly - Ny*Lx;
            float spec_raw = 1.0f - sqrtf(cx*cx + cy*cy + cz*cz);
            if (spec_raw < 0.0f) spec_raw = 0.0f;
            float spec = powf(spec_raw, p.shininess);
            float sc = spec * intensity_scale;
            sr += sc * p.spec_r * p.lights[i].r;
            sg += sc * p.spec_g * p.lights[i].g;
            sb += sc * p.spec_b * p.lights[i].b;
        }
    }

    // ---- AO Map: per-pixel Ambient luminance modulator ----
    float ao_factor = 1.0f;
    if (p.has_ao) {
        int ao_xc = (x < p.ao_width)  ? x : (p.ao_width  - 1);
        int ao_yc = (y < p.ao_height) ? y : (p.ao_height - 1);
        float4 ap = ao[ao_yc * p.ao_pitch + ao_xc];
        ao_factor = nf_luma(ap.z, ap.y, ap.x);
        if (ao_factor < 0.0f) ao_factor = 0.0f;
    }

    // ---- Shading composition (Diffuse + Ambient + AO + Incandescence) ----
    float fr = 0.0f, fg = 0.0f, fb = 0.0f;
    if (p.enable_shading) {
        fr = dr * p.diff_r * p.amb_r * ao_factor * 2.0f + p.ainc_r;
        fg = dg * p.diff_g * p.amb_g * ao_factor * 2.0f + p.ainc_g;
        fb = db * p.diff_b * p.amb_b * ao_factor * 2.0f + p.ainc_b;
    }

    // ---- Specular Map: per-pixel Specular intensity modulator ----
    if (p.has_specmap) {
        int sm_xc = (x < p.specmap_width)  ? x : (p.specmap_width  - 1);
        int sm_yc = (y < p.specmap_height) ? y : (p.specmap_height - 1);
        float4 sp = specmap[sm_yc * p.specmap_pitch + sm_xc];
        float spec_mask = nf_luma(sp.z, sp.y, sp.x);
        if (spec_mask < 0.0f) spec_mask = 0.0f;
        sr *= spec_mask;
        sg *= spec_mask;
        sb *= spec_mask;
    }

    // exposure / gamma applied to fr/fg/fb and sr/sg/sb (matches CPU order;
    // ir/ig/ib intentionally NOT post-gamma'd, mirroring CPU behavior).
    fr = powf(fmaxf(fr * p.ev_global, 0.0f), p.inv_gamma);
    fg = powf(fmaxf(fg * p.ev_global, 0.0f), p.inv_gamma);
    fb = powf(fmaxf(fb * p.ev_global, 0.0f), p.inv_gamma);
    sr = powf(fmaxf(sr * p.ev_global, 0.0f), p.inv_gamma);
    sg = powf(fmaxf(sg * p.ev_global, 0.0f), p.inv_gamma);
    sb = powf(fmaxf(sb * p.ev_global, 0.0f), p.inv_gamma);

    // ---- Incidence: angular falloff toward grazing angles ----
    float ir = 0.0f, ig = 0.0f, ib = 0.0f;
    if (p.enable_incidence) {
        float raw = 1.0f - Nz;
        if (raw < 0.0f) raw = 0.0f;
        float falloff_exp = 1.0f / fmaxf(p.inc_falloff, 0.01f);
        float inc = powf(raw, falloff_exp);
        ir = inc * p.inc_r;
        ig = inc * p.inc_g;
        ib = inc * p.inc_b;
    }

    // ---- Rim Light (G4-b): directional incidence ----
    float rr = 0.0f, rg = 0.0f, rb = 0.0f;
    if (p.enable_rim) {
        float inc_factor = powf(1.001f - Nz_raw, 1.0f / fmaxf(p.rim_size, 0.01f));

        float angle_rad = p.rim_angle_deg * 0.017453292f;
        float s = sinf(angle_rad);
        float c = cosf(angle_rad);
        float dir;
        if (s <= 0.0f) dir = (1.0f - Nx_raw) * (-s);
        else           dir = s * Nx_raw;
        if (c <= 0.0f) dir -= (1.0f - Ny_raw) * c;
        else           dir += c * Ny_raw;
        if (dir < 0.0f) dir = 0.0f;
        float directional = powf(dir, 5.0f / fmaxf(p.rim_width, 0.01f));

        float intensity = inc_factor * directional * 4.0f;
        rr = intensity * p.rim_r;
        rg = intensity * p.rim_g;
        rb = intensity * p.rim_b;
    }

    // ---- Toon (G4-b): three-zone quantization ----
    float toon_r_out = 0.0f, toon_g_out = 0.0f, toon_b_out = 0.0f;
    if (p.enable_toon) {
        float smooth = fmaxf(p.toon_smoothing * 0.25f, 1e-5f);

        if (toon_factor <= p.toon_shadow_width) {
            if (toon_factor <= p.toon_shadow_width - smooth) {
                float t = p.toon_shadow_intensity;
                float u = 1.0f - t;
                toon_r_out = u * p.toon_mid_r + t * p.toon_shadow_r;
                toon_g_out = u * p.toon_mid_g + t * p.toon_shadow_g;
                toon_b_out = u * p.toon_mid_b + t * p.toon_shadow_b;
            } else {
                float t = ((p.toon_shadow_width - toon_factor) / smooth) * p.toon_shadow_intensity;
                float u = 1.0f - t;
                toon_r_out = t * p.toon_shadow_r + u * p.toon_mid_r;
                toon_g_out = t * p.toon_shadow_g + u * p.toon_mid_g;
                toon_b_out = t * p.toon_shadow_b + u * p.toon_mid_b;
            }
        } else if (toon_factor <= p.toon_hi_width) {
            if (toon_factor <= p.toon_hi_width - smooth) {
                toon_r_out = p.toon_mid_r;
                toon_g_out = p.toon_mid_g;
                toon_b_out = p.toon_mid_b;
            } else {
                float t = (p.toon_hi_width - toon_factor) / smooth;
                float u = 1.0f - t;
                toon_r_out = ((u * p.toon_hi_r + p.toon_mid_r * t) - p.toon_mid_r) * p.toon_hi_intensity + p.toon_mid_r;
                toon_g_out = ((u * p.toon_hi_g + p.toon_mid_g * t) - p.toon_mid_g) * p.toon_hi_intensity + p.toon_mid_g;
                toon_b_out = ((u * p.toon_hi_b + p.toon_mid_b * t) - p.toon_mid_b) * p.toon_hi_intensity + p.toon_mid_b;
            }
        } else {
            float t = p.toon_hi_intensity;
            float u = 1.0f - t;
            toon_r_out = u * p.toon_mid_r + t * p.toon_hi_r;
            toon_g_out = u * p.toon_mid_g + t * p.toon_hi_g;
            toon_b_out = u * p.toon_mid_b + t * p.toon_hi_b;
        }
    }

    // ---- Gradient (G4-b): luminance-based color tinting ----
    float gr_out = 0.0f, gg_out = 0.0f, gb_out = 0.0f;
    if (p.enable_gradient && p.gradient_amount > 1e-5f) {
        float lumi = nf_luma(Nx_raw, Ny_raw, Nz_raw);
        gr_out = p.gradient_r * lumi;
        gg_out = p.gradient_g * lumi;
        gb_out = p.gradient_b * lumi;
    }

    // ---- Matcap (G5-c) ----
    // Map Nx_raw / Ny_raw to UV on the matcap layer (Y inverted), apply Tile
    // Mode + Bilinear. When matcap_blur > 0, the host side has already
    // running-sum-blurred the matcap input into a separate buffer at the
    // same dimensions; we sample from that buffer instead.
    float matcap_r_out = 0.0f, matcap_g_out = 0.0f, matcap_b_out = 0.0f;
    bool  matcap_valid = false;
    if (p.enable_matcap && p.has_matcap &&
        p.matcap_width > 1 && p.matcap_height > 1)
    {
        float u_f = (float)p.matcap_width  * Nx_raw + (float)p.matcap_offset_x;
        float v_f = (float)p.matcap_height * (1.0f - Ny_raw) + (float)p.matcap_offset_y;

        int u0 = (int)floorf(u_f);
        int v0 = (int)floorf(v_f);
        int u0w = nf_tile_wrap(p.matcap_mode, p.matcap_width  - 1, u0);
        int v0w = nf_tile_wrap(p.matcap_mode, p.matcap_height - 1, v0);
        int u1w = nf_tile_wrap(p.matcap_mode, p.matcap_width  - 1, u0 + 1);
        int v1w = nf_tile_wrap(p.matcap_mode, p.matcap_height - 1, v0 + 1);
        if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
            float fu = u_f - (float)u0;
            float fv = v_f - (float)v0;
            float4 p00 = matcap[v0w * p.matcap_pitch + u0w];
            float4 p10 = matcap[v0w * p.matcap_pitch + u1w];
            float4 p01 = matcap[v1w * p.matcap_pitch + u0w];
            float4 p11 = matcap[v1w * p.matcap_pitch + u1w];
            float ufm = 1.0f - fu;
            float vfm = 1.0f - fv;
            matcap_r_out = vfm * (ufm * p00.z + fu * p10.z) + fv * (ufm * p01.z + fu * p11.z);
            matcap_g_out = vfm * (ufm * p00.y + fu * p10.y) + fv * (ufm * p01.y + fu * p11.y);
            matcap_b_out = vfm * (ufm * p00.x + fu * p10.x) + fv * (ufm * p01.x + fu * p11.x);
            matcap_valid = true;
        }
    }

    // ---- Texture (G5-b) ----
    // UV Map -> R/G as UV [0,1] -> sample Texture Map.
    // Mirrors CPU path: enable_texture + both layers connected (with width
    // and height > 1, matching the host >1 guard).
    float texture_r_out = 0.0f, texture_g_out = 0.0f, texture_b_out = 0.0f;
    bool  texture_valid = false;
    if (p.enable_texture && p.has_uv_map && p.has_tex_map &&
        p.tex_width > 1 && p.tex_height > 1)
    {
        // Sample UV Map at integer (col, row), clamp.
        int uv_xc = (x < p.uv_width)  ? x : (p.uv_width  - 1);
        int uv_yc = (y < p.uv_height) ? y : (p.uv_height - 1);
        float4 uvp = uvmap[uv_yc * p.uv_pitch + uv_xc];
        float u_norm = uvp.z;  // R
        float v_norm = uvp.y;  // G

        // UV Mirror: 1=Off, 2=X, 3=Y, 4=X+Y
        if (p.texture_uv_mirror == 2 || p.texture_uv_mirror == 4) u_norm = 1.0f - u_norm;
        if (p.texture_uv_mirror == 3 || p.texture_uv_mirror == 4) v_norm = 1.0f - v_norm;

        float u_pix = u_norm * (float)p.tex_width  - (float)p.texture_offset_x;
        float v_pix = v_norm * (float)p.tex_height - (float)p.texture_offset_y;

        if (p.texture_filter == 2 /* Nearest */) {
            int u = (int)floorf(u_pix);
            int v = (int)floorf(v_pix);
            int uw = nf_tile_wrap(p.texture_tile, p.tex_width  - 1, u);
            int vw = nf_tile_wrap(p.texture_tile, p.tex_height - 1, v);
            if (uw >= 0 && vw >= 0) {
                float4 tp = texmap[vw * p.tex_pitch + uw];
                texture_r_out = tp.z;
                texture_g_out = tp.y;
                texture_b_out = tp.x;
                texture_valid = true;
            }
        } else /* Bilinear */ {
            int u0 = (int)floorf(u_pix);
            int v0 = (int)floorf(v_pix);
            int u0w = nf_tile_wrap(p.texture_tile, p.tex_width  - 1, u0);
            int v0w = nf_tile_wrap(p.texture_tile, p.tex_height - 1, v0);
            int u1w = nf_tile_wrap(p.texture_tile, p.tex_width  - 1, u0 + 1);
            int v1w = nf_tile_wrap(p.texture_tile, p.tex_height - 1, v0 + 1);
            // Match host: only sample if all four taps are valid (no zero-fill
            // for OOB taps in this path; differs from nf_sample_bilinear which
            // zero-fills individual taps).
            if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                float fu = u_pix - (float)u0;
                float fv = v_pix - (float)v0;
                float4 p00 = texmap[v0w * p.tex_pitch + u0w];
                float4 p10 = texmap[v0w * p.tex_pitch + u1w];
                float4 p01 = texmap[v1w * p.tex_pitch + u0w];
                float4 p11 = texmap[v1w * p.tex_pitch + u1w];
                float ufm = 1.0f - fu;
                float vfm = 1.0f - fv;
                texture_r_out = vfm * (ufm * p00.z + fu * p10.z) + fv * (ufm * p01.z + fu * p11.z);
                texture_g_out = vfm * (ufm * p00.y + fu * p10.y) + fv * (ufm * p01.y + fu * p11.y);
                texture_b_out = vfm * (ufm * p00.x + fu * p10.x) + fv * (ufm * p01.x + fu * p11.x);
                texture_valid = true;
            }
        }
    }

    // ---- Reflection (G5-d) ----
    // Phase 4 T1 decision: view-independent reflection (R = N) for matcap-like
    // smoothness across the surface (the view-dependent formula leaves the
    // sphere edge fixed-color). Inclination/Azimuth rotate the normal before
    // the env-map UV is computed.
    float reflection_r_out = 0.0f, reflection_g_out = 0.0f, reflection_b_out = 0.0f;
    bool  reflection_valid = false;
    if (p.enable_reflection && p.has_envmap &&
        p.env_width > 1 && p.env_height > 1)
    {
        float Rx = Nx, Ry = Ny, Rz = Nz;

        float incl_rad = p.reflection_inclination_deg * 0.017453292f;
        float azim_rad = p.reflection_azimuth_deg     * 0.017453292f;
        float ci = cosf(incl_rad), si = sinf(incl_rad);
        float Rx2 = Rx;
        float Ry2 = Ry * ci - Rz * si;
        float Rz2 = Ry * si + Rz * ci;
        float ca = cosf(azim_rad), sa = sinf(azim_rad);
        float Rx3 =  Rx2 * ca + Rz2 * sa;
        float Ry3 =  Ry2;
        float Rz3 = -Rx2 * sa + Rz2 * ca;
        Rx = Rx3; Ry = Ry3; Rz = Rz3;

        float u_norm, v_norm;
        if (p.reflection_env_mode == 1) {
            // Spherical: uv from atan2/acos of normalized R.
            float r_len = sqrtf(Rx*Rx + Ry*Ry + Rz*Rz);
            if (r_len < 1e-5f) r_len = 1.0f;
            float Rxn = Rx / r_len, Ryn = Ry / r_len, Rzn = Rz / r_len;
            u_norm = atan2f(Rxn, Rzn) / 6.283185307f + 0.5f;
            float clamped_y = fmaxf(-1.0f, fminf(1.0f, Ryn));
            v_norm = acosf(clamped_y) / 3.141592653f;
        } else {
            // Panorama: simpler atan2 for U, linear Y for V.
            u_norm = atan2f(Rx, Rz) / 6.283185307f + 0.5f;
            v_norm = Ry * 0.5f + 0.5f;
        }

        // Scale: zoom around UV center (0.5, 0.5).
        float scale_safe = fmaxf(p.reflection_scale, 1e-3f);
        float scale_inv  = 1.0f / scale_safe;
        u_norm = (u_norm - 0.5f) * scale_inv + 0.5f;
        v_norm = (v_norm - 0.5f) * scale_inv + 0.5f;

        float u_pix = u_norm * (float)p.env_width;
        float v_pix = v_norm * (float)p.env_height;

        // Bilinear (matches host: sample only when all 4 taps are valid -
        // for None tile this is the natural OOB rejection; the host's
        // SampleBlurredRGB has the same behavior since a blurred buffer's
        // OOB is also handled by TileWrap).
        float sr_s = 0.0f, sg_s = 0.0f, sb_s = 0.0f;
        bool sampled = false;
        int u0 = (int)floorf(u_pix);
        int v0 = (int)floorf(v_pix);
        int u0w = nf_tile_wrap(p.reflection_tile, p.env_width  - 1, u0);
        int v0w = nf_tile_wrap(p.reflection_tile, p.env_height - 1, v0);
        int u1w = nf_tile_wrap(p.reflection_tile, p.env_width  - 1, u0 + 1);
        int v1w = nf_tile_wrap(p.reflection_tile, p.env_height - 1, v0 + 1);
        if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
            float fu = u_pix - (float)u0;
            float fv = v_pix - (float)v0;
            float4 e00 = envmap[v0w * p.env_pitch + u0w];
            float4 e10 = envmap[v0w * p.env_pitch + u1w];
            float4 e01 = envmap[v1w * p.env_pitch + u0w];
            float4 e11 = envmap[v1w * p.env_pitch + u1w];
            float ufm = 1.0f - fu;
            float vfm = 1.0f - fv;
            sr_s = vfm * (ufm * e00.z + fu * e10.z) + fv * (ufm * e01.z + fu * e11.z);
            sg_s = vfm * (ufm * e00.y + fu * e10.y) + fv * (ufm * e01.y + fu * e11.y);
            sb_s = vfm * (ufm * e00.x + fu * e10.x) + fv * (ufm * e01.x + fu * e11.x);
            sampled = true;
        }

        if (sampled) {
            // Gamma
            if (p.reflection_gamma > 1e-5f && fabsf(p.reflection_gamma - 1.0f) > 1e-5f) {
                float inv_g = 1.0f / p.reflection_gamma;
                sr_s = powf(fmaxf(0.0f, sr_s), inv_g);
                sg_s = powf(fmaxf(0.0f, sg_s), inv_g);
                sb_s = powf(fmaxf(0.0f, sb_s), inv_g);
            }

            // Schlick Fresnel: R0 = ((ior-1)/(ior+1))^2; F = R0 + (1-R0)(1-cos)^5
            float fresnel_factor = 1.0f;
            if (p.reflection_fresnel > 1e-5f) {
                float cos_theta = fmaxf(0.0f, Nz);
                float r0_factor = 0.04f;
                float ior_diff = (p.reflection_ior - 1.0f) / (p.reflection_ior + 1.0f);
                if (fabsf(p.reflection_ior + 1.0f) > 1e-5f) r0_factor = ior_diff * ior_diff;
                float F = r0_factor + (1.0f - r0_factor) * powf(1.0f - cos_theta, 5.0f);
                float depth_safe = fmaxf(0.01f, fabsf(p.reflection_fresnel_depth));
                float F_pow = powf(fmaxf(0.0f, F), 1.0f / depth_safe);
                fresnel_factor = fminf(1.0f, F_pow * p.reflection_fresnel);
            }

            reflection_r_out = sr_s * p.refl_color_r * fresnel_factor;
            reflection_g_out = sg_s * p.refl_color_g * fresnel_factor;
            reflection_b_out = sb_s * p.refl_color_b * fresnel_factor;
            reflection_valid = true;
        }
    }

    // ---- Refraction (G5-e) ----
    // screen-space refraction: sample point shifted by Nx/-Ny scaled by IOR
    // and Strength. Use Depth modulates by pixel_z. CPU same formula.
    float refraction_r_out = 0.0f, refraction_g_out = 0.0f, refraction_b_out = 0.0f;
    bool  refraction_valid = false;
    if (p.enable_refraction && p.has_refrmap &&
        p.refr_width > 1 && p.refr_height > 1)
    {
        float ior_safe = (fabsf(p.refraction_ior) < 1e-3f) ? 1e-3f : p.refraction_ior;
        float depth_modulator = (p.refraction_use_depth)
                                    ? (1.0f + pixel_z * 0.01f) : 1.0f;
        float ref_factor = (1.0f - 1.0f / ior_safe) * p.refraction_strength * depth_modulator;
        float dx = Nx * ref_factor;
        float dy = -Ny * ref_factor;  // AE Y-down

        float scale_inv = (p.refraction_scale > 1e-3f) ? 1.0f / p.refraction_scale : 1.0f;
        float u_pix = ((float)x - (float)p.refraction_offset_x) * scale_inv + dx;
        float v_pix = ((float)y - (float)p.refraction_offset_y) * scale_inv + dy;

        float sr_s = 0.0f, sg_s = 0.0f, sb_s = 0.0f;
        bool sampled = false;
        int u0 = (int)floorf(u_pix);
        int v0 = (int)floorf(v_pix);
        int u0w = nf_tile_wrap(p.refraction_tile, p.refr_width  - 1, u0);
        int v0w = nf_tile_wrap(p.refraction_tile, p.refr_height - 1, v0);
        int u1w = nf_tile_wrap(p.refraction_tile, p.refr_width  - 1, u0 + 1);
        int v1w = nf_tile_wrap(p.refraction_tile, p.refr_height - 1, v0 + 1);
        if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
            float fu = u_pix - (float)u0;
            float fv = v_pix - (float)v0;
            float4 r00 = refrmap[v0w * p.refr_pitch + u0w];
            float4 r10 = refrmap[v0w * p.refr_pitch + u1w];
            float4 r01 = refrmap[v1w * p.refr_pitch + u0w];
            float4 r11 = refrmap[v1w * p.refr_pitch + u1w];
            float ufm = 1.0f - fu;
            float vfm = 1.0f - fv;
            sr_s = vfm * (ufm * r00.z + fu * r10.z) + fv * (ufm * r01.z + fu * r11.z);
            sg_s = vfm * (ufm * r00.y + fu * r10.y) + fv * (ufm * r01.y + fu * r11.y);
            sb_s = vfm * (ufm * r00.x + fu * r10.x) + fv * (ufm * r01.x + fu * r11.x);
            sampled = true;
        }

        if (sampled) {
            if (p.refraction_gamma > 1e-5f && fabsf(p.refraction_gamma - 1.0f) > 1e-5f) {
                float inv_g = 1.0f / p.refraction_gamma;
                sr_s = powf(fmaxf(0.0f, sr_s), inv_g);
                sg_s = powf(fmaxf(0.0f, sg_s), inv_g);
                sb_s = powf(fmaxf(0.0f, sb_s), inv_g);
            }

            // Inverted Schlick (refraction transmits where reflection blocks).
            float fresnel_factor = 1.0f;
            if (p.refraction_fresnel > 1e-5f) {
                float cos_theta = fmaxf(0.0f, Nz);
                float ior_diff = (p.refraction_ior - 1.0f) / (p.refraction_ior + 1.0f);
                float r0_factor = ior_diff * ior_diff;
                if (fabsf(p.refraction_ior + 1.0f) < 1e-5f) r0_factor = 0.04f;
                float F = r0_factor + (1.0f - r0_factor) * powf(1.0f - cos_theta, 5.0f);
                F = 1.0f - F;
                float depth_safe = fmaxf(0.01f, fabsf(p.refraction_fresnel_depth));
                float F_pow = powf(fmaxf(0.0f, F), 1.0f / depth_safe);
                fresnel_factor = fminf(1.0f, F_pow * p.refraction_fresnel);
            }

            refraction_r_out = sr_s * p.refr_color_r * fresnel_factor;
            refraction_g_out = sg_s * p.refr_color_g * fresnel_factor;
            refraction_b_out = sb_s * p.refr_color_b * fresnel_factor;
            refraction_valid = true;
        }
    }

    // ---- Display switch ----
    float ar = 0.0f, ag = 0.0f, ab = 0.0f;
    if (p.display == NF_DISPLAY_DIFFUSE) {
        ar = fr; ag = fg; ab = fb;
    } else if (p.display == NF_DISPLAY_SPECULAR) {
        ar = sr * p.spec_amount;
        ag = sg * p.spec_amount;
        ab = sb * p.spec_amount;
    } else if (p.display == NF_DISPLAY_INCIDENCE) {
        ar = ir * p.inc_amount;
        ag = ig * p.inc_amount;
        ab = ib * p.inc_amount;
    } else if (p.display == NF_DISPLAY_RIM) {
        ar = rr * p.rim_amount;
        ag = rg * p.rim_amount;
        ab = rb * p.rim_amount;
    } else if (p.display == NF_DISPLAY_TOON) {
        ar = toon_r_out * p.toon_amount;
        ag = toon_g_out * p.toon_amount;
        ab = toon_b_out * p.toon_amount;
    } else if (p.display == NF_DISPLAY_GRADIENT) {
        ar = gr_out * p.gradient_amount;
        ag = gg_out * p.gradient_amount;
        ab = gb_out * p.gradient_amount;
    } else if (p.display == NF_DISPLAY_BUMP) {
        // Display = BUMP: encode the (Bump-modified) normalized normal
        // back to [0,1] for visualization. CPU writes the same value.
        ar = Nx * 0.5f + 0.5f;
        ag = Ny * 0.5f + 0.5f;
        ab = Nz * 0.5f + 0.5f;
    } else if (p.display == NF_DISPLAY_TEXTURE) {
        if (texture_valid) {
            ar = texture_r_out * p.texture_amount;
            ag = texture_g_out * p.texture_amount;
            ab = texture_b_out * p.texture_amount;
        } else {
            ar = 0.0f; ag = 0.0f; ab = 0.0f;
        }
    } else if (p.display == NF_DISPLAY_MATCAP) {
        if (matcap_valid) {
            ar = matcap_r_out * p.matcap_amount;
            ag = matcap_g_out * p.matcap_amount;
            ab = matcap_b_out * p.matcap_amount;
        } else {
            ar = 0.0f; ag = 0.0f; ab = 0.0f;
        }
    } else if (p.display == NF_DISPLAY_REFLECTION) {
        if (reflection_valid) {
            ar = reflection_r_out * p.reflection_amount;
            ag = reflection_g_out * p.reflection_amount;
            ab = reflection_b_out * p.reflection_amount;
        } else {
            ar = 0.0f; ag = 0.0f; ab = 0.0f;
        }
    } else if (p.display == NF_DISPLAY_REFRACTION) {
        if (refraction_valid) {
            ar = refraction_r_out * p.refraction_amount;
            ag = refraction_g_out * p.refraction_amount;
            ab = refraction_b_out * p.refraction_amount;
        } else {
            ar = 0.0f; ag = 0.0f; ab = 0.0f;
        }
    } else if (p.display == NF_DISPLAY_OFF) {
        // OFF should be input pass-through; SmartPreRender routes this case
        // through a separate cudaMemcpy2D on the host side, so we should not
        // see it here. Output black if it ever does land here.
        ar = 0.0f; ag = 0.0f; ab = 0.0f;
    } else if (p.display == NF_DISPLAY_ALL) {
        // G4-c: full ApplyBlend pipeline mirroring CPU's DISPLAY_ALL path.
        // Order matches NormForge.cpp: Shading -> (Matcap, Texture: G5)
        // -> Toon -> Specular -> Incidence -> Rim -> Gradient -> (Reflection,
        // Refraction: G5). Each section runs ApplyBlend with its own popup
        // mode and amount.
        float4 in_pix = src[y * p.src_pitch + x];
        ar = in_pix.z;
        ag = in_pix.y;
        ab = in_pix.x;

        if (p.enable_shading) {
            nf_apply_blend(p.shading_blend, p.shading_blend_amount,
                           fr, fg, fb, ar, ag, ab);
        }
        if (p.enable_matcap && matcap_valid) {
            nf_apply_blend(p.matcap_blend, p.matcap_amount,
                           matcap_r_out, matcap_g_out, matcap_b_out,
                           ar, ag, ab);
        }
        if (p.enable_texture && texture_valid) {
            nf_apply_blend(p.texture_blend, p.texture_amount,
                           texture_r_out, texture_g_out, texture_b_out,
                           ar, ag, ab);
        }
        if (p.enable_toon) {
            nf_apply_blend(p.toon_blend, p.toon_amount,
                           toon_r_out, toon_g_out, toon_b_out, ar, ag, ab);
        }
        if (p.enable_specular) {
            nf_apply_blend(p.specular_blend, p.spec_amount,
                           sr, sg, sb, ar, ag, ab);
        }
        if (p.enable_incidence) {
            nf_apply_blend(p.incidence_blend, p.inc_amount,
                           ir, ig, ib, ar, ag, ab);
        }
        if (p.enable_rim) {
            nf_apply_blend(p.rim_blend, p.rim_amount,
                           rr, rg, rb, ar, ag, ab);
        }
        if (p.enable_gradient && p.gradient_amount > 1e-5f) {
            nf_apply_blend(p.gradient_blend, p.gradient_amount,
                           gr_out, gg_out, gb_out, ar, ag, ab);
        }
        if (p.enable_reflection && reflection_valid) {
            nf_apply_blend(p.reflection_blend, p.reflection_amount,
                           reflection_r_out, reflection_g_out, reflection_b_out,
                           ar, ag, ab);
        }
        if (p.enable_refraction && refraction_valid) {
            nf_apply_blend(p.refraction_blend, p.refraction_amount,
                           refraction_r_out, refraction_g_out, refraction_b_out,
                           ar, ag, ab);
        }
    } else {
        // Other displays gated off in SmartPreRender; defensive fallback.
        ar = 0.0f; ag = 0.0f; ab = 0.0f;
    }

    // ---- Output BGRA128 ----
    float4 out;
    out.x = ab;
    out.y = ag;
    out.z = ar;
    out.w = (p.use_normal_alpha) ? N_alpha : 1.0f;
    dst[y * p.dst_pitch + x] = out;
}

extern "C" int NormForge_CUDA_RenderShading(
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
    NF_ShadingParams params)
{
    dim3 block(16, 16);
    dim3 grid((params.width  + 15) / 16,
              (params.height + 15) / 16);

    NormForge_ShadingKernel<<<grid, block>>>(
        (const float4 *)src_mem,
        (const float4 *)nm_mem,
        (const float4 *)ao_mem,
        (const float4 *)specmap_mem,
        (const float4 *)bump_mem,
        (const float4 *)uv_mem,
        (const float4 *)tex_mem,
        (const float4 *)matcap_mem,
        (const float4 *)env_mem,
        (const float4 *)refr_mem,
        (const float4 *)depth_mem,
        (float4 *)dst_mem,
        params);

    cudaError_t err = cudaPeekAtLastError();
    return (err == cudaSuccess) ? 0 : (int)err;
}
