CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -Idsp_core
LDFLAGS = -lportaudio -lpthread -lm

CORE_SRCS = dsp_core/fft.c dsp_core/window.c dsp_core/ringbuffer.c dsp_core/stft.c dsp_core/pitch_detect.c dsp_core/pitch_shift.c wav_io/wav_io.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

# audio_util.c wraps PortAudio calls, so it's only linked into targets
# that actually use a live audio device -- NOT main_autotune, which is
# a pure offline WAV-in/WAV-out tool with no PortAudio dependency at all.
AUDIO_UTIL_OBJ = audio_util/audio_util.o

ANALYZER_TARGET = main_spectrum_analyzer
ANALYZER_OBJS = main_spec_analyzer.o $(CORE_OBJS) $(AUDIO_UTIL_OBJ)

EQUALIZER_TARGET = main_equalizer
EQUALIZER_OBJS = main_equalizer.o $(CORE_OBJS) $(AUDIO_UTIL_OBJ)

TUNER_TARGET = main_tuner
TUNER_OBJS = main_tuner.o $(CORE_OBJS) $(AUDIO_UTIL_OBJ)

AUTOTUNE_TARGET = main_autotune
AUTOTUNE_OBJS = main_autotune.o $(CORE_OBJS)



TEST_FFT_TARGET = test_fft
TEST_FFT_OBJS = test_fft.o dsp_core/fft.o

TEST_STFT_TARGET = test_stft
TEST_STFT_OBJS = test_stft.o dsp_core/stft.o dsp_core/fft.o dsp_core/window.o

TEST_EQ_SYM_TARGET = test_eq_symmetry
TEST_EQ_SYM_OBJS = test_eq_symmetry.o dsp_core/fft.o

TEST_PITCH_TARGET = test_pitch_detect
TEST_PITCH_OBJS = test_pitch_detect.o dsp_core/pitch_detect.o dsp_core/stft.o dsp_core/fft.o dsp_core/window.o

TEST_SHIFT_TARGET = test_pitch_shift
TEST_SHIFT_OBJS = test_pitch_shift.o dsp_core/pitch_shift.o dsp_core/fft.o dsp_core/window.o

TEST_WAV_TARGET = test_wav_io
TEST_WAV_OBJS = test_wav_io.o dsp_core/wav_io.o

.PHONY: all clean test run-test-fft run-test-stft run-test-eq-symmetry run-test-pitch-detect run-test-pitch-shift run-test-wav-io

all: $(ANALYZER_TARGET) $(EQUALIZER_TARGET) $(TUNER_TARGET) $(AUTOTUNE_TARGET)

$(ANALYZER_TARGET): $(ANALYZER_OBJS)
	$(CC) $(ANALYZER_OBJS) -o $@ $(LDFLAGS)

$(EQUALIZER_TARGET): $(EQUALIZER_OBJS)
	$(CC) $(EQUALIZER_OBJS) -o $@ $(LDFLAGS)

$(TUNER_TARGET): $(TUNER_OBJS)
	$(CC) $(TUNER_OBJS) -o $@ $(LDFLAGS)

$(AUTOTUNE_TARGET): $(AUTOTUNE_OBJS)
	$(CC) $(AUTOTUNE_OBJS) -o $@ -lm

test: run-test-fft run-test-stft run-test-eq-symmetry run-test-pitch-detect run-test-pitch-shift run-test-wav-io

run-test-fft: $(TEST_FFT_TARGET)
	./$(TEST_FFT_TARGET)

run-test-stft: $(TEST_STFT_TARGET)
	./$(TEST_STFT_TARGET)

run-test-eq-symmetry: $(TEST_EQ_SYM_TARGET)
	./$(TEST_EQ_SYM_TARGET)

run-test-pitch-detect: $(TEST_PITCH_TARGET)
	./$(TEST_PITCH_TARGET)

run-test-pitch-shift: $(TEST_SHIFT_TARGET)
	./$(TEST_SHIFT_TARGET)

run-test-wav-io: $(TEST_WAV_TARGET)
	./$(TEST_WAV_TARGET)


$(TEST_FFT_TARGET): $(TEST_FFT_OBJS)
	$(CC) $(TEST_FFT_OBJS) -o $@ -lm

$(TEST_STFT_TARGET): $(TEST_STFT_OBJS)
	$(CC) $(TEST_STFT_OBJS) -o $@ -lm

$(TEST_EQ_SYM_TARGET): $(TEST_EQ_SYM_OBJS)
	$(CC) $(TEST_EQ_SYM_OBJS) -o $@ -lm

$(TEST_PITCH_TARGET): $(TEST_PITCH_OBJS)
	$(CC) $(TEST_PITCH_OBJS) -o $@ -lm

$(TEST_SHIFT_TARGET): $(TEST_SHIFT_OBJS)
	$(CC) $(TEST_SHIFT_OBJS) -o $@ -lm

$(TEST_WAV_TARGET): $(TEST_WAV_OBJS)
	$(CC) $(TEST_WAV_OBJS) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o dsp_core/*.o audio_util/*.o wav_io/*.o $(ANALYZER_TARGET) $(EQUALIZER_TARGET) $(TUNER_TARGET) $(AUTOTUNE_TARGET) $(TEST_FFT_TARGET) $(TEST_STFT_TARGET) $(TEST_EQ_SYM_TARGET) $(TEST_PITCH_TARGET) $(TEST_SHIFT_TARGET) $(TEST_WAV_TARGET)
