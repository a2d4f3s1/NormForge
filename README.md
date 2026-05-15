# NormForge

**Normal map relighting plugin for Adobe After Effects**

> **Status: Work in Progress** — Not yet released. Features and UI are subject to change.

---

## Overview

NormForge is a relighting plugin for After Effects that uses normal maps and depth maps to apply dynamic lighting to 2D layers. Inspired by classic AE relighting workflows (such as Normality), it is designed for Windows + NVIDIA environments.

## Planned Features

- Normal map input with per-channel assignment and inversion
- Multiple AE light support (up to 5 lights)
- Diffuse / Specular / Incidence / Rim lighting
- Toon shading, Matcap, Reflection, Refraction, Texture, Bump
- CUDA GPU acceleration (CPU fallback included)

## Requirements

| Item | Requirement |
|---|---|
| OS | Windows 10/11 x64 |
| After Effects | 2024 or later |
| GPU | NVIDIA (CUDA) — required for GPU mode |

## Installation

1. Download the latest release ZIP from
   [Releases](https://github.com/a2d4f3s1/NormForge/releases)
   (e.g. `NormForge_v0.6.15_win64.zip`).
2. Extract the archive. You will get a `NormForge` folder containing
   `NormForge.aex`, `cudart64_12.dll`, and `README.txt`.
3. Move the **whole `NormForge` folder** into:

   `C:\Program Files\Adobe\Adobe After Effects 2024\Support Files\Plug-ins\`

   (administrator privileges required for `Program Files`).
4. Start After Effects. NormForge will appear in the Effect menu.

The bundled `cudart64_12.dll` is the NVIDIA CUDA Runtime; no separate
CUDA Toolkit installation is required.

To uninstall, delete the `NormForge` folder.

## Usage

1. Apply the **NormForge** effect to a layer.
2. Plug in a Normal Map (and optionally a Depth Map, AO Map, etc.) via
   the layer pickers in the Normals / Depth / Shading sections.
3. Add AE lights (Point / Spot / Parallel / Ambient) to the comp.
   NormForge picks up to 5 of them automatically.
4. Use the **Display** popup at the top of the controls to preview any
   per-pixel section in isolation (Diffuse / Specular / Reflection /
   Refraction / etc.) or **All** for the full composite.
5. **Render Mode**:
   - `Auto` — GPU when 32 bpc, CPU otherwise (recommended)
   - `GPU (32 bpc only)` — force GPU
   - `CPU only` — force CPU regardless of bpc

For 32 bpc projects on NVIDIA hardware, Auto delivers roughly 3–10x
speedup over CPU-only depending on resolution. See `CHANGELOG.md` for
full benchmarks and feature notes.

## License

MIT — see [LICENSE](LICENSE).
