# Changelog

All notable changes to NormForge will be documented in this file.

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
