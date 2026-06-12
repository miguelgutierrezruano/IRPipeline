/*
 * main.cpp -- Standalone test harness for the LWIR microbolometer pipeline.
 *
 * Loads a FLIR raw thermal JPEG, reverse-engineers approximate Planck radiance
 * in the 8-14 um LWIR band, then runs the full pipeline.  Outputs:
 *   - output_radiance.png : the reconstructed radiance field (grayscale viz)
 *   - output_rgba.png     : pipeline display output (ironbow colormap)
 *   - output_raw.png      : pipeline raw DN pre-AGC (grayscale viz)
 */

#include "ir_pipeline.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---------------------------------------------------------------------- */
/*  Planck radiance for LWIR band (8-14 um)                               */
/* ---------------------------------------------------------------------- */

/* Physical constants */
static constexpr double H_PLANCK = 6.62607015e-34;  /* J·s   */
static constexpr double C_LIGHT  = 2.99792458e8;    /* m/s   */
static constexpr double K_BOLTZ  = 1.380649e-23;    /* J/K   */

/* Planck spectral radiance B(lambda, T)  [W·m^-2·sr^-1·m^-1] */
static double planck_spectral(double lambda_m, double T_kelvin)
{
    double c1 = 2.0 * H_PLANCK * C_LIGHT * C_LIGHT;
    double c2 = H_PLANCK * C_LIGHT / (K_BOLTZ * T_kelvin);
    return c1 / (lambda_m * lambda_m * lambda_m * lambda_m * lambda_m
                 * (std::exp(c2 / lambda_m) - 1.0));
}

/*
 * Band-integrated radiance over [8, 14] um using trapezoidal rule.
 * Returns L in W·m^-2·sr^-1.
 */
static float planck_band_radiance(float T_kelvin)
{
    const double lam_min = 8.0e-6;   /* 8 um  */
    const double lam_max = 14.0e-6;  /* 14 um */
    const int    N_STEPS = 120;
    const double dlam    = (lam_max - lam_min) / N_STEPS;

    double integral = 0.0;
    for (int i = 0; i <= N_STEPS; ++i) {
        double lam = lam_min + i * dlam;
        double w   = (i == 0 || i == N_STEPS) ? 0.5 : 1.0;
        integral  += w * planck_spectral(lam, static_cast<double>(T_kelvin));
    }
    integral *= dlam;

    return static_cast<float>(integral);
}

/* ---------------------------------------------------------------------- */
/*  Load FLIR raw thermal image -> radiance field                         */
/* ---------------------------------------------------------------------- */

/*
 * The FLIR raw thermal JPEG is a pseudo-color or grayscale rendering of
 * the thermal data.  The color bar in the image shows 23.1 C to 60.0 C.
 * We map pixel luminance [0..255] -> temperature -> Planck radiance.
 */
static bool load_thermal_to_radiance(const char* path,
                                     std::vector<float>& radiance,
                                     int& out_w, int& out_h,
                                     float& L_min, float& L_max)
{
    int w, h, ch;
    unsigned char* img = stbi_load(path, &w, &h, &ch, 0);
    if (!img) {
        fprintf(stderr, "ERROR: cannot load '%s': %s\n", path, stbi_failure_reason());
        return false;
    }

    printf("Loaded '%s': %d x %d, %d channels\n", path, w, h, ch);

    /* Temperature range from the FLIR color bar */
    const float T_MIN_C = 23.1f;
    const float T_MAX_C = 60.0f;
    const float T_MIN_K = T_MIN_C + 273.15f;
    const float T_MAX_K = T_MAX_C + 273.15f;

    out_w = w;
    out_h = h;
    radiance.resize(w * h);

    L_min =  1e30f;
    L_max = -1e30f;

    for (int i = 0; i < w * h; ++i) {
        /* Extract luminance from pixel */
        float lum;
        if (ch == 1) {
            lum = img[i] / 255.0f;
        } else {
            /* Rec. 709 luminance from RGB */
            float r = img[i * ch + 0] / 255.0f;
            float g = img[i * ch + 1] / 255.0f;
            float b = img[i * ch + 2] / 255.0f;
            lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        /* Map luminance to temperature (linear interpolation) */
        float T_K = T_MIN_K + lum * (T_MAX_K - T_MIN_K);

        /* Convert temperature to band-integrated Planck radiance */
        float L = planck_band_radiance(T_K);
        radiance[i] = L;

        if (L < L_min) L_min = L;
        if (L > L_max) L_max = L;
    }

    stbi_image_free(img);
    return true;
}

/* ---------------------------------------------------------------------- */
/*  Write a float buffer as a normalized grayscale PNG                    */
/* ---------------------------------------------------------------------- */

static void write_float_png(const char* path, const float* buf,
                            int w, int h, float lo, float hi)
{
    float range = hi - lo;
    if (range < 1e-6f) range = 1.0f;

    std::vector<uint8_t> gray(w * h);
    for (int i = 0; i < w * h; ++i) {
        float v = (buf[i] - lo) / range * 255.0f;
        gray[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    }

    if (stbi_write_png(path, w, h, 1, gray.data(), w))
        printf("Wrote %s\n", path);
    else
        fprintf(stderr, "WARNING: failed to write %s\n", path);
}

/* ---------------------------------------------------------------------- */
/*  main                                                                  */
/* ---------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
    /* Resolve path relative to the exe so it works from any working dir */
    std::string thermal_path;
    if (argc > 1) {
        thermal_path = argv[1];
    } else {
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        /* Strip exe filename to get directory */
        char* last_sep = strrchr(exe_dir, '\\');
        if (last_sep) *(last_sep + 1) = '\0';
        thermal_path = std::string(exe_dir) + "..\\..\\shared\\resources\\FLIR5292_rawthermal.jpg";
    }

    printf("IRPipeline Test Harness\n");
    printf("=======================\n\n");

    /* ---- Load thermal image and convert to radiance ---- */
    std::vector<float> radiance;
    int W, H;
    float L_min, L_max;

    if (!load_thermal_to_radiance(thermal_path.c_str(), radiance, W, H, L_min, L_max))
        return 1;

    printf("Radiance range: %.3f to %.3f W/(m^2 sr)\n", L_min, L_max);
    printf("  (corresponds to %.1f C to %.1f C)\n\n", 23.1f, 60.0f);

    /* ---- Configure pipeline for this radiance range ---- */
    IRConfig cfg = ir_default_config(static_cast<uint32_t>(W),
                                     static_cast<uint32_t>(H));

    /*
     * Calibration: map [L_min, L_max] -> [0, dn_max] linearly.
     *   DN = a0 + a1 * L
     *   a1 = dn_max / (L_max - L_min)
     *   a0 = -a1 * L_min
     */
    float dn_max = cfg.dn_max;
    cfg.cal_a1 = dn_max / (L_max - L_min);
    cfg.cal_a0 = -cfg.cal_a1 * L_min;
    cfg.cal_a2 = 0.0f;

    /* Use ironbow to match the FLIR thermal palette */
    cfg.colormap = IR_CMAP_IRONBOW;

    /* Moderate noise and FPN for realistic look */
    cfg.netd_dn         = 8.0f;
    cfg.fpn_gain_sigma   = 0.015f;
    cfg.fpn_offset_sigma = 3.0f;
    cfg.psf_sigma        = 0.8f;
    cfg.tau_frames       = 1.5f;

    IRPipeline* pipeline = ir_create(&cfg);
    if (!pipeline) {
        fprintf(stderr, "ERROR: ir_create failed\n");
        return 1;
    }

    /* ---- Write radiance field visualization ---- */
    write_float_png("output_radiance.png", radiance.data(), W, H, L_min, L_max);

    /* ---- Allocate output buffers ---- */
    uint32_t npix = static_cast<uint32_t>(W * H);
    std::vector<uint8_t> rgba(npix * 4);
    std::vector<float>   raw(npix);

    /* ---- Process frames (run a few to let IIR settle) ---- */
    const int NUM_FRAMES = 5;
    for (int f = 0; f < NUM_FRAMES; ++f) {
        auto t0 = std::chrono::high_resolution_clock::now();

        IRStatus status = ir_process(pipeline, radiance.data(),
                                     rgba.data(), raw.data());

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (status != IR_OK) {
            fprintf(stderr, "ERROR: ir_process returned %d on frame %d\n",
                    status, f);
            ir_destroy(pipeline);
            return 1;
        }

        printf("Frame %d: %.2f ms (%.1f fps)\n", f, ms, 1000.0 / ms);
    }

    /* ---- Write pipeline outputs ---- */
    printf("\n");

    if (stbi_write_png("output_rgba.png", W, H, 4, rgba.data(), W * 4))
        printf("Wrote output_rgba.png  (ironbow display)\n");

    write_float_png("output_raw.png", raw.data(), W, H, 0.0f, dn_max);

    ir_destroy(pipeline);
    printf("\nDone.\n");
    return 0;
}
