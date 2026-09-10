/*
 * Real-time 3-band equalizer + spectral noise gate.
 *
 * Pipeline (all inside a single duplex PortAudio callback):
 *   mic in (hop_size samples)
 *     --> stft_analyze()         [window + FFT, from dsp_core/stft.c]
 *     --> apply_spectral_effects [EQ band gains + noise gate, this file]
 *     --> stft_synthesize()      [IFFT + overlap-add, from dsp_core/stft.c]
 *     --> speaker out (hop_size samples)
 *
 * Live control via keyboard (no need to restart):
 *   w/s : bass gain up/down      (< 250 Hz)
 *   e/d : mid gain up/down       (250 Hz - 4 kHz)
 *   r/f : treble gain up/down    (> 4 kHz)
 *   g   : toggle noise gate on/off
 *   t/y : gate threshold down/up (more/less aggressive)
 *   q   : quit
 *
 * IMPORTANT DESIGN NOTE: for simplicity, the STFT processing (FFT +
 * effect + IFFT) runs directly inside the real-time audio callback.
 * At FFT_SIZE=1024 this easily fits within the ~11.6ms callback budget
 * on any modern desktop CPU, so it's fine for a learning project.
 * A production real-time audio engine would instead push raw samples
 * from the callback into a lock-free queue and do the FFT work on a
 * separate, non-real-time-priority thread, to guarantee the callback
 * itself never has unbounded work. That decoupling is exactly the
 * ring-buffer pattern already used in project 1 (dsp_core/ringbuffer.c)
 * — worth revisiting here if you want to harden this into something
 * you'd actually ship.
 *
 * Build:  make main_equalizer
 * Run:    ./main_equalizer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <portaudio.h>

#include "dsp_core/fft.h"
#include "dsp_core/window.h"
#include "dsp_core/stft.h"

#define SAMPLE_RATE   44100.0 // Nyquist = fs/2 = 22050 Hz
#define FFT_SIZE      1024
#define HOP_SIZE      (FFT_SIZE / 2)

#define BAND_LOW_MAX_HZ   250.0
#define BAND_MID_MAX_HZ   4000.0
/*
    |   Low band    |     Mid band     |      High Band     |
    0              250                4000                22050
*/

#define GAIN_STEP_DB      1.5f
#define GATE_STEP_DB      2.0f

static _Atomic float g_gain_low_db  = 0.0f;
static _Atomic float g_gain_mid_db  = 0.0f;
static _Atomic float g_gain_high_db = 0.0f;
static _Atomic float g_gate_threshold_db = -55.0f;
static _Atomic int   g_gate_enabled = 0;
static _Atomic int   g_quit = 0;

static stft_t g_stft;

static float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

/*
 * Applies EQ band gains and (optionally) a spectral noise gate to one
 * frame's spectrum, in place.
 *
 * Correctness detail that matters: the input is real-valued, so the
 * spectrum has conjugate symmetry (X[N-k] = conj(X[k])). Any per-bin
 * gain we apply MUST be identical for bin k and its mirror bin N-k,
 * or the inverse FFT will produce a signal with a non-negligible
 * imaginary part — i.e. it silently stops being real audio. We handle
 * this by only iterating k = 0..N/2 and writing the same real gain to
 * both k and its mirror in the same step.
 */
static void apply_spectral_effects(cplx_t *spectrum, size_t n, double sample_rate) {
    float low_db  = atomic_load(&g_gain_low_db);
    float mid_db  = atomic_load(&g_gain_mid_db);
    float high_db = atomic_load(&g_gain_high_db);
    float gate_threshold = atomic_load(&g_gate_threshold_db);
    int gate_on = atomic_load(&g_gate_enabled);

    float low_gain  = db_to_linear(low_db);
    float mid_gain  = db_to_linear(mid_db);
    float high_gain = db_to_linear(high_db);

    size_t nyquist_bin = n / 2;

    for (size_t k = 0; k <= nyquist_bin; k++) {
        double freq = (double)k * sample_rate / (double)n;

        float band_gain;
        if (freq < BAND_LOW_MAX_HZ)      band_gain = low_gain;
        else if (freq < BAND_MID_MAX_HZ) band_gain = mid_gain;
        else                              band_gain = high_gain;

        float gate_gain = 1.0f;
        if (gate_on) {
            float mag = sqrtf(spectrum[k].re * spectrum[k].re
                             + spectrum[k].im * spectrum[k].im);
            /* ref chosen so a full-scale sine's dominant bin sits near 0 dB */
            float db = fft_magnitude_to_db(mag, (float)n / 4.0f, -120.0f);
            if (db < gate_threshold) {
                gate_gain = 0.0f; /* hard gate: simple, but can cause
                                      "musical noise" artifacts on
                                      complex material — a smoother
                                      soft-knee gain curve is the
                                      natural next improvement here */
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

static int audio_callback(const void *input_buffer, void *output_buffer,
                           unsigned long frames_per_buffer,
                           const PaStreamCallbackTimeInfo *time_info,
                           PaStreamCallbackFlags status_flags,
                           void *user_data) {
    (void)time_info; (void)status_flags; (void)user_data;
    const float *in = (const float *)input_buffer;
    float *out = (float *)output_buffer;

    if (!in || !out || frames_per_buffer != HOP_SIZE) {
        if (out) memset(out, 0, frames_per_buffer * sizeof(float));
        return paContinue;
    }

    stft_analyze(&g_stft, in);
    apply_spectral_effects(g_stft.spectrum, FFT_SIZE, SAMPLE_RATE);
    stft_synthesize(&g_stft, out);

    return paContinue;
}

/* --- minimal non-blocking keyboard input (POSIX termios) --- */
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

static void print_status(void) {
    printf("\rbass %+.1fdB  mid %+.1fdB  treble %+.1fdB   gate %s (%.0fdB)   [w/s e/d r/f | g | t/y | q]   ",
           atomic_load(&g_gain_low_db),
           atomic_load(&g_gain_mid_db),
           atomic_load(&g_gain_high_db),
           atomic_load(&g_gate_enabled) ? "ON " : "OFF",
           atomic_load(&g_gate_threshold_db));
    fflush(stdout);
}

/* C11's atomic_fetch_add is only defined for integer/pointer types, not
 * float, so float "atomics" here are a plain load-modify-store. This is
 * safe in practice: only this one thread (keyboard input) ever writes
 * these variables; the audio callback only reads them. A torn read
 * would at worst cause one frame's gain to be very briefly stale —
 * inaudible and harmless, so a heavier CAS loop isn't warranted. */
static void bump_atomic_float(_Atomic float *var, float delta) {
    atomic_store(var, atomic_load(var) + delta);
}

static void handle_key(char c) {
    switch (c) {
        case 'w': bump_atomic_float(&g_gain_low_db, GAIN_STEP_DB); break;
        case 's': bump_atomic_float(&g_gain_low_db, -GAIN_STEP_DB); break;
        case 'e': bump_atomic_float(&g_gain_mid_db, GAIN_STEP_DB); break;
        case 'd': bump_atomic_float(&g_gain_mid_db, -GAIN_STEP_DB); break;
        case 'r': bump_atomic_float(&g_gain_high_db, GAIN_STEP_DB); break;
        case 'f': bump_atomic_float(&g_gain_high_db, -GAIN_STEP_DB); break;
        case 'g': atomic_store(&g_gate_enabled, !atomic_load(&g_gate_enabled)); break;
        case 't': bump_atomic_float(&g_gate_threshold_db, -GATE_STEP_DB); break;
        case 'y': bump_atomic_float(&g_gate_threshold_db, GATE_STEP_DB); break;
        case 'q': atomic_store(&g_quit, 1); break;
        default: break;
    }
}

int main(void) {
    if (stft_init(&g_stft, FFT_SIZE, HOP_SIZE, WINDOW_HANN) != 0) {
        fprintf(stderr, "stft_init failed\n");
        return 1;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    PaStreamParameters input_params, output_params;
    input_params.device = Pa_GetDefaultInputDevice();
    input_params.channelCount = 1;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = Pa_GetDeviceInfo(input_params.device)->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = NULL;

    output_params.device = Pa_GetDefaultOutputDevice();
    output_params.channelCount = 1;
    output_params.sampleFormat = paFloat32;
    output_params.suggestedLatency = Pa_GetDeviceInfo(output_params.device)->defaultLowOutputLatency;
    output_params.hostApiSpecificStreamInfo = NULL;

    PaStream *stream;
    err = Pa_OpenStream(&stream, &input_params, &output_params, SAMPLE_RATE,
                         HOP_SIZE, paNoFlag, audio_callback, NULL);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open duplex stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    printf("Live EQ + noise gate running. Speak/play into your mic.\n");
    printf("NOTE: use headphones to avoid feedback from speaker into mic.\n\n");
    set_raw_nonblocking_terminal();

    while (!atomic_load(&g_quit)) {
        print_status();
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 1) {
            handle_key(c);
        }
        Pa_Sleep(50);
    }

    printf("\nShutting down...\n");
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    stft_free(&g_stft);
    return 0;
}
