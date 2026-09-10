#ifndef DSP_RINGBUFFER_H
#define DSP_RINGBUFFER_H

#include <stddef.h>
#include <stdatomic.h>

/*
 * Single-producer / single-consumer lock-free ring buffer of floats.
 *
 * Designed so the PRODUCER side can be called safely from a real-time
 * audio callback (or, later, an embedded ISR): it never blocks, never
 * allocates, and never takes a mutex. Only one thread may write and
 * only one thread may read.
 */
typedef struct {
    float *buffer;
    size_t capacity;      /* must be a power of two */
    size_t mask;          /* capacity - 1, for fast wraparound */
    atomic_size_t write_idx;
    atomic_size_t read_idx;
} ringbuffer_t;

/* capacity_pow2 must be a power of two. Returns 0 on success, -1 on failure. */
int ringbuffer_init(ringbuffer_t *rb, size_t capacity_pow2);
void ringbuffer_free(ringbuffer_t *rb);

/*
 * Producer side. Writes up to n samples; returns the number actually
 * written (less than n if the buffer is full — the caller decides
 * whether to drop samples, which is usually correct for real-time audio
 * rather than blocking and causing an underrun elsewhere).
 */
size_t ringbuffer_write(ringbuffer_t *rb, const float *samples, size_t n);

/* Consumer side. Reads up to n samples; returns the number actually read. */
size_t ringbuffer_read(ringbuffer_t *rb, float *out, size_t n);

/* Number of samples currently available to read. Safe from either thread. */
size_t ringbuffer_available(const ringbuffer_t *rb);

#endif /* DSP_RINGBUFFER_H */
