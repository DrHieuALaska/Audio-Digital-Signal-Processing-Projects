/*
 * Correctness test for dsp_core/pitch_detect.c.
 *
 * Feeds synthetic sine waves at known frequencies through the detector
 * and checks the reported pitch is close to the true value — the
 * whole point of the phase-vocoder refinement is sub-bin accuracy, so
 * we hold it to a much tighter tolerance than the raw bin spacing
 * (sample_rate/fft_size) would allow.
 *
 * Run with: make run-test-pitch-detect
 */
#include <stdio.h>
#include <math.h>
#include "dsp_core/pitch_detect.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 44100.0
#define FFT_SIZE    2048
#define HOP_SIZE    (FFT_SIZE / 2)

static double test_frequency(double freq_hz) {
    pitch_detector_t pd;
    pitch_detector_init(&pd, FFT_SIZE, HOP_SIZE, SAMPLE_RATE, 50.0f, 2000.0f);

    /* Feed enough hops for the phase-difference estimate to settle:
     * first call has no valid previous phase, so discard it. */
    int num_hops = 30;
    double last_estimate = 0.0;
    float buf[HOP_SIZE];
    static double phase_accum = 0.0;

    for (int h = 0; h < num_hops; h++) {
        for (int i = 0; i < HOP_SIZE; i++) {
            buf[i] = 0.7f * (float)sin(phase_accum);
            phase_accum += 2.0 * M_PI * freq_hz / SAMPLE_RATE;
        }
        double est = pitch_detector_process(&pd, buf);
        if (h > 2) { /* skip startup transient */
            last_estimate = est;
        }
    }

    pitch_detector_free(&pd);
    return last_estimate;
}

int main(void) {
    double test_freqs[] = {110.0, 220.0, 440.0, 880.0, 329.63 /* E4 */};
    int n_tests = (int)(sizeof(test_freqs) / sizeof(test_freqs[0]));
    int failures = 0;

    for (int i = 0; i < n_tests; i++) {
        double true_freq = test_freqs[i];
        double detected = test_frequency(true_freq);
        double error_hz = fabs(detected - true_freq);
        double error_cents = 1200.0 * log2(detected / true_freq);

        printf("true=%.2f Hz  detected=%.4f Hz  error=%.4f Hz (%.2f cents)\n",
               true_freq, detected, error_hz, error_cents);

        /* 5 cents is a tight, musically-meaningful tolerance --
         * well below what a human ear perceives as out of tune. */
        if (fabs(error_cents) > 5.0) {
            failures++;
        }
    }

    if (failures > 0) {
        printf("FAIL: %d/%d frequencies exceeded 5-cent tolerance\n", failures, n_tests);
        return 1;
    }
    printf("PASS: all %d frequencies detected within 5 cents\n", n_tests);
    return 0;
}
