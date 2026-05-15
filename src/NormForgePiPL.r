#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "NormForge"
        },
        Category {
            "NormForge"
        },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 {"EffectMain"},
    #endif
#endif
        AE_PiPL_Version {
            2,
            0
        },
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },
        AE_Effect_Version {
            229376  /* 0.7.0 (Normal Map レイヤー未指定 (None) のとき入力レイヤー (param[0]) を Normal Map として fallback 参照する仕様を追加。AE SDK 推奨パターン + Normality 互換。あわせて Use Normal Map Alpha のデフォルトを OFF から ON に変更。CPU/GPU 両経路で対応) */
        },
        AE_Effect_Info_Flags {
            0
        },
        AE_Effect_Global_OutFlags {
            0x02000400  /* DEEP_COLOR_AWARE (0x02000000) | PIX_INDEPENDENT (0x400) */
        },
        AE_Effect_Global_OutFlags_2 {
            0x0A001404  /* SUPPORTS_GPU_RENDER_F32 (0x02000000) | SUPPORTS_THREADED_RENDERING (0x08000000) | FLOAT_COLOR_AWARE (0x1000) | SUPPORTS_SMART_RENDER (0x400) | I_USE_3D_LIGHTS (0x4) */
        },
        AE_Effect_Match_Name {
            "ADBE NormForge"
        },
        AE_Reserved_Info {
            0
        },
        AE_Effect_Support_URL {
            "https://github.com"
        }
    }
};
