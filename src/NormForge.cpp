#include "NormForge.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef HAS_CUDA
#include "AE_EffectGPUSuites.h"
#include "AEFX_SuiteHelper.h"
#include "NormForge_GPU.h"
#endif

// ---- DLL search path setup for cudart64_12.dll (delay loading) ------------
// cudart64_12.dll is configured as a delay-loaded import (vcxproj
// <DelayLoadDLLs>) so that Windows does NOT try to resolve it at .aex load
// time. Before any CUDA function is called we add the .aex's own folder to
// the DLL search path via SetDllDirectoryA. This lets users place
// NormForge.aex and cudart64_12.dll together in
//   <AE>\Support Files\Plug-ins\NormForge\
// instead of having to drop the DLL alongside AfterFX.exe.
//
// SetDllDirectory is process-wide and is only consulted before the standard
// search paths (application dir, System32, Windows). Other plugins may
// overwrite it later, but cudart will already be in the loader cache by
// then so subsequent lookups succeed regardless.
static void EnsurePluginDllPath()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    HMODULE hModule = NULL;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&EnsurePluginDllPath),
            &hModule)) {
        return;
    }

    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    // Trim filename component, leaving only the folder.
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == '\\' || path[i - 1] == '/') {
            path[i - 1] = '\0';
            break;
        }
    }

    SetDllDirectoryA(path);
}

static PF_Err
About(
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));

#ifdef HAS_CUDA
    // Phase 5 PoC: keep CUDA symbol linked (no behavioral effect)
    volatile int dummy_gpu_count = NormForge_CUDA_GetDeviceCount();
    (void)dummy_gpu_count;
#endif

    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output)
{
    out_data->my_version = PF_VERSION(
        MAJOR_VERSION,
        MINOR_VERSION,
        BUG_VERSION,
        STAGE_VERSION,
        BUILD_VERSION);

    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_PIX_INDEPENDENT;
    out_data->out_flags2 = PF_OutFlag2_I_USE_3D_LIGHTS |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
                           PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    // Threaded rendering note (0.5.12):
    //   per-pixel body has no shared mutable state; lights/blur/depth buffers
    //   are read-only during render; output is row-disjoint via iterate_generic.
    //   AEGP suite calls in CollectLights() are technically not thread-safe per
    //   Adobe guidelines (suites are AEGP-only / main-thread by design). For
    //   light-using comps this can race; we accept the risk to opt-in to MFR
    //   for the common case (Shading without lights, or static lights that
    //   resolve identically across threads). Distribution docs note the caveat.

    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;

    // Render Mode (Step G2 / 0.6.1): top-level GPU/CPU selector.
    // 8/16bpc are always CPU (AE SDK constraint). For 32bpc, Auto/GPU pick
    // the GPU path while CPU forces CPU even when CUDA is available.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        "Render Mode",
        3,
        RENDER_MODE_AUTO,
        "Auto|GPU (32bpc only)|CPU only",
        NF_RENDER_MODE);

    // Display mode
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        "Display",
        19,
        DISPLAY_ALL,
        "Off|-|Adjusted Normals|Original Normals|Depth|-|"
        "Diffuse|Specular|Incidence|Rim|Toon|Gradient|"
        "Matcap|Reflection|Refraction|Texture|Bump|-|All",
        NF_DISPLAY);

    // --- Normals group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Normals", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_NORMALS_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Normal Map", PF_LayerDefault_NONE, NF_NORMAL_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Use Normal Map Alpha", "", TRUE, 0, NF_USE_NORMAL_ALPHA);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("X", 5, CHAN_R, "R|G|B|1.0|0.0", NF_CHANNEL_X);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Invert X", "", FALSE, 0, NF_INVERT_X);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Y", 5, CHAN_G, "R|G|B|1.0|0.0", NF_CHANNEL_Y);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Invert Y", "", FALSE, 0, NF_INVERT_Y);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Z", 5, CHAN_B, "R|G|B|1.0|0.0", NF_CHANNEL_Z);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Invert Z", "", FALSE, 0, NF_INVERT_Z);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Pre Blur", 0, 100, 0, 10, 0, NF_PRE_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Blur Alpha", "", FALSE, 0, NF_PRE_BLUR_ALPHA);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Normal Strength", 0.0, 5.0, 0.0, 2.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_NORMAL_STRENGTH);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_NORMALS_GROUP_END, &def));

    // --- Depth group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Depth", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_DEPTH_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Depth Map", PF_LayerDefault_NONE, NF_DEPTH_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Invert", "", FALSE, 0, NF_DEPTH_INVERT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        "Strength",
        -100000.0, 100000.0,
        -1000.0, 1000.0,
        100.0,
        3,
        PF_ValueDisplayFlag_NONE,
        0,
        NF_DEPTH_STRENGTH);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_DEPTH_GROUP_END, &def));

    // --- Lights group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Lights", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_LIGHTS_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Render", 8, RENDER_ALL,
        "None|All|Light 1|Light 2|Light 3|Light 4|Light 5|Light *",
        NF_LIGHTS_RENDER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable invisible lights", "", FALSE, 0, NF_LIGHTS_INVISIBLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Falloff Mode", 4, FALLOFF_LINEAR,
        "Constant|Linear|Quadratic|Cubic",
        NF_FALLOFF_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Falloff", 0.0, 5.0, 0.0, 5.0, 2.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_FALLOFF);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure", -2.0, 2.0, -2.0, 2.0, 0.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_EXPOSURE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gamma", 0.0, 5.0, 0.0, 5.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_GAMMA);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_LIGHTS_GROUP_END, &def));

    // --- Shading group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Shading", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_SHADING_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Shading", "", TRUE, 0, NF_SHADING_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        "Diffuse",
        0.0, 5.0,
        0.0, 5.0,
        1.0,
        3,
        PF_ValueDisplayFlag_NONE,
        0,
        NF_DIFFUSE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 255, 255, 255, NF_DIFFUSE_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Ambient", 127, 127, 127, NF_AMBIENT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("AO Map", PF_LayerDefault_NONE, NF_AO_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Incandescence", 0, 0, 0, NF_INCANDESCENCE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 1 /*Normal*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_SHADING_BLEND);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blend Amount", 0.0, 1.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_SHADING_BLEND_AMOUNT);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_SHADING_GROUP_END, &def));

    // --- Specular group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Specular", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_SPECULAR_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Specular", "", FALSE, 0, NF_SPECULAR_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Specular Map", PF_LayerDefault_NONE, NF_SPECULAR_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        "Specular",
        0.0, 20.0,
        0.0, 2.0,
        1.0,
        3,
        PF_ValueDisplayFlag_NONE,
        0,
        NF_SPECULAR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        "Spread",
        0.0, 10.0,
        0.0, 1.0,
        0.5,
        3,
        PF_ValueDisplayFlag_NONE,
        0,
        NF_SPECULAR_SPREAD);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 255, 255, 255, NF_SPECULAR_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 10 /*Add*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_SPECULAR_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_SPECULAR_GROUP_END, &def));

    // --- Incidence group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Incidence", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_INCIDENCE_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Incidence", "", FALSE, 0, NF_INCIDENCE_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Incidence", 0.0, 20.0, 0.0, 2.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_INCIDENCE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Falloff", 0.0, 10.0, 0.0, 1.0, 0.5, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_INCIDENCE_FALLOFF);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 255, 255, 255, NF_INCIDENCE_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 10 /*Add*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_INCIDENCE_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_INCIDENCE_GROUP_END, &def));

    // --- Rim Light group ---
    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Rim Light", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_RIM_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Rim Light", "", FALSE, 0, NF_RIM_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Rim Light", 0.0, 20.0, 0.0, 2.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_RIM);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Size", 0.0, 10.0, 0.0, 1.0, 0.8, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_RIM_SIZE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Width", 0.0, 10.0, 0.0, 1.0, 0.5, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_RIM_WIDTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 255, 255, 255, NF_RIM_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", 0, NF_RIM_ANGLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 10 /*Add*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_RIM_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_RIM_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Gradient", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_GRADIENT_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Gradient", "", FALSE, 0, NF_GRADIENT_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gradient", 0.0, 5.0, 0.0, 5.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_GRADIENT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 255, 255, 255, NF_GRADIENT_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, BLEND_AVERAGE,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_GRADIENT_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_GRADIENT_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Toon", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_TOON_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Toon", "", FALSE, 0, NF_TOON_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Toon", 0.0, 10.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Smoothing", 0.0, 1.0, 0.0, 1.0, 0.20, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON_SMOOTHING);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Shadow Color", 88, 88, 88, NF_TOON_SHADOW_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Intensity", 0.0, 1.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON_SHADOW_INTENSITY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Width", 0.0, 1.0, 0.0, 1.0, 0.30, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON_SHADOW_WIDTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 127, 127, 127, NF_TOON_MID_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Width", 0.0, 1.0, 0.0, 1.0, 0.70, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON_HIGHLIGHT_WIDTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Highlight Color", 172, 172, 173, NF_TOON_HIGHLIGHT_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Intensity", 0.0, 1.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TOON_HIGHLIGHT_INTENSITY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 1 /*Normal*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_TOON_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_TOON_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Matcap", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_MATCAP_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Matcap", "", FALSE, 0, NF_MATCAP_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Matcap Map", PF_LayerDefault_NONE, NF_MATCAP_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Matcap", 0.0, 10.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_MATCAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Offset", 0, 0, 0, NF_MATCAP_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Mode", 4, 2 /*Edge*/,
        "None|Edge|Repeat|Mirror", NF_MATCAP_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur", 0, 20, 0, 20, 0, NF_MATCAP_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 1 /*Normal*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_MATCAP_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_MATCAP_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Reflection", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_REFLECTION_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Reflection", "", FALSE, 0, NF_REFLECTION_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Environment Map", PF_LayerDefault_NONE, NF_REFLECTION_ENV_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Reflection", 0.0, 10.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Environment Mode", 2, 1 /*Spherical*/,
        "Spherical|Panorama", NF_REFLECTION_ENV_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gamma", 0.0, 3.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION_GAMMA);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Tile Mode", 4, 3 /*Repeat*/,
        "None|Edge|Repeat|Mirror", NF_REFLECTION_TILE_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur", 0, 20, 0, 20, 0, NF_REFLECTION_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 127, 127, 127, NF_REFLECTION_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Inclination", 0, NF_REFLECTION_INCLINATION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Azimuth", 0, NF_REFLECTION_AZIMUTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Scale", 0.0, 10.0, 0.0, 10.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION_SCALE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Index of Refraction", -50.0, 50.0, -3.0, 3.0, 0.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION_IOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fresnel", 0.0, 10.0, 0.0, 1.0, 0.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION_FRESNEL);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fresnel Depth", -5.0, 5.0, -1.0, 1.0, 0.5, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFLECTION_FRESNEL_DEPTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 10 /*Add*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_REFLECTION_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_REFLECTION_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Refraction", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_REFRACTION_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Refraction", "", FALSE, 0, NF_REFRACTION_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Refraction Map", PF_LayerDefault_NONE, NF_REFRACTION_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Refraction", 0.0, 10.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Index of Refraction", -10.0, 10.0, -3.0, 3.0, 1.5, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_IOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Strength", -100000.0, 100000.0, -1000.0, 1000.0, 50.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Use Depth", "", FALSE, 0, NF_REFRACTION_USE_DEPTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gamma", 0.0, 3.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_GAMMA);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Tile Mode", 4, 3 /*Repeat*/,
        "None|Edge|Repeat|Mirror", NF_REFRACTION_TILE_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Blur", 0, 50, 0, 50, 0, NF_REFRACTION_BLUR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Offset", 0, 0, 0, NF_REFRACTION_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Scale", 0.0, 10.0, 0.0, 10.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_SCALE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Color", 127, 127, 127, NF_REFRACTION_COLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fresnel", 0.0, 10.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_FRESNEL);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fresnel Depth", -5.0, 5.0, -1.0, 1.0, 0.5, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_REFRACTION_FRESNEL_DEPTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 10 /*Add*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_REFRACTION_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_REFRACTION_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Texture", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_TEXTURE_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Texture", "", FALSE, 0, NF_TEXTURE_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("UV Map", PF_LayerDefault_NONE, NF_TEXTURE_UV_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Texture Map", PF_LayerDefault_NONE, NF_TEXTURE_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Texture", 0.0, 1.0, 0.0, 1.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_TEXTURE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Tile Mode", 4, 2 /*Edge*/,
        "None|Edge|Repeat|Mirror", NF_TEXTURE_TILE_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("UV Mirror", 4, 1 /*Off (NormForge default)*/,
        "Off|X|Y|Both", NF_TEXTURE_UV_MIRROR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Filter", 2, 1 /*Bilinear*/,
        "Bilinear|Nearest", NF_TEXTURE_FILTER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Offset", 0, 0, 0, NF_TEXTURE_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Blend", 30, 1 /*Normal*/,
        "Normal|Average|Darken|Multiply|Color Burn|Inverse Color Burn|Subtract|Darker|Lighten|Add|Screen|Color Dodge|Inverse Color Dodge|Lighter|Divide|Overlay|Soft Light|Hard Light|Reflect|Glow|Difference|Exclusion|Grain Merge|Exponential (A^B)|Exponential (B^A)|Hue|Saturation|Color|Luminosity|Phoenix",
        NF_TEXTURE_BLEND);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_TEXTURE_GROUP_END, &def));

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_START;
    strncpy_s(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "Bump", _TRUNCATE);
    ERR(PF_ADD_PARAM(in_data, NF_BUMP_GROUP, &def));

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Enable Bump", "", FALSE, 0, NF_BUMP_ENABLE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Bump/Normal Map", PF_LayerDefault_NONE, NF_BUMP_MAP);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Intensity", -1.0, 1.0, -1.0, 1.0, 0.20, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_BUMP_INTENSITY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Tile Mode", 4, 2 /*Edge*/,
        "None|Edge|Repeat|Mirror", NF_BUMP_TILE_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Filter", 2, 1 /*Bilinear*/,
        "Bilinear|Nearest", NF_BUMP_FILTER);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Offset", 0, 0, 0, NF_BUMP_OFFSET);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Scale", 0.0, 10.0, 0.0, 10.0, 1.0, 3,
        PF_ValueDisplayFlag_NONE, 0, NF_BUMP_SCALE);

    AEFX_CLR_STRUCT(def);
    def.param_type = PF_Param_GROUP_END;
    ERR(PF_ADD_PARAM(in_data, NF_BUMP_GROUP_END, &def));

    out_data->num_params = NF_NUM_PARAMS;
    return err;
}

// ---- Light collection ----

static PF_Err
CollectLights(PF_InData *in_data, LightInfo lights[], A_long *out_count,
              A_long render_mode, A_Boolean enable_invisible)
{
    PF_Err err = PF_Err_NONE;
    *out_count = 0;

    if (render_mode == RENDER_NONE) return PF_Err_NONE;

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    AEGP_LayerH effect_layerH = nullptr;
    ERR(suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
        in_data->effect_ref, &effect_layerH));
    if (err || !effect_layerH) return err;

    AEGP_CompH compH = nullptr;
    ERR(suites.LayerSuite9()->AEGP_GetLayerParentComp(effect_layerH, &compH));
    if (err || !compH) return err;

    A_Time comp_time = { 0, 1 };
    ERR(suites.PFInterfaceSuite1()->AEGP_ConvertEffectToCompTime(
        in_data->effect_ref,
        in_data->current_time,
        in_data->time_scale,
        &comp_time));
    if (err) return err;

    A_long num_layers = 0;
    ERR(suites.LayerSuite9()->AEGP_GetCompNumLayers(compH, &num_layers));
    if (err) return err;

    A_long light_index = 0;  // sequential index among light layers only
    for (A_long i = 0; i < num_layers && *out_count < MAX_LIGHTS; ++i) {
        AEGP_LayerH layerH = nullptr;
        ERR(suites.LayerSuite9()->AEGP_GetCompLayerByIndex(compH, i, &layerH));
        if (err || !layerH) { err = PF_Err_NONE; continue; }

        AEGP_ObjectType obj_type = AEGP_ObjectType_NONE;
        ERR(suites.LayerSuite9()->AEGP_GetLayerObjectType(layerH, &obj_type));
        if (err || obj_type != AEGP_ObjectType_LIGHT) { err = PF_Err_NONE; continue; }

        light_index++;

        A_Boolean is_active = FALSE;
        suites.LayerSuite9()->AEGP_IsLayerVideoReallyOn(layerH, &is_active);
        if (!enable_invisible && !is_active) continue;

        // Render filter: Light 1-5 → match by layer name ("Light 1" .. "Light 5")
        if (render_mode >= RENDER_LIGHT1 && render_mode <= RENDER_LIGHT5) {
            int target_num = render_mode - RENDER_LIGHT1 + 1;
            static const char *light_names[] = {
                "Light 1", "Light 2", "Light 3", "Light 4", "Light 5"
            };
            const char *expected = light_names[target_num - 1];

            AEGP_MemHandle nameH = nullptr;
            AEGP_MemHandle srcH  = nullptr;
            suites.LayerSuite9()->AEGP_GetLayerName(0, layerH, &nameH, &srcH);
            if (srcH) suites.MemorySuite1()->AEGP_FreeMemHandle(srcH);
            bool name_match = false;
            if (nameH) {
                void *nameP = nullptr;
                suites.MemorySuite1()->AEGP_LockMemHandle(nameH, &nameP);
                if (nameP) {
                    A_UTF16Char *utf16 = (A_UTF16Char*)nameP;
                    int k = 0;
                    while (expected[k] && utf16[k] == (A_UTF16Char)(unsigned char)expected[k]) k++;
                    name_match = (expected[k] == '\0' && utf16[k] == 0);
                }
                suites.MemorySuite1()->AEGP_UnlockMemHandle(nameH);
                suites.MemorySuite1()->AEGP_FreeMemHandle(nameH);
            }
            err = PF_Err_NONE;
            if (!name_match) continue;
        }

        LightInfo &li = lights[*out_count];
        li.enabled = TRUE;

        AEGP_StreamVal2 stream_val;
        AEGP_StreamType stream_type;

        err = suites.StreamSuite6()->AEGP_GetLayerStreamValue(
            layerH, AEGP_LayerStream_POSITION, AEGP_LTimeMode_CompTime,
            &comp_time, FALSE, &stream_val, &stream_type);
        if (!err) {
            li.x = stream_val.three_d.x;
            li.y = stream_val.three_d.y;
            li.z = stream_val.three_d.z;
        }
        err = PF_Err_NONE;

        err = suites.StreamSuite6()->AEGP_GetLayerStreamValue(
            layerH, AEGP_LayerStream_COLOR, AEGP_LTimeMode_CompTime,
            &comp_time, FALSE, &stream_val, &stream_type);
        if (!err) {
            li.r = stream_val.color.redF;
            li.g = stream_val.color.greenF;
            li.b = stream_val.color.blueF;
        }
        err = PF_Err_NONE;

        err = suites.StreamSuite6()->AEGP_GetLayerStreamValue(
            layerH, AEGP_LayerStream_INTENSITY, AEGP_LTimeMode_CompTime,
            &comp_time, FALSE, &stream_val, &stream_type);
        if (!err) {
            li.intensity = stream_val.one_d;
        }
        err = PF_Err_NONE;

        (*out_count)++;
    }

    return PF_Err_NONE;
}

// ---- Blend helper: applies one channel of one of 30 blend modes ----
// HSL-based modes (Hue=26, Saturation=27, Color=28, Luminosity=29) fall back to Add.
static float BlendChannel(A_long mode, float strength, float src, float dst)
{
    float u = 1.0f - strength;
    switch (mode) {
        case 1: // Normal
            return strength * src + u * dst;
        case 2: // Average
            return (dst + src) * 0.5f * strength + dst * u;
        case 3: // Darken
            return ((dst < src) ? dst : src) * strength + dst * u;
        case 4: // Multiply
            return dst * src * strength + dst * u;
        case 5: // Color Burn
            if (src == 0.0f) return dst;
            return (1.0f - (1.0f - dst) / src) * strength + dst * u;
        case 6: // Inverse Color Burn
            if (dst == 0.0f) return dst;
            return (1.0f - (1.0f - src) / dst) * strength + dst * u;
        case 7: // Subtract
            return ((dst + src) - 1.0f) * strength + dst * u;
        case 8: // Darker (per-channel min, then lerp)
            return (src < dst) ? (u * dst + src * strength) : dst;
        case 9: // Lighten
            return ((dst > src) ? dst : src) * strength + dst * u;
        case 10: // Add
            return dst + src * strength;
        case 11: // Screen
            return (1.0f - (1.0f - src) * (1.0f - dst)) * strength + dst * u;
        case 12: // Color Dodge
            return (dst / std::max(1.0f - src, 1e-6f)) * strength + dst * u;
        case 13: // Inverse Color Dodge
            return (src / std::max(1.0f - dst, 1e-6f)) * strength + dst * u;
        case 14: // Lighter (per-channel max, then lerp)
            return (src > dst) ? (u * dst + src * strength) : dst;
        case 15: // Divide
            if (src == 0.0f) return 1.0f;
            return u * dst + (dst * strength) / src;
        case 16: { // Overlay
            float r = (dst >= 0.5f)
                ? (1.0f - (1.0f - dst) * 2.0f * (1.0f - src))
                : (dst * src);
            return r * strength + dst * u;
        }
        case 17: { // Soft Light
            float r;
            if (src >= 0.5f) {
                r = sqrtf(std::max(dst, 0.0f)) * (src * 2.0f - 1.0f) + dst * 2.0f * (1.0f - src);
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
                                    : ((dst * dst) / std::max(1.0f - src, 1e-6f) * strength + dst * u);
            return r;
        }
        case 20: { // Glow
            float r = (dst == 1.0f) ? (u * dst + strength)
                                    : ((src * src) / std::max(1.0f - dst, 1e-6f) * strength + dst * u);
            return r;
        }
        case 21: // Difference
            return fabsf(dst - src) * strength + dst * u;
        case 22: // Exclusion
            return ((src + dst) - dst * 2.0f * src) * strength + dst * u;
        case 23: // Grain Merge
            return ((dst + src) - 0.5f) * strength + dst * u;
        case 24: // Exponential (A^B): dst ^ (1/src)
            return powf(std::max(dst, 0.0f), 1.0f / std::max(src, 1e-6f)) * strength + dst * u;
        case 25: // Exponential (B^A): src ^ (1/dst)
            return powf(std::max(src, 0.0f), 1.0f / std::max(dst, 1e-6f)) * strength + dst * u;
        case 26: case 27: case 28: case 29:
            // Hue/Saturation/Color/Luminosity: HSL conversion not yet implemented, fallback to Add
            return dst + src * strength;
        case 30: { // Phoenix
            float lo = std::min(dst, src);
            float hi = std::max(dst, src);
            return ((lo - hi) + 1.0f) * strength + dst * u;
        }
        default:
            return dst + src * strength;
    }
}

static void ApplyBlend(A_long mode, float strength,
                       float src_r, float src_g, float src_b,
                       float &dst_r, float &dst_g, float &dst_b)
{
    dst_r = BlendChannel(mode, strength, src_r, dst_r);
    dst_g = BlendChannel(mode, strength, src_g, dst_g);
    dst_b = BlendChannel(mode, strength, src_b, dst_b);
}

// ---- Normal decode helpers (8-bit path) ----

static A_u_char FloatToU8(float v)
{
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<A_u_char>(v * 255.0f + 0.5f);
}

static A_u_short FloatToU16(float v)
{
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<A_u_short>(v * 32768.0f + 0.5f);
}

// ---- Pixel accessors (overloaded for 8/16/32 bpc, 14b-β) ----

static inline float ReadR(const PF_Pixel8 &p) { return p.red   / 255.0f; }
static inline float ReadG(const PF_Pixel8 &p) { return p.green / 255.0f; }
static inline float ReadB(const PF_Pixel8 &p) { return p.blue  / 255.0f; }
static inline float ReadA(const PF_Pixel8 &p) { return p.alpha / 255.0f; }

static inline void Write(PF_Pixel8 &p, float r, float g, float b, float a = 1.0f)
{
    p.red   = FloatToU8(r);
    p.green = FloatToU8(g);
    p.blue  = FloatToU8(b);
    p.alpha = FloatToU8(a);
}

static inline float ReadR(const PF_Pixel16 &p) { return p.red   / 32768.0f; }
static inline float ReadG(const PF_Pixel16 &p) { return p.green / 32768.0f; }
static inline float ReadB(const PF_Pixel16 &p) { return p.blue  / 32768.0f; }
static inline float ReadA(const PF_Pixel16 &p) { return p.alpha / 32768.0f; }

static inline void Write(PF_Pixel16 &p, float r, float g, float b, float a = 1.0f)
{
    p.red   = FloatToU16(r);
    p.green = FloatToU16(g);
    p.blue  = FloatToU16(b);
    p.alpha = FloatToU16(a);
}

// 32 bpc passes linear values via PF_OutFlag2_FLOAT_COLOR_AWARE. NormForge keeps
// it linear (no gamma correction) and lets AE handle color management on display.
// Working color space (sRGB / Rec.709 / ACES / etc.) is project-dependent, so
// hard-coding any gamma here would break under non-sRGB working spaces.
// Future tuning may revisit linearization strategies for non-sRGB working spaces.
static inline float ReadR(const PF_PixelFloat &p) { return p.red;   }
static inline float ReadG(const PF_PixelFloat &p) { return p.green; }
static inline float ReadB(const PF_PixelFloat &p) { return p.blue;  }
static inline float ReadA(const PF_PixelFloat &p) { return p.alpha; }

static inline void Write(PF_PixelFloat &p, float r, float g, float b, float a = 1.0f)
{
    p.red   = r;
    p.green = g;
    p.blue  = b;
    p.alpha = a;
}

static inline void SetAlpha(PF_Pixel8     &p, float a) { p.alpha = FloatToU8(a);  }
static inline void SetAlpha(PF_Pixel16    &p, float a) { p.alpha = FloatToU16(a); }
static inline void SetAlpha(PF_PixelFloat &p, float a) { p.alpha = a;             }

template <typename Pixel>
static inline float GetChannel(const Pixel &pix, A_long sel)
{
    switch (sel) {
        case CHAN_R:    return ReadR(pix);
        case CHAN_G:    return ReadG(pix);
        case CHAN_B:    return ReadB(pix);
        case CHAN_ONE:  return 1.0f;
        case CHAN_ZERO: return 0.0f;
        default:        return 0.0f;
    }
}

// Backward-compatible wrapper (kept until Step 14b removes the last 8-bit caller).
static inline float GetChannel8(const PF_Pixel8 &pix, A_long sel)
{
    return GetChannel<PF_Pixel8>(pix, sel);
}

static float DecodeNormalComp(float raw, A_Boolean invert)
{
    if (invert) raw = 1.0f - raw;
    return raw * 2.0f - 1.0f;
}

// ---- Pre Blur helpers: separable 3-pass box blur (Gaussian approximation) ----
// Running sum (sliding window) gives O(1) cost per pixel regardless of radius.
// Three passes approximate a Gaussian within ~3% by the central limit theorem.

static void BoxBlur1D_H(const std::vector<float> &src,
                        std::vector<float> &dst,
                        A_long w, A_long h, A_long radius)
{
    if (radius <= 0) {
        dst = src;
        return;
    }
    for (A_long y = 0; y < h; ++y) {
        const float *sp = &src[y * w];
        float *dp = &dst[y * w];
        float sum = 0.0f;
        A_long cnt = 0;
        A_long lim = std::min(radius, w - 1);
        for (A_long x = 0; x <= lim; ++x) {
            sum += sp[x];
            ++cnt;
        }
        for (A_long x = 0; x < w; ++x) {
            dp[x] = (cnt > 0) ? sum / float(cnt) : 0.0f;
            A_long add = x + radius + 1;
            A_long rem = x - radius;
            if (add < w) {
                sum += sp[add];
                ++cnt;
            }
            if (rem >= 0) {
                sum -= sp[rem];
                --cnt;
            }
        }
    }
}

static void BoxBlur1D_V(const std::vector<float> &src,
                        std::vector<float> &dst,
                        A_long w, A_long h, A_long radius)
{
    if (radius <= 0) {
        dst = src;
        return;
    }
    for (A_long x = 0; x < w; ++x) {
        float sum = 0.0f;
        A_long cnt = 0;
        A_long lim = std::min(radius, h - 1);
        for (A_long y = 0; y <= lim; ++y) {
            sum += src[y * w + x];
            ++cnt;
        }
        for (A_long y = 0; y < h; ++y) {
            dst[y * w + x] = (cnt > 0) ? sum / float(cnt) : 0.0f;
            A_long add = y + radius + 1;
            A_long rem = y - radius;
            if (add < h) {
                sum += src[add * w + x];
                ++cnt;
            }
            if (rem >= 0) {
                sum -= src[rem * w + x];
                --cnt;
            }
        }
    }
}

// 3-pass Box Blur ≒ Gaussian. buf is blurred in-place via a tmp buffer.
static void BoxBlur3Pass(std::vector<float> &buf,
                         std::vector<float> &tmp,
                         A_long w, A_long h, A_long radius)
{
    if (radius <= 0) return;
    for (int i = 0; i < 3; ++i) {
        BoxBlur1D_H(buf, tmp, w, h, radius);
        BoxBlur1D_V(tmp, buf, w, h, radius);
    }
}

// Build pre-blurred normal-map buffers from a normal-map layer.
// Output buffers hold decoded normals in [-1,1] (pre-renormalize) and alpha in [0,1].
template <typename Pixel>
static void PreBlurNormalMapT(
    const PF_LayerDef *nm,
    A_long radius,
    A_long chan_x, A_long chan_y, A_long chan_z,
    A_Boolean inv_x, A_Boolean inv_y, A_Boolean inv_z,
    bool include_alpha,
    std::vector<float> &nx_buf,
    std::vector<float> &ny_buf,
    std::vector<float> &nz_buf,
    std::vector<float> &a_buf)
{
    A_long w = nm->width;
    A_long h = nm->height;
    A_long n = w * h;
    nx_buf.assign(n, 0.0f);
    ny_buf.assign(n, 0.0f);
    nz_buf.assign(n, 0.0f);
    if (include_alpha) a_buf.assign(n, 0.0f);

    for (A_long y = 0; y < h; ++y) {
        const Pixel *row = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(nm->data) + y * nm->rowbytes);
        for (A_long x = 0; x < w; ++x) {
            const Pixel &p = row[x];
            float rx = GetChannel(p, chan_x);
            float ry = GetChannel(p, chan_y);
            float rz = GetChannel(p, chan_z);
            if (inv_x) rx = 1.0f - rx;
            if (inv_y) ry = 1.0f - ry;
            if (inv_z) rz = 1.0f - rz;
            A_long idx = y * w + x;
            nx_buf[idx] = rx * 2.0f - 1.0f;
            ny_buf[idx] = ry * 2.0f - 1.0f;
            nz_buf[idx] = rz * 2.0f - 1.0f;
            if (include_alpha) a_buf[idx] = ReadA(p);
        }
    }

    std::vector<float> tmp(n);
    BoxBlur3Pass(nx_buf, tmp, w, h, radius);
    BoxBlur3Pass(ny_buf, tmp, w, h, radius);
    BoxBlur3Pass(nz_buf, tmp, w, h, radius);
    if (include_alpha) BoxBlur3Pass(a_buf, tmp, w, h, radius);
}

// Forward declaration: defined later (alongside Bilinear sampling helpers).
static inline A_long TileWrap(A_long mode, A_long max_idx, A_long v);

// ---- Generic layer blur (separable 3-pass running sum) ----
// Decodes the source layer to RGB float buffers and applies a 3-pass box blur
// per channel (Gaussian approximation). per-pixel cost is O(1) regardless of radius.
// Edge handling is clamp; Tile Mode is applied at sampling time.
template <typename Pixel>
static void BlurLayerRGBT(
    const PF_LayerDef *src,
    A_long radius,
    A_long &out_w, A_long &out_h,
    std::vector<float> &r_buf,
    std::vector<float> &g_buf,
    std::vector<float> &b_buf)
{
    out_w = src->width;
    out_h = src->height;
    A_long n = out_w * out_h;
    r_buf.assign(n, 0.0f);
    g_buf.assign(n, 0.0f);
    b_buf.assign(n, 0.0f);

    for (A_long y = 0; y < out_h; ++y) {
        const Pixel *row = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(src->data) + y * src->rowbytes);
        for (A_long x = 0; x < out_w; ++x) {
            A_long idx = y * out_w + x;
            r_buf[idx] = ReadR(row[x]);
            g_buf[idx] = ReadG(row[x]);
            b_buf[idx] = ReadB(row[x]);
        }
    }

    if (radius > 0) {
        std::vector<float> tmp(n);
        BoxBlur3Pass(r_buf, tmp, out_w, out_h, radius);
        BoxBlur3Pass(g_buf, tmp, out_w, out_h, radius);
        BoxBlur3Pass(b_buf, tmp, out_w, out_h, radius);
    }
}

// Bilinear sample from RGB float buffers using TileWrap mode.
// Returns (0,0,0) when sample falls outside under "None" tile mode.
static inline void SampleBlurredRGB(
    A_long tile_mode, A_long w, A_long h,
    const std::vector<float> &r_buf,
    const std::vector<float> &g_buf,
    const std::vector<float> &b_buf,
    float u_f, float v_f,
    float &out_r, float &out_g, float &out_b)
{
    A_long u0 = (A_long)floorf(u_f);
    A_long v0 = (A_long)floorf(v_f);
    float fu = u_f - float(u0);
    float fv = v_f - float(v0);
    A_long u0w = TileWrap(tile_mode, w - 1, u0);
    A_long v0w = TileWrap(tile_mode, h - 1, v0);
    A_long u1w = TileWrap(tile_mode, w - 1, u0 + 1);
    A_long v1w = TileWrap(tile_mode, h - 1, v0 + 1);
    if (u0w < 0 || v0w < 0 || u1w < 0 || v1w < 0) {
        out_r = out_g = out_b = 0.0f;
        return;
    }
    float ufm = 1.0f - fu;
    float vfm = 1.0f - fv;
    A_long i00 = v0w * w + u0w;
    A_long i10 = v0w * w + u1w;
    A_long i01 = v1w * w + u0w;
    A_long i11 = v1w * w + u1w;
    out_r = (ufm * r_buf[i00] + fu * r_buf[i10]) * vfm + (ufm * r_buf[i01] + fu * r_buf[i11]) * fv;
    out_g = (ufm * g_buf[i00] + fu * g_buf[i10]) * vfm + (ufm * g_buf[i01] + fu * g_buf[i11]) * fv;
    out_b = (ufm * b_buf[i00] + fu * b_buf[i10]) * vfm + (ufm * b_buf[i01] + fu * b_buf[i11]) * fv;
}

// ---- Depth preprocessing ----
// Build a per-output-pixel depth buffer from the Depth Map layer.
// Strength + Invert are applied during preprocessing so the per-pixel render loop
// reads a single float (= AE-space pixel Z, in pixels) with O(1) cost.
// Luminance uses BT.709 (0.2126*R + 0.7152*G + 0.0722*B).
template <typename Pixel>
static void PreprocessDepthMapT(
    const PF_LayerDef *dm,
    A_long out_w, A_long out_h,
    A_Boolean invert,
    float    strength,
    std::vector<float> &depth_buf)
{
    A_long n = out_w * out_h;
    depth_buf.assign(n, 0.0f);
    if (!dm || !dm->data || dm->width <= 0 || dm->height <= 0 || strength == 0.0f) return;

    A_long dw = dm->width;
    A_long dh = dm->height;
    float fw = float(dw);
    float fh = float(dh);

    for (A_long y = 0; y < out_h; ++y) {
        // Bilinear sample mapping output (col, row) into Depth Map space.
        float v_f = (out_h > 1) ? (float(y) * (fh - 1.0f) / float(out_h - 1)) : 0.0f;
        A_long v0 = (A_long)floorf(v_f);
        A_long v1 = std::min(dh - 1, v0 + 1);
        float fv = v_f - float(v0);
        if (v0 < 0) { v0 = 0; fv = 0.0f; }

        const Pixel *r0 = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(dm->data) + v0 * dm->rowbytes);
        const Pixel *r1 = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(dm->data) + v1 * dm->rowbytes);

        for (A_long x = 0; x < out_w; ++x) {
            float u_f = (out_w > 1) ? (float(x) * (fw - 1.0f) / float(out_w - 1)) : 0.0f;
            A_long u0 = (A_long)floorf(u_f);
            A_long u1 = std::min(dw - 1, u0 + 1);
            float fu = u_f - float(u0);
            if (u0 < 0) { u0 = 0; fu = 0.0f; }

            auto lum = [](float r, float g, float b) {
                return 0.2126f * r + 0.7152f * g + 0.0722f * b;
            };
            float l00 = lum(ReadR(r0[u0]), ReadG(r0[u0]), ReadB(r0[u0]));
            float l10 = lum(ReadR(r0[u1]), ReadG(r0[u1]), ReadB(r0[u1]));
            float l01 = lum(ReadR(r1[u0]), ReadG(r1[u0]), ReadB(r1[u0]));
            float l11 = lum(ReadR(r1[u1]), ReadG(r1[u1]), ReadB(r1[u1]));
            float a = l00 * (1.0f - fu) + l10 * fu;
            float b = l01 * (1.0f - fu) + l11 * fu;
            float d = a * (1.0f - fv) + b * fv;
            if (invert) d = 1.0f - d;
            depth_buf[y * out_w + x] = d * strength;
        }
    }
}

// ---- Refcon for parallel RenderNormalsT (Step 14b-gamma / 0.5.11) ----
// Shared, read-only context handed to NormalsRowFnT via iterate_generic.

template <typename Pixel>
struct NormalsRefconT {
    PF_InData         *in_data;
    PF_LayerDef       *src;
    PF_LayerDef       *output;
    A_long             chan_x, chan_y, chan_z;
    A_Boolean          inv_x, inv_y, inv_z;
    float              normal_strength;
    A_long             display;
    const std::vector<float> *blur_nx;
    const std::vector<float> *blur_ny;
    const std::vector<float> *blur_nz;
    const std::vector<float> *depth_buf;
    float              depth_strength;

    // Per-row invariant cached values
    A_long             out_w, out_h;
    A_long             src_w, src_h;
    bool               have_blur;
    float              depth_norm_inv;
};

// Per-row function called by iterate_generic for the Normals preview
// (DISPLAY_ADJUSTED_NORMALS / DISPLAY_ORIGINAL_NORMALS / DISPLAY_DEPTH).
template <typename Pixel>
static PF_Err
NormalsRowFnT(void *refconPV, A_long /*thread_index*/, A_long i, A_long /*iterationsL*/)
{
    const NormalsRefconT<Pixel> *ctx =
        reinterpret_cast<const NormalsRefconT<Pixel> *>(refconPV);

    PF_LayerDef *src    = ctx->src;
    PF_LayerDef *output = ctx->output;
    A_long  chan_x = ctx->chan_x, chan_y = ctx->chan_y, chan_z = ctx->chan_z;
    A_Boolean inv_x = ctx->inv_x, inv_y = ctx->inv_y, inv_z = ctx->inv_z;
    float  normal_strength = ctx->normal_strength;
    A_long display = ctx->display;
    const std::vector<float> *blur_nx = ctx->blur_nx;
    const std::vector<float> *blur_ny = ctx->blur_ny;
    const std::vector<float> *blur_nz = ctx->blur_nz;
    const std::vector<float> *depth_buf = ctx->depth_buf;

    A_long out_w = ctx->out_w;
    A_long src_w = ctx->src_w;
    A_long src_h = ctx->src_h;
    bool   have_blur = ctx->have_blur;
    float  depth_norm_inv = ctx->depth_norm_inv;

    A_long y = i;
    Pixel *out_row = reinterpret_cast<Pixel *>(
        reinterpret_cast<char *>(output->data) + y * output->rowbytes);

    A_long ny = std::min(y, src_h - 1);
    const Pixel *src_row = reinterpret_cast<const Pixel *>(
        reinterpret_cast<const char *>(src->data) + ny * src->rowbytes);

    for (A_long x = 0; x < out_w; ++x) {
            A_long nx = std::min(x, src_w - 1);
            Pixel &dst = out_row[x];

            if (display == DISPLAY_DEPTH) {
                float dv = 0.0f;
                if (depth_buf) {
                    dv = (*depth_buf)[y * out_w + x] * depth_norm_inv;
                    if (dv < 0.0f) dv = 0.0f;
                    if (dv > 1.0f) dv = 1.0f;
                }
                Write(dst, dv, dv, dv);
                continue;
            }

            if (have_blur) {
                A_long idx = ny * src_w + nx;
                float bnx = (*blur_nx)[idx];
                float bny = (*blur_ny)[idx];
                float bnz = (*blur_nz)[idx];
                if (display == DISPLAY_ORIGINAL_NORMALS) {
                    // Re-encode pre-renormalize blurred normals back to [0,1].
                    Write(dst,
                          bnx * 0.5f + 0.5f,
                          bny * 0.5f + 0.5f,
                          bnz * 0.5f + 0.5f);
                } else {
                    // Adjusted: renormalize, apply Normal Strength, re-encode.
                    float nl = sqrtf(bnx*bnx + bny*bny + bnz*bnz);
                    if (nl > 1e-5f) { bnx /= nl; bny /= nl; bnz /= nl; }
                    if (fabsf(normal_strength - 1.0f) > 1e-5f) {
                        bnx *= normal_strength;
                        bny *= normal_strength;
                        float ns_l = sqrtf(bnx*bnx + bny*bny + bnz*bnz);
                        if (ns_l > 1e-5f) { bnx /= ns_l; bny /= ns_l; bnz /= ns_l; }
                    }
                    Write(dst,
                          bnx * 0.5f + 0.5f,
                          bny * 0.5f + 0.5f,
                          bnz * 0.5f + 0.5f);
                }
            } else {
                const Pixel &s = src_row[nx];
                float raw_x = GetChannel(s, chan_x);
                float raw_y = GetChannel(s, chan_y);
                float raw_z = GetChannel(s, chan_z);
                if (display == DISPLAY_ORIGINAL_NORMALS) {
                    Write(dst, raw_x, raw_y, raw_z);
                } else {
                    float nx_v = DecodeNormalComp(raw_x, inv_x);
                    float ny_v = DecodeNormalComp(raw_y, inv_y);
                    float nz_v = DecodeNormalComp(raw_z, inv_z);
                    float nl_v = sqrtf(nx_v*nx_v + ny_v*ny_v + nz_v*nz_v);
                    if (nl_v > 1e-5f) { nx_v /= nl_v; ny_v /= nl_v; nz_v /= nl_v; }
                    if (fabsf(normal_strength - 1.0f) > 1e-5f) {
                        nx_v *= normal_strength;
                        ny_v *= normal_strength;
                        float ns_l = sqrtf(nx_v*nx_v + ny_v*ny_v + nz_v*nz_v);
                        if (ns_l > 1e-5f) { nx_v /= ns_l; ny_v /= ns_l; nz_v /= ns_l; }
                    }
                    Write(dst,
                          nx_v * 0.5f + 0.5f,
                          ny_v * 0.5f + 0.5f,
                          nz_v * 0.5f + 0.5f);
                }
            }
        }

    return PF_Err_NONE;
}

// RenderNormalsT: builds NormalsRefconT and dispatches NormalsRowFnT across
// rows via iterate_generic (Step 14b-gamma / 0.5.11).
template <typename Pixel>
static PF_Err
RenderNormalsT(
    PF_InData   *in_data,
    PF_LayerDef *src,
    PF_LayerDef *output,
    A_long       chan_x,
    A_long       chan_y,
    A_long       chan_z,
    A_Boolean    inv_x,
    A_Boolean    inv_y,
    A_Boolean    inv_z,
    float        normal_strength,
    A_long       display,
    const std::vector<float> *blur_nx,
    const std::vector<float> *blur_ny,
    const std::vector<float> *blur_nz,
    const std::vector<float> *depth_buf,
    float        depth_strength)
{
    NormalsRefconT<Pixel> ctx;
    ctx.in_data = in_data;
    ctx.src     = src;
    ctx.output  = output;
    ctx.chan_x = chan_x; ctx.chan_y = chan_y; ctx.chan_z = chan_z;
    ctx.inv_x = inv_x; ctx.inv_y = inv_y; ctx.inv_z = inv_z;
    ctx.normal_strength = normal_strength;
    ctx.display = display;
    ctx.blur_nx = blur_nx;
    ctx.blur_ny = blur_ny;
    ctx.blur_nz = blur_nz;
    ctx.depth_buf = depth_buf;
    ctx.depth_strength = depth_strength;

    ctx.out_w = output->width;
    ctx.out_h = output->height;
    ctx.src_w = src->width;
    ctx.src_h = src->height;
    ctx.have_blur = (blur_nx && blur_ny && blur_nz);
    ctx.depth_norm_inv = (depth_strength > 1e-5f) ? (1.0f / depth_strength) : 0.0f;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    return suites.Iterate8Suite1()->iterate_generic(
        ctx.out_h, &ctx, &NormalsRowFnT<Pixel>);
}

// Tile mode wrap. Returns -1 when out of range under "None" mode.
//   mode: 1=None, 2=Edge, 3=Repeat, 4=Mirror (matches NormForge UI popup)
static inline A_long TileWrap(A_long mode, A_long max_idx, A_long v)
{
    if (v >= 0 && v <= max_idx) return v;
    if (mode == 1) return -1;
    if (mode == 2) {
        if (v < 0) return 0;
        return max_idx;
    }
    A_long w = max_idx + 1;
    if (mode == 3) {
        v = v % w;
        if (v < 0) v += w;
        return v;
    }
    if (mode == 4) {
        A_long w2 = w * 2;
        v = v % w2;
        if (v < 0) v += w2;
        if (v >= w) v = w2 - 1 - v;
        return v;
    }
    if (v < 0) return 0;
    return max_idx;
}

// ---- Refcon for parallel RenderShadingT (Step 14b-γ / 0.5.11) ----
// Shared, read-only context handed to ShadingRowFnT via iterate_generic.
// All pointers reference resources owned by the caller; their lifetime
// covers the entire iterate_generic call.

template <typename Pixel>
struct ShadingRefconT {
    // I/O
    PF_InData         *in_data;
    PF_LayerDef       *input;       // canvas (used for DISPLAY_ALL, may be null)
    const PF_LayerDef *nm;
    PF_LayerDef       *output;
    const PF_LayerDef *ao_ld;
    const PF_LayerDef *spec_ld;
    const PF_LayerDef *matcap_ld;
    const PF_LayerDef *bump_ld;
    const PF_LayerDef *tex_uv_ld;
    const PF_LayerDef *tex_map_ld;
    const PF_LayerDef *env_map_ld;
    const PF_LayerDef *refr_map_ld;

    // Normals
    A_long    chan_x, chan_y, chan_z;
    A_Boolean inv_x, inv_y, inv_z;
    A_Boolean use_normal_alpha;
    float     normal_strength;

    // Pre-blur / depth / matcap-blur / reflection-blur / refraction-blur buffers
    const std::vector<float> *blur_nx;
    const std::vector<float> *blur_ny;
    const std::vector<float> *blur_nz;
    const std::vector<float> *blur_a;
    const std::vector<float> *depth_buf;
    const std::vector<float> *matcap_blur_r;
    const std::vector<float> *matcap_blur_g;
    const std::vector<float> *matcap_blur_b;
    const std::vector<float> *reflection_blur_r;
    const std::vector<float> *reflection_blur_g;
    const std::vector<float> *reflection_blur_b;
    const std::vector<float> *refraction_blur_r;
    const std::vector<float> *refraction_blur_g;
    const std::vector<float> *refraction_blur_b;

    // Lights
    const LightInfo *lights;
    A_long           light_count;

    // Display / Shading
    A_long    display;
    A_Boolean enable_shading;
    float     diffuse_amount;
    PF_Pixel8 diffuse_color, ambient_color, incandescence_color;
    A_long    shading_blend;
    float     shading_blend_amount;

    // Specular
    A_Boolean enable_specular;
    float     specular_amount, specular_spread;
    PF_Pixel8 specular_color;
    A_long    specular_blend;

    // Lights global
    A_long    falloff_mode;
    float     falloff_val, exposure, gamma;

    // Incidence
    A_Boolean enable_incidence;
    float     incidence_amount, incidence_falloff;
    PF_Pixel8 incidence_color;
    A_long    incidence_blend;

    // Rim
    A_Boolean enable_rim;
    float     rim_amount, rim_size, rim_width;
    PF_Pixel8 rim_color;
    float     rim_angle_deg;
    A_long    rim_blend;

    // Gradient
    A_Boolean enable_gradient;
    float     gradient_amount;
    PF_Pixel8 gradient_color;
    A_long    gradient_blend;

    // Toon
    A_Boolean enable_toon;
    float     toon_amount, toon_smoothing;
    PF_Pixel8 toon_shadow_color;
    float     toon_shadow_intensity, toon_shadow_width;
    PF_Pixel8 toon_mid_color;
    float     toon_highlight_width;
    PF_Pixel8 toon_highlight_color;
    float     toon_highlight_intensity;
    A_long    toon_blend;

    // Matcap
    A_Boolean enable_matcap;
    float     matcap_amount;
    A_long    matcap_offset_x, matcap_offset_y;
    A_long    matcap_mode, matcap_blur, matcap_blend;

    // Bump
    A_Boolean enable_bump;
    float     bump_intensity;
    A_long    bump_tile, bump_filter;
    A_long    bump_offset_x, bump_offset_y;
    float     bump_scale;

    // Texture
    A_Boolean enable_texture;
    float     texture_amount;
    A_long    texture_tile, texture_uv_mirror, texture_filter;
    A_long    texture_offset_x, texture_offset_y;
    A_long    texture_blend;

    // Reflection
    A_Boolean enable_reflection;
    float     reflection_amount;
    A_long    reflection_env_mode;
    float     reflection_gamma;
    A_long    reflection_tile, reflection_blur;
    PF_Pixel8 reflection_color;
    float     reflection_inclination_deg, reflection_azimuth_deg;
    float     reflection_scale, reflection_ior;
    float     reflection_fresnel, reflection_fresnel_depth;
    A_long    reflection_blend;

    // Refraction
    A_Boolean enable_refraction;
    float     refraction_amount, refraction_ior, refraction_strength;
    A_Boolean refraction_use_depth;
    float     refraction_gamma;
    A_long    refraction_tile, refraction_blur;
    A_long    refraction_offset_x, refraction_offset_y;
    float     refraction_scale;
    PF_Pixel8 refraction_color;
    float     refraction_fresnel, refraction_fresnel_depth;
    A_long    refraction_blend;

    // Dimensions / per-render derived values (computed once before iterate)
    double sx, sy;
    A_long out_w, out_h;
    A_long nm_w, nm_h;
    bool   have_blur;

    float ev_global;
    float shininess;
    float diff_r, diff_g, diff_b;
    float amb_r, amb_g, amb_b;
    float ainc_r, ainc_g, ainc_b;
    float spec_r, spec_g, spec_b;
};

// ---- Shading rendering: Diffuse + Specular (templated by pixel type) ----

// per-row function called by iterate_generic. ctx is read-only shared data;
// every thread reads the same struct without synchronization. We extract all
// fields into locals at the top so the original per-pixel body (~740 lines)
// can be reused verbatim with no rewrites.
template <typename Pixel>
static PF_Err
ShadingRowFnT(void *refconPV, A_long /*thread_index*/, A_long i, A_long /*iterationsL*/)
{
    const ShadingRefconT<Pixel> *ctx =
        reinterpret_cast<const ShadingRefconT<Pixel> *>(refconPV);

    // ---- Extract refcon into locals (same names/types as old RenderShadingT params) ----
    PF_InData         *in_data = ctx->in_data;
    PF_LayerDef       *input   = ctx->input;
    const PF_LayerDef *nm      = ctx->nm;
    PF_LayerDef       *output  = ctx->output;
    const PF_LayerDef *ao_ld   = ctx->ao_ld;
    const PF_LayerDef *spec_ld = ctx->spec_ld;
    const PF_LayerDef *matcap_ld = ctx->matcap_ld;
    const PF_LayerDef *bump_ld   = ctx->bump_ld;
    const PF_LayerDef *tex_uv_ld = ctx->tex_uv_ld;
    const PF_LayerDef *tex_map_ld = ctx->tex_map_ld;
    const PF_LayerDef *env_map_ld = ctx->env_map_ld;
    const PF_LayerDef *refr_map_ld = ctx->refr_map_ld;

    A_long    chan_x = ctx->chan_x, chan_y = ctx->chan_y, chan_z = ctx->chan_z;
    A_Boolean inv_x = ctx->inv_x, inv_y = ctx->inv_y, inv_z = ctx->inv_z;
    A_Boolean use_normal_alpha = ctx->use_normal_alpha;
    float     normal_strength  = ctx->normal_strength;

    const std::vector<float> *blur_nx = ctx->blur_nx;
    const std::vector<float> *blur_ny = ctx->blur_ny;
    const std::vector<float> *blur_nz = ctx->blur_nz;
    const std::vector<float> *blur_a  = ctx->blur_a;
    const std::vector<float> *depth_buf = ctx->depth_buf;
    const std::vector<float> *matcap_blur_r = ctx->matcap_blur_r;
    const std::vector<float> *matcap_blur_g = ctx->matcap_blur_g;
    const std::vector<float> *matcap_blur_b = ctx->matcap_blur_b;
    const std::vector<float> *reflection_blur_r = ctx->reflection_blur_r;
    const std::vector<float> *reflection_blur_g = ctx->reflection_blur_g;
    const std::vector<float> *reflection_blur_b = ctx->reflection_blur_b;
    const std::vector<float> *refraction_blur_r = ctx->refraction_blur_r;
    const std::vector<float> *refraction_blur_g = ctx->refraction_blur_g;
    const std::vector<float> *refraction_blur_b = ctx->refraction_blur_b;

    const LightInfo *lights = ctx->lights;
    A_long           light_count = ctx->light_count;

    A_long    display = ctx->display;
    A_Boolean enable_shading = ctx->enable_shading;
    float     diffuse_amount = ctx->diffuse_amount;
    PF_Pixel8 diffuse_color = ctx->diffuse_color;
    PF_Pixel8 ambient_color = ctx->ambient_color;
    PF_Pixel8 incandescence_color = ctx->incandescence_color;
    A_long    shading_blend = ctx->shading_blend;
    float     shading_blend_amount = ctx->shading_blend_amount;

    A_Boolean enable_specular = ctx->enable_specular;
    float     specular_amount = ctx->specular_amount;
    float     specular_spread = ctx->specular_spread;
    PF_Pixel8 specular_color = ctx->specular_color;
    A_long    specular_blend = ctx->specular_blend;

    A_long    falloff_mode = ctx->falloff_mode;
    float     falloff_val  = ctx->falloff_val;
    float     exposure     = ctx->exposure;
    float     gamma        = ctx->gamma;

    A_Boolean enable_incidence = ctx->enable_incidence;
    float     incidence_amount = ctx->incidence_amount;
    float     incidence_falloff = ctx->incidence_falloff;
    PF_Pixel8 incidence_color = ctx->incidence_color;
    A_long    incidence_blend = ctx->incidence_blend;

    A_Boolean enable_rim = ctx->enable_rim;
    float     rim_amount = ctx->rim_amount;
    float     rim_size   = ctx->rim_size;
    float     rim_width  = ctx->rim_width;
    PF_Pixel8 rim_color  = ctx->rim_color;
    float     rim_angle_deg = ctx->rim_angle_deg;
    A_long    rim_blend = ctx->rim_blend;

    A_Boolean enable_gradient = ctx->enable_gradient;
    float     gradient_amount = ctx->gradient_amount;
    PF_Pixel8 gradient_color  = ctx->gradient_color;
    A_long    gradient_blend  = ctx->gradient_blend;

    A_Boolean enable_toon = ctx->enable_toon;
    float     toon_amount = ctx->toon_amount;
    float     toon_smoothing = ctx->toon_smoothing;
    PF_Pixel8 toon_shadow_color = ctx->toon_shadow_color;
    float     toon_shadow_intensity = ctx->toon_shadow_intensity;
    float     toon_shadow_width = ctx->toon_shadow_width;
    PF_Pixel8 toon_mid_color = ctx->toon_mid_color;
    float     toon_highlight_width = ctx->toon_highlight_width;
    PF_Pixel8 toon_highlight_color = ctx->toon_highlight_color;
    float     toon_highlight_intensity = ctx->toon_highlight_intensity;
    A_long    toon_blend = ctx->toon_blend;

    A_Boolean enable_matcap = ctx->enable_matcap;
    float     matcap_amount = ctx->matcap_amount;
    A_long    matcap_offset_x = ctx->matcap_offset_x;
    A_long    matcap_offset_y = ctx->matcap_offset_y;
    A_long    matcap_mode = ctx->matcap_mode;
    A_long    matcap_blur = ctx->matcap_blur;
    A_long    matcap_blend = ctx->matcap_blend;

    A_Boolean enable_bump = ctx->enable_bump;
    float     bump_intensity = ctx->bump_intensity;
    A_long    bump_tile = ctx->bump_tile;
    A_long    bump_filter = ctx->bump_filter;
    A_long    bump_offset_x = ctx->bump_offset_x;
    A_long    bump_offset_y = ctx->bump_offset_y;
    float     bump_scale = ctx->bump_scale;

    A_Boolean enable_texture = ctx->enable_texture;
    float     texture_amount = ctx->texture_amount;
    A_long    texture_tile = ctx->texture_tile;
    A_long    texture_uv_mirror = ctx->texture_uv_mirror;
    A_long    texture_filter = ctx->texture_filter;
    A_long    texture_offset_x = ctx->texture_offset_x;
    A_long    texture_offset_y = ctx->texture_offset_y;
    A_long    texture_blend = ctx->texture_blend;

    A_Boolean enable_reflection = ctx->enable_reflection;
    float     reflection_amount = ctx->reflection_amount;
    A_long    reflection_env_mode = ctx->reflection_env_mode;
    float     reflection_gamma = ctx->reflection_gamma;
    A_long    reflection_tile = ctx->reflection_tile;
    A_long    reflection_blur = ctx->reflection_blur;
    PF_Pixel8 reflection_color = ctx->reflection_color;
    float     reflection_inclination_deg = ctx->reflection_inclination_deg;
    float     reflection_azimuth_deg = ctx->reflection_azimuth_deg;
    float     reflection_scale = ctx->reflection_scale;
    float     reflection_ior = ctx->reflection_ior;
    float     reflection_fresnel = ctx->reflection_fresnel;
    float     reflection_fresnel_depth = ctx->reflection_fresnel_depth;
    A_long    reflection_blend = ctx->reflection_blend;

    A_Boolean enable_refraction = ctx->enable_refraction;
    float     refraction_amount = ctx->refraction_amount;
    float     refraction_ior = ctx->refraction_ior;
    float     refraction_strength = ctx->refraction_strength;
    A_Boolean refraction_use_depth = ctx->refraction_use_depth;
    float     refraction_gamma = ctx->refraction_gamma;
    A_long    refraction_tile = ctx->refraction_tile;
    A_long    refraction_blur = ctx->refraction_blur;
    A_long    refraction_offset_x = ctx->refraction_offset_x;
    A_long    refraction_offset_y = ctx->refraction_offset_y;
    float     refraction_scale = ctx->refraction_scale;
    PF_Pixel8 refraction_color = ctx->refraction_color;
    float     refraction_fresnel = ctx->refraction_fresnel;
    float     refraction_fresnel_depth = ctx->refraction_fresnel_depth;
    A_long    refraction_blend = ctx->refraction_blend;

    // Derived values (computed once per render, before iterate)
    double sx = ctx->sx;
    (void)sx;  // currently per-pixel uses comp_x via sx; kept for parity
    double sy = ctx->sy;
    A_long out_w = ctx->out_w;
    A_long out_h = ctx->out_h;
    (void)out_h;
    bool   have_blur = ctx->have_blur;
    A_long nm_w = ctx->nm_w;
    A_long nm_h = ctx->nm_h;
    float ev_global = ctx->ev_global;
    float shininess = ctx->shininess;
    float diff_r = ctx->diff_r, diff_g = ctx->diff_g, diff_b = ctx->diff_b;
    float amb_r  = ctx->amb_r,  amb_g  = ctx->amb_g,  amb_b  = ctx->amb_b;
    float ainc_r = ctx->ainc_r, ainc_g = ctx->ainc_g, ainc_b = ctx->ainc_b;
    float spec_r = ctx->spec_r, spec_g = ctx->spec_g, spec_b = ctx->spec_b;

    // ---- Per-row prep + per-pixel body (verbatim copy from old RenderShadingT) ----
    A_long row = i;

    Pixel *out_p = reinterpret_cast<Pixel *>(
        reinterpret_cast<char *>(output->data) + row * output->rowbytes);

    const Pixel *nm_p = nullptr;
    if (nm->data) {
        A_long nm_r = std::min(row, nm->height - 1);
        nm_p = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(nm->data) + nm_r * nm->rowbytes);
    }

    const Pixel *in_p = nullptr;
    if (display == DISPLAY_ALL && input && input->data) {
        A_long in_r = std::min(row, input->height - 1);
        in_p = reinterpret_cast<const Pixel *>(
            reinterpret_cast<const char *>(input->data) + in_r * input->rowbytes);
    }

    // TODO: add layer-to-comp offset (AEGP_LayerStream_POSITION - ANCHORPOINT)
    double comp_y = row * sy;

    for (A_long col = 0; col < out_w; ++col) {
        float Nx = 0.0f, Ny = 0.0f, Nz = 1.0f;
        float Nx_raw = 0.5f, Ny_raw = 0.5f, Nz_raw = 1.0f;
        float N_alpha = 1.0f;
        if (nm_p) {
            if (have_blur) {
                // Pre-blurred buffers hold pre-renormalize [-1,1] normals (and alpha).
                A_long nm_c = std::min(col, nm_w - 1);
                A_long nm_r = std::min(row, nm_h - 1);
                A_long idx = nm_r * nm_w + nm_c;
                float bnx = (*blur_nx)[idx];
                float bny = (*blur_ny)[idx];
                float bnz = (*blur_nz)[idx];
                Nx_raw = bnx * 0.5f + 0.5f;
                Ny_raw = bny * 0.5f + 0.5f;
                Nz_raw = bnz * 0.5f + 0.5f;
                Nx = bnx; Ny = bny; Nz = bnz;
                float nl = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
                if (nl > 1e-5f) { Nx /= nl; Ny /= nl; Nz /= nl; }
                if (use_normal_alpha) {
                    if (blur_a) {
                        N_alpha = (*blur_a)[idx];
                    } else {
                        N_alpha = ReadA(nm_p[nm_c]);
                    }
                }
            } else {
                A_long nm_c = std::min(col, nm->width - 1);
                const Pixel &ns = nm_p[nm_c];
                Nx = DecodeNormalComp(GetChannel(ns, chan_x), inv_x);
                Ny = DecodeNormalComp(GetChannel(ns, chan_y), inv_y);
                Nz = DecodeNormalComp(GetChannel(ns, chan_z), inv_z);
                float nl = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
                if (nl > 1e-5f) { Nx /= nl; Ny /= nl; Nz /= nl; }
                Nx_raw = GetChannel(ns, chan_x);
                if (inv_x) Nx_raw = 1.0f - Nx_raw;
                Ny_raw = GetChannel(ns, chan_y);
                if (inv_y) Ny_raw = 1.0f - Ny_raw;
                Nz_raw = GetChannel(ns, chan_z);
                if (inv_z) Nz_raw = 1.0f - Nz_raw;
                if (use_normal_alpha) N_alpha = ReadA(ns);
            }
        }

        // Normal Strength: scale tangent (X/Y) deviation, keep Nz, renormalize.
        // 1.0 = identity, 0 = flat, >1 = exaggerated bumps.
        // Affects shading-related uses of Nx/Ny/Nz (Diffuse, Specular, Reflection,
        // Refraction, Bump). Raw values used by Rim Light / Matcap are left
        // untouched.
        if (nm_p && fabsf(normal_strength - 1.0f) > 1e-5f) {
            Nx *= normal_strength;
            Ny *= normal_strength;
            float ns_l = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
            if (ns_l > 1e-5f) { Nx /= ns_l; Ny /= ns_l; Nz /= ns_l; }
        }

        if (enable_bump && bump_ld && bump_ld->data &&
            bump_intensity != 0.0f && bump_scale > 1e-5f &&
            bump_ld->width > 0 && bump_ld->height > 0)
        {
            float scale_inv = 1.0f / bump_scale;
            float u_pix = (float(col) - float(bump_offset_x)) * scale_inv;
            float v_pix = (float(row) - float(bump_offset_y)) * scale_inv;

            float br = 0.0f, bg = 0.0f, bb = 0.0f;
            bool sampled = false;

            if (bump_filter == 2 /*Nearest*/) {
                A_long u = TileWrap(bump_tile, bump_ld->width  - 1, (A_long)floorf(u_pix));
                A_long v = TileWrap(bump_tile, bump_ld->height - 1, (A_long)floorf(v_pix));
                if (u >= 0 && v >= 0) {
                    const Pixel *bp_row = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(bump_ld->data) + v * bump_ld->rowbytes);
                    const Pixel &bp = bp_row[u];
                    br = ReadR(bp);
                    bg = ReadG(bp);
                    bb = ReadB(bp);
                    sampled = true;
                }
            } else /*Bilinear*/ {
                A_long u0 = (A_long)floorf(u_pix);
                A_long v0 = (A_long)floorf(v_pix);
                float fu = u_pix - float(u0);
                float fv = v_pix - float(v0);
                A_long u0w = TileWrap(bump_tile, bump_ld->width  - 1, u0);
                A_long v0w = TileWrap(bump_tile, bump_ld->height - 1, v0);
                A_long u1w = TileWrap(bump_tile, bump_ld->width  - 1, u0 + 1);
                A_long v1w = TileWrap(bump_tile, bump_ld->height - 1, v0 + 1);
                if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                    const Pixel *r0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(bump_ld->data) + v0w * bump_ld->rowbytes);
                    const Pixel *r1p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(bump_ld->data) + v1w * bump_ld->rowbytes);
                    const Pixel &p00 = r0p[u0w];
                    const Pixel &p10 = r0p[u1w];
                    const Pixel &p01 = r1p[u0w];
                    const Pixel &p11 = r1p[u1w];
                    float ufm = 1.0f - fu;
                    float vfm = 1.0f - fv;
                    br = vfm * (ufm * ReadR(p00) + fu * ReadR(p10)) + fv * (ufm * ReadR(p01) + fu * ReadR(p11));
                    bg = vfm * (ufm * ReadG(p00) + fu * ReadG(p10)) + fv * (ufm * ReadG(p01) + fu * ReadG(p11));
                    bb = vfm * (ufm * ReadB(p00) + fu * ReadB(p10)) + fv * (ufm * ReadB(p01) + fu * ReadB(p11));
                    sampled = true;
                }
            }

            if (sampled) {
                float r_n, g_n;
                if (br == bg && bb == bg) {
                    r_n = 1.0f;
                    g_n = 1.0f;
                } else {
                    r_n = br * 2.0f - 1.0f;
                    g_n = bg * 2.0f - 1.0f;
                }
                float b_n = (bb * 2.0f - 1.0f) * bump_intensity;
                Nx += r_n * bump_intensity;
                Ny += g_n * bump_intensity;
                Nz += b_n;
                float bn_l = sqrtf(Nx*Nx + Ny*Ny + Nz*Nz);
                if (bn_l > 1e-5f) { Nx /= bn_l; Ny /= bn_l; Nz /= bn_l; }
            }
        }

        double comp_x = col * ctx->sx;

        float dr = 0.0f, dg = 0.0f, db = 0.0f;
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        float toon_factor = 0.0f;

        float pixel_z = depth_buf ? (*depth_buf)[row * out_w + col] : 0.0f;

        for (A_long li_idx = 0; li_idx < light_count; ++li_idx) {
            const LightInfo &li = lights[li_idx];
            if (!li.enabled) continue;
            float lx = float(li.x - comp_x);
            float ly = float(li.y - comp_y);
            // AE: +Z = away from camera, -Z = toward camera.
            // Normal map: +Z = toward camera, so we flip Z when projecting into normal space.
            // With depth: pixel sits at AE Z = pixel_z (positive = away). Vector light - pixel
            // in AE = (li.x-px, li.y-py, li.z-pixel_z). Flip Z for normal-map space:
            //   lz = -(li.z - pixel_z) = pixel_z - li.z = -li.z + pixel_z
            float lz = -float(li.z) + pixel_z;
            float dist = sqrtf(lx*lx + ly*ly + lz*lz);
            if (dist < 0.001f) continue;
            float Lx = lx / dist, Ly = ly / dist, Lz = lz / dist;

            float NdotL = Nx*Lx + Ny*Ly + Nz*Lz;
            if (NdotL < 0.0f) NdotL = 0.0f;
            if (NdotL > toon_factor) toon_factor = NdotL;

            float falloff_coeff = 1.0f;
            if (falloff_mode != FALLOFF_CONSTANT) {
                float eff = (falloff_val > 1e-3f) ? falloff_val : 1e-3f;
                float s = dist * 0.01f / eff + 1e-4f;
                if (falloff_mode == FALLOFF_LINEAR) {
                    falloff_coeff = 1.0f / s;
                } else if (falloff_mode == FALLOFF_QUADRATIC) {
                    falloff_coeff = 1.0f / (s * s);
                } else if (falloff_mode == FALLOFF_CUBIC) {
                    falloff_coeff = 1.0f / (s * s * s);
                }
            }

            float intensity_scale = float(li.intensity * 0.01) * falloff_coeff;

            if (enable_shading) {
                float dc = NdotL * intensity_scale * diffuse_amount;
                dr += dc * float(li.r);
                dg += dc * float(li.g);
                db += dc * float(li.b);
            }

            if (enable_specular) {
                float cx = Ny*Lz - Nz*Ly;
                float cy = Nz*Lx - Nx*Lz;
                float cz = Nx*Ly - Ny*Lx;
                float spec_raw = 1.0f - sqrtf(cx*cx + cy*cy + cz*cz);
                if (spec_raw < 0.0f) spec_raw = 0.0f;
                float spec = powf(spec_raw, shininess);
                float sc = spec * intensity_scale;
                sr += sc * spec_r * float(li.r);
                sg += sc * spec_g * float(li.g);
                sb += sc * spec_b * float(li.b);
            }
        }

        Pixel &dst = out_p[col];

        // AO Map: per-pixel modulator for the Ambient term (luminance of sample).
        float ao_factor = 1.0f;
        if (ao_ld && ao_ld->data && ao_ld->width > 0 && ao_ld->height > 0) {
            A_long ao_c = std::min(col, ao_ld->width  - 1);
            A_long ao_r = std::min(row, ao_ld->height - 1);
            const Pixel *ao_row_p = reinterpret_cast<const Pixel *>(
                reinterpret_cast<const char *>(ao_ld->data) + ao_r * ao_ld->rowbytes);
            const Pixel &ap = ao_row_p[ao_c];
            ao_factor = ReadR(ap) * 0.21267f + ReadG(ap) * 0.71516f + ReadB(ap) * 0.072169f;
            if (ao_factor < 0.0f) ao_factor = 0.0f;
        }

        float fr = 0.0f, fg = 0.0f, fb = 0.0f;
        if (enable_shading) {
            fr = dr * diff_r * amb_r * ao_factor * 2.0f + ainc_r;
            fg = dg * diff_g * amb_g * ao_factor * 2.0f + ainc_g;
            fb = db * diff_b * amb_b * ao_factor * 2.0f + ainc_b;
        }

        // Specular Map: per-pixel modulator for the Specular accumulator (luminance).
        if (spec_ld && spec_ld->data && spec_ld->width > 0 && spec_ld->height > 0) {
            A_long sm_c = std::min(col, spec_ld->width  - 1);
            A_long sm_r = std::min(row, spec_ld->height - 1);
            const Pixel *sm_row_p = reinterpret_cast<const Pixel *>(
                reinterpret_cast<const char *>(spec_ld->data) + sm_r * spec_ld->rowbytes);
            const Pixel &sp = sm_row_p[sm_c];
            float spec_mask = ReadR(sp) * 0.21267f + ReadG(sp) * 0.71516f + ReadB(sp) * 0.072169f;
            if (spec_mask < 0.0f) spec_mask = 0.0f;
            sr *= spec_mask;
            sg *= spec_mask;
            sb *= spec_mask;
        }

        float inv_gamma = (gamma > 1e-3f) ? (1.0f / gamma) : 1.0f;
        fr = powf(std::max(fr * ev_global, 0.0f), inv_gamma);
        fg = powf(std::max(fg * ev_global, 0.0f), inv_gamma);
        fb = powf(std::max(fb * ev_global, 0.0f), inv_gamma);
        sr = powf(std::max(sr * ev_global, 0.0f), inv_gamma);
        sg = powf(std::max(sg * ev_global, 0.0f), inv_gamma);
        sb = powf(std::max(sb * ev_global, 0.0f), inv_gamma);

        float ir = 0.0f, ig = 0.0f, ib = 0.0f;
        if (enable_incidence) {
            float inc_r = incidence_color.red   / 255.0f;
            float inc_g = incidence_color.green / 255.0f;
            float inc_b = incidence_color.blue  / 255.0f;
            float raw = 1.0f - Nz;
            if (raw < 0.0f) raw = 0.0f;
            float falloff_exp = 1.0f / std::max(incidence_falloff, 0.01f);
            float inc = powf(raw, falloff_exp);
            ir = inc * inc_r;
            ig = inc * inc_g;
            ib = inc * inc_b;
        }

        // Rim Light: directional incidence
        float rr = 0.0f, rg = 0.0f, rb = 0.0f;
        if (enable_rim) {
            float rim_r = rim_color.red   / 255.0f;
            float rim_g = rim_color.green / 255.0f;
            float rim_b = rim_color.blue  / 255.0f;

            float inc_factor = powf(1.001f - Nz_raw, 1.0f / std::max(rim_size, 0.01f));

            float angle_rad = rim_angle_deg * 0.017453292f;
            float s = sinf(angle_rad);
            float c = cosf(angle_rad);
            float dir;
            if (s <= 0.0f) dir = (1.0f - Nx_raw) * (-s);
            else           dir = s * Nx_raw;
            if (c <= 0.0f) dir -= (1.0f - Ny_raw) * c;
            else           dir += c * Ny_raw;
            dir = std::max(0.0f, dir);
            float directional = powf(dir, 5.0f / std::max(rim_width, 0.01f));

            // amount applied once via ApplyBlend strength (linear amount^1).
            // Unified with Specular/Incidence in 0.5.9.
            float intensity = inc_factor * directional * 4.0f;
            rr = intensity * rim_r;
            rg = intensity * rim_g;
            rb = intensity * rim_b;
        }

        float gr_out = 0.0f, gg_out = 0.0f, gb_out = 0.0f;
        if (enable_gradient && gradient_amount > 1e-5f) {
            float grad_r = gradient_color.red   / 255.0f;
            float grad_g = gradient_color.green / 255.0f;
            float grad_b = gradient_color.blue  / 255.0f;
            float lumi = Nx_raw * 0.21267f + Ny_raw * 0.71516f + Nz_raw * 0.072169f;
            gr_out = grad_r * lumi;
            gg_out = grad_g * lumi;
            gb_out = grad_b * lumi;
        }

        float toon_r_out = 0.0f, toon_g_out = 0.0f, toon_b_out = 0.0f;
        if (enable_toon) {
            float shadow_r = toon_shadow_color.red   / 255.0f;
            float shadow_g = toon_shadow_color.green / 255.0f;
            float shadow_b = toon_shadow_color.blue  / 255.0f;
            float mid_r    = toon_mid_color.red      / 255.0f;
            float mid_g    = toon_mid_color.green    / 255.0f;
            float mid_b    = toon_mid_color.blue     / 255.0f;
            float hi_r     = toon_highlight_color.red   / 255.0f;
            float hi_g     = toon_highlight_color.green / 255.0f;
            float hi_b     = toon_highlight_color.blue  / 255.0f;
            float smooth   = std::max(toon_smoothing * 0.25f, 1e-5f);

            if (toon_factor <= toon_shadow_width) {
                if (toon_factor <= toon_shadow_width - smooth) {
                    float t = toon_shadow_intensity;
                    float u = 1.0f - t;
                    toon_r_out = u * mid_r + t * shadow_r;
                    toon_g_out = u * mid_g + t * shadow_g;
                    toon_b_out = u * mid_b + t * shadow_b;
                } else {
                    float t = ((toon_shadow_width - toon_factor) / smooth) * toon_shadow_intensity;
                    float u = 1.0f - t;
                    toon_r_out = t * shadow_r + u * mid_r;
                    toon_g_out = t * shadow_g + u * mid_g;
                    toon_b_out = t * shadow_b + u * mid_b;
                }
            } else if (toon_factor <= toon_highlight_width) {
                if (toon_factor <= toon_highlight_width - smooth) {
                    toon_r_out = mid_r;
                    toon_g_out = mid_g;
                    toon_b_out = mid_b;
                } else {
                    float t = (toon_highlight_width - toon_factor) / smooth;
                    float u = 1.0f - t;
                    toon_r_out = ((u * hi_r + mid_r * t) - mid_r) * toon_highlight_intensity + mid_r;
                    toon_g_out = ((u * hi_g + mid_g * t) - mid_g) * toon_highlight_intensity + mid_g;
                    toon_b_out = ((u * hi_b + mid_b * t) - mid_b) * toon_highlight_intensity + mid_b;
                }
            } else {
                float t = toon_highlight_intensity;
                float u = 1.0f - t;
                toon_r_out = u * mid_r + t * hi_r;
                toon_g_out = u * mid_g + t * hi_g;
                toon_b_out = u * mid_b + t * hi_b;
            }
        }

        float matcap_r_out = 0.0f, matcap_g_out = 0.0f, matcap_b_out = 0.0f;
        bool  matcap_valid = false;
        if (enable_matcap && matcap_ld && matcap_ld->data &&
            matcap_ld->width > 1 && matcap_ld->height > 1)
        {
            A_long mw = matcap_ld->width;
            A_long mh = matcap_ld->height;
            float u_f = float(mw) * Nx_raw + float(matcap_offset_x);
            float v_f = float(mh) * (1.0f - Ny_raw) + float(matcap_offset_y);

            if (matcap_blur > 0 && matcap_blur_r) {
                SampleBlurredRGB(matcap_mode, mw, mh,
                                 *matcap_blur_r, *matcap_blur_g, *matcap_blur_b,
                                 u_f, v_f,
                                 matcap_r_out, matcap_g_out, matcap_b_out);
                matcap_valid = true;
            } else {
                A_long u0 = (A_long)floorf(u_f);
                A_long v0 = (A_long)floorf(v_f);
                float fu = u_f - float(u0);
                float fv = v_f - float(v0);
                A_long u0w = TileWrap(matcap_mode, mw - 1, u0);
                A_long v0w = TileWrap(matcap_mode, mh - 1, v0);
                A_long u1w = TileWrap(matcap_mode, mw - 1, u0 + 1);
                A_long v1w = TileWrap(matcap_mode, mh - 1, v0 + 1);
                if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                    const Pixel *r0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(matcap_ld->data) + v0w * matcap_ld->rowbytes);
                    const Pixel *r1p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(matcap_ld->data) + v1w * matcap_ld->rowbytes);
                    const Pixel &p00 = r0p[u0w];
                    const Pixel &p10 = r0p[u1w];
                    const Pixel &p01 = r1p[u0w];
                    const Pixel &p11 = r1p[u1w];
                    float ufm = 1.0f - fu;
                    float vfm = 1.0f - fv;
                    float r_top = ufm * ReadR(p00) + fu * ReadR(p10);
                    float r_bot = ufm * ReadR(p01) + fu * ReadR(p11);
                    float g_top = ufm * ReadG(p00) + fu * ReadG(p10);
                    float g_bot = ufm * ReadG(p01) + fu * ReadG(p11);
                    float b_top = ufm * ReadB(p00) + fu * ReadB(p10);
                    float b_bot = ufm * ReadB(p01) + fu * ReadB(p11);
                    matcap_r_out = vfm * r_top + fv * r_bot;
                    matcap_g_out = vfm * g_top + fv * g_bot;
                    matcap_b_out = vfm * b_top + fv * b_bot;
                    matcap_valid = true;
                }
            }
        }

        float texture_r_out = 0.0f, texture_g_out = 0.0f, texture_b_out = 0.0f;
        bool  texture_valid = false;
        if (enable_texture && tex_uv_ld && tex_uv_ld->data &&
            tex_map_ld && tex_map_ld->data &&
            tex_map_ld->width > 1 && tex_map_ld->height > 1)
        {
            A_long uv_c = std::min(col, tex_uv_ld->width  - 1);
            A_long uv_r = std::min(row, tex_uv_ld->height - 1);
            const Pixel *uv_row_p = reinterpret_cast<const Pixel *>(
                reinterpret_cast<const char *>(tex_uv_ld->data) + uv_r * tex_uv_ld->rowbytes);
            const Pixel &uvp = uv_row_p[uv_c];
            float u_norm = ReadR(uvp);
            float v_norm = ReadG(uvp);

            if (texture_uv_mirror == 2 || texture_uv_mirror == 4) u_norm = 1.0f - u_norm;
            if (texture_uv_mirror == 3 || texture_uv_mirror == 4) v_norm = 1.0f - v_norm;

            A_long tw = tex_map_ld->width;
            A_long th = tex_map_ld->height;
            float u_pix = u_norm * float(tw) - float(texture_offset_x);
            float v_pix = v_norm * float(th) - float(texture_offset_y);

            if (texture_filter == 2 /*Nearest*/) {
                A_long u0w = TileWrap(texture_tile, tw - 1, (A_long)floorf(u_pix));
                A_long v0w = TileWrap(texture_tile, th - 1, (A_long)floorf(v_pix));
                if (u0w >= 0 && v0w >= 0) {
                    const Pixel *r0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(tex_map_ld->data) + v0w * tex_map_ld->rowbytes);
                    const Pixel &p00 = r0p[u0w];
                    texture_r_out = ReadR(p00);
                    texture_g_out = ReadG(p00);
                    texture_b_out = ReadB(p00);
                    texture_valid = true;
                }
            } else /*Bilinear*/ {
                A_long u0 = (A_long)floorf(u_pix);
                A_long v0 = (A_long)floorf(v_pix);
                float fu = u_pix - float(u0);
                float fv = v_pix - float(v0);
                A_long u0w = TileWrap(texture_tile, tw - 1, u0);
                A_long v0w = TileWrap(texture_tile, th - 1, v0);
                A_long u1w = TileWrap(texture_tile, tw - 1, u0 + 1);
                A_long v1w = TileWrap(texture_tile, th - 1, v0 + 1);
                if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                    const Pixel *r0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(tex_map_ld->data) + v0w * tex_map_ld->rowbytes);
                    const Pixel *r1p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(tex_map_ld->data) + v1w * tex_map_ld->rowbytes);
                    const Pixel &p00 = r0p[u0w];
                    const Pixel &p10 = r0p[u1w];
                    const Pixel &p01 = r1p[u0w];
                    const Pixel &p11 = r1p[u1w];
                    float ufm = 1.0f - fu;
                    float vfm = 1.0f - fv;
                    float r_top = ufm * ReadR(p00) + fu * ReadR(p10);
                    float r_bot = ufm * ReadR(p01) + fu * ReadR(p11);
                    float g_top = ufm * ReadG(p00) + fu * ReadG(p10);
                    float g_bot = ufm * ReadG(p01) + fu * ReadG(p11);
                    float b_top = ufm * ReadB(p00) + fu * ReadB(p10);
                    float b_bot = ufm * ReadB(p01) + fu * ReadB(p11);
                    texture_r_out = vfm * r_top + fv * r_bot;
                    texture_g_out = vfm * g_top + fv * g_bot;
                    texture_b_out = vfm * b_top + fv * b_bot;
                    texture_valid = true;
                }
            }
        }

        float reflection_r_out = 0.0f, reflection_g_out = 0.0f, reflection_b_out = 0.0f;
        bool  reflection_valid = false;
        if (enable_reflection && env_map_ld && env_map_ld->data &&
            env_map_ld->width > 1 && env_map_ld->height > 1)
        {
            // View-independent (normal-based): R = N.
            // The view-dependent formula R = V - 2*dot(V,N)*N causes the side
            // of a sphere (Nz~=0) to map to a fixed env-map point, leaving
            // it gray. Mapping the normal directly to the env-map (Matcap-style)
            // gives a smooth reflection across the whole surface. We use
            // direct normal-component products instead of atan2 for visual
            // continuity at silhouettes.
            float Rx = Nx;
            float Ry = Ny;
            float Rz = Nz;

            float incl_rad = reflection_inclination_deg * 0.017453292f;
            float azim_rad = reflection_azimuth_deg     * 0.017453292f;
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
            if (reflection_env_mode == 1) {
                float r_len = sqrtf(Rx*Rx + Ry*Ry + Rz*Rz);
                if (r_len < 1e-5f) r_len = 1.0f;
                float Rxn = Rx / r_len, Ryn = Ry / r_len, Rzn = Rz / r_len;
                u_norm = atan2f(Rxn, Rzn) / 6.283185307f + 0.5f;
                float clamped_y = std::max(-1.0f, std::min(1.0f, Ryn));
                v_norm = acosf(clamped_y) / 3.141592653f;
            } else {
                u_norm = atan2f(Rx, Rz) / 6.283185307f + 0.5f;
                v_norm = Ry * 0.5f + 0.5f;
            }

            // Scale: zoom around UV center (0.5, 0.5).
            // > 1 = features look larger / seam pushed off-screen.
            // < 1 = features tiled/zoomed out (Tile=Repeat for repetitions).
            float scale_safe = std::max(reflection_scale, 1e-3f);
            float scale_inv  = 1.0f / scale_safe;
            u_norm = (u_norm - 0.5f) * scale_inv + 0.5f;
            v_norm = (v_norm - 0.5f) * scale_inv + 0.5f;

            A_long ew = env_map_ld->width;
            A_long eh = env_map_ld->height;
            float u_pix = u_norm * float(ew);
            float v_pix = v_norm * float(eh);

            float sr_s = 0.0f, sg_s = 0.0f, sb_s = 0.0f;
            bool sampled = false;

            if (reflection_blur > 0 && reflection_blur_r) {
                SampleBlurredRGB(reflection_tile, ew, eh,
                                 *reflection_blur_r, *reflection_blur_g, *reflection_blur_b,
                                 u_pix, v_pix, sr_s, sg_s, sb_s);
                sampled = true;
            } else {
                A_long u0 = (A_long)floorf(u_pix);
                A_long v0 = (A_long)floorf(v_pix);
                float fu = u_pix - float(u0);
                float fv = v_pix - float(v0);
                A_long u0w = TileWrap(reflection_tile, ew - 1, u0);
                A_long v0w = TileWrap(reflection_tile, eh - 1, v0);
                A_long u1w = TileWrap(reflection_tile, ew - 1, u0 + 1);
                A_long v1w = TileWrap(reflection_tile, eh - 1, v0 + 1);
                if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                    const Pixel *e0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(env_map_ld->data) + v0w * env_map_ld->rowbytes);
                    const Pixel *e1p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(env_map_ld->data) + v1w * env_map_ld->rowbytes);
                    const Pixel &p00 = e0p[u0w];
                    const Pixel &p10 = e0p[u1w];
                    const Pixel &p01 = e1p[u0w];
                    const Pixel &p11 = e1p[u1w];
                    float ufm = 1.0f - fu;
                    float vfm = 1.0f - fv;
                    sr_s = vfm * (ufm * ReadR(p00) + fu * ReadR(p10)) + fv * (ufm * ReadR(p01) + fu * ReadR(p11));
                    sg_s = vfm * (ufm * ReadG(p00) + fu * ReadG(p10)) + fv * (ufm * ReadG(p01) + fu * ReadG(p11));
                    sb_s = vfm * (ufm * ReadB(p00) + fu * ReadB(p10)) + fv * (ufm * ReadB(p01) + fu * ReadB(p11));
                    sampled = true;
                }
            }

            if (sampled) {
                if (reflection_gamma > 1e-5f && fabsf(reflection_gamma - 1.0f) > 1e-5f) {
                    float inv_g = 1.0f / reflection_gamma;
                    sr_s = powf(std::max(0.0f, sr_s), inv_g);
                    sg_s = powf(std::max(0.0f, sg_s), inv_g);
                    sb_s = powf(std::max(0.0f, sb_s), inv_g);
                }

                float col_r = reflection_color.red   / 255.0f;
                float col_g = reflection_color.green / 255.0f;
                float col_b = reflection_color.blue  / 255.0f;

                float fresnel_factor = 1.0f;
                if (reflection_fresnel > 1e-5f) {
                    float cos_theta = std::max(0.0f, Nz);
                    float r0_factor = 0.04f;
                    float ior_diff = (reflection_ior - 1.0f) / (reflection_ior + 1.0f);
                    if (fabsf(reflection_ior + 1.0f) > 1e-5f) r0_factor = ior_diff * ior_diff;
                    float F = r0_factor + (1.0f - r0_factor) * powf(1.0f - cos_theta, 5.0f);
                    float depth_safe = std::max(0.01f, fabsf(reflection_fresnel_depth));
                    float F_pow = powf(std::max(0.0f, F), 1.0f / depth_safe);
                    fresnel_factor = std::min(1.0f, F_pow * reflection_fresnel);
                }

                reflection_r_out = sr_s * col_r * fresnel_factor;
                reflection_g_out = sg_s * col_g * fresnel_factor;
                reflection_b_out = sb_s * col_b * fresnel_factor;
                reflection_valid = true;
            }
        }

        float refraction_r_out = 0.0f, refraction_g_out = 0.0f, refraction_b_out = 0.0f;
        bool  refraction_valid = false;
        if (enable_refraction && refr_map_ld && refr_map_ld->data &&
            refr_map_ld->width > 1 && refr_map_ld->height > 1)
        {
            // Refraction screen-space offset (simple Nx/Ny + IOR).
            // Scale acts as texture zoom (1/Scale on UV step), like Bump.
            // Phase 4 may swap to strict Snell.
            float ior_safe = (fabsf(refraction_ior) < 1e-3f) ? 1e-3f : refraction_ior;
            // Depth-aware modulation: deeper pixels (larger pixel_z) get stronger
            // refraction, simulating thicker glass / longer optical path.
            // pixel_z is in AE comp px (Depth Strength applied). Scale 0.01 means
            // 100 px deep = 2x refraction.
            float depth_modulator = (refraction_use_depth) ? (1.0f + pixel_z * 0.01f) : 1.0f;
            float ref_factor = (1.0f - 1.0f / ior_safe) * refraction_strength * depth_modulator;
            float dx = Nx * ref_factor;
            float dy = -Ny * ref_factor;  // AE Y-down

            float scale_inv = (refraction_scale > 1e-3f) ? 1.0f / refraction_scale : 1.0f;
            A_long rw = refr_map_ld->width;
            A_long rh = refr_map_ld->height;
            float u_pix = (float(col) - float(refraction_offset_x)) * scale_inv + dx;
            float v_pix = (float(row) - float(refraction_offset_y)) * scale_inv + dy;

            float sr_s = 0.0f, sg_s = 0.0f, sb_s = 0.0f;
            bool sampled = false;

            if (refraction_blur > 0 && refraction_blur_r) {
                SampleBlurredRGB(refraction_tile, rw, rh,
                                 *refraction_blur_r, *refraction_blur_g, *refraction_blur_b,
                                 u_pix, v_pix, sr_s, sg_s, sb_s);
                sampled = true;
            } else {
                A_long u0 = (A_long)floorf(u_pix);
                A_long v0 = (A_long)floorf(v_pix);
                float fu = u_pix - float(u0);
                float fv = v_pix - float(v0);
                A_long u0w = TileWrap(refraction_tile, rw - 1, u0);
                A_long v0w = TileWrap(refraction_tile, rh - 1, v0);
                A_long u1w = TileWrap(refraction_tile, rw - 1, u0 + 1);
                A_long v1w = TileWrap(refraction_tile, rh - 1, v0 + 1);
                if (u0w >= 0 && v0w >= 0 && u1w >= 0 && v1w >= 0) {
                    const Pixel *r0p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(refr_map_ld->data) + v0w * refr_map_ld->rowbytes);
                    const Pixel *r1p = reinterpret_cast<const Pixel *>(
                        reinterpret_cast<const char *>(refr_map_ld->data) + v1w * refr_map_ld->rowbytes);
                    const Pixel &p00 = r0p[u0w];
                    const Pixel &p10 = r0p[u1w];
                    const Pixel &p01 = r1p[u0w];
                    const Pixel &p11 = r1p[u1w];
                    float ufm = 1.0f - fu;
                    float vfm = 1.0f - fv;
                    sr_s = vfm * (ufm * ReadR(p00) + fu * ReadR(p10)) + fv * (ufm * ReadR(p01) + fu * ReadR(p11));
                    sg_s = vfm * (ufm * ReadG(p00) + fu * ReadG(p10)) + fv * (ufm * ReadG(p01) + fu * ReadG(p11));
                    sb_s = vfm * (ufm * ReadB(p00) + fu * ReadB(p10)) + fv * (ufm * ReadB(p01) + fu * ReadB(p11));
                    sampled = true;
                }
            }

            if (sampled) {
                if (refraction_gamma > 1e-5f && fabsf(refraction_gamma - 1.0f) > 1e-5f) {
                    float inv_g = 1.0f / refraction_gamma;
                    sr_s = powf(std::max(0.0f, sr_s), inv_g);
                    sg_s = powf(std::max(0.0f, sg_s), inv_g);
                    sb_s = powf(std::max(0.0f, sb_s), inv_g);
                }

                float col_r = refraction_color.red   / 255.0f;
                float col_g = refraction_color.green / 255.0f;
                float col_b = refraction_color.blue  / 255.0f;

                float fresnel_factor = 1.0f;
                if (refraction_fresnel > 1e-5f) {
                    float cos_theta = std::max(0.0f, Nz);
                    float ior_diff = (refraction_ior - 1.0f) / (refraction_ior + 1.0f);
                    float r0_factor = ior_diff * ior_diff;
                    if (fabsf(refraction_ior + 1.0f) < 1e-5f) r0_factor = 0.04f;
                    // Refraction lets light pass through, so invert Schlick.
                    float F = r0_factor + (1.0f - r0_factor) * powf(1.0f - cos_theta, 5.0f);
                    F = 1.0f - F;
                    float depth_safe = std::max(0.01f, fabsf(refraction_fresnel_depth));
                    float F_pow = powf(std::max(0.0f, F), 1.0f / depth_safe);
                    fresnel_factor = std::min(1.0f, F_pow * refraction_fresnel);
                }

                refraction_r_out = sr_s * col_r * fresnel_factor;
                refraction_g_out = sg_s * col_g * fresnel_factor;
                refraction_b_out = sb_s * col_b * fresnel_factor;
                refraction_valid = true;
            }
        }

        if (display == DISPLAY_INCIDENCE) {
            Write(dst, ir * incidence_amount, ig * incidence_amount, ib * incidence_amount);
        } else if (display == DISPLAY_RIM) {
            Write(dst, rr * rim_amount, rg * rim_amount, rb * rim_amount);
        } else if (display == DISPLAY_SPECULAR) {
            Write(dst, sr * specular_amount, sg * specular_amount, sb * specular_amount);
        } else if (display == DISPLAY_GRADIENT) {
            Write(dst, gr_out * gradient_amount, gg_out * gradient_amount, gb_out * gradient_amount);
        } else if (display == DISPLAY_TOON) {
            Write(dst, toon_r_out * toon_amount, toon_g_out * toon_amount, toon_b_out * toon_amount);
        } else if (display == DISPLAY_MATCAP) {
            if (matcap_valid) {
                Write(dst, matcap_r_out * matcap_amount, matcap_g_out * matcap_amount, matcap_b_out * matcap_amount);
            } else {
                Write(dst, 0.0f, 0.0f, 0.0f);
            }
        } else if (display == DISPLAY_BUMP) {
            Write(dst, Nx * 0.5f + 0.5f, Ny * 0.5f + 0.5f, Nz * 0.5f + 0.5f);
        } else if (display == DISPLAY_TEXTURE) {
            if (texture_valid) {
                Write(dst, texture_r_out * texture_amount, texture_g_out * texture_amount, texture_b_out * texture_amount);
            } else {
                Write(dst, 0.0f, 0.0f, 0.0f);
            }
        } else if (display == DISPLAY_REFLECTION) {
            if (reflection_valid) {
                Write(dst, reflection_r_out * reflection_amount, reflection_g_out * reflection_amount, reflection_b_out * reflection_amount);
            } else {
                Write(dst, 0.0f, 0.0f, 0.0f);
            }
        } else if (display == DISPLAY_REFRACTION) {
            if (refraction_valid) {
                Write(dst, refraction_r_out * refraction_amount, refraction_g_out * refraction_amount, refraction_b_out * refraction_amount);
            } else {
                Write(dst, 0.0f, 0.0f, 0.0f);
            }
        } else if (display == DISPLAY_ALL) {
            // Canvas: input pixel (or 0 if no input). Each section blends onto it.
            float ar = 0.0f, ag = 0.0f, ab = 0.0f;
            if (in_p) {
                A_long in_c = std::min(col, input->width - 1);
                const Pixel &ip = in_p[in_c];
                ar = ReadR(ip);
                ag = ReadG(ip);
                ab = ReadB(ip);
            }
            if (enable_shading) {
                ApplyBlend(shading_blend, shading_blend_amount, fr, fg, fb, ar, ag, ab);
            }
            if (enable_matcap && matcap_valid) {
                ApplyBlend(matcap_blend, matcap_amount, matcap_r_out, matcap_g_out, matcap_b_out, ar, ag, ab);
            }
            if (enable_texture && texture_valid) {
                ApplyBlend(texture_blend, texture_amount, texture_r_out, texture_g_out, texture_b_out, ar, ag, ab);
            }
            if (enable_toon) {
                ApplyBlend(toon_blend, toon_amount, toon_r_out, toon_g_out, toon_b_out, ar, ag, ab);
            }
            if (enable_specular) {
                ApplyBlend(specular_blend, specular_amount, sr, sg, sb, ar, ag, ab);
            }
            if (enable_incidence) {
                ApplyBlend(incidence_blend, incidence_amount, ir, ig, ib, ar, ag, ab);
            }
            if (enable_rim) {
                ApplyBlend(rim_blend, rim_amount, rr, rg, rb, ar, ag, ab);
            }
            if (enable_gradient && gradient_amount > 1e-5f) {
                ApplyBlend(gradient_blend, gradient_amount, gr_out, gg_out, gb_out, ar, ag, ab);
            }
            if (enable_reflection && reflection_valid) {
                ApplyBlend(reflection_blend, reflection_amount, reflection_r_out, reflection_g_out, reflection_b_out, ar, ag, ab);
            }
            if (enable_refraction && refraction_valid) {
                ApplyBlend(refraction_blend, refraction_amount, refraction_r_out, refraction_g_out, refraction_b_out, ar, ag, ab);
            }
            Write(dst, ar, ag, ab);
        } else {
            // DISPLAY_DIFFUSE
            Write(dst, fr, fg, fb);
        }
        if (use_normal_alpha) SetAlpha(dst, N_alpha);
    }

    return PF_Err_NONE;
}

template <typename Pixel>
static PF_Err
RenderShadingT(
    PF_InData        *in_data,
    PF_LayerDef      *input,
    const PF_LayerDef *nm,
    PF_LayerDef      *output,
    A_long            chan_x, A_long chan_y, A_long chan_z,
    A_Boolean         inv_x, A_Boolean inv_y, A_Boolean inv_z,
    A_Boolean         use_normal_alpha,
    float             normal_strength,
    const std::vector<float> *blur_nx,
    const std::vector<float> *blur_ny,
    const std::vector<float> *blur_nz,
    const std::vector<float> *blur_a,
    const std::vector<float> *depth_buf,
    const LightInfo   lights[], A_long light_count,
    A_long            display,
    A_Boolean         enable_shading,
    float             diffuse_amount,
    PF_Pixel8         diffuse_color,
    PF_Pixel8         ambient_color,
    const PF_LayerDef *ao_ld,
    PF_Pixel8         incandescence_color,
    A_long            shading_blend,
    float             shading_blend_amount,
    A_Boolean         enable_specular,
    const PF_LayerDef *spec_ld,
    float             specular_amount,
    float             specular_spread,
    PF_Pixel8         specular_color,
    A_long            specular_blend,
    A_long            falloff_mode,
    float             falloff_val,
    float             exposure,
    float             gamma,
    A_Boolean         enable_incidence,
    float             incidence_amount,
    float             incidence_falloff,
    PF_Pixel8         incidence_color,
    A_long            incidence_blend,
    A_Boolean         enable_rim,
    float             rim_amount,
    float             rim_size,
    float             rim_width,
    PF_Pixel8         rim_color,
    float             rim_angle_deg,
    A_long            rim_blend,
    A_Boolean         enable_gradient,
    float             gradient_amount,
    PF_Pixel8         gradient_color,
    A_long            gradient_blend,
    A_Boolean         enable_toon,
    float             toon_amount,
    float             toon_smoothing,
    PF_Pixel8         toon_shadow_color,
    float             toon_shadow_intensity,
    float             toon_shadow_width,
    PF_Pixel8         toon_mid_color,
    float             toon_highlight_width,
    PF_Pixel8         toon_highlight_color,
    float             toon_highlight_intensity,
    A_long            toon_blend,
    A_Boolean         enable_matcap,
    const PF_LayerDef *matcap_ld,
    float             matcap_amount,
    A_long            matcap_offset_x,
    A_long            matcap_offset_y,
    A_long            matcap_mode,
    A_long            matcap_blur,
    A_long            matcap_blend,
    const std::vector<float> *matcap_blur_r,
    const std::vector<float> *matcap_blur_g,
    const std::vector<float> *matcap_blur_b,
    A_Boolean         enable_bump,
    const PF_LayerDef *bump_ld,
    float             bump_intensity,
    A_long            bump_tile,
    A_long            bump_filter,
    A_long            bump_offset_x,
    A_long            bump_offset_y,
    float             bump_scale,
    A_Boolean         enable_texture,
    const PF_LayerDef *tex_uv_ld,
    const PF_LayerDef *tex_map_ld,
    float             texture_amount,
    A_long            texture_tile,
    A_long            texture_uv_mirror,
    A_long            texture_filter,
    A_long            texture_offset_x,
    A_long            texture_offset_y,
    A_long            texture_blend,
    A_Boolean         enable_reflection,
    const PF_LayerDef *env_map_ld,
    float             reflection_amount,
    A_long            reflection_env_mode,
    float             reflection_gamma,
    A_long            reflection_tile,
    A_long            reflection_blur,
    PF_Pixel8         reflection_color,
    float             reflection_inclination_deg,
    float             reflection_azimuth_deg,
    float             reflection_scale,
    float             reflection_ior,
    float             reflection_fresnel,
    float             reflection_fresnel_depth,
    A_long            reflection_blend,
    const std::vector<float> *reflection_blur_r,
    const std::vector<float> *reflection_blur_g,
    const std::vector<float> *reflection_blur_b,
    A_Boolean         enable_refraction,
    const PF_LayerDef *refr_map_ld,
    float             refraction_amount,
    float             refraction_ior,
    float             refraction_strength,
    A_Boolean         refraction_use_depth,
    float             refraction_gamma,
    A_long            refraction_tile,
    A_long            refraction_blur,
    A_long            refraction_offset_x,
    A_long            refraction_offset_y,
    float             refraction_scale,
    PF_Pixel8         refraction_color,
    float             refraction_fresnel,
    float             refraction_fresnel_depth,
    A_long            refraction_blend,
    const std::vector<float> *refraction_blur_r,
    const std::vector<float> *refraction_blur_g,
    const std::vector<float> *refraction_blur_b)
{
    double sx = (in_data->downsample_x.num > 0)
        ? double(in_data->downsample_x.den) / in_data->downsample_x.num : 1.0;
    double sy = (in_data->downsample_y.num > 0)
        ? double(in_data->downsample_y.den) / in_data->downsample_y.num : 1.0;

    float diff_r = diffuse_color.red   / 255.0f;
    float diff_g = diffuse_color.green / 255.0f;
    float diff_b = diffuse_color.blue  / 255.0f;

    float amb_r = ambient_color.red   / 255.0f;
    float amb_g = ambient_color.green / 255.0f;
    float amb_b = ambient_color.blue  / 255.0f;

    float ainc_r = incandescence_color.red   / 255.0f;
    float ainc_g = incandescence_color.green / 255.0f;
    float ainc_b = incandescence_color.blue  / 255.0f;

    float ev_global = powf(2.0f, exposure);

    float spec_r = specular_color.red   / 255.0f;
    float spec_g = specular_color.green / 255.0f;
    float spec_b = specular_color.blue  / 255.0f;

    float shininess = 5.0f / std::max(specular_spread, 0.01f);

    A_long out_w = output->width;
    A_long out_h = output->height;

    bool have_blur = (blur_nx && blur_ny && blur_nz);
    A_long nm_w = nm ? nm->width  : 0;
    A_long nm_h = nm ? nm->height : 0;

    // ---- Build ctx and dispatch to per-row function ShadingRowFnT ----
    // Step 14b-gamma (0.5.11): rows are processed in parallel via iterate_generic
    // (Iterate8Suite1 -- depth-independent, drives any Pixel type).
    ShadingRefconT<Pixel> ctx;
    ctx.in_data = in_data;
    ctx.input   = input;
    ctx.nm      = nm;
    ctx.output  = output;
    ctx.ao_ld   = ao_ld;
    ctx.spec_ld = spec_ld;
    ctx.matcap_ld = matcap_ld;
    ctx.bump_ld   = bump_ld;
    ctx.tex_uv_ld = tex_uv_ld;
    ctx.tex_map_ld = tex_map_ld;
    ctx.env_map_ld = env_map_ld;
    ctx.refr_map_ld = refr_map_ld;

    ctx.chan_x = chan_x; ctx.chan_y = chan_y; ctx.chan_z = chan_z;
    ctx.inv_x = inv_x; ctx.inv_y = inv_y; ctx.inv_z = inv_z;
    ctx.use_normal_alpha = use_normal_alpha;
    ctx.normal_strength  = normal_strength;

    ctx.blur_nx = blur_nx;
    ctx.blur_ny = blur_ny;
    ctx.blur_nz = blur_nz;
    ctx.blur_a  = blur_a;
    ctx.depth_buf = depth_buf;
    ctx.matcap_blur_r = matcap_blur_r;
    ctx.matcap_blur_g = matcap_blur_g;
    ctx.matcap_blur_b = matcap_blur_b;
    ctx.reflection_blur_r = reflection_blur_r;
    ctx.reflection_blur_g = reflection_blur_g;
    ctx.reflection_blur_b = reflection_blur_b;
    ctx.refraction_blur_r = refraction_blur_r;
    ctx.refraction_blur_g = refraction_blur_g;
    ctx.refraction_blur_b = refraction_blur_b;

    ctx.lights = lights;
    ctx.light_count = light_count;

    ctx.display = display;
    ctx.enable_shading = enable_shading;
    ctx.diffuse_amount = diffuse_amount;
    ctx.diffuse_color = diffuse_color;
    ctx.ambient_color = ambient_color;
    ctx.incandescence_color = incandescence_color;
    ctx.shading_blend = shading_blend;
    ctx.shading_blend_amount = shading_blend_amount;

    ctx.enable_specular = enable_specular;
    ctx.specular_amount = specular_amount;
    ctx.specular_spread = specular_spread;
    ctx.specular_color = specular_color;
    ctx.specular_blend = specular_blend;

    ctx.falloff_mode = falloff_mode;
    ctx.falloff_val  = falloff_val;
    ctx.exposure     = exposure;
    ctx.gamma        = gamma;

    ctx.enable_incidence = enable_incidence;
    ctx.incidence_amount = incidence_amount;
    ctx.incidence_falloff = incidence_falloff;
    ctx.incidence_color = incidence_color;
    ctx.incidence_blend = incidence_blend;

    ctx.enable_rim = enable_rim;
    ctx.rim_amount = rim_amount;
    ctx.rim_size   = rim_size;
    ctx.rim_width  = rim_width;
    ctx.rim_color  = rim_color;
    ctx.rim_angle_deg = rim_angle_deg;
    ctx.rim_blend = rim_blend;

    ctx.enable_gradient = enable_gradient;
    ctx.gradient_amount = gradient_amount;
    ctx.gradient_color  = gradient_color;
    ctx.gradient_blend  = gradient_blend;

    ctx.enable_toon = enable_toon;
    ctx.toon_amount = toon_amount;
    ctx.toon_smoothing = toon_smoothing;
    ctx.toon_shadow_color = toon_shadow_color;
    ctx.toon_shadow_intensity = toon_shadow_intensity;
    ctx.toon_shadow_width = toon_shadow_width;
    ctx.toon_mid_color = toon_mid_color;
    ctx.toon_highlight_width = toon_highlight_width;
    ctx.toon_highlight_color = toon_highlight_color;
    ctx.toon_highlight_intensity = toon_highlight_intensity;
    ctx.toon_blend = toon_blend;

    ctx.enable_matcap = enable_matcap;
    ctx.matcap_amount = matcap_amount;
    ctx.matcap_offset_x = matcap_offset_x;
    ctx.matcap_offset_y = matcap_offset_y;
    ctx.matcap_mode = matcap_mode;
    ctx.matcap_blur = matcap_blur;
    ctx.matcap_blend = matcap_blend;

    ctx.enable_bump = enable_bump;
    ctx.bump_intensity = bump_intensity;
    ctx.bump_tile = bump_tile;
    ctx.bump_filter = bump_filter;
    ctx.bump_offset_x = bump_offset_x;
    ctx.bump_offset_y = bump_offset_y;
    ctx.bump_scale = bump_scale;

    ctx.enable_texture = enable_texture;
    ctx.texture_amount = texture_amount;
    ctx.texture_tile = texture_tile;
    ctx.texture_uv_mirror = texture_uv_mirror;
    ctx.texture_filter = texture_filter;
    ctx.texture_offset_x = texture_offset_x;
    ctx.texture_offset_y = texture_offset_y;
    ctx.texture_blend = texture_blend;

    ctx.enable_reflection = enable_reflection;
    ctx.reflection_amount = reflection_amount;
    ctx.reflection_env_mode = reflection_env_mode;
    ctx.reflection_gamma = reflection_gamma;
    ctx.reflection_tile = reflection_tile;
    ctx.reflection_blur = reflection_blur;
    ctx.reflection_color = reflection_color;
    ctx.reflection_inclination_deg = reflection_inclination_deg;
    ctx.reflection_azimuth_deg = reflection_azimuth_deg;
    ctx.reflection_scale = reflection_scale;
    ctx.reflection_ior = reflection_ior;
    ctx.reflection_fresnel = reflection_fresnel;
    ctx.reflection_fresnel_depth = reflection_fresnel_depth;
    ctx.reflection_blend = reflection_blend;

    ctx.enable_refraction = enable_refraction;
    ctx.refraction_amount = refraction_amount;
    ctx.refraction_ior = refraction_ior;
    ctx.refraction_strength = refraction_strength;
    ctx.refraction_use_depth = refraction_use_depth;
    ctx.refraction_gamma = refraction_gamma;
    ctx.refraction_tile = refraction_tile;
    ctx.refraction_blur = refraction_blur;
    ctx.refraction_offset_x = refraction_offset_x;
    ctx.refraction_offset_y = refraction_offset_y;
    ctx.refraction_scale = refraction_scale;
    ctx.refraction_color = refraction_color;
    ctx.refraction_fresnel = refraction_fresnel;
    ctx.refraction_fresnel_depth = refraction_fresnel_depth;
    ctx.refraction_blend = refraction_blend;

    ctx.sx = sx; ctx.sy = sy;
    ctx.out_w = out_w; ctx.out_h = out_h;
    ctx.nm_w = nm_w; ctx.nm_h = nm_h;
    ctx.have_blur = have_blur;
    ctx.ev_global = ev_global;
    ctx.shininess = shininess;
    ctx.diff_r = diff_r; ctx.diff_g = diff_g; ctx.diff_b = diff_b;
    ctx.amb_r  = amb_r;  ctx.amb_g  = amb_g;  ctx.amb_b  = amb_b;
    ctx.ainc_r = ainc_r; ctx.ainc_g = ainc_g; ctx.ainc_b = ainc_b;
    ctx.spec_r = spec_r; ctx.spec_g = spec_g; ctx.spec_b = spec_b;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    return suites.Iterate8Suite1()->iterate_generic(
        out_h, &ctx, &ShadingRowFnT<Pixel>);
}

template <typename Pixel>
static PF_Err
RenderT(
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output,
    PF_LayerDef *input_world,
    const NormForgeLightCache *light_cache)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    A_long display    = params[NF_DISPLAY]->u.pd.value;
    bool show_normals  = (display == DISPLAY_ADJUSTED_NORMALS ||
                          display == DISPLAY_ORIGINAL_NORMALS ||
                          display == DISPLAY_DEPTH);
    bool show_shading  = (display == DISPLAY_ALL ||
                          display == DISPLAY_DIFFUSE ||
                          display == DISPLAY_SPECULAR ||
                          display == DISPLAY_INCIDENCE ||
                          display == DISPLAY_RIM ||
                          display == DISPLAY_GRADIENT ||
                          display == DISPLAY_TOON ||
                          display == DISPLAY_MATCAP ||
                          display == DISPLAY_BUMP ||
                          display == DISPLAY_TEXTURE ||
                          display == DISPLAY_REFLECTION ||
                          display == DISPLAY_REFRACTION);

    // Passthrough: unsupported or off modes
    if (display == DISPLAY_OFF || (!show_normals && !show_shading)) {
        ERR(suites.WorldTransformSuite1()->copy(
            in_data->effect_ref,
            input_world,
            output, NULL, NULL));
        return err;
    }

    A_long    falloff_mode  = params[NF_FALLOFF_MODE]->u.pd.value;
    float     falloff_val   = float(params[NF_FALLOFF]->u.fs_d.value);
    float     exposure      = float(params[NF_EXPOSURE]->u.fs_d.value);
    float     gamma_val     = float(params[NF_GAMMA]->u.fs_d.value);

    // Pattern D (0.5.13): lights are pre-collected by SmartPreRender on the
    // per-frame thread. We just copy them out of the cache here. No AEGP suite
    // calls in SmartRender threads -> no race with MFR per-pixel parallelism.
    LightInfo lights[MAX_LIGHTS] = {};
    A_long light_count = 0;
    if (show_shading && light_cache) {
        memcpy(lights, light_cache->lights, sizeof(lights));
        light_count = light_cache->light_count;
    }

    // Checkout normal map — CHECKIN must be called if and only if this succeeds
    PF_ParamDef nm_param;
    AEFX_CLR_STRUCT(nm_param);
    PF_Err co_err = PF_CHECKOUT_PARAM(in_data, NF_NORMAL_MAP,
        in_data->current_time, in_data->time_step,
        in_data->time_scale, &nm_param);
    if (co_err != PF_Err_NONE) return co_err;

    PF_LayerDef *nm = &nm_param.u.ld;

    // Phase 6-1 (0.7.0): If Normal Map layer is None / unconnected, fall back
    // to the input layer (param[0]) as recommended by the AE SDK and for
    // Normality compatibility. Subsequent code (PreBlurNormalMapT,
    // RenderShadingT, RenderNormalsT, per-row body) works unchanged because
    // nm->data becomes non-NULL again.
    if (nm->data == NULL) {
        nm = input_world;
    }

    PF_ParamDef depth_param;
    AEFX_CLR_STRUCT(depth_param);
    PF_LayerDef *depth_ld = NULL;
    {
        PF_Err dm_err = PF_CHECKOUT_PARAM(in_data, NF_DEPTH_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &depth_param);
        if (dm_err == PF_Err_NONE) {
            depth_ld = &depth_param.u.ld;
        }
    }

    PF_ParamDef matcap_param;
    AEFX_CLR_STRUCT(matcap_param);
    PF_LayerDef *matcap_ld = NULL;
    {
        PF_Err mc_err = PF_CHECKOUT_PARAM(in_data, NF_MATCAP_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &matcap_param);
        if (mc_err == PF_Err_NONE) {
            matcap_ld = &matcap_param.u.ld;
        }
    }

    PF_ParamDef bump_param;
    AEFX_CLR_STRUCT(bump_param);
    PF_LayerDef *bump_ld = NULL;
    {
        PF_Err bm_err = PF_CHECKOUT_PARAM(in_data, NF_BUMP_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &bump_param);
        if (bm_err == PF_Err_NONE) {
            bump_ld = &bump_param.u.ld;
        }
    }

    PF_ParamDef env_map_param;
    AEFX_CLR_STRUCT(env_map_param);
    PF_LayerDef *env_map_ld = NULL;
    {
        PF_Err em_err = PF_CHECKOUT_PARAM(in_data, NF_REFLECTION_ENV_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &env_map_param);
        if (em_err == PF_Err_NONE) {
            env_map_ld = &env_map_param.u.ld;
        }
    }

    PF_ParamDef refr_map_param;
    AEFX_CLR_STRUCT(refr_map_param);
    PF_LayerDef *refr_map_ld = NULL;
    {
        PF_Err rm_err = PF_CHECKOUT_PARAM(in_data, NF_REFRACTION_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &refr_map_param);
        if (rm_err == PF_Err_NONE) {
            refr_map_ld = &refr_map_param.u.ld;
        }
    }

    PF_ParamDef tex_uv_param;
    AEFX_CLR_STRUCT(tex_uv_param);
    PF_LayerDef *tex_uv_ld = NULL;
    {
        PF_Err uv_err = PF_CHECKOUT_PARAM(in_data, NF_TEXTURE_UV_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &tex_uv_param);
        if (uv_err == PF_Err_NONE) {
            tex_uv_ld = &tex_uv_param.u.ld;
        }
    }

    PF_ParamDef tex_map_param;
    AEFX_CLR_STRUCT(tex_map_param);
    PF_LayerDef *tex_map_ld = NULL;
    {
        PF_Err tm_err = PF_CHECKOUT_PARAM(in_data, NF_TEXTURE_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &tex_map_param);
        if (tm_err == PF_Err_NONE) {
            tex_map_ld = &tex_map_param.u.ld;
        }
    }

    PF_ParamDef ao_map_param;
    AEFX_CLR_STRUCT(ao_map_param);
    PF_LayerDef *ao_ld = NULL;
    {
        PF_Err ao_err = PF_CHECKOUT_PARAM(in_data, NF_AO_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &ao_map_param);
        if (ao_err == PF_Err_NONE && ao_map_param.u.ld.data) {
            ao_ld = &ao_map_param.u.ld;
        }
    }

    PF_ParamDef spec_map_param;
    AEFX_CLR_STRUCT(spec_map_param);
    PF_LayerDef *spec_ld = NULL;
    {
        PF_Err sm_err = PF_CHECKOUT_PARAM(in_data, NF_SPECULAR_MAP,
            in_data->current_time, in_data->time_step,
            in_data->time_scale, &spec_map_param);
        if (sm_err == PF_Err_NONE && spec_map_param.u.ld.data) {
            spec_ld = &spec_map_param.u.ld;
        }
    }

    A_long chan_x   = params[NF_CHANNEL_X]->u.pd.value;
    A_long chan_y   = params[NF_CHANNEL_Y]->u.pd.value;
    A_long chan_z   = params[NF_CHANNEL_Z]->u.pd.value;
    A_Boolean inv_x = params[NF_INVERT_X]->u.bd.value;
    A_Boolean inv_y = params[NF_INVERT_Y]->u.bd.value;
    A_Boolean inv_z = params[NF_INVERT_Z]->u.bd.value;
    A_Boolean use_normal_alpha = params[NF_USE_NORMAL_ALPHA]->u.bd.value;
    A_long    pre_blur_radius  = params[NF_PRE_BLUR]->u.sd.value;
    A_Boolean pre_blur_alpha   = params[NF_PRE_BLUR_ALPHA]->u.bd.value;
    float     normal_strength  = float(params[NF_NORMAL_STRENGTH]->u.fs_d.value);
    A_Boolean depth_invert     = params[NF_DEPTH_INVERT]->u.bd.value;
    float     depth_strength   = float(params[NF_DEPTH_STRENGTH]->u.fs_d.value);

    A_Boolean enable_shading     = params[NF_SHADING_ENABLE]->u.bd.value;
    float     diffuse_amount     = float(params[NF_DIFFUSE]->u.fs_d.value);
    PF_Pixel8 diffuse_color      = params[NF_DIFFUSE_COLOR]->u.cd.value;
    PF_Pixel8 ambient_color      = params[NF_AMBIENT]->u.cd.value;
    PF_Pixel8 incandescence_color = params[NF_INCANDESCENCE]->u.cd.value;

    A_Boolean enable_specular = params[NF_SPECULAR_ENABLE]->u.bd.value;
    float     specular_amount = float(params[NF_SPECULAR]->u.fs_d.value);
    float     specular_spread = float(params[NF_SPECULAR_SPREAD]->u.fs_d.value);
    PF_Pixel8 specular_color  = params[NF_SPECULAR_COLOR]->u.cd.value;
    A_long    specular_blend  = params[NF_SPECULAR_BLEND]->u.pd.value;

    A_Boolean enable_incidence  = params[NF_INCIDENCE_ENABLE]->u.bd.value;
    float     incidence_amount  = float(params[NF_INCIDENCE]->u.fs_d.value);
    float     incidence_falloff = float(params[NF_INCIDENCE_FALLOFF]->u.fs_d.value);
    PF_Pixel8 incidence_color   = params[NF_INCIDENCE_COLOR]->u.cd.value;
    A_long    incidence_blend   = params[NF_INCIDENCE_BLEND]->u.pd.value;

    A_Boolean enable_rim    = params[NF_RIM_ENABLE]->u.bd.value;
    float     rim_amount    = float(params[NF_RIM]->u.fs_d.value);
    float     rim_size      = float(params[NF_RIM_SIZE]->u.fs_d.value);
    float     rim_width     = float(params[NF_RIM_WIDTH]->u.fs_d.value);
    PF_Pixel8 rim_color     = params[NF_RIM_COLOR]->u.cd.value;
    float     rim_angle_deg = float(params[NF_RIM_ANGLE]->u.ad.value) / 65536.0f;
    A_long    rim_blend     = params[NF_RIM_BLEND]->u.pd.value;

    A_Boolean enable_gradient = params[NF_GRADIENT_ENABLE]->u.bd.value;
    float     gradient_amount = float(params[NF_GRADIENT]->u.fs_d.value);
    PF_Pixel8 gradient_color  = params[NF_GRADIENT_COLOR]->u.cd.value;
    A_long    gradient_blend  = params[NF_GRADIENT_BLEND]->u.pd.value;

    A_Boolean enable_matcap   = params[NF_MATCAP_ENABLE]->u.bd.value;
    float     matcap_amount   = float(params[NF_MATCAP]->u.fs_d.value);
    A_long    matcap_offset_x = params[NF_MATCAP_OFFSET]->u.td.x_value >> 16;
    A_long    matcap_offset_y = params[NF_MATCAP_OFFSET]->u.td.y_value >> 16;
    A_long    matcap_mode     = params[NF_MATCAP_MODE]->u.pd.value;
    A_long    matcap_blur     = params[NF_MATCAP_BLUR]->u.sd.value;
    A_long    matcap_blend    = params[NF_MATCAP_BLEND]->u.pd.value;

    A_Boolean enable_reflection         = params[NF_REFLECTION_ENABLE]->u.bd.value;
    float     reflection_amount         = float(params[NF_REFLECTION]->u.fs_d.value);
    A_long    reflection_env_mode       = params[NF_REFLECTION_ENV_MODE]->u.pd.value;
    float     reflection_gamma          = float(params[NF_REFLECTION_GAMMA]->u.fs_d.value);
    A_long    reflection_tile           = params[NF_REFLECTION_TILE_MODE]->u.pd.value;
    A_long    reflection_blur           = params[NF_REFLECTION_BLUR]->u.sd.value;
    PF_Pixel8 reflection_color          = params[NF_REFLECTION_COLOR]->u.cd.value;
    float     reflection_inclination_deg = float(params[NF_REFLECTION_INCLINATION]->u.ad.value) / 65536.0f;
    float     reflection_azimuth_deg    = float(params[NF_REFLECTION_AZIMUTH]->u.ad.value) / 65536.0f;
    float     reflection_scale          = float(params[NF_REFLECTION_SCALE]->u.fs_d.value);
    float     reflection_ior            = float(params[NF_REFLECTION_IOR]->u.fs_d.value);
    float     reflection_fresnel        = float(params[NF_REFLECTION_FRESNEL]->u.fs_d.value);
    float     reflection_fresnel_depth  = float(params[NF_REFLECTION_FRESNEL_DEPTH]->u.fs_d.value);
    A_long    reflection_blend          = params[NF_REFLECTION_BLEND]->u.pd.value;

    A_Boolean enable_refraction          = params[NF_REFRACTION_ENABLE]->u.bd.value;
    float     refraction_amount          = float(params[NF_REFRACTION]->u.fs_d.value);
    float     refraction_ior             = float(params[NF_REFRACTION_IOR]->u.fs_d.value);
    float     refraction_strength        = float(params[NF_REFRACTION_STRENGTH]->u.fs_d.value);
    A_Boolean refraction_use_depth       = params[NF_REFRACTION_USE_DEPTH]->u.bd.value;
    float     refraction_gamma           = float(params[NF_REFRACTION_GAMMA]->u.fs_d.value);
    A_long    refraction_tile            = params[NF_REFRACTION_TILE_MODE]->u.pd.value;
    A_long    refraction_blur             = params[NF_REFRACTION_BLUR]->u.sd.value;
    A_long    refraction_offset_x        = params[NF_REFRACTION_OFFSET]->u.td.x_value >> 16;
    A_long    refraction_offset_y        = params[NF_REFRACTION_OFFSET]->u.td.y_value >> 16;
    float     refraction_scale           = float(params[NF_REFRACTION_SCALE]->u.fs_d.value);
    PF_Pixel8 refraction_color           = params[NF_REFRACTION_COLOR]->u.cd.value;
    float     refraction_fresnel         = float(params[NF_REFRACTION_FRESNEL]->u.fs_d.value);
    float     refraction_fresnel_depth   = float(params[NF_REFRACTION_FRESNEL_DEPTH]->u.fs_d.value);
    A_long    refraction_blend           = params[NF_REFRACTION_BLEND]->u.pd.value;

    A_Boolean enable_texture     = params[NF_TEXTURE_ENABLE]->u.bd.value;
    float     texture_amount     = float(params[NF_TEXTURE]->u.fs_d.value);
    A_long    texture_tile       = params[NF_TEXTURE_TILE_MODE]->u.pd.value;
    A_long    texture_uv_mirror  = params[NF_TEXTURE_UV_MIRROR]->u.pd.value;
    A_long    texture_filter     = params[NF_TEXTURE_FILTER]->u.pd.value;
    A_long    texture_offset_x   = params[NF_TEXTURE_OFFSET]->u.td.x_value >> 16;
    A_long    texture_offset_y   = params[NF_TEXTURE_OFFSET]->u.td.y_value >> 16;
    A_long    texture_blend      = params[NF_TEXTURE_BLEND]->u.pd.value;

    A_Boolean enable_bump      = params[NF_BUMP_ENABLE]->u.bd.value;
    float     bump_intensity   = float(params[NF_BUMP_INTENSITY]->u.fs_d.value);
    A_long    bump_tile        = params[NF_BUMP_TILE_MODE]->u.pd.value;
    A_long    bump_filter      = params[NF_BUMP_FILTER]->u.pd.value;
    A_long    bump_offset_x    = params[NF_BUMP_OFFSET]->u.td.x_value >> 16;
    A_long    bump_offset_y    = params[NF_BUMP_OFFSET]->u.td.y_value >> 16;
    float     bump_scale       = float(params[NF_BUMP_SCALE]->u.fs_d.value);

    A_long shading_blend         = params[NF_SHADING_BLEND]->u.pd.value;
    float  shading_blend_amount  = float(params[NF_SHADING_BLEND_AMOUNT]->u.fs_d.value);

    A_Boolean enable_toon          = params[NF_TOON_ENABLE]->u.bd.value;
    float     toon_amount          = float(params[NF_TOON]->u.fs_d.value);
    float     toon_smoothing       = float(params[NF_TOON_SMOOTHING]->u.fs_d.value);
    PF_Pixel8 toon_shadow_color    = params[NF_TOON_SHADOW_COLOR]->u.cd.value;
    float     toon_shadow_intensity = float(params[NF_TOON_SHADOW_INTENSITY]->u.fs_d.value);
    float     toon_shadow_width    = float(params[NF_TOON_SHADOW_WIDTH]->u.fs_d.value);
    PF_Pixel8 toon_mid_color       = params[NF_TOON_MID_COLOR]->u.cd.value;
    float     toon_highlight_width = float(params[NF_TOON_HIGHLIGHT_WIDTH]->u.fs_d.value);
    PF_Pixel8 toon_highlight_color = params[NF_TOON_HIGHLIGHT_COLOR]->u.cd.value;
    float     toon_highlight_intensity = float(params[NF_TOON_HIGHLIGHT_INTENSITY]->u.fs_d.value);
    A_long    toon_blend           = params[NF_TOON_BLEND]->u.pd.value;

    // ---- Pre Blur preprocess: build separable 3-pass-box-blurred buffers ----
    // Cost is O(1) per pixel regardless of radius. Buffers hold pre-renormalize
    // [-1,1] normals (and alpha when blur_alpha is set).
    std::vector<float> blur_nx_v, blur_ny_v, blur_nz_v, blur_a_v;
    const std::vector<float> *blur_nx_ptr = nullptr;
    const std::vector<float> *blur_ny_ptr = nullptr;
    const std::vector<float> *blur_nz_ptr = nullptr;
    const std::vector<float> *blur_a_ptr  = nullptr;
    if (pre_blur_radius > 0 && nm && nm->data &&
        nm->width > 0 && nm->height > 0)
    {
        bool include_alpha = (use_normal_alpha && pre_blur_alpha);
        PreBlurNormalMapT<Pixel>(nm, pre_blur_radius,
                                 chan_x, chan_y, chan_z,
                                 inv_x, inv_y, inv_z,
                                 include_alpha,
                                 blur_nx_v, blur_ny_v, blur_nz_v, blur_a_v);
        blur_nx_ptr = &blur_nx_v;
        blur_ny_ptr = &blur_ny_v;
        blur_nz_ptr = &blur_nz_v;
        if (include_alpha) blur_a_ptr = &blur_a_v;
    }

    // ---- Depth preprocess: build per-output-pixel Z buffer (Strength + Invert applied). ----
    std::vector<float> depth_buf_v;
    const std::vector<float> *depth_buf_ptr = nullptr;
    if (depth_ld && depth_ld->data &&
        depth_ld->width > 0 && depth_ld->height > 0 &&
        depth_strength != 0.0f)
    {
        PreprocessDepthMapT<Pixel>(depth_ld, output->width, output->height,
                                   depth_invert, depth_strength, depth_buf_v);
        depth_buf_ptr = &depth_buf_v;
    }

    // ---- Map blur preprocess (Matcap / Reflection / Refraction) ----
    // separable 3-pass running sum, per-pixel O(1). Applied at sampling time via TileWrap.
    std::vector<float> mc_blur_r, mc_blur_g, mc_blur_b;
    const std::vector<float> *mc_blur_r_ptr = nullptr, *mc_blur_g_ptr = nullptr, *mc_blur_b_ptr = nullptr;
    if (enable_matcap && matcap_ld && matcap_ld->data &&
        matcap_blur > 0 && matcap_ld->width > 0 && matcap_ld->height > 0) {
        A_long w_, h_;
        BlurLayerRGBT<Pixel>(matcap_ld, matcap_blur, w_, h_, mc_blur_r, mc_blur_g, mc_blur_b);
        mc_blur_r_ptr = &mc_blur_r;
        mc_blur_g_ptr = &mc_blur_g;
        mc_blur_b_ptr = &mc_blur_b;
    }

    std::vector<float> rfl_blur_r, rfl_blur_g, rfl_blur_b;
    const std::vector<float> *rfl_blur_r_ptr = nullptr, *rfl_blur_g_ptr = nullptr, *rfl_blur_b_ptr = nullptr;
    if (enable_reflection && env_map_ld && env_map_ld->data &&
        reflection_blur > 0 && env_map_ld->width > 0 && env_map_ld->height > 0) {
        A_long w_, h_;
        BlurLayerRGBT<Pixel>(env_map_ld, reflection_blur, w_, h_, rfl_blur_r, rfl_blur_g, rfl_blur_b);
        rfl_blur_r_ptr = &rfl_blur_r;
        rfl_blur_g_ptr = &rfl_blur_g;
        rfl_blur_b_ptr = &rfl_blur_b;
    }

    std::vector<float> rfr_blur_r, rfr_blur_g, rfr_blur_b;
    const std::vector<float> *rfr_blur_r_ptr = nullptr, *rfr_blur_g_ptr = nullptr, *rfr_blur_b_ptr = nullptr;
    if (enable_refraction && refr_map_ld && refr_map_ld->data &&
        refraction_blur > 0 && refr_map_ld->width > 0 && refr_map_ld->height > 0) {
        A_long w_, h_;
        BlurLayerRGBT<Pixel>(refr_map_ld, refraction_blur, w_, h_, rfr_blur_r, rfr_blur_g, rfr_blur_b);
        rfr_blur_r_ptr = &rfr_blur_r;
        rfr_blur_g_ptr = &rfr_blur_g;
        rfr_blur_b_ptr = &rfr_blur_b;
    }

    if (show_normals) {
        if (nm->data) {
            if (!err) {
                err = RenderNormalsT<Pixel>(in_data, nm, output,
                                     chan_x, chan_y, chan_z,
                                     inv_x, inv_y, inv_z,
                                     normal_strength,
                                     display,
                                     blur_nx_ptr, blur_ny_ptr, blur_nz_ptr,
                                     depth_buf_ptr, depth_strength);
            }
        } else {
            A_long ri, ci;
            for (ri = 0; ri < output->height; ++ri) {
                Pixel *p = reinterpret_cast<Pixel *>(
                    reinterpret_cast<char *>(output->data) + ri * output->rowbytes);
                for (ci = 0; ci < output->width; ++ci) {
                    Write(p[ci], 0.0f, 0.0f, 0.0f);
                }
            }
        }
    } else if (show_shading) {
        PF_LayerDef *input_ld = input_world;
        PF_Err s_err = RenderShadingT<Pixel>(
            in_data, input_ld, nm, output,
            chan_x, chan_y, chan_z,
            inv_x, inv_y, inv_z,
            use_normal_alpha,
            normal_strength,
            blur_nx_ptr, blur_ny_ptr, blur_nz_ptr, blur_a_ptr,
            depth_buf_ptr,
            lights, light_count, display,
            enable_shading, diffuse_amount, diffuse_color, ambient_color, ao_ld, incandescence_color,
            shading_blend, shading_blend_amount,
            enable_specular, spec_ld, specular_amount, specular_spread, specular_color, specular_blend,
            falloff_mode, falloff_val, exposure, gamma_val,
            enable_incidence, incidence_amount, incidence_falloff, incidence_color, incidence_blend,
            enable_rim, rim_amount, rim_size, rim_width, rim_color, rim_angle_deg, rim_blend,
            enable_gradient, gradient_amount, gradient_color, gradient_blend,
            enable_toon, toon_amount, toon_smoothing,
            toon_shadow_color, toon_shadow_intensity, toon_shadow_width,
            toon_mid_color, toon_highlight_width,
            toon_highlight_color, toon_highlight_intensity, toon_blend,
            enable_matcap, matcap_ld, matcap_amount,
            matcap_offset_x, matcap_offset_y,
            matcap_mode, matcap_blur, matcap_blend,
            mc_blur_r_ptr, mc_blur_g_ptr, mc_blur_b_ptr,
            enable_bump, bump_ld, bump_intensity,
            bump_tile, bump_filter, bump_offset_x, bump_offset_y, bump_scale,
            enable_texture, tex_uv_ld, tex_map_ld, texture_amount,
            texture_tile, texture_uv_mirror, texture_filter,
            texture_offset_x, texture_offset_y, texture_blend,
            enable_reflection, env_map_ld, reflection_amount,
            reflection_env_mode, reflection_gamma, reflection_tile,
            reflection_blur, reflection_color,
            reflection_inclination_deg, reflection_azimuth_deg,
            reflection_scale, reflection_ior, reflection_fresnel,
            reflection_fresnel_depth, reflection_blend,
            rfl_blur_r_ptr, rfl_blur_g_ptr, rfl_blur_b_ptr,
            enable_refraction, refr_map_ld, refraction_amount,
            refraction_ior, refraction_strength, refraction_use_depth,
            refraction_gamma, refraction_tile,
            refraction_blur,
            refraction_offset_x, refraction_offset_y, refraction_scale,
            refraction_color, refraction_fresnel,
            refraction_fresnel_depth, refraction_blend,
            rfr_blur_r_ptr, rfr_blur_g_ptr, rfr_blur_b_ptr);
        if (!err) err = s_err;
    }

    PF_CHECKIN_PARAM(in_data, &nm_param);
    if (depth_ld)   PF_CHECKIN_PARAM(in_data, &depth_param);
    if (matcap_ld)  PF_CHECKIN_PARAM(in_data, &matcap_param);
    if (bump_ld)    PF_CHECKIN_PARAM(in_data, &bump_param);
    if (env_map_ld)  PF_CHECKIN_PARAM(in_data, &env_map_param);
    if (refr_map_ld) PF_CHECKIN_PARAM(in_data, &refr_map_param);
    if (tex_uv_ld)   PF_CHECKIN_PARAM(in_data, &tex_uv_param);
    if (tex_map_ld)  PF_CHECKIN_PARAM(in_data, &tex_map_param);
    if (ao_ld)       PF_CHECKIN_PARAM(in_data, &ao_map_param);
    if (spec_ld)     PF_CHECKIN_PARAM(in_data, &spec_map_param);
    return err;
}

// ---- SmartFX handlers (Step 14b-α: 8 bpc only; other depths pass-through) ----

static inline void NF_UnionLRect(const PF_LRect *src, PF_LRect *dst)
{
    if (src->left   < dst->left)   dst->left   = src->left;
    if (src->top    < dst->top)    dst->top    = src->top;
    if (src->right  > dst->right)  dst->right  = src->right;
    if (src->bottom > dst->bottom) dst->bottom = src->bottom;
}

// Delete callback for pre_render_data; AE invokes this when the cached
// data is no longer needed. We malloc'd the NormForgeLightCache, so free it.
static void
NF_DeleteLightCache(void *pre_render_data)
{
    if (pre_render_data) free(pre_render_data);
}

static PF_Err
SmartPreRender(PF_InData *in_data, PF_OutData *out_data, void *extra_void)
{
    PF_Err err = PF_Err_NONE;
    PF_PreRenderExtra *extra = static_cast<PF_PreRenderExtra *>(extra_void);
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    err = extra->cb->checkout_layer(in_data->effect_ref,
                                    NF_INPUT, NF_INPUT,
                                    &req,
                                    in_data->current_time,
                                    in_data->time_step,
                                    in_data->time_scale,
                                    &in_result);

    // SmartFX requires checkout_layer in PreRender for any auxiliary layer
    // that SmartRender will read. Normal Map (G3-a), AO Map / Specular Map
    // (G4-a). All best-effort - SmartRenderGPU falls back gracefully if any
    // layer is unconnected.
    PF_CheckoutResult aux_result;
    AEFX_CLR_STRUCT(aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_NORMAL_MAP, NF_NORMAL_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_AO_MAP, NF_AO_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_SPECULAR_MAP, NF_SPECULAR_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_BUMP_MAP, NF_BUMP_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_TEXTURE_UV_MAP, NF_TEXTURE_UV_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_TEXTURE_MAP, NF_TEXTURE_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_MATCAP_MAP, NF_MATCAP_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_REFLECTION_ENV_MAP, NF_REFLECTION_ENV_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_REFRACTION_MAP, NF_REFRACTION_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);
    extra->cb->checkout_layer(in_data->effect_ref,
                              NF_DEPTH_MAP, NF_DEPTH_MAP,
                              &req,
                              in_data->current_time,
                              in_data->time_step,
                              in_data->time_scale,
                              &aux_result);

    if (!err) {
        NF_UnionLRect(&in_result.result_rect, &extra->output->result_rect);
        NF_UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);
        extra->output->solid = FALSE;
        extra->output->flags = 0;

        // Step G2 (0.6.1): gate GPU path on the Render Mode popup.
        //   Auto / GPU only -> advertise GPU_RENDER_POSSIBLE for 32bpc
        //   CPU only        -> never advertise (force CPU)
        // 8/16bpc are always CPU (AE SDK GPU path is 32bpc only).
        A_long nf_render_mode = RENDER_MODE_AUTO;
        A_long nf_display     = DISPLAY_ALL;
        {
            PF_ParamDef rm_param;
            AEFX_CLR_STRUCT(rm_param);
            PF_Err rm_err = PF_CHECKOUT_PARAM(in_data, NF_RENDER_MODE,
                                              in_data->current_time,
                                              in_data->time_step,
                                              in_data->time_scale,
                                              &rm_param);
            if (!rm_err) {
                nf_render_mode = rm_param.u.pd.value;
                PF_CHECKIN_PARAM(in_data, &rm_param);
            }

            PF_ParamDef disp_param;
            AEFX_CLR_STRUCT(disp_param);
            PF_Err disp_err = PF_CHECKOUT_PARAM(in_data, NF_DISPLAY,
                                                in_data->current_time,
                                                in_data->time_step,
                                                in_data->time_scale,
                                                &disp_param);
            if (!disp_err) {
                nf_display = disp_param.u.pd.value;
                PF_CHECKIN_PARAM(in_data, &disp_param);
            }
        }

        // Step G4-a (0.6.5) / G4-b (0.6.6) / G5-a (0.6.8): the GPU shading
        // kernel handles a growing subset of Display modes. Modes still on
        // CPU: NORMALS / DEPTH / MATCAP / REFLECTION / REFRACTION / TEXTURE.
        // Update the supported set as G5 sub-steps add coverage.
        const bool gpu_supports_display =
            (nf_display == DISPLAY_DIFFUSE   ||
             nf_display == DISPLAY_SPECULAR  ||
             nf_display == DISPLAY_INCIDENCE ||
             nf_display == DISPLAY_RIM       ||
             nf_display == DISPLAY_TOON      ||
             nf_display == DISPLAY_GRADIENT  ||
             nf_display == DISPLAY_BUMP      ||
             nf_display == DISPLAY_TEXTURE   ||
             nf_display == DISPLAY_MATCAP    ||
             nf_display == DISPLAY_REFLECTION ||
             nf_display == DISPLAY_REFRACTION ||
             nf_display == DISPLAY_OFF       ||
             nf_display == DISPLAY_ALL);

        const bool gpu_allowed = (nf_render_mode != RENDER_MODE_CPU);
        if (extra->input->bitdepth == 32 && gpu_allowed && gpu_supports_display) {
            extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
        }
    }

    // ---- Pattern D (0.5.13): per-frame light snapshot for MFR safety ----
    // Collect lights on this per-frame thread (PreRender is typically 1 thread
    // per frame, much safer for AEGP suite calls than the per-pixel-heavy
    // SmartRender threads). Pass the snapshot to SmartRender via pre_render_data.
    extra->output->pre_render_data = NULL;
    extra->output->delete_pre_render_data_func = &NF_DeleteLightCache;

    if (!err) {
        PF_ParamDef render_mode_param;
        AEFX_CLR_STRUCT(render_mode_param);
        PF_ParamDef enable_invis_param;
        AEFX_CLR_STRUCT(enable_invis_param);

        PF_Err co_err1 = PF_CHECKOUT_PARAM(in_data, NF_LIGHTS_RENDER,
                                           in_data->current_time,
                                           in_data->time_step,
                                           in_data->time_scale,
                                           &render_mode_param);
        PF_Err co_err2 = PF_CHECKOUT_PARAM(in_data, NF_LIGHTS_INVISIBLE,
                                           in_data->current_time,
                                           in_data->time_step,
                                           in_data->time_scale,
                                           &enable_invis_param);

        if (!co_err1 && !co_err2) {
            A_long    render_mode  = render_mode_param.u.pd.value;
            A_Boolean enable_invis = enable_invis_param.u.bd.value;

            NormForgeLightCache *cache =
                (NormForgeLightCache *)malloc(sizeof(NormForgeLightCache));
            if (cache) {
                memset(cache, 0, sizeof(*cache));
                CollectLights(in_data, cache->lights, &cache->light_count,
                              render_mode, enable_invis);
                extra->output->pre_render_data = cache;
            }
        }

        if (!co_err1) PF_CHECKIN_PARAM(in_data, &render_mode_param);
        if (!co_err2) PF_CHECKIN_PARAM(in_data, &enable_invis_param);
    }

    return err;
}

static void NF_PassThroughCopy(PF_EffectWorld *src, PF_EffectWorld *dst, A_long bytes_per_pixel)
{
    A_long h = std::min(src->height, dst->height);
    A_long w = std::min(src->width, dst->width);
    A_long copy_bytes = w * bytes_per_pixel;
    for (A_long y = 0; y < h; ++y) {
        char *src_row = reinterpret_cast<char *>(src->data) + y * src->rowbytes;
        char *dst_row = reinterpret_cast<char *>(dst->data) + y * dst->rowbytes;
        memcpy(dst_row, src_row, copy_bytes);
    }
}

static PF_Err
SmartRenderCPU(PF_InData *in_data, PF_OutData *out_data,
               PF_SmartRenderExtra *extra,
               PF_EffectWorld *input_world, PF_EffectWorld *output_world)
{
    PF_Err err = PF_Err_NONE;

    A_long depth = extra->input->bitdepth;
    if (depth != 8 && depth != 16 && depth != 32) {
        A_long bpp = (depth == 32) ? 16 : (depth == 16) ? 8 : 4;
        NF_PassThroughCopy(input_world, output_world, bpp);
        return err;
    }

    PF_ParamDef  param_defs[NF_NUM_PARAMS] = {};
    PF_ParamDef *params[NF_NUM_PARAMS];
    for (A_long i = 0; i < NF_NUM_PARAMS; ++i) {
        AEFX_CLR_STRUCT(param_defs[i]);
        params[i] = &param_defs[i];
    }

    for (A_long i = 1; i < NF_NUM_PARAMS; ++i) {
        if (i == NF_NORMAL_MAP   || i == NF_DEPTH_MAP   ||
            i == NF_MATCAP_MAP   || i == NF_TEXTURE_UV_MAP ||
            i == NF_TEXTURE_MAP  || i == NF_BUMP_MAP    ||
            i == NF_AO_MAP       || i == NF_SPECULAR_MAP) continue;
        PF_CHECKOUT_PARAM(in_data, i,
                          in_data->current_time,
                          in_data->time_step,
                          in_data->time_scale,
                          &param_defs[i]);
    }

    // Pattern D (0.5.13): retrieve per-frame light snapshot built in SmartPreRender.
    // SmartRender threads no longer call AEGP suites; they read this cache instead.
    const NormForgeLightCache *light_cache =
        (const NormForgeLightCache *)extra->input->pre_render_data;

    if (depth == 8) {
        err = RenderT<PF_Pixel8>(in_data, out_data, params, output_world, input_world, light_cache);
    } else if (depth == 16) {
        err = RenderT<PF_Pixel16>(in_data, out_data, params, output_world, input_world, light_cache);
    } else { // depth == 32
        err = RenderT<PF_PixelFloat>(in_data, out_data, params, output_world, input_world, light_cache);
    }

    for (A_long i = 1; i < NF_NUM_PARAMS; ++i) {
        if (i == NF_NORMAL_MAP   || i == NF_DEPTH_MAP   ||
            i == NF_MATCAP_MAP   || i == NF_TEXTURE_UV_MAP ||
            i == NF_TEXTURE_MAP  || i == NF_BUMP_MAP    ||
            i == NF_AO_MAP       || i == NF_SPECULAR_MAP) continue;
        PF_CHECKIN_PARAM(in_data, &param_defs[i]);
    }

    return err;
}

// Step G4-a (0.6.5): GPU Shading rendering. Covers Normal Map decode (channel/
// invert/strength/alpha), Diffuse, Specular (with Specular Map), Incidence,
// AO Map, Display = DIFFUSE / SPECULAR / INCIDENCE / ALL / OFF. Other display
// modes are gated off in SmartPreRender so AE falls back to CPU. Rim / Toon /
// Gradient and full Blend modes (30 kinds) arrive in G4-b / G4-c.
static PF_Err
SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
               PF_SmartRenderExtra *extra,
               PF_EffectWorld *input_world, PF_EffectWorld *output_world)
{
    PF_Err err = PF_Err_NONE;

#ifdef HAS_CUDA
    if (extra->input->what_gpu != PF_GPU_Framework_CUDA) {
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu_suite =
        AEFX_SuiteScoper<PF_GPUDeviceSuite1>(in_data,
                                             kPFGPUDeviceSuite,
                                             kPFGPUDeviceSuiteVersion1,
                                             out_data);

    AEFX_SuiteScoper<PF_WorldSuite2> world_suite =
        AEFX_SuiteScoper<PF_WorldSuite2>(in_data,
                                         kPFWorldSuite,
                                         kPFWorldSuiteVersion2,
                                         out_data);

    PF_PixelFormat pixel_format = PF_PixelFormat_INVALID;
    ERR(world_suite->PF_GetPixelFormat(input_world, &pixel_format));
    if (!err && pixel_format != PF_PixelFormat_GPU_BGRA128) {
        err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }

    // Auxiliary layers as GPU worlds (any of these may be NULL = not connected)
    auto checkout_aux = [&](A_long idx) -> PF_EffectWorld * {
        PF_EffectWorld *w = NULL;
        PF_Err e = extra->cb->checkout_layer_pixels(in_data->effect_ref, idx, &w);
        return (e == PF_Err_NONE) ? w : NULL;
    };
    PF_EffectWorld *normal_world  = checkout_aux(NF_NORMAL_MAP);
    PF_EffectWorld *ao_world      = checkout_aux(NF_AO_MAP);
    PF_EffectWorld *specmap_world = checkout_aux(NF_SPECULAR_MAP);
    PF_EffectWorld *bump_world    = checkout_aux(NF_BUMP_MAP);
    PF_EffectWorld *uv_world      = checkout_aux(NF_TEXTURE_UV_MAP);
    PF_EffectWorld *tex_world     = checkout_aux(NF_TEXTURE_MAP);
    PF_EffectWorld *matcap_world  = checkout_aux(NF_MATCAP_MAP);
    PF_EffectWorld *env_world     = checkout_aux(NF_REFLECTION_ENV_MAP);
    PF_EffectWorld *refr_world    = checkout_aux(NF_REFRACTION_MAP);
    PF_EffectWorld *depth_world   = checkout_aux(NF_DEPTH_MAP);

    // Resolve GPU memory pointers
    auto get_gpu_data = [&](PF_EffectWorld *w) -> void * {
        if (!w) return NULL;
        void *m = NULL;
        PF_Err e = gpu_suite->GetGPUWorldData(in_data->effect_ref, w, &m);
        return (e == PF_Err_NONE) ? m : NULL;
    };
    void *src_mem     = NULL;
    void *dst_mem     = NULL;
    if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, input_world,  &src_mem));
    if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, output_world, &dst_mem));
    void *nm_mem      = !err ? get_gpu_data(normal_world)  : NULL;

    // Phase 6-1 (0.7.0): If Normal Map is None / unconnected on the GPU path,
    // fall back to the input layer. On GPU, normal_world->data is host-side
    // NULL by design (G3-a learning), so we gate on nm_mem and normal_world
    // pointer itself rather than ->data.
    if (!nm_mem || !normal_world) {
        nm_mem       = src_mem;
        normal_world = input_world;
    }

    void *ao_mem      = !err ? get_gpu_data(ao_world)      : NULL;
    void *specmap_mem = !err ? get_gpu_data(specmap_world) : NULL;
    void *bump_mem    = !err ? get_gpu_data(bump_world)    : NULL;
    void *uv_mem      = !err ? get_gpu_data(uv_world)      : NULL;
    void *tex_mem     = !err ? get_gpu_data(tex_world)     : NULL;
    void *matcap_mem  = !err ? get_gpu_data(matcap_world)  : NULL;
    void *env_mem     = !err ? get_gpu_data(env_world)     : NULL;
    void *refr_mem    = !err ? get_gpu_data(refr_world)    : NULL;
    void *depth_mem   = !err ? get_gpu_data(depth_world)   : NULL;

    // Checkout all non-layer params via the same loop pattern as
    // SmartRenderCPU. Cheaper to mirror the CPU bookkeeping than maintain
    // a hand-curated subset.
    PF_ParamDef  param_defs[NF_NUM_PARAMS] = {};
    PF_ParamDef *params[NF_NUM_PARAMS];
    for (A_long i = 0; i < NF_NUM_PARAMS; ++i) {
        AEFX_CLR_STRUCT(param_defs[i]);
        params[i] = &param_defs[i];
    }
    auto is_layer_param = [](A_long i) -> bool {
        return (i == NF_NORMAL_MAP   || i == NF_DEPTH_MAP   ||
                i == NF_MATCAP_MAP   || i == NF_TEXTURE_UV_MAP ||
                i == NF_TEXTURE_MAP  || i == NF_BUMP_MAP    ||
                i == NF_AO_MAP       || i == NF_SPECULAR_MAP ||
                i == NF_REFLECTION_ENV_MAP || i == NF_REFRACTION_MAP);
    };
    for (A_long i = 1; i < NF_NUM_PARAMS; ++i) {
        if (is_layer_param(i)) continue;
        PF_CHECKOUT_PARAM(in_data, i,
                          in_data->current_time,
                          in_data->time_step,
                          in_data->time_scale,
                          &param_defs[i]);
    }

    if (!err && src_mem && dst_mem) {
        // ---- Translate params into kernel POD ----
        NF_ShadingParams sp;
        memset(&sp, 0, sizeof(sp));

        sp.width      = (int)output_world->width;
        sp.height     = (int)output_world->height;
        sp.src_pitch  = (int)(input_world->rowbytes  / 16);
        sp.dst_pitch  = (int)(output_world->rowbytes / 16);

        sp.has_nm     = (nm_mem && normal_world) ? 1 : 0;
        sp.nm_pitch   = sp.has_nm ? (int)(normal_world->rowbytes / 16) : 0;
        sp.nm_width   = sp.has_nm ? (int)normal_world->width  : 0;
        sp.nm_height  = sp.has_nm ? (int)normal_world->height : 0;

        sp.has_ao     = (ao_mem && ao_world) ? 1 : 0;
        sp.ao_pitch   = sp.has_ao ? (int)(ao_world->rowbytes / 16) : 0;
        sp.ao_width   = sp.has_ao ? (int)ao_world->width  : 0;
        sp.ao_height  = sp.has_ao ? (int)ao_world->height : 0;

        sp.has_specmap     = (specmap_mem && specmap_world) ? 1 : 0;
        sp.specmap_pitch   = sp.has_specmap ? (int)(specmap_world->rowbytes / 16) : 0;
        sp.specmap_width   = sp.has_specmap ? (int)specmap_world->width  : 0;
        sp.specmap_height  = sp.has_specmap ? (int)specmap_world->height : 0;

        sp.has_bump_map    = (bump_mem && bump_world) ? 1 : 0;
        sp.bump_pitch      = sp.has_bump_map ? (int)(bump_world->rowbytes / 16) : 0;
        sp.bump_width      = sp.has_bump_map ? (int)bump_world->width  : 0;
        sp.bump_height     = sp.has_bump_map ? (int)bump_world->height : 0;

        sp.has_uv_map      = (uv_mem && uv_world) ? 1 : 0;
        sp.uv_pitch        = sp.has_uv_map ? (int)(uv_world->rowbytes / 16) : 0;
        sp.uv_width        = sp.has_uv_map ? (int)uv_world->width  : 0;
        sp.uv_height       = sp.has_uv_map ? (int)uv_world->height : 0;

        sp.has_tex_map     = (tex_mem && tex_world) ? 1 : 0;
        sp.tex_pitch       = sp.has_tex_map ? (int)(tex_world->rowbytes / 16) : 0;
        sp.tex_width       = sp.has_tex_map ? (int)tex_world->width  : 0;
        sp.tex_height      = sp.has_tex_map ? (int)tex_world->height : 0;

        sp.has_matcap      = (matcap_mem && matcap_world) ? 1 : 0;
        sp.matcap_pitch    = sp.has_matcap ? (int)(matcap_world->rowbytes / 16) : 0;
        sp.matcap_width    = sp.has_matcap ? (int)matcap_world->width  : 0;
        sp.matcap_height   = sp.has_matcap ? (int)matcap_world->height : 0;

        sp.has_envmap      = (env_mem && env_world) ? 1 : 0;
        sp.env_pitch       = sp.has_envmap ? (int)(env_world->rowbytes / 16) : 0;
        sp.env_width       = sp.has_envmap ? (int)env_world->width  : 0;
        sp.env_height      = sp.has_envmap ? (int)env_world->height : 0;

        sp.has_refrmap     = (refr_mem && refr_world) ? 1 : 0;
        sp.refr_pitch      = sp.has_refrmap ? (int)(refr_world->rowbytes / 16) : 0;
        sp.refr_width      = sp.has_refrmap ? (int)refr_world->width  : 0;
        sp.refr_height     = sp.has_refrmap ? (int)refr_world->height : 0;

        sp.has_depthmap    = (depth_mem && depth_world) ? 1 : 0;
        sp.depth_pitch     = sp.has_depthmap ? (int)(depth_world->rowbytes / 16) : 0;
        sp.depth_width     = sp.has_depthmap ? (int)depth_world->width  : 0;
        sp.depth_height    = sp.has_depthmap ? (int)depth_world->height : 0;

        sp.display = (int)params[NF_DISPLAY]->u.pd.value;

        // Normal Map decode config
        sp.chan_x = (int)params[NF_CHANNEL_X]->u.pd.value;
        sp.chan_y = (int)params[NF_CHANNEL_Y]->u.pd.value;
        sp.chan_z = (int)params[NF_CHANNEL_Z]->u.pd.value;
        sp.inv_x  = params[NF_INVERT_X]->u.bd.value ? 1 : 0;
        sp.inv_y  = params[NF_INVERT_Y]->u.bd.value ? 1 : 0;
        sp.inv_z  = params[NF_INVERT_Z]->u.bd.value ? 1 : 0;
        sp.normal_strength   = (float)params[NF_NORMAL_STRENGTH]->u.fs_d.value;
        sp.use_normal_alpha  = params[NF_USE_NORMAL_ALPHA]->u.bd.value ? 1 : 0;

        // Shading
        sp.enable_shading = params[NF_SHADING_ENABLE]->u.bd.value ? 1 : 0;
        sp.diffuse_amount = (float)params[NF_DIFFUSE]->u.fs_d.value;
        PF_Pixel8 dc = params[NF_DIFFUSE_COLOR]->u.cd.value;
        PF_Pixel8 ac = params[NF_AMBIENT]->u.cd.value;
        PF_Pixel8 ic = params[NF_INCANDESCENCE]->u.cd.value;
        sp.diff_r = dc.red / 255.0f; sp.diff_g = dc.green / 255.0f; sp.diff_b = dc.blue / 255.0f;
        sp.amb_r  = ac.red / 255.0f; sp.amb_g  = ac.green / 255.0f; sp.amb_b  = ac.blue / 255.0f;
        sp.ainc_r = sp.amb_r * (ic.red   / 255.0f);
        sp.ainc_g = sp.amb_g * (ic.green / 255.0f);
        sp.ainc_b = sp.amb_b * (ic.blue  / 255.0f);
        sp.shading_blend_amount = (float)params[NF_SHADING_BLEND_AMOUNT]->u.fs_d.value;
        sp.shading_blend = (int)params[NF_SHADING_BLEND]->u.pd.value;

        // Specular
        sp.enable_specular = params[NF_SPECULAR_ENABLE]->u.bd.value ? 1 : 0;
        sp.spec_amount = (float)params[NF_SPECULAR]->u.fs_d.value;
        float spec_spread = (float)params[NF_SPECULAR_SPREAD]->u.fs_d.value;
        sp.shininess = 5.0f / std::max(spec_spread, 0.01f);
        PF_Pixel8 sc = params[NF_SPECULAR_COLOR]->u.cd.value;
        sp.spec_r = sc.red / 255.0f; sp.spec_g = sc.green / 255.0f; sp.spec_b = sc.blue / 255.0f;
        sp.specular_blend = (int)params[NF_SPECULAR_BLEND]->u.pd.value;

        // Incidence
        sp.enable_incidence = params[NF_INCIDENCE_ENABLE]->u.bd.value ? 1 : 0;
        sp.inc_amount  = (float)params[NF_INCIDENCE]->u.fs_d.value;
        sp.inc_falloff = (float)params[NF_INCIDENCE_FALLOFF]->u.fs_d.value;
        PF_Pixel8 inc = params[NF_INCIDENCE_COLOR]->u.cd.value;
        sp.inc_r = inc.red / 255.0f; sp.inc_g = inc.green / 255.0f; sp.inc_b = inc.blue / 255.0f;
        sp.incidence_blend = (int)params[NF_INCIDENCE_BLEND]->u.pd.value;

        // Rim Light (G4-b)
        sp.enable_rim    = params[NF_RIM_ENABLE]->u.bd.value ? 1 : 0;
        sp.rim_amount    = (float)params[NF_RIM]->u.fs_d.value;
        sp.rim_size      = (float)params[NF_RIM_SIZE]->u.fs_d.value;
        sp.rim_width     = (float)params[NF_RIM_WIDTH]->u.fs_d.value;
        sp.rim_angle_deg = (float)params[NF_RIM_ANGLE]->u.ad.value / 65536.0f;
        PF_Pixel8 rc = params[NF_RIM_COLOR]->u.cd.value;
        sp.rim_r = rc.red / 255.0f; sp.rim_g = rc.green / 255.0f; sp.rim_b = rc.blue / 255.0f;
        sp.rim_blend = (int)params[NF_RIM_BLEND]->u.pd.value;

        // Toon (G4-b)
        sp.enable_toon = params[NF_TOON_ENABLE]->u.bd.value ? 1 : 0;
        sp.toon_amount       = (float)params[NF_TOON]->u.fs_d.value;
        sp.toon_smoothing    = (float)params[NF_TOON_SMOOTHING]->u.fs_d.value;
        sp.toon_shadow_intensity = (float)params[NF_TOON_SHADOW_INTENSITY]->u.fs_d.value;
        sp.toon_shadow_width = (float)params[NF_TOON_SHADOW_WIDTH]->u.fs_d.value;
        sp.toon_hi_intensity = (float)params[NF_TOON_HIGHLIGHT_INTENSITY]->u.fs_d.value;
        sp.toon_hi_width     = (float)params[NF_TOON_HIGHLIGHT_WIDTH]->u.fs_d.value;
        PF_Pixel8 tsc = params[NF_TOON_SHADOW_COLOR]->u.cd.value;
        PF_Pixel8 tmc = params[NF_TOON_MID_COLOR]->u.cd.value;
        PF_Pixel8 thc = params[NF_TOON_HIGHLIGHT_COLOR]->u.cd.value;
        sp.toon_shadow_r = tsc.red / 255.0f; sp.toon_shadow_g = tsc.green / 255.0f; sp.toon_shadow_b = tsc.blue / 255.0f;
        sp.toon_mid_r    = tmc.red / 255.0f; sp.toon_mid_g    = tmc.green / 255.0f; sp.toon_mid_b    = tmc.blue / 255.0f;
        sp.toon_hi_r     = thc.red / 255.0f; sp.toon_hi_g     = thc.green / 255.0f; sp.toon_hi_b     = thc.blue / 255.0f;
        sp.toon_blend = (int)params[NF_TOON_BLEND]->u.pd.value;

        // Gradient (G4-b)
        sp.enable_gradient = params[NF_GRADIENT_ENABLE]->u.bd.value ? 1 : 0;
        sp.gradient_amount = (float)params[NF_GRADIENT]->u.fs_d.value;
        PF_Pixel8 gc = params[NF_GRADIENT_COLOR]->u.cd.value;
        sp.gradient_r = gc.red / 255.0f; sp.gradient_g = gc.green / 255.0f; sp.gradient_b = gc.blue / 255.0f;
        sp.gradient_blend = (int)params[NF_GRADIENT_BLEND]->u.pd.value;

        // Bump (G5-a)
        sp.enable_bump    = params[NF_BUMP_ENABLE]->u.bd.value ? 1 : 0;
        sp.bump_intensity = (float)params[NF_BUMP_INTENSITY]->u.fs_d.value;
        sp.bump_tile      = (int)params[NF_BUMP_TILE_MODE]->u.pd.value;
        sp.bump_filter    = (int)params[NF_BUMP_FILTER]->u.pd.value;
        sp.bump_offset_x  = (int)(params[NF_BUMP_OFFSET]->u.td.x_value >> 16);
        sp.bump_offset_y  = (int)(params[NF_BUMP_OFFSET]->u.td.y_value >> 16);
        sp.bump_scale     = (float)params[NF_BUMP_SCALE]->u.fs_d.value;

        // Texture (G5-b)
        sp.enable_texture     = params[NF_TEXTURE_ENABLE]->u.bd.value ? 1 : 0;
        sp.texture_amount     = (float)params[NF_TEXTURE]->u.fs_d.value;
        sp.texture_tile       = (int)params[NF_TEXTURE_TILE_MODE]->u.pd.value;
        sp.texture_uv_mirror  = (int)params[NF_TEXTURE_UV_MIRROR]->u.pd.value;
        sp.texture_filter     = (int)params[NF_TEXTURE_FILTER]->u.pd.value;
        sp.texture_offset_x   = (int)(params[NF_TEXTURE_OFFSET]->u.td.x_value >> 16);
        sp.texture_offset_y   = (int)(params[NF_TEXTURE_OFFSET]->u.td.y_value >> 16);
        sp.texture_blend      = (int)params[NF_TEXTURE_BLEND]->u.pd.value;

        // Matcap (G5-c)
        sp.enable_matcap   = params[NF_MATCAP_ENABLE]->u.bd.value ? 1 : 0;
        sp.matcap_amount   = (float)params[NF_MATCAP]->u.fs_d.value;
        sp.matcap_offset_x = (int)(params[NF_MATCAP_OFFSET]->u.td.x_value >> 16);
        sp.matcap_offset_y = (int)(params[NF_MATCAP_OFFSET]->u.td.y_value >> 16);
        sp.matcap_mode     = (int)params[NF_MATCAP_MODE]->u.pd.value;
        sp.matcap_blur     = (int)params[NF_MATCAP_BLUR]->u.sd.value;
        sp.matcap_blend    = (int)params[NF_MATCAP_BLEND]->u.pd.value;

        // Reflection (G5-d)
        sp.enable_reflection = params[NF_REFLECTION_ENABLE]->u.bd.value ? 1 : 0;
        sp.reflection_amount = (float)params[NF_REFLECTION]->u.fs_d.value;
        sp.reflection_env_mode = (int)params[NF_REFLECTION_ENV_MODE]->u.pd.value;
        sp.reflection_gamma  = (float)params[NF_REFLECTION_GAMMA]->u.fs_d.value;
        sp.reflection_tile   = (int)params[NF_REFLECTION_TILE_MODE]->u.pd.value;
        sp.reflection_blur   = (int)params[NF_REFLECTION_BLUR]->u.sd.value;
        PF_Pixel8 rcol = params[NF_REFLECTION_COLOR]->u.cd.value;
        sp.refl_color_r = rcol.red / 255.0f;
        sp.refl_color_g = rcol.green / 255.0f;
        sp.refl_color_b = rcol.blue / 255.0f;
        sp.reflection_inclination_deg = (float)params[NF_REFLECTION_INCLINATION]->u.ad.value / 65536.0f;
        sp.reflection_azimuth_deg     = (float)params[NF_REFLECTION_AZIMUTH]->u.ad.value     / 65536.0f;
        sp.reflection_scale          = (float)params[NF_REFLECTION_SCALE]->u.fs_d.value;
        sp.reflection_ior            = (float)params[NF_REFLECTION_IOR]->u.fs_d.value;
        sp.reflection_fresnel        = (float)params[NF_REFLECTION_FRESNEL]->u.fs_d.value;
        sp.reflection_fresnel_depth  = (float)params[NF_REFLECTION_FRESNEL_DEPTH]->u.fs_d.value;
        sp.reflection_blend          = (int)params[NF_REFLECTION_BLEND]->u.pd.value;

        // Refraction (G5-e)
        sp.enable_refraction        = params[NF_REFRACTION_ENABLE]->u.bd.value ? 1 : 0;
        sp.refraction_amount        = (float)params[NF_REFRACTION]->u.fs_d.value;
        sp.refraction_ior           = (float)params[NF_REFRACTION_IOR]->u.fs_d.value;
        sp.refraction_strength      = (float)params[NF_REFRACTION_STRENGTH]->u.fs_d.value;
        sp.refraction_use_depth     = params[NF_REFRACTION_USE_DEPTH]->u.bd.value ? 1 : 0;
        sp.refraction_gamma         = (float)params[NF_REFRACTION_GAMMA]->u.fs_d.value;
        sp.refraction_tile          = (int)params[NF_REFRACTION_TILE_MODE]->u.pd.value;
        sp.refraction_blur          = (int)params[NF_REFRACTION_BLUR]->u.sd.value;
        sp.refraction_offset_x      = (int)(params[NF_REFRACTION_OFFSET]->u.td.x_value >> 16);
        sp.refraction_offset_y      = (int)(params[NF_REFRACTION_OFFSET]->u.td.y_value >> 16);
        sp.refraction_scale         = (float)params[NF_REFRACTION_SCALE]->u.fs_d.value;
        PF_Pixel8 refr_col = params[NF_REFRACTION_COLOR]->u.cd.value;
        sp.refr_color_r = refr_col.red / 255.0f;
        sp.refr_color_g = refr_col.green / 255.0f;
        sp.refr_color_b = refr_col.blue / 255.0f;
        sp.refraction_fresnel       = (float)params[NF_REFRACTION_FRESNEL]->u.fs_d.value;
        sp.refraction_fresnel_depth = (float)params[NF_REFRACTION_FRESNEL_DEPTH]->u.fs_d.value;
        sp.refraction_blend         = (int)params[NF_REFRACTION_BLEND]->u.pd.value;

        // Depth Map (G5-e)
        sp.depth_invert   = params[NF_DEPTH_INVERT]->u.bd.value ? 1 : 0;
        sp.depth_strength = (float)params[NF_DEPTH_STRENGTH]->u.fs_d.value;

        // Common (Lights)
        float exposure = (float)params[NF_EXPOSURE]->u.fs_d.value;
        float gamma    = (float)params[NF_GAMMA]->u.fs_d.value;
        sp.ev_global = powf(2.0f, exposure);
        sp.inv_gamma = (gamma > 1e-3f) ? (1.0f / gamma) : 1.0f;
        sp.falloff_mode = (int)params[NF_FALLOFF_MODE]->u.pd.value;
        sp.falloff_val  = (float)params[NF_FALLOFF]->u.fs_d.value;

        double sx = (in_data->downsample_x.num > 0)
            ? (double)in_data->downsample_x.den / in_data->downsample_x.num : 1.0;
        double sy = (in_data->downsample_y.num > 0)
            ? (double)in_data->downsample_y.den / in_data->downsample_y.num : 1.0;
        sp.sx = (float)sx;
        sp.sy = (float)sy;

        // Light cache from SmartPreRender
        const NormForgeLightCache *light_cache =
            (const NormForgeLightCache *)extra->input->pre_render_data;
        sp.num_lights = 0;
        if (light_cache) {
            int n = (int)light_cache->light_count;
            if (n > NF_GPU_MAX_LIGHTS) n = NF_GPU_MAX_LIGHTS;
            for (int i = 0; i < n; ++i) {
                const LightInfo &li = light_cache->lights[i];
                sp.lights[i].enabled   = li.enabled ? 1 : 0;
                sp.lights[i].x         = (float)li.x;
                sp.lights[i].y         = (float)li.y;
                sp.lights[i].z         = (float)li.z;
                sp.lights[i].r         = (float)li.r;
                sp.lights[i].g         = (float)li.g;
                sp.lights[i].b         = (float)li.b;
                sp.lights[i].intensity = (float)li.intensity;
            }
            sp.num_lights = n;
        }

        // ---- Map Blur for Matcap (G5-c) ----
        // When matcap_blur > 0, run 3-pass (H+V) box blur on the matcap layer
        // into a ping-pong pair of intermediate GPU worlds. Replace matcap_mem
        // with the blurred result before the shading kernel sees it.
        PF_EffectWorld *mc_ping_world = NULL;
        PF_EffectWorld *mc_pong_world = NULL;
        if (!err && sp.enable_matcap && sp.has_matcap && sp.matcap_blur > 0) {
            PF_PixelFormat blur_fmt = PF_PixelFormat_GPU_BGRA128;
            ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                          extra->input->device_index,
                                          matcap_world->width,
                                          matcap_world->height,
                                          matcap_world->pix_aspect_ratio,
                                          in_data->field,
                                          blur_fmt, false,
                                          &mc_ping_world));
            if (!err) ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                                   extra->input->device_index,
                                                   matcap_world->width,
                                                   matcap_world->height,
                                                   matcap_world->pix_aspect_ratio,
                                                   in_data->field,
                                                   blur_fmt, false,
                                                   &mc_pong_world));
            void *ping_mem = NULL;
            void *pong_mem = NULL;
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     mc_ping_world, &ping_mem));
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     mc_pong_world, &pong_mem));
            if (!err && ping_mem && pong_mem) {
                int mw = (int)matcap_world->width;
                int mh = (int)matcap_world->height;
                int mc_pitch_in   = (int)(matcap_world->rowbytes / 16);
                int ping_pitch    = (int)(mc_ping_world->rowbytes / 16);
                int pong_pitch    = (int)(mc_pong_world->rowbytes / 16);
                int radius = sp.matcap_blur;

                // Initial copy: matcap source -> ping
                NormForge_CUDA_PassThrough(
                    matcap_mem, ping_mem,
                    (size_t)matcap_world->rowbytes,
                    (size_t)mc_ping_world->rowbytes,
                    (size_t)mw * 16, (size_t)mh);
                (void)mc_pitch_in;

                // 3-pass (H + V) ping-pong
                for (int i = 0; i < 3; ++i) {
                    NormForge_CUDA_BoxBlur1D(ping_mem, pong_mem,
                                             mw, mh,
                                             ping_pitch, pong_pitch,
                                             radius, /*direction=*/0);
                    NormForge_CUDA_BoxBlur1D(pong_mem, ping_mem,
                                             mw, mh,
                                             pong_pitch, ping_pitch,
                                             radius, /*direction=*/1);
                }

                matcap_mem      = ping_mem;
                sp.matcap_pitch = ping_pitch;
            }
        }

        // ---- Map Blur for Reflection (G5-d) ----
        // Same pattern as Matcap above. ping-pong pair of intermediate worlds
        // at env_map dimensions, 3 iterations of (H + V).
        PF_EffectWorld *env_ping_world = NULL;
        PF_EffectWorld *env_pong_world = NULL;
        if (!err && sp.enable_reflection && sp.has_envmap && sp.reflection_blur > 0) {
            PF_PixelFormat blur_fmt = PF_PixelFormat_GPU_BGRA128;
            ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                          extra->input->device_index,
                                          env_world->width,
                                          env_world->height,
                                          env_world->pix_aspect_ratio,
                                          in_data->field,
                                          blur_fmt, false,
                                          &env_ping_world));
            if (!err) ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                                   extra->input->device_index,
                                                   env_world->width,
                                                   env_world->height,
                                                   env_world->pix_aspect_ratio,
                                                   in_data->field,
                                                   blur_fmt, false,
                                                   &env_pong_world));
            void *ping_mem = NULL;
            void *pong_mem = NULL;
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     env_ping_world, &ping_mem));
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     env_pong_world, &pong_mem));
            if (!err && ping_mem && pong_mem) {
                int ew = (int)env_world->width;
                int eh = (int)env_world->height;
                int ping_pitch = (int)(env_ping_world->rowbytes / 16);
                int pong_pitch = (int)(env_pong_world->rowbytes / 16);
                int radius = sp.reflection_blur;

                NormForge_CUDA_PassThrough(
                    env_mem, ping_mem,
                    (size_t)env_world->rowbytes,
                    (size_t)env_ping_world->rowbytes,
                    (size_t)ew * 16, (size_t)eh);

                for (int i = 0; i < 3; ++i) {
                    NormForge_CUDA_BoxBlur1D(ping_mem, pong_mem,
                                             ew, eh,
                                             ping_pitch, pong_pitch,
                                             radius, /*direction=*/0);
                    NormForge_CUDA_BoxBlur1D(pong_mem, ping_mem,
                                             ew, eh,
                                             pong_pitch, ping_pitch,
                                             radius, /*direction=*/1);
                }

                env_mem      = ping_mem;
                sp.env_pitch = ping_pitch;
            }
        }

        // ---- Map Blur for Refraction (G5-e) ----
        // Same pattern as Reflection.
        PF_EffectWorld *rf_ping_world = NULL;
        PF_EffectWorld *rf_pong_world = NULL;
        if (!err && sp.enable_refraction && sp.has_refrmap && sp.refraction_blur > 0) {
            PF_PixelFormat blur_fmt = PF_PixelFormat_GPU_BGRA128;
            ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                          extra->input->device_index,
                                          refr_world->width,
                                          refr_world->height,
                                          refr_world->pix_aspect_ratio,
                                          in_data->field,
                                          blur_fmt, false,
                                          &rf_ping_world));
            if (!err) ERR(gpu_suite->CreateGPUWorld(in_data->effect_ref,
                                                   extra->input->device_index,
                                                   refr_world->width,
                                                   refr_world->height,
                                                   refr_world->pix_aspect_ratio,
                                                   in_data->field,
                                                   blur_fmt, false,
                                                   &rf_pong_world));
            void *ping_mem = NULL;
            void *pong_mem = NULL;
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     rf_ping_world, &ping_mem));
            if (!err) ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref,
                                                     rf_pong_world, &pong_mem));
            if (!err && ping_mem && pong_mem) {
                int rw = (int)refr_world->width;
                int rh = (int)refr_world->height;
                int ping_pitch = (int)(rf_ping_world->rowbytes / 16);
                int pong_pitch = (int)(rf_pong_world->rowbytes / 16);
                int radius = sp.refraction_blur;

                NormForge_CUDA_PassThrough(
                    refr_mem, ping_mem,
                    (size_t)refr_world->rowbytes,
                    (size_t)rf_ping_world->rowbytes,
                    (size_t)rw * 16, (size_t)rh);

                for (int i = 0; i < 3; ++i) {
                    NormForge_CUDA_BoxBlur1D(ping_mem, pong_mem,
                                             rw, rh,
                                             ping_pitch, pong_pitch,
                                             radius, /*direction=*/0);
                    NormForge_CUDA_BoxBlur1D(pong_mem, ping_mem,
                                             rw, rh,
                                             pong_pitch, ping_pitch,
                                             radius, /*direction=*/1);
                }

                refr_mem      = ping_mem;
                sp.refr_pitch = ping_pitch;
            }
        }

        // ---- Dispatch ----
        if (!err && sp.display == DISPLAY_OFF) {
            // OFF = input pass-through (matches CPU's WorldTransformSuite copy)
            int rc = NormForge_CUDA_PassThrough(
                src_mem, dst_mem,
                (size_t)input_world->rowbytes,
                (size_t)output_world->rowbytes,
                (size_t)std::min(input_world->width, output_world->width) * 16,
                (size_t)std::min(input_world->height, output_world->height));
            if (rc != 0) err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        } else if (!err) {
            int rc = NormForge_CUDA_RenderShading(
                src_mem, nm_mem, ao_mem, specmap_mem, bump_mem,
                uv_mem, tex_mem, matcap_mem, env_mem, refr_mem, depth_mem,
                dst_mem, sp);
            if (rc != 0) err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        }

        // Dispose intermediate blur worlds (after kernel completes)
        if (mc_ping_world)  gpu_suite->DisposeGPUWorld(in_data->effect_ref, mc_ping_world);
        if (mc_pong_world)  gpu_suite->DisposeGPUWorld(in_data->effect_ref, mc_pong_world);
        if (env_ping_world) gpu_suite->DisposeGPUWorld(in_data->effect_ref, env_ping_world);
        if (env_pong_world) gpu_suite->DisposeGPUWorld(in_data->effect_ref, env_pong_world);
        if (rf_ping_world)  gpu_suite->DisposeGPUWorld(in_data->effect_ref, rf_ping_world);
        if (rf_pong_world)  gpu_suite->DisposeGPUWorld(in_data->effect_ref, rf_pong_world);
    }

    // Checkin params (unconditional - cleared param_defs are safe to checkin)
    for (A_long i = 1; i < NF_NUM_PARAMS; ++i) {
        if (is_layer_param(i)) continue;
        PF_CHECKIN_PARAM(in_data, &param_defs[i]);
    }
#else
    (void)in_data; (void)out_data; (void)extra;
    (void)input_world; (void)output_world;
    err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
#endif

    return err;
}

static PF_Err
SmartRender(PF_InData *in_data, PF_OutData *out_data, void *extra_void, bool isGPU)
{
    PF_Err err = PF_Err_NONE;
    PF_SmartRenderExtra *extra = static_cast<PF_SmartRenderExtra *>(extra_void);
    PF_EffectWorld *input_world  = NULL;
    PF_EffectWorld *output_world = NULL;

    err = extra->cb->checkout_layer_pixels(in_data->effect_ref, NF_INPUT, &input_world);
    if (!err) err = extra->cb->checkout_output(in_data->effect_ref, &output_world);
    if (err || !input_world || !output_world) return err;

    // Step G2.5 (0.6.2): Stats logging via OutputDebugStringA. Capture per-frame
    // render time so the G3 GPU vs CPU comparison can be quantified. Inspect
    // with DebugView (Microsoft Sysinternals).
    LARGE_INTEGER qpc_freq, qpc_t0, qpc_t1;
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_t0);

    if (isGPU) {
        err = SmartRenderGPU(in_data, out_data, extra, input_world, output_world);
    } else {
        err = SmartRenderCPU(in_data, out_data, extra, input_world, output_world);
    }

    QueryPerformanceCounter(&qpc_t1);
    double elapsed_ms = (double)(qpc_t1.QuadPart - qpc_t0.QuadPart) * 1000.0
                      / (double)qpc_freq.QuadPart;
    char log_buf[256];
    sprintf_s(log_buf, sizeof(log_buf),
              "[NormForge] mode=%s bpc=%d size=%ldx%ld render=%.2fms\n",
              isGPU ? "GPU" : "CPU",
              (int)extra->input->bitdepth,
              (long)output_world->width,
              (long)output_world->height,
              elapsed_ms);
    OutputDebugStringA(log_buf);

    return err;
}

static PF_Err
GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
               PF_GPUDeviceSetupExtra *extra)
{
    // Step G1 (0.6.0): CUDA kernels are statically linked, so there is no
    // runtime kernel compilation. Just re-advertise GPU support for CUDA.
    if (extra->input->what_gpu == PF_GPU_Framework_CUDA) {
        out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    }
    return PF_Err_NONE;
}

static PF_Err
GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                 PF_GPUDeviceSetdownExtra *extra)
{
    // Step G1 (0.6.0): no gpu_data was allocated in Setup, nothing to free.
    (void)in_data; (void)out_data; (void)extra;
    return PF_Err_NONE;
}

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr    inPtr,
    PF_PluginDataCB2    inPluginDataCallBackPtr,
    SPBasicSuite        *inSPBasicSuitePtr,
    const char          *inHostName,
    const char          *inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "NormForge",
        "ADBE NormForge",
        "NormForge",
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com");

    return result;
}

PF_Err
EffectMain(
    PF_Cmd      cmd,
    PF_InData   *in_data,
    PF_OutData  *out_data,
    PF_ParamDef *params[],
    PF_LayerDef *output,
    void        *extra)
{
    PF_Err err = PF_Err_NONE;

    // Add this .aex's own folder to the DLL search path so the
    // delay-loaded cudart64_12.dll can be resolved when placed
    // alongside the .aex (e.g. <AE>\Support Files\Plug-ins\NormForge\).
    EnsurePluginDllPath();

    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = SmartPreRender(in_data, out_data, extra);
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data, extra, /*isGPU=*/false);
                break;
            case PF_Cmd_SMART_RENDER_GPU:
                err = SmartRender(in_data, out_data, extra, /*isGPU=*/true);
                break;
            case PF_Cmd_GPU_DEVICE_SETUP:
                err = GPUDeviceSetup(in_data, out_data,
                                     static_cast<PF_GPUDeviceSetupExtra *>(extra));
                break;
            case PF_Cmd_GPU_DEVICE_SETDOWN:
                err = GPUDeviceSetdown(in_data, out_data,
                                       static_cast<PF_GPUDeviceSetdownExtra *>(extra));
                break;
        }
    }
    catch (PF_Err &thrown_err) {
        err = thrown_err;
    }

    return err;
}
