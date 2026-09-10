#ifndef DSP_STFT_H
#define DSP_STFT_H

#include <stddef.h>
#include "fft.h"
#include "window.h"

/*
 * Short-Time Fourier Transform engine with overlap-add (OLA) synthesis.
 *
 * Usage per hop:
 *   stft_analyze(&s, new_hop_samples);      // -> fills s.spectrum
 *   ... modify s.spectrum[i] in place ...   // your effect goes here
 *   stft_synthesize(&s, output_hop_buffer); // -> reconstructed audio
 *
 * Design notes (why it's built this way):
 *
 *  - Uses a PERIODIC window (window_generate_periodic).
 *    At hop = fft_size/2, a periodic
 *    Hann window satisfies the classic constant-overlap-add (COLA)
 *    identity: overlapping shifted copies of the window sum to exactly
 *    1.0. That's what lets synthesis skip a second (synthesis) window
 *    entirely and still reconstruct clean audio when the spectrum is
 *    left unmodified.
 *
 *  - fft_size must be an integer multiple of hop_size (this module
 *    assumes hop_size == fft_size / 2, i.e. 50% overlap, the standard
 *    COLA-satisfying configuration for Hann/Hamming).
 *
 *  - Multiplying spectrum bins by a real gain (as an EQ or noise gate
 *    does) is technically a *circular* convolution of the windowed
 *    frame with the effect's implied impulse response. Because the
 *    analysis window (1024 samples here) is far longer than the
 *    effective impulse response of a smooth EQ curve or a hard gate
 *    threshold, the wraparound artifact is negligible in practice —
 *    but it's why aggressive, spiky frequency-domain edits (e.g. a
 *    single bin zeroed to -inf dB) can introduce audible ringing.
 *    A brick-wall linear-phase filter implemented this way would need
 *    zero-padded frames (classic overlap-add *filtering*, not just
 *    overlap-add *reconstruction*) — noted here as a known extension,
 *    not implemented in this project.
 */
typedef struct {
    size_t fft_size;
    size_t hop_size;
    float *window;      /* periodic window, length fft_size */
    float *in_frame;    /* sliding analysis buffer, length fft_size */
    float *out_accum;   /* overlap-add accumulator, length fft_size */
    cplx_t *spectrum;   /* scratch: exposed for the caller to read/modify */
} stft_t;

/* hop_size must equal fft_size / 2. Returns 0 on success, -1 on bad params
 * or allocation failure. */
int stft_init(stft_t *s, size_t fft_size, size_t hop_size, window_type_t wtype);
void stft_free(stft_t *s);

/* Slides hop_size new real samples into the analysis frame, applies the
 * window, and runs the forward FFT. Result is left in s->spectrum for
 * the caller to read or modify in place before calling stft_synthesize(). */
void stft_analyze(stft_t *s, const float *hop_in);

/* Runs the inverse FFT on s->spectrum (as possibly modified by the
 * caller), overlap-adds the result into the internal accumulator, and
 * writes the next hop_size samples of reconstructed audio into out_hop.
 * Note: the first (fft_size - hop_size) samples of output are a ramp-up
 * transient (buffer still filling) — this is normal STFT latency. */
void stft_synthesize(stft_t *s, float *out_hop);

#endif /* DSP_STFT_H */
