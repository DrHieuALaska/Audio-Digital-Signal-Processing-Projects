#ifndef DSP_FFT_H
#define DSP_FFT_H

#include <stddef.h>

/*
 * Minimal complex type used throughout dsp_core.
 *
 * We deliberately avoid C99's <complex.h> here: this same struct layout
 * (two floats) will be reused almost unchanged when we port this code to
 * fixed-point (Q15) for the embedded guitar-tuner project later, and
 * <complex.h> semantics don't translate cleanly to that world.
 */
typedef struct {
    float re;
    float im;
} cplx_t;

/*
 * In-place iterative radix-2 Cooley-Tukey FFT (decimation-in-time).
 *
 * Requirements:
 *   - n must be a power of two (use fft_is_power_of_two() to check).
 *   - data must point to an array of exactly n cplx_t values.
 *
 * On return, data[] holds the DFT of the input, in natural (not
 * bit-reversed) order. Not normalized (matches the standard DFT
 * definition, i.e. no 1/n factor).
 */
void fft_forward(cplx_t *data, size_t n);

/*
 * True inverse FFT (in-place): includes the 1/n normalization, so
 * fft_inverse(fft_forward(x)) == x (up to floating point error).
 *
 * Same size requirements as fft_forward.
 */
void fft_inverse(cplx_t *data, size_t n);

/*
 * Computes magnitude |X[k]| = sqrt(re^2 + im^2) for each of the n bins
 * in `data` and writes the result into `mag` (must have >= n entries).
 */
void fft_magnitude(const cplx_t *data, float *mag, size_t n);

/*
 * Converts a magnitude value to decibels relative to `ref`
 * (commonly ref = 1.0f, or ref = max magnitude in the frame).
 * Clamps at floor_db to avoid -inf for zero/near-zero magnitudes.
 */
float fft_magnitude_to_db(float magnitude, float ref, float floor_db);

/* Returns 1 if n is a nonzero power of two, else 0. */
int fft_is_power_of_two(size_t n);

#endif /* DSP_FFT_H */
