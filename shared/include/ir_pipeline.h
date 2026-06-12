/*
 * ir_pipeline.h -- Stable C ABI for the LWIR microbolometer simulation pipeline.
 *
 * This header is shared by all projects in the solution:
 *   IRPipelineCore (.lib)  -- implements everything
 *   IRPipelineLib  (.dll)  -- re-exports for external consumers (Python, etc.)
 *   IRPipelineTest (.exe)  -- standalone validation harness
 *
 * Rules:
 *   1. Pure C interface (extern "C").  No C++ types cross this boundary.
 *   2. Opaque handle (IRPipeline*) owns all per-camera state.
 *   3. Caller owns all buffers.  The library never allocates for the caller.
 *   4. ABI version field in IRConfig guards against struct-layout mismatches.
 *   5. Both output pointers (rgba_out, raw_out) are nullable -- skip what you
 *      don't need.
 */

#ifndef IR_PIPELINE_H
#define IR_PIPELINE_H

#include <stdint.h>

/* ---------------------------------------------------------------------- */
/*  DLL export / import macros                                            */
/* ---------------------------------------------------------------------- */

#ifdef IR_PIPELINE_BUILD
    /* Building the DLL (IRPipelineLib) */
    #define IR_API __declspec(dllexport)
#elif defined(IR_PIPELINE_DLL)
    /* Consuming the DLL */
    #define IR_API __declspec(dllimport)
#else
    /* Static library (IRPipelineCore) or direct linkage */
    #define IR_API
#endif

/* ---------------------------------------------------------------------- */
/*  ABI version                                                           */
/* ---------------------------------------------------------------------- */

#define IR_ABI_VERSION 1

/* ---------------------------------------------------------------------- */
/*  Status codes                                                          */
/* ---------------------------------------------------------------------- */

typedef enum IRStatus {
    IR_OK                 =  0,
    IR_ERR_NULL_INPUT     = -1,
    IR_ERR_BAD_CONFIG     = -2,
    IR_ERR_ABI_MISMATCH   = -3,
    IR_ERR_NO_OUTPUT      = -4   /* both output pointers are null */
} IRStatus;

/* ---------------------------------------------------------------------- */
/*  AGC mode                                                              */
/* ---------------------------------------------------------------------- */

typedef enum IRAgcMode {
    IR_AGC_LINEAR_MINMAX  = 0,
    IR_AGC_PLATEAU_EQ     = 1
} IRAgcMode;

/* ---------------------------------------------------------------------- */
/*  Colormap                                                              */
/* ---------------------------------------------------------------------- */

typedef enum IRColormap {
    IR_CMAP_WHITE_HOT = 0,
    IR_CMAP_BLACK_HOT = 1,
    IR_CMAP_IRONBOW   = 2
} IRColormap;

/* ---------------------------------------------------------------------- */
/*  Configuration                                                         */
/* ---------------------------------------------------------------------- */

typedef struct IRConfig {
    uint32_t    version;        /* must equal IR_ABI_VERSION                  */

    /* Sensor geometry */
    uint32_t    width;          /* pixels                                     */
    uint32_t    height;         /* pixels                                     */
    uint32_t    bands;          /* spectral bands (1 = band-integrated)       */

    /* Stage 1: PSF / optics blur */
    float       psf_sigma;      /* Gaussian sigma in pixels (0 = skip)       */

    /* Stage 2: Radiometric calibration  DN = a0 + a1*L + a2*L^2            */
    float       cal_a0;
    float       cal_a1;
    float       cal_a2;

    /* Stage 3: Detector thermal lag (1st-order IIR)                         */
    float       tau_frames;     /* time constant in frames (0 = no lag)      */

    /* Stage 4: Residual FPN maps                                            */
    float       fpn_gain_sigma;   /* std-dev of per-pixel gain map           */
    float       fpn_offset_sigma; /* std-dev of per-pixel offset map (DN)    */
    uint32_t    fpn_seed;         /* RNG seed for map generation             */

    /* Stage 5: Read noise                                                   */
    float       netd_dn;        /* noise std-dev in DN units                 */
    uint32_t    noise_seed;     /* RNG seed for read noise                   */

    /* Stage 6: Clamp                                                        */
    float       dn_max;         /* maximum DN value                          */

    /* Stage 7: AGC                                                          */
    IRAgcMode   agc_mode;
    float       agc_plateau_pct; /* plateau %  (for IR_AGC_PLATEAU_EQ)       */

    /* Stage 8: Colormap                                                     */
    IRColormap  colormap;

} IRConfig;

/* ---------------------------------------------------------------------- */
/*  Opaque handle                                                         */
/* ---------------------------------------------------------------------- */

typedef struct IRPipeline IRPipeline;

/* ---------------------------------------------------------------------- */
/*  API functions                                                         */
/* ---------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ir_create  -- Allocate and initialise a pipeline instance.
 *               Returns NULL on ABI mismatch or invalid config.
 */
IR_API IRPipeline*  ir_create(const IRConfig* config);

/*
 * ir_process -- Run the full pipeline on one frame of radiance data.
 *
 *   radiance_in : width * height floats, band-integrated spectral radiance
 *   rgba_out    : width * height * 4 bytes (RGBA), or NULL to skip display path
 *   raw_out     : width * height floats (calibrated DN pre-AGC), or NULL to skip
 */
IR_API IRStatus     ir_process(IRPipeline* pipeline,
                               const float* radiance_in,
                               uint8_t* rgba_out,
                               float* raw_out);

/*
 * ir_destroy -- Free all resources owned by the pipeline instance.
 */
IR_API void         ir_destroy(IRPipeline* pipeline);

/*
 * ir_default_config -- Fill a config struct with sensible defaults.
 *                      Caller can then override individual fields.
 */
IR_API IRConfig     ir_default_config(uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* IR_PIPELINE_H */
