CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c11
TARGET = ixgIPStream

# FFmpeg pkg-config dependencies
FFMPEG_LIBS = libavformat libavcodec libavutil libswscale
FFMPEG_CFLAGS = $(shell pkg-config --cflags $(FFMPEG_LIBS))
FFMPEG_LDFLAGS = $(shell pkg-config --libs $(FFMPEG_LIBS)) -lpthread -lm

SRCS = src/main.c src/pipeline.c src/utils.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(FFMPEG_LDFLAGS) -o $(TARGET)
	@echo "Build successful: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) -Isrc -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned build artifacts."

test: $(TARGET)
	@echo "Running local pipeline test..."
	./scripts/simulate_multicast.sh --test

.PHONY: all clean test
