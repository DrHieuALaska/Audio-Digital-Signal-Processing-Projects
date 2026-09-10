/*
 * Offline 3-band equalizer + spectral noise gate — FILE-TO-FILE version.
 *
 * Reads an existing audio file, runs it through the same STFT -> EQ/gate
 * -> ISTFT pipeline as main_equalizer.c and main_record_eq.c, and writes
 * the result to a new audio file. No PortAudio at all — this is pure
 * offline processing via libsndfile, so there's no real-time budget to
 * worry about.
 *
 * All effect settings are supplied up front as command-line arguments
 * (see print_usage below), since there's no live listener to tune
 * against while it runs.
 *
 * Multi-channel handling: each channel is de-interleaved and run
 * through its own independent STFT state (a stereo file gets two
 * completely separate mono pipelines, left and right), then
 * re-interleaved on write. This reuses the exact same
 * apply_spectral_effects() logic as the mono live version instead of
 * inventing a stereo-aware variant.
 *
 * Algorithmic latency: the STFT/overlap-add process delays audio by
 * (FFT_SIZE - HOP_SIZE) samples relative to the input, same as in the
 * live version. This program compensates automatically: it feeds one
 * extra "flush" hop of silence at the end to push out the tail, then
 * trims that same amount of latency off the front of the output, so
 * the output file ends up the same length and alignment as the input.
 *
 * Usage:
 *   ./file_eq INPUT.wav OUTPUT.wav \
 *       [--bass DB] [--mid DB] [--treble DB] \
 *       [--gate] [--gate-threshold DB]
 *
 * Defaults: bass=mid=treble=0dB, gate off, gate-threshold=-55dB.
 *
 * Run:    ./file_eq voice.wav voice_eq.wav --bass 2 --gate --gate-threshold -50
 *
 * NOTE ON MEMORY: for simplicity this reads the entire input file into
 * memory before processing (fine for anything up to a few minutes of
 * audio on a modern machine). For very long files, switch to
 * chunked/streaming reads with sf_readf_float in a loop instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sndfile.h>

#include "dsp_core/fft.h"
#include "dsp_core/window.h"
#include "dsp_core/stft.h"

#define FFT_SIZE      1024
#define HOP_SIZE      (FFT_SIZE / 2)
#define LATENCY_SAMPLES (FFT_SIZE - HOP_SIZE) /* used for stripping the leading delay*/

#define BAND_LOW_MAX_HZ   250.0
#define BAND_MID_MAX_HZ   4000.0

typedef struct {
    float gain_low_db;
    float gain_mid_db;
    float gain_high_db;
    float gate_threshold_db;
    int   gate_enabled;
    const char *input_path;
    const char *output_path;
} config_t;

static float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

/* Identical logic to the live version's apply_spectral_effects. */
static void apply_spectral_effects(cplx_t *spectrum, size_t n, double sample_rate,
                                    const config_t *cfg) {
    float low_gain  = db_to_linear(cfg->gain_low_db);
    float mid_gain  = db_to_linear(cfg->gain_mid_db);
    float high_gain = db_to_linear(cfg->gain_high_db);

    size_t nyquist_bin = n / 2;

    for (size_t k = 0; k <= nyquist_bin; k++) {
        double freq = (double)k * sample_rate / (double)n;

        float band_gain;
        if (freq < BAND_LOW_MAX_HZ)      band_gain = low_gain;
        else if (freq < BAND_MID_MAX_HZ) band_gain = mid_gain;
        else                              band_gain = high_gain;

        float gate_gain = 1.0f;
        if (cfg->gate_enabled) {
            float mag = sqrtf(spectrum[k].re * spectrum[k].re
                             + spectrum[k].im * spectrum[k].im);
            float db = fft_magnitude_to_db(mag, (float)n / 4.0f, -120.0f);
            if (db < cfg->gate_threshold_db) {
                gate_gain = 0.0f;
            }
        }

        float total_gain = band_gain * gate_gain;

        spectrum[k].re *= total_gain;
        spectrum[k].im *= total_gain;

        size_t mirror = n - k;
        if (k != 0 && k != nyquist_bin) {
            spectrum[mirror].re *= total_gain;
            spectrum[mirror].im *= total_gain;
        }
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s INPUT.wav OUTPUT.wav "
        "[--bass DB] [--mid DB] [--treble DB] [--gate] [--gate-threshold DB]\n",
        prog);
}

static int parse_args(int argc, char **argv, config_t *cfg) {
    cfg->gain_low_db = cfg->gain_mid_db = cfg->gain_high_db = 0.0f;
    cfg->gate_threshold_db = -55.0f;
    cfg->gate_enabled = 0;
    cfg->input_path = NULL;
    cfg->output_path = NULL;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bass") && i + 1 < argc) {
            cfg->gain_low_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mid") && i + 1 < argc) {
            cfg->gain_mid_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--treble") && i + 1 < argc) {
            cfg->gain_high_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--gate")) {
            cfg->gate_enabled = 1;
        } else if (!strcmp(argv[i], "--gate-threshold") && i + 1 < argc) {
            cfg->gate_threshold_db = (float)atof(argv[++i]);
        } else if (argv[i][0] != '-') {
            if (positional == 0) cfg->input_path = argv[i];
            else if (positional == 1) cfg->output_path = argv[i];
            positional++;
        } else {
            fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!cfg->input_path || !cfg->output_path) {
        fprintf(stderr, "Both INPUT.wav and OUTPUT.wav are required.\n");
        return -1;
    }
    return 0;
}

/* Runs the full STFT -> effects -> ISTFT pipeline over one channel's
 * worth of de-interleaved samples, including a trailing flush hop, and
 * writes the latency-compensated result into *out (caller-allocated,
 * length >= num_frames). */
static int process_channel(const float *in, sf_count_t num_frames,
                            double sample_rate, const config_t *cfg,
                            float *out) {
    stft_t stft;
    if (stft_init(&stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) {
        fprintf(stderr, "stft_init failed\n");
        return -1;
    }

    /* Hops needed to cover num_frames of real input, plus one extra
     * flush hop of silence to push out the OLA (overlap-add) tail. */
    sf_count_t total_hops = (num_frames + HOP_SIZE - 1) / HOP_SIZE + 1;

    float hop_in[HOP_SIZE];
    float hop_out[HOP_SIZE];

    sf_count_t scratch_len = total_hops * HOP_SIZE;
    float *scratch = calloc((size_t)scratch_len, sizeof(float));
    if (!scratch) {
        fprintf(stderr, "Out of memory\n");
        stft_free(&stft);
        return -1;
    }

    sf_count_t produced = 0;
    for (sf_count_t hop = 0; hop < total_hops; hop++) {
        sf_count_t start = hop * HOP_SIZE;
        sf_count_t remaining = num_frames - start;

        if (remaining >= HOP_SIZE) {
            memcpy(hop_in, in + start, HOP_SIZE * sizeof(float));
        } else if (remaining > 0) {
            memcpy(hop_in, in + start, (size_t)remaining * sizeof(float));
            memset(hop_in + remaining, 0, (HOP_SIZE - (size_t)remaining) * sizeof(float));
        } else {
            memset(hop_in, 0, sizeof(hop_in)); /* the trailing flush hop */
        }

        stft_analyze(&stft, hop_in);
        apply_spectral_effects(stft.spectrum, FFT_SIZE, sample_rate, cfg);
        stft_synthesize(&stft, hop_out);

        memcpy(scratch + produced, hop_out, HOP_SIZE * sizeof(float));
        produced += HOP_SIZE;
    }

    /* Trim off the front latency so output[0] lines up with input[0]. */
    memcpy(out, scratch + LATENCY_SAMPLES, (size_t)num_frames * sizeof(float));

    free(scratch);
    stft_free(&stft);
    return 0;
}

int main(int argc, char **argv) {
    config_t cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(sfinfo));
    SNDFILE *infile = sf_open(cfg.input_path, SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "Failed to open input file \"%s\": %s\n",
                cfg.input_path, sf_strerror(NULL));
        return 1;
    }

    sf_count_t num_frames = sfinfo.frames;
    int channels = sfinfo.channels;

    printf("Input: \"%s\" (%d ch, %d Hz, %lld frames = %.2fs)\n",
           cfg.input_path, channels, sfinfo.samplerate,
           (long long)num_frames, (double)num_frames / sfinfo.samplerate);
    printf("Config: bass %+.1fdB  mid %+.1fdB  treble %+.1fdB  gate %s (%.0fdB)\n",
           cfg.gain_low_db, cfg.gain_mid_db, cfg.gain_high_db,
           cfg.gate_enabled ? "ON" : "OFF", cfg.gate_threshold_db);

    /* Read the whole file in one shot (interleaved), then de-interleave. */
    float *interleaved_in = malloc((size_t)num_frames * channels * sizeof(float));
    if (!interleaved_in) {
        fprintf(stderr, "Out of memory reading input\n");
        sf_close(infile);
        return 1;
    }
    sf_count_t read = sf_readf_float(infile, interleaved_in, num_frames);
    sf_close(infile);
    if (read != num_frames) {
        fprintf(stderr, "Warning: expected %lld frames, read %lld\n",
                (long long)num_frames, (long long)read);
        num_frames = read;
    }

    float **channel_in  = malloc(channels * sizeof(float *));
    float **channel_out = malloc(channels * sizeof(float *));
    for (int c = 0; c < channels; c++) {
        channel_in[c]  = malloc((size_t)num_frames * sizeof(float));
        channel_out[c] = malloc((size_t)num_frames * sizeof(float));
        for (sf_count_t f = 0; f < num_frames; f++) {
            channel_in[c][f] = interleaved_in[f * channels + c];
        }
    }
    free(interleaved_in);

    printf("Processing...\n");
    for (int c = 0; c < channels; c++) {
        if (process_channel(channel_in[c], num_frames, sfinfo.samplerate, &cfg,
                             channel_out[c]) != 0) {
            fprintf(stderr, "Failed to process channel %d\n", c);
            return 1;
        }
    }

    /* Re-interleave for output. */
    float *interleaved_out = malloc((size_t)num_frames * channels * sizeof(float));
    for (int c = 0; c < channels; c++) {
        for (sf_count_t f = 0; f < num_frames; f++) {
            interleaved_out[f * channels + c] = channel_out[c][f];
        }
    }

    SF_INFO out_info = sfinfo;
    out_info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE *outfile = sf_open(cfg.output_path, SFM_WRITE, &out_info);
    if (!outfile) {
        fprintf(stderr, "Failed to open output file \"%s\": %s\n",
                cfg.output_path, sf_strerror(NULL));
        return 1;
    }
    sf_count_t written = sf_writef_float(outfile, interleaved_out, num_frames);
    sf_close(outfile);
    if (written != num_frames) {
        fprintf(stderr, "Warning: short write (%lld/%lld frames)\n",
                (long long)written, (long long)num_frames);
    }

    printf("Done. Wrote \"%s\" (%lld frames).\n", cfg.output_path, (long long)written);

    for (int c = 0; c < channels; c++) {
        free(channel_in[c]);
        free(channel_out[c]);
    }
    free(channel_in);
    free(channel_out);
    free(interleaved_out);

    return 0;
}