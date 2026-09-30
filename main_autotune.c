/*
 * Auto-tune: reads a mono WAV file, tracks its pitch, computes how far
 * off the nearest equal-tempered semitone it is, and pitch-shifts the
 * audio to correct it. Writes the result to a new WAV file.
 *
 * This is an OFFLINE tool (batch WAV in -> WAV out).
 *
 * Pipeline (whole file, no independent blocks, no crossfades):
 *
 *   1. PITCH PASS   run the detector once over contiguous audio ->
 *                   one frequency estimate per HOP_SIZE samples.
 *   2. CORRECTION   median-filter the track (rejects octave glitches
 *                   and bridges 1-2 frame dropouts), then convert to
 *                   semitones of correction toward the nearest note.
 *   3. PER-FRAME    resample the correction track onto the pitch
 *      TRACK        shifter's frame grid and smooth it (zero-phase),
 *                   so the shift ratio glides instead of jumping.
 *   4. SHIFT PASS   one pitch_shift_track() call over the entire file.
 *                   The phase vocoder's last_phase / sum_phase live
 *                   across the whole file, so there are no seams.
 *
 * Build:  make main_autotune
 * Run:    ./main_autotune input.wav output.wav [strength 0.0-1.0]
 *   strength: 1.0 = full correction to nearest semitone, 0.5 = half
 *   (more natural, "gentle"), 0.0 = passthrough.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wav_io/wav_io.h"
#include "dsp_core/pitch_detect.h"
#include "dsp_core/pitch_shift.h"

#define FFT_SIZE 2048
#define HOP_SIZE (FFT_SIZE / 2)
#define MIN_FREQ_HZ 60.0f
#define MAX_FREQ_HZ 1200.0f

/* Median filter radius over detector estimates (window = 2*R+1). A bin
 * is treated as voiced if at least MEDIAN_MIN_VOICED of the window are. */
#define MEDIAN_RADIUS      2
#define MEDIAN_MIN_VOICED  3

/* One-pole smoothing coefficient per shifter frame (PS_HOP samples),
 * applied forward and backward (zero phase). Smaller = slower/smoother
 * retune; 1.0 = no smoothing. */
#define SMOOTH_COEF 0.3f

static double nearest_semitone_freq(double freq_hz) {
    double midi = 69.0 + 12.0 * log2(freq_hz / 440.0);
    double nearest_midi = round(midi);
    return 440.0 * pow(2.0, (nearest_midi - 69.0) / 12.0);
}

/* Sample index (input coordinates) at the center of detector estimate h.
 * After feeding hop h, the detector has seen samples up to (h+1)*HOP. */
static long det_center(size_t h) {
    return (long)((h + 1) * HOP_SIZE) - (long)(FFT_SIZE / 2);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Median of voiced (>0) estimates in [h-R, h+R]; 0 if too few. */
static double voiced_median(const double *hz, size_t n, size_t h) {
    double buf[2 * MEDIAN_RADIUS + 1];
    int count = 0;
    long lo = (long)h - MEDIAN_RADIUS, hi = (long)h + MEDIAN_RADIUS;
    for (long i = lo; i <= hi; i++) {
        if (i < 0 || i >= (long)n) continue;
        if (hz[i] > 0.0) buf[count++] = hz[i];
    }
    if (count < MEDIAN_MIN_VOICED) return 0.0;
    qsort(buf, (size_t)count, sizeof(double), cmp_double);
    return buf[count / 2];
}

/* Forward + backward one-pole low-pass => no time lag. */
static void smooth_zero_phase(float *x, size_t n, float a) {
    if (n < 2) return;
    for (size_t i = 1; i < n; i++) x[i] = x[i - 1] + a * (x[i] - x[i - 1]);
    for (size_t i = n - 1; i-- > 0;) x[i] = x[i + 1] + a * (x[i] - x[i + 1]);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s input.wav output.wav [strength 0.0-1.0]\n", argv[0]);
        return 1;
    }
    float strength = (argc >= 4) ? (float)atof(argv[3]) : 1.0f;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;

    float *input = NULL;
    size_t in_len = 0;
    double sample_rate = 0.0;
    if (wav_read_mono_pcm16(argv[1], &input, &in_len, &sample_rate) != 0) {
        return 1;
    }
    printf("Loaded '%s': %zu samples, %.0f Hz (%.2f sec)\n",
           argv[1], in_len, sample_rate, (double)in_len / sample_rate);

    /* ---- 1. Pitch pass: one contiguous run over the whole file ---- */
    size_t n_det = in_len / HOP_SIZE;
    double *det_hz = (double *)calloc(n_det ? n_det : 1, sizeof(double));
    double *corr   = (double *)calloc(n_det ? n_det : 1, sizeof(double));

    pitch_detector_t pd;
    pitch_detector_init(&pd, FFT_SIZE, HOP_SIZE, sample_rate, MIN_FREQ_HZ, MAX_FREQ_HZ);
    for (size_t h = 0; h < n_det; h++) {
        double est = pitch_detector_process(&pd, &input[h * HOP_SIZE]);
        det_hz[h] = (est > 0.0) ? est : 0.0;
    }
    pitch_detector_free(&pd);

    /* ---- 2. Correction in semitones per detector estimate ---- */
    size_t voiced = 0;
    double sum_abs = 0.0;
    for (size_t h = 0; h < n_det; h++) {
        double f = voiced_median(det_hz, n_det, h);
        if (f > 0.0) {
            double target = nearest_semitone_freq(f);
            corr[h] = 12.0 * log2(target / f) * (double)strength;
            voiced++;
            sum_abs += fabs(corr[h]);
        } else {
            corr[h] = 0.0;   /* no pitch: no correction */
        }
    }
    printf("Voiced estimates: %zu / %zu, mean |correction| = %.3f semitones\n",
           voiced, n_det, voiced ? sum_abs / (double)voiced : 0.0);

    /* ---- 3. Per-frame semitone track for the shifter ---- */
    size_t n_frames = pitch_shift_frame_count(in_len);
    float *semis = (float *)calloc(n_frames, sizeof(float));
    if (n_det > 0) {
        for (size_t f = 0; f < n_frames; f++) {
            double x = (double)(pitch_shift_frame_center(f) - det_center(0)) / (double)HOP_SIZE;
            if (x < 0.0) x = 0.0;
            if (x > (double)(n_det - 1)) x = (double)(n_det - 1);
            size_t i0 = (size_t)x;
            size_t i1 = (i0 + 1 < n_det) ? i0 + 1 : i0;
            double frac = x - (double)i0;
            semis[f] = (float)((1.0 - frac) * corr[i0] + frac * corr[i1]);
        }
    }
    smooth_zero_phase(semis, n_frames, SMOOTH_COEF);

    /* ---- 4. Single continuous shift pass ---- */
    float *output = (float *)calloc(in_len, sizeof(float));
    if (!output || !semis ||
        pitch_shift_track(input, in_len, semis, n_frames, output) != 0) {
        fprintf(stderr, "Pitch shifting failed\n");
        free(input); free(output); free(det_hz); free(corr); free(semis);
        return 1;
    }

    /* Overlap-add can overshoot full scale slightly; avoid wrap/clip
     * artifacts in the 16-bit writer. */
    for (size_t i = 0; i < in_len; i++) {
        if (output[i] > 1.0f) output[i] = 1.0f;
        else if (output[i] < -1.0f) output[i] = -1.0f;
    }

    int rc = wav_write_mono_pcm16(argv[2], output, in_len, sample_rate);
    if (rc == 0) printf("Wrote '%s'\n", argv[2]);

    free(input); free(output); free(det_hz); free(corr); free(semis);
    return rc != 0;
}