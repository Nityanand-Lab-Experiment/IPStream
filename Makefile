CC ?= gcc
CXX ?= g++
CFLAGS ?= -Wall -Wextra -O2 -std=c11
CXXFLAGS ?= -Wall -Wextra -O2 -std=c++11 -Wno-multichar
TARGET = ixgIPStream

# FFmpeg & SRT pkg-config dependencies
FFMPEG_LIBS = libavformat libavcodec libavutil libswscale libswresample
FFMPEG_CFLAGS = $(shell pkg-config --cflags $(FFMPEG_LIBS))
FFMPEG_LDFLAGS = $(shell pkg-config --libs $(FFMPEG_LIBS)) -lpthread -lm -ldl -lstdc++

SRT_CFLAGS = $(shell pkg-config --cflags srt 2>/dev/null)
SRT_LDFLAGS = $(shell pkg-config --libs srt 2>/dev/null || echo "-lsrt")
FFMPEG_CFLAGS += $(SRT_CFLAGS)
FFMPEG_LDFLAGS += $(SRT_LDFLAGS)

INCLUDES = -Isrc -IBMSDK_14_2/Linux/include

SRCS_C = src/main.c src/pipeline.c src/utils.c src/logger.c src/rtmp.c src/hls.c src/srt.c src/transcode.c
SRCS_CPP = src/sdi_decklink.cpp BMSDK_14_2/Linux/include/DeckLinkAPIDispatch.cpp
OBJS_C = $(SRCS_C:.c=.o)
OBJS_CPP = src/sdi_decklink.o BMSDK_14_2/Linux/include/DeckLinkAPIDispatch.o
OBJS = $(OBJS_C) $(OBJS_CPP)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(FFMPEG_LDFLAGS) -o $(TARGET)
	@echo "Build successful: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(FFMPEG_CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned build artifacts."

test: $(TARGET)
	@echo "Running local pipeline test..."
	./scripts/simulate_multicast.sh --test

.PHONY: all clean test
