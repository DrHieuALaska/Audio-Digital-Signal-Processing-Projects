#include "stft.h"
#include <stdlib.h>
#include <string.h>

int stft_init(stft_t *s, size_t fft_size, size_t hop_size, window_type_t wtype) {
    if (!fft_is_power_of_two(fft_size) || hop_size != fft_size / 2) {
        return -1; /* this module only supports 50% overlap */
    }

    s->fft_size = fft_size;
    s->hop_size = hop_size;

    s->window    = (float *)calloc(fft_size, sizeof(float));
    s->in_frame  = (float *)calloc(fft_size, sizeof(float));
    s->out_accum = (float *)calloc(fft_size, sizeof(float));
    s->spectrum  = (cplx_t *)calloc(fft_size, sizeof(cplx_t));

    if (!s->window || !s->in_frame || !s->out_accum || !s->spectrum) {
        stft_free(s);
        return -1;
    }

    window_generate_periodic(s->window, fft_size, wtype);
    return 0;
}

void stft_free(stft_t *s) {
    free(s->window);    s->window = NULL;
    free(s->in_frame);  s->in_frame = NULL;
    free(s->out_accum); s->out_accum = NULL;
    free(s->spectrum);  s->spectrum = NULL;
}

void stft_analyze(stft_t *s, const float *hop_in) {
    size_t n = s->fft_size, hop = s->hop_size;

    /* Slide the analysis frame left by one hop, append the new hop. */
    memmove(s->in_frame, s->in_frame + hop, (n - hop) * sizeof(float));
    memcpy(s->in_frame + (n - hop), hop_in, hop * sizeof(float));

    window_apply_to_complex(s->in_frame, s->window, s->spectrum, n);
    fft_forward(s->spectrum, n);
}

void stft_synthesize(stft_t *s, float *out_hop) {
    size_t n = s->fft_size, hop = s->hop_size;

    fft_inverse(s->spectrum, n); /* time-domain result now in spectrum[i].re */

    for (size_t i = 0; i < n; i++) {
        s->out_accum[i] += s->spectrum[i].re;
    }

    /* The front hop_size samples of the accumulator are now "done" —
     * no future frame will add anything more to them. */
    memcpy(out_hop, s->out_accum, hop * sizeof(float));

    /* Slide the accumulator left by one hop, zero-fill the freed tail
     * so the next frame's overlap-add starts from a clean slate. */
    memmove(s->out_accum, s->out_accum + hop, (n - hop) * sizeof(float));
    memset(s->out_accum + (n - hop), 0, hop * sizeof(float));
}
