#pragma once

#ifndef NORMFORGE_H
#define NORMFORGE_H

typedef unsigned char   u_char;
typedef unsigned short  u_short;
typedef unsigned short  u_int16;
typedef unsigned long   u_long;
typedef short int       int16;

#define PF_TABLE_BITS   12
#define PF_TABLE_SZ_16  4096

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"

#ifdef AE_OS_WIN
    typedef unsigned short PixelType;
    #define NOMINMAX
    #include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"

#include "NormForge_Strings.h"

/* Version */
#define MAJOR_VERSION   0
#define MINOR_VERSION   7
#define BUG_VERSION     2
#define STAGE_VERSION   PF_Stage_DEVELOP
#define BUILD_VERSION   0

/* Parameter indices */
enum {
    NF_INPUT = 0,

    NF_RENDER_MODE,

    NF_DISPLAY,

    NF_NORMALS_GROUP,
    NF_NORMAL_MAP,
    NF_USE_NORMAL_ALPHA,
    NF_CHANNEL_X,
    NF_INVERT_X,
    NF_CHANNEL_Y,
    NF_INVERT_Y,
    NF_CHANNEL_Z,
    NF_INVERT_Z,
    NF_PRE_BLUR,
    NF_PRE_BLUR_ALPHA,
    NF_NORMAL_STRENGTH,
    NF_NORMALS_GROUP_END,

    NF_DEPTH_GROUP,
    NF_DEPTH_MAP,
    NF_DEPTH_INVERT,
    NF_DEPTH_STRENGTH,
    NF_DEPTH_GROUP_END,

    NF_LIGHTS_GROUP,
    NF_LIGHTS_RENDER,
    NF_LIGHTS_INVISIBLE,
    NF_FALLOFF_MODE,
    NF_FALLOFF,
    NF_EXPOSURE,
    NF_GAMMA,
    NF_LIGHTS_GROUP_END,

    NF_SHADING_GROUP,
    NF_SHADING_ENABLE,
    NF_DIFFUSE,
    NF_DIFFUSE_COLOR,
    NF_AMBIENT,
    NF_AO_MAP,
    NF_INCANDESCENCE,
    NF_SHADING_BLEND,
    NF_SHADING_BLEND_AMOUNT,
    NF_SHADING_GROUP_END,

    NF_SPECULAR_GROUP,
    NF_SPECULAR_ENABLE,
    NF_SPECULAR_MAP,
    NF_SPECULAR,
    NF_SPECULAR_SPREAD,
    NF_SPECULAR_COLOR,
    NF_SPECULAR_BLEND,
    NF_SPECULAR_GROUP_END,

    NF_INCIDENCE_GROUP,
    NF_INCIDENCE_ENABLE,
    NF_INCIDENCE,
    NF_INCIDENCE_FALLOFF,
    NF_INCIDENCE_COLOR,
    NF_INCIDENCE_BLEND,
    NF_INCIDENCE_GROUP_END,

    NF_RIM_GROUP,
    NF_RIM_ENABLE,
    NF_RIM,
    NF_RIM_SIZE,
    NF_RIM_WIDTH,
    NF_RIM_COLOR,
    NF_RIM_ANGLE,
    NF_RIM_BLEND,
    NF_RIM_GROUP_END,

    NF_GRADIENT_GROUP,
    NF_GRADIENT_ENABLE,
    NF_GRADIENT,
    NF_GRADIENT_COLOR,
    NF_GRADIENT_BLEND,
    NF_GRADIENT_GROUP_END,

    NF_TOON_GROUP,
    NF_TOON_ENABLE,
    NF_TOON,
    NF_TOON_SMOOTHING,
    NF_TOON_SHADOW_COLOR,
    NF_TOON_SHADOW_INTENSITY,
    NF_TOON_SHADOW_WIDTH,
    NF_TOON_MID_COLOR,
    NF_TOON_HIGHLIGHT_WIDTH,
    NF_TOON_HIGHLIGHT_COLOR,
    NF_TOON_HIGHLIGHT_INTENSITY,
    NF_TOON_BLEND,
    NF_TOON_GROUP_END,

    NF_MATCAP_GROUP,
    NF_MATCAP_ENABLE,
    NF_MATCAP_MAP,
    NF_MATCAP,
    NF_MATCAP_OFFSET,
    NF_MATCAP_MODE,
    NF_MATCAP_BLUR,
    NF_MATCAP_BLEND,
    NF_MATCAP_GROUP_END,

    NF_REFLECTION_GROUP,
    NF_REFLECTION_ENABLE,
    NF_REFLECTION_ENV_MAP,
    NF_REFLECTION,
    NF_REFLECTION_ENV_MODE,
    NF_REFLECTION_GAMMA,
    NF_REFLECTION_TILE_MODE,
    NF_REFLECTION_BLUR,
    NF_REFLECTION_COLOR,
    NF_REFLECTION_INCLINATION,
    NF_REFLECTION_AZIMUTH,
    NF_REFLECTION_SCALE,
    NF_REFLECTION_IOR,
    NF_REFLECTION_FRESNEL,
    NF_REFLECTION_FRESNEL_DEPTH,
    NF_REFLECTION_BLEND,
    NF_REFLECTION_GROUP_END,

    NF_REFRACTION_GROUP,
    NF_REFRACTION_ENABLE,
    NF_REFRACTION_MAP,
    NF_REFRACTION,
    NF_REFRACTION_IOR,
    NF_REFRACTION_STRENGTH,
    NF_REFRACTION_USE_DEPTH,
    NF_REFRACTION_GAMMA,
    NF_REFRACTION_TILE_MODE,
    NF_REFRACTION_BLUR,
    NF_REFRACTION_OFFSET,
    NF_REFRACTION_SCALE,
    NF_REFRACTION_COLOR,
    NF_REFRACTION_FRESNEL,
    NF_REFRACTION_FRESNEL_DEPTH,
    NF_REFRACTION_BLEND,
    NF_REFRACTION_GROUP_END,

    NF_TEXTURE_GROUP,
    NF_TEXTURE_ENABLE,
    NF_TEXTURE_UV_MAP,
    NF_TEXTURE_MAP,
    NF_TEXTURE,
    NF_TEXTURE_TILE_MODE,
    NF_TEXTURE_UV_MIRROR,
    NF_TEXTURE_FILTER,
    NF_TEXTURE_OFFSET,
    NF_TEXTURE_BLEND,
    NF_TEXTURE_GROUP_END,

    NF_BUMP_GROUP,
    NF_BUMP_ENABLE,
    NF_BUMP_MAP,
    NF_BUMP_INTENSITY,
    NF_BUMP_TILE_MODE,
    NF_BUMP_FILTER,
    NF_BUMP_OFFSET,
    NF_BUMP_SCALE,
    NF_BUMP_GROUP_END,

    NF_NUM_PARAMS
};

/* Render Mode values (1-indexed popup, Step G2 / 0.6.1) */
enum {
    RENDER_MODE_AUTO = 1,   // GPU when 32bpc + CUDA available, else CPU
    RENDER_MODE_GPU,        // Force GPU (still falls back to CPU for 8/16bpc)
    RENDER_MODE_CPU         // Force CPU
};

/* Display mode values (1-indexed popup, separators "(-" occupy slots 2, 6, 18) */
enum {
    DISPLAY_OFF               = 1,
    // slot 2: separator
    DISPLAY_ADJUSTED_NORMALS  = 3,
    DISPLAY_ORIGINAL_NORMALS  = 4,
    DISPLAY_DEPTH             = 5,
    // slot 6: separator
    DISPLAY_DIFFUSE           = 7,
    DISPLAY_SPECULAR          = 8,
    DISPLAY_INCIDENCE         = 9,
    DISPLAY_RIM               = 10,
    DISPLAY_TOON              = 11,
    DISPLAY_GRADIENT          = 12,
    DISPLAY_MATCAP            = 13,
    DISPLAY_REFLECTION        = 14,
    DISPLAY_REFRACTION        = 15,
    DISPLAY_TEXTURE           = 16,
    DISPLAY_BUMP              = 17,
    // slot 18: separator
    DISPLAY_ALL               = 19
};

/* Lights render filter values (1-indexed popup) */
enum {
    RENDER_NONE = 1,
    RENDER_ALL,
    RENDER_LIGHT1,
    RENDER_LIGHT2,
    RENDER_LIGHT3,
    RENDER_LIGHT4,
    RENDER_LIGHT5,
    RENDER_LIGHT_ANY    // name prefix wildcard: matches any layer whose name starts with "Light" (0.7.2)
};

/* Falloff mode values (1-indexed popup) */
enum {
    FALLOFF_CONSTANT = 1,  // case 1: 1.0 (no distance attenuation)
    FALLOFF_LINEAR,        // case 2: 1/d  (default)
    FALLOFF_QUADRATIC,     // case 3: 1/d^2
    FALLOFF_CUBIC          // case 4: 1/d^3
};

/* Channel selector values (1-indexed popup) */
enum {
    CHAN_R = 1,
    CHAN_G,
    CHAN_B,
    CHAN_ONE,   // constant 1.0
    CHAN_ZERO   // constant 0.0
};

/* Blend mode values (1-indexed popup) */
enum {
    BLEND_NORMAL = 1,
    BLEND_AVERAGE,
    BLEND_ADD
};

/* Light data collected from AE comp */
#define MAX_LIGHTS 5

struct LightInfo {
    A_Boolean   enabled;
    A_FpLong    x, y, z;       // position in comp space (pixels)
    A_FpLong    r, g, b;       // color [0, 1]
    A_FpLong    intensity;      // AE intensity (0–100+ percentage)
};

/* Per-frame light snapshot, populated in SmartPreRender (per-frame thread,
 * AEGP suite call site) and consumed in SmartRender (per-pixel parallel,
 * no AEGP suite calls). Pattern D for Step 14b-γ MFR completion. */
struct NormForgeLightCache {
    LightInfo lights[MAX_LIGHTS];
    A_long    light_count;
};

extern "C" {
    DllExport
    PF_Err
    EffectMain(
        PF_Cmd      cmd,
        PF_InData   *in_data,
        PF_OutData  *out_data,
        PF_ParamDef *params[],
        PF_LayerDef *output,
        void        *extra);
}

#endif // NORMFORGE_H
