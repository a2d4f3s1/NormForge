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
            233472  /* 0.7.2 (Light * dead code を wildcard 名前パターンマッチとして実装。0.7.1 popup レイアウト (None|All|Light 1..5|Light * の 8 値) を完全維持しつつ、Light * の挙動を「実質 All」(旧版で条件分岐に入らずスルーしていたため) から「レイヤー名の先頭 5 文字が "Light" のライト全部にマッチ」(case-sensitive prefix) に修正。Light Key / Light Fill / Light Rim 等の prefix 命名で 5 個超の複数選択も可能 (MAX_LIGHTS=5 で打ち切り)。既存プロジェクト layout 完全互換のため PATCH バンプ。当初 PF_Param_LAYER ピッカー方式 (0.8.0 予定) で計画したが AE SDK 制約で路線変更) */
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
