#include "audio_util.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

PaError audio_initialize_quiet(void) {
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    PaError err = Pa_Initialize();

    fflush(stderr);
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    return err;
}

static int find_device_with_channels(int want_input) {
    int count = Pa_GetDeviceCount();
    if (count <= 0) return -1;

    int preferred = want_input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
    if (preferred != paNoDevice) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(preferred);
        int channels = want_input ? info->maxInputChannels : info->maxOutputChannels;
        if (channels > 0) {
            return preferred;
        }
    }

    /* Default device missing or unusable: scan everything else. This
     * covers e.g. a system whose "default" is a disconnected HDMI
     * output, or a WSL setup where PortAudio's notion of "default"
     * doesn't line up with the one real device that works. */
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        int channels = want_input ? info->maxInputChannels : info->maxOutputChannels;
        if (channels > 0) {
            return i;
        }
    }
    return -1;
}

int audio_find_input_device(void) {
    return find_device_with_channels(1);
}

int audio_find_output_device(void) {
    return find_device_with_channels(0);
}

void audio_print_no_input_device_help(void) {
    fprintf(stderr,
        "\nNo usable audio input device was found.\n\n"
        "This means PortAudio could not find any device with input\n"
        "channels on this system -- not a bug in this program, but a\n"
        "genuine lack of an available microphone from the OS's point of\n"
        "view. To fix it:\n\n"
        "  On native Linux:\n"
        "    - Check a mic is actually connected: `arecord -l`\n"
        "    - Check PulseAudio/PipeWire sees it: `pactl list sources short`\n\n"
        "  On WSL2 (Windows 11):\n"
        "    - WSLg's audio bridge supports playback more reliably than\n"
        "      microphone capture; this is a known rough edge, not\n"
        "      something this program can work around.\n"
        "    - Check Windows: Settings > Privacy & security > Microphone\n"
        "      > \"Let desktop apps access your microphone\" must be ON.\n"
        "    - Install `pulseaudio-utils` in WSL and test capture directly:\n"
        "        parecord --channels=1 --rate=44100 /tmp/test.wav\n"
        "        paplay /tmp/test.wav   # do you hear your own voice back?\n"
        "    - If that's silent, mic passthrough isn't working in your\n"
        "      WSL setup right now -- this is an environment issue, not\n"
        "      an issue with the code.\n\n"
        "  Regardless of platform: if you have a WAV recording instead of\n"
        "  a live mic, every project here that reads live audio can be\n"
        "  pointed at a file instead -- see each program's --help / usage.\n\n");
}
