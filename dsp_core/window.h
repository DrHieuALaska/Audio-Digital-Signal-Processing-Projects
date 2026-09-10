#ifndef DSP_WINDOW_H
#define DSP_WINDOW_H

#include <stddef.h>
#include "fft.h"

typedef enum {
    WINDOW_RECTANGULAR,
    WINDOW_HANN,
    WINDOW_HAMMING,
    WINDOW_BLACKMAN
} window_type_t;

/* Fills w[0..n-1] with the coefficients of the requested window.
 * This is the "symmetric" form (denominator n-1), the conventional
 * choice for one-shot spectral analysis / display, as used by the
 * spectrum analyzer project. Do NOT use this form for STFT overlap-add
 * processing — see window_generate_periodic() below. */
void window_generate(float *w, size_t n, window_type_t type);

/* Fills w[0..n-1] with the "periodic" form of the window (denominator
 * n instead of n-1). This is the form required for the classic
 * constant-overlap-add (COLA) identity to hold in STFT analysis/
 * synthesis pipelines — e.g. a periodic Hann window at 50% hop sums to
 * exactly 1.0 across overlapping frames. Using the symmetric window
 * here instead introduces a small periodic amplitude ripple in
 * reconstructed audio. Used by dsp_core/stft.c. */
void window_generate_periodic(float *w, size_t n, window_type_t type);

/*
 * Applies a precomputed real-valued window to `samples` (real input,
 * length n) and writes the windowed result into `out` as complex
 * values (imaginary part = 0), ready to hand to fft_forward().
 */
void window_apply_to_complex(const float *samples, const float *w,
                              cplx_t *out, size_t n);

#endif /* DSP_WINDOW_H */
