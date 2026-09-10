#include "ringbuffer.h"
#include <stdlib.h>

int ringbuffer_init(ringbuffer_t *rb, size_t capacity_pow2) {
    if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
        return -1; /* must be a power of two */
    }
    rb->buffer = (float *)malloc(capacity_pow2 * sizeof(float));
    if (!rb->buffer) {
        return -1;
    }
    rb->capacity = capacity_pow2;
    rb->mask = capacity_pow2 - 1;
    atomic_store(&rb->write_idx, 0);
    atomic_store(&rb->read_idx, 0);
    return 0;
}

void ringbuffer_free(ringbuffer_t *rb) {
    free(rb->buffer);
    rb->buffer = NULL;
    rb->capacity = 0;
    rb->mask = 0;
}

size_t ringbuffer_available(const ringbuffer_t *rb) {
    size_t w = atomic_load_explicit((atomic_size_t *)&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit((atomic_size_t *)&rb->read_idx, memory_order_acquire);
    return w - r; /* unsigned wraparound is fine as long as capacity <= SIZE_MAX/2 */
}

size_t ringbuffer_write(ringbuffer_t *rb, const float *samples, size_t n) {
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    size_t free_space = rb->capacity - (w - r);

    size_t to_write = n < free_space ? n : free_space;
    for (size_t i = 0; i < to_write; i++) {
        rb->buffer[(w + i) & rb->mask] = samples[i];
    }

    atomic_store_explicit(&rb->write_idx, w + to_write, memory_order_release);
    return to_write;
}

size_t ringbuffer_read(ringbuffer_t *rb, float *out, size_t n) {
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    size_t available = w - r;

    size_t to_read = n < available ? n : available;
    for (size_t i = 0; i < to_read; i++) {
        out[i] = rb->buffer[(r + i) & rb->mask];
    }

    atomic_store_explicit(&rb->read_idx, r + to_read, memory_order_release);
    return to_read;
}
