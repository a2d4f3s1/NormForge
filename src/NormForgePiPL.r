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
            225280  /* 0.6.14 (Step G7: 配布前ランタイム整備。vcxproj で <CudaRuntime>Shared</CudaRuntime> を明示 + AdditionalDependencies を cudart_static.lib → cudart.lib に切替して LNK4098 (CRT 二重) 警告を解消、.aex サイズが 2.0 MB → 1.4 MB に減少。deploy_to_ae.ps1 を拡張して cudart64_12.dll を AfterFX.exe 同階層 (Support Files\) にコピー、Windows DLL search で確実に解決させる。動作確認: AE 32bpc Auto モードで mode=GPU ログを確認、FHD で render ~3 ms = 0.6.4 ベンチ (~3.4 ms) と整合、機能・性能の回帰なし) */
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
