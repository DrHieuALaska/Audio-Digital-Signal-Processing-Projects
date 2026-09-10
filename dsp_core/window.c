#include "window.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void window_generate(float *w, size_t n, window_type_t type) {
    if (n == 0) return;
    if (n == 1) { w[0] = 1.0f; return; }

    for (size_t i = 0; i < n; i++) {
        double x = (double)i / (double)(n - 1); /* 0..1 */
        switch (type) {
            case WINDOW_HANN:
                w[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * x)));
                break;
            case WINDOW_HAMMING:
                w[i] = (float)(0.54 - 0.46 * cos(2.0 * M_PI * x));
                break;
            case WINDOW_BLACKMAN:
                w[i] = (float)(0.42
                                - 0.5 * cos(2.0 * M_PI * x)
                                + 0.08 * cos(4.0 * M_PI * x));
                break;
            case WINDOW_RECTANGULAR:
            default:
                w[i] = 1.0f;
                break;
        }
    }
}

void window_generate_periodic(float *w, size_t n, window_type_t type) {
    if (n == 0) return;

    for (size_t i = 0; i < n; i++) {
        double x = (double)i / (double)n; /* note: n, not n-1 */
        switch (type) {
            case WINDOW_HANN:
                w[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * x)));
                break;
            case WINDOW_HAMMING:
                w[i] = (float)(0.54 - 0.46 * cos(2.0 * M_PI * x));
                break;
            case WINDOW_BLACKMAN:
                w[i] = (float)(0.42
                                - 0.5 * cos(2.0 * M_PI * x)
                                + 0.08 * cos(4.0 * M_PI * x));
                break;
            case WINDOW_RECTANGULAR:
            default:
                w[i] = 1.0f;
                break;
        }
    }
}

void window_apply_to_complex(const float *samples, const float *w,
                              cplx_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i].re = samples[i] * w[i];
        out[i].im = 0.0f;
    }
}
