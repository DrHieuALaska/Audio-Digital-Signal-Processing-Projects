// #ifndef DSP_PITCH_SHIFT_H
// #define DSP_PITCH_SHIFT_H

// #include <stddef.h>

// /*
//  * Phase-vocoder pitch shifter.
//  *
//  * The classic two-step trick for shifting pitch WITHOUT changing
//  * duration:
//  *   1. Time-stretch the audio by `ratio` using a phase vocoder (this
//  *      changes duration, but crucially does NOT change pitch — a
//  *      phase vocoder's whole job is decoupling the two).
//  *   2. Resample the stretched result back down by 1/ratio in time.
//  *      Resampling DOES change pitch (speeding up playback raises
//  *      pitch) — that's what we're actually exploiting. Since we
//  *      stretched by `ratio` and resample by `1/ratio`, duration ends
//  *      up back where it started, while pitch has shifted by `ratio`.
//  *
//  * Unlike dsp_core/stft.c (built for a fixed 50% analysis/synthesis
//  * hop), this module needs a variable synthesis hop, since the
//  * synthesis hop = round(analysis_hop * ratio) and ratio is arbitrary.
//  * At an arbitrary hop, the simple "one window, COLA sums to exactly 1"
//  * trick from stft.c no longer holds. So this module applies a window
//  * at BOTH analysis and synthesis, and normalizes each output sample by
//  * the sum of squared window values that landed on it — the standard,
//  * more general "weighted overlap-add" (WOLA) technique, robust to any
//  * hop size.
//  *
//  * This is an OFFLINE, buffer-based implementation (input: a full
//  * array of samples, e.g. from a WAV file) rather than a real-time
//  * streaming one. Real-time pitch shifting has to solve a genuinely
//  * harder problem — a variable-rate output stream, since the resample
//  * step changes the sample count on the fly — which is a substantial
//  * further project on its own and out of scope here.
//  */

// /*
//  * Shifts the pitch of `input` (length in_len, mono) by `semitones`
//  * (positive = up, negative = down, 0 = no change), preserving the
//  * original duration. Writes exactly in_len samples into `output`
//  * (caller-allocated).
//  *
//  * Returns 0 on success, -1 on invalid parameters or allocation failure.
//  */
// int pitch_shift_buffer(const float *input, size_t in_len, double sample_rate,
//                         float semitones, float *output);

// #endif /* DSP_PITCH_SHIFT_H */


#ifndef DSP_PITCH_SHIFT_H
#define DSP_PITCH_SHIFT_H

#include <stddef.h>

/*
 * Streaming phase-vocoder pitch shifter (spectral bin shifting).
 *
 * Design
 * ------
 * Earlier versions time-stretched a buffer and then resampled it back
 * to its original length. That forced every block to be processed
 * independently, with fresh phase state, and the blocks had to be
 * crossfaded together afterwards -- the phase mismatch at those seams
 * is what caused the crackle.
 *
 * This version has NO time-stretch and NO resampling. Analysis hop ==
 * synthesis hop (PS_HOP), so output stays sample-aligned with input.
 * The pitch is shifted directly in the frequency domain:
 *
 *   1. FFT a frame; per bin, measure magnitude and TRUE instantaneous
 *      frequency (from the frame-to-frame phase difference).
 *   2. For each output bin k, read the spectrum at k/ratio (magnitude
 *      interpolated, frequency taken from the nearest source bin and
 *      multiplied by ratio).
 *   3. Advance a persistent per-bin synthesis phase (`sum_phase`) by
 *      that shifted frequency, build the output spectrum, IFFT,
 *      window, overlap-add.
 *
 * `last_phase` and `sum_phase` live in the pitch_shifter_t object, so
 * they persist across the WHOLE signal. The shift ratio may change on
 * every frame (e.g. a pitch-correction track) without any seams.
 *
 * Limitations: whole-spectrum shifting also moves formants (fine for
 * small corrections, audible on large shifts), and there is no
 * peak-based phase locking, so very large shifts sound a bit "phasey".
 *
 * Assumptions about the other dsp_core modules (same as before):
 *   - fft_inverse() includes the 1/N normalization.
 *   - window_generate_periodic(..., WINDOW_HANN) satisfies COLA for
 *     window^2 at PS_HOP.
 */

#define PS_FFT_SIZE 2048
#define PS_HOP      (PS_FFT_SIZE / 4)   /* 75% overlap */

typedef struct pitch_shifter pitch_shifter_t;

/* Creates a shifter with all phase state cleared. NULL on failure. */
pitch_shifter_t *pitch_shifter_create(void);
void pitch_shifter_destroy(pitch_shifter_t *ps);

/*
 * Low-level, frame-at-a-time interface.
 *
 * Consumes ONE analysis frame (PS_FFT_SIZE samples; frames must be
 * PS_HOP apart) and shifts it by `semitones`. Writes PS_FFT_SIZE
 * samples to `out_frame`, ALREADY windowed and gain-normalized: just
 * overlap-add each output frame at PS_HOP spacing.
 *
 * Call it with consecutive frames of the same signal, on the same
 * pitch_shifter_t, to keep phase continuity.
 * Returns 0 on success, -1 on invalid arguments.
 */
int pitch_shifter_process_frame(pitch_shifter_t *ps, const float *in_frame,
                                float semitones, float *out_frame);

/*
 * Offline helpers for a whole mono buffer.
 *
 * The signal is zero-padded by PS_FFT_SIZE on each side internally, so
 * frame f is centered on input sample  pitch_shift_frame_center(f).
 * The number of frames needed for an input of in_len samples is
 * pitch_shift_frame_count(in_len).
 */
size_t pitch_shift_frame_count(size_t in_len);
long   pitch_shift_frame_center(size_t frame_index);   /* may be negative */

/*
 * Shifts `input` (mono, in_len samples) using a per-frame semitone
 * track: frame_semitones[f] for f in [0, n_frames). n_frames must be
 * >= pitch_shift_frame_count(in_len). Writes exactly in_len samples to
 * `output` (caller-allocated; must not alias input).
 * Returns 0 on success, -1 on invalid parameters/allocation failure.
 */
int pitch_shift_track(const float *input, size_t in_len,
                      const float *frame_semitones, size_t n_frames,
                      float *output);

/*
 * Convenience: constant shift for the whole buffer. Same signature as
 * before (sample_rate is unused -- the algorithm works in bin units).
 */
int pitch_shift_buffer(const float *input, size_t in_len, double sample_rate,
                       float semitones, float *output);

#endif /* DSP_PITCH_SHIFT_H */