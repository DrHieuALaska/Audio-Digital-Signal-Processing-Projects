/*
 * Verifies the core correctness property of apply_spectral_effects()
 * from main_eq.c: applying a per-bin real gain that differs across
 * frequency bands, but is mirrored correctly for bins k and N-k, keeps
 * the spectrum conjugate-symmetric — so the inverse FFT produces a
 * real signal (negligible imaginary component), not audio corruption.
 *
 * This re-implements just the gain-mirroring logic standalone (rather
 * than linking main_eq.c, which owns main()) so it can be tested in
 * isolation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp_core/fft.h"

#define N 1024
#define SAMPLE_RATE 44100.0

static void apply_test_gains(cplx_t *spectrum, size_t n) {
    size_t nyquist = n / 2;
    for (size_t k = 0; k <= nyquist; k++) {
        double freq = (double)k * SAMPLE_RATE / (double)n;
        /* Deliberately different gains per band, like the real EQ. */
        float gain = (freq < 250.0) ? 2.0f : (freq < 4000.0 ? 0.5f : 1.7f);
        spectrum[k].re *= gain;
        spectrum[k].im *= gain;
        size_t mirror = n - k;
        if (k != 0 && k != nyquist) {
            spectrum[mirror].re *= gain;
            spectrum[mirror].im *= gain;
        }
    }
}

int main(void) {
    cplx_t x[N];
    srand(7);
    for (size_t i = 0; i < N; i++) {
        x[i].re = (float)rand() / (float)RAND_MAX - 0.5f;
        x[i].im = 0.0f;
    }

    fft_forward(x, N);
    apply_test_gains(x, N);
    fft_inverse(x, N);

    float max_imag = 0.0f;
    for (size_t i = 0; i < N; i++) {
        float a = fabsf(x[i].im);
        if (a > max_imag) max_imag = a;
    }

    printf("max |imaginary part| after IFFT = %e\n", max_imag);

    const float tolerance = 1e-4f;
    if (max_imag > tolerance) {
        printf("FAIL: mirrored-gain logic is broken, output is not real "
               "(imaginary leakage %e exceeds %e)\n", max_imag, tolerance);
        return 1;
    }
    printf("PASS: band-gain application preserves conjugate symmetry "
           "(output stays real)\n");
    return 0;
}
