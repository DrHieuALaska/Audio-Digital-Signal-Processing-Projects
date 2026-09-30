# Audio DSP Project

A series of audio DSP sub projects sharing one `dsp_core/` engine:

1. **Spectrum analyzer** — `main_spec_analyzer` 
2. **Equalizer / noise gate** — `main_equalizer` 
3. **Pitch detection** — `main_tuner`
4. **Auto-tune*** — `main_autotune`

## Why this structure

Everything in `dsp_core/` is written to be reused, unchanged, by sub projects:

| Module            | Provides                                   | Reused by |
|--------------------|---------------------------------------------|-----------|
| `dsp_core/fft.*`        | radix-2 iterative FFT/IFFT, magnitude, dB  | 1, 2, 3, 4 |
| `dsp_core/window.*`     | Hann/Hamming/Blackman windows (symmetric + periodic) | 2, 3, 4 |
| `dsp_core/ringbuffer.*` | lock-free SPSC buffer for real-time audio callbacks  | 1, 3 |
| `dsp_core/stft.*`       | fixed-hop analysis/synthesis engine (50% overlap) | 2, 3 , 4 |
| `dsp_core/pitch_detect.*` | phase-vocoder instantaneous-frequency pitch detector | 3, 4 |
| `dsp_core/pitch_shift.*`  | phase-vocoder time-stretch + resample pitch shifter | 4 |
| `dsp_core/wav_io.*`       | minimal mono 16-bit PCM WAV read/write | 2, 3, 4 |
| `dsp_core/audio_util.*`   | quiet PortAudio init + real input/output device validation | 1, 2, 3 |

## Automated correctness tests

Every layer is verified before anything is built on top of it:

| Test | Verifies | Result |
|------|----------|--------|
| `test_fft` | `fft_forward` matches a naive O(n²) DFT; `fft_inverse` round-trips | max error ~2.5e-6 |
| `test_stft` | overlap-add reconstructs the original signal when the spectrum is left unmodified (COLA correctness) | max error ~3.1e-6 |
| `test_eq_symmetry` | per-band EQ gain preserves conjugate symmetry, so IFFT output stays real | max imaginary leakage ~2.5e-6 |
| `test_pitch_detect` | detected frequency matches known synthetic tones (110–880 Hz) | within 0.01 cents |
| `test_pitch_shift` | shifted output's dominant frequency (measured independently, via a *separate* FFT peak-pick — not the phase vocoder itself) matches the expected shifted frequency | within 0.1% |
| `test_wav_io` | WAV write→read round-trip matches the original signal | within 16-bit quantization error |

Run all of them with `make test`.

## Build

Requires PortAudio development headers.

```bash
# Debian/Ubuntu
sudo apt-get install portaudio19-dev

# macOS
brew install portaudio
```

Then:

```bash
make          # builds ./spectrum_analyzer and ./main_eq
make test     # builds and runs all 3 correctness tests
```

## Run: Project 1 — Spectrum Analyzer

```bash
./main_spectrum_analyzer
```

You should see a live ASCII bar spectrum in your terminal reacting to
whatever your default microphone picks up (0–5 kHz range, tuned for
voice/guitar). Ctrl+C to quit.

`dsp_core/audio_util.c` fixes this two ways:

## Run: Feature 2 — Equalizer / Noise Gate

### 2.1 Realtime equalizer

```bash
./main_equalizer
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
./main_equalizer name.wav [--bass DB] [--mid DB] [--treble DB] \
                 [--gate] [--gate-threshold DB]
```

| args | default |
|-----|--------|
| `bass` | 0 dB|
| `mid` |  0 dB |
| `treble` |  0 dB |
| `gate` | off |
| `gate-threshold` | -55dB |

### 2.3 File equalizer
Takes ALL effect settings up front, as command-line arguments. Reads an existing audio file. Writes the processed audio straight to a WAV file.

```bash
./main_equalizer INPUT_name.wav OUTPUT_name.wav \
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

1. Every hop (512 samples), `stft_analyze()` windows and FFTs the current frame.
2. `apply_spectral_effects()` walks bins `0..N/2` and,
   per bin: picks a band gain based on that bin's frequency, and (if the gate is on) zeroes the bin when its magnitude in dB falls below the threshold. The **same** gain is applied to the mirror bin `N-k`, which is required to keep the spectrum conjugate-symmetric — skip that and the inverse FFT stops producing real audio. `test_eq_symmetry` exists specifically to catch a regression there.
3. `stft_synthesize()` inverse-FFTs and overlap-adds the result back into a continuous audio stream.


## Run: Project 3 — Pitch Detection

### `main_tuner` — real-time pitch detector (like a guitar tuner)

```bash
./main_tuner
```

Live note name, frequency, and cents-off-pitch, updated continuously from your mic. This is genuinely real-time — pitch *detection* alone doesn't need to change the audio's duration.

### How pitch detection works (`dsp_core/pitch_detect.c`)

An FFT magnitude spectrum alone only resolves frequency to one bin width (`sample_rate / fft_size` — about 21.5 Hz at our settings, far too coarse to tell a note is a few cents flat). The fix is tracking **phase**: a sinusoid sitting exactly at a bin's center frequency has a perfectly predictable phase advance from one hop to the next. Any
deviation from that predicted advance tells you exactly how far the true frequency sits from that bin's center — giving sub-bin accuracy. `test_pitch_detect` confirms this lands within a hundredth of a cent on clean synthetic tones.


## Run: Project 4 — Auto-tune
### `main_autotune` — offline pitch correction on a WAV file

```bash
./main_autotune input.wav output.wav [strength]
```

- `input.wav` must be mono, 16-bit PCM.
- `strength` (optional, default 1.0): 1.0 = snap fully to the nearest semitone, 0.5 = half correction (subtler, more natural), 0.0 = no correction.

This is an **offline batch tool, not a real-time effect** — and that's a deliberate scope decision, not a shortcut taken lightly. Real-time pitch *shifting* (as opposed to detection) has to solve a genuinely harder problem: the phase-vocoder step changes the sample count on the
fly (that's literally how it shifts pitch), which means a real-time version needs a variable-rate streaming architecture — a substantial project of its own. Building it offline first, and getting the core algorithm provably correct via the tests below, is the right order of
operations before ever attempting that.


### How pitch shifting works (`dsp_core/pitch_shift.c`)

The input signal is processed using overlapping STFT frames:

1. **Windowing and FFT**
   - Each frame is multiplied by a Hann window and transformed into the frequency domain using the FFT.
   - For each frequency bin, the magnitude and phase are extracted.

2. **Instantaneous Frequency Estimation**
   - The phase difference between consecutive frames is used to estimate the actual frequency of each bin.
   - This provides better frequency estimation than simply using the fixed FFT-bin frequency.

3. **Spectral Frequency Mapping**
   - A pitch-shift ratio is computed from the desired number of semitones:

     $$
     r = 2^{s/12}
     $$

     where $s$ is the pitch shift in semitones.
   - For an output frequency bin $k$, the corresponding source frequency is approximately:

     $$
     k_{\text{source}} = \frac{k}{r}
     $$

   - The magnitude is interpolated from the source spectrum, while the estimated frequency is multiplied by $r$.

4. **Phase Accumulation**
   - The modified frequencies are used to update a synthesis phase accumulator.
   - Maintaining the accumulated phase across frames provides phase continuity and reduces discontinuities between frames.

5. **IFFT and Overlap-Add**
   - The modified spectrum is transformed back to the time domain using the IFFT.
   - A synthesis Hann window is applied.
   - Consecutive frames are overlap-added using a 75% overlap.
   - The analysis and synthesis windows produce a squared-window overlap pattern, which is compensated by a normalization scale.


## Known limitations (intentional, for a learning project)

 **Mono, 16-bit PCM WAV only** — `wav_io.c` was written from scratch to stay dependency-free and instructive, not to be a general-purpose audio file library.

