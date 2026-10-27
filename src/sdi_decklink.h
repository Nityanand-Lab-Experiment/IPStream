#ifndef SDI_DECKLINK_H
#define SDI_DECKLINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/frame.h>
#include <libavutil/rational.h>

typedef struct SdiInputContext SdiInputContext;
typedef struct SdiOutputContext SdiOutputContext;

typedef struct SdiDeviceInfo {
    int index;                     /* 1-based device index */
    char name[128];                /* e.g. "DeckLink Duo (1)" */
    char model[128];
    bool supports_input;
    bool supports_output;
    bool has_input_signal;
    char detected_mode_name[64];   /* e.g. "1080p59.94" or "No Signal" */
    int detected_width;
    int detected_height;
    double detected_fps;
} SdiDeviceInfo;

/**
 * Scan all installed Blackmagic DeckLink devices, print an interactive summary
 * to stdout/logs, and optionally populate the devices array.
 *
 * @param devices Pointer to array of SdiDeviceInfo (can be NULL)
 * @param max_devices Maximum items to populate in devices array
 * @return Number of Blackmagic devices found.
 */
int sdi_list_devices(SdiDeviceInfo *devices, int max_devices);

/**
 * Open a DeckLink SDI input port for low-latency ingest.
 *
 * @param device_id Device name (e.g. "DeckLink Duo (1)") or "1", "2", etc.
 * @param mode_name "auto" for auto-signal detection, or explicit e.g. "1080p5994", "1080p60", "1080i50", "720p60"
 * @param audio_channels Number of audio channels (e.g. 2 or 8)
 * @return Opaque SdiInputContext pointer, or NULL on error.
 */
SdiInputContext *sdi_input_open(const char *device_id, const char *mode_name, int audio_channels);

typedef enum SdiSyncType {
    SDI_SYNC_HARDWARE = 0, /* Lock timestamps to Blackmagic DeckLink hardware clock */
    SDI_SYNC_SELF = 1      /* Pure self-generated monotonic frame/sample sequence timestamps */
} SdiSyncType;

/**
 * Configure synchronization mode (hardware clock vs self-generated monotonic sequence).
 */
void sdi_input_set_sync_type(SdiInputContext *ctx, SdiSyncType sync_type);

/**
 * Enable or disable generating SMPTE 75% colorbars when no SDI carrier / input signal is present.
 */
void sdi_input_set_test_bars(SdiInputContext *ctx, bool enable);

/**
 * Start video and audio capture streams on the SDI hardware.
 */
int sdi_input_start(SdiInputContext *ctx);

/**
 * Read the next captured raw video frame (blocking with timeout).
 * Populates an allocated AVFrame (format AV_PIX_FMT_UYVY422).
 * Caller is responsible for freeing with av_frame_free(out_frame).
 *
 * @param ctx SdiInputContext pointer
 * @param out_frame Pointer to store newly allocated/referenced AVFrame
 * @param timeout_ms Timeout in milliseconds (e.g. 100)
 * @return 0 on success, AVERROR(EAGAIN) on timeout, negative on fatal error.
 */
int sdi_input_read_video_frame(SdiInputContext *ctx, AVFrame **out_frame, int timeout_ms);

/**
 * Read the next captured raw audio frame (blocking with timeout).
 * Format is AV_SAMPLE_FMT_S16, 48000Hz.
 */
int sdi_input_read_audio_frame(SdiInputContext *ctx, AVFrame **out_frame, int timeout_ms);

/**
 * Query current input format resolution and framerate.
 */
int sdi_input_get_format(SdiInputContext *ctx, int *width, int *height, AVRational *framerate);

/**
 * Wait up to timeout_ms for hardware carrier lock and format auto-detection.
 * @return 0 on signal locked, -1 on timeout.
 */
int sdi_input_wait_for_format(SdiInputContext *ctx, int timeout_ms);

/**
 * Flush all buffered audio and video queues and reset zero-origin timestamps.
 * Call right before entering the streaming loop or after mode changes to ensure zero initial drift.
 */
void sdi_input_flush(SdiInputContext *ctx);

/**
 * Stop capture and release all DeckLink input resources.
 */
void sdi_input_close(SdiInputContext **ctx);

/**
 * Open a DeckLink SDI output port for low-latency playout.
 *
 * @param device_id Device name (e.g. "DeckLink Duo (2)") or "2"
 * @param width Target display width (e.g. 1920)
 * @param height Target display height (e.g. 1080)
 * @param framerate Target display framerate (e.g. 60/1 or 60000/1001)
 * @param audio_channels Audio channels (e.g. 2)
 * @return Opaque SdiOutputContext pointer, or NULL on error.
 */
SdiOutputContext *sdi_output_open(const char *device_id, int width, int height,
                                  AVRational framerate, int audio_channels);

/**
 * Display a video frame over SDI with zero delay.
 * Accepts AV_PIX_FMT_UYVY422 natively or AV_PIX_FMT_YUV420P (converted internally via swscale).
 */
int sdi_output_write_video_frame(SdiOutputContext *ctx, const AVFrame *frame);

/**
 * Playout audio samples through SDI embedded audio.
 */
int sdi_output_write_audio_frame(SdiOutputContext *ctx, const AVFrame *frame);

/**
 * Stop and release SDI playout resources.
 */
void sdi_output_close(SdiOutputContext **ctx);

#ifdef __cplusplus
}
#endif

#endif /* SDI_DECKLINK_H */
