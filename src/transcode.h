#ifndef TRANSCODE_H
#define TRANSCODE_H

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRANSCODE_CODEC_COPY = 0,
    TRANSCODE_CODEC_H264,       /* libx264 software */
    TRANSCODE_CODEC_HEVC,       /* libx265 software */
    TRANSCODE_CODEC_H264_NVENC, /* NVIDIA NVENC H.264 */
    TRANSCODE_CODEC_HEVC_NVENC  /* NVIDIA NVENC HEVC/H.265 */
} TranscodeVideoCodec;

typedef enum {
    TRANSCODE_RC_CBR = 0,
    TRANSCODE_RC_VBR
} TranscodeRateControl;

typedef struct TranscodeConfig {
    TranscodeVideoCodec vcodec;
    char vcodec_name[32];       /* e.g. "copy", "h264", "hevc", "h264_nvenc", "hevc_nvenc" */
    TranscodeRateControl rc;    /* Rate control mode: CBR or VBR */
    char rc_name[16];           /* "cbr" or "vbr" */
    int64_t vbitrate;           /* Target video bitrate in bps (e.g. 4000000 = 4 Mbps) */
    int64_t maxrate;            /* Maximum video bitrate in bps (for VBR, default: 1.5x vbitrate) */
    int64_t bufsize;            /* VBV buffer size in bytes (e.g. 2000000 = 2 MB) */
    int gop_size;               /* GOP / Keyframe interval in frames (e.g. 30 or 60) */
    int b_frames;               /* Max B-frames (0 for ultra-low latency) */
    char preset[32];            /* e.g. "ultrafast", "superfast", "veryfast", "p1", "p2" */
    char tune[32];              /* e.g. "zerolatency", "ll" */
    bool intra_refresh;         /* Intra-refresh to avoid I-frame packet bursts */
    int width;                  /* Target width (0 = maintain source) */
    int height;                 /* Target height (0 = maintain source) */
    AVRational framerate;       /* Target framerate ({0, 0} = maintain source) */
    bool low_delay;             /* Enforce AV_CODEC_FLAG_LOW_DELAY and slice threading */
} TranscodeConfig;

typedef struct TranscodeEngine TranscodeEngine;

/**
 * Parse string codec name into TranscodeVideoCodec enum.
 */
TranscodeVideoCodec transcode_parse_codec_name(const char *name);

/**
 * Returns human-readable name of video codec enum.
 */
const char *transcode_get_codec_name(TranscodeVideoCodec codec);

/**
 * Parse string rate control name ("cbr" or "vbr").
 */
TranscodeRateControl transcode_parse_rc_name(const char *name);

/**
 * Returns human-readable name of rate control enum.
 */
const char *transcode_get_rc_name(TranscodeRateControl rc);

/**
 * Initialize low-latency transcode engine.
 *
 * @param cfg TranscodeConfig settings
 * @param in_codecpar Source codec parameters (from input demuxer, or synthetic for SDI)
 * @param in_time_base Source timebase
 * @return Allocated TranscodeEngine pointer, or NULL on error.
 */
TranscodeEngine *transcode_engine_init(const TranscodeConfig *cfg,
                                       const AVCodecParameters *in_codecpar,
                                       AVRational in_time_base);

/**
 * Populate output AVCodecParameters from initialized encoder.
 */
int transcode_engine_get_codecpar(TranscodeEngine *te, AVCodecParameters *out_codecpar);

/**
 * Get the timebase of the active encoder.
 */
AVRational transcode_engine_get_timebase(TranscodeEngine *te);

/**
 * Submit an incoming compressed packet to the low-delay decoder.
 * Pass NULL to flush decoder.
 */
int transcode_engine_send_packet(TranscodeEngine *te, const AVPacket *in_pkt);

/**
 * Retrieve the next decoded raw AVFrame.
 * Caller provides allocated destination frame.
 * @return 0 on success, AVERROR(EAGAIN) if more packets needed, AVERROR_EOF at end, < 0 on error.
 */
int transcode_engine_receive_decoded_frame(TranscodeEngine *te, AVFrame *out_frame);

/**
 * Submit an uncompressed raw AVFrame (from decoder or native SDI) to the encoder.
 * Handles automatic color conversion (e.g. UYVY422 -> YUV420P) and scaling.
 * Pass NULL to flush encoder.
 */
int transcode_engine_send_frame(TranscodeEngine *te, const AVFrame *raw_frame);

/**
 * Retrieve the next encoded compressed AVPacket.
 * Caller provides allocated destination packet.
 * @return 0 on success, AVERROR(EAGAIN) if more frames needed, AVERROR_EOF at end, < 0 on error.
 */
int transcode_engine_receive_packet(TranscodeEngine *te, AVPacket *out_pkt);

/**
 * Get internal transcode latency metrics (in microseconds).
 * Returns processing time of last encoded frame ($T_{encode\_done} - T_{frame\_in}$).
 */
int64_t transcode_engine_get_last_latency_us(TranscodeEngine *te);

/**
 * Free all decoder, scaler, and encoder resources.
 */
void transcode_engine_free(TranscodeEngine **te);

/* =========================================================================
 * Audio Transcoding Engine API
 * ========================================================================= */

typedef enum {
    TRANSCODE_AUDIO_COPY = 0,
    TRANSCODE_AUDIO_AAC,
    TRANSCODE_AUDIO_OPUS,
    TRANSCODE_AUDIO_MP2
} TranscodeAudioCodec;

typedef struct AudioTranscodeConfig {
    TranscodeAudioCodec acodec;
    char acodec_name[32];       /* "copy", "aac", "opus", "mp2" */
    int abitrate;               /* Target audio bitrate (bps), e.g. 192000 */
    int sample_rate;            /* Target sample rate (Hz), e.g. 48000 */
    int channels;               /* Target channels, e.g. 2 */
} AudioTranscodeConfig;

typedef struct AudioTranscodeEngine AudioTranscodeEngine;

/**
 * Parse string codec name into TranscodeAudioCodec enum.
 */
TranscodeAudioCodec transcode_parse_audio_codec_name(const char *name);

/**
 * Returns human-readable name of audio codec enum.
 */
const char *transcode_get_audio_codec_name(TranscodeAudioCodec codec);

/**
 * Initialize audio transcode engine.
 */
AudioTranscodeEngine *audio_transcode_engine_init(const AudioTranscodeConfig *cfg,
                                                 const AVCodecParameters *in_codecpar,
                                                 AVRational in_time_base);

/**
 * Populate output AVCodecParameters from active audio encoder.
 */
int audio_transcode_engine_get_codecpar(AudioTranscodeEngine *ate, AVCodecParameters *out_codecpar);

/**
 * Get the timebase of the active audio encoder.
 */
AVRational audio_transcode_engine_get_timebase(AudioTranscodeEngine *ate);

/**
 * Submit an incoming compressed audio packet to the decoder.
 */
int audio_transcode_engine_send_packet(AudioTranscodeEngine *ate, const AVPacket *in_pkt);

/**
 * Retrieve the next decoded raw audio AVFrame.
 */
int audio_transcode_engine_receive_decoded_frame(AudioTranscodeEngine *ate, AVFrame *out_frame);

/**
 * Submit an uncompressed raw audio AVFrame (from SDI or decoded IP stream).
 * Performs resampling and AVAudioFifo buffering.
 */
int audio_transcode_engine_send_frame(AudioTranscodeEngine *ate, const AVFrame *raw_frame);

/**
 * Retrieve the next encoded compressed audio AVPacket.
 */
int audio_transcode_engine_receive_packet(AudioTranscodeEngine *ate, AVPacket *out_pkt);

/**
 * Resample decoded audio frame for SDI hardware playout (ensuring 48kHz S16 stereo).
 */
int audio_transcode_engine_resample_for_sdi(AudioTranscodeEngine *ate, const AVFrame *in_frame, AVFrame **out_frame);

/**
 * Free audio transcode engine resources.
 */
void audio_transcode_engine_free(AudioTranscodeEngine **ate);

/**
 * Get initial video PTS offset in encoder timebase units (for B-frame delay).
 */
int64_t transcode_engine_get_pts_offset(TranscodeEngine *te);

/**
 * Set initial audio timestamp offset in sample units to synchronize with video.
 */
void audio_transcode_engine_set_pts_offset(AudioTranscodeEngine *ate, int64_t offset_samples);

/**
 * Flush audio FIFO and reset internal sample state.
 */
void audio_transcode_engine_flush(AudioTranscodeEngine *ate);

/**
 * Flush video transcode engine encoder buffers.
 */
void transcode_engine_flush(TranscodeEngine *te);

#ifdef __cplusplus
}
#endif

#endif /* TRANSCODE_H */
