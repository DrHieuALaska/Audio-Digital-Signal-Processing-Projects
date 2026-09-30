#ifndef DSP_PITCH_DETECT_H
#define DSP_PITCH_DETECT_H

#include <stddef.h>
#include "stft.h"

/*
 * Phase-vocoder pitch detector.
 *
 * Why phase, not just the FFT magnitude peak: a magnitude spectrum
 * only resolves frequency to within one bin width (sample_rate /
 * fft_size). At fft_size=2048, sample_rate=44100, that's ~21.5 Hz per
 * bin — nowhere near precise enough to tell a guitar string is a few
 * cents flat. The trick (the same one phase vocoders use for time-
 * stretching) is: track how far each bin's phase has drifted from
 * where a *stationary* sinusoid at that bin's exact center frequency
 * would have left it, hop to hop. That phase drift directly gives you
 * the bin's true instantaneous frequency, accurate to a small fraction
 * of a bin width — this is what makes the detector precise enough to
 * report cents of pitch deviation, not just "somewhere near 440 Hz".
 */
typedef struct {
    stft_t stft;              /* reused purely for its analyze() half */
    double sample_rate;
    float *last_phase;        /* size fft_size/2 + 1 */
    float min_frequency_hz;   /* ignore candidates below this */
    float max_frequency_hz;   /* ignore candidates above this */
} pitch_detector_t;

int  pitch_detector_init(pitch_detector_t *pd, size_t fft_size, size_t hop_size,
                          double sample_rate, float min_freq_hz, float max_freq_hz);
void pitch_detector_free(pitch_detector_t *pd);

/*
 * Feeds hop_size new real samples in. Returns the estimated
 * fundamental frequency in Hz, or 0.0 if no confident pitch was found
 * (silence, noise, or a peak outside [min_freq_hz, max_freq_hz]).
 *
 * Note: the very first call after init has no previous-frame phase to
 * compare against, so its result should be discarded by the caller —
 * pitch tracking needs at least 2 hops of history.
 */
double pitch_detector_process(pitch_detector_t *pd, const float *hop_in);

#endif /* DSP_PITCH_DETECT_H */
