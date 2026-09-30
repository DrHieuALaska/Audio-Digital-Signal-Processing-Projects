#include "wav_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Little-endian read helpers -- WAV/RIFF fields are always little-endian. */
static uint32_t read_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_u16le(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static void write_u32le(FILE *f, uint32_t v) {
    unsigned char b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    fwrite(b, 1, 4, f);
}
static void write_u16le(FILE *f, uint16_t v) {
    unsigned char b[2] = { v & 0xFF, (v >> 8) & 0xFF };
    fwrite(b, 1, 2, f);
}

int wav_read_mono_pcm16(const char *path, float **out_samples,
                         size_t *out_len, double *out_sample_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "wav_read: could not open '%s'\n", path);
        return -1;
    }

    unsigned char header[12];
    if (fread(header, 1, 12, f) != 12 ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "wav_read: '%s' is not a valid RIFF/WAVE file\n", path);
        fclose(f);
        return -1;
    }

    uint16_t num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    long data_offset = -1;
    uint32_t data_size = 0;

    /* Walk RIFF sub-chunks until we've found both fmt and data. */
    unsigned char chunk_hdr[8];
    while (fread(chunk_hdr, 1, 8, f) == 8) {
        uint32_t chunk_size = read_u32le(chunk_hdr + 4);
        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (chunk_size < 16 || fread(fmt, 1, 16, f) != 16) {
                fprintf(stderr, "wav_read: malformed fmt chunk\n");
                fclose(f);
                return -1;
            }
            uint16_t audio_format = read_u16le(fmt + 0);
            num_channels = read_u16le(fmt + 2);
            sample_rate = read_u32le(fmt + 4);
            bits_per_sample = read_u16le(fmt + 14);
            if (audio_format != 1) { /* 1 = PCM */
                fprintf(stderr, "wav_read: only uncompressed PCM is supported\n");
                fclose(f);
                return -1;
            }
            if (chunk_size > 16) fseek(f, (long)(chunk_size - 16), SEEK_CUR);
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            data_offset = ftell(f);
            data_size = chunk_size;
            fseek(f, (long)chunk_size, SEEK_CUR);
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR); /* skip unknown chunk */
        }
        if (chunk_size % 2 == 1) fseek(f, 1, SEEK_CUR); /* chunks are word-aligned */
    }

    if (data_offset < 0 || bits_per_sample != 16 || num_channels != 1) {
        fprintf(stderr, "wav_read: '%s' must be mono 16-bit PCM (got %u ch, %u-bit)\n",
                path, num_channels, bits_per_sample);
        fclose(f);
        return -1;
    }

    size_t n = data_size / 2;
    int16_t *raw = (int16_t *)malloc(n * sizeof(int16_t));
    float *samples = (float *)malloc(n * sizeof(float));
    if (!raw || !samples) {
        free(raw); free(samples);
        fclose(f);
        return -1;
    }

    fseek(f, data_offset, SEEK_SET);
    if (fread(raw, sizeof(int16_t), n, f) != n) {
        fprintf(stderr, "wav_read: truncated data chunk\n");
        free(raw); free(samples);
        fclose(f);
        return -1;
    }
    fclose(f);

    for (size_t i = 0; i < n; i++) {
        samples[i] = (float)raw[i] / 32768.0f;
    }
    free(raw);

    *out_samples = samples;
    *out_len = n;
    *out_sample_rate = (double)sample_rate;
    return 0;
}

int wav_write_mono_pcm16(const char *path, const float *samples, size_t n,
                          double sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "wav_write: could not open '%s' for writing\n", path);
        return -1;
    }

    uint32_t sr = (uint32_t)sample_rate;
    uint16_t bits_per_sample = 16;
    uint16_t num_channels = 1;
    uint32_t byte_rate = sr * num_channels * bits_per_sample / 8;
    uint16_t block_align = (uint16_t)(num_channels * bits_per_sample / 8);
    uint32_t data_size = (uint32_t)(n * sizeof(int16_t));

    fwrite("RIFF", 1, 4, f);
    write_u32le(f, 36 + data_size);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    write_u32le(f, 16);              /* fmt chunk size */
    write_u16le(f, 1);               /* PCM */
    write_u16le(f, num_channels);
    write_u32le(f, sr);
    write_u32le(f, byte_rate);
    write_u16le(f, block_align);
    write_u16le(f, bits_per_sample);

    fwrite("data", 1, 4, f);
    write_u32le(f, data_size);

    for (size_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767.0f);
        unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
        fwrite(b, 1, 2, f);
    }

    fclose(f);
    return 0;
}
