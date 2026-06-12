/*
 * ir_pipeline.cpp -- Full LWIR microbolometer pipeline implementation.
 *
 * Stage order (see CLAUDE.md for physics rationale):
 *   0. Spectral band collapse (placeholder)
 *   1. PSF / MTF blur
 *   2. Radiometric calibration (L -> DN)
 *   3. Detector thermal lag (IIR)
 *   4. Residual FPN
 *   5. Read noise
 *   6. Clamp
 *   --- raw_out tapped here ---
 *   7. AGC
 *   8. Colormap -> RGBA
 */

#include "ir_pipeline.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <random>

/* ---------------------------------------------------------------------- */
/*  Internal state (opaque to callers)                                    */
/* ---------------------------------------------------------------------- */

struct IRPipeline {
    IRConfig config;

    uint32_t    npixels;       /* width * height, cached                    */
    uint32_t    frame_count;   /* frames processed so far                   */

    /* Stage 1: precomputed PSF kernel (separable, 1-D) */
    std::vector<float> psf_kernel;

    /* Stage 3: temporal IIR state */
    std::vector<float> s_prev;  /* previous-frame signal per pixel          */

    /* Stage 4: FPN maps (generated once from seed) */
    std::vector<float> fpn_gain;
    std::vector<float> fpn_offset;

    /* Stage 5: RNG for read noise */
    std::mt19937 noise_rng;

    /* Scratch buffers (reused every frame to avoid per-frame allocation) */
    std::vector<float> buf_a;
    std::vector<float> buf_b;
};

/* ---------------------------------------------------------------------- */
/*  Helper: build 1-D Gaussian kernel                                     */
/* ---------------------------------------------------------------------- */

static std::vector<float> build_gaussian_kernel(float sigma)
{
    if (sigma <= 0.0f) return {};

    int radius = static_cast<int>(std::ceil(sigma * 3.0f));
    if (radius < 1) radius = 1;

    int size = 2 * radius + 1;
    std::vector<float> k(size);
    float sum = 0.0f;

    for (int i = 0; i < size; ++i) {
        float x = static_cast<float>(i - radius);
        k[i] = std::exp(-0.5f * (x * x) / (sigma * sigma));
        sum += k[i];
    }
    for (int i = 0; i < size; ++i)
        k[i] /= sum;

    return k;
}

/* ---------------------------------------------------------------------- */
/*  Helper: separable Gaussian blur (horizontal + vertical)               */
/* ---------------------------------------------------------------------- */

static void gaussian_blur(const float* src, float* dst, float* tmp,
                          uint32_t w, uint32_t h,
                          const std::vector<float>& kernel)
{
    if (kernel.empty()) {
        if (dst != src) std::memcpy(dst, src, w * h * sizeof(float));
        return;
    }

    int radius = static_cast<int>(kernel.size()) / 2;

    /* Horizontal pass: src -> tmp */
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(h); ++y) {
        for (int x = 0; x < static_cast<int>(w); ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sx = std::clamp(x + k, 0, static_cast<int>(w) - 1);
                acc += src[y * w + sx] * kernel[k + radius];
            }
            tmp[y * w + x] = acc;
        }
    }

    /* Vertical pass: tmp -> dst */
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(h); ++y) {
        for (int x = 0; x < static_cast<int>(w); ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sy = std::clamp(y + k, 0, static_cast<int>(h) - 1);
                acc += tmp[sy * w + x] * kernel[k + radius];
            }
            dst[y * w + x] = acc;
        }
    }
}

/* ---------------------------------------------------------------------- */
/*  ir_default_config                                                     */
/* ---------------------------------------------------------------------- */

IR_API IRConfig ir_default_config(uint32_t width, uint32_t height)
{
    IRConfig c{};
    c.version           = IR_ABI_VERSION;
    c.width             = width;
    c.height            = height;
    c.bands             = 1;

    /* Stage 1 */
    c.psf_sigma         = 1.2f;

    /* Stage 2 -- placeholder coefficients */
    c.cal_a0            = 0.0f;
    c.cal_a1            = 16383.0f;   /* maps [0,1] radiance to ~14-bit DN */
    c.cal_a2            = 0.0f;

    /* Stage 3 */
    c.tau_frames        = 2.0f;

    /* Stage 4 */
    c.fpn_gain_sigma    = 0.02f;
    c.fpn_offset_sigma  = 5.0f;
    c.fpn_seed          = 42;

    /* Stage 5 */
    c.netd_dn           = 10.0f;
    c.noise_seed        = 123;

    /* Stage 6 */
    c.dn_max            = 16383.0f;

    /* Stage 7 */
    c.agc_mode          = IR_AGC_LINEAR_MINMAX;
    c.agc_plateau_pct   = 5.0f;

    /* Stage 8 */
    c.colormap          = IR_CMAP_WHITE_HOT;

    return c;
}

/* ---------------------------------------------------------------------- */
/*  ir_create                                                             */
/* ---------------------------------------------------------------------- */

IR_API IRPipeline* ir_create(const IRConfig* config)
{
    if (!config) return nullptr;
    if (config->version != IR_ABI_VERSION) return nullptr;
    if (config->width == 0 || config->height == 0) return nullptr;

    IRPipeline* p = new (std::nothrow) IRPipeline();
    if (!p) return nullptr;

    p->config      = *config;
    p->npixels     = config->width * config->height;
    p->frame_count = 0;

    /* Stage 1: PSF kernel */
    p->psf_kernel = build_gaussian_kernel(config->psf_sigma);

    /* Stage 3: temporal IIR -- zero-init (first frame seeds the buffer) */
    p->s_prev.resize(p->npixels, 0.0f);

    /* Stage 4: generate FPN maps from seed */
    p->fpn_gain.resize(p->npixels);
    p->fpn_offset.resize(p->npixels);
    {
        std::mt19937 fpn_rng(config->fpn_seed);
        std::normal_distribution<float> gain_dist(1.0f, config->fpn_gain_sigma);
        std::normal_distribution<float> offset_dist(0.0f, config->fpn_offset_sigma);

        for (uint32_t i = 0; i < p->npixels; ++i) {
            p->fpn_gain[i]   = gain_dist(fpn_rng);
            p->fpn_offset[i] = offset_dist(fpn_rng);
        }
    }

    /* Stage 5: noise RNG */
    p->noise_rng.seed(config->noise_seed);

    /* Scratch buffers */
    p->buf_a.resize(p->npixels);
    p->buf_b.resize(p->npixels);

    return p;
}

/* ---------------------------------------------------------------------- */
/*  Stage 7: AGC                                                          */
/* ---------------------------------------------------------------------- */

static void agc_linear_minmax(const float* src, uint8_t* dst, uint32_t n)
{
    float lo = src[0], hi = src[0];

    for (uint32_t i = 1; i < n; ++i) {
        if (src[i] < lo) lo = src[i];
        if (src[i] > hi) hi = src[i];
    }

    float range = hi - lo;
    if (range < 1e-6f) range = 1.0f;
    float scale = 255.0f / range;

    #pragma omp parallel for
    for (int i = 0; i < static_cast<int>(n); ++i) {
        float v = (src[i] - lo) * scale;
        dst[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    }
}

static void agc_plateau_eq(const float* src, uint8_t* dst, uint32_t n,
                           float plateau_pct, float dn_max)
{
    /* Build histogram (256 bins spanning [0, dn_max]) */
    const int NBINS = 256;
    std::vector<uint32_t> hist(NBINS, 0);

    float bin_scale = (NBINS - 1) / dn_max;
    for (uint32_t i = 0; i < n; ++i) {
        int bin = static_cast<int>(std::clamp(src[i] * bin_scale, 0.0f, (float)(NBINS - 1)));
        hist[bin]++;
    }

    /* Plateau clip */
    uint32_t clip_val = static_cast<uint32_t>(n * plateau_pct / 100.0f);
    if (clip_val < 1) clip_val = 1;
    for (int i = 0; i < NBINS; ++i) {
        if (hist[i] > clip_val) hist[i] = clip_val;
    }

    /* CDF */
    std::vector<uint32_t> cdf(NBINS);
    cdf[0] = hist[0];
    for (int i = 1; i < NBINS; ++i)
        cdf[i] = cdf[i - 1] + hist[i];

    uint32_t cdf_min = cdf[0];
    uint32_t cdf_max = cdf[NBINS - 1];
    float cdf_range = static_cast<float>(cdf_max - cdf_min);
    if (cdf_range < 1.0f) cdf_range = 1.0f;

    /* Map */
    #pragma omp parallel for
    for (int i = 0; i < static_cast<int>(n); ++i) {
        int bin = static_cast<int>(std::clamp(src[i] * bin_scale, 0.0f, (float)(NBINS - 1)));
        float v = (static_cast<float>(cdf[bin] - cdf_min) / cdf_range) * 255.0f;
        dst[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    }
}

/* ---------------------------------------------------------------------- */
/*  Stage 8: Colormap                                                     */
/* ---------------------------------------------------------------------- */

static void apply_colormap(const uint8_t* intensity, uint8_t* rgba,
                           uint32_t n, IRColormap cmap)
{
    #pragma omp parallel for
    for (int i = 0; i < static_cast<int>(n); ++i) {
        uint8_t v = intensity[i];
        uint8_t r, g, b;

        switch (cmap) {
        case IR_CMAP_WHITE_HOT:
            r = g = b = v;
            break;

        case IR_CMAP_BLACK_HOT:
            r = g = b = static_cast<uint8_t>(255 - v);
            break;

        case IR_CMAP_IRONBOW: {
            /* Simplified ironbow: cold=blue, mid=red, hot=yellow/white */
            float t = v / 255.0f;
            if (t < 0.33f) {
                float s = t / 0.33f;
                r = static_cast<uint8_t>(s * 180);
                g = 0;
                b = static_cast<uint8_t>((1.0f - s) * 80 + s * 30);
            } else if (t < 0.66f) {
                float s = (t - 0.33f) / 0.33f;
                r = static_cast<uint8_t>(180 + s * 75);
                g = static_cast<uint8_t>(s * 160);
                b = static_cast<uint8_t>(30 * (1.0f - s));
            } else {
                float s = (t - 0.66f) / 0.34f;
                r = 255;
                g = static_cast<uint8_t>(160 + s * 95);
                b = static_cast<uint8_t>(s * 200);
            }
            break;
        }
        default:
            r = g = b = v;
            break;
        }

        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = 255;
    }
}

/* ---------------------------------------------------------------------- */
/*  ir_process                                                            */
/* ---------------------------------------------------------------------- */

IR_API IRStatus ir_process(IRPipeline* p,
                           const float* radiance_in,
                           uint8_t* rgba_out,
                           float* raw_out)
{
    if (!p) return IR_ERR_BAD_CONFIG;
    if (!radiance_in) return IR_ERR_NULL_INPUT;
    if (!rgba_out && !raw_out) return IR_ERR_NO_OUTPUT;

    const IRConfig& cfg = p->config;
    const uint32_t  n   = p->npixels;
    const uint32_t  w   = cfg.width;
    const uint32_t  h   = cfg.height;

    float* a = p->buf_a.data();
    float* b = p->buf_b.data();

    /* ---- Stage 0: Spectral band collapse (placeholder) ---- */
    /* For bands == 1, input is already band-integrated. */
    const float* current = radiance_in;

    /* ---- Stage 1: PSF / optics blur ---- */
    if (!p->psf_kernel.empty()) {
        gaussian_blur(current, a, b, w, h, p->psf_kernel);
        current = a;
    }

    /* ---- Stage 2: Radiometric calibration  DN = a0 + a1*L + a2*L^2 ---- */
    {
        float* dst = (current == a) ? b : a;
        #pragma omp parallel for
        for (int i = 0; i < static_cast<int>(n); ++i) {
            float L = current[i];
            dst[i] = cfg.cal_a0 + cfg.cal_a1 * L + cfg.cal_a2 * L * L;
        }
        current = dst;
    }

    /* ---- Stage 3: Detector thermal lag (1st-order IIR) ---- */
    if (cfg.tau_frames > 0.0f) {
        float alpha = 1.0f / (1.0f + cfg.tau_frames);
        float* dst = (current == a) ? b : a;

        if (p->frame_count == 0) {
            /* Seed the IIR with the first frame */
            std::memcpy(p->s_prev.data(), current, n * sizeof(float));
            std::memcpy(dst, current, n * sizeof(float));
        } else {
            #pragma omp parallel for
            for (int i = 0; i < static_cast<int>(n); ++i) {
                float s = alpha * current[i] + (1.0f - alpha) * p->s_prev[i];
                p->s_prev[i] = s;
                dst[i] = s;
            }
        }
        current = dst;
    }

    /* ---- Stage 4: Residual FPN ---- */
    {
        float* dst = (current == a) ? b : a;
        #pragma omp parallel for
        for (int i = 0; i < static_cast<int>(n); ++i) {
            dst[i] = current[i] * p->fpn_gain[i] + p->fpn_offset[i];
        }
        current = dst;
    }

    /* ---- Stage 5: Read noise ---- */
    {
        float* dst = (current == a) ? b : a;
        std::normal_distribution<float> noise_dist(0.0f, cfg.netd_dn);

        /* NOTE: mt19937 is the bottleneck here. Replace with counter-based
         * PRNG (Philox / xorshift128+) for production performance. */
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = current[i] + noise_dist(p->noise_rng);
        }
        current = dst;
    }

    /* ---- Stage 6: Clamp to [0, dn_max] ---- */
    {
        float* dst = (current == a) ? b : a;
        #pragma omp parallel for
        for (int i = 0; i < static_cast<int>(n); ++i) {
            dst[i] = std::clamp(current[i], 0.0f, cfg.dn_max);
        }
        current = dst;
    }

    /* ---- raw_out tap (pre-AGC) ---- */
    if (raw_out) {
        std::memcpy(raw_out, current, n * sizeof(float));
    }

    /* ---- Display path (stages 7-8) ---- */
    if (rgba_out) {
        /* We need a temporary buffer for 8-bit intensity */
        std::vector<uint8_t> intensity(n);

        /* Stage 7: AGC */
        switch (cfg.agc_mode) {
        case IR_AGC_PLATEAU_EQ:
            agc_plateau_eq(current, intensity.data(), n,
                           cfg.agc_plateau_pct, cfg.dn_max);
            break;
        case IR_AGC_LINEAR_MINMAX:
        default:
            agc_linear_minmax(current, intensity.data(), n);
            break;
        }

        /* Stage 8: Colormap */
        apply_colormap(intensity.data(), rgba_out, n, cfg.colormap);
    }

    p->frame_count++;
    return IR_OK;
}

/* ---------------------------------------------------------------------- */
/*  ir_destroy                                                            */
/* ---------------------------------------------------------------------- */

IR_API void ir_destroy(IRPipeline* pipeline)
{
    delete pipeline;
}
