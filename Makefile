CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -Idsp_core
LDFLAGS = -lportaudio -lpthread -lm -lsndfile

CORE_SRCS = dsp_core/fft.c dsp_core/window.c dsp_core/ringbuffer.c dsp_core/stft.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

ANALYZER_TARGET = spectrum_analyzer
ANALYZER_OBJS = main_spec_analyzer.o $(CORE_OBJS)

REALTIME_EQ_TARGET = realtime_equalizer
REALTIME_EQ_OBJS = main_realtime_equalizer.o $(CORE_OBJS)

RECORD_EQ_TARGET = record_equalizer
RECORD_EQ_OBJS = main_record_equalizer.o $(CORE_OBJS)

FILE_EQ_TARGET = file_equalizer
FILE_EQ_OBJS = main_file_equalizer.o $(CORE_OBJS)

TEST_FFT_TARGET = test_fft
TEST_FFT_OBJS = test_fft.o dsp_core/fft.o

TEST_STFT_TARGET = test_stft
TEST_STFT_OBJS = test_stft.o dsp_core/stft.o dsp_core/fft.o dsp_core/window.o

TEST_EQ_SYM_TARGET = test_eq_symmetry
TEST_EQ_SYM_OBJS = test_eq_symmetry.o dsp_core/fft.o

.PHONY: all clean test run-test-fft run-test-stft run-test-eq-symmetry

all: $(ANALYZER_TARGET) $(REALTIME_EQ_TARGET) $(RECORD_EQ_TARGET) $(FILE_EQ_TARGET)

$(ANALYZER_TARGET): $(ANALYZER_OBJS)
	$(CC) $(ANALYZER_OBJS) -o $@ $(LDFLAGS)

$(REALTIME_EQ_TARGET): $(REALTIME_EQ_OBJS)
	$(CC) $(REALTIME_EQ_OBJS) -o $@ $(LDFLAGS)

$(RECORD_EQ_TARGET): $(RECORD_EQ_OBJS)
	$(CC) $(RECORD_EQ_OBJS) -o $@ $(LDFLAGS)

$(FILE_EQ_TARGET): $(FILE_EQ_OBJS)
	$(CC) $(FILE_EQ_OBJS) -o $@ $(LDFLAGS)

test: run-test-fft run-test-stft run-test-eq-symmetry

run-test-fft: $(TEST_FFT_TARGET)
	./$(TEST_FFT_TARGET)

run-test-stft: $(TEST_STFT_TARGET)
	./$(TEST_STFT_TARGET)

run-test-eq-symmetry: $(TEST_EQ_SYM_TARGET)
	./$(TEST_EQ_SYM_TARGET)

$(TEST_FFT_TARGET): $(TEST_FFT_OBJS)
	$(CC) $(TEST_FFT_OBJS) -o $@ -lm

$(TEST_STFT_TARGET): $(TEST_STFT_OBJS)
	$(CC) $(TEST_STFT_OBJS) -o $@ -lm

$(TEST_EQ_SYM_TARGET): $(TEST_EQ_SYM_OBJS)
	$(CC) $(TEST_EQ_SYM_OBJS) -o $@ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o dsp_core/*.o $(ANALYZER_TARGET) $(REALTIME_EQ_TARGET) $(RECORD_EQ_TARGET) $(FILE_EQ_TARGET) $(TEST_FFT_TARGET) $(TEST_STFT_TARGET) $(TEST_EQ_SYM_TARGET)
