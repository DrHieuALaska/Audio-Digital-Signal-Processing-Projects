#include "pitch_detect.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int pitch_detector_init(pitch_detector_t *pd, size_t fft_size, size_t hop_size,
                         double sample_rate, float min_freq_hz, float max_freq_hz) {
    if (stft_init(&pd->stft, fft_size, hop_size, WINDOW_HANN) != 0) {
        return -1;
    }
    size_t n_bins = fft_size / 2 + 1;
    pd->last_phase = (float *)calloc(n_bins, sizeof(float));
    if (!pd->last_phase) {
        stft_free(&pd->stft);
        return -1;
    }
    pd->sample_rate = sample_rate;
    pd->min_frequency_hz = min_freq_hz;
    pd->max_frequency_hz = max_freq_hz;
    return 0;
}

void pitch_detector_free(pitch_detector_t *pd) {
    stft_free(&pd->stft);
    free(pd->last_phase);
    pd->last_phase = NULL;
}

/* Wraps a phase value into (-pi, pi]. */
static float wrap_phase(float phase) {
    float wrapped = fmodf(phase + (float)M_PI, 2.0f * (float)M_PI);
    if (wrapped < 0.0f) wrapped += 2.0f * (float)M_PI;
    return wrapped - (float)M_PI;
}

double pitch_detector_process(pitch_detector_t *pd, const float *hop_in) {
    stft_analyze(&pd->stft, hop_in);

    size_t n = pd->stft.fft_size;
    size_t hop = pd->stft.hop_size;
    size_t n_bins = n / 2 + 1;
    double sr = pd->sample_rate;

    /* Heuristic silence/noise floor: below this, we don't trust any
     * peak enough to report a pitch. Scales with fft_size because our
     * magnitudes come from an unnormalized DFT. */
    const float min_magnitude = (float)n * 0.01f;

    size_t best_bin = 0;
    float best_mag = 0.0f;
    double best_true_freq = 0.0;

    for (size_t k = 0; k < n_bins; k++) {
        float re = pd->stft.spectrum[k].re;
        float im = pd->stft.spectrum[k].im;
        float mag = sqrtf(re * re + im * im);
        float phase = atan2f(im, re);

        double bin_freq = (double)k * sr / (double)n;

        /* Phase vocoder instantaneous frequency estimate: how far this
         * bin's phase drifted from what a stationary sinusoid at
         * exactly bin_freq would produce over one hop, converted back
         * into a frequency and added to the bin's center frequency.
         * This is what gives sub-bin frequency resolution. */
        double expected_advance = 2.0 * M_PI * (double)k * (double)hop / (double)n;
        float phase_diff = wrap_phase(phase - pd->last_phase[k] - (float)expected_advance);
        double true_freq = bin_freq + (double)phase_diff * sr / (2.0 * M_PI * (double)hop);

        pd->last_phase[k] = phase;

        if (mag > best_mag && bin_freq >= pd->min_frequency_hz
                            && bin_freq <= pd->max_frequency_hz) {
            best_mag = mag;
            best_bin = k;
            best_true_freq = true_freq;
        }
    }

    if (best_bin == 0 || best_mag < min_magnitude) {
        return 0.0; /* no confident pitch */
    }

    return best_true_freq;
}
