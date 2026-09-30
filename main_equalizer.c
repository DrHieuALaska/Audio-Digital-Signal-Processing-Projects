#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <portaudio.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "audio_util/audio_util.h"
#include "dsp_core/fft.h"
#include "dsp_core/stft.h"
#include "dsp_core/window.h"
#include "wav_io/wav_io.h"

#define SAMPLE_RATE 44100.0
#define FFT_SIZE 1024
#define HOP_SIZE (FFT_SIZE / 2)
#define LATENCY_SAMPLES HOP_SIZE
#define BAND_LOW_MAX_HZ 250.0
#define BAND_MID_MAX_HZ 4000.0
#define GAIN_STEP_DB 1.5f
#define GATE_STEP_DB 2.0f

typedef struct {
    float low_db, mid_db, high_db, gate_threshold_db;
    int gate_enabled;
} eq_config_t;

static _Atomic float g_low_db, g_mid_db, g_high_db;
static _Atomic float g_gate_threshold_db = -55.0f;
static _Atomic int g_gate_enabled, g_quit;
static stft_t g_live_stft;
static volatile sig_atomic_t g_stop_requested;

static float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }

static void apply_effects(cplx_t *spectrum, size_t n, double sample_rate,
                          const eq_config_t *cfg) {
    const float low = db_to_linear(cfg->low_db);
    const float mid = db_to_linear(cfg->mid_db);
    const float high = db_to_linear(cfg->high_db);
    const size_t nyquist = n / 2;
    for (size_t k = 0; k <= nyquist; ++k) {
        const double hz = (double)k * sample_rate / (double)n;
        const float band = hz < BAND_LOW_MAX_HZ ? low :
                           hz < BAND_MID_MAX_HZ ? mid : high;
        float gate = 1.0f;
        if (cfg->gate_enabled) {
            const float mag = sqrtf(spectrum[k].re * spectrum[k].re +
                                    spectrum[k].im * spectrum[k].im);

            float level_db = fft_magnitude_to_db(mag, (float)n / 4.0f, -120.0f);
            const float ratio = 2.0f; // ratio 2:1 for the gate
            const float max_atten_db = -30.0f;
            const float threshold = cfg->gate_threshold_db;

            float gain_db = 0.0f;

            if (level_db < threshold) {
                gain_db = (level_db - threshold) * (ratio - 1.0f);

                if (gain_db < max_atten_db)
                    gain_db = max_atten_db; // Limit maximum attenuation
            }
            gate = db_to_linear(gain_db);
        }
        spectrum[k].re *= band * gate;
        spectrum[k].im *= band * gate;
        if (k && k != nyquist) {
            spectrum[n-k].re *= band * gate;
            spectrum[n-k].im *= band * gate;
        }
    }
}

static eq_config_t live_config(void) {
    eq_config_t c = { atomic_load(&g_low_db), atomic_load(&g_mid_db),
        atomic_load(&g_high_db), atomic_load(&g_gate_threshold_db),
        atomic_load(&g_gate_enabled) };
    return c;
}

static void bump(_Atomic float *v, float d) { atomic_store(v, atomic_load(v) + d); }
static void live_key(char c) {
    switch (c) {
    case 'w': bump(&g_low_db, GAIN_STEP_DB); break;
    case 's': bump(&g_low_db, -GAIN_STEP_DB); break;
    case 'e': bump(&g_mid_db, GAIN_STEP_DB); break;
    case 'd': bump(&g_mid_db, -GAIN_STEP_DB); break;
    case 'r': bump(&g_high_db, GAIN_STEP_DB); break;
    case 'f': bump(&g_high_db, -GAIN_STEP_DB); break;
    case 'g': atomic_store(&g_gate_enabled, !atomic_load(&g_gate_enabled)); break;
    case 't': bump(&g_gate_threshold_db, -GATE_STEP_DB); break;
    case 'y': bump(&g_gate_threshold_db, GATE_STEP_DB); break;
    case 'q': atomic_store(&g_quit, 1); break;
    }
}

static int live_callback(const void *input, void *output, unsigned long frames,
        const PaStreamCallbackTimeInfo *time, PaStreamCallbackFlags flags, void *data) {
    (void)time; (void)flags; (void)data;
    const float *in = input; float *out = output;
    if (!in || !out || frames != HOP_SIZE) {
        if (out) memset(out, 0, frames * sizeof(*out));
        return paContinue;
    }
    eq_config_t cfg = live_config();
    stft_analyze(&g_live_stft, in);
    apply_effects(g_live_stft.spectrum, FFT_SIZE, SAMPLE_RATE, &cfg);
    stft_synthesize(&g_live_stft, out);
    return paContinue;
}

static struct termios g_original_termios;
static int g_original_flags;
static void restore_terminal(void) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
    if (g_original_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, g_original_flags);
}
static int raw_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &g_original_termios) != 0) return -1;
    struct termios raw = g_original_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    g_original_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (g_original_flags >= 0) (void)fcntl(STDIN_FILENO, F_SETFL, g_original_flags | O_NONBLOCK);
    atexit(restore_terminal);
    return 0;
}

static int open_input_stream(PaStream **stream, double rate, int blocking) {
    PaError e = audio_initialize_quiet();
    if (e != paNoError) { fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(e)); return -1; }
    int device = audio_find_input_device();
    if (device < 0) { audio_print_no_input_device_help(); Pa_Terminate(); return -1; }
    const PaDeviceInfo *info = Pa_GetDeviceInfo(device);
    PaStreamParameters in = { device, 1, paFloat32, info->defaultLowInputLatency, NULL };
    e = Pa_OpenStream(stream, &in, NULL, rate, HOP_SIZE, paNoFlag,
                      blocking ? NULL : live_callback, NULL);
    if (e != paNoError) { fprintf(stderr, "Failed to open input stream: %s\n", Pa_GetErrorText(e)); Pa_Terminate(); return -1; }
    e = Pa_StartStream(*stream);
    if (e != paNoError) { fprintf(stderr, "Failed to start input stream: %s\n", Pa_GetErrorText(e)); Pa_CloseStream(*stream); Pa_Terminate(); return -1; }
    return 0;
}

static int open_live_stream(PaStream **stream) {
    PaError e = audio_initialize_quiet();
    if (e != paNoError) { fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(e)); return -1; }
    int input_device = audio_find_input_device();
    int output_device = audio_find_output_device();
    if (input_device < 0 || output_device < 0) {
        if (input_device < 0) audio_print_no_input_device_help();
        else fprintf(stderr, "No usable audio output device was found.\n");
        Pa_Terminate(); return -1;
    }
    const PaDeviceInfo *ii = Pa_GetDeviceInfo(input_device);
    const PaDeviceInfo *oi = Pa_GetDeviceInfo(output_device);
    PaStreamParameters in = { input_device, 1, paFloat32, ii->defaultLowInputLatency, NULL };
    PaStreamParameters out = { output_device, 1, paFloat32, oi->defaultLowOutputLatency, NULL };
    e = Pa_OpenStream(stream, &in, &out, SAMPLE_RATE, HOP_SIZE,
                      paNoFlag, live_callback, NULL);
    if (e != paNoError) { fprintf(stderr, "Failed to open duplex stream: %s\n", Pa_GetErrorText(e)); Pa_Terminate(); return -1; }
    e = Pa_StartStream(*stream);
    if (e != paNoError) { fprintf(stderr, "Failed to start stream: %s\n", Pa_GetErrorText(e)); Pa_CloseStream(*stream); Pa_Terminate(); return -1; }
    return 0;
}

static int run_live(void) {
    if (stft_init(&g_live_stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) { fprintf(stderr, "stft_init failed\n"); return 1; }
    PaStream *stream;
    if (open_live_stream(&stream) != 0) { stft_free(&g_live_stft); return 1; }
    printf("Live EQ + noise gate. Keys: w/s bass, e/d mid, r/f treble, g gate, t/y threshold, q quit.\n");
    (void)raw_terminal();
    while (!atomic_load(&g_quit)) {
        printf("\rbass %+.1fdB mid %+.1fdB treble %+.1fdB gate %s (%+.0fdB)   ",
            atomic_load(&g_low_db), atomic_load(&g_mid_db), atomic_load(&g_high_db),
            atomic_load(&g_gate_enabled) ? "ON" : "OFF", atomic_load(&g_gate_threshold_db));
        fflush(stdout);
        char key; ssize_t n = read(STDIN_FILENO, &key, 1);
        if (n == 1) live_key(key);
        Pa_Sleep(50);
    }
    printf("\nShutting down.\n");
    Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate(); stft_free(&g_live_stft);
    return 0;
}

static void stop_signal(int sig) { (void)sig; g_stop_requested = 1; }
static int key_waiting(void) {
    char c; int found = 0;
    while (read(STDIN_FILENO, &c, 1) == 1) found = 1;
    return found;
}
static int reserve_samples(float **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return 0;
    size_t next = *cap ? *cap : HOP_SIZE * 32;
    while (next < needed) {
        if (next > SIZE_MAX / 2 / sizeof(float)) return -1;
        next *= 2;
    }
    float *p = realloc(*buf, next * sizeof(float));
    if (!p) return -1;
    *buf = p; *cap = next; return 0;
}

static int run_record(const char *path, eq_config_t cfg) {
    stft_t stft;
    if (stft_init(&stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) { fprintf(stderr, "stft_init failed\n"); return 1; }
    PaStream *stream;
    if (open_input_stream(&stream, SAMPLE_RATE, 1) != 0) { stft_free(&stft); return 1; }
    if (raw_terminal() != 0) fprintf(stderr, "Warning: terminal key detection unavailable; use Ctrl+C to stop.\n");
    signal(SIGINT, stop_signal);
    printf("Recording EQ: bass %+.1f dB, mid %+.1f dB, treble %+.1f dB, gate %s (%.0f dB).\n",
           cfg.low_db, cfg.mid_db, cfg.high_db,
           cfg.gate_enabled ? "ON" : "OFF", cfg.gate_threshold_db);
    printf("Recording processed audio to '%s'. Press any key or Ctrl+C to stop.\n", path);
    float *samples = NULL; size_t length = 0, capacity = 0;
    float in[HOP_SIZE], out[HOP_SIZE]; int failed = 0;
    while (!g_stop_requested) {
        PaError e = Pa_ReadStream(stream, in, HOP_SIZE);
        if (e != paNoError && e != paInputOverflowed) { fprintf(stderr, "Audio read failed: %s\n", Pa_GetErrorText(e)); failed = 1; break; }
        apply_effects((stft_analyze(&stft, in), stft.spectrum), FFT_SIZE, SAMPLE_RATE, &cfg);
        stft_synthesize(&stft, out);
        if (length > SIZE_MAX - HOP_SIZE || reserve_samples(&samples, &capacity, length + HOP_SIZE) != 0) { fprintf(stderr, "Out of memory recording audio\n"); failed = 1; break; }
        memcpy(samples + length, out, sizeof(out)); length += HOP_SIZE;
        if (key_waiting()) break;
    }
    memset(in, 0, sizeof(in)); stft_analyze(&stft, in);
    apply_effects(stft.spectrum, FFT_SIZE, SAMPLE_RATE, &cfg); stft_synthesize(&stft, out);
    if (!failed && reserve_samples(&samples, &capacity, length + HOP_SIZE) == 0) { memcpy(samples + length, out, sizeof(out)); length += HOP_SIZE; }
    Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate(); stft_free(&stft);
    /* The STFT delay is removed and the trailing silence used to flush its overlap is omitted. */
    size_t written_n = length > LATENCY_SAMPLES ? length - LATENCY_SAMPLES : 0;
    int rc = failed || !written_n || wav_write_mono_pcm16(path, samples + (length > LATENCY_SAMPLES ? LATENCY_SAMPLES : 0), written_n, SAMPLE_RATE) != 0;
    if (!rc) printf("Saved %zu samples to '%s'.\n", written_n, path);
    free(samples); return rc ? 1 : 0;
}

static int process_file(const char *in_path, const char *out_path, eq_config_t cfg) {
    float *input = NULL; size_t n = 0; double rate = 0;
    if (wav_read_mono_pcm16(in_path, &input, &n, &rate) != 0) return 1;
    stft_t stft;
    if (stft_init(&stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) { fprintf(stderr, "stft_init failed\n"); free(input); return 1; }
    if (n > SIZE_MAX - FFT_SIZE) { fprintf(stderr, "Input is too large\n"); stft_free(&stft); free(input); return 1; }
    size_t hops = (n + HOP_SIZE - 1) / HOP_SIZE + 1;
    size_t scratch_n = hops * HOP_SIZE;
    float *scratch = calloc(scratch_n, sizeof(float));
    float *output = malloc(n * sizeof(float));
    if (!scratch || !output) { fprintf(stderr, "Out of memory\n"); free(input); free(scratch); free(output); stft_free(&stft); return 1; }
    float in[HOP_SIZE], out[HOP_SIZE]; size_t produced = 0;
    for (size_t h = 0; h < hops; ++h) {
        size_t offset = h * HOP_SIZE, count = offset < n ? n - offset : 0;
        if (count > HOP_SIZE) count = HOP_SIZE;
        memset(in, 0, sizeof(in)); if (count) memcpy(in, input + offset, count * sizeof(float));
        stft_analyze(&stft, in); apply_effects(stft.spectrum, FFT_SIZE, rate, &cfg);
        stft_synthesize(&stft, out); memcpy(scratch + produced, out, sizeof(out)); produced += HOP_SIZE;
    }
    memcpy(output, scratch + LATENCY_SAMPLES, n * sizeof(float));
    int rc = wav_write_mono_pcm16(out_path, output, n, rate) != 0;
    if (!rc) printf("Processed '%s' (%zu samples, %.0f Hz) to '%s'.\n", in_path, n, rate, out_path);
    free(input); free(scratch); free(output); stft_free(&stft); return rc ? 1 : 0;
}

static void usage(const char *p) {
    fprintf(stderr, "Usage:\n  %s                         live microphone EQ\n  %s record.wav [--bass DB] [--mid DB] [--treble DB] [--gate] [--gate-threshold DB]\n  %s input.wav output.wav [--bass DB] [--mid DB] [--treble DB] [--gate] [--gate-threshold DB]\n", p, p, p);
}
int main(int argc, char **argv) {
    if (argc == 1) return run_live();
    eq_config_t cfg = {0, 0, 0, -55.0f, 0};
    int recording = argc == 2 || (argc >= 3 && argv[2][0] == '-');
    int option_start = recording ? 2 : 3;
    if (!recording && argc < 3) { usage(argv[0]); return 2; }
    for (int i = option_start; i < argc; ++i) {
        if (!strcmp(argv[i], "--gate")) cfg.gate_enabled = 1;
        else if (!strcmp(argv[i], "--bass") && i + 1 < argc) cfg.low_db = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--mid") && i + 1 < argc) cfg.mid_db = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--treble") && i + 1 < argc) cfg.high_db = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--gate-threshold") && i + 1 < argc) cfg.gate_threshold_db = strtof(argv[++i], NULL);
        else { fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (recording) return run_record(argv[1], cfg);
    return process_file(argv[1], argv[2], cfg);
}
