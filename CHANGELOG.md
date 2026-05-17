# Changelog

All notable changes to NormForge will be documented in this file.

---

## [0.7.3] — 2026-05-17

### Fixed
- **Normal Map fallback の degenerate normal 罠を解消.** Normal Map 未指定
  (None) 時に入力レイヤーを normal map として fallback 参照する仕様 (0.7.0〜)
  で、grey solid を入力にすると 8 bpc では量子化誤差 (128/255≈0.502) で偶然
  normal が tilted になって lit に見えるが、16/32 bpc では厳密 0.5 から
  `(0, 0, 0)` に degenerate して真っ黒になる現象を修正. CPU / GPU の normal
  decode 段階で `|N| < 0.01` のとき強制的に `(0, 0, 1)` flat に置換、全深度で
  「flat 平面の正常 lit」に統一. 「8 bpc で作業 → 16 / 32 bpc でレンダリングしたら
  消える」UX 罠を解消.

### Internal
- `SmartRenderCPU` の bulk `PF_CHECKOUT_PARAM` / `PF_CHECKIN_PARAM` 除外
  リストに `NF_REFLECTION_ENV_MAP` / `NF_REFRACTION_MAP` を追加 (GPU 経路の
  `is_layer_param` lambda と整合化). 二重 checkout だった状態を解消、挙動変化
  なし.

---

## [0.7.2] — 2026-05-17

### Fixed
- **`Light *` を本来意図どおり name prefix wildcard として実装.** Lights
  Render popup の最後の選択肢 `Light *` は、旧版では実装が落ちて実質「All」と
  等価のデッドコードだった. 本リリースでレイヤー名の先頭 5 文字が `"Light"` の
  ライト全部にマッチ (case-sensitive prefix) する挙動を実装.
- `Light Key` / `Light Fill` / `Light Rim` のような prefix 命名で **5 個超の
  複数選択も可能** (内部の `MAX_LIGHTS = 5` で打ち切り).
- popup レイアウト (`None | All | Light 1..5 | Light *` の 8 値) は完全
  維持. 既存プロジェクトへの影響は `Light *` 選択時の挙動変化 (実質 All →
  wildcard) のみ.

---

## [0.7.1] — 2026-05-15

### Fixed
- **32 bpc Auto モード (GPU 経路) で `Pre Blur` slider が無視されていた問題を
  修正.** 法線マップに 3-pass box blur が適用され、CPU 経路と同等の挙動になる.
- `Blur Alpha` checkbox を GPU 経路でも反映 (OFF で alpha 成分は blur されず
  原本がパススルー、CPU と同じ振る舞い).

---

## [0.7.0] — 2026-05-15

### Added
- **Normal Map レイヤー未指定 (None) 時に入力レイヤーを Normal Map として
  fallback 参照する仕様** (AE SDK 推奨パターン). Normal Map を別途用意しなくても
  入力レイヤーの柄に応じた shading が得られる. CPU / GPU 両経路で対応.

### Changed
- `Use Normal Map Alpha` のデフォルトを OFF から ON に変更 (input fallback 時に
  入力レイヤーの Alpha 透過を尊重するのが自然).
- `Display = ADJUSTED NORMALS / ORIGINAL NORMALS / DEPTH` のプレビューで、
  Normal Map 未指定時の「真っ黒」表示が入力レイヤー / Depth Map 由来の表示に
  変化.

> ⚠ Normal Map を指定せずに NormForge を使っていた既存プロジェクトでは
> shading の見た目が変化します (flat normal → 入力レイヤーを法線解釈).

---

## [0.6.15] — 2026-05-07

### Changed
- **Distribution layout consolidated into a single folder.** Place the
  `NormForge` folder under `<AE>\Support Files\Plug-ins\` and you're
  done; uninstall is just "delete the folder". The plugin now uses
  delay-loaded import for `cudart64_12.dll` combined with
  `SetDllDirectory` at runtime, so the CUDA Runtime DLL can sit
  alongside the `.aex` instead of having to be dropped into the
  `AfterFX.exe` directory.

### Build
- `Win/NormForge.vcxproj`: added `<DelayLoadDLLs>cudart64_12.dll</DelayLoadDLLs>`
  and linked `Delayimp.lib` (Debug + Release).
- `src/NormForge.cpp`: added `EnsurePluginDllPath()` (uses
  `GetModuleHandleEx` + `GetModuleFileName` + `SetDllDirectoryA`),
  invoked at the top of `EffectMain` so that the delay-loaded
  `cudart64_12.dll` resolves from the same folder as the `.aex`.

### Distribution
- ZIP package (`NormForge_v0.6.15_win64.zip`) now contains a single
  `NormForge\` folder ready to be dropped into `Plug-ins\` directly.
  README.txt updated with the new (simpler) install procedure.

### No functional changes
- All 11 per-pixel features, 30 blend modes, lighting, and Depth Map
  behavior are identical to 0.6.14. Visual output is unchanged.

---

## [0.6.14] — 2026-05-07 (Initial Public Release)

### Per-pixel features

- **Shading**: Diffuse (Lambert) + Specular + Incidence + Ambient + Incandescence
- **Specular**: cross-product highlight (`1 - |N x L|`), optional Specular Map (luminance modulator)
- **Incidence**: rim-edge falloff `(1 - Nz)^(1 / falloff)` with custom color and curve
- **Rim Light**: directional incidence with size / width / color / angle controls
- **Toon**: 3-step quantized shading (shadow / mid / highlight) with smoothing
- **Gradient**: luminance-based color tinting
- **Bump**: normal-map perturbation with grayscale auto-detection, 4 tile modes, Bilinear/Nearest sampling
- **Texture**: UV-based texture sampling with per-axis Mirror, 4 tile modes, sub-pixel offset
- **Matcap**: normal → UV sphere mapping with Box Blur 3-pass pre-filter
- **Reflection**: environment map with Spherical / Panorama UV, Inclination / Azimuth, Schlick Fresnel, Map Blur
- **Refraction**: screen-space displacement with IOR, Strength, Use-Depth modulation, Inverted-Schlick Fresnel, Map Blur

### Lighting

- Up to **5 AE lights** per layer (Point / Spot / Parallel / Ambient)
- Per-light Falloff modes: Constant / Linear / Quadratic / Cubic
- Global Falloff scale, Exposure (-2..+2), Gamma (0..5)

### Compositing

- **30 blend modes** per section: Normal / Multiply / Add / Screen / Overlay / Soft Light / Hard Light / Difference / Exclusion / Color Dodge / Color Burn / Phoenix etc. (HSL family falls back to Add)
- Per-section Blend popup + Blend Amount slider
- Display selector to preview any per-pixel section in isolation (Diffuse / Specular / Incidence / Rim / Toon / Gradient / Bump / Texture / Matcap / Reflection / Refraction / All / Off)

### Depth Map (optional auxiliary layer)

- BT.709 luminance decoding with Invert and Strength scaling
- Depth-aware Diffuse: `lz = -light.z + pixel_z` for 3D-aware lighting from a 2D source

### Performance

- **CUDA GPU acceleration** for all per-pixel features (Windows + NVIDIA, 32 bpc only)
- Render Mode selector: Auto / GPU (32 bpc only) / CPU only
- CPU fallback path (per-row parallel via AE SDK `iterate_generic`) for 8 / 16 bpc, and 32 bpc when CPU-only is selected
- Map Blur (Box Blur 3-pass) is GPU-accelerated for Matcap / Reflection / Refraction

#### Benchmark (RTX 4090 Laptop, 32 bpc, Display = Diffuse, default settings)

| Resolution | CPU median | GPU median | Speedup |
|---|---|---|---|
| FHD (1920x1080) | ~12.7 ms | ~3.4 ms | 3.7x |
| 4K (3840x2160) | ~32 ms | ~3.3 ms | 9.7x |
| 5.5K (5902x3320) | ~85 ms | ~8.2 ms | 10.4x |

### Multi-Frame Rendering

- `PF_OutFlag2_SUPPORTS_THREADED_RENDERING` enabled for AE's MFR
- AEGP suite calls (light collection) confined to `SmartPreRender` (per-frame thread)
- No race conditions in `SmartRender` — safe under MFR

### Build / Distribution

- DLL-mode CUDA Runtime linkage (`cudart.lib`)
- Bundles `cudart64_12.dll` alongside the `.aex` (placed in `AfterFX.exe` directory for Windows DLL search)
- Targets: NVIDIA `compute_50` (Maxwell) or higher; PTX JIT for forward compatibility
- Build environment: Visual Studio 2022 (v17.11+), CUDA Toolkit 12.6, AE SDK 2025
- Target platform: Adobe After Effects 2024 or later, Windows 10/11 x64
