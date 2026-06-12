# IRPipeline

A physically-based simulation of a complete LWIR (8–14 µm) uncooled microbolometer camera chain, from scene spectral radiance to display-ready RGBA output.

Built as part of a Master's thesis (TFM) at UTAD.

## What it does

Takes a flat buffer of band-integrated spectral radiance (W·m⁻²·sr⁻¹) at the camera aperture and runs it through a 9-stage sensor model:

```
Radiance → PSF/MTF blur → Radiometric calibration → Thermal lag (IIR)
→ Fixed-pattern noise → Read noise → Clamp → AGC → Colormap → RGBA
```

Produces two outputs:
- **RGBA 8-bit** display image (post-AGC, colormapped) for visualization
- **Float DN** calibrated digital numbers (pre-AGC) preserving the radiometric relationship for ML/analysis

Both outputs are optional — pass `nullptr` to skip what you don't need.

## Architecture

| Project | Type | Purpose |
|---|---|---|
| **IRPipelineCore** | Static library (.lib) | All physics logic. Linked directly by UE plugins. |
| **IRPipelineLib** | Dynamic library (.dll) | Thin C ABI shim for external consumers (Python, MATLAB). |
| **IRPipelineTest** | Executable | Standalone harness: synthetic scene → PNG + timing. |

The static/dynamic split avoids CRT heap conflicts when linking into Unreal Engine's memory allocator.

## Key design choices

- **Pure C interface** (`extern "C"`) — stable across compilers and MSVC toolchains
- **Opaque handle** (`IRPipeline*`) — one instance per simulated camera, owns all internal state
- **Caller-owned buffers** — no cross-boundary allocation
- **OpenMP parallelism** on all pixel-level stages
- **Raw output tapped before AGC** — AGC is lossy and scene-adaptive; raw DN preserves radiometry

## Target

- Resolution: 1920 x 1080
- Framerate: 60 fps (30 fps fallback)
- Final integration: Unreal Engine 5 (UMG widget / material)

## References

Based on published models from Meribout 2019 (radiometric calibration), Saxena et al. 2011/2012 (bolometer physics), Wolf et al. 2016 (FPN/NUC), Silverman 1993 (plateau AGC), and others. See `CLAUDE.md` for the full reference table.

## Build

Open `IRPipeline.slnx` in Visual Studio 2022+. Platform: **x64**. Requires MSVC v145 toolset, C++17.

## Status

- [x] Full pipeline implementation (9 stages)
- [x] C ABI stable interface
- [x] Standalone test harness
- [ ] Counter-based PRNG optimization (priority)
- [ ] Spectral band integration with Planck weights
- [ ] Dynamic FPN drift with FPA temperature
- [ ] Full MTF model
- [ ] IRPipelineValidator (per-stage unit tests)
- [ ] UE plugin scaffold
