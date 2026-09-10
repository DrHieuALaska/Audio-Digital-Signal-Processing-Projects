/*
 * Offline 3-band equalizer + spectral noise gate — RECORD-TO-FILE version.
 *
 * Unlike main_equalizer.c (live playback + live keyboard control), this
 * program:
 *   1. Takes ALL effect settings up front, as command-line arguments
 *      (no live EQ/gate tweaking while recording — the mic is busy and
 *      there's no speaker output to monitor against).
 *   2. Records from the microphone for as long as the user wants: it
 *      keeps going until they press any key or hit Ctrl+C, rather than
 *      forcing them to pre-guess a duration in seconds (nobody knows
 *      exactly how long they're about to talk). An optional
 *      --max-seconds safety cap is still available if you want one
 *      (e.g. for an unattended/scripted recording).
 *   3. Runs each captured hop through the same STFT -> EQ/gate -> ISTFT
 *      pipeline as the live version.
 *   4. Writes the processed audio straight to a WAV file as it goes
 *      (streamed via libsndfile, not buffered in memory) — so even if
 *      you Ctrl+C out, everything captured so far is already on disk.
 *
 * Pipeline per hop (identical DSP core to main_equalizer.c):
 *   mic in (HOP_SIZE samples)
 *     --> stft_analyze()         [window + FFT, dsp_core/stft.c]
 *     --> apply_spectral_effects [EQ band gains + noise gate]
 *     --> stft_synthesize()      [IFFT + overlap-add, dsp_core/stft.c]
 *     --> sf_writef_float()      [append to output WAV file]
 *
 * This version uses PortAudio's BLOCKING API (Pa_ReadStream) instead of
 * a callback, since there's no real-time output to keep in lockstep
 * with — we just pull mic data as it becomes available and process it
 * inline. That means the "keep the callback cheap" concern from
 * main_equalizer.c's design note doesn't apply here: there is no
 * callback at all. Because Pa_ReadStream only blocks for one hop
 * (~11.6ms at these settings) at a time, checking for a keypress
 * between hops keeps stop latency imperceptible.
 *
 * Stopping: press any key, or Ctrl+C, at any time. Either way, what
 * you've recorded so far is flushed and written to the output file
 * before the program exits — nothing is thrown away.
 *
 * Usage:
 *   ./record_eq --output take1.wav \
 *       [--bass DB] [--mid DB] [--treble DB] \
 *       [--gate] [--gate-threshold DB] [--sample-rate HZ] \
 *       [--max-seconds N]
 *
 * Defaults: bass=mid=treble=0dB, gate off, gate-threshold=-55dB,
 *           sample-rate=44100, no max duration (stop manually).
 *
 * Run:    ./record_eq --output take1.wav --bass 3 --treble -2
 *         (then just talk, and press any key when you're done)
 *
 * NOTE ON THE dsp_core API: this file assumes the exact same
 * dsp_core/{fft,window,stft}.h interface used by main_equalizer.c
 * (stft_init/stft_analyze/stft_synthesize/stft_free, cplx_t{re,im},
 * fft_magnitude_to_db, WINDOW_HANN). If your actual headers differ,
 * adjust the calls below accordingly — apply_spectral_effects() itself
 * is copied verbatim from main_equalizer.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <portaudio.h>
#include <sndfile.h>

#include "dsp_core/fft.h"
#include "dsp_core/window.h"
#include "dsp_core/stft.h"

#define FFT_SIZE      1024
#define HOP_SIZE      (FFT_SIZE / 2)

#define BAND_LOW_MAX_HZ   250.0
#define BAND_MID_MAX_HZ   4000.0

typedef struct {
    double sample_rate;
    float  gain_low_db;
    float  gain_mid_db;
    float  gain_high_db;
    float  gate_threshold_db;
    int    gate_enabled;
    double max_seconds; /* <= 0 means "no cap, stop on keypress/Ctrl+C only" */
    const char *output_path;
} config_t;

static float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

/* Identical to main_equalizer.c's apply_spectral_effects, just reading
 * plain (non-atomic) config: settings are fixed before recording
 * starts and nothing else ever writes them while this runs, so there's
 * no cross-thread hazard to guard against here. */
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
            /* ref chosen so a full-scale sine's dominant bin sits near 0 dB */
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

/* Set by the SIGINT handler (Ctrl+C) or by detecting a keypress in the
 * main loop. sig_atomic_t is what's actually safe to touch from a
 * signal handler; we still read it as a plain int elsewhere since only
 * the main thread ever does that. */
static volatile sig_atomic_t g_stop_requested = 0;

static void handle_sigint(int signum) {
    (void)signum;
    g_stop_requested = 1;
}

/* --- minimal non-blocking keyboard input (POSIX termios), same
 * approach as main_equalizer.c's set_raw_nonblocking_terminal --- */
static struct termios g_orig_termios;

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static void set_raw_nonblocking_terminal(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    atexit(restore_terminal);
}

/* Non-blocking check: did the user press any key since we last looked?
 * Drains stdin so repeated keypresses don't queue up oddly, and
 * returns true if there was at least one byte waiting. */
static int keypress_waiting(void) {
    char c;
    int got_one = 0;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        got_one = 1;
    }
    return got_one;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --output FILE.wav "
        "[--bass DB] [--mid DB] [--treble DB] [--gate] [--gate-threshold DB] "
        "[--sample-rate HZ] [--max-seconds N]\n"
        "  Recording stops when you press any key or hit Ctrl+C.\n"
        "  --max-seconds is an optional safety cap; omit it to record\n"
        "  for as long as you like.\n",
        prog);
}

static int parse_args(int argc, char **argv, config_t *cfg) {
    cfg->sample_rate = 44100.0;
    cfg->gain_low_db = cfg->gain_mid_db = cfg->gain_high_db = 0.0f;
    cfg->gate_threshold_db = -55.0f;
    cfg->gate_enabled = 0;
    cfg->max_seconds = -1.0; /* no cap by default */
    cfg->output_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) {
            cfg->max_seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
            cfg->output_path = argv[++i];
        } else if (!strcmp(argv[i], "--bass") && i + 1 < argc) {
            cfg->gain_low_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mid") && i + 1 < argc) {
            cfg->gain_mid_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--treble") && i + 1 < argc) {
            cfg->gain_high_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--gate")) {
            cfg->gate_enabled = 1;
        } else if (!strcmp(argv[i], "--gate-threshold") && i + 1 < argc) {
            cfg->gate_threshold_db = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--sample-rate") && i + 1 < argc) {
            cfg->sample_rate = atof(argv[++i]);
        } else {
            fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!cfg->output_path) {
        fprintf(stderr, "--output is required.\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    config_t cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("Config: bass %+.1fdB  mid %+.1fdB  treble %+.1fdB  gate %s (%.0fdB)\n",
           cfg.gain_low_db, cfg.gain_mid_db, cfg.gain_high_db,
           cfg.gate_enabled ? "ON" : "OFF", cfg.gate_threshold_db);
    if (cfg.max_seconds > 0.0) {
        printf("Recording to \"%s\" (up to %.1fs, or press any key / Ctrl+C to stop sooner)...\n",
               cfg.output_path, cfg.max_seconds);
    } else {
        printf("Recording to \"%s\". Press any key or Ctrl+C when you're done...\n",
               cfg.output_path);
    }

    signal(SIGINT, handle_sigint);

    stft_t stft;
    if (stft_init(&stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) {
        fprintf(stderr, "stft_init failed\n");
        return 1;
    }

    PaError perr = Pa_Initialize();
    if (perr != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(perr));
        stft_free(&stft);
        return 1;
    }

    PaStreamParameters input_params;
    input_params.device = Pa_GetDefaultInputDevice();
    if (input_params.device == paNoDevice) {
        fprintf(stderr, "No default input device found.\n");
        Pa_Terminate();
        stft_free(&stft);
        return 1;
    }
    input_params.channelCount = 1;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = Pa_GetDeviceInfo(input_params.device)->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = NULL;

    PaStream *stream;
    /* No callback (NULL) — this is a blocking, input-only stream. We
     * pull hops with Pa_ReadStream in the loop below. */
    perr = Pa_OpenStream(&stream, &input_params, NULL, cfg.sample_rate,
                          HOP_SIZE, paNoFlag, NULL, NULL);
    if (perr != paNoError) {
        fprintf(stderr, "Failed to open input stream: %s\n", Pa_GetErrorText(perr));
        Pa_Terminate();
        stft_free(&stft);
        return 1;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(sfinfo));
    sfinfo.samplerate = (int)cfg.sample_rate;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE *outfile = sf_open(cfg.output_path, SFM_WRITE, &sfinfo);
    if (!outfile) {
        fprintf(stderr, "Failed to open output file \"%s\": %s\n",
                cfg.output_path, sf_strerror(NULL));
        Pa_CloseStream(stream);
        Pa_Terminate();
        stft_free(&stft);
        return 1;
    }

    perr = Pa_StartStream(stream);
    if (perr != paNoError) {
        fprintf(stderr, "Failed to start stream: %s\n", Pa_GetErrorText(perr));
        sf_close(outfile);
        Pa_CloseStream(stream);
        Pa_Terminate();
        stft_free(&stft);
        return 1;
    }

    printf("Recording... speak now.\n");
    set_raw_nonblocking_terminal();

    float in_buf[HOP_SIZE];
    float out_buf[HOP_SIZE];

    /* hop==-1 sentinel would be overkill; just count hops so far and
     * compare against the optional cap each iteration. */
    long hop = 0;
    long max_hops = (cfg.max_seconds > 0.0)
        ? (long)ceil(cfg.max_seconds * cfg.sample_rate / (double)HOP_SIZE)
        : -1; /* -1 means "no cap" */

    while (!g_stop_requested) {
        if (max_hops >= 0 && hop >= max_hops) {
            break; /* hit the optional safety cap */
        }

        perr = Pa_ReadStream(stream, in_buf, HOP_SIZE);
        if (perr != paNoError && perr != paInputOverflowed) {
            fprintf(stderr, "\nPa_ReadStream error: %s\n", Pa_GetErrorText(perr));
            break;
        }

        stft_analyze(&stft, in_buf);
        apply_spectral_effects(stft.spectrum, FFT_SIZE, cfg.sample_rate, &cfg);
        stft_synthesize(&stft, out_buf);

        sf_count_t written = sf_writef_float(outfile, out_buf, HOP_SIZE);
        if (written != HOP_SIZE) {
            fprintf(stderr, "\nWarning: short write to output file (%lld/%d frames)\n",
                    (long long)written, HOP_SIZE);
        }

        hop++;
        if (hop % 20 == 0) {
            printf("\r  %.1fs recorded (press any key to stop)  ",
                   (hop * HOP_SIZE) / cfg.sample_rate);
            fflush(stdout);
        }

        /* Cheap, non-blocking: Pa_ReadStream above already pauses for
         * one hop's worth of audio (~11.6ms at these settings), so
         * polling stdin here doesn't add meaningfully to stop latency. */
        if (keypress_waiting()) {
            break;
        }
    }
    printf("\r  %.1fs recorded. Stopping...\n", (hop * HOP_SIZE) / cfg.sample_rate);

    /* Flush: the STFT/overlap-add pipeline holds back roughly one hop
     * of audio (window overlap latency) before it comes out the other
     * end. Feed one hop of silence so that final bit of real speech
     * still gets written, instead of being lost when we just stop. */
    memset(in_buf, 0, sizeof(in_buf));
    stft_analyze(&stft, in_buf);
    apply_spectral_effects(stft.spectrum, FFT_SIZE, cfg.sample_rate, &cfg);
    stft_synthesize(&stft, out_buf);
    sf_writef_float(outfile, out_buf, HOP_SIZE);

    printf("Done. Wrote \"%s\".\n", cfg.output_path);

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    sf_close(outfile);
    stft_free(&stft);
    return 0;
}