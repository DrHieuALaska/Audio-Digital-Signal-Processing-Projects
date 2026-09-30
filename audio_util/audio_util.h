#ifndef DSP_AUDIO_UTIL_H
#define DSP_AUDIO_UTIL_H

#include <portaudio.h>

/*
 * Shared PortAudio setup used by every live-audio project (spectrum
 * analyzer, EQ, tuner). Exists to fix two real problems, not just
 * cosmetic ones:
 *
 * 1. On Linux (and especially WSL, whose PulseAudio bridge is
 *    incomplete), Pa_Initialize() enumerates every ALSA/JACK device
 *    name it knows about, and the underlying C libraries print a
 *    warning straight to stderr for each one that doesn't exist on
 *    your system. This is harmless but noisy, and — importantly —
 *    it's printed by libasound/libjack directly, bypassing our own
 *    error handling, so it looks alarming even when nothing failed.
 *    audio_initialize_quiet() suppresses exactly this, and only this
 *    (your own fprintf(stderr, ...) calls elsewhere are unaffected).
 *
 * 2. If no real input device exists (common in WSL without working
 *    mic passthrough, or a headless VM), PortAudio can still report a
 *    "default" input device that doesn't actually work, or none at
 *    all. Without checking, a program just opens a stream that never
 *    delivers real samples and sits there silently forever — exactly
 *    what "(listening...)" forever looks like. audio_find_input_device()
 *    validates a device actually has input channels before we trust it,
 *    and returns -1 (rather than a device index) so the caller can fail
 *    loudly and clearly instead of hanging.
 */

/* Wraps Pa_Initialize() with stderr temporarily silenced, to swallow
 * ALSA/JACK's device-enumeration warnings without touching any other
 * output. Returns the same PaError Pa_Initialize() would. */
PaError audio_initialize_quiet(void);

/* Returns a usable input device index (falls back to scanning all
 * devices if the "default" one has no input channels), or -1 if none
 * exists at all. Call after audio_initialize_quiet() succeeds. */
int audio_find_input_device(void);

/* Same idea for output (used by the duplex EQ). Returns -1 if none. */
int audio_find_output_device(void);

/* Prints a clear, actionable explanation (not ALSA jargon) for why no
 * input device was found, including WSL-specific guidance, to stderr. */
void audio_print_no_input_device_help(void);

#endif /* DSP_AUDIO_UTIL_H */
