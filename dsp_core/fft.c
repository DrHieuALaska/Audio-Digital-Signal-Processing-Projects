#include "fft.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline cplx_t cadd(cplx_t a, cplx_t b) {
    cplx_t r = { a.re + b.re, a.im + b.im };
    return r;
}

static inline cplx_t csub(cplx_t a, cplx_t b) {
    cplx_t r = { a.re - b.re, a.im - b.im };
    return r;
}

static inline cplx_t cmul(cplx_t a, cplx_t b) {
    cplx_t r = { a.re * b.re - a.im * b.im,
                 a.re * b.im + a.im * b.re };
    return r;
}

int fft_is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

/*
 * Standard bit-reversal permutation. This is the "decimation" step of
 * decimation-in-time: after this reorder, the iterative butterfly loop
 * below can work purely on contiguous, increasingly large blocks.
 */

 /*
 Origin     |   Mirror
 001            100 (swap)
 010            010 (swap same index)
 011            110 (swap)
 100            001 (re-swap)
 101            101 (swap same index)
 110            011 (re-swap)
 111            111 (swap same index)
 */
static void bit_reverse_permute(cplx_t *data, size_t n) {
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1; // add 1 to the half MSB 
        // Find the first position 0
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit; // switch that position to 1
        if (i < j) {
            cplx_t tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }
}

/*
 * Core butterfly computation, shared by forward and inverse transforms.
 * `sign` is -1.0 for forward, +1.0 for inverse (controls twiddle rotation
 * direction). Caller is responsible for any 1/n normalization.
 */

 /*
    data [0, 1, 2, 3, 4, 5, 6, 7]
    -> Bit reverse data
    data [0, 4, 2, 6, 1, 5, 3, 7]

    *len = 2
        (0, 4) (2, 6) (1, 5) (3, 7)
    *len = 4
        (0, 4, 2, 6) (1, 5, 3, 7)
    *len = 8
        (0, 4, 2, 6, 1, 5, 3, 7)
 */
static void fft_radix2(cplx_t *data, size_t n, double sign) {
    bit_reverse_permute(data, n);

    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = sign * 2.0 * M_PI / (double)len;
        cplx_t wlen = { (float)cos(ang), (float)sin(ang) };

        for (size_t i = 0; i < n; i += len) {
            cplx_t w = { 1.0f, 0.0f };
            size_t half = len / 2;
            for (size_t j = 0; j < half; j++) {
                cplx_t u = data[i + j];
                cplx_t v = cmul(data[i + j + half], w);
                data[i + j]        = cadd(u, v);
                data[i + j + half] = csub(u, v);
                w = cmul(w, wlen);
            }
        }
    }
}

void fft_forward(cplx_t *data, size_t n) {
    fft_radix2(data, n, -1.0);
}

void fft_inverse(cplx_t *data, size_t n) {
    fft_radix2(data, n, 1.0);
    float inv_n = 1.0f / (float)n;
    for (size_t i = 0; i < n; i++) {
        data[i].re *= inv_n;
        data[i].im *= inv_n;
    }
}

void fft_magnitude(const cplx_t *data, float *mag, size_t n) {
    for (size_t i = 0; i < n; i++) {
        mag[i] = sqrtf(data[i].re * data[i].re + data[i].im * data[i].im);
    }
}

float fft_magnitude_to_db(float magnitude, float ref, float floor_db) {
    if (ref <= 0.0f) {
        ref = 1e-12f;
    }
    float ratio = magnitude / ref;
    if (ratio < 1e-12f) {
        ratio = 1e-12f;
    }
    float db = 20.0f * log10f(ratio);
    return db < floor_db ? floor_db : db;
}
