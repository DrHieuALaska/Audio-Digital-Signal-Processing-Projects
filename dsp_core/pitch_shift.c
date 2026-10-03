// #include "pitch_shift.h"
// #include "fft.h"
// #include "window.h"
// #include <stdlib.h>
// #include <string.h>
// #include <math.h>

// #ifndef M_PI
// #define M_PI 3.14159265358979323846
// #endif

// #define PS_FFT_SIZE 2048
// #define PS_ANALYSIS_HOP (PS_FFT_SIZE / 4)   /* 75% overlap: good phase estimation quality */

// static float wrap_phase(float phase) {
//     float wrapped = fmodf(phase + (float)M_PI, 2.0f * (float)M_PI);
//     if (wrapped < 0.0f) wrapped += 2.0f * (float)M_PI;
//     return wrapped - (float)M_PI;
// }

// int pitch_shift_buffer(const float *input, size_t in_len, double sample_rate,
//                         float semitones, float *output) {
//     if (!input || !output || in_len == 0) return -1;

//     const size_t n = PS_FFT_SIZE;
//     const size_t ra = PS_ANALYSIS_HOP;
//     const size_t n_bins = n / 2 + 1;

//     double ratio = pow(2.0, (double)semitones / 12.0);
//     size_t rs = (size_t)lround((double)ra * ratio);
//     if (rs < 1) rs = 1;

//     /* Pad the input with trailing silence so the last analysis frames
//      * (which read fft_size samples starting near the true end) don't
//      * read out of bounds, and so WOLA has enough frames to fully flush
//      * the tail of the last real samples out through overlap-add. */
//     size_t padded_in_len = in_len + n;
//     float *padded_in = (float *)calloc(padded_in_len, sizeof(float));
//     if (!padded_in) return -1;
//     memcpy(padded_in, input, in_len * sizeof(float));

//     /* Generous upper bound on the stretched buffer length. */
//     size_t stretched_cap = (size_t)((double)in_len * ratio) + 4 * n;
//     float *out_accum  = (float *)calloc(stretched_cap, sizeof(float));
//     float *norm_accum = (float *)calloc(stretched_cap, sizeof(float));
//     float *window     = (float *)calloc(n, sizeof(float));
//     float *last_phase = (float *)calloc(n_bins, sizeof(float));
//     float *sum_phase  = (float *)calloc(n_bins, sizeof(float));
//     cplx_t *spectrum  = (cplx_t *)calloc(n, sizeof(cplx_t));

//     if (!out_accum || !norm_accum || !window || !last_phase || !sum_phase || !spectrum) {
//         free(padded_in); free(out_accum); free(norm_accum);
//         free(window); free(last_phase); free(sum_phase); free(spectrum);
//         return -1;
//     }

//     window_generate_periodic(window, n, WINDOW_HANN);

//     size_t analysis_start = 0;
//     size_t synthesis_start = 0;
//     size_t last_synthesis_end = 0;

//     while (analysis_start + n <= padded_in_len && synthesis_start + n <= stretched_cap) {
//         /* --- Analysis: window + FFT this frame --- */
//         window_apply_to_complex(&padded_in[analysis_start], window, spectrum, n);
//         fft_forward(spectrum, n);

//         /* --- Phase vocoder: extract magnitude + true instantaneous
//          * frequency per bin (same technique as pitch_detect.c), then
//          * accumulate a NEW synthesis phase that advances at that true
//          * frequency but over the (different) synthesis hop duration.
//          * This is the step that stretches time while preserving pitch. */
//         for (size_t k = 0; k < n_bins; k++) {
//             float re = spectrum[k].re;
//             float im = spectrum[k].im;
//             float mag = sqrtf(re * re + im * im);
//             float phase = atan2f(im, re);

//             double bin_freq = (double)k * sample_rate / (double)n;
//             double expected_advance = 2.0 * M_PI * (double)k * (double)ra / (double)n;
//             float phase_diff = wrap_phase(phase - last_phase[k] - (float)expected_advance);
//             double true_freq = bin_freq + (double)phase_diff * sample_rate / (2.0 * M_PI * (double)ra);

//             last_phase[k] = phase;

//             sum_phase[k] += (float)(true_freq * 2.0 * M_PI * (double)rs / sample_rate);

//             spectrum[k].re = mag * cosf(sum_phase[k]);
//             spectrum[k].im = mag * sinf(sum_phase[k]);
//         }
//         /* Mirror to keep the spectrum conjugate-symmetric (real output). */
//         for (size_t k = 1; k < n / 2; k++) {
//             spectrum[n - k].re =  spectrum[k].re;
//             spectrum[n - k].im = -spectrum[k].im;
//         }

//         fft_inverse(spectrum, n);

//         /* --- Synthesis: window again (WOLA), overlap-add + track
//          * normalization so we can divide it out afterward. Necessary
//          * because rs is arbitrary, so the simple "COLA sums to 1"
//          * shortcut from stft.c doesn't apply here. --- */
//         for (size_t j = 0; j < n; j++) {
//             float w = window[j];
//             out_accum[synthesis_start + j]  += spectrum[j].re * w;
//             norm_accum[synthesis_start + j] += w * w;
//         }

//         if (synthesis_start + n > last_synthesis_end) {
//             last_synthesis_end = synthesis_start + n;
//         }

//         analysis_start += ra;
//         synthesis_start += rs;
//     }

//     /* Normalize the stretched signal. */
//     for (size_t i = 0; i < last_synthesis_end; i++) {
//         float denom = norm_accum[i];
//         out_accum[i] = (denom > 1e-6f) ? (out_accum[i] / denom) : 0.0f;
//     }

//     /* --- Resample the stretched signal back to the ORIGINAL length.
//      * This is the step that actually shifts pitch: reading the
//      * time-stretched (but pitch-unchanged) signal at `ratio` speed
//      * both restores the original duration and multiplies the pitch by
//      * `ratio`, since resampling doesn't decouple pitch from speed the
//      * way the phase vocoder step did. Simple linear interpolation —
//      * a known accuracy/simplicity tradeoff, noted in the header. --- */
//     for (size_t i = 0; i < in_len; i++) {
//         double pos = (double)i * ratio;
//         size_t i0 = (size_t)pos;
//         double frac = pos - (double)i0;
//         size_t i1 = i0 + 1;
//         if (i1 >= last_synthesis_end) i1 = last_synthesis_end > 0 ? last_synthesis_end - 1 : 0;
//         if (i0 >= last_synthesis_end) i0 = last_synthesis_end > 0 ? last_synthesis_end - 1 : 0;
//         output[i] = (float)((1.0 - frac) * out_accum[i0] + frac * out_accum[i1]);
//     }

//     free(padded_in); free(out_accum); free(norm_accum);
//     free(window); free(last_phase); free(sum_phase); free(spectrum);
//     return 0;
// }


#include "pitch_shift.h"
#include "fft.h"
#include "window.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PS_BINS (PS_FFT_SIZE / 2 + 1)
#define TWO_PI  (2.0 * M_PI)

/* If a source bin had less magnitude than this in the previous frame,
 * treat it as a partial that is just starting: its phase history is
 * meaningless, so re-seed the synthesis phase from the analysis phase
 * instead of accumulating from garbage. (Full-scale sine ~ 512 here;
 * 16-bit quantization noise floor ~ 3e-4.) */
#define PS_ONSET_MAG 1e-3

struct pitch_shifter {
    float  *window;
    cplx_t *spectrum;
    double *last_phase;   /* analysis phase of previous frame, per bin   */
    double *sum_phase;    /* accumulated SYNTHESIS phase, per bin        */
    double *ana_phase;    /* scratch: this frame's analysis phase        */
    double *ana_freq;     /* scratch: this frame's true freq, in bins    */
    double *ana_mag;      /* scratch: this frame's magnitude             */
    double *prev_mag;     /* previous frame's magnitude (onset detect)   */
    double  scale;        /* 1 / (sum of window^2 per hop)               */
};

static double wrap_phase(double phase) {
    double w = fmod(phase + M_PI, TWO_PI);
    if (w < 0.0) w += TWO_PI;
    return w - M_PI;
}

void pitch_shifter_destroy(pitch_shifter_t *ps) {
    if (!ps) return;
    free(ps->window);
    free(ps->spectrum);
    free(ps->last_phase);
    free(ps->sum_phase);
    free(ps->ana_phase);
    free(ps->ana_freq);
    free(ps->ana_mag);
    free(ps->prev_mag);
    free(ps);
}

pitch_shifter_t *pitch_shifter_create(void) {
    pitch_shifter_t *ps = (pitch_shifter_t *)calloc(1, sizeof(*ps));
    if (!ps) return NULL;

    ps->window     = (float *)calloc(PS_FFT_SIZE, sizeof(float));
    ps->spectrum   = (cplx_t *)calloc(PS_FFT_SIZE, sizeof(cplx_t));
    ps->last_phase = (double *)calloc(PS_BINS, sizeof(double));
    ps->sum_phase  = (double *)calloc(PS_BINS, sizeof(double));
    ps->ana_phase  = (double *)calloc(PS_BINS, sizeof(double));
    ps->ana_freq   = (double *)calloc(PS_BINS, sizeof(double));
    ps->ana_mag    = (double *)calloc(PS_BINS, sizeof(double));
    ps->prev_mag   = (double *)calloc(PS_BINS, sizeof(double));

    if (!ps->window || !ps->spectrum || !ps->last_phase || !ps->sum_phase ||
        !ps->ana_phase || !ps->ana_freq || !ps->ana_mag || !ps->prev_mag) {
        pitch_shifter_destroy(ps);
        return NULL;
    }

    window_generate_periodic(ps->window, PS_FFT_SIZE, WINDOW_HANN);

    /* With analysis AND synthesis windows and a fixed hop, the summed
     * w^2 is constant in steady state. Its per-sample average over one
     * hop is (sum of w^2) / hop. (Hann @ 75% overlap -> 1.5.) */
    double total = 0.0;
    for (size_t j = 0; j < PS_FFT_SIZE; j++) {
        total += (double)ps->window[j] * (double)ps->window[j];
    }
    ps->scale = (double)PS_HOP / total; // = 2/3 for Hann @ 75% overlap (for further information, pls read report)

    return ps;
}

int pitch_shifter_process_frame(pitch_shifter_t *ps, const float *in_frame,
                                float semitones, float *out_frame) {
    if (!ps || !in_frame || !out_frame) return -1;

    const size_t n  = PS_FFT_SIZE;
    const size_t nb = PS_BINS;
    const double ratio = pow(2.0, (double)semitones / 12.0);

    /* ---------- Analysis ---------- */
    window_apply_to_complex(in_frame, ps->window, ps->spectrum, n);
    fft_forward(ps->spectrum, n);

    for (size_t k = 0; k < nb; k++) {
        double re = (double)ps->spectrum[k].re;
        double im = (double)ps->spectrum[k].im;
        double phase = atan2(im, re);

        double expected = TWO_PI * (double)k * (double)PS_HOP / (double)n;
        double dev = wrap_phase(phase - ps->last_phase[k] - expected);

        ps->ana_mag[k]   = sqrt(re * re + im * im);
        ps->ana_phase[k] = phase;
        /* true frequency in units of bins: divide by (sample rate/n) */
        ps->ana_freq[k]  = (double)k + dev * (double)n / (TWO_PI * (double)PS_HOP);
        ps->last_phase[k] = phase;
    }

    /* ---------- Shift + phase accumulation ---------- */
    for (size_t k = 0; k < nb; k++) {
        double src = (double)k / ratio;   /* where this output bin reads from */
        double mag = 0.0;
        double fbins = (double)k;         /* default: bin-center frequency (frequency in units of bins) */
        size_t j = k;
        int valid = 0;

        if (src <= (double)(nb - 1)) {
            size_t i0 = (size_t)src;
            double frac = src - (double)i0;
            size_t i1 = (i0 + 1 < nb) ? i0 + 1 : i0;
            mag = (1.0 - frac) * ps->ana_mag[i0] + frac * ps->ana_mag[i1];

            j = (size_t)(src + 0.5);
            if (j >= nb) j = nb - 1;
            fbins = ps->ana_freq[j] * ratio;   /* the actual pitch shift */
            valid = 1;
        }

        if (valid && ps->prev_mag[j] < PS_ONSET_MAG) {
            /* Start of signal / new partial: seed from analysis phase. */
            ps->sum_phase[k] = ps->ana_phase[j];
        } else {
            /* Advance by the (shifted) true frequency over ONE hop. */
            ps->sum_phase[k] = wrap_phase(ps->sum_phase[k] +
                               TWO_PI * fbins * (double)PS_HOP / (double)n);
        }

        ps->spectrum[k].re = (float)(mag * cos(ps->sum_phase[k]));
        ps->spectrum[k].im = (float)(mag * sin(ps->sum_phase[k]));
    }
    memcpy(ps->prev_mag, ps->ana_mag, nb * sizeof(double));

    /* DC and Nyquist must be purely real. */
    ps->spectrum[0].im = 0.0f;
    ps->spectrum[n / 2].im = 0.0f;
    /* Conjugate-symmetric mirror so the IFFT output is real. */
    for (size_t k = 1; k < n / 2; k++) {
        ps->spectrum[n - k].re =  ps->spectrum[k].re;
        ps->spectrum[n - k].im = -ps->spectrum[k].im;
    }

    fft_inverse(ps->spectrum, n);

    /* ---------- Synthesis window + gain ---------- */
    for (size_t i = 0; i < n; i++) {
        out_frame[i] = (float)((double)ps->spectrum[i].re *
                               (double)ps->window[i] * ps->scale);
    }
    return 0;
}

size_t pitch_shift_frame_count(size_t in_len) {
    /* padded length = in_len + 2N; frames start every PS_HOP and must
     * fit entirely: floor((padded - N) / hop) + 1 */
    return (in_len + PS_FFT_SIZE) / PS_HOP + 1;
}

long pitch_shift_frame_center(size_t frame_index) {
    /* Frame f starts at padded index f*HOP == input index f*HOP - N,
     * so its center is f*HOP - N + N/2. */
    return (long)(frame_index * PS_HOP) - (long)(PS_FFT_SIZE / 2);
}

int pitch_shift_track(const float *input, size_t in_len,
                      const float *frame_semitones, size_t n_frames,
                      float *output) {
    if (!input || !output || !frame_semitones || in_len == 0) return -1;
    if (n_frames < pitch_shift_frame_count(in_len)) return -1;

    const size_t need = pitch_shift_frame_count(in_len);
    const size_t padded_len = in_len + 2 * (size_t)PS_FFT_SIZE;

    float *padded    = (float *)calloc(padded_len, sizeof(float));
    float *accum     = (float *)calloc(padded_len, sizeof(float));
    float *frame_out = (float *)malloc(PS_FFT_SIZE * sizeof(float));
    pitch_shifter_t *ps = pitch_shifter_create();

    if (!padded || !accum || !frame_out || !ps) {
        free(padded); free(accum); free(frame_out);
        pitch_shifter_destroy(ps);
        return -1;
    }

    memcpy(padded + PS_FFT_SIZE, input, in_len * sizeof(float));

    /* One shifter, one continuous pass: phase state never resets. */
    for (size_t f = 0; f < need; f++) {
        size_t start = f * PS_HOP;
        pitch_shifter_process_frame(ps, &padded[start], frame_semitones[f], frame_out);
        for (size_t j = 0; j < PS_FFT_SIZE; j++) {
            accum[start + j] += frame_out[j];
        }
    }

    memcpy(output, accum + PS_FFT_SIZE, in_len * sizeof(float));

    free(padded); free(accum); free(frame_out);
    pitch_shifter_destroy(ps);
    return 0;
}

int pitch_shift_buffer(const float *input, size_t in_len, double sample_rate,
                       float semitones, float *output) {
    (void)sample_rate;
    if (!input || !output || in_len == 0) return -1;

    size_t n_frames = pitch_shift_frame_count(in_len);
    float *track = (float *)malloc(n_frames * sizeof(float));
    if (!track) return -1;
    for (size_t f = 0; f < n_frames; f++) track[f] = semitones;

    int rc = pitch_shift_track(input, in_len, track, n_frames, output);
    free(track);
    return rc;
}