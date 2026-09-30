/*
 * Correctness test for dsp_core/pitch_shift.c.
 *
 * Generates a pure sine tone, pitch-shifts it, then independently
 * measures the dominant frequency of the OUTPUT using a separate FFT
 * with parabolic peak interpolation (a different, simpler technique
 * than the phase vocoder used internally — deliberately independent,
 * so this test isn't just checking the algorithm agrees with itself).
 *
 * Run with: make run-test-pitch-shift
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp_core/pitch_shift.h"
#include "dsp_core/fft.h"
#include "dsp_core/window.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 44100.0
#define DURATION_SEC 1.0
#define MEASURE_FFT_SIZE 8192

/* Measures the dominant frequency in a segment via FFT + parabolic
 * interpolation for sub-bin accuracy. */
static double measure_dominant_freq(const float *signal, size_t offset) {
    float w[MEASURE_FFT_SIZE];
    cplx_t spec[MEASURE_FFT_SIZE];

    window_generate(w, MEASURE_FFT_SIZE, WINDOW_HANN);
    window_apply_to_complex(&signal[offset], w, spec, MEASURE_FFT_SIZE);
    fft_forward(spec, MEASURE_FFT_SIZE);

    float mag[MEASURE_FFT_SIZE / 2 + 1];
    fft_magnitude(spec, mag, MEASURE_FFT_SIZE / 2 + 1);

    size_t peak = 1;
    float peak_mag = 0.0f;
    for (size_t k = 1; k < MEASURE_FFT_SIZE / 2; k++) {
        if (mag[k] > peak_mag) { peak_mag = mag[k]; peak = k; }
    }

    float a = mag[peak - 1], b = mag[peak], c = mag[peak + 1];
    float denom = (a - 2.0f * b + c);
    float delta = (fabsf(denom) > 1e-9f) ? 0.5f * (a - c) / denom : 0.0f;
    double refined_bin = (double)peak + (double)delta;

    return refined_bin * SAMPLE_RATE / (double)MEASURE_FFT_SIZE;
}

static int run_case(double input_freq, float semitones) {
    size_t n = (size_t)(SAMPLE_RATE * DURATION_SEC);
    float *input = (float *)malloc(n * sizeof(float));
    float *output = (float *)malloc(n * sizeof(float));

    for (size_t i = 0; i < n; i++) {
        input[i] = 0.7f * (float)sin(2.0 * M_PI * input_freq * (double)i / SAMPLE_RATE);
    }

    if (pitch_shift_buffer(input, n, SAMPLE_RATE, semitones, output) != 0) {
        printf("pitch_shift_buffer failed\n");
        free(input); free(output);
        return 1;
    }

    double expected_freq = input_freq * pow(2.0, (double)semitones / 12.0);
    /* Measure from the middle of the signal to avoid startup/tail
     * transients inherent to any block-based STFT processing. */
    size_t offset = n / 2 - MEASURE_FFT_SIZE / 2;
    double measured_freq = measure_dominant_freq(output, offset);

    double error_pct = 100.0 * fabs(measured_freq - expected_freq) / expected_freq;
    printf("input=%.2fHz  shift=%+.1f semitones  expected=%.2fHz  measured=%.2fHz  error=%.2f%%\n",
           input_freq, semitones, expected_freq, measured_freq, error_pct);

    free(input);
    free(output);

    /* Linear-interpolation resampling introduces some slop; 2% is
     * generous but still clearly confirms the shift direction and
     * magnitude are correct, not just "roughly the same frequency". */
    return (error_pct > 2.0) ? 1 : 0;
}

int main(void) {
    int failures = 0;
    failures += run_case(440.0, 12.0);   /* one octave up */
    failures += run_case(440.0, -12.0);  /* one octave down */
    failures += run_case(220.0, 7.0);    /* a perfect fifth up */
    failures += run_case(330.0, 0.0);    /* no shift: sanity check */

    if (failures > 0) {
        printf("FAIL: %d case(s) exceeded tolerance\n", failures);
        return 1;
    }
    printf("PASS: all pitch shift cases within tolerance\n");
    return 0;
}
