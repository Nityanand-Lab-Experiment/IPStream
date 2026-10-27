#define _GNU_SOURCE
#include "transcode.h"
#include "logger.h"
#include "utils.h"

#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <string.h>
#include <stdlib.h>

struct TranscodeEngine {
    TranscodeConfig config;

    /* Decoder */
    AVCodecContext *dec_ctx;
    bool has_decoder;

    /* Scaler & Format Converter */
    struct SwsContext *sws_ctx;
    AVFrame *scaled_frame;
    int sws_src_w;
    int sws_src_h;
    enum AVPixelFormat sws_src_fmt;

    /* Encoder */
    AVCodecContext *enc_ctx;
    AVRational in_time_base;
    AVRational enc_time_base;

    int64_t frame_in_count;
    int64_t pkt_out_count;
    int64_t last_pts;
    int64_t last_frame_in_time_us;
    int64_t last_latency_us;
    int64_t pts_offset;
};

TranscodeVideoCodec transcode_parse_codec_name(const char *name) {
    if (!name || strcasecmp(name, "copy") == 0) return TRANSCODE_CODEC_COPY;
    if (strcasecmp(name, "h264") == 0 || strcasecmp(name, "libx264") == 0 || strcasecmp(name, "avc") == 0)
        return TRANSCODE_CODEC_H264;
    if (strcasecmp(name, "hevc") == 0 || strcasecmp(name, "libx265") == 0 || strcasecmp(name, "h265") == 0)
        return TRANSCODE_CODEC_HEVC;
    if (strcasecmp(name, "h264_nvenc") == 0 || strcasecmp(name, "nvenc_h264") == 0)
        return TRANSCODE_CODEC_H264_NVENC;
    if (strcasecmp(name, "hevc_nvenc") == 0 || strcasecmp(name, "nvenc_hevc") == 0 || strcasecmp(name, "h265_nvenc") == 0)
        return TRANSCODE_CODEC_HEVC_NVENC;
    return TRANSCODE_CODEC_COPY;
}

const char *transcode_get_codec_name(TranscodeVideoCodec codec) {
    switch (codec) {
        case TRANSCODE_CODEC_H264: return "libx264 (H.264 CPU)";
        case TRANSCODE_CODEC_HEVC: return "libx265 (H.265/HEVC CPU)";
        case TRANSCODE_CODEC_H264_NVENC: return "h264_nvenc (NVIDIA GPU)";
        case TRANSCODE_CODEC_HEVC_NVENC: return "hevc_nvenc (NVIDIA GPU)";
        case TRANSCODE_CODEC_COPY:
        default: return "copy (Passthrough)";
    }
}

TranscodeRateControl transcode_parse_rc_name(const char *name) {
    if (!name || strcasecmp(name, "cbr") == 0) return TRANSCODE_RC_CBR;
    if (strcasecmp(name, "vbr") == 0) return TRANSCODE_RC_VBR;
    return TRANSCODE_RC_CBR;
}

const char *transcode_get_rc_name(TranscodeRateControl rc) {
    switch (rc) {
        case TRANSCODE_RC_VBR: return "VBR (Variable Bitrate)";
        case TRANSCODE_RC_CBR:
        default: return "CBR (Constant Bitrate)";
    }
}

TranscodeEngine *transcode_engine_init(const TranscodeConfig *cfg,
                                       const AVCodecParameters *in_codecpar,
                                       AVRational in_time_base) {
    if (!cfg || cfg->vcodec == TRANSCODE_CODEC_COPY) {
        return NULL;
    }

    TranscodeEngine *te = (TranscodeEngine*)calloc(1, sizeof(*te));
    if (!te) return NULL;

    te->config = *cfg;
    te->in_time_base = in_time_base;
    te->last_pts = AV_NOPTS_VALUE;

    int src_width = (in_codecpar && in_codecpar->width > 0) ? in_codecpar->width : 1920;
    int src_height = (in_codecpar && in_codecpar->height > 0) ? in_codecpar->height : 1080;
    enum AVPixelFormat src_pix_fmt = (in_codecpar && in_codecpar->format != AV_PIX_FMT_NONE) ?
                                      (enum AVPixelFormat)in_codecpar->format : AV_PIX_FMT_YUV420P;
    (void)src_pix_fmt;

    /* 1. Initialize Decoder if input is compressed */
    if (in_codecpar && in_codecpar->codec_id != AV_CODEC_ID_RAWVIDEO && in_codecpar->codec_id != AV_CODEC_ID_NONE) {
        const AVCodec *decoder = avcodec_find_decoder(in_codecpar->codec_id);
        if (!decoder) {
            LOG_ERROR("[TRANSCODE] Decoder for codec '%s' not found!", avcodec_get_name(in_codecpar->codec_id));
            transcode_engine_free(&te);
            return NULL;
        }

        te->dec_ctx = avcodec_alloc_context3(decoder);
        if (!te->dec_ctx) {
            transcode_engine_free(&te);
            return NULL;
        }

        if (avcodec_parameters_to_context(te->dec_ctx, in_codecpar) < 0) {
            LOG_ERROR("[TRANSCODE] Failed to copy decoder parameters");
            transcode_engine_free(&te);
            return NULL;
        }

        /* Low-delay decoder tuning: slice threading, zero lookahead delay */
        te->dec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        te->dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        te->dec_ctx->thread_type = FF_THREAD_SLICE;
        te->dec_ctx->thread_count = 0; /* Auto slice threads */

        if (avcodec_open2(te->dec_ctx, decoder, NULL) < 0) {
            LOG_ERROR("[TRANSCODE] Failed to open decoder '%s'", decoder->name);
            transcode_engine_free(&te);
            return NULL;
        }
        te->has_decoder = true;
        LOG_INFO("[TRANSCODE] Initialized low-delay decoder: %s", decoder->name);
    }

    /* 2. Determine target dimensions and framerate */
    int target_w = (cfg->width > 0) ? cfg->width : src_width;
    int target_h = (cfg->height > 0) ? cfg->height : src_height;
    AVRational target_fps;
    if (cfg->framerate.num > 0 && cfg->framerate.den > 0) {
        target_fps = cfg->framerate;
    } else if (in_time_base.num > 0 && in_time_base.den > 0) {
        if (in_time_base.num == 1001 && (in_time_base.den == 60000 || in_time_base.den == 30000 || in_time_base.den == 24000)) {
            target_fps = av_inv_q(in_time_base);
        } else if (in_time_base.num == 1 && (in_time_base.den == 60 || in_time_base.den == 50 || in_time_base.den == 30 || in_time_base.den == 25 || in_time_base.den == 24)) {
            target_fps = av_inv_q(in_time_base);
        } else if (in_time_base.den <= 120 && in_time_base.num == 1) {
            target_fps = av_inv_q(in_time_base);
        } else {
            target_fps = (AVRational){60000, 1001};
        }
    } else {
        target_fps = (AVRational){60000, 1001};
    }

    if (cfg->b_frames > 0) {
        te->pts_offset = cfg->b_frames + 2;
    } else {
        te->pts_offset = 0;
    }

    /* 3. Find Encoder */
    const AVCodec *encoder = NULL;
    const char *enc_name = NULL;

    switch (cfg->vcodec) {
        case TRANSCODE_CODEC_H264_NVENC:
            encoder = avcodec_find_encoder_by_name("h264_nvenc");
            if (!encoder) {
                LOG_WARN("[TRANSCODE] h264_nvenc not available, falling back to libx264");
                encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
            }
            break;
        case TRANSCODE_CODEC_HEVC_NVENC:
            encoder = avcodec_find_encoder_by_name("hevc_nvenc");
            if (!encoder) {
                LOG_WARN("[TRANSCODE] hevc_nvenc not available, falling back to libx265");
                encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
            }
            break;
        case TRANSCODE_CODEC_H264:
            encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
            break;
        case TRANSCODE_CODEC_HEVC:
            encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
            break;
        default:
            break;
    }

    if (!encoder) {
        LOG_ERROR("[TRANSCODE] No suitable encoder found for mode: %s", transcode_get_codec_name(cfg->vcodec));
        transcode_engine_free(&te);
        return NULL;
    }
    enc_name = encoder->name;

    /* 4. Configure Encoder Context */
    te->enc_ctx = avcodec_alloc_context3(encoder);
    if (!te->enc_ctx) {
        transcode_engine_free(&te);
        return NULL;
    }

    te->enc_ctx->width = target_w;
    te->enc_ctx->height = target_h;
    te->enc_ctx->time_base = (target_fps.num > 0) ? av_inv_q(target_fps) : (AVRational){1, 60};
    te->enc_ctx->framerate = target_fps;
    te->enc_time_base = te->enc_ctx->time_base;

    /* Determine best pixel format */
    enum AVPixelFormat enc_pix_fmt = AV_PIX_FMT_YUV420P;
    if (strstr(enc_name, "nvenc")) {
        enc_pix_fmt = AV_PIX_FMT_NV12;
    }
    te->enc_ctx->pix_fmt = enc_pix_fmt;

    /* Bitrate & Buffer Size (Rate Control: CBR or VBR) */
    int64_t br = (cfg->vbitrate > 0) ? cfg->vbitrate : 4000000;
    te->enc_ctx->bit_rate = br;

    if (cfg->rc == TRANSCODE_RC_VBR) {
        int64_t max_br = (cfg->maxrate > 0) ? cfg->maxrate : (int64_t)(br * 1.5);
        int64_t buf_sz = (cfg->bufsize > 0) ? cfg->bufsize : (br * 2); /* 2 seconds buffer for VBR */
        te->enc_ctx->rc_max_rate = max_br;
        te->enc_ctx->rc_min_rate = 0;
        te->enc_ctx->rc_buffer_size = buf_sz;
    } else {
        /* CBR: strict bit_rate == rc_max_rate == rc_min_rate */
        int64_t buf_sz = (cfg->bufsize > 0) ? cfg->bufsize : (br / 2); /* 500ms low-latency VBV buffer */
        te->enc_ctx->rc_max_rate = br;
        te->enc_ctx->rc_min_rate = br;
        te->enc_ctx->rc_buffer_size = buf_sz;
    }

    te->enc_ctx->gop_size = (cfg->gop_size > 0) ? cfg->gop_size : 30;
    te->enc_ctx->max_b_frames = (cfg->b_frames >= 0) ? cfg->b_frames : 0;

    /* Low-latency flags and threading */
    if (te->enc_ctx->max_b_frames == 0) {
        te->enc_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    te->enc_ctx->thread_type = FF_THREAD_SLICE;
    te->enc_ctx->slices = 4;

    /* 5. Encoder-Specific Low-Latency Options */
    AVDictionary *enc_opts = NULL;

    if (strcmp(enc_name, "libx264") == 0) {
        const char *preset = (cfg->preset[0] != '\0') ? cfg->preset : "ultrafast";
        av_dict_set(&enc_opts, "preset", preset, 0);
        av_dict_set(&enc_opts, "tune", "zerolatency", 0);
        if (cfg->intra_refresh) {
            av_dict_set(&enc_opts, "intra-refresh", "1", 0);
        }
        if (cfg->rc == TRANSCODE_RC_CBR) {
            av_dict_set(&enc_opts, "x264-params", "nal-hrd=cbr:bframes=0:rc-lookahead=0:sync-lookahead=0:sliced-threads=1:repeat-headers=1:aud=1:scenecut=0", 0);
        } else {
            av_dict_set(&enc_opts, "x264-params", "bframes=0:rc-lookahead=0:sync-lookahead=0:sliced-threads=1:repeat-headers=1:aud=1:scenecut=0", 0);
        }
    } else if (strcmp(enc_name, "libx265") == 0) {
        const char *preset = (cfg->preset[0] != '\0') ? cfg->preset : "ultrafast";
        av_dict_set(&enc_opts, "preset", preset, 0);
        av_dict_set(&enc_opts, "tune", "zerolatency", 0);
        char x265_opts[256];
        snprintf(x265_opts, sizeof(x265_opts), "%sbframes=0:rc-lookahead=0:no-sao=1:repeat-headers=1:aud=1%s",
                 cfg->intra_refresh ? "intra-refresh=1:" : "",
                 (cfg->rc == TRANSCODE_RC_CBR) ? ":strict-cbr=1" : "");
        av_dict_set(&enc_opts, "x265-params", x265_opts, 0);
    } else if (strstr(enc_name, "nvenc")) {
        const char *nv_preset = "p1";
        if (cfg->preset[0] != '\0') {
            if (strcasecmp(cfg->preset, "ultrafast") == 0) nv_preset = "p1";
            else if (strcasecmp(cfg->preset, "superfast") == 0) nv_preset = "p2";
            else if (strcasecmp(cfg->preset, "veryfast") == 0 || strcasecmp(cfg->preset, "fast") == 0) nv_preset = "p3";
            else if (strcasecmp(cfg->preset, "medium") == 0) nv_preset = "p4";
            else if (strcasecmp(cfg->preset, "slow") == 0) nv_preset = "p5";
            else nv_preset = cfg->preset;
        }
        av_dict_set(&enc_opts, "preset", nv_preset, 0);

        const char *nv_tune = (cfg->b_frames > 0) ? "hq" : "ull";
        if (cfg->tune[0] != '\0') {
            if (strcasecmp(cfg->tune, "zerolatency") == 0) nv_tune = (cfg->b_frames > 0) ? "ll" : "ull";
            else nv_tune = cfg->tune;
        }
        av_dict_set(&enc_opts, "tune", nv_tune, 0);

        if (cfg->rc == TRANSCODE_RC_CBR) {
            av_dict_set(&enc_opts, "rc", "cbr", 0);
            av_dict_set(&enc_opts, "cbr", "1", 0);
        } else {
            av_dict_set(&enc_opts, "rc", "vbr", 0);
            av_dict_set(&enc_opts, "cbr", "0", 0);
        }

        if (cfg->b_frames == 0) {
            av_dict_set(&enc_opts, "zerolatency", "1", 0);
            av_dict_set(&enc_opts, "delay", "0", 0);
            av_dict_set(&enc_opts, "surfaces", "2", 0);
        } else {
            char delay_str[16];
            snprintf(delay_str, sizeof(delay_str), "%d", cfg->b_frames);
            av_dict_set(&enc_opts, "delay", delay_str, 0);
            av_dict_set(&enc_opts, "b_ref_mode", "middle", 0);
            av_dict_set(&enc_opts, "surfaces", "16", 0);
        }

        /* Crucial for broadcast multicast and live player lock-on */
        av_dict_set(&enc_opts, "forced-idr", "1", 0);
        av_dict_set(&enc_opts, "aud", "1", 0);
        av_dict_set(&enc_opts, "no-scenecut", "1", 0);

        if (cfg->intra_refresh) {
            av_dict_set(&enc_opts, "intra-refresh", "1", 0);
        }
    }

    if (avcodec_open2(te->enc_ctx, encoder, &enc_opts) < 0) {
        LOG_ERROR("[TRANSCODE] Failed to open encoder '%s'", enc_name);
        av_dict_free(&enc_opts);
        transcode_engine_free(&te);
        return NULL;
    }
    av_dict_free(&enc_opts);

    /* 6. Allocate Intermediate Scaled Frame */
    te->scaled_frame = av_frame_alloc();
    if (!te->scaled_frame) {
        transcode_engine_free(&te);
        return NULL;
    }
    te->scaled_frame->format = enc_pix_fmt;
    te->scaled_frame->width = target_w;
    te->scaled_frame->height = target_h;
    if (av_frame_get_buffer(te->scaled_frame, 32) < 0) {
        LOG_ERROR("[TRANSCODE] Failed to allocate scaled frame buffer");
        transcode_engine_free(&te);
        return NULL;
    }

    LOG_INFO("==================================================================");
    LOG_INFO("[TRANSCODE] Low-Latency Transcoder Active:");
    LOG_INFO("  Encoder Engine : %s (%s)", enc_name, transcode_get_codec_name(cfg->vcodec));
    LOG_INFO("  Resolution     : %dx%d (Bitrate: %d kbps, GOP: %d)",
             target_w, target_h, (int)(br / 1000), te->enc_ctx->gop_size);
    LOG_INFO("  Rate Control   : %s (Max: %d kbps, VBV Buf: %d kb)",
             transcode_get_rc_name(cfg->rc),
             (int)(te->enc_ctx->rc_max_rate / 1000),
             (int)(te->enc_ctx->rc_buffer_size / 1000));
    LOG_INFO("  Latency Tuning : %d B-frames, %s",
             te->enc_ctx->max_b_frames,
             cfg->intra_refresh ? "Intra-Refresh ON" : "Intra-Refresh OFF");
    LOG_INFO("==================================================================");

    return te;
}

int transcode_engine_get_codecpar(TranscodeEngine *te, AVCodecParameters *out_codecpar) {
    if (!te || !te->enc_ctx || !out_codecpar) return -1;
    return avcodec_parameters_from_context(out_codecpar, te->enc_ctx);
}

AVRational transcode_engine_get_timebase(TranscodeEngine *te) {
    if (!te || !te->enc_ctx) return (AVRational){1, 60};
    return te->enc_time_base;
}

int transcode_engine_send_packet(TranscodeEngine *te, const AVPacket *in_pkt) {
    if (!te || !te->dec_ctx) return -1;
    return avcodec_send_packet(te->dec_ctx, in_pkt);
}

int transcode_engine_receive_decoded_frame(TranscodeEngine *te, AVFrame *out_frame) {
    if (!te || !te->dec_ctx || !out_frame) return -1;
    return avcodec_receive_frame(te->dec_ctx, out_frame);
}

int transcode_engine_send_frame(TranscodeEngine *te, const AVFrame *raw_frame) {
    if (!te || !te->enc_ctx) return -1;

    if (!raw_frame) {
        /* Flush encoder */
        return avcodec_send_frame(te->enc_ctx, NULL);
    }

    te->last_frame_in_time_us = av_gettime_relative();
    te->frame_in_count++;

    const AVFrame *frame_to_encode = raw_frame;

    int64_t base_pts = raw_frame->pts;
    if (base_pts == AV_NOPTS_VALUE) {
        base_pts = te->frame_in_count;
    } else if (te->has_decoder && te->in_time_base.num > 0 && te->enc_time_base.num > 0) {
        base_pts = av_rescale_q(base_pts, te->in_time_base, te->enc_time_base);
    }
    int64_t target_pts = base_pts + te->pts_offset;

    /* Check if format or size conversion is required */
    if (raw_frame->format != te->enc_ctx->pix_fmt ||
        raw_frame->width != te->enc_ctx->width ||
        raw_frame->height != te->enc_ctx->height) {

        if (!te->sws_ctx || te->sws_src_w != raw_frame->width ||
            te->sws_src_h != raw_frame->height || te->sws_src_fmt != raw_frame->format) {
            if (te->sws_ctx) sws_freeContext(te->sws_ctx);
            te->sws_ctx = sws_getContext(raw_frame->width, raw_frame->height,
                                         (enum AVPixelFormat)raw_frame->format,
                                         te->enc_ctx->width, te->enc_ctx->height,
                                         te->enc_ctx->pix_fmt,
                                         SWS_FAST_BILINEAR, NULL, NULL, NULL);
            te->sws_src_w = raw_frame->width;
            te->sws_src_h = raw_frame->height;
            te->sws_src_fmt = (enum AVPixelFormat)raw_frame->format;
        }

        if (te->sws_ctx) {
            sws_scale(te->sws_ctx, (const uint8_t * const*)raw_frame->data, raw_frame->linesize, 0,
                      raw_frame->height, te->scaled_frame->data, te->scaled_frame->linesize);
            te->scaled_frame->pts = target_pts;
            frame_to_encode = te->scaled_frame;
        }
    } else {
        ((AVFrame*)raw_frame)->pts = target_pts;
    }

    return avcodec_send_frame(te->enc_ctx, frame_to_encode);
}

int transcode_engine_receive_packet(TranscodeEngine *te, AVPacket *out_pkt) {
    if (!te || !te->enc_ctx || !out_pkt) return -1;
    int ret = avcodec_receive_packet(te->enc_ctx, out_pkt);
    if (ret == 0) {
        te->pkt_out_count++;
        int64_t now_us = av_gettime_relative();
        if (te->last_frame_in_time_us > 0) {
            te->last_latency_us = now_us - te->last_frame_in_time_us;
        }
    }
    return ret;
}

int64_t transcode_engine_get_last_latency_us(TranscodeEngine *te) {
    return te ? te->last_latency_us : 0;
}

void transcode_engine_free(TranscodeEngine **te_ptr) {
    if (!te_ptr || !*te_ptr) return;
    TranscodeEngine *te = *te_ptr;

    if (te->dec_ctx) {
        avcodec_free_context(&te->dec_ctx);
    }
    if (te->enc_ctx) {
        avcodec_free_context(&te->enc_ctx);
    }
    if (te->sws_ctx) {
        sws_freeContext(te->sws_ctx);
    }
    if (te->scaled_frame) {
        av_frame_free(&te->scaled_frame);
    }

    free(te);
    *te_ptr = NULL;
}

/* =========================================================================
 * Audio Transcode Engine Implementation
 * ========================================================================= */

struct AudioTranscodeEngine {
    AudioTranscodeConfig config;

    /* Decoder */
    AVCodecContext *dec_ctx;
    bool has_decoder;

    /* Resampler for Encoding */
    SwrContext *swr_ctx;
    int swr_src_rate;
    enum AVSampleFormat swr_src_fmt;
    AVChannelLayout swr_src_layout;

    /* Audio FIFO and pre-allocated encoder frame */
    AVAudioFifo *fifo;
    int enc_frame_size;
    AVFrame *enc_frame;

    /* Encoder */
    AVCodecContext *enc_ctx;
    AVRational in_time_base;
    AVRational enc_time_base;
    int64_t total_out_samples;

    /* Resampler for SDI playout (ensuring 48kHz S16 stereo) */
    SwrContext *sdi_swr_ctx;
    int sdi_swr_src_rate;
    enum AVSampleFormat sdi_swr_src_fmt;
    AVChannelLayout sdi_swr_src_layout;
};

TranscodeAudioCodec transcode_parse_audio_codec_name(const char *name) {
    if (!name || strcasecmp(name, "copy") == 0) return TRANSCODE_AUDIO_COPY;
    if (strcasecmp(name, "aac") == 0 || strcasecmp(name, "libfdk_aac") == 0)
        return TRANSCODE_AUDIO_AAC;
    if (strcasecmp(name, "opus") == 0 || strcasecmp(name, "libopus") == 0)
        return TRANSCODE_AUDIO_OPUS;
    if (strcasecmp(name, "mp2") == 0)
        return TRANSCODE_AUDIO_MP2;
    return TRANSCODE_AUDIO_COPY;
}

const char *transcode_get_audio_codec_name(TranscodeAudioCodec codec) {
    switch (codec) {
        case TRANSCODE_AUDIO_AAC: return "aac (AAC Audio)";
        case TRANSCODE_AUDIO_OPUS: return "opus (Opus Audio)";
        case TRANSCODE_AUDIO_MP2: return "mp2 (MPEG-1 Layer II)";
        case TRANSCODE_AUDIO_COPY:
        default: return "copy (Passthrough)";
    }
}

AudioTranscodeEngine *audio_transcode_engine_init(const AudioTranscodeConfig *cfg,
                                                 const AVCodecParameters *in_codecpar,
                                                 AVRational in_time_base) {
    if (!cfg || cfg->acodec == TRANSCODE_AUDIO_COPY) {
        return NULL;
    }

    AudioTranscodeEngine *ate = (AudioTranscodeEngine*)calloc(1, sizeof(*ate));
    if (!ate) return NULL;

    ate->config = *cfg;
    ate->in_time_base = in_time_base;

    /* 1. Decoder initialization if input stream has compressed audio */
    if (in_codecpar && in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        in_codecpar->codec_id != AV_CODEC_ID_PCM_S16LE &&
        in_codecpar->codec_id != AV_CODEC_ID_RAWVIDEO &&
        in_codecpar->codec_id != AV_CODEC_ID_NONE) {

        const AVCodec *decoder = avcodec_find_decoder(in_codecpar->codec_id);
        if (decoder) {
            ate->dec_ctx = avcodec_alloc_context3(decoder);
            if (ate->dec_ctx) {
                if (avcodec_parameters_to_context(ate->dec_ctx, in_codecpar) == 0 &&
                    avcodec_open2(ate->dec_ctx, decoder, NULL) == 0) {
                    ate->has_decoder = true;
                    LOG_INFO("[AUDIO TRANSCODE] Decoder initialized for '%s'", decoder->name);
                } else {
                    avcodec_free_context(&ate->dec_ctx);
                }
            }
        }
    }

    /* 2. Encoder lookup */
    const AVCodec *encoder = NULL;
    const char *enc_name = "aac";
    if (cfg->acodec == TRANSCODE_AUDIO_AAC) {
        encoder = avcodec_find_encoder_by_name("aac");
        if (!encoder) encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        enc_name = "aac";
    } else if (cfg->acodec == TRANSCODE_AUDIO_OPUS) {
        encoder = avcodec_find_encoder_by_name("libopus");
        if (!encoder) encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        enc_name = "opus";
    } else if (cfg->acodec == TRANSCODE_AUDIO_MP2) {
        encoder = avcodec_find_encoder_by_name("mp2");
        if (!encoder) encoder = avcodec_find_encoder(AV_CODEC_ID_MP2);
        enc_name = "mp2";
    }

    if (!encoder) {
        LOG_ERROR("[AUDIO TRANSCODE] Audio encoder '%s' not found!", enc_name);
        audio_transcode_engine_free(&ate);
        return NULL;
    }

    ate->enc_ctx = avcodec_alloc_context3(encoder);
    if (!ate->enc_ctx) {
        audio_transcode_engine_free(&ate);
        return NULL;
    }

    int target_sr = (cfg->sample_rate > 0) ? cfg->sample_rate : 48000;
    int target_channels = (cfg->channels > 0) ? cfg->channels : 2;
    int target_br = (cfg->abitrate > 0) ? cfg->abitrate : 192000;

    ate->enc_ctx->sample_rate = target_sr;
    av_channel_layout_default(&ate->enc_ctx->ch_layout, target_channels);

    enum AVSampleFormat best_fmt = AV_SAMPLE_FMT_FLTP;
    if (encoder->sample_fmts) {
        best_fmt = encoder->sample_fmts[0];
    }
    ate->enc_ctx->sample_fmt = best_fmt;
    ate->enc_ctx->bit_rate = target_br;
    ate->enc_ctx->time_base = (AVRational){1, target_sr};
    ate->enc_time_base = ate->enc_ctx->time_base;

    if (ate->enc_ctx->flags & AVFMT_GLOBALHEADER) {
        ate->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ate->enc_ctx, encoder, NULL) < 0) {
        LOG_ERROR("[AUDIO TRANSCODE] Failed to open audio encoder '%s'", enc_name);
        audio_transcode_engine_free(&ate);
        return NULL;
    }

    ate->enc_frame_size = (ate->enc_ctx->frame_size > 0) ? ate->enc_ctx->frame_size : 1024;

    /* Pre-allocate enc_frame */
    ate->enc_frame = av_frame_alloc();
    if (!ate->enc_frame) {
        audio_transcode_engine_free(&ate);
        return NULL;
    }
    ate->enc_frame->nb_samples = ate->enc_frame_size;
    ate->enc_frame->format = ate->enc_ctx->sample_fmt;
    av_channel_layout_copy(&ate->enc_frame->ch_layout, &ate->enc_ctx->ch_layout);
    ate->enc_frame->sample_rate = ate->enc_ctx->sample_rate;
    if (av_frame_get_buffer(ate->enc_frame, 0) < 0) {
        audio_transcode_engine_free(&ate);
        return NULL;
    }

    /* Allocate AVAudioFifo with capacity for at least 8 frames */
    ate->fifo = av_audio_fifo_alloc(ate->enc_ctx->sample_fmt,
                                    ate->enc_ctx->ch_layout.nb_channels,
                                    ate->enc_frame_size * 8);
    if (!ate->fifo) {
        audio_transcode_engine_free(&ate);
        return NULL;
    }

    LOG_INFO("==================================================================");
    LOG_INFO("[AUDIO TRANSCODE] Audio Transcoder Active:");
    LOG_INFO("  Codec Engine   : %s (%s)", enc_name, transcode_get_audio_codec_name(cfg->acodec));
    LOG_INFO("  Sample Rate    : %d Hz (Channels: %d, Bitrate: %d kbps)",
             target_sr, target_channels, (int)(target_br / 1000));
    LOG_INFO("  Frame Size     : %d samples / frame", ate->enc_frame_size);
    LOG_INFO("==================================================================");

    return ate;
}

int audio_transcode_engine_get_codecpar(AudioTranscodeEngine *ate, AVCodecParameters *out_codecpar) {
    if (!ate || !ate->enc_ctx || !out_codecpar) return -1;
    return avcodec_parameters_from_context(out_codecpar, ate->enc_ctx);
}

AVRational audio_transcode_engine_get_timebase(AudioTranscodeEngine *ate) {
    if (!ate || !ate->enc_ctx) return (AVRational){1, 48000};
    return ate->enc_time_base;
}

int audio_transcode_engine_send_packet(AudioTranscodeEngine *ate, const AVPacket *in_pkt) {
    if (!ate || !ate->dec_ctx) return -1;
    return avcodec_send_packet(ate->dec_ctx, in_pkt);
}

int audio_transcode_engine_receive_decoded_frame(AudioTranscodeEngine *ate, AVFrame *out_frame) {
    if (!ate || !ate->dec_ctx || !out_frame) return -1;
    return avcodec_receive_frame(ate->dec_ctx, out_frame);
}

int audio_transcode_engine_send_frame(AudioTranscodeEngine *ate, const AVFrame *raw_frame) {
    if (!ate || !ate->enc_ctx || !ate->fifo) return -1;

    if (!raw_frame) {
        /* Flush remaining samples from FIFO to encoder */
        int remain = av_audio_fifo_size(ate->fifo);
        if (remain > 0) {
            av_audio_fifo_read(ate->fifo, (void **)ate->enc_frame->data, remain);
            ate->enc_frame->nb_samples = remain;
            ate->enc_frame->pts = ate->total_out_samples;
            ate->total_out_samples += remain;
            avcodec_send_frame(ate->enc_ctx, ate->enc_frame);
        }
        return avcodec_send_frame(ate->enc_ctx, NULL);
    }

    if (raw_frame->nb_samples <= 0) return 0;

    /* Check if format, sample rate, or channel layout differs from encoder requirements */
    bool need_swr = (raw_frame->format != ate->enc_ctx->sample_fmt ||
                     raw_frame->sample_rate != ate->enc_ctx->sample_rate ||
                     av_channel_layout_compare(&raw_frame->ch_layout, &ate->enc_ctx->ch_layout) != 0);

    if (need_swr) {
        if (!ate->swr_ctx ||
            ate->swr_src_fmt != raw_frame->format ||
            ate->swr_src_rate != raw_frame->sample_rate ||
            av_channel_layout_compare(&ate->swr_src_layout, &raw_frame->ch_layout) != 0) {

            if (ate->swr_ctx) swr_free(&ate->swr_ctx);
            int ret = swr_alloc_set_opts2(&ate->swr_ctx,
                                          &ate->enc_ctx->ch_layout,
                                          ate->enc_ctx->sample_fmt,
                                          ate->enc_ctx->sample_rate,
                                          &raw_frame->ch_layout,
                                          (enum AVSampleFormat)raw_frame->format,
                                          raw_frame->sample_rate,
                                          0, NULL);
            if (ret < 0 || swr_init(ate->swr_ctx) < 0) {
                LOG_ERROR("[AUDIO TRANSCODE] swr_init failed for audio resampling!");
                return -1;
            }
            ate->swr_src_fmt = (enum AVSampleFormat)raw_frame->format;
            ate->swr_src_rate = raw_frame->sample_rate;
            av_channel_layout_copy(&ate->swr_src_layout, &raw_frame->ch_layout);
        }

        int max_out_samples = av_rescale_rnd(swr_get_delay(ate->swr_ctx, raw_frame->sample_rate) + raw_frame->nb_samples,
                                             ate->enc_ctx->sample_rate, raw_frame->sample_rate, AV_ROUND_UP);
        if (max_out_samples <= 0) max_out_samples = raw_frame->nb_samples;

        uint8_t **resampled_data = NULL;
        int ret = av_samples_alloc_array_and_samples(&resampled_data, NULL,
                                                     ate->enc_ctx->ch_layout.nb_channels,
                                                     max_out_samples,
                                                     ate->enc_ctx->sample_fmt, 0);
        if (ret < 0) {
            return ret;
        }

        int actual_out = swr_convert(ate->swr_ctx, resampled_data, max_out_samples,
                                     (const uint8_t **)raw_frame->data, raw_frame->nb_samples);
        if (actual_out > 0) {
            if (av_audio_fifo_space(ate->fifo) < actual_out) {
                if (av_audio_fifo_realloc(ate->fifo, av_audio_fifo_size(ate->fifo) + actual_out + ate->enc_frame_size * 2) < 0) {
                    LOG_ERROR("[AUDIO TRANSCODE] av_audio_fifo_realloc failed!");
                }
            }
            av_audio_fifo_write(ate->fifo, (void **)resampled_data, actual_out);
        }

        av_freep(&resampled_data[0]);
        av_freep(&resampled_data);
    } else {
        if (av_audio_fifo_space(ate->fifo) < raw_frame->nb_samples) {
            if (av_audio_fifo_realloc(ate->fifo, av_audio_fifo_size(ate->fifo) + raw_frame->nb_samples + ate->enc_frame_size * 2) < 0) {
                LOG_ERROR("[AUDIO TRANSCODE] av_audio_fifo_realloc failed!");
            }
        }
        av_audio_fifo_write(ate->fifo, (void **)raw_frame->data, raw_frame->nb_samples);
    }

    return 0;
}

int audio_transcode_engine_receive_packet(AudioTranscodeEngine *ate, AVPacket *out_pkt) {
    if (!ate || !ate->enc_ctx || !out_pkt) return -1;

    /* 1. Check if encoder has already buffered a packet */
    int ret = avcodec_receive_packet(ate->enc_ctx, out_pkt);
    if (ret == 0) return 0;
    if (ret != AVERROR(EAGAIN)) return ret;

    /* 2. If more frames needed and FIFO has at least frame_size samples, feed one frame */
    if (av_audio_fifo_size(ate->fifo) >= ate->enc_frame_size) {
        int read_count = av_audio_fifo_read(ate->fifo, (void **)ate->enc_frame->data, ate->enc_frame_size);
        if (read_count < ate->enc_frame_size) {
            return AVERROR(EAGAIN);
        }
        ate->enc_frame->nb_samples = ate->enc_frame_size;
        ate->enc_frame->pts = ate->total_out_samples;
        ate->total_out_samples += ate->enc_frame_size;

        ret = avcodec_send_frame(ate->enc_ctx, ate->enc_frame);
        if (ret < 0) {
            return ret;
        }
        return avcodec_receive_packet(ate->enc_ctx, out_pkt);
    }

    return AVERROR(EAGAIN);
}

int audio_transcode_engine_resample_for_sdi(AudioTranscodeEngine *ate, const AVFrame *in_frame, AVFrame **out_frame) {
    if (!ate || !in_frame || !out_frame) return -1;

    AVChannelLayout sdi_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    if (in_frame->format == AV_SAMPLE_FMT_S16 &&
        in_frame->sample_rate == 48000 &&
        av_channel_layout_compare(&in_frame->ch_layout, &sdi_layout) == 0) {
        *out_frame = av_frame_clone(in_frame);
        return 0;
    }

    if (!ate->sdi_swr_ctx ||
        ate->sdi_swr_src_fmt != in_frame->format ||
        ate->sdi_swr_src_rate != in_frame->sample_rate ||
        av_channel_layout_compare(&ate->sdi_swr_src_layout, &in_frame->ch_layout) != 0) {

        if (ate->sdi_swr_ctx) swr_free(&ate->sdi_swr_ctx);
        int ret = swr_alloc_set_opts2(&ate->sdi_swr_ctx,
                                      &sdi_layout,
                                      AV_SAMPLE_FMT_S16,
                                      48000,
                                      &in_frame->ch_layout,
                                      (enum AVSampleFormat)in_frame->format,
                                      in_frame->sample_rate,
                                      0, NULL);
        if (ret < 0 || swr_init(ate->sdi_swr_ctx) < 0) {
            return -1;
        }
        ate->sdi_swr_src_fmt = (enum AVSampleFormat)in_frame->format;
        ate->sdi_swr_src_rate = in_frame->sample_rate;
        av_channel_layout_copy(&ate->sdi_swr_src_layout, &in_frame->ch_layout);
    }

    int out_samples = av_rescale_rnd(swr_get_delay(ate->sdi_swr_ctx, in_frame->sample_rate) + in_frame->nb_samples,
                                     48000, in_frame->sample_rate, AV_ROUND_UP);
    if (out_samples <= 0) out_samples = in_frame->nb_samples;

    AVFrame *resampled = av_frame_alloc();
    if (!resampled) return -1;

    resampled->format = AV_SAMPLE_FMT_S16;
    resampled->sample_rate = 48000;
    av_channel_layout_copy(&resampled->ch_layout, &sdi_layout);
    resampled->nb_samples = out_samples;
    if (av_frame_get_buffer(resampled, 0) < 0) {
        av_frame_free(&resampled);
        return -1;
    }

    int actual_out = swr_convert(ate->sdi_swr_ctx, resampled->data, out_samples,
                                 (const uint8_t **)in_frame->data, in_frame->nb_samples);
    if (actual_out <= 0) {
        av_frame_free(&resampled);
        return -1;
    }
    resampled->nb_samples = actual_out;
    resampled->pts = in_frame->pts;
    *out_frame = resampled;
    return 0;
}

void audio_transcode_engine_free(AudioTranscodeEngine **ate_ptr) {
    if (!ate_ptr || !*ate_ptr) return;
    AudioTranscodeEngine *ate = *ate_ptr;

    if (ate->dec_ctx) avcodec_free_context(&ate->dec_ctx);
    if (ate->enc_ctx) avcodec_free_context(&ate->enc_ctx);
    if (ate->swr_ctx) swr_free(&ate->swr_ctx);
    if (ate->sdi_swr_ctx) swr_free(&ate->sdi_swr_ctx);
    if (ate->fifo) av_audio_fifo_free(ate->fifo);
    if (ate->enc_frame) av_frame_free(&ate->enc_frame);

    free(ate);
    *ate_ptr = NULL;
}

int64_t transcode_engine_get_pts_offset(TranscodeEngine *te) {
    return te ? te->pts_offset : 0;
}

void audio_transcode_engine_set_pts_offset(AudioTranscodeEngine *ate, int64_t offset_samples) {
    if (ate) {
        ate->total_out_samples = offset_samples;
    }
}

void audio_transcode_engine_flush(AudioTranscodeEngine *ate) {
    if (!ate) return;
    if (ate->fifo) {
        av_audio_fifo_reset(ate->fifo);
    }
    if (ate->enc_ctx) {
        avcodec_flush_buffers(ate->enc_ctx);
    }
    ate->total_out_samples = 0;
}

void transcode_engine_flush(TranscodeEngine *te) {
    if (!te) return;
    if (te->enc_ctx) {
        avcodec_flush_buffers(te->enc_ctx);
    }
    te->frame_in_count = 0;
}
