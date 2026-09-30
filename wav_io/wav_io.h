#ifndef DSP_WAV_IO_H
#define DSP_WAV_IO_H

#include <stddef.h>

/*
 * Minimal WAV file I/O: mono, 16-bit PCM only. Written from scratch
 * rather than pulling in libsndfile, since the format is simple enough
 * that hand-rolling it is both self-contained and instructive — this
 * is exactly the RIFF/WAVE header layout you'd read about in any
 * digital audio primer.
 */

/*
 * Reads a mono 16-bit PCM WAV file, converting samples to float in
 * [-1, 1]. On success, *out_samples is malloc'd (caller must free()
 * it), *out_len is the sample count, and *out_sample_rate is read from
 * the file header. Returns 0 on success, -1 on failure (missing file,
 * unsupported format, etc — a message is printed to stderr).
 */
int wav_read_mono_pcm16(const char *path, float **out_samples,
                         size_t *out_len, double *out_sample_rate);

/*
 * Writes `samples` (length n, float in [-1, 1], values outside that
 * range are clamped) as a mono 16-bit PCM WAV file at the given
 * sample rate. Returns 0 on success, -1 on failure.
 */
int wav_write_mono_pcm16(const char *path, const float *samples, size_t n,
                          double sample_rate);

#endif /* DSP_WAV_IO_H */
