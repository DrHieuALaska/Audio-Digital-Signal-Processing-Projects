#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wav_io/wav_io.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(void) {
    size_t n = 44100;
    double sr = 44100.0;
    float *original = (float *)malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        original[i] = 0.5f * (float)sin(2.0 * M_PI * 440.0 * (double)i / sr);
    }

    if (wav_write_mono_pcm16("/tmp/wav_io_test.wav", original, n, sr) != 0) {
        printf("FAIL: write failed\n");
        return 1;
    }

    float *readback = NULL;
    size_t read_len = 0;
    double read_sr = 0.0;
    if (wav_read_mono_pcm16("/tmp/wav_io_test.wav", &readback, &read_len, &read_sr) != 0) {
        printf("FAIL: read failed\n");
        return 1;
    }

    int ok = 1;
    if (read_len != n) { printf("FAIL: length mismatch %zu vs %zu\n", read_len, n); ok = 0; }
    if (read_sr != sr) { printf("FAIL: sample rate mismatch %.1f vs %.1f\n", read_sr, sr); ok = 0; }

    float max_err = 0.0f;
    size_t compare_n = read_len < n ? read_len : n;
    for (size_t i = 0; i < compare_n; i++) {
        float err = fabsf(original[i] - readback[i]);
        if (err > max_err) max_err = err;
    }
    printf("max round-trip error (16-bit quantization) = %e\n", max_err);
    /* 1/32768 is the 16-bit quantization step -- error should be within a couple steps. */
    if (max_err > 2.0f / 32768.0f) { printf("FAIL: quantization error too large\n"); ok = 0; }

    free(original);
    free(readback);

    if (!ok) return 1;
    printf("PASS: WAV round-trip matches within 16-bit quantization\n");
    return 0;
}
