/*
 * main.cpp -- Standalone test harness for the LWIR microbolometer pipeline.
 *
 * Generates a synthetic radiance frame (gradient + hot spot), runs the full
 * pipeline, writes output PNG(s) and prints per-frame timing.
 */

#define IR_PIPELINE_DLL
#include "ir_pipeline.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <vector>

/* ---------------------------------------------------------------------- */
/*  Synthetic radiance scene                                              */
/* ---------------------------------------------------------------------- */

static void generate_radiance(float* buf, uint32_t w, uint32_t h)
{
    /* Horizontal gradient [0.1 .. 0.9] with a hot circular spot */
    float cx = w * 0.6f;
    float cy = h * 0.4f;
    float spot_r = static_cast<float>(std::min(w, h)) * 0.08f;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float base = 0.1f + 0.8f * (static_cast<float>(x) / (w - 1));

            float dx = x - cx;
            float dy = y - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < spot_r) {
                float t = 1.0f - (dist / spot_r);
                base += 0.4f * t * t;  /* quadratic hot spot */
            }

            buf[y * w + x] = base;
        }
    }
}

/* ---------------------------------------------------------------------- */
/*  main                                                                  */
/* ---------------------------------------------------------------------- */

int main()
{
    const uint32_t W = 640;
    const uint32_t H = 480;

    printf("IRPipeline Test Harness\n");
    printf("Resolution: %u x %u\n\n", W, H);

    /* Setup */
    IRConfig cfg = ir_default_config(W, H);
    IRPipeline* pipeline = ir_create(&cfg);
    if (!pipeline) {
        fprintf(stderr, "ERROR: ir_create failed\n");
        return 1;
    }

    /* Allocate buffers */
    std::vector<float>   radiance(W * H);
    std::vector<uint8_t> rgba(W * H * 4);
    std::vector<float>   raw(W * H);

    /* Generate synthetic scene */
    generate_radiance(radiance.data(), W, H);

    /* Process multiple frames to exercise temporal lag */
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

    /* Write last frame as PNG */
    if (stbi_write_png("output_rgba.png", W, H, 4, rgba.data(), W * 4)) {
        printf("\nWrote output_rgba.png\n");
    } else {
        fprintf(stderr, "WARNING: failed to write output_rgba.png\n");
    }

    /* Write raw DN as grayscale PNG (normalized to 8-bit for visualization) */
    {
        float lo = raw[0], hi = raw[0];
        for (uint32_t i = 1; i < W * H; ++i) {
            lo = std::min(lo, raw[i]);
            hi = std::max(hi, raw[i]);
        }
        float range = hi - lo;
        if (range < 1e-6f) range = 1.0f;

        std::vector<uint8_t> gray(W * H);
        for (uint32_t i = 0; i < W * H; ++i) {
            gray[i] = static_cast<uint8_t>(
                std::clamp((raw[i] - lo) / range * 255.0f, 0.0f, 255.0f));
        }

        if (stbi_write_png("output_raw.png", W, H, 1, gray.data(), W)) {
            printf("Wrote output_raw.png\n");
        }
    }

    ir_destroy(pipeline);
    printf("\nDone.\n");
    return 0;
}
