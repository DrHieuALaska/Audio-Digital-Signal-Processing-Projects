# Audio DSP (Digital Signal Processing) Projects

A series of audio DSP projects sharing one `dsp_core/` engine:

1. **Spectrum analyzer** — `spectrum_analyzer`
2. **Equalizer / noise gate** — `file_equalizer` `record_equalizer` `realtime_equalizer`


## Why this structure

Everything in `dsp_core/` is written to be reused, unchanged, by project's features.

| Module            | Provides                                   | Reused by |
|--------------------|---------------------------------------------|-----------|
| `dsp_core/fft.*`        | radix-2 iterative FFT/IFFT, magnitude, dB  | Equalizer |
| `dsp_core/window.*`     | Hann/Hamming/Blackman windows (symmetric + periodic) | spectrum_analyzer, Equalizer |
| `dsp_core/ringbuffer.*` | lock-free SPSC buffer for audio callbacks  | spectrum_analyzer |
| `dsp_core/stft.*`       | analysis/synthesis engine with overlap-add | Equalizer |

## Automated correctness tests

Every layer is verified before anything is built on top of it:

| Test | Verifies | Result |
|------|----------|--------|
| `test_fft` | `fft_forward` matches a naive O(n²) DFT; `fft_inverse` round-trips | max error ~2.5e-6 |
| `test_stft` | overlap-add reconstructs the original signal when the spectrum is left unmodified (COLA correctness) | max error ~3.1e-6 |
| `test_eq_symmetry` | per-band EQ gain preserves conjugate symmetry, so IFFT output stays real | max imaginary leakage ~2.5e-6 |

Run all three with `make test`.

## Build

Requires PortAudio development headers.

```bash
# Debian/Ubuntu
sudo apt install portaudio19-dev
sudo apt install libsndfile1  

# macOS
brew install portaudio
brew install libsndfile
```

Then:

```bash
make          # builds
make test     # builds and runs all 3 correctness tests
```

## Run: Feature 1 — Spectrum Analyzer

```bash
./spectrum_analyzer
```

You should see a live ASCII bar spectrum in your terminal reacting to
whatever your default microphone picks up (0–5 kHz range, tuned for
voice/guitar). Ctrl+C to quit.


## Run: Feature 2 — Equalizer / Noise Gate

### 2.1 Realtime equalizer

```bash
./realtime_equalizer
```

**Use headphones.** This opens a duplex stream (mic in, speaker out) —
without headphones the speaker output will feed back into the mic.

Live keyboard controls (no restart needed):

| Key | Effect |
|-----|--------|
| `w` / `s` | bass gain up / down (< 250 Hz) |
| `e` / `d` | mid gain up / down (250 Hz – 4 kHz) |
| `r` / `f` | treble gain up / down (> 4 kHz) |
| `g` | toggle noise gate on/off |
| `t` / `y` | gate threshold down / up (more/less aggressive) |
| `q` | quit |

### 2.2 Record equalizer
Takes ALL effect settings up front, as command-line arguments. Records from the microphone for as long as the user wants (Ctrl + C to stop). Writes the processed audio straight to a WAV file.

```bash
./record_equalizer --output [name.wav] \ 
                  [--bass DB] [--mid DB] [--treble DB] \
                  [--gate] [--gate-threshold DB] [--sample-rate HZ] \
                  [--max-seconds N]
```

| args | default |
|-----|--------|
| `bass` | 0 dB|
| `mid` |  0 dB |
| `treble` |  0 dB |
| `gate` | off |
| `gate-threshold` | -55dB |
| `sample-rate` | 44100 Hz |
| `max-seconds` | Null (stop manually) |

### 2.3 File equalizer
Takes ALL effect settings up front, as command-line arguments. Reads an existing audio file. Writes the processed audio straight to a WAV file.

```bash
./file_equalizer [INPUT_name.wav] [OUTPUT_name.wav] \ 
                 [--bass DB] [--mid DB] [--treble DB] \
                 [--gate] [--gate-threshold DB]
```

| args | default |
|-----|--------|
| `bass` | 0 dB|
| `mid` |  0 dB |
| `treble` |  0 dB |
| `gate` | off |
| `gate-threshold` | -55dB |

### How the EQ/gate actually works

1. Every hop (512 samples), `stft_analyze()` windows and FFTs the
   current frame.
2. `apply_spectral_effects()` walks bins `0..N/2` and,
   per bin: picks a band gain based on that bin's frequency, and
   (if the gate is on) zeroes the bin when its magnitude in dB falls
   below the threshold. The **same** gain is applied to the mirror
   bin `N-k`, which is required to keep the spectrum conjugate-
   symmetric — skip that and the inverse FFT stops producing real
   audio. `test_eq_symmetry` exists specifically to catch a regression
   there.
3. `stft_synthesize()` inverse-FFTs and overlap-adds the result back
   into a continuous audio stream.

### Known limitations

- **Hard gate, not soft-knee.** The noise gate is an on/off cliff per
  bin, which can cause "musical noise" (a warbly artifact) on complex
  material. A smoother gain curve as a function of how far below
  threshold a bin is would fix this.
- **FFT runs inside the real-time audio callback.** Fine at this size
  on a desktop CPU, but not how you'd want a shipped real-time audio
  engine built — see the comment at the top of `main_realtime_equalizer.c` for the
  production alternative (decouple via the ring buffer pattern from
  project 1).
