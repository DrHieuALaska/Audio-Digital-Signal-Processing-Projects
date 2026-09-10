/*
 * Correctness test for dsp_core/stft.c.
 *
 * Feeds a signal through stft_analyze() -> stft_synthesize() with an
 * IDENTITY spectrum modification (i.e. no effect applied) and checks
 * that the output matches the input, after accounting for the
 * (fft_size - hop_size) samples of startup latency inherent to any
 * overlap-add STFT pipeline.
 *
 * If this test fails, something is wrong with the COLA (constant
 * overlap-add) math itself — before trusting any effect built on top
 * of stft.c (EQ, noise gate, phase vocoder), this must pass.
 *
 * Run with: make test_stft
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp_core/stft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_SIZE 1024
#define HOP_SIZE (FFT_SIZE / 2)
#define NUM_HOPS 40

int main(void) {
    stft_t s;
    if (stft_init(&s, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) {
        fprintf(stderr, "stft_init failed\n");
        return 1;
    }

    size_t total_samples = NUM_HOPS * HOP_SIZE;
    float *input = (float *)malloc(total_samples * sizeof(float));
    float *output = (float *)malloc(total_samples * sizeof(float));

    /* A simple test tone: not a multiple of the frame size, so it
     * genuinely exercises windowing/overlap rather than lining up
     * suspiciously with frame boundaries. */
    double freq = 440.0, sr = 44100.0;
    for (size_t i = 0; i < total_samples; i++) {
        input[i] = (float)(0.6 * sin(2.0 * M_PI * freq * (double)i / sr));
    }

    for (size_t h = 0; h < NUM_HOPS; h++) {
        stft_analyze(&s, &input[h * HOP_SIZE]);
        /* No modification to s.spectrum: this is the identity case. */
        stft_synthesize(&s, &output[h * HOP_SIZE]);
    }

    /* First (fft_size - hop_size) output samples are the ramp-up
     * transient while the analysis buffer is still filling — skip them. */
    size_t latency = FFT_SIZE - HOP_SIZE;
    size_t compare_len = total_samples - latency;

    float max_err = 0.0f, sum_sq_err = 0.0f;
    for (size_t i = 0; i < compare_len; i++) {
        float err = output[i + latency] - input[i];
        float aerr = fabsf(err);
        if (aerr > max_err) max_err = aerr;
        sum_sq_err += err * err;
    }
    float rmse = sqrtf(sum_sq_err / (float)compare_len);

    printf("Latency (samples)   = %zu\n", latency);
    printf("Compared samples    = %zu\n", compare_len);
    printf("Max abs error       = %e\n", max_err);
    printf("RMSE                = %e\n", rmse);

    const float tolerance = 1e-4f;
    if (max_err > tolerance) {
        printf("FAIL: max error exceeds tolerance (%e)\n", tolerance);
        free(input); free(output); stft_free(&s);
        return 1;
    }
    printf("PASS: overlap-add reconstruction matches input\n");

    free(input);
    free(output);
    stft_free(&s);
    return 0;
}
