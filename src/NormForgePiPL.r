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
            235520  /* 0.7.3 (degenerate normal を flat (0,0,1) にフォールバック。CPU / GPU の normal decode primary 段階で |N| < 0.01 のとき強制的に (0,0,1) (画面正面 flat) に置換。Phase 6-1 input fallback (Normal Map 未指定時に入力レイヤーを normal として参照) でグレー solid を input にした場合、8bit は量子化誤差 (128/255≈0.502) で偶然 normal が tilted になって light が当たって見えたが、16/32bit は厳密 0.5 から (0,0,0) へ degenerate して真っ黒になる罠の解消。「8bit で作業 → 16bit でレンダリングしたら消える」UX 罠回避。layout 完全互換、挙動変化は input fallback / degenerate normal map ピクセル時のみで「無効 N → flat shading」(改善方向)) */
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
