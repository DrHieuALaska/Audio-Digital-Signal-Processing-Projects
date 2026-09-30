/*
 * Pitch tuner (guitar/voice) — live mic or WAV file input.
 *
 * Live mode:
 *   mic --(PortAudio callback)--> ring buffer --(main loop)-->
 *       pitch_detector_process() --> note name + cents deviation --> display
 *
 * File mode (no live audio device required):
 *   WAV file --(read in one shot)--> pitch_detector_process() per hop
 *       --> note name + cents deviation --> printed per timestamp
 *
 * This is the piece that gets ported to fixed-point for project 4
 * (embedded tuner) — the algorithm here is exactly what runs on the
 * microcontroller, just with float math swapped for Q15/Q31.
 *
 * Build:  make main_tuner
 * Run:    ./main_tuner                 (live mic; Ctrl+C to quit)
 *         ./main_tuner recording.wav   (file mode; mono 16-bit PCM)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <portaudio.h>

#include "dsp_core/pitch_detect.h"
#include "dsp_core/ringbuffer.h"
#include "wav_io/wav_io.h"
#include "audio_util/audio_util.h"

#define SAMPLE_RATE   44100.0
#define FFT_SIZE      2048
#define HOP_SIZE      (FFT_SIZE / 2)
#define RING_CAPACITY (HOP_SIZE * 16)
#define MIN_FREQ_HZ   60.0f   /* below low E on a bass; adjust for your instrument */
#define MAX_FREQ_HZ   1200.0f

static const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static ringbuffer_t g_ring;

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

/* Converts a frequency to the nearest equal-tempered note (A4 = 440 Hz),
 * writing the note name (with octave) into name_out and the deviation
 * in cents (-50..+50) into cents_out. */
static void freq_to_note(double freq_hz, char *name_out, size_t name_len, double *cents_out) {
    double midi = 69.0 + 12.0 * log2(freq_hz / 440.0);
    long nearest_midi = lround(midi);
    double cents = (midi - (double)nearest_midi) * 100.0;

    long note_index = ((nearest_midi % 12) + 12) % 12;
    long octave = nearest_midi / 12 - 1;

    snprintf(name_out, name_len, "%s%d", NOTE_NAMES[note_index], (int)octave);
    *cents_out = cents;
}

static void draw_needle(double cents) {
    /* -50..+50 cents mapped across a 21-character-wide meter. */
    int width = 21;
    int center = width / 2;
    int pos = center + (int)lround(cents / 50.0 * center);
    if (pos < 0) pos = 0;
    if (pos >= width) pos = width - 1;

    putchar('[');
    for (int i = 0; i < width; i++) {
        if (i == center && i != pos) putchar('|');       /* in-tune marker */
        else if (i == pos) putchar('#');                  /* current reading */
        else putchar('-');
    }
    putchar(']');
}

/*
 * File mode: runs the exact same pitch_detector_process() code path
 * used live, but over a WAV file instead of a microphone. No
 * PortAudio involved at all here.
 *
 * This exists so the DSP algorithm can be verified independent of
 * whatever state your OS's audio stack happens to be in — live mic
 * capture is a real systems dependency (drivers, permissions, routing)
 * that has nothing to do with whether the pitch detector itself is
 * correct. WSL in particular is known to be unreliable for mic
 * capture specifically (playback tends to work better than capture),
 * so this mode lets you keep making progress regardless.
 */
static int run_file_mode(const char *wav_path) {
    float *samples = NULL;
    size_t n = 0;
    double sample_rate = 0.0;
    if (wav_read_mono_pcm16(wav_path, &samples, &n, &sample_rate) != 0) {
        return 1;
    }

    pitch_detector_t pd;
    if (pitch_detector_init(&pd, FFT_SIZE, HOP_SIZE, sample_rate, MIN_FREQ_HZ, MAX_FREQ_HZ) != 0) {
        fprintf(stderr, "pitch_detector_init failed\n");
        free(samples);
        return 1;
    }

    printf("Running pitch detector over '%s' (%zu samples, %.0f Hz, %.2f sec)\n\n",
           wav_path, n, sample_rate, (double)n / sample_rate);

    size_t pos = 0;
    while (pos + HOP_SIZE <= n) {
        double t = (double)pos / sample_rate;
        double freq = pitch_detector_process(&pd, &samples[pos]);

        if (freq > 0.0) {
            char note[16];
            double cents;
            freq_to_note(freq, note, sizeof(note), &cents);
            printf("t=%6.3fs  %-4s  %7.2f Hz  %+6.1f cents\n", t, note, freq, cents);
        } else {
            printf("t=%6.3fs  (no confident pitch)\n", t);
        }
        pos += HOP_SIZE;
    }

    pitch_detector_free(&pd);
    free(samples);
    return 0;
}

static int run_mic_mode(void) {
    if (ringbuffer_init(&g_ring, RING_CAPACITY) != 0) {
        fprintf(stderr, "Failed to allocate ring buffer\n");
        return 1;
    }

    pitch_detector_t pd;
    if (pitch_detector_init(&pd, FFT_SIZE, HOP_SIZE, SAMPLE_RATE, MIN_FREQ_HZ, MAX_FREQ_HZ) != 0) {
        fprintf(stderr, "pitch_detector_init failed\n");
        return 1;
    }

    PaError err = audio_initialize_quiet();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        pitch_detector_free(&pd);
        return 1;
    }

    int device = audio_find_input_device();
    if (device < 0) {
        audio_print_no_input_device_help();
        Pa_Terminate();
        pitch_detector_free(&pd);
        return 1;
    }

    PaStreamParameters input_params;
    input_params.device = device;
    input_params.channelCount = 1;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = Pa_GetDeviceInfo(device)->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = NULL;

    printf("Using input device: %s\n", Pa_GetDeviceInfo(device)->name);

    PaStream *stream;
    err = Pa_OpenStream(&stream, &input_params, NULL, SAMPLE_RATE,
                         HOP_SIZE, paNoFlag, audio_callback, NULL);
    if (err != paNoError) {
        fprintf(stderr, "Failed to open stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        pitch_detector_free(&pd);
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "Failed to start stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return 1;
    }

    printf("Listening... play a single note (Ctrl+C to quit)\n\n");

    float hop_buf[HOP_SIZE];
    while (1) {
        if (ringbuffer_available(&g_ring) >= HOP_SIZE) {
            ringbuffer_read(&g_ring, hop_buf, HOP_SIZE);
            double freq = pitch_detector_process(&pd, hop_buf);

            if (freq > 0.0) {
                char note[16];
                double cents;
                freq_to_note(freq, note, sizeof(note), &cents);
                printf("\r%-4s  %7.2f Hz  %+6.1f cents  ", note, freq, cents);
                draw_needle(cents);
                printf("   ");
            } else {
                // printf("\r(listening...)                                                  ");
            }
            fflush(stdout);
        } else {
            Pa_Sleep(5);
        }
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    pitch_detector_free(&pd);
    ringbuffer_free(&g_ring);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        return run_file_mode(argv[1]);
    }
    return run_mic_mode();
}
