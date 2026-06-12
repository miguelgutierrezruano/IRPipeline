# CLAUDE.md — LWIR Microbolometer Pipeline Project

This file gives you full context for the project. Read it before touching any file.
The user is building a physically-based LWIR thermal camera simulation pipeline,
ultimately targeting Unreal Engine. This is also an academic project (TFM / Master's thesis).

---

## Project goal

Simulate a complete LWIR (8–14 µm) uncooled microbolometer camera chain.

- **Input**: a flat buffer of `float` values representing **in-band spectral radiance**
  at the camera aperture — one `float` per pixel, band-integrated (W·m⁻²·sr⁻¹, 8–14 µm).
  The providing department cannot yet specify spectral precision, so band-integrated
  is the correct default. A `bands` field exists in the config for future spectral extension.
- **Output A** (`uint8_t* rgba_out`): display image, RGBA 8-bit, post-AGC + colormap.
  This is the visualization for UE or any consumer that only needs to display.
- **Output B** (`float* raw_out`): calibrated sensor DN (digital number), **PRE-AGC**.
  This preserves the radiometric relationship for ML / downstream analysis.
  Both output pointers are nullable — callers only pay for what they use.
- **Target resolution**: 1920×1080.
- **Target framerate**: 60 fps (30 fps acceptable fallback).
- **Final rendering target**: Unreal Engine 5, real-time display on a UMG widget or material.

---

## Physics pipeline — stage order (order is a deliberate modeling choice)

```
radiance L (float, per pixel, band-integrated)
    │
    ├─ [stage 0, optional] spectral band collapse — placeholder if bands > 1
    │
    ├─ [stage 1] PSF / MTF  — separable Gaussian blur in the radiance domain
    │              (optics degrade the scene before the detector samples it)
    │
    ├─ [stage 2] Radiometric calibration / ideal signal
    │              DN = a0 + a1·L + a2·L²   (quadratic, Meribout 2019)
    │
    ├─ [stage 3] Detector thermal lag — temporal IIR (1st-order low-pass)
    │              S_t[n] = α·S[n] + (1-α)·S_t[n-1],   α = 1/(1 + τ_frames)
    │
    ├─ [stage 4] Residual FPN — per-pixel gain + offset maps (post-NUC residual)
    │              D = S_t · gain_map + offset_map
    │              Maps are fixed per camera instance, generated from seed at creation.
    │
    ├─ [stage 5] Read noise — N(0, NETD_dn) added per pixel
    │
    ├─ [stage 6] Clamp to [0, dn_max]
    │
    ├──────────── raw_out tapped HERE (calibrated DN, pre-AGC, radiometric) ──────────
    │
    ├─ [stage 7] AGC — plateau histogram equalization or linear min/max stretch
    │              → 8-bit intensity
    │
    └─ [stage 8] Colormap → RGBA (white-hot, black-hot, ironbow)
                 → rgba_out
```

**Key rule**: AGC is lossy and scene-adaptive. Raw output MUST be tapped before AGC.
AGC is for human eyes only — it destroys the radiometric relationship.

---

## Key academic references (for each stage)

| Stage | Paper |
|---|---|
| Radiometric calibration (L→DN) | Meribout 2019, IEEE Sensors Journal |
| Bolometer physics, TCR, τ | Saxena et al. 2011, 2012 |
| FPN, dynamic NUC | Wolf et al. 2016 |
| MTF / PSF characterization | Boreman SPIE SC157; Marchal 2021; Adams & Strickland 2023 |
| NUC adaptive (post-correction residual) | Tendero & Gilles (ADMIRE 2024) |
| AGC plateau equalization | Silverman 1993; Li 2018 |
| Sim-to-real gap / synthetic IR | ThermalGaussian (ICLR 2025); Mayr et al. (CVPR 2024) |
| LWIR Fresnel reflectance | ter Heerdt et al. 2026 |

---

## Visual Studio solution structure

One `.sln`, four projects:

```
IRPipeline.sln
├── IRPipelineCore        (Static Library, .lib)   ← ALL physics logic lives here
├── IRPipelineLib         (Dynamic Library, .dll)  ← thin C ABI shim, wraps Core
├── IRPipelineTest        (EXE)                    ← standalone validation harness
└── IRPipelineValidator   (EXE, future)            ← per-stage unit tests
```

### Why this split

- **IRPipelineCore** (`.lib`): the real implementation. No platform ceremony.
  All pipeline stages live here. Consumed by both the DLL and the UE plugin.
- **IRPipelineLib** (`.dll`): re-exports the C ABI so external simulators, Python
  (ctypes), or other non-UE consumers can call the pipeline without linking C++.
- **UE Plugin**: links `IRPipelineCore.lib` directly via UBT (`PublicAdditionalLibraries`).
  Does NOT use the DLL. Reason: UE has its own memory allocator (`FMemory`);
  a DLL brings a second CRT heap into the process → double-free crashes at the boundary.
  Static lib inherits the consumer's CRT, so UE owns the heap completely.

### File layout

```
IRPipeline/                     ← solution root (.slnx lives here)
├── CLAUDE.md
├── shared/                     ← libraries and resources shared between all projects
│   ├── include/
│   │   └── ir_pipeline.h       ← the stable C ABI header (shared by all projects)
│   └── third_party/
│       └── stb_image_write.h   ← stb, public domain, for PNG output in tests
├── IRPipelineCore/
│   ├── IRPipelineCore.vcxproj
│   ├── lib/                    ← project-specific library dependencies
│   └── src/
│       └── ir_pipeline.cpp     ← full pipeline implementation
├── IRPipelineLib/
│   ├── IRPipelineLib.vcxproj
│   ├── lib/                    ← project-specific library dependencies
│   └── src/
│       └── ir_pipeline_dll.cpp ← thin wrapper: includes ir_pipeline.h, re-exports C ABI
├── IRPipelineTest/
│   ├── IRPipelineTest.vcxproj
│   ├── lib/                    ← project-specific library dependencies
│   └── main.cpp                ← standalone harness: synthetic radiance → PNG + timing
└── IRPipelineValidator/        (future)
    └── IRPipelineValidator.vcxproj
```

### MSVC project settings (apply to ALL configurations: Debug + Release, x64 only)

| Setting | IRPipelineCore (.lib) | IRPipelineLib (.dll) | IRPipelineTest (.exe) |
|---|---|---|---|
| Configuration Type | Static Library | Dynamic Library | Application |
| C++ Standard | C++17 | C++17 | C++17 |
| Runtime Library | /MD (Release), /MDd (Debug) | /MD / /MDd | /MD / /MDd |
| Additional Include Dirs | `$(SolutionDir)shared\include` | `$(SolutionDir)shared\include` | `$(SolutionDir)shared\include;$(SolutionDir)shared\third_party` |
| Preprocessor Definitions | — | `IR_PIPELINE_BUILD` | — |
| OpenMP | Yes (/openmp) | Yes (/openmp) | — |
| Additional Lib Dirs | — | `$(OutDir)` | `$(OutDir)` |
| Additional Dependencies | — | `IRPipelineCore.lib` | `IRPipelineLib.lib` |
| References (build order) | — | IRPipelineCore | IRPipelineLib |

**Critical**: `/MD` must match UE. Do NOT use `/MT` — that statically links the CRT
and causes double-free crashes at the DLL boundary with any other runtime.

Post-build event for IRPipelineLib and IRPipelineTest:
```
xcopy /Y "$(OutDir)ir_pipeline.dll" "$(OutDir)\"
```

---

## C ABI contract (`ir_pipeline.h`)

Stable rules — do not break these:

1. **Pure C interface** (`extern "C"`). Stable across compilers and MSVC toolchains.
   UE is built with a specific MSVC version; C++ name mangling differs, C does not.
2. **Opaque handle** (`IRPipeline*`) owns ALL per-camera state:
   temporal IIR buffer (`s_prev`), FPN maps (`fpn_gain`, `fpn_offset`), RNG seed,
   AGC state, frame counter. One handle per simulated camera.
3. **Caller owns all buffers**. The DLL only writes into caller-allocated memory.
   Never allocate in the DLL and free in UE, or vice versa.
4. **`version` field** in `IRConfig` is an ABI guard (`IR_ABI_VERSION = 1`).
   Check it in `ir_create` and return `nullptr` on mismatch before the struct grows.
5. Both output pointers (`rgba_out`, `raw_out`) are **nullable**.
   Pass `nullptr` for the output you don't need — the stage is skipped entirely.

### Key types

```c
IRStatus   ir_process(IRPipeline*, const float* radiance_in,
                      uint8_t* rgba_out,   // nullable: RGBA display
                      float*   raw_out);   // nullable: calibrated DN pre-AGC
```

---

## Performance profile (measured, Linux, 1 core, 1920×1080)

| Path | ms/frame | fps |
|---|---|---|
| Full (rgba + raw) | ~82 ms | ~12 fps |
| Raw only | ~73 ms | ~14 fps |

**Single-threaded floor** — all inner loops are `#pragma omp parallel for`.
On an 8-core desktop this projects to ~30–60 fps without further optimization.

**Bottleneck identified**: ~31 ms of the 82 ms is `std::mt19937` + `std::normal_distribution`
for per-pixel read noise. Replacing with a counter-based PRNG (e.g. Philox, xorshift128+)
brings this under 5 ms and is the **highest-priority optimization**.

PSF blur (σ=1.2 px) is negligible — the separable Gaussian is fast at typical kernel radii.

The real-time bottleneck in UE will not be this DLL — it will be the **GPU→CPU readback**
of the scene radiance render target. Use async readback with double-buffering
(accept ~1 frame latency) to avoid stalling the render thread.

---

## UE integration sketch (future work, for context)

1. **Radiance source**: `USceneCaptureComponent2D` → `R32F` or `RGBA16F` render target.
   The float values coming out of UE represent in-band radiance — pin down units
   with the owning department before writing the calibration coefficients.
2. **Readback**: render command on render thread → async GPU readback → float buffer on CPU.
3. **Process**: hand float buffer to `ir_process` on a worker thread (not game thread).
4. **Upload**: write `rgba_out` into a `UTexture2D` via `UpdateTextureRegions`, or blit
   to a render target and sample it in a material / UMG Image widget.
5. **Plugin build**: `IRPipelineCore.lib` listed in `PublicAdditionalLibraries` in
   the plugin's `.Build.cs`. No DLL, no deployment surface, no CRT mismatch.

---

## What is NOT implemented yet (known gaps)

- [ ] Counter-based PRNG for read noise (replace `mt19937` / `normal_distribution`)
- [ ] Actual electrothermal bolometer model (Saxena 2011) — currently approximated by the quadratic calibration + IIR lag
- [ ] Spectral band integration with Planck weights (stage 0 is a placeholder average)
- [ ] Dynamic FPN drift with FPA temperature (Wolf 2016) — currently FPN is static per session
- [ ] Full MTF model (Adams & Strickland 2023) — currently a Gaussian PSF stand-in
- [ ] Shutter-based NUC correction model (Nugent 2014)
- [ ] Inverse calibration LUT for apparent-temperature output
- [ ] IRPipelineValidator project (per-stage unit tests)
- [ ] UE plugin scaffold

---

## Language note

The user works in both English and Spanish. Code, identifiers, comments, and this file
are in English. Conversation can be in either language — match whatever the user uses.