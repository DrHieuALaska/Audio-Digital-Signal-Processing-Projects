/*
 * Real-time audio spectrum analyzer.
 *
 * Pipeline:
 *   mic --(PortAudio callback)--> ring buffer --(main loop)--> window
 *       --> FFT --> magnitude (dB) --> ASCII bar visualizer in terminal
 *
 * Build:  make main_spec_analyzer
 * Run:    ./spectrum_analyzer
 * Quit:   Ctrl+C
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <portaudio.h>

#include "dsp_core/fft.h"
#include "dsp_core/window.h"
#include "dsp_core/ringbuffer.h"

#define SAMPLE_RATE     44100
#define FFT_SIZE        1024               /* must be a power of two */
#define RING_CAPACITY   (FFT_SIZE * 8)      /* generous headroom */
#define NUM_BARS        48                  /* terminal bar columns */
#define BAR_HEIGHT      20                  /* terminal rows per bar */
#define DB_FLOOR        -80.0f
#define DB_CEIL         0.0f
#define DISPLAY_MAX_HZ  5000.0f             /* guitar/voice range, keeps bars meaningful */

static ringbuffer_t g_ring;

/* PortAudio calls this from a real-time thread — must not block/allocate. */
static int audio_callback(const void *input_buffer, void *output_buffer,
                           unsigned long frames_per_buffer,
                           const PaStreamCallbackTimeInfo *time_info,
                           PaStreamCallbackFlags status_flags,
                           void *user_data) {
    (void)output_buffer; (void)time_info; (void)status_flags; (void)user_data;
    const float *in = (const float *)input_buffer;
    if (in) {
        ringbuffer_write(&g_ring, in, frames_per_buffer);
    }
    return paContinue;
}

static void draw_spectrum(const float *mag_db, size_t n_bins) {
    /* Only the first n/2 bins are unique for a real input signal. */
    size_t usable_bins = n_bins / 2;
    size_t max_bin = (size_t)(DISPLAY_MAX_HZ / ((float)SAMPLE_RATE / (float)n_bins));
    if (max_bin > usable_bins) max_bin = usable_bins;

    float bars[NUM_BARS];
    size_t bins_per_bar = (max_bin / NUM_BARS) > 1 ? (max_bin / NUM_BARS) : 1;

    for (size_t b = 0; b < NUM_BARS; b++) {
        size_t start = b * bins_per_bar;
        size_t end = start + bins_per_bar;
        if (end > max_bin) end = max_bin;
        float peak = DB_FLOOR;
        for (size_t k = start; k < end; k++) {
            if (mag_db[k] > peak) peak = mag_db[k];
        }
        bars[b] = peak;
    }

    printf("\033[H\033[J"); /* clear terminal + move cursor home */
    printf("Real-time spectrum (0 - %.0f Hz)   [Ctrl+C to quit]\n\n", DISPLAY_MAX_HZ);

    for (int row = BAR_HEIGHT; row >= 1; row--) {
        float threshold = DB_FLOOR + (DB_CEIL - DB_FLOOR) * ((float)row / BAR_HEIGHT);
        for (size_t b = 0; b < NUM_BARS; b++) {
            putchar(bars[b] >= threshold ? '#' : ' ');
        }
        putchar('\n');
    }
    for (size_t b = 0; b < NUM_BARS; b++) putchar('-');
    printf("\nlow freq %*sHigh freq\n", NUM_BARS - 18, "");
}

int main(void) {
    if (ringbuffer_init(&g_ring, RING_CAPACITY) != 0) {
        fprintf(stderr, "Failed to allocate ring buffer\n");
        return 1;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    PaStream *stream;
    err = Pa_OpenDefaultStream(&stream,
                                1,              /* mono input */
                                0,              /* no output */
                                paFloat32,
                                SAMPLE_RATE,
                                FFT_SIZE / 2,   /* frames per callback */
                                audio_callback,
                                NULL);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    float window_coeffs[FFT_SIZE];
    window_generate(window_coeffs, FFT_SIZE, WINDOW_HANN);

    float samples[FFT_SIZE];
    cplx_t spectrum[FFT_SIZE];
    float mag[FFT_SIZE];
    float mag_db[FFT_SIZE];

    printf("Listening on default input device... (Ctrl+C to quit)\n");

    while (1) {
        if (ringbuffer_available(&g_ring) >= FFT_SIZE) {
            ringbuffer_read(&g_ring, samples, FFT_SIZE);

            window_apply_to_complex(samples, window_coeffs, spectrum, FFT_SIZE);
            fft_forward(spectrum, FFT_SIZE);
            fft_magnitude(spectrum, mag, FFT_SIZE);

            for (size_t i = 0; i < FFT_SIZE; i++) {
                mag_db[i] = fft_magnitude_to_db(mag[i], (float)FFT_SIZE / 2.0f, DB_FLOOR);
            }

            draw_spectrum(mag_db, FFT_SIZE);
        } else {
            Pa_Sleep(5); /* avoid busy-spinning while waiting for more samples */
        }
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    ringbuffer_free(&g_ring);
    return 0;
}
