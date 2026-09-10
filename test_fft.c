/*
 * Correctness test for dsp_core/fft.c.
 *
 * Compares fft_forward() against a naive O(n^2) DFT on a pseudo-random
 * signal, and checks that fft_inverse(fft_forward(x)) reconstructs x.
 * Run with: make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp_core/fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void naive_dft(const cplx_t *in, cplx_t *out, size_t n) {
    for (size_t k = 0; k < n; k++) {
        double sum_re = 0.0, sum_im = 0.0;
        for (size_t t = 0; t < n; t++) {
            double ang = -2.0 * M_PI * (double)k * (double)t / (double)n;
            double c = cos(ang), s = sin(ang);
            sum_re += in[t].re * c - in[t].im * s;
            sum_im += in[t].re * s + in[t].im * c;
        }
        out[k].re = (float)sum_re;
        out[k].im = (float)sum_im;
    }
}

int main(void) {
    const size_t n = 64;
    cplx_t signal[64], via_fft[64], via_naive[64], reconstructed[64];

    srand(42);
    for (size_t i = 0; i < n; i++) {
        signal[i].re = (float)rand() / (float)RAND_MAX - 0.5f;
        signal[i].im = 0.0f;
    }

    for (size_t i = 0; i < n; i++) via_fft[i] = signal[i];
    fft_forward(via_fft, n);
    naive_dft(signal, via_naive, n);

    float max_err_fwd = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float dre = via_fft[i].re - via_naive[i].re;
        float dim = via_fft[i].im - via_naive[i].im;
        float err = sqrtf(dre * dre + dim * dim);
        if (err > max_err_fwd) max_err_fwd = err;
    }

    for (size_t i = 0; i < n; i++) reconstructed[i] = via_fft[i];
    fft_inverse(reconstructed, n);

    float max_err_inv = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float dre = reconstructed[i].re - signal[i].re;
        float dim = reconstructed[i].im - signal[i].im;
        float err = sqrtf(dre * dre + dim * dim);
        if (err > max_err_inv) max_err_inv = err;
    }

    printf("n = %zu\n", n);
    printf("max |FFT - naive DFT| error   = %e\n", max_err_fwd);
    printf("max |IFFT(FFT(x)) - x| error  = %e\n", max_err_inv);

    const float tolerance = 1e-3f;
    if (max_err_fwd > tolerance || max_err_inv > tolerance) {
        printf("FAIL: error exceeds tolerance (%e)\n", tolerance);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
