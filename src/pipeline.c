#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "utils.h"

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <sys/stat.h>

static void create_parent_dir_if_local(const char *filepath) {
    if (!filepath || strncmp(filepath, "http://", 7) == 0 || strncmp(filepath, "https://", 8) == 0) {
        return;
    }
    char tmp[1024];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *p = strrchr(tmp, '/');
    if (!p) return;
    *p = '\0';
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(tmp, 0755);
            *s = '/';
        }
    }
    mkdir(tmp, 0755);
}

static volatile sig_atomic_t s_stop_requested = 0;
static volatile uint64_t s_udp_lost_packets = 0;
static volatile uint64_t s_udp_cc_errors = 0;

static void ffmpeg_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    if (level <= AV_LOG_WARNING && fmt && strstr(fmt, "Continuity check failed")) {
        int pid = 0, expected = 0, got = 0;
        va_list vl_copy;
        va_copy(vl_copy, vl);
        char line[512];
        vsnprintf(line, sizeof(line), fmt, vl_copy);
        va_end(vl_copy);

        if (sscanf(line, "Continuity check failed for pid %d expected %d got %d", &pid, &expected, &got) == 3) {
            int lost = (got - expected) & 0x0F;
            if (lost <= 0) lost = 1;
            s_udp_cc_errors++;
            s_udp_lost_packets += (uint64_t)lost;
        } else {
            s_udp_cc_errors++;
            s_udp_lost_packets++;
        }
        return; /* Suppress raw line to prevent corrupting live terminal UI */
    }
    av_log_default_callback(ptr, level, fmt, vl);
}

typedef struct {
    int64_t last_packet_time_us;
    int timeout_sec;
} InterruptContext;

static int interrupt_cb(void *opaque) {
    if (s_stop_requested) {
        return 1;
    }
    InterruptContext *ctx = (InterruptContext *)opaque;
    if (ctx && ctx->timeout_sec > 0 && ctx->last_packet_time_us > 0) {
        int64_t now_us = av_gettime_relative();
        if ((now_us - ctx->last_packet_time_us) > ((int64_t)ctx->timeout_sec * 1000000LL)) {
            LOG_WARN("Network read timeout exceeded (%d seconds with no data)", ctx->timeout_sec);
            return 1;
        }
    }
    return 0;
}

static int output_interrupt_cb(void *opaque) {
    (void)opaque;
    return s_stop_requested ? 1 : 0;
}

SyncType parse_sync_type(const char *str) {
    if (!str) return SYNC_TYPE_HARDWARE;
    if (strcasecmp(str, "self") == 0 ||
        strcasecmp(str, "self-generated") == 0 ||
        strcasecmp(str, "self_generated") == 0 ||
        strcasecmp(str, "gen") == 0 ||
        strcasecmp(str, "internal") == 0) {
        return SYNC_TYPE_SELF;
    }
    return SYNC_TYPE_HARDWARE;
}

const char *sync_type_to_string(SyncType type) {
    switch (type) {
        case SYNC_TYPE_SELF: return "self";
        case SYNC_TYPE_HARDWARE:
        default: return "hardware";
    }
}

void pipeline_config_init_defaults(PipelineConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    strncpy(config->input_url, "udp://239.255.0.1:1234", sizeof(config->input_url) - 1);
    config->output_url[0] = '\0';
    config->num_outputs = 0;
    config->local_addr[0] = '\0';
    config->fifo_size = (50 * 1024 * 1024) / 188;  /* ~50 MB in 188-byte packets */
    config->buffer_size = 16 * 1024 * 1024;     /* 16 MB OS UDP receive buffer */
    config->overrun_nonfatal = 1;
    config->timeout_sec = 10;                   /* 10 seconds timeout */
    config->wait_keyframe = true;               /* Synchronize on first keyframe */
    config->normalize_pts = true;               /* Offset timestamps starting at 0 */
    config->print_stats = true;
    config->stats_interval_ms = 1000;           /* 1 second default telemetry interval */
    config->json_telemetry = false;             /* JSON telemetry */
    config->detailed_telemetry = false;         /* Multi-line card */
    config->log_telemetry = false;              /* Output telemetry as standard LOG entries */
    config->enable_diagnostics = true;          /* Active YouTube buffering diagnostics */
    config->hls_url[0] = '\0';
    strncpy(config->hls_method, "PUT", sizeof(config->hls_method) - 1);
    config->hls_time = 2;
    config->hls_list_size = 5;
    config->hls_delete_segments = true;
    config->hls_segment_filename[0] = '\0';
    config->hls_fmp4 = false;
    config->srt_url[0] = '\0';
    config->rtp_url[0] = '\0';
    config->srt_latency_ms = 200;
    config->srt_passphrase[0] = '\0';
    config->srt_streamid[0] = '\0';
    config->srt_mode[0] = '\0';
    config->srt_max_clients = 32;
    config->monitor_file[0] = '\0';
    config->monitor_fps = 5;
    config->monitor_width = 0;
    config->monitor_height = 0;
    config->monitor_quality = 4;

    /* Blackmagic SDI Defaults */
    strncpy(config->sdi_mode, "auto", sizeof(config->sdi_mode) - 1);
    config->sdi_channels = 2;
    config->sdi_test_bars = false;
    config->sync_type = SYNC_TYPE_HARDWARE;

    /* Low-Latency Transcoding Defaults */
    config->transcode.vcodec = TRANSCODE_CODEC_COPY;
    strncpy(config->transcode.vcodec_name, "copy", sizeof(config->transcode.vcodec_name) - 1);
    config->transcode.vbitrate = 4000000;
    config->transcode.rc = TRANSCODE_RC_CBR;
    strncpy(config->transcode.rc_name, "cbr", sizeof(config->transcode.rc_name) - 1);
    config->transcode.maxrate = 0;
    config->transcode.bufsize = 0;
    config->transcode.gop_size = 30;
    config->transcode.b_frames = 0;
    strncpy(config->transcode.preset, "ultrafast", sizeof(config->transcode.preset) - 1);
    strncpy(config->transcode.tune, "zerolatency", sizeof(config->transcode.tune) - 1);
    config->transcode.intra_refresh = false;
    config->transcode.low_delay = true;

    /* Audio Transcoding Defaults */
    config->audio_transcode.acodec = TRANSCODE_AUDIO_COPY;
    strncpy(config->audio_transcode.acodec_name, "copy", sizeof(config->audio_transcode.acodec_name) - 1);
    config->audio_transcode.abitrate = 192000;
    config->audio_transcode.sample_rate = 48000;
    config->audio_transcode.channels = 2;
}

void pipeline_request_stop(void) {
    s_stop_requested = 1;
}

static void log_stream_info(const AVStream *stream, int stream_index, const char *prefix) {
    const AVCodecParameters *codecpar = stream->codecpar;
    const char *type_name = av_get_media_type_string(codecpar->codec_type);
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const char *codec_name = desc ? desc->name : "unknown";

    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        LOG_INFO("%s Stream #%d: Video [%s], %dx%d, time_base=%d/%d",
                 prefix, stream_index, codec_name,
                 codecpar->width, codecpar->height,
                 stream->time_base.num, stream->time_base.den);
    } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        LOG_INFO("%s Stream #%d: Audio [%s], %d channels, %d Hz, time_base=%d/%d",
                 prefix, stream_index, codec_name,
                 codecpar->ch_layout.nb_channels, codecpar->sample_rate,
                 stream->time_base.num, stream->time_base.den);
    } else {
        LOG_DEBUG("%s Stream #%d: Other (%s) [%s]",
                  prefix, stream_index, type_name ? type_name : "unknown", codec_name);
    }
}

typedef struct {
    bool enabled;
    char filepath[1024];
    char tmp_filepath[1050];
    int fps;
    int64_t interval_us;
    int64_t last_capture_time_us;
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
    struct SwsContext *sws_ctx;
    AVFrame *raw_frame;
    AVFrame *scaled_frame;
    AVPacket *jpeg_pkt;
    int target_width;
    int target_height;
    int quality;
    int last_src_w;
    int last_src_h;
    enum AVPixelFormat last_src_fmt;
    uint64_t frames_written;
} ConfidenceMonitor;

static void confidence_monitor_init_encoder(ConfidenceMonitor *mon, int src_width, int src_height) {
    if (!mon || mon->enc_ctx) return;

    int out_w = (mon->target_width > 0) ? mon->target_width : src_width;
    int out_h = (mon->target_height > 0) ? mon->target_height : src_height;
    out_w &= ~1;
    out_h &= ~1;
    if (out_w <= 0) out_w = 640;
    if (out_h <= 0) out_h = 360;

    const AVCodec *mjpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg) {
        LOG_WARN("[MONITOR] MJPEG encoder not found!");
        return;
    }

    mon->enc_ctx = avcodec_alloc_context3(mjpeg);
    if (!mon->enc_ctx) return;

    mon->enc_ctx->width = out_w;
    mon->enc_ctx->height = out_h;
    mon->enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    mon->enc_ctx->color_range = AVCOL_RANGE_JPEG;
    mon->enc_ctx->time_base = (AVRational){1, mon->fps};
    mon->enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
    mon->enc_ctx->global_quality = FF_QP2LAMBDA * mon->quality;

    if (avcodec_open2(mon->enc_ctx, mjpeg, NULL) < 0) {
        mon->enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        if (avcodec_open2(mon->enc_ctx, mjpeg, NULL) < 0) {
            LOG_ERROR("[MONITOR] Could not open MJPEG encoder for confidence monitor!");
            avcodec_free_context(&mon->enc_ctx);
            return;
        }
    }

    mon->scaled_frame = av_frame_alloc();
    if (mon->scaled_frame) {
        mon->scaled_frame->format = mon->enc_ctx->pix_fmt;
        mon->scaled_frame->color_range = mon->enc_ctx->color_range;
        mon->scaled_frame->width = mon->enc_ctx->width;
        mon->scaled_frame->height = mon->enc_ctx->height;
        av_frame_get_buffer(mon->scaled_frame, 32);
    }

    LOG_INFO("[MONITOR] Initialized MJPEG encoder: %dx%d @ %d fps -> %s",
             out_w, out_h, mon->fps, mon->filepath);
}

static void confidence_monitor_init(ConfidenceMonitor *mon, const PipelineConfig *config,
                                    AVFormatContext *ifmt_ctx, int video_stream_idx, bool is_sdi) {
    if (!mon || !config || config->monitor_file[0] == '\0') return;

    memset(mon, 0, sizeof(*mon));
    mon->enabled = true;
    snprintf(mon->filepath, sizeof(mon->filepath), "%s", config->monitor_file);
    snprintf(mon->tmp_filepath, sizeof(mon->tmp_filepath), "%s.tmp", config->monitor_file);
    create_parent_dir_if_local(mon->filepath);

    mon->fps = config->monitor_fps > 0 ? config->monitor_fps : 5;
    mon->interval_us = 1000000LL / mon->fps;
    mon->quality = (config->monitor_quality >= 1 && config->monitor_quality <= 31) ? config->monitor_quality : 4;
    mon->target_width = config->monitor_width;
    mon->target_height = config->monitor_height;

    if (video_stream_idx < 0 || !ifmt_ctx || video_stream_idx >= (int)ifmt_ctx->nb_streams) {
        LOG_WARN("[MONITOR] Video stream index invalid, confidence monitor disabled");
        mon->enabled = false;
        return;
    }

    AVStream *vstream = ifmt_ctx->streams[video_stream_idx];

    mon->jpeg_pkt = av_packet_alloc();
    if (!mon->jpeg_pkt) {
        LOG_WARN("[MONITOR] Failed to allocate packet for confidence monitor");
        mon->enabled = false;
        return;
    }

    /* For SDI input, frames arrive already decoded as raw baseband video.
     * We don't need a video decoder; we only need the MJPEG encoder! */
    if (!is_sdi) {
        const AVCodec *vdec = avcodec_find_decoder(vstream->codecpar->codec_id);
        if (!vdec) {
            LOG_WARN("[MONITOR] No decoder found for video codec %s", avcodec_get_name(vstream->codecpar->codec_id));
            mon->enabled = false;
            return;
        }

        mon->dec_ctx = avcodec_alloc_context3(vdec);
        if (!mon->dec_ctx) {
            mon->enabled = false;
            return;
        }

        if (avcodec_parameters_to_context(mon->dec_ctx, vstream->codecpar) < 0) {
            LOG_WARN("[MONITOR] Failed to copy codec parameters to decoder context");
            avcodec_free_context(&mon->dec_ctx);
            mon->enabled = false;
            return;
        }

        mon->dec_ctx->thread_count = 2;
        if (avcodec_open2(mon->dec_ctx, vdec, NULL) < 0) {
            LOG_WARN("[MONITOR] Failed to open video decoder for confidence monitor");
            avcodec_free_context(&mon->dec_ctx);
            mon->enabled = false;
            return;
        }

        mon->raw_frame = av_frame_alloc();
        if (!mon->raw_frame) {
            mon->enabled = false;
            return;
        }
    }

    if (vstream->codecpar->width > 0 && vstream->codecpar->height > 0) {
        confidence_monitor_init_encoder(mon, vstream->codecpar->width, vstream->codecpar->height);
    }

    LOG_INFO("[MONITOR] Confidence monitor active (%s source): %s (Target: %d fps, Q: %d)",
             is_sdi ? "SDI Baseband" : "IP Demux", mon->filepath, mon->fps, mon->quality);
}

static void confidence_monitor_process_frame(ConfidenceMonitor *mon, const AVFrame *raw_frame, PipelineStats *stats) {
    if (!mon || !mon->enabled || !raw_frame) return;

    int64_t now_us = get_time_us();
    if (now_us - mon->last_capture_time_us < mon->interval_us) {
        return;
    }
    mon->last_capture_time_us = now_us;

    if (!mon->enc_ctx) {
        confidence_monitor_init_encoder(mon, raw_frame->width, raw_frame->height);
    }
    if (!mon->enc_ctx || !mon->scaled_frame) return;

    if (mon->sws_ctx && (mon->last_src_w != raw_frame->width ||
                         mon->last_src_h != raw_frame->height ||
                         mon->last_src_fmt != raw_frame->format)) {
        sws_freeContext(mon->sws_ctx);
        mon->sws_ctx = NULL;
    }
    if (!mon->sws_ctx) {
        mon->last_src_w = raw_frame->width;
        mon->last_src_h = raw_frame->height;
        mon->last_src_fmt = raw_frame->format;
        mon->sws_ctx = sws_getContext(
            raw_frame->width, raw_frame->height, (enum AVPixelFormat)raw_frame->format,
            mon->enc_ctx->width, mon->enc_ctx->height, mon->enc_ctx->pix_fmt,
            SWS_BILINEAR, NULL, NULL, NULL);
    }

    if (mon->sws_ctx) {
        sws_scale(mon->sws_ctx,
                  (const uint8_t * const *)raw_frame->data, raw_frame->linesize,
                  0, raw_frame->height,
                  mon->scaled_frame->data, mon->scaled_frame->linesize);
        mon->scaled_frame->pts = mon->frames_written;

        int ret = avcodec_send_frame(mon->enc_ctx, mon->scaled_frame);
        if (ret >= 0) {
            while (avcodec_receive_packet(mon->enc_ctx, mon->jpeg_pkt) == 0) {
                FILE *fp = fopen(mon->tmp_filepath, "wb");
                if (fp) {
                    fwrite(mon->jpeg_pkt->data, 1, mon->jpeg_pkt->size, fp);
                    fclose(fp);
                    rename(mon->tmp_filepath, mon->filepath);
                    mon->frames_written++;
                    if (stats) {
                        stats->monitor_frames_written = mon->frames_written;
                        stats->last_monitor_write_time_us = now_us;
                    }
                }
                av_packet_unref(mon->jpeg_pkt);
            }
        }
    }
}

static void confidence_monitor_process(ConfidenceMonitor *mon, const AVPacket *pkt, PipelineStats *stats) {
    if (!mon || !mon->enabled || !mon->dec_ctx) return;

    int ret = avcodec_send_packet(mon->dec_ctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return;
    }

    while (avcodec_receive_frame(mon->dec_ctx, mon->raw_frame) == 0) {
        confidence_monitor_process_frame(mon, mon->raw_frame, stats);
    }
}

static void confidence_monitor_cleanup(ConfidenceMonitor *mon) {
    if (!mon || !mon->enabled) return;
    if (mon->raw_frame) av_frame_free(&mon->raw_frame);
    if (mon->scaled_frame) av_frame_free(&mon->scaled_frame);
    if (mon->jpeg_pkt) av_packet_free(&mon->jpeg_pkt);
    if (mon->sws_ctx) {
        sws_freeContext(mon->sws_ctx);
        mon->sws_ctx = NULL;
    }
    if (mon->dec_ctx) avcodec_free_context(&mon->dec_ctx);
    if (mon->enc_ctx) avcodec_free_context(&mon->enc_ctx);
    mon->enabled = false;
}

int pipeline_run(const PipelineConfig *config) {
    int ret = 0;
    AVFormatContext *ifmt_ctx = NULL;
    AVPacket *pkt = NULL;
    ConfidenceMonitor monitor;
    memset(&monitor, 0, sizeof(monitor));

    OutputContext out_ctxs[MAX_OUTPUTS];
    memset(out_ctxs, 0, sizeof(out_ctxs));
    int num_outputs = 0;
    int num_rtmp_outputs = 0;
    int num_hls_outputs = 0;
    int num_srt_outputs = 0;
    int num_udp_outputs = 0;
    int num_rtp_outputs = 0;

    int *stream_mapping = NULL;
    int nb_mapped_streams = 0;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};

    AVFrame *dec_frame = NULL;
    AVPacket *enc_pkt = NULL;
    AVFrame *dec_aframe = NULL;
    AVPacket *enc_a_pkt = NULL;
    SdiInputContext *sdi_in = NULL;
    TranscodeEngine *transcoder = NULL;
    AudioTranscodeEngine *a_transcoder = NULL;
    int num_sdi_outputs = 0;
    int srt_in_sock = -1;
    int sdi_w = 1920, sdi_h = 1080;
    AVRational sdi_fps = (AVRational){60000, 1001};

    InterruptContext interrupt_ctx = {
        .last_packet_time_us = 0,
        .timeout_sec = config->timeout_sec
    };

    s_stop_requested = 0;
    s_udp_lost_packets = 0;
    s_udp_cc_errors = 0;
    av_log_set_callback(ffmpeg_log_callback);

    LOG_INFO("Initializing FFmpeg network subsystems...");
    if ((ret = avformat_network_init()) < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("Failed to initialize network: %s", err_buf);
        return ret;
    }

    bool is_srt_input = (strncmp(config->input_url, "srt://", 6) == 0);
    bool is_sdi_input = (strncmp(config->input_url, "sdi:", 4) == 0);
    bool is_rtp_input = (strncmp(config->input_url, "rtp://", 6) == 0 ||
                         (strlen(config->input_url) > 4 && strcmp(config->input_url + strlen(config->input_url) - 4, ".sdp") == 0));

    if (is_sdi_input) {
        const char *sdi_dev = config->input_url + 4;
        LOG_INFO("[SDI Ingress] Opening Blackmagic DeckLink port: '%s' (Mode: %s, %d ch audio)...",
                 sdi_dev, config->sdi_mode, config->sdi_channels);
        sdi_in = sdi_input_open(sdi_dev, config->sdi_mode, config->sdi_channels);
        if (!sdi_in) {
            LOG_ERROR("[SDI Ingress] Failed to open DeckLink input port '%s'", sdi_dev);
            ret = AVERROR(ENODEV);
            goto cleanup;
        }
        sdi_input_set_test_bars(sdi_in, config->sdi_test_bars);
        sdi_input_set_sync_type(sdi_in, (config->sync_type == SYNC_TYPE_SELF) ? SDI_SYNC_SELF : SDI_SYNC_HARDWARE);
        if (sdi_input_start(sdi_in) < 0) {
            LOG_ERROR("[SDI Ingress] Failed to start DeckLink hardware capture!");
            ret = AVERROR_EXTERNAL;
            goto cleanup;
        }
        /* Always wait up to 1500ms for hardware carrier lock & format auto-detection (regardless of initial mode hint) */
        LOG_INFO("[SDI Ingress] Waiting for carrier lock and hardware format detection (up to 1500ms)...");
        if (sdi_input_wait_for_format(sdi_in, 1500) == 0) {
            LOG_INFO("[SDI Ingress] Hardware carrier signal locked successfully.");
        } else {
            LOG_WARN("[SDI Ingress] Signal lock wait timed out; continuing with default mode or test bars.");
        }
        sdi_input_get_format(sdi_in, &sdi_w, &sdi_h, &sdi_fps);

        ifmt_ctx = avformat_alloc_context();
        if (!ifmt_ctx) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }
        AVStream *vs = avformat_new_stream(ifmt_ctx, NULL);
        if (!vs) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }
        vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vs->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        vs->codecpar->format = AV_PIX_FMT_UYVY422;
        vs->codecpar->width = sdi_w;
        vs->codecpar->height = sdi_h;
        vs->time_base = (AVRational){sdi_fps.den, sdi_fps.num};
        vs->r_frame_rate = sdi_fps;
        video_stream_idx = 0;
        LOG_INFO("[SDI Ingress] Native Blackmagic SDI source locked: %dx%d @ %.2f fps",
                 sdi_w, sdi_h, (double)sdi_fps.num / sdi_fps.den);

        if (config->sdi_channels > 0) {
            AVStream *as = avformat_new_stream(ifmt_ctx, NULL);
            if (!as) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            as->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
            as->codecpar->format = AV_SAMPLE_FMT_S16;
            as->codecpar->sample_rate = 48000;
            av_channel_layout_default(&as->codecpar->ch_layout, config->sdi_channels > 0 ? config->sdi_channels : 2);
            as->time_base = (AVRational){1, 48000};
            audio_stream_idx = 1;
            LOG_INFO("[SDI Ingress] Native Blackmagic SDI audio stream registered: 48kHz %d ch PCM S16LE",
                     as->codecpar->ch_layout.nb_channels);
        }
    } else {
        /* 1. Allocate input context with interrupt callback */
        ifmt_ctx = avformat_alloc_context();
        if (!ifmt_ctx) {
            LOG_ERROR("Could not allocate input AVFormatContext");
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        ifmt_ctx->interrupt_callback.callback = interrupt_cb;
        ifmt_ctx->interrupt_callback.opaque = &interrupt_ctx;
        interrupt_ctx.last_packet_time_us = av_gettime_relative();

        /* 2. Configure Input Options (SRT, RTP, or UDP Multicast) */
        AVDictionary *input_opts = NULL;
        char opt_str[64];

        if (is_srt_input) {
            LOG_INFO("[SRT STATUS] Connecting / Listening on SRT input: %s ...", config->input_url);
            srt_setup_ingress_options(&input_opts, config);
        } else if (is_rtp_input) {
            snprintf(opt_str, sizeof(opt_str), "%d", config->fifo_size);
            av_dict_set(&input_opts, "fifo_size", opt_str, 0);

            snprintf(opt_str, sizeof(opt_str), "%d", config->buffer_size);
            av_dict_set(&input_opts, "buffer_size", opt_str, 0);

            snprintf(opt_str, sizeof(opt_str), "%d", config->overrun_nonfatal);
            av_dict_set(&input_opts, "overrun_nonfatal", opt_str, 0);

            if (config->timeout_sec > 0) {
                snprintf(opt_str, sizeof(opt_str), "%d", config->timeout_sec * 1000000);
                av_dict_set(&input_opts, "timeout", opt_str, 0);
            }

            if (strlen(config->local_addr) > 0) {
                av_dict_set(&input_opts, "localaddr", config->local_addr, 0);
                LOG_INFO("RTP multicast bind interface: %s", config->local_addr);
            }

            /* RTP demuxer packet reorder queue to absorb network jitter */
            av_dict_set(&input_opts, "reorder_queue_size", "500", 0);

            LOG_INFO("[RTP STATUS] Binding RTP receiver: %s ...", config->input_url);
        } else {
            snprintf(opt_str, sizeof(opt_str), "%d", config->fifo_size);
            av_dict_set(&input_opts, "fifo_size", opt_str, 0);

            snprintf(opt_str, sizeof(opt_str), "%d", config->buffer_size);
            av_dict_set(&input_opts, "buffer_size", opt_str, 0);

            snprintf(opt_str, sizeof(opt_str), "%d", config->overrun_nonfatal);
            av_dict_set(&input_opts, "overrun_nonfatal", opt_str, 0);

            if (config->timeout_sec > 0) {
                snprintf(opt_str, sizeof(opt_str), "%d", config->timeout_sec * 1000000);
                av_dict_set(&input_opts, "timeout", opt_str, 0);
            }

            if (strlen(config->local_addr) > 0) {
                av_dict_set(&input_opts, "localaddr", config->local_addr, 0);
                LOG_INFO("Multicast bind interface: %s", config->local_addr);
            }

            LOG_INFO("[UDP STATUS] Joining multicast group: %s ...", config->input_url);
        }

        int64_t input_conn_start_us = get_time_us();
        ret = avformat_open_input(&ifmt_ctx, config->input_url, NULL, &input_opts);
        av_dict_free(&input_opts);
        int64_t input_conn_time_ms = (get_time_us() - input_conn_start_us) / 1000;

        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            if (is_srt_input) {
                LOG_ERROR("[SRT STATUS] Failed to bind/connect SRT input '%s': %s", config->input_url, err_buf);
            } else if (is_rtp_input) {
                LOG_ERROR("[RTP STATUS] Failed to bind/open RTP input '%s': %s", config->input_url, err_buf);
                LOG_ERROR("[RTP DIAGNOSTIC] Check if RTP port is held or if multicast IGMP join is allowed.");
            } else {
                LOG_ERROR("[UDP STATUS] Failed to bind/open multicast input '%s': %s", config->input_url, err_buf);
                LOG_ERROR("[UDP DIAGNOSTIC] Check if IGMP join is allowed or if another process is holding port.");
            }
            goto cleanup;
        }
        if (is_srt_input) {
            if (ifmt_ctx && ifmt_ctx->pb) {
                srt_in_sock = srt_extract_socket(ifmt_ctx->pb);
            }
            if (srt_in_sock > 0) {
                LOG_INFO("[SRT Ingress] SRT socket (fd=%d) active and receiving in %lld ms. RTT, NAK, Loss & ARQ tracking engaged.",
                         srt_in_sock, (long long)input_conn_time_ms);
            } else {
                LOG_INFO("[SRT STATUS] SRT socket active and receiving in %lld ms.", (long long)input_conn_time_ms);
            }
        } else if (is_rtp_input) {
            LOG_INFO("[RTP STATUS] RTP receiver bound and active in %lld ms (Jitter buffer: 500 pkts).", (long long)input_conn_time_ms);
        } else {
            LOG_INFO("[UDP STATUS] Multicast socket bound and active in %lld ms.", (long long)input_conn_time_ms);
        }

        /* 3. Probe stream information */
        LOG_INFO("Probing input stream information...");
        interrupt_ctx.last_packet_time_us = av_gettime_relative();
        ret = avformat_find_stream_info(ifmt_ctx, NULL);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("Failed to retrieve input stream info: %s", err_buf);
            goto cleanup;
        }
    }

    /* 4. Determine Active Output Targets */
    OutputTarget targets[MAX_OUTPUTS];
    int num_targets = 0;
    for (int i = 0; i < config->num_outputs && num_targets < MAX_OUTPUTS; i++) {
        targets[num_targets++] = config->outputs[i];
    }
    if (num_targets == 0) {
        if (config->output_url[0] != '\0' && strcmp(config->output_url, "none") != 0) {
            targets[num_targets].type = OUTPUT_TYPE_RTMP;
            snprintf(targets[num_targets].url, sizeof(targets[num_targets].url), "%s", config->output_url);
            num_targets++;
        }
        if (config->hls_url[0] != '\0' && num_targets < MAX_OUTPUTS) {
            targets[num_targets].type = OUTPUT_TYPE_HLS;
            snprintf(targets[num_targets].url, sizeof(targets[num_targets].url), "%s", config->hls_url);
            num_targets++;
        }
        if (config->srt_url[0] != '\0' && num_targets < MAX_OUTPUTS) {
            targets[num_targets].type = OUTPUT_TYPE_SRT;
            snprintf(targets[num_targets].url, sizeof(targets[num_targets].url), "%s", config->srt_url);
            num_targets++;
        }
        if (config->rtp_url[0] != '\0' && num_targets < MAX_OUTPUTS) {
            targets[num_targets].type = OUTPUT_TYPE_RTP;
            snprintf(targets[num_targets].url, sizeof(targets[num_targets].url), "%s", config->rtp_url);
            num_targets++;
        }
    }
    if (num_targets == 0) {
        LOG_ERROR("No output destination configured! Specify -o rtmp <url>, -o hls <url>, -o srt <url>, -o sdi <dev>, -o udp <url>, or -o rtp <url>");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    num_outputs = num_targets;

    int rtmp_counter = 0;
    int hls_counter = 0;
    int srt_counter = 0;
    int sdi_counter = 0;
    int udp_counter = 0;
    int rtp_counter = 0;
    num_rtp_outputs = 0;
    for (int i = 0; i < num_outputs; i++) {
        out_ctxs[i].type = targets[i].type;
        strncpy(out_ctxs[i].url, targets[i].url, sizeof(out_ctxs[i].url) - 1);
        out_ctxs[i].index = i;
        out_ctxs[i].tcp_fd = -1;
        out_ctxs[i].write_latency_min_us = INT64_MAX;
        for (int s = 0; s < MAX_STREAMS; s++) {
            out_ctxs[i].stream_last_dts[s] = AV_NOPTS_VALUE;
        }

        if (targets[i].type == OUTPUT_TYPE_RTMP) {
            num_rtmp_outputs++;
            out_ctxs[i].type_index = ++rtmp_counter;
            LOG_INFO("Preparing RTMP output target [%d]: %s", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avformat_alloc_output_context2(&out_ctxs[i].ofmt_ctx, NULL, "flv", out_ctxs[i].url);
            if (!out_ctxs[i].ofmt_ctx) {
                LOG_ERROR("Could not allocate output format context for 'flv': %d", ret);
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = output_interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = NULL;
        } else if (targets[i].type == OUTPUT_TYPE_HLS) {
            num_hls_outputs++;
            out_ctxs[i].type_index = ++hls_counter;
            create_parent_dir_if_local(out_ctxs[i].url);
            LOG_INFO("Preparing HLS output target [%d]: %s (Method: %s)",
                     out_ctxs[i].type_index, out_ctxs[i].url, config->hls_method);
            ret = avformat_alloc_output_context2(&out_ctxs[i].ofmt_ctx, NULL, "hls", out_ctxs[i].url);
            if (!out_ctxs[i].ofmt_ctx) {
                LOG_ERROR("Could not allocate output format context for 'hls': %d", ret);
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = output_interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = NULL;

            hls_init_telemetry_state(&out_ctxs[i].hls_state, targets[i].url, config);
            hls_install_custom_io(&out_ctxs[i]);
        } else if (targets[i].type == OUTPUT_TYPE_SRT) {
            num_srt_outputs++;
            out_ctxs[i].type_index = ++srt_counter;
            LOG_INFO("Preparing SRT output target [%d]: %s", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avformat_alloc_output_context2(&out_ctxs[i].ofmt_ctx, NULL, "mpegts", out_ctxs[i].url);
            if (!out_ctxs[i].ofmt_ctx) {
                LOG_ERROR("Could not allocate output format context for 'mpegts': %d", ret);
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = output_interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = NULL;
        } else if (targets[i].type == OUTPUT_TYPE_UDP) {
            num_udp_outputs++;
            out_ctxs[i].type_index = ++udp_counter;
            LOG_INFO("Preparing UDP Multicast output target [%d]: %s", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avformat_alloc_output_context2(&out_ctxs[i].ofmt_ctx, NULL, "mpegts", out_ctxs[i].url);
            if (!out_ctxs[i].ofmt_ctx) {
                LOG_ERROR("Could not allocate output format context for 'mpegts': %d", ret);
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = output_interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = NULL;
        } else if (targets[i].type == OUTPUT_TYPE_RTP) {
            num_rtp_outputs++;
            out_ctxs[i].type_index = ++rtp_counter;
            LOG_INFO("Preparing RTP output target [%d]: %s", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avformat_alloc_output_context2(&out_ctxs[i].ofmt_ctx, NULL, "rtp_mpegts", out_ctxs[i].url);
            if (!out_ctxs[i].ofmt_ctx) {
                LOG_ERROR("Could not allocate output format context for 'rtp_mpegts': %d", ret);
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = output_interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = NULL;
        } else if (targets[i].type == OUTPUT_TYPE_SDI) {
            num_sdi_outputs++;
            out_ctxs[i].type_index = ++sdi_counter;
            const char *sdi_dev = targets[i].url;
            if (strncmp(sdi_dev, "sdi:", 4) == 0) sdi_dev += 4;
            int out_w = 1920, out_h = 1080;
            AVRational out_fps = (AVRational){60000, 1001};
            if (video_stream_idx >= 0 && video_stream_idx < (int)ifmt_ctx->nb_streams) {
                out_w = ifmt_ctx->streams[video_stream_idx]->codecpar->width;
                out_h = ifmt_ctx->streams[video_stream_idx]->codecpar->height;
                if (ifmt_ctx->streams[video_stream_idx]->r_frame_rate.num > 0) {
                    out_fps = ifmt_ctx->streams[video_stream_idx]->r_frame_rate;
                }
            }
            LOG_INFO("Preparing Blackmagic SDI Playout target [%d]: %s (%dx%d)",
                     out_ctxs[i].type_index, sdi_dev, out_w, out_h);
            out_ctxs[i].sdi_out = sdi_output_open(sdi_dev, out_w, out_h, out_fps, 2);
            if (!out_ctxs[i].sdi_out) {
                LOG_ERROR("[SDI Egress] Failed to open DeckLink playout device '%s'", sdi_dev);
                ret = AVERROR(ENODEV);
                goto cleanup;
            }
        }
    }

    /* 5. Build Stream Mappings & Validate Codecs (Zero-Encoding) */
    stream_mapping = av_malloc_array(ifmt_ctx->nb_streams, sizeof(*stream_mapping));
    if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        log_stream_info(in_stream, i, "[Input]");

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            stream_mapping[i] = -1;
            LOG_DEBUG("Skipping non-audio/video stream #%d", i);
            continue;
        }

        /* Verify standard RTMP/FLV codec compatibility if RTMP active */
        if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (num_rtmp_outputs > 0 && in_codecpar->codec_id != AV_CODEC_ID_H264 &&
                in_codecpar->codec_id != AV_CODEC_ID_HEVC) {
                const AVCodecDescriptor *d = avcodec_descriptor_get(in_codecpar->codec_id);
                LOG_WARN("Video codec '%s' may not be supported by standard FLV/RTMP without transcoding!",
                         d ? d->name : "unknown");
            }
            if (video_stream_idx < 0) {
                video_stream_idx = i;
            }
        } else if (in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (audio_stream_idx < 0) {
                audio_stream_idx = i;
            }
            if (num_rtmp_outputs > 0 && in_codecpar->codec_id != AV_CODEC_ID_AAC &&
                in_codecpar->codec_id != AV_CODEC_ID_MP3) {
                const AVCodecDescriptor *d = avcodec_descriptor_get(in_codecpar->codec_id);
                LOG_WARN("Audio codec '%s' may not be supported by standard FLV/RTMP without transcoding!",
                         d ? d->name : "unknown");
            }
        }

        /* Initialize TranscodeEngine if video transcode requested, input is raw SDI, or output has SDI */
        TranscodeConfig tc = config->transcode;
        if (i == (unsigned int)video_stream_idx && (tc.vcodec != TRANSCODE_CODEC_COPY || is_sdi_input || num_sdi_outputs > 0)) {
            if ((is_sdi_input || num_sdi_outputs > 0) && tc.vcodec == TRANSCODE_CODEC_COPY) {
                tc.vcodec = TRANSCODE_CODEC_H264;
                strncpy(tc.vcodec_name, "h264", sizeof(tc.vcodec_name) - 1);
            }
            if (is_sdi_input) {
                if (tc.framerate.num <= 0 || tc.framerate.den <= 0) {
                    if (sdi_fps.num > 0 && sdi_fps.den > 0) {
                        tc.framerate = sdi_fps;
                    }
                }
                if (sdi_w > 0 && sdi_h > 0) {
                    if (tc.width <= 0) tc.width = sdi_w;
                    if (tc.height <= 0) tc.height = sdi_h;
                }
            } else if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (in_stream->r_frame_rate.num > 0 && in_stream->r_frame_rate.den > 0) {
                    tc.framerate = in_stream->r_frame_rate;
                } else if (in_stream->avg_frame_rate.num > 0 && in_stream->avg_frame_rate.den > 0) {
                    tc.framerate = in_stream->avg_frame_rate;
                }
                if (in_codecpar->width > 0 && in_codecpar->height > 0) {
                    if (tc.width <= 0) tc.width = in_codecpar->width;
                    if (tc.height <= 0) tc.height = in_codecpar->height;
                }
            }
            transcoder = transcode_engine_init(&tc, in_codecpar, in_stream->time_base);
            if (!transcoder) {
                LOG_ERROR("[TRANSCODE] Failed to initialize TranscodeEngine!");
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
        }

        /* Initialize AudioTranscodeEngine if audio transcode requested, input is raw SDI, or output has SDI */
        AudioTranscodeConfig atc = config->audio_transcode;
        if (i == (unsigned int)audio_stream_idx && (atc.acodec != TRANSCODE_AUDIO_COPY || is_sdi_input || num_sdi_outputs > 0)) {
            if ((is_sdi_input || num_sdi_outputs > 0) && atc.acodec == TRANSCODE_AUDIO_COPY) {
                atc.acodec = TRANSCODE_AUDIO_AAC;
                strncpy(atc.acodec_name, "aac", sizeof(atc.acodec_name) - 1);
            }
            a_transcoder = audio_transcode_engine_init(&atc, in_codecpar, in_stream->time_base);
            if (!a_transcoder) {
                LOG_ERROR("[AUDIO TRANSCODE] Failed to initialize AudioTranscodeEngine!");
                ret = AVERROR_UNKNOWN;
                goto cleanup;
            }
        }

        for (int j = 0; j < num_outputs; j++) {
            if (out_ctxs[j].type == OUTPUT_TYPE_SDI) {
                continue; /* SDI uses native baseband playout, no ofmt_ctx needed */
            }
            AVStream *out_stream = avformat_new_stream(out_ctxs[j].ofmt_ctx, NULL);
            if (!out_stream) {
                LOG_ERROR("Failed to allocate output stream for output #%d input #%d", j, i);
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO && transcoder != NULL) {
                ret = transcode_engine_get_codecpar(transcoder, out_stream->codecpar);
                out_stream->time_base = transcode_engine_get_timebase(transcoder);
            } else if (in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO && a_transcoder != NULL) {
                ret = audio_transcode_engine_get_codecpar(a_transcoder, out_stream->codecpar);
                out_stream->time_base = audio_transcode_engine_get_timebase(a_transcoder);
            } else {
                ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
                out_stream->time_base = in_stream->time_base;
            }
            if (ret < 0) {
                LOG_ERROR("Failed to copy codec parameters for stream #%d", i);
                goto cleanup;
            }
            out_stream->codecpar->codec_tag = 0;
            char prefix[64];
            const char *out_type_str = (out_ctxs[j].type == OUTPUT_TYPE_RTMP) ? "RTMP" :
                                       ((out_ctxs[j].type == OUTPUT_TYPE_HLS) ? "HLS" :
                                       ((out_ctxs[j].type == OUTPUT_TYPE_SRT) ? "SRT" :
                                       ((out_ctxs[j].type == OUTPUT_TYPE_UDP) ? "UDP" :
                                       ((out_ctxs[j].type == OUTPUT_TYPE_RTP) ? "RTP" : "SDI"))));
            snprintf(prefix, sizeof(prefix), "[%s #%d Output]", out_type_str, out_ctxs[j].type_index);
            log_stream_info(out_stream, out_stream->index, prefix);
        }

        stream_mapping[i] = nb_mapped_streams++;
    }

    if (transcoder && a_transcoder) {
        int64_t v_pts_offset = transcode_engine_get_pts_offset(transcoder);
        if (v_pts_offset > 0) {
            AVRational v_tb = transcode_engine_get_timebase(transcoder);
            AVRational a_tb = audio_transcode_engine_get_timebase(a_transcoder);
            int64_t a_offset_samples = av_rescale_q(v_pts_offset, v_tb, a_tb);
            audio_transcode_engine_set_pts_offset(a_transcoder, a_offset_samples);
            LOG_INFO("[SYNC] Synchronized audio/video start timestamps (Video offset: +%lld frames, Audio offset: +%lld samples)",
                     (long long)v_pts_offset, (long long)a_offset_samples);
        }
    }

    if (nb_mapped_streams == 0) {
        LOG_ERROR("No audio or video streams found in input source!");
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }

    /* Initialize Confidence Monitor (if configured) */
    if (config->monitor_file[0] != '\0') {
        confidence_monitor_init(&monitor, config, ifmt_ctx, video_stream_idx, is_sdi_input);
    }

    /* 6. Open Output Connections */
    int established_tcp_fds[MAX_OUTPUTS];
    int num_established_tcp = 0;

    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].type == OUTPUT_TYPE_SDI) {
            continue; /* Already opened above via sdi_output_open */
        }
        if (out_ctxs[i].type == OUTPUT_TYPE_RTMP) {
            ret = rtmp_open_egress(&out_ctxs[i], established_tcp_fds, &num_established_tcp,
                                   &interrupt_ctx.last_packet_time_us);
            if (ret < 0) {
                goto cleanup;
            }
        } else if (out_ctxs[i].type == OUTPUT_TYPE_HLS) {
            ret = hls_open_egress(&out_ctxs[i], config, &interrupt_ctx.last_packet_time_us);
            if (ret < 0) {
                goto cleanup;
            }
            if (out_ctxs[i].tcp_fd >= 0 && num_established_tcp < MAX_OUTPUTS) {
                established_tcp_fds[num_established_tcp++] = out_ctxs[i].tcp_fd;
            }
        } else if (out_ctxs[i].type == OUTPUT_TYPE_SRT) {
            ret = srt_open_egress(&out_ctxs[i], config, &interrupt_ctx.last_packet_time_us);
            if (ret < 0) {
                if (num_outputs > 1) {
                    LOG_WARN("[SRT #%d] Failed to open SRT egress '%s' (continuing other active outputs).",
                             out_ctxs[i].type_index, out_ctxs[i].url);
                    out_ctxs[i].failed = true;
                } else {
                    goto cleanup;
                }
            }
        } else if (out_ctxs[i].type == OUTPUT_TYPE_UDP) {
            AVDictionary *udp_opts = NULL;
            av_dict_set(&udp_opts, "pkt_size", "1316", 0);
            av_dict_set(&udp_opts, "buffer_size", "6553600", 0);
            int64_t conn_start_us = get_time_us();
            LOG_INFO("[UDP #%d] Opening multicast socket: %s ...", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avio_open2(&out_ctxs[i].ofmt_ctx->pb, out_ctxs[i].url, AVIO_FLAG_WRITE,
                             &out_ctxs[i].ofmt_ctx->interrupt_callback, &udp_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[UDP #%d] Failed to open UDP destination '%s': %s",
                          out_ctxs[i].type_index, out_ctxs[i].url, err_buf);
                av_dict_free(&udp_opts);
                goto cleanup;
            }
            out_ctxs[i].conn_time_ms = (get_time_us() - conn_start_us) / 1000;
            LOG_INFO("[UDP #%d] Multicast socket active in %lld ms. Writing MPEG-TS headers...",
                     out_ctxs[i].type_index, (long long)out_ctxs[i].conn_time_ms);
            av_dict_free(&udp_opts);
            AVDictionary *ts_opts = NULL;
            av_dict_set(&ts_opts, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
            av_dict_set(&ts_opts, "pcr_period", "20", 0);
            av_dict_set(&ts_opts, "pat_period", "0.1", 0);
            av_dict_set(&ts_opts, "sdt_period", "0.25", 0);
            ret = avformat_write_header(out_ctxs[i].ofmt_ctx, &ts_opts);
            av_dict_free(&ts_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[UDP #%d] Stream header rejected: %s", out_ctxs[i].type_index, err_buf);
                goto cleanup;
            }
            LOG_INFO("[UDP #%d] Multicast publishing active. Target: %s", out_ctxs[i].type_index, out_ctxs[i].url);
        } else if (out_ctxs[i].type == OUTPUT_TYPE_RTP) {
            AVDictionary *rtp_opts = NULL;
            av_dict_set(&rtp_opts, "pkt_size", "1316", 0);
            char buf_size_str[32];
            snprintf(buf_size_str, sizeof(buf_size_str), "%d", config->buffer_size > 0 ? config->buffer_size : 6553600);
            av_dict_set(&rtp_opts, "buffer_size", buf_size_str, 0);
            if (strlen(config->local_addr) > 0) {
                av_dict_set(&rtp_opts, "localaddr", config->local_addr, 0);
            }
            int64_t conn_start_us = get_time_us();
            LOG_INFO("[RTP #%d] Opening RTP socket: %s ...", out_ctxs[i].type_index, out_ctxs[i].url);
            ret = avio_open2(&out_ctxs[i].ofmt_ctx->pb, out_ctxs[i].url, AVIO_FLAG_WRITE,
                             &out_ctxs[i].ofmt_ctx->interrupt_callback, &rtp_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[RTP #%d] Failed to open RTP destination '%s': %s",
                          out_ctxs[i].type_index, out_ctxs[i].url, err_buf);
                av_dict_free(&rtp_opts);
                goto cleanup;
            }
            out_ctxs[i].conn_time_ms = (get_time_us() - conn_start_us) / 1000;
            LOG_INFO("[RTP #%d] RTP socket active in %lld ms. Writing RFC 2250 RTP/MPEG-TS headers...",
                     out_ctxs[i].type_index, (long long)out_ctxs[i].conn_time_ms);
            av_dict_free(&rtp_opts);
            AVDictionary *ts_opts = NULL;
            av_dict_set(&ts_opts, "mpegts_muxer_options", "resend_headers=1", 0);
            ret = avformat_write_header(out_ctxs[i].ofmt_ctx, &ts_opts);
            av_dict_free(&ts_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[RTP #%d] Stream header rejected: %s", out_ctxs[i].type_index, err_buf);
                goto cleanup;
            }
            LOG_INFO("[RTP #%d] RTP publishing active (RFC 2250 MP2T). Target: %s", out_ctxs[i].type_index, out_ctxs[i].url);
        }
    }

    /* Verify at least one egress output initialized successfully */
    bool any_output_ready = false;
    for (int i = 0; i < num_outputs; i++) {
        if (!out_ctxs[i].failed) {
            any_output_ready = true;
            break;
        }
    }
    if (!any_output_ready) {
        LOG_ERROR("All configured egress targets failed to initialize!");
        ret = AVERROR(EIO);
        goto cleanup;
    }

    /* Assign socket exclusions for HLS dynamic reconnection */
    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].type == OUTPUT_TYPE_HLS) {
            int count = 0;
            for (int j = 0; j < num_outputs; j++) {
                if (j != i && out_ctxs[j].tcp_fd >= 0) {
                    out_ctxs[i].hls_state.exclude_fds[count++] = out_ctxs[j].tcp_fd;
                }
            }
            out_ctxs[i].hls_state.num_exclude_fds = count;
        }
    }

    LOG_INFO("==================================================================");
    LOG_INFO("Pipeline active: Streaming %s -> %d Active Outputs [%s]%s",
             is_sdi_input ? "Blackmagic SDI" : (is_srt_input ? "SRT" : (is_rtp_input ? "RTP" : "UDP Multicast")),
             num_outputs,
             transcoder ? "Low-Latency Transcode" : "Zero Encoding",
             monitor.enabled ? " + Confidence Monitor" : "");
    for (int i = 0; i < num_outputs; i++) {
        const char *tname = (out_ctxs[i].type == OUTPUT_TYPE_RTMP) ? "RTMP" :
                            ((out_ctxs[i].type == OUTPUT_TYPE_HLS) ? "HLS" :
                            ((out_ctxs[i].type == OUTPUT_TYPE_SRT) ? "SRT" :
                            ((out_ctxs[i].type == OUTPUT_TYPE_UDP) ? "UDP" :
                            ((out_ctxs[i].type == OUTPUT_TYPE_RTP) ? "RTP" : "SDI"))));
        LOG_INFO("  Egress #%d: [%s] %s", i + 1, tname, out_ctxs[i].url);
    }
    LOG_INFO("Telemetry active: Reporting per-egress bandwidth, packets, TCP/SRT & jitter");
    LOG_INFO("==================================================================");

    /* 7. Allocate packets and state tracking */
    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].type == OUTPUT_TYPE_SDI) continue;
        out_ctxs[i].pkt = av_packet_alloc();
        if (!out_ctxs[i].pkt) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }
    }

    PipelineStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.start_time_us = get_time_us();
    stats.last_stats_time_us = stats.start_time_us;
    stats.cur_video_pts_ms = AV_NOPTS_VALUE;
    stats.cur_audio_pts_ms = AV_NOPTS_VALUE;
    stats.last_video_dts_ms = AV_NOPTS_VALUE;
    stats.first_video_pts_ms = AV_NOPTS_VALUE;
    stats.last_stats_video_pts_ms = AV_NOPTS_VALUE;
    stats.last_keyframe_time_us = 0;
    stats.cur_speed_factor = 1.0;
    stats.cur_wall_lead_ms = 0;
    stats.rtmp_tcp_fd = (num_rtmp_outputs > 0) ? out_ctxs[0].tcp_fd : -1;
    stats.hls_tcp_fd = (num_hls_outputs > 0) ? out_ctxs[num_rtmp_outputs].tcp_fd : -1;
    stats.srt_sock = -1;
    stats.srt_in_sock = srt_in_sock;
    stats.srt_arq_success_pct = 100.0;
    stats.srt_in_arq_success_pct = 100.0;
    stats.write_latency_min_us = INT64_MAX;
    stats.write_latency_max_us = 0;
    stats.write_latency_peak_us = 0;
    strncpy(stats.youtube_health_status, "INITIALIZING", sizeof(stats.youtube_health_status) - 1);

    bool seen_first_keyframe = !config->wait_keyframe;
    int64_t stream_first_pts[MAX_STREAMS];
    int64_t stream_first_dts[MAX_STREAMS];
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_first_pts[i] = AV_NOPTS_VALUE;
        stream_first_dts[i] = AV_NOPTS_VALUE;
    }

    dec_frame = av_frame_alloc();
    enc_pkt = av_packet_alloc();
    dec_aframe = av_frame_alloc();
    enc_a_pkt = av_packet_alloc();
    if (!dec_frame || !enc_pkt || !dec_aframe || !enc_a_pkt) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    uint64_t last_sdi_frames_read = 0;
    uint64_t last_sdi_frames_written = 0;

    /* Flush pre-roll frames buffered during format negotiation/pipeline setup */
    if (is_sdi_input && sdi_in) {
        sdi_input_flush(sdi_in);
    }

    /* 8. Core Streaming / Transcoding Loop */
    while (!s_stop_requested) {
        if (is_sdi_input) {
            AVFrame *sdi_vframe = NULL;
            ret = sdi_input_read_video_frame(sdi_in, &sdi_vframe, 100);
            if (ret == AVERROR(EAGAIN)) {
                goto check_sdi_audio;
            }
            if (ret < 0) {
                if (s_stop_requested) break;
                goto check_sdi_audio;
            }
            stats.sdi_frames_read++;
            stats.last_sdi_read_time_us = get_time_us();
            interrupt_ctx.last_packet_time_us = av_gettime_relative();

            /* Playout to any SDI output targets */
            for (int j = 0; j < num_outputs; j++) {
                if (out_ctxs[j].type == OUTPUT_TYPE_SDI && out_ctxs[j].sdi_out) {
                    sdi_output_write_video_frame(out_ctxs[j].sdi_out, sdi_vframe);
                    stats.sdi_frames_written++;
                    stats.last_sdi_write_time_us = get_time_us();
                }
            }

            /* Confidence Monitor Snapshot for SDI Ingress */
            if (monitor.enabled && sdi_vframe) {
                confidence_monitor_process_frame(&monitor, sdi_vframe, &stats);
            }

            /* Low-Latency Encode SDI raw frame for all active IP outputs */
            if (transcoder) {
                transcode_engine_send_frame(transcoder, sdi_vframe);
                while (transcode_engine_receive_packet(transcoder, enc_pkt) == 0) {
                    stats.transcode_latency_us = transcode_engine_get_last_latency_us(transcoder);
                    enc_pkt->stream_index = 0;

                    AVRational enc_tb = transcode_engine_get_timebase(transcoder);
                    if (enc_pkt->pts != AV_NOPTS_VALUE) {
                        stats.cur_video_pts_ms = av_rescale_q(enc_pkt->pts, enc_tb, (AVRational){1, 1000});
                        stats.last_video_write_time_us = get_time_us();
                        if (stats.first_video_pts_ms == AV_NOPTS_VALUE) {
                            stats.first_video_pts_ms = stats.cur_video_pts_ms;
                        }
                    }

                    if (enc_pkt->flags & AV_PKT_FLAG_KEY) {
                        int64_t now_kf_us = get_time_us();
                        if (stats.last_keyframe_time_us > 0) {
                            double kf_interval = (double)(now_kf_us - stats.last_keyframe_time_us) / 1000000.0;
                            stats.last_keyframe_interval_sec = kf_interval;
                        }
                        stats.last_keyframe_time_us = now_kf_us;
                        stats.total_keyframes_seen++;
                    }

                    for (int j = 0; j < num_outputs; j++) {
                        if (out_ctxs[j].type == OUTPUT_TYPE_SDI) continue;
                        AVStream *out_stream = out_ctxs[j].ofmt_ctx->streams[0];
                        AVPacket *out_pkt = out_ctxs[j].pkt;

                        if (av_packet_ref(out_pkt, enc_pkt) < 0) continue;
                        out_pkt->stream_index = 0;
                        out_pkt->pos = -1;

                        AVRational enc_tb = transcode_engine_get_timebase(transcoder);
                        av_packet_rescale_ts(out_pkt, enc_tb, out_stream->time_base);
                        out_pkt->duration = av_rescale_q(1, enc_tb, out_stream->time_base);

                        if (out_pkt->dts < 0) {
                            out_pkt->dts = 0;
                        }
                        if (out_ctxs[j].stream_last_dts[0] != AV_NOPTS_VALUE && out_pkt->dts <= out_ctxs[j].stream_last_dts[0]) {
                            out_pkt->dts = out_ctxs[j].stream_last_dts[0] + 1;
                        }
                        if (out_pkt->pts < out_pkt->dts) {
                            out_pkt->pts = out_pkt->dts;
                        }
                        out_ctxs[j].stream_last_dts[0] = out_pkt->dts;

                        int packet_size = out_pkt->size;
                        int64_t write_start_us = get_time_us();

                        ret = av_interleaved_write_frame(out_ctxs[j].ofmt_ctx, out_pkt);
                        int64_t write_finish_us = get_time_us();
                        int64_t write_duration_us = write_finish_us - write_start_us;

                        if (ret < 0) {
                            static int64_t last_sdi_werr_us = 0;
                            int64_t now_w = get_time_us();
                            if (now_w - last_sdi_werr_us > 3000000LL) {
                                char err_str[AV_ERROR_MAX_STRING_SIZE] = {0};
                                av_strerror(ret, err_str, sizeof(err_str));
                                LOG_WARN("[Output #%d] av_interleaved_write_frame failed: %s",
                                         out_ctxs[j].type_index, err_str);
                                last_sdi_werr_us = now_w;
                            }
                        }

                        out_ctxs[j].write_latency_sum_us += write_duration_us;
                        out_ctxs[j].write_latency_count++;
                        if (write_duration_us < out_ctxs[j].write_latency_min_us)
                            out_ctxs[j].write_latency_min_us = write_duration_us;
                        if (write_duration_us > out_ctxs[j].write_latency_max_us)
                            out_ctxs[j].write_latency_max_us = write_duration_us;
                        if (write_duration_us > out_ctxs[j].write_latency_peak_us)
                            out_ctxs[j].write_latency_peak_us = write_duration_us;

                        out_ctxs[j].packets_written++;
                        out_ctxs[j].video_packets_written++;
                        out_ctxs[j].bytes_written += packet_size;
                        out_ctxs[j].last_write_time_us = write_finish_us;
                        out_ctxs[j].last_video_write_time_us = write_finish_us;

                        if (j == 0) {
                            stats.last_video_write_time_us = write_finish_us;
                        }
                        av_packet_unref(out_pkt);
                    }
                    av_packet_unref(enc_pkt);
                }
            }
            av_frame_free(&sdi_vframe);

check_sdi_audio:
            /* Drain & encode SDI Audio frames */
            AVFrame *sdi_aframe = NULL;
            while (sdi_input_read_audio_frame(sdi_in, &sdi_aframe, 0) == 0) {
                interrupt_ctx.last_packet_time_us = av_gettime_relative();

                /* 1. Playout to any SDI output targets */
                for (int j = 0; j < num_outputs; j++) {
                    if (out_ctxs[j].type == OUTPUT_TYPE_SDI && out_ctxs[j].sdi_out) {
                        sdi_output_write_audio_frame(out_ctxs[j].sdi_out, sdi_aframe);
                    }
                }

                /* 2. Audio Encode for IP outputs */
                if (a_transcoder) {
                    audio_transcode_engine_send_frame(a_transcoder, sdi_aframe);
                    while (audio_transcode_engine_receive_packet(a_transcoder, enc_a_pkt) == 0) {
                        int out_a_idx = (audio_stream_idx >= 0 && audio_stream_idx < MAX_STREAMS) ?
                                        stream_mapping[audio_stream_idx] : 1;
                        enc_a_pkt->stream_index = out_a_idx;

                        AVRational a_enc_tb = audio_transcode_engine_get_timebase(a_transcoder);
                        if (enc_a_pkt->pts != AV_NOPTS_VALUE) {
                            stats.cur_audio_pts_ms = av_rescale_q(enc_a_pkt->pts, a_enc_tb, (AVRational){1, 1000});
                        }

                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type == OUTPUT_TYPE_SDI) continue;
                            if (out_a_idx < 0 || out_a_idx >= (int)out_ctxs[j].ofmt_ctx->nb_streams) continue;
                            AVStream *out_stream = out_ctxs[j].ofmt_ctx->streams[out_a_idx];
                            AVPacket *out_pkt = out_ctxs[j].pkt;

                            if (av_packet_ref(out_pkt, enc_a_pkt) < 0) continue;
                            out_pkt->stream_index = out_a_idx;
                            out_pkt->pos = -1;

                            AVRational enc_tb = audio_transcode_engine_get_timebase(a_transcoder);
                            av_packet_rescale_ts(out_pkt, enc_tb, out_stream->time_base);
                            out_pkt->duration = av_rescale_q(1024, enc_tb, out_stream->time_base);

                            if (out_pkt->dts < 0) {
                                out_pkt->dts = 0;
                            }
                            if (out_ctxs[j].stream_last_dts[out_a_idx] != AV_NOPTS_VALUE &&
                                out_pkt->dts <= out_ctxs[j].stream_last_dts[out_a_idx]) {
                                out_pkt->dts = out_ctxs[j].stream_last_dts[out_a_idx] + 1;
                            }
                            if (out_pkt->pts < out_pkt->dts) {
                                out_pkt->pts = out_pkt->dts;
                            }
                            out_ctxs[j].stream_last_dts[out_a_idx] = out_pkt->dts;

                            int a_size = out_pkt->size;
                            int a_ret = av_interleaved_write_frame(out_ctxs[j].ofmt_ctx, out_pkt);
                            if (a_ret == 0) {
                                out_ctxs[j].packets_written++;
                                out_ctxs[j].audio_packets_written++;
                                out_ctxs[j].bytes_written += a_size;
                                out_ctxs[j].last_audio_write_time_us = get_time_us();
                            }
                            av_packet_unref(out_pkt);
                        }
                        av_packet_unref(enc_a_pkt);
                    }
                }
                av_frame_free(&sdi_aframe);
            }
            goto check_telemetry;
        }

        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("[STREAM END] End of input stream reached (EOF).");
                ret = 0;
            } else if (s_stop_requested) {
                LOG_INFO("[STREAM STOP] Stop requested by user signal.");
                ret = 0;
            } else {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[STREAM ERROR] Input read error: %s", err_buf);
            }
            break;
        }

        interrupt_ctx.last_packet_time_us = av_gettime_relative();
        stats.udp_packets_read++;
        stats.udp_bytes_read += pkt->size;

        int in_idx = pkt->stream_index;
        if (in_idx < 0 || in_idx >= (int)ifmt_ctx->nb_streams || stream_mapping[in_idx] < 0) {
            av_packet_unref(pkt);
            goto check_telemetry;
        }

        int out_idx = stream_mapping[in_idx];
        AVStream *in_stream = ifmt_ctx->streams[in_idx];
        bool is_video = (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        for (int j = 0; j < num_outputs; j++) {
            if (out_ctxs[j].type == OUTPUT_TYPE_HLS && is_video) {
                out_ctxs[j].hls_state.cur_video_pts = pkt->pts;
                out_ctxs[j].hls_state.video_time_base = in_stream->time_base;
            }
        }

        /* Confidence Monitor Snapshot */
        if (monitor.enabled && is_video) {
            confidence_monitor_process(&monitor, pkt, &stats);
        }

        /* Low-Latency Video Transcode Engine & SDI Playout for IP Ingress */
        if (is_video && transcoder != NULL) {
            ret = transcode_engine_send_packet(transcoder, pkt);
            while (transcode_engine_receive_decoded_frame(transcoder, dec_frame) == 0) {
                /* If SDI playout active, write decoded baseband frame directly to SDI output */
                for (int j = 0; j < num_outputs; j++) {
                    if (out_ctxs[j].type == OUTPUT_TYPE_SDI && out_ctxs[j].sdi_out) {
                        sdi_output_write_video_frame(out_ctxs[j].sdi_out, dec_frame);
                        stats.sdi_frames_written++;
                        stats.last_sdi_write_time_us = get_time_us();
                    }
                }

                /* Encode frame */
                transcode_engine_send_frame(transcoder, dec_frame);
                while (transcode_engine_receive_packet(transcoder, enc_pkt) == 0) {
                    stats.transcode_latency_us = transcode_engine_get_last_latency_us(transcoder);
                    enc_pkt->stream_index = out_idx;

                    AVRational enc_tb = transcode_engine_get_timebase(transcoder);
                    if (enc_pkt->pts != AV_NOPTS_VALUE) {
                        stats.cur_video_pts_ms = av_rescale_q(enc_pkt->pts, enc_tb, (AVRational){1, 1000});
                        stats.last_video_write_time_us = get_time_us();
                        if (stats.first_video_pts_ms == AV_NOPTS_VALUE) {
                            stats.first_video_pts_ms = stats.cur_video_pts_ms;
                        }
                    }

                    if (enc_pkt->flags & AV_PKT_FLAG_KEY) {
                        int64_t now_kf_us = get_time_us();
                        if (stats.last_keyframe_time_us > 0) {
                            double kf_interval = (double)(now_kf_us - stats.last_keyframe_time_us) / 1000000.0;
                            stats.last_keyframe_interval_sec = kf_interval;
                        }
                        stats.last_keyframe_time_us = now_kf_us;
                        stats.total_keyframes_seen++;
                    }

                    for (int j = 0; j < num_outputs; j++) {
                        if (out_ctxs[j].type == OUTPUT_TYPE_SDI) continue;
                        AVStream *out_stream = out_ctxs[j].ofmt_ctx->streams[out_idx];
                        AVPacket *out_pkt = out_ctxs[j].pkt;

                        if (av_packet_ref(out_pkt, enc_pkt) < 0) continue;
                        out_pkt->stream_index = out_idx;
                        out_pkt->pos = -1;

                        AVRational enc_tb = transcode_engine_get_timebase(transcoder);
                        av_packet_rescale_ts(out_pkt, enc_tb, out_stream->time_base);
                        out_pkt->duration = av_rescale_q(1, enc_tb, out_stream->time_base);

                        if (out_pkt->dts < 0) {
                            out_pkt->dts = 0;
                        }
                        if (out_ctxs[j].stream_last_dts[out_idx] != AV_NOPTS_VALUE && out_pkt->dts <= out_ctxs[j].stream_last_dts[out_idx]) {
                            out_pkt->dts = out_ctxs[j].stream_last_dts[out_idx] + 1;
                        }
                        if (out_pkt->pts < out_pkt->dts) {
                            out_pkt->pts = out_pkt->dts;
                        }
                        out_ctxs[j].stream_last_dts[out_idx] = out_pkt->dts;

                        int packet_size = out_pkt->size;
                        int64_t write_start_us = get_time_us();

                        ret = av_interleaved_write_frame(out_ctxs[j].ofmt_ctx, out_pkt);
                        int64_t write_finish_us = get_time_us();
                        int64_t write_duration_us = write_finish_us - write_start_us;

                        if (ret < 0) {
                            static int64_t last_ipt_werr_us = 0;
                            int64_t now_w = get_time_us();
                            if (now_w - last_ipt_werr_us > 3000000LL) {
                                char err_str[AV_ERROR_MAX_STRING_SIZE] = {0};
                                av_strerror(ret, err_str, sizeof(err_str));
                                LOG_WARN("[Output #%d] av_interleaved_write_frame failed: %s",
                                         out_ctxs[j].type_index, err_str);
                                last_ipt_werr_us = now_w;
                            }
                        }

                        out_ctxs[j].write_latency_sum_us += write_duration_us;
                        out_ctxs[j].write_latency_count++;
                        if (write_duration_us < out_ctxs[j].write_latency_min_us)
                            out_ctxs[j].write_latency_min_us = write_duration_us;
                        if (write_duration_us > out_ctxs[j].write_latency_max_us)
                            out_ctxs[j].write_latency_max_us = write_duration_us;
                        if (write_duration_us > out_ctxs[j].write_latency_peak_us)
                            out_ctxs[j].write_latency_peak_us = write_duration_us;

                        out_ctxs[j].packets_written++;
                        out_ctxs[j].video_packets_written++;
                        out_ctxs[j].bytes_written += packet_size;
                        out_ctxs[j].last_write_time_us = write_finish_us;
                        out_ctxs[j].last_video_write_time_us = write_finish_us;

                        if (j == 0) {
                            stats.last_video_write_time_us = write_finish_us;
                        }
                        av_packet_unref(out_pkt);
                    }
                    av_packet_unref(enc_pkt);
                }
                av_frame_unref(dec_frame);
            }
            av_packet_unref(pkt);
            goto check_telemetry;
        }

        /* Low-Latency Audio Transcode Engine & SDI Playout for IP Ingress */
        if (!is_video && in_idx == audio_stream_idx && (a_transcoder != NULL || num_sdi_outputs > 0)) {
            if (a_transcoder) {
                ret = audio_transcode_engine_send_packet(a_transcoder, pkt);
                while (audio_transcode_engine_receive_decoded_frame(a_transcoder, dec_aframe) == 0) {
                    /* If SDI playout active, write decoded baseband audio to SDI outputs */
                    for (int j = 0; j < num_outputs; j++) {
                        if (out_ctxs[j].type == OUTPUT_TYPE_SDI && out_ctxs[j].sdi_out) {
                            AVFrame *sdi_pcm = NULL;
                            if (audio_transcode_engine_resample_for_sdi(a_transcoder, dec_aframe, &sdi_pcm) == 0 && sdi_pcm) {
                                sdi_output_write_audio_frame(out_ctxs[j].sdi_out, sdi_pcm);
                                stats.sdi_frames_written++;
                                stats.last_sdi_write_time_us = get_time_us();
                                av_frame_free(&sdi_pcm);
                            }
                        }
                    }

                    /* If audio transcode requested (not copy), encode and write to IP outputs */
                    if (config->audio_transcode.acodec != TRANSCODE_AUDIO_COPY) {
                        audio_transcode_engine_send_frame(a_transcoder, dec_aframe);
                        while (audio_transcode_engine_receive_packet(a_transcoder, enc_a_pkt) == 0) {
                            enc_a_pkt->stream_index = out_idx;
                            AVRational a_enc_tb = audio_transcode_engine_get_timebase(a_transcoder);
                            if (enc_a_pkt->pts != AV_NOPTS_VALUE) {
                                stats.cur_audio_pts_ms = av_rescale_q(enc_a_pkt->pts, a_enc_tb, (AVRational){1, 1000});
                            }

                            for (int j = 0; j < num_outputs; j++) {
                                if (out_ctxs[j].type == OUTPUT_TYPE_SDI) continue;
                                AVStream *out_stream = out_ctxs[j].ofmt_ctx->streams[out_idx];
                                AVPacket *out_pkt = out_ctxs[j].pkt;

                                if (av_packet_ref(out_pkt, enc_a_pkt) < 0) continue;
                                out_pkt->stream_index = out_idx;
                                out_pkt->pos = -1;

                                AVRational enc_tb = audio_transcode_engine_get_timebase(a_transcoder);
                                av_packet_rescale_ts(out_pkt, enc_tb, out_stream->time_base);
                                out_pkt->duration = av_rescale_q(1024, enc_tb, out_stream->time_base);

                                if (out_pkt->dts < 0) {
                                    out_pkt->dts = 0;
                                }
                                if (out_ctxs[j].stream_last_dts[out_idx] != AV_NOPTS_VALUE &&
                                    out_pkt->dts <= out_ctxs[j].stream_last_dts[out_idx]) {
                                    out_pkt->dts = out_ctxs[j].stream_last_dts[out_idx] + 1;
                                }
                                if (out_pkt->pts < out_pkt->dts) {
                                    out_pkt->pts = out_pkt->dts;
                                }
                                out_ctxs[j].stream_last_dts[out_idx] = out_pkt->dts;

                                int packet_size = out_pkt->size;
                                ret = av_interleaved_write_frame(out_ctxs[j].ofmt_ctx, out_pkt);
                                if (ret == 0) {
                                    out_ctxs[j].packets_written++;
                                    out_ctxs[j].audio_packets_written++;
                                    out_ctxs[j].bytes_written += packet_size;
                                    out_ctxs[j].last_write_time_us = get_time_us();
                                    out_ctxs[j].last_audio_write_time_us = get_time_us();
                                }
                                av_packet_unref(out_pkt);
                            }
                            av_packet_unref(enc_a_pkt);
                        }
                    }
                    av_frame_unref(dec_aframe);
                }
            }
            if (config->audio_transcode.acodec != TRANSCODE_AUDIO_COPY) {
                av_packet_unref(pkt);
                goto check_telemetry;
            }
        }

        /* Keyframe Detection and GOP Interval Diagnostics */
        if (is_video && (pkt->flags & AV_PKT_FLAG_KEY)) {
            int64_t now_kf_us = get_time_us();
            if (stats.last_keyframe_time_us > 0) {
                double kf_interval = (double)(now_kf_us - stats.last_keyframe_time_us) / 1000000.0;
                stats.last_keyframe_interval_sec = kf_interval;
                if (config->enable_diagnostics && kf_interval > 4.2) {
                    LOG_WARN("[BUFFER RISK] Keyframe interval is %.2fs (> 4.0s). YouTube/players recommend 2-4s keyframes. Viewers may experience buffering!",
                             kf_interval);
                }
            }
            stats.last_keyframe_time_us = now_kf_us;
            stats.total_keyframes_seen++;
        }

        /* Wait for first video keyframe */
        if (!seen_first_keyframe) {
            if (is_video) {
                if (pkt->flags & AV_PKT_FLAG_KEY) {
                    seen_first_keyframe = true;
                    LOG_INFO("First video keyframe received (PTS=%lld). Output transmission started.",
                             (long long)pkt->pts);
                } else {
                    stats.udp_dropped_pre_keyframe++;
                    av_packet_unref(pkt);
                    continue;
                }
            } else {
                stats.udp_dropped_pre_keyframe++;
                av_packet_unref(pkt);
                continue;
            }
        }

        /* Compute base timestamps */
        if (config->normalize_pts && out_idx < MAX_STREAMS) {
            if (stream_first_dts[out_idx] == AV_NOPTS_VALUE) {
                stream_first_dts[out_idx] = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                stream_first_pts[out_idx] = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            }
        }

        int64_t dts_offset = (config->normalize_pts && out_idx < MAX_STREAMS && pkt->dts != AV_NOPTS_VALUE) ?
                             (pkt->dts - stream_first_dts[out_idx]) : pkt->dts;
        int64_t pts_offset = (config->normalize_pts && out_idx < MAX_STREAMS && pkt->pts != AV_NOPTS_VALUE) ?
                             (pkt->pts - stream_first_pts[out_idx]) : (config->normalize_pts ? dts_offset : pkt->pts);
        int64_t in_duration = pkt->duration;

        int64_t pts_in_ms = (pkt->pts != AV_NOPTS_VALUE) ?
            av_rescale_q(config->normalize_pts ? pts_offset : pkt->pts, in_stream->time_base, (AVRational){1, 1000}) : AV_NOPTS_VALUE;

        /* Write packet to each active output */
        bool output_error = false;
        bool timeline_updated_this_pkt = false;
        for (int j = 0; j < num_outputs; j++) {
            if (out_ctxs[j].failed) continue;
            if (out_ctxs[j].type == OUTPUT_TYPE_SDI) continue;
            AVStream *out_stream = out_ctxs[j].ofmt_ctx->streams[out_idx];
            AVPacket *out_pkt = out_ctxs[j].pkt;

            ret = av_packet_ref(out_pkt, pkt);
            if (ret < 0) continue;

            if (config->normalize_pts) {
                out_pkt->pts = av_rescale_q_rnd(pts_offset, in_stream->time_base, out_stream->time_base,
                                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                out_pkt->dts = av_rescale_q_rnd(dts_offset, in_stream->time_base, out_stream->time_base,
                                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                if (in_duration > 0) {
                    out_pkt->duration = av_rescale_q(in_duration, in_stream->time_base, out_stream->time_base);
                }
            } else {
                av_packet_rescale_ts(out_pkt, in_stream->time_base, out_stream->time_base);
            }

            /* Timestamp Gap Diagnostics on primary video (normalized to milliseconds) */
            int64_t dts_in_ms = (out_pkt->dts != AV_NOPTS_VALUE) ?
                av_rescale_q(out_pkt->dts, out_stream->time_base, (AVRational){1, 1000}) : AV_NOPTS_VALUE;

            if (j == 0 && is_video && config->enable_diagnostics && stats.last_video_dts_ms != AV_NOPTS_VALUE && dts_in_ms != AV_NOPTS_VALUE) {
                int64_t dts_delta_ms = dts_in_ms - stats.last_video_dts_ms;
                if (dts_delta_ms > 250) {
                    stats.num_timestamp_gaps++;
                    LOG_WARN("[BUFFER RISK] Video timestamp gap of %lld ms detected! Multicast UDP packet loss likely.",
                             (long long)dts_delta_ms);
                }
            }

            /* Strict Monotonicity Enforcement */
            if (out_idx < MAX_STREAMS) {
                if (out_pkt->dts < 0) {
                    out_pkt->dts = 0;
                }
                if (out_ctxs[j].stream_last_dts[out_idx] != AV_NOPTS_VALUE && out_pkt->dts <= out_ctxs[j].stream_last_dts[out_idx]) {
                    out_pkt->dts = out_ctxs[j].stream_last_dts[out_idx] + 1;
                }
                if (out_pkt->pts < out_pkt->dts) {
                    out_pkt->pts = out_pkt->dts;
                }
                out_ctxs[j].stream_last_dts[out_idx] = out_pkt->dts;
            }

            if (j == 0 && is_video) {
                stats.last_video_dts_ms = dts_in_ms;
            }

            out_pkt->stream_index = out_idx;
            out_pkt->pos = -1;

            int packet_size = out_pkt->size;
            int64_t packet_pts = out_pkt->pts;
            int64_t write_start_us = get_time_us();

            if (j == 0) {
                if (stats.last_packet_write_time_us > 0) {
                    int64_t pacing_gap_us = write_start_us - stats.last_packet_write_time_us;
                    if (pacing_gap_us > stats.max_pacing_gap_us) {
                        stats.max_pacing_gap_us = pacing_gap_us;
                    }
                }
                stats.last_packet_write_time_us = write_start_us;
            }

            ret = av_interleaved_write_frame(out_ctxs[j].ofmt_ctx, out_pkt);
            int64_t write_finish_us = get_time_us();
            int64_t write_duration_us = write_finish_us - write_start_us;
            int64_t write_duration_ms = write_duration_us / 1000;

            out_ctxs[j].write_latency_sum_us += write_duration_us;
            out_ctxs[j].write_latency_count++;
            if (write_duration_us < out_ctxs[j].write_latency_min_us)
                out_ctxs[j].write_latency_min_us = write_duration_us;
            if (write_duration_us > out_ctxs[j].write_latency_max_us)
                out_ctxs[j].write_latency_max_us = write_duration_us;
            if (write_duration_us > out_ctxs[j].write_latency_peak_us)
                out_ctxs[j].write_latency_peak_us = write_duration_us;

            if (config->enable_diagnostics) {
                if (out_ctxs[j].type == OUTPUT_TYPE_RTMP && write_duration_ms > 300) {
                    stats.num_socket_slow_writes++;
                    LOG_WARN("[RTMP #%d] Socket write blocked for %lld ms! Uplink internet bandwidth congestion detected.",
                             out_ctxs[j].type_index, (long long)write_duration_ms);
                } else if (out_ctxs[j].type == OUTPUT_TYPE_SRT && write_duration_ms > 300) {
                    stats.num_socket_slow_writes++;
                    LOG_WARN("[SRT #%d] Socket write blocked for %lld ms! Network jitter or uplink congestion detected.",
                             out_ctxs[j].type_index, (long long)write_duration_ms);
                } else if (out_ctxs[j].type == OUTPUT_TYPE_HLS) {
                    int64_t hls_threshold_ms = (config->hls_time > 0 ? config->hls_time : 2) * 800;
                    if (write_duration_ms > hls_threshold_ms) {
                        stats.num_socket_slow_writes++;
                        LOG_WARN("[HLS #%d] Segment rotation / upload blocked for %lld ms (> %lld ms threshold)! CDN uplink congestion detected.",
                                 out_ctxs[j].type_index, (long long)write_duration_ms, (long long)hls_threshold_ms);
                    }
                }
            }

            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                const char *out_err_type = (out_ctxs[j].type == OUTPUT_TYPE_RTMP) ? "RTMP" :
                                           ((out_ctxs[j].type == OUTPUT_TYPE_HLS) ? "HLS" :
                                           ((out_ctxs[j].type == OUTPUT_TYPE_SRT) ? "SRT" :
                                           ((out_ctxs[j].type == OUTPUT_TYPE_UDP) ? "UDP" :
                                           ((out_ctxs[j].type == OUTPUT_TYPE_RTP) ? "RTP" : "SDI"))));
                LOG_ERROR("[%s #%d] Write Error: %s", out_err_type, out_ctxs[j].type_index, err_buf);
                av_packet_unref(out_pkt);
                if (num_outputs > 1) {
                    LOG_WARN("[%s #%d] Output '%s' encountered error and is deactivated; other targets remain active.",
                             out_err_type, out_ctxs[j].type_index, out_ctxs[j].url);
                    out_ctxs[j].failed = true;
                    bool any_alive = false;
                    for (int k = 0; k < num_outputs; k++) {
                        if (!out_ctxs[k].failed && out_ctxs[k].type != OUTPUT_TYPE_SDI) {
                            any_alive = true;
                            break;
                        }
                    }
                    if (!any_alive) {
                        output_error = true;
                        break;
                    }
                    continue;
                } else {
                    output_error = true;
                    break;
                }
            }

            out_ctxs[j].packets_written++;
            out_ctxs[j].bytes_written += packet_size;
            out_ctxs[j].last_write_time_us = write_finish_us;

            if (is_video) {
                out_ctxs[j].video_packets_written++;
                out_ctxs[j].last_video_write_time_us = write_finish_us;
            } else {
                out_ctxs[j].audio_packets_written++;
                out_ctxs[j].last_audio_write_time_us = write_finish_us;
            }

            /* Master timeline tracking (from primary output) */
            if (!timeline_updated_this_pkt) {
                timeline_updated_this_pkt = true;
                if (is_video) {
                    stats.cur_video_pts_ms = (out_ctxs[j].type == OUTPUT_TYPE_RTMP) ? packet_pts : pts_in_ms;
                    stats.last_video_write_time_us = write_finish_us;
                    if (stats.first_video_pts_ms == AV_NOPTS_VALUE) {
                        stats.first_video_pts_ms = stats.cur_video_pts_ms;
                    }
                } else {
                    stats.cur_audio_pts_ms = (out_ctxs[j].type == OUTPUT_TYPE_RTMP) ? packet_pts : pts_in_ms;
                    stats.last_audio_write_time_us = write_finish_us;
                }
            }

            av_packet_unref(out_pkt);
        }

        av_packet_unref(pkt);
        if (output_error) {
            break;
        }

check_telemetry:
        /* Telemetry Reporting Each Second */
        if (config->print_stats) {
            int64_t now_us = get_time_us();
            int64_t elapsed_stats_us = now_us - stats.last_stats_time_us;

            if (elapsed_stats_us >= ((int64_t)config->stats_interval_ms * 1000LL)) {
                double elapsed_sec = (double)elapsed_stats_us / 1000000.0;
                int64_t uptime_sec = (now_us - stats.start_time_us) / 1000000LL;

                /* SDI Ingress & Playout Telemetry */
                double raw_sdi_in_fps = (elapsed_sec > 0.0) ? (double)(stats.sdi_frames_read - last_sdi_frames_read) / elapsed_sec : 0.0;
                double raw_sdi_out_fps = (elapsed_sec > 0.0) ? (double)(stats.sdi_frames_written - last_sdi_frames_written) / elapsed_sec : 0.0;
                if (stats.sdi_in_fps > 0.0 && raw_sdi_in_fps > 0.0) {
                    stats.sdi_in_fps = 0.35 * raw_sdi_in_fps + 0.65 * stats.sdi_in_fps;
                } else {
                    stats.sdi_in_fps = raw_sdi_in_fps;
                }
                if (stats.sdi_out_fps > 0.0 && raw_sdi_out_fps > 0.0) {
                    stats.sdi_out_fps = 0.35 * raw_sdi_out_fps + 0.65 * stats.sdi_out_fps;
                } else {
                    stats.sdi_out_fps = raw_sdi_out_fps;
                }
                last_sdi_frames_read = stats.sdi_frames_read;
                last_sdi_frames_written = stats.sdi_frames_written;

                /* UDP Ingress Metrics & Continuity Packet Loss */
                double udp_bitrate_kbps = ((double)(stats.udp_bytes_read - stats.last_udp_bytes) * 8.0) /
                                          (elapsed_sec * 1000.0);
                double udp_pps = (double)(stats.udp_packets_read - stats.last_udp_packets) / elapsed_sec;

                stats.udp_lost_packets = s_udp_lost_packets;
                stats.udp_cc_errors = s_udp_cc_errors;
                uint64_t interval_lost = (stats.udp_lost_packets >= stats.last_udp_lost_packets) ?
                    (stats.udp_lost_packets - stats.last_udp_lost_packets) : 0;
                uint64_t interval_ts_pkts = ((stats.udp_bytes_read - stats.last_udp_bytes) / 188) + interval_lost;
                stats.cur_interval_loss_pct = (interval_ts_pkts > 0) ?
                    ((double)interval_lost * 100.0 / (double)interval_ts_pkts) : 0.0;
                uint64_t total_ts_pkts = (stats.udp_bytes_read / 188) + stats.udp_lost_packets;
                stats.udp_loss_pct = (total_ts_pkts > 0) ?
                    ((double)stats.udp_lost_packets * 100.0 / (double)total_ts_pkts) : 0.0;

                /* SRT Ingress Metrics & ARQ Stats */
                if (is_srt_input) {
                    if (stats.srt_in_sock <= 0 && ifmt_ctx && ifmt_ctx->pb) {
                        stats.srt_in_sock = srt_extract_socket(ifmt_ctx->pb);
                    }
                    srt_update_ingress_stats(stats.srt_in_sock, &stats);
                }

                /* Per-Output Metrics & TCP/SRT Stats */
                for (int j = 0; j < num_outputs; j++) {
                    OutputContext *oc = &out_ctxs[j];
                    oc->bitrate_kbps = ((double)(oc->bytes_written - oc->last_bytes) * 8.0) / (elapsed_sec * 1000.0);
                    oc->pps = (double)(oc->packets_written - oc->last_packets) / elapsed_sec;
                    double raw_vfps = (double)(oc->video_packets_written - oc->last_video_pkts) / elapsed_sec;
                    if (oc->video_fps > 0.0 && raw_vfps > 0.0) {
                        oc->video_fps = 0.35 * raw_vfps + 0.65 * oc->video_fps;
                    } else {
                        oc->video_fps = raw_vfps;
                    }
                    oc->audio_pps = (double)(oc->audio_packets_written - oc->last_audio_pkts) / elapsed_sec;

                    oc->write_avg_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_sum_us / (double)oc->write_latency_count / 1000.0 : 0.0;
                    oc->write_min_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_min_us / 1000.0 : 0.0;
                    oc->write_max_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_max_us / 1000.0 : 0.0;

                    if (oc->type == OUTPUT_TYPE_RTMP) {
                        rtmp_update_tcp_stats(oc);
                    } else if (oc->type == OUTPUT_TYPE_HLS) {
                        hls_update_tcp_stats(&oc->hls_state);
                        oc->tcp_fd = oc->hls_state.tcp_fd;
                        oc->tcp_retrans = oc->hls_state.total_retrans;
                        oc->tcp_lost = oc->hls_state.total_lost;
                        oc->tcp_rtt_ms = oc->hls_state.rtt_ms;
                        oc->tcp_rttvar_ms = oc->hls_state.rttvar_ms;
                    } else if (oc->type == OUTPUT_TYPE_SRT) {
                        srt_update_egress_stats(oc);
                    }
                }

                /* Primary Output pointers */
                OutputContext *prim_rtmp = NULL;
                OutputContext *prim_hls = NULL;
                OutputContext *prim_srt = NULL;
                OutputContext *prim_udp = NULL;
                OutputContext *prim_rtp = NULL;
                for (int j = 0; j < num_outputs; j++) {
                    if (out_ctxs[j].type == OUTPUT_TYPE_RTMP && !prim_rtmp) prim_rtmp = &out_ctxs[j];
                    if (out_ctxs[j].type == OUTPUT_TYPE_HLS && !prim_hls) prim_hls = &out_ctxs[j];
                    if (out_ctxs[j].type == OUTPUT_TYPE_SRT && !prim_srt) prim_srt = &out_ctxs[j];
                    if (out_ctxs[j].type == OUTPUT_TYPE_UDP && !prim_udp) prim_udp = &out_ctxs[j];
                    if (out_ctxs[j].type == OUTPUT_TYPE_RTP && !prim_rtp) prim_rtp = &out_ctxs[j];
                }

                if (prim_rtmp) {
                    stats.rtmp_bytes_written = prim_rtmp->bytes_written;
                    stats.rtmp_packets_written = prim_rtmp->packets_written;
                    stats.rtmp_video_packets_written = prim_rtmp->video_packets_written;
                    stats.rtmp_audio_packets_written = prim_rtmp->audio_packets_written;
                    stats.rtmp_tcp_fd = prim_rtmp->tcp_fd;
                    stats.rtmp_tcp_retrans = prim_rtmp->tcp_retrans;
                    stats.rtmp_tcp_lost = prim_rtmp->tcp_lost;
                    stats.rtmp_tcp_rtt_ms = prim_rtmp->tcp_rtt_ms;
                    stats.rtmp_tcp_rttvar_ms = prim_rtmp->tcp_rttvar_ms;
                    stats.write_latency_min_us = prim_rtmp->write_latency_min_us;
                    stats.write_latency_max_us = prim_rtmp->write_latency_max_us;
                    stats.write_latency_sum_us = prim_rtmp->write_latency_sum_us;
                    stats.write_latency_count = prim_rtmp->write_latency_count;
                    stats.write_latency_peak_us = prim_rtmp->write_latency_peak_us;
                }
                if (prim_hls) {
                    stats.hls_bytes_written = prim_hls->bytes_written;
                    stats.hls_packets_written = prim_hls->packets_written;
                    stats.hls_segments_completed = prim_hls->hls_state.segments_completed;
                    snprintf(stats.hls_active_segment, sizeof(stats.hls_active_segment), "%s", prim_hls->hls_state.active_segment_name);
                    stats.hls_last_segment_size = prim_hls->hls_state.last_segment_size_bytes;
                    stats.hls_last_segment_duration_sec = prim_hls->hls_state.last_segment_duration_sec;
                    stats.hls_last_segment_upload_ms = prim_hls->hls_state.last_segment_upload_ms;
                    stats.hls_last_segment_speed_factor = prim_hls->hls_state.last_segment_speed_factor;
                    snprintf(stats.hls_origin_health, sizeof(stats.hls_origin_health), "%s", prim_hls->hls_state.origin_health_str);
                    stats.hls_tcp_fd = prim_hls->tcp_fd;
                    stats.hls_tcp_retrans = prim_hls->tcp_retrans;
                    stats.hls_tcp_lost = prim_hls->tcp_lost;
                    stats.hls_tcp_rtt_ms = prim_hls->tcp_rtt_ms;
                    stats.hls_tcp_rttvar_ms = prim_hls->tcp_rttvar_ms;
                }
                if (prim_srt) {
                    stats.srt_bytes_written = prim_srt->bytes_written;
                    stats.srt_packets_written = prim_srt->packets_written;
                    stats.srt_video_packets_written = prim_srt->video_packets_written;
                    stats.srt_audio_packets_written = prim_srt->audio_packets_written;
                    stats.srt_sock = prim_srt->srt_sock;
                    stats.srt_is_listener = prim_srt->is_srt_listener;
                    stats.srt_client_count = prim_srt->srt_client_count;
                    stats.srt_rtt_ms = prim_srt->srt_rtt_ms;
                    stats.srt_nack_interval = prim_srt->srt_nack_interval;
                    stats.srt_nack_total = prim_srt->srt_nack_total;
                    stats.srt_loss_interval = prim_srt->srt_loss_interval;
                    stats.srt_loss_total = prim_srt->srt_loss_total;
                    stats.srt_retrans_interval = prim_srt->srt_retrans_interval;
                    stats.srt_retrans_total = prim_srt->srt_retrans_total;
                    stats.srt_drop_interval = prim_srt->srt_drop_interval;
                    stats.srt_drop_total = prim_srt->srt_drop_total;
                    stats.srt_arq_success_pct = prim_srt->srt_arq_success_pct;
                    stats.srt_mbps_bandwidth = prim_srt->srt_mbps_bandwidth;
                    stats.srt_snd_buf_ms = prim_srt->srt_snd_buf_ms;
                }
                if (prim_rtp) {
                    stats.rtp_bytes_written = prim_rtp->bytes_written;
                    stats.rtp_packets_written = prim_rtp->packets_written;
                    stats.rtp_video_packets_written = prim_rtp->video_packets_written;
                    stats.rtp_audio_packets_written = prim_rtp->audio_packets_written;
                }

                double write_avg_ms = prim_rtmp ? prim_rtmp->write_avg_ms : (prim_srt ? prim_srt->write_avg_ms : (prim_hls ? prim_hls->write_avg_ms : (prim_rtp ? prim_rtp->write_avg_ms : (prim_udp ? prim_udp->write_avg_ms : 0.0))));
                double write_min_ms = prim_rtmp ? prim_rtmp->write_min_ms : (prim_srt ? prim_srt->write_min_ms : (prim_hls ? prim_hls->write_min_ms : (prim_rtp ? prim_rtp->write_min_ms : (prim_udp ? prim_udp->write_min_ms : 0.0))));
                double write_max_ms = prim_rtmp ? prim_rtmp->write_max_ms : (prim_srt ? prim_srt->write_max_ms : (prim_hls ? prim_hls->write_max_ms : (prim_rtp ? prim_rtp->write_max_ms : (prim_udp ? prim_udp->write_max_ms : 0.0))));
                double max_gap_ms = (double)stats.max_pacing_gap_us / 1000.0;

                /* Media Delivery Speed Factor */
                double instant_speed = 1.0;
                if (stats.cur_video_pts_ms != AV_NOPTS_VALUE && stats.last_stats_video_pts_ms != AV_NOPTS_VALUE) {
                    double delta_pts_sec = (double)(stats.cur_video_pts_ms - stats.last_stats_video_pts_ms) / 1000.0;
                    instant_speed = (elapsed_sec > 0.0) ? (delta_pts_sec / elapsed_sec) : 1.0;
                    if (instant_speed < 0.0) instant_speed = 0.0;
                } else if (is_sdi_input && sdi_fps.num > 0 && sdi_fps.den > 0) {
                    double expected_fps = (double)sdi_fps.num / sdi_fps.den;
                    instant_speed = (expected_fps > 0.0 && stats.sdi_in_fps > 0.0) ? (stats.sdi_in_fps / expected_fps) : 1.0;
                } else if (stats.cur_video_pts_ms != AV_NOPTS_VALUE) {
                    instant_speed = 1.0;
                } else {
                    instant_speed = (out_ctxs[0].video_packets_written > 0 || out_ctxs[0].audio_packets_written > 0) ? 1.0 : 0.0;
                }
                if (stats.cur_speed_factor > 0.0 && instant_speed > 0.0) {
                    stats.cur_speed_factor = 0.35 * instant_speed + 0.65 * stats.cur_speed_factor;
                } else {
                    stats.cur_speed_factor = instant_speed;
                }

                /* Keyframe age */
                if (stats.last_keyframe_time_us > 0) {
                    stats.cur_keyframe_age_sec = (double)(now_us - stats.last_keyframe_time_us) / 1000000.0;
                } else {
                    stats.cur_keyframe_age_sec = 0.0;
                }

                /* Silence tracking */
                int64_t last_v_write = prim_rtmp ? prim_rtmp->last_video_write_time_us : (prim_srt ? prim_srt->last_video_write_time_us : (prim_hls ? prim_hls->last_video_write_time_us : (prim_rtp ? prim_rtp->last_video_write_time_us : (prim_udp ? prim_udp->last_video_write_time_us : 0))));
                int64_t last_a_write = prim_rtmp ? prim_rtmp->last_audio_write_time_us : (prim_srt ? prim_srt->last_audio_write_time_us : (prim_hls ? prim_hls->last_audio_write_time_us : (prim_rtp ? prim_rtp->last_audio_write_time_us : (prim_udp ? prim_udp->last_audio_write_time_us : 0))));
                stats.cur_video_silence_ms = (last_v_write > 0) ? ((now_us - last_v_write) / 1000LL) : 0;
                stats.cur_audio_silence_ms = (last_a_write > 0) ? ((now_us - last_a_write) / 1000LL) : 0;

                /* Media timeline lead vs wall clock */
                if (stats.cur_video_pts_ms != AV_NOPTS_VALUE && stats.first_video_pts_ms != AV_NOPTS_VALUE) {
                    int64_t media_elapsed_ms = stats.cur_video_pts_ms - stats.first_video_pts_ms;
                    int64_t wall_elapsed_ms = (now_us - stats.start_time_us) / 1000LL;
                    stats.cur_wall_lead_ms = media_elapsed_ms - wall_elapsed_ms;
                } else {
                    stats.cur_wall_lead_ms = 0;
                }

                /* A/V Sync Drift */
                int64_t av_drift_ms = 0;
                if (stats.cur_video_pts_ms != AV_NOPTS_VALUE && stats.cur_audio_pts_ms != AV_NOPTS_VALUE) {
                    av_drift_ms = stats.cur_video_pts_ms - stats.cur_audio_pts_ms;
                }

                /* Keyframe and GOP Interval Evaluation */
                char gop_str[32] = "";
                double effective_gop_sec = stats.last_keyframe_interval_sec;
                if (config->transcode.intra_refresh) {
                    snprintf(gop_str, sizeof(gop_str), "GOP: Intra");
                } else {
                    if (effective_gop_sec <= 0.0 && transcoder != NULL && config->transcode.gop_size > 0) {
                        AVRational enc_tb = transcode_engine_get_timebase(transcoder);
                        if (enc_tb.num > 0 && enc_tb.den > 0) {
                            effective_gop_sec = (double)config->transcode.gop_size * enc_tb.num / enc_tb.den;
                        }
                    }
                    snprintf(gop_str, sizeof(gop_str), "GOP: %.1fs", effective_gop_sec);
                }

                /* YouTube / Stream Health Evaluation */
                if (num_rtmp_outputs > 0) {
                    bool is_stalled_video = (stats.cur_video_silence_ms > 600 &&
                                             (prim_rtmp->audio_packets_written - prim_rtmp->last_audio_pkts) > 5);
                    bool is_underfeeding = (prim_rtmp->video_packets_written > 30 && stats.cur_speed_factor < 0.88);
                    bool is_kf_delayed = !config->transcode.intra_refresh && (stats.cur_keyframe_age_sec > 4.2);
                    bool is_av_desync = (llabs(av_drift_ms) > 600);
                    bool is_slow_socket = (write_max_ms > 400.0);
                    bool is_bursty = (max_gap_ms > 400.0);
                    bool is_udp_loss = (interval_lost > 5 || stats.cur_interval_loss_pct > 1.5);
                    bool is_tcp_loss = (prim_rtmp->tcp_lost > 0);

                    if (is_stalled_video) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Video Frozen", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_underfeeding) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Underfeeding", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_udp_loss) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: UDP Loss", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_kf_delayed) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Waiting Keyframe", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_tcp_loss) {
                        strncpy(stats.youtube_health_status, "NETWORK STALL: TCP Loss", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_av_desync) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: A/V Desync", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_slow_socket) {
                        strncpy(stats.youtube_health_status, "NETWORK STALL: TCP Latency", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_bursty) {
                        strncpy(stats.youtube_health_status, "BURSTY PACING", sizeof(stats.youtube_health_status) - 1);
                    } else if (prim_rtmp->video_packets_written > 30 && stats.cur_speed_factor < 0.95) {
                        strncpy(stats.youtube_health_status, "SLOW PACING", sizeof(stats.youtube_health_status) - 1);
                    } else {
                        strncpy(stats.youtube_health_status, "HEALTHY", sizeof(stats.youtube_health_status) - 1);
                    }
                } else if (num_srt_outputs > 0 && prim_srt) {
                    bool is_stalled_video = (stats.cur_video_silence_ms > 600 &&
                                             (prim_srt->audio_packets_written - prim_srt->last_audio_pkts) > 5);
                    bool is_underfeeding = (prim_srt->video_packets_written > 30 && stats.cur_speed_factor < 0.88);
                    bool is_kf_delayed = !config->transcode.intra_refresh && (stats.cur_keyframe_age_sec > 4.2);
                    bool is_av_desync = (llabs(av_drift_ms) > 600);
                    bool is_slow_socket = (write_max_ms > 400.0);
                    bool is_bursty = (max_gap_ms > 400.0);
                    bool is_srt_loss = (prim_srt->srt_loss_interval > 5 || prim_srt->srt_arq_success_pct < 90.0);
                    bool is_high_rtt = (prim_srt->srt_rtt_ms > 500.0);

                    if (is_stalled_video) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Video Frozen", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_underfeeding) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Underfeeding", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_srt_loss) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: SRT Loss", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_high_rtt) {
                        strncpy(stats.youtube_health_status, "NETWORK STALL: High RTT", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_kf_delayed) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Waiting Keyframe", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_av_desync) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: A/V Desync", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_slow_socket) {
                        strncpy(stats.youtube_health_status, "NETWORK STALL: Write Latency", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_bursty) {
                        strncpy(stats.youtube_health_status, "BURSTY PACING", sizeof(stats.youtube_health_status) - 1);
                    } else if (prim_srt->video_packets_written > 30 && stats.cur_speed_factor < 0.95) {
                        strncpy(stats.youtube_health_status, "SLOW PACING", sizeof(stats.youtube_health_status) - 1);
                    } else {
                        strncpy(stats.youtube_health_status, "HEALTHY", sizeof(stats.youtube_health_status) - 1);
                    }
                } else if (num_rtp_outputs > 0 && prim_rtp) {
                    bool is_stalled_video = (stats.cur_video_silence_ms > 600 &&
                                             (prim_rtp->audio_packets_written - prim_rtp->last_audio_pkts) > 5);
                    bool is_underfeeding = (prim_rtp->video_packets_written > 30 && stats.cur_speed_factor < 0.88);
                    bool is_kf_delayed = !config->transcode.intra_refresh && (stats.cur_keyframe_age_sec > 4.2);
                    bool is_av_desync = (llabs(av_drift_ms) > 600);
                    bool is_slow_socket = (write_max_ms > 400.0);
                    bool is_bursty = (max_gap_ms > 400.0);

                    if (is_stalled_video) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Video Frozen", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_underfeeding) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Underfeeding", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_kf_delayed) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: Waiting Keyframe", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_av_desync) {
                        strncpy(stats.youtube_health_status, "BUFFER RISK: A/V Desync", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_slow_socket) {
                        strncpy(stats.youtube_health_status, "NETWORK STALL: Socket Latency", sizeof(stats.youtube_health_status) - 1);
                    } else if (is_bursty) {
                        strncpy(stats.youtube_health_status, "BURSTY PACING", sizeof(stats.youtube_health_status) - 1);
                    } else if (prim_rtp->video_packets_written > 30 && stats.cur_speed_factor < 0.95) {
                        strncpy(stats.youtube_health_status, "SLOW PACING", sizeof(stats.youtube_health_status) - 1);
                    } else {
                        strncpy(stats.youtube_health_status, "HEALTHY", sizeof(stats.youtube_health_status) - 1);
                    }
                }

                /* Trigger active warnings when diagnostics enabled */
                if (config->enable_diagnostics) {
                    if (interval_lost > 0) {
                        if (is_rtp_input) {
                            LOG_WARN("[BUFFER RISK] RTP Ingress loss: %llu TS packets dropped in last second (Loss: %.2f%%)!",
                                     (unsigned long long)interval_lost, stats.cur_interval_loss_pct);
                        } else if (!is_srt_input && !is_sdi_input) {
                            LOG_WARN("[BUFFER RISK] UDP Multicast loss: %llu TS packets dropped in last second (Loss: %.2f%%)!",
                                     (unsigned long long)interval_lost, stats.cur_interval_loss_pct);
                        }
                    }
                    if (is_srt_input && stats.srt_in_loss_interval > 0) {
                        LOG_WARN("[BUFFER RISK] SRT Ingress loss: %d packets lost in last second (Drops: %d, NAKs: %d, ARQ: %.1f%%)!",
                                 stats.srt_in_loss_interval, stats.srt_in_drop_interval, stats.srt_in_nack_interval, stats.srt_in_arq_success_pct);
                    }
                    if (prim_rtmp && prim_rtmp->tcp_lost > 0) {
                        LOG_WARN("[BUFFER RISK] RTMP uplink TCP loss: %u packets lost in transit!",
                                 prim_rtmp->tcp_lost);
                    }
                    if (prim_hls && prim_hls->tcp_lost > 0) {
                        LOG_WARN("[BUFFER RISK] HLS uplink TCP loss: %u packets lost in transit to CDN/origin!",
                                 prim_hls->tcp_lost);
                    }
                    if (prim_srt && prim_srt->srt_loss_interval > 0) {
                        LOG_WARN("[BUFFER RISK] SRT Egress loss: %d packets lost in transit (Retrans: %d/s, Drops: %d/s, ARQ: %.1f%%)!",
                                 prim_srt->srt_loss_interval, prim_srt->srt_retrans_interval, prim_srt->srt_drop_interval, prim_srt->srt_arq_success_pct);
                    }
                }

                if (config->json_telemetry) {
                    /* Structured JSON Telemetry Output (1 per second) */
                    printf("{\"uptime_sec\":%lld", (long long)uptime_sec);
                    if (num_rtmp_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0) {
                        printf(",\"stream_status\":\"STREAMING\",\"health\":{\"status\":\"%s\",\"speed_factor\":%.2f,\"keyframe_age_sec\":%.2f,\"max_pacing_gap_ms\":%.1f,\"wall_lead_ms\":%lld,\"video_silence_ms\":%lld,\"audio_silence_ms\":%lld}",
                               stats.youtube_health_status, stats.cur_speed_factor, stats.cur_keyframe_age_sec,
                               max_gap_ms, (long long)stats.cur_wall_lead_ms,
                               (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms);
                    }
                    const char *in_tag = is_sdi_input ? "sdi_in" : (is_srt_input ? "srt_in" : (is_rtp_input ? "rtp_in" : "udp_in"));
                    const char *in_proto = is_sdi_input ? "sdi" : (is_srt_input ? "srt" : (is_rtp_input ? "rtp" : "udp"));
                    printf(",\"%s\":{\"protocol\":\"%s\",\"bitrate_kbps\":%.1f,\"pps\":%.1f,\"total_packets\":%llu,\"total_bytes\":%llu,\"dropped_pre_keyframe\":%llu",
                           in_tag, in_proto,
                           udp_bitrate_kbps, udp_pps,
                           (unsigned long long)stats.udp_packets_read, (unsigned long long)stats.udp_bytes_read,
                           (unsigned long long)stats.udp_dropped_pre_keyframe);
                    if (is_srt_input) {
                        printf(",\"srt_transport\":{\"rtt_ms\":%.1f,\"nack\":{\"interval\":%d,\"total\":%d},\"loss\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"retransmit\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"drop\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"arq_success_pct\":%.1f,\"rcv_buf_ms\":%d,\"recv_rate_mbps\":%.2f}",
                               stats.srt_in_rtt_ms,
                               stats.srt_in_nack_interval, stats.srt_in_nack_total,
                               stats.srt_in_loss_interval, stats.srt_in_loss_total,
                               stats.srt_in_retrans_interval, stats.srt_in_retrans_total,
                               stats.srt_in_drop_interval, stats.srt_in_drop_total,
                               stats.srt_in_arq_success_pct,
                               stats.srt_in_rcv_buf_ms, stats.srt_in_rcv_rate_mbps);
                    } else {
                        printf(",\"packet_loss\":{\"lost_packets\":%llu,\"cc_errors\":%llu,\"total_loss_pct\":%.2f,\"interval_loss_pct\":%.2f}",
                               (unsigned long long)stats.udp_lost_packets,
                               (unsigned long long)stats.udp_cc_errors,
                               stats.udp_loss_pct, stats.cur_interval_loss_pct);
                    }
                    printf("}");

                    /* RTMP Output(s) in JSON */
                    if (num_rtmp_outputs == 1 && prim_rtmp) {
                        printf(",\"rtmp_out\":{\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"video_pts_ms\":%lld,\"audio_pts_ms\":%lld,\"av_drift_ms\":%lld,\"keyframe_interval_sec\":%.2f,\"total_packets\":%llu,\"total_bytes\":%llu,\"tcp_loss\":{\"retrans_packets\":%u,\"lost_packets\":%u,\"rtt_ms\":%.1f,\"rttvar_ms\":%.1f},\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"peak_ms\":%.2f}}",
                               prim_rtmp->url, prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->audio_pps,
                               (long long)(stats.cur_video_pts_ms != AV_NOPTS_VALUE ? stats.cur_video_pts_ms : 0),
                               (long long)(stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? stats.cur_audio_pts_ms : 0),
                               (long long)av_drift_ms, effective_gop_sec,
                               (unsigned long long)prim_rtmp->packets_written, (unsigned long long)prim_rtmp->bytes_written,
                               prim_rtmp->tcp_retrans, prim_rtmp->tcp_lost, prim_rtmp->tcp_rtt_ms, prim_rtmp->tcp_rttvar_ms,
                               prim_rtmp->write_avg_ms, prim_rtmp->write_min_ms, prim_rtmp->write_max_ms,
                               (double)prim_rtmp->write_latency_peak_us / 1000.0);
                    } else if (num_rtmp_outputs > 1) {
                        printf(",\"rtmp_outputs\":[");
                        int count = 0;
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_RTMP) continue;
                            if (count++ > 0) printf(",");
                            printf("{\"index\":%d,\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"total_bytes\":%llu,\"tcp_loss\":{\"retrans_packets\":%u,\"lost_packets\":%u,\"rtt_ms\":%.1f,\"rttvar_ms\":%.1f}}",
                                   out_ctxs[j].type_index, out_ctxs[j].url, out_ctxs[j].bitrate_kbps,
                                   out_ctxs[j].video_fps, out_ctxs[j].audio_pps,
                                   (unsigned long long)out_ctxs[j].bytes_written,
                                   out_ctxs[j].tcp_retrans, out_ctxs[j].tcp_lost,
                                   out_ctxs[j].tcp_rtt_ms, out_ctxs[j].tcp_rttvar_ms);
                        }
                        printf("]");
                    }

                    /* HLS Output(s) in JSON */
                    if (num_hls_outputs == 1 && prim_hls) {
                        const char *hls_method_name = (strncmp(prim_hls->url, "http", 4) == 0) ? config->hls_method : "FILE";
                        printf(",\"hls_out\":{\"bitrate_kbps\":%.1f,\"pps\":%.1f,\"total_bytes\":%llu,\"target\":\"%s\",\"method\":\"%s\",\"segments_completed\":%llu,\"active_segment\":{\"name\":\"%s\",\"size_bytes\":%lld,\"duration_sec\":%.2f,\"upload_time_ms\":%.1f,\"upload_speed_factor\":%.1f},\"window_segments\":%d,\"origin_health\":\"%s\"",
                                prim_hls->bitrate_kbps, prim_hls->pps, (unsigned long long)prim_hls->bytes_written,
                                prim_hls->url, hls_method_name,
                                (unsigned long long)prim_hls->hls_state.segments_completed,
                                prim_hls->hls_state.active_segment_name,
                                (long long)prim_hls->hls_state.last_segment_size_bytes,
                                prim_hls->hls_state.last_segment_duration_sec,
                                prim_hls->hls_state.last_segment_upload_ms,
                                prim_hls->hls_state.last_segment_speed_factor,
                                config->hls_list_size,
                                prim_hls->hls_state.origin_health_str);
                        if (strncmp(prim_hls->url, "http", 4) == 0) {
                            printf(",\"tcp_uplink\":{\"retrans_packets\":%u,\"lost_packets\":%u,\"rtt_ms\":%.1f,\"rttvar_ms\":%.1f}",
                                   prim_hls->tcp_retrans, prim_hls->tcp_lost, prim_hls->tcp_rtt_ms, prim_hls->tcp_rttvar_ms);
                        }
                        printf("}");
                    } else if (num_hls_outputs > 1) {
                        printf(",\"hls_outputs\":[");
                        int count = 0;
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_HLS) continue;
                            if (count++ > 0) printf(",");
                            printf("{\"index\":%d,\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"pps\":%.1f,\"total_bytes\":%llu,\"segments_completed\":%llu,\"origin_health\":\"%s\"",
                                   out_ctxs[j].type_index, out_ctxs[j].url, out_ctxs[j].bitrate_kbps, out_ctxs[j].pps,
                                   (unsigned long long)out_ctxs[j].bytes_written,
                                   (unsigned long long)out_ctxs[j].hls_state.segments_completed,
                                   out_ctxs[j].hls_state.origin_health_str);
                            if (strncmp(out_ctxs[j].url, "http", 4) == 0) {
                                printf(",\"tcp_uplink\":{\"retrans_packets\":%u,\"lost_packets\":%u,\"rtt_ms\":%.1f,\"rttvar_ms\":%.1f}",
                                       out_ctxs[j].tcp_retrans, out_ctxs[j].tcp_lost, out_ctxs[j].tcp_rtt_ms, out_ctxs[j].tcp_rttvar_ms);
                            }
                            printf("}");
                        }
                        printf("]");
                    }

                    /* SRT Output(s) in JSON */
                    if (num_srt_outputs == 1 && prim_srt) {
                        printf(",\"srt_out\":{\"target\":\"%s\",\"is_listener\":%s,\"clients\":%d,\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"video_pts_ms\":%lld,\"audio_pts_ms\":%lld,\"av_drift_ms\":%lld,\"keyframe_interval_sec\":%.2f,\"total_packets\":%llu,\"total_bytes\":%llu,\"srt_transport\":{\"rtt_ms\":%.1f,\"nack\":{\"interval\":%d,\"total\":%d},\"loss\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"retransmit\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"drop\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"arq_success_pct\":%.1f,\"bandwidth_mbps\":%.2f,\"snd_buf_ms\":%d},\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"peak_ms\":%.2f}}",
                               prim_srt->url, prim_srt->is_srt_listener ? "true" : "false", prim_srt->srt_client_count,
                               prim_srt->bitrate_kbps, prim_srt->video_fps, prim_srt->audio_pps,
                               (long long)(stats.cur_video_pts_ms != AV_NOPTS_VALUE ? stats.cur_video_pts_ms : 0),
                               (long long)(stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? stats.cur_audio_pts_ms : 0),
                               (long long)av_drift_ms, effective_gop_sec,
                               (unsigned long long)prim_srt->packets_written, (unsigned long long)prim_srt->bytes_written,
                               prim_srt->srt_rtt_ms,
                               prim_srt->srt_nack_interval, prim_srt->srt_nack_total,
                               prim_srt->srt_loss_interval, prim_srt->srt_loss_total,
                               prim_srt->srt_retrans_interval, prim_srt->srt_retrans_total,
                               prim_srt->srt_drop_interval, prim_srt->srt_drop_total,
                               prim_srt->srt_arq_success_pct,
                               prim_srt->srt_mbps_bandwidth, prim_srt->srt_snd_buf_ms,
                               prim_srt->write_avg_ms, prim_srt->write_min_ms, prim_srt->write_max_ms,
                               (double)prim_srt->write_latency_peak_us / 1000.0);
                    } else if (num_srt_outputs > 1) {
                        printf(",\"srt_outputs\":[");
                        int count = 0;
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_SRT) continue;
                            if (count++ > 0) printf(",");
                            printf("{\"index\":%d,\"target\":\"%s\",\"is_listener\":%s,\"clients\":%d,\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"total_bytes\":%llu,\"srt_transport\":{\"rtt_ms\":%.1f,\"nack\":{\"interval\":%d,\"total\":%d},\"loss\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"retransmit\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"drop\":{\"interval_pkts\":%d,\"total_pkts\":%d},\"arq_success_pct\":%.1f,\"bandwidth_mbps\":%.2f,\"snd_buf_ms\":%d},\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f}}",
                                   out_ctxs[j].type_index, out_ctxs[j].url, out_ctxs[j].is_srt_listener ? "true" : "false",
                                   out_ctxs[j].srt_client_count, out_ctxs[j].bitrate_kbps,
                                   out_ctxs[j].video_fps, out_ctxs[j].audio_pps,
                                   (unsigned long long)out_ctxs[j].bytes_written,
                                   out_ctxs[j].srt_rtt_ms,
                                   out_ctxs[j].srt_nack_interval, out_ctxs[j].srt_nack_total,
                                   out_ctxs[j].srt_loss_interval, out_ctxs[j].srt_loss_total,
                                   out_ctxs[j].srt_retrans_interval, out_ctxs[j].srt_retrans_total,
                                   out_ctxs[j].srt_drop_interval, out_ctxs[j].srt_drop_total,
                                   out_ctxs[j].srt_arq_success_pct,
                                   out_ctxs[j].srt_mbps_bandwidth, out_ctxs[j].srt_snd_buf_ms,
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                        printf("]");
                    }

                    /* RTP Output(s) in JSON */
                    if (num_rtp_outputs == 1 && prim_rtp) {
                        printf(",\"rtp_out\":{\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"pps\":%.1f,\"video_pts_ms\":%lld,\"audio_pts_ms\":%lld,\"av_drift_ms\":%lld,\"keyframe_interval_sec\":%.2f,\"total_packets\":%llu,\"total_bytes\":%llu,\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"peak_ms\":%.2f}}",
                               prim_rtp->url, prim_rtp->bitrate_kbps, prim_rtp->video_fps, prim_rtp->audio_pps, prim_rtp->pps,
                               (long long)(stats.cur_video_pts_ms != AV_NOPTS_VALUE ? stats.cur_video_pts_ms : 0),
                               (long long)(stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? stats.cur_audio_pts_ms : 0),
                               (long long)av_drift_ms, effective_gop_sec,
                               (unsigned long long)prim_rtp->packets_written, (unsigned long long)prim_rtp->bytes_written,
                               prim_rtp->write_avg_ms, prim_rtp->write_min_ms, prim_rtp->write_max_ms,
                               (double)prim_rtp->write_latency_peak_us / 1000.0);
                    } else if (num_rtp_outputs > 1) {
                        printf(",\"rtp_outputs\":[");
                        int count = 0;
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_RTP) continue;
                            if (count++ > 0) printf(",");
                            printf("{\"index\":%d,\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"pps\":%.1f,\"total_bytes\":%llu,\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f}}",
                                   out_ctxs[j].type_index, out_ctxs[j].url, out_ctxs[j].bitrate_kbps,
                                   out_ctxs[j].video_fps, out_ctxs[j].audio_pps, out_ctxs[j].pps,
                                   (unsigned long long)out_ctxs[j].bytes_written,
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                        printf("]");
                    }

                    if (is_sdi_input) {
                        printf(",\"sdi_in\":{\"frames_read\":%llu,\"fps\":%.1f}",
                               (unsigned long long)stats.sdi_frames_read, stats.sdi_in_fps);
                    }
                    if (num_sdi_outputs > 0) {
                        printf(",\"sdi_out\":{\"frames_written\":%llu,\"fps\":%.1f}",
                               (unsigned long long)stats.sdi_frames_written, stats.sdi_out_fps);
                    }
                    if (transcoder) {
                        printf(",\"transcode\":{\"codec\":\"%s\",\"latency_ms\":%.2f}",
                               config->transcode.vcodec_name[0] ? config->transcode.vcodec_name : "auto",
                               (double)stats.transcode_latency_us / 1000.0);
                    }

                    if (monitor.enabled) {
                        printf(",\"confidence_monitor\":{\"file\":\"%s\",\"fps\":%d,\"frames_written\":%llu}",
                               config->monitor_file, config->monitor_fps, (unsigned long long)stats.monitor_frames_written);
                    }
                    printf("}\n");
                    fflush(stdout);
                } else if (config->log_telemetry) {
                    /* Standard Timestamped Console Log Entry */
                    char mon_str[64] = "";
                    if (monitor.enabled) {
                        snprintf(mon_str, sizeof(mon_str), " | Mon: %llu imgs", (unsigned long long)stats.monitor_frames_written);
                    }

                    char in_str[192];
                    if (is_sdi_input) {
                        snprintf(in_str, sizeof(in_str), "SDI-IN: %llu frames (%.1f fps)",
                                 (unsigned long long)stats.sdi_frames_read, stats.sdi_in_fps);
                    } else if (is_srt_input) {
                        snprintf(in_str, sizeof(in_str), "SRT-Ingress: %.1f kbps (%.1f pps, RTT: %.1fms, Loss: %d/s, Retrans: %d/s, NAK: %d/s, ARQ: %.1f%%)",
                                 udp_bitrate_kbps, udp_pps,
                                 stats.srt_in_rtt_ms, stats.srt_in_loss_interval,
                                 stats.srt_in_retrans_interval, stats.srt_in_nack_interval,
                                 stats.srt_in_arq_success_pct);
                    } else if (is_rtp_input) {
                        snprintf(in_str, sizeof(in_str), "RTP: %.1f kbps (Loss: %.2f%%, %llu pkts)",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct, (unsigned long long)stats.udp_lost_packets);
                    } else {
                        snprintf(in_str, sizeof(in_str), "UDP: %.1f kbps (Loss: %.2f%%, %llu pkts)",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct, (unsigned long long)stats.udp_lost_packets);
                    }

                    char rtmp_str[128] = "";
                    char hls_str[128] = "";
                    char srt_str[192] = "";
                    char rtp_out_str[128] = "";
                    char udp_out_str[128] = "";
                    char sdi_out_str[128] = "";
                    char transcode_str[64] = "";
                    if (num_sdi_outputs > 0) {
                        snprintf(sdi_out_str, sizeof(sdi_out_str), " | SDI-OUT: %llu frames (%.1f fps)",
                                 (unsigned long long)stats.sdi_frames_written, stats.sdi_out_fps);
                    }
                    if (transcoder) {
                        snprintf(transcode_str, sizeof(transcode_str), " | Tenc: %.2fms",
                                 (double)stats.transcode_latency_us / 1000.0);
                    }
                    if (num_rtmp_outputs > 0 && prim_rtmp) {
                        snprintf(rtmp_str, sizeof(rtmp_str), " | RTMP: %.1f kbps (%.1f fps, Retrans: %u, RTT: %.1fms)",
                                 prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->tcp_retrans, prim_rtmp->tcp_rtt_ms);
                    }
                    if (num_hls_outputs > 0 && prim_hls) {
                        char hls_tcp_str[64] = "";
                        if (strncmp(prim_hls->url, "http", 4) == 0) {
                            snprintf(hls_tcp_str, sizeof(hls_tcp_str), ", Retrans: %u", prim_hls->tcp_retrans);
                        }
                        snprintf(hls_str, sizeof(hls_str), " | HLS: %.1f kbps (%.1f pps, Seg #%llu%s)",
                                 prim_hls->bitrate_kbps, prim_hls->pps,
                                 (unsigned long long)prim_hls->hls_state.segments_completed, hls_tcp_str);
                    }
                    if (num_srt_outputs > 0 && prim_srt) {
                        char client_str[32] = "";
                        if (prim_srt->is_srt_listener) {
                            snprintf(client_str, sizeof(client_str), "Clients: %d, ", prim_srt->srt_client_count);
                        }
                        snprintf(srt_str, sizeof(srt_str), " | SRT-Egress: %.1f kbps (%s%.1f fps, RTT: %.1fms, Loss: %d/s, Retrans: %d/s, NAK: %d/s, ARQ: %.1f%%)",
                                 prim_srt->bitrate_kbps, client_str, prim_srt->video_fps,
                                 prim_srt->srt_rtt_ms, prim_srt->srt_loss_interval,
                                 prim_srt->srt_retrans_interval, prim_srt->srt_nack_interval,
                                 prim_srt->srt_arq_success_pct);
                    }
                    if (num_rtp_outputs > 0 && prim_rtp) {
                        snprintf(rtp_out_str, sizeof(rtp_out_str), " | RTP-OUT: %.1f kbps (%.1f fps, %.1f pps)",
                                 prim_rtp->bitrate_kbps, prim_rtp->video_fps, prim_rtp->pps);
                    }
                    if (num_udp_outputs > 0 && prim_udp) {
                        snprintf(udp_out_str, sizeof(udp_out_str), " | UDP-OUT: %.1f kbps (%.1f fps, %.1f pps)",
                                 prim_udp->bitrate_kbps, prim_udp->video_fps, prim_udp->pps);
                    }

                    if (num_rtmp_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0) {
                        LOG_INFO("%s%s%s%s%s%s%s%s%s | Lat: %.2f/%.2f/%.2fms | Silence(V/A): %lld/%lldms | Speed: %.2fx | Drift: %+lldms | Sync: %s | %s | Health: %s",
                                 in_str, rtmp_str, hls_str, srt_str, rtp_out_str, udp_out_str, sdi_out_str, transcode_str, mon_str,
                                 write_min_ms, write_avg_ms, write_max_ms,
                                 (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms,
                                 stats.cur_speed_factor, (long long)av_drift_ms, sync_type_to_string(config->sync_type),
                                 gop_str, stats.youtube_health_status);
                    } else {
                        LOG_INFO("%s%s%s%s%s%s%s%s | Lat: %.2f/%.2f/%.2fms | Speed: %.2fx | Drift: %+lldms | Sync: %s",
                                 in_str, hls_str, srt_str, rtp_out_str, udp_out_str, sdi_out_str, transcode_str, mon_str,
                                 write_min_ms, write_avg_ms, write_max_ms,
                                 stats.cur_speed_factor, (long long)av_drift_ms, sync_type_to_string(config->sync_type));
                    }
                } else if (config->detailed_telemetry) {
                    /* Multi-line Telemetry Card */
                    char uptime_str[32], total_udp_str[32];
                    format_time(uptime_sec, uptime_str, sizeof(uptime_str));
                    format_bytes(stats.udp_bytes_read, total_udp_str, sizeof(total_udp_str));

                    char title_outputs[128] = "";
                    int pos = 0;
                    if (num_sdi_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "SDI: %d active%s",
                                        num_sdi_outputs, (num_rtmp_outputs > 0 || num_hls_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0 || num_udp_outputs > 0) ? ", " : " (LIVE)");
                    }
                    if (num_rtmp_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "RTMP: %d active%s",
                                        num_rtmp_outputs, (num_hls_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0 || num_udp_outputs > 0) ? ", " : " (STREAMING)");
                    }
                    if (num_hls_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "HLS: %d active%s",
                                        num_hls_outputs, (num_srt_outputs > 0 || num_rtp_outputs > 0 || num_udp_outputs > 0) ? ", " : " (STREAMING)");
                    }
                    if (num_srt_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "SRT: %d active%s",
                                        num_srt_outputs, (num_rtp_outputs > 0 || num_udp_outputs > 0) ? ", " : " (STREAMING)");
                    }
                    if (num_rtp_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "RTP: %d active%s",
                                        num_rtp_outputs, (num_udp_outputs > 0) ? ", " : " (STREAMING)");
                    }
                    if (num_udp_outputs > 0) {
                        pos += snprintf(title_outputs + pos, sizeof(title_outputs) - pos, "UDP: %d active (MULTICAST)",
                                        num_udp_outputs);
                    }

                    printf("\n\033[1;36m┌── [INFO @ %s] (%s) ──────────────────────────────────────────────────────────\033[0m\n",
                           uptime_str, title_outputs);
                    if (is_sdi_input) {
                        printf("│  \033[1;32m[SDI Ingress]\033[0m   : %llu frames read | %5.1f fps | Port: %s\n",
                               (unsigned long long)stats.sdi_frames_read, stats.sdi_in_fps, config->input_url + 4);
                    } else if (is_srt_input) {
                        printf("│  \033[1;32m[SRT Ingress]\033[0m   : %7.1f kbps  | %5.1f pkts/s  | In: %s\n",
                               udp_bitrate_kbps, udp_pps, total_udp_str);
                        printf("│    ├─ SRT Transport: RTT: %5.1fms | Loss/s: %d (Tot: %d) | Retrans/s: %d (Tot: %d) | NAK/s: %d (Tot: %d)\n",
                               stats.srt_in_rtt_ms,
                               stats.srt_in_loss_interval, stats.srt_in_loss_total,
                               stats.srt_in_retrans_interval, stats.srt_in_retrans_total,
                               stats.srt_in_nack_interval, stats.srt_in_nack_total);
                        printf("│    └─ ARQ Health   : Success: %5.1f%% | Drops/s: %d (Tot: %d) | Rcv Rate: %5.1f Mbps | Rcv Buf: %dms\n",
                               stats.srt_in_arq_success_pct,
                               stats.srt_in_drop_interval, stats.srt_in_drop_total,
                               stats.srt_in_rcv_rate_mbps, stats.srt_in_rcv_buf_ms);
                    } else if (is_rtp_input) {
                        printf("│  \033[1;32m[RTP Ingress]\033[0m   : %7.1f kbps  | %5.1f pkts/s  | In: %s | Loss: %.2f%% (%llu dropped, %llu CC errs)\n",
                               udp_bitrate_kbps, udp_pps, total_udp_str, stats.udp_loss_pct,
                               (unsigned long long)stats.udp_lost_packets, (unsigned long long)stats.udp_cc_errors);
                    } else {
                        printf("│  \033[1;32m[UDP Ingress]\033[0m   : %7.1f kbps  | %5.1f pkts/s  | In: %s | Loss: %.2f%% (%llu dropped, %llu CC errs)\n",
                               udp_bitrate_kbps, udp_pps, total_udp_str, stats.udp_loss_pct,
                               (unsigned long long)stats.udp_lost_packets, (unsigned long long)stats.udp_cc_errors);
                    }

                    if (transcoder) {
                        printf("│  \033[1;36m[Transcoder ]\033[0m   : %s | Latency: %5.2fms | %s | %d B-frames\n",
                               transcode_get_codec_name(config->transcode.vcodec),
                               (double)stats.transcode_latency_us / 1000.0,
                               (config->transcode.rc == TRANSCODE_RC_VBR) ? "VBR" : "CBR",
                               config->transcode.b_frames);
                    }

                    /* SDI Playout Display */
                    if (num_sdi_outputs > 0) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_SDI) continue;
                            printf("│  \033[1;35m[SDI #%d Out]\033[0m    : %llu frames | %5.1f fps | Port: %s\n",
                                   out_ctxs[j].type_index, (unsigned long long)stats.sdi_frames_written,
                                   stats.sdi_out_fps, out_ctxs[j].url);
                        }
                    }

                    /* RTMP Egress Display */
                    if (num_rtmp_outputs == 1 && prim_rtmp) {
                        char total_rtmp_str[32];
                        format_bytes(prim_rtmp->bytes_written, total_rtmp_str, sizeof(total_rtmp_str));
                        printf("│  \033[1;34m[RTMP Egress]\033[0m  : %7.1f kbps  | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                               prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->audio_pps, total_rtmp_str);
                        if (prim_rtmp->tcp_fd >= 0) {
                            printf("│  \033[1;34m[TCP Uplink ]\033[0m  : Retrans: %u lost/resend | In-Flight Lost: %u | RTT: %5.1fms (Jitter: %4.1fms)\n",
                                   prim_rtmp->tcp_retrans, prim_rtmp->tcp_lost, prim_rtmp->tcp_rtt_ms, prim_rtmp->tcp_rttvar_ms);
                        }
                        printf("│  \033[1;35m[Write Latency]\033[0m : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms | Peak: %5.2fms\n",
                               prim_rtmp->write_avg_ms, prim_rtmp->write_min_ms, prim_rtmp->write_max_ms,
                               (double)prim_rtmp->write_latency_peak_us / 1000.0);
                    } else if (num_rtmp_outputs > 1) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_RTMP) continue;
                            char total_str[32];
                            format_bytes(out_ctxs[j].bytes_written, total_str, sizeof(total_str));
                            printf("│  \033[1;34m[RTMP #%d]\033[0m       : %7.1f kbps | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                                   out_ctxs[j].type_index, out_ctxs[j].bitrate_kbps, out_ctxs[j].video_fps,
                                   out_ctxs[j].audio_pps, total_str);
                            printf("│    ├─ Target   : %s\n", out_ctxs[j].url);
                            if (out_ctxs[j].tcp_fd >= 0) {
                                printf("│    ├─ Uplink   : Retrans: %u lost/resend | Lost: %u | RTT: %5.1fms (Jitter: %4.1fms)\n",
                                       out_ctxs[j].tcp_retrans, out_ctxs[j].tcp_lost, out_ctxs[j].tcp_rtt_ms, out_ctxs[j].tcp_rttvar_ms);
                            }
                            printf("│    └─ Latency  : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                    }

                    /* HLS Egress Display */
                    if (num_hls_outputs == 1 && prim_hls) {
                        char total_hls_str[32];
                        format_bytes(prim_hls->bytes_written, total_hls_str, sizeof(total_hls_str));
                        char seg_size_str[32];
                        format_bytes(prim_hls->hls_state.last_segment_size_bytes, seg_size_str, sizeof(seg_size_str));
                        const char *hls_method_name = (strncmp(prim_hls->url, "http", 4) == 0) ? config->hls_method : "FILE";

                        printf("│  \033[1;35m[HLS Egress]\033[0m\n");
                        printf("│    ├─ Target           : %s (Method: %s)\n", prim_hls->url, hls_method_name);
                        printf("│    ├─ Data Egress      : %7.1f kbps | %5.1f pkts/s | Total: %s\n",
                                prim_hls->bitrate_kbps, prim_hls->pps, total_hls_str);
                        if (prim_hls->hls_state.segments_completed > 0) {
                            printf("│    ├─ Active Segment   : %s (Size: %s, Duration: %.2fs)\n",
                                   prim_hls->hls_state.active_segment_name, seg_size_str, prim_hls->hls_state.last_segment_duration_sec);
                            printf("│    ├─ Segments Pushed  : %llu completed | Window: %d segments in playlist\n",
                                   (unsigned long long)prim_hls->hls_state.segments_completed, config->hls_list_size);
                            printf("│    ├─ %s : %.1fms per segment (%.1fx real-time margin)\n",
                                   (strncmp(prim_hls->url, "http", 4) == 0) ? "CDN Upload Speed" : "Disk Write Speed",
                                   prim_hls->hls_state.last_segment_upload_ms, prim_hls->hls_state.last_segment_speed_factor);
                        } else {
                            printf("│    ├─ Active Segment   : %s (Buffering initial segment...)\n",
                                   prim_hls->hls_state.active_segment_name[0] ? prim_hls->hls_state.active_segment_name : "starting");
                            printf("│    ├─ Segments Pushed  : 0 completed | Window: %d segments in playlist\n",
                                   config->hls_list_size);
                            printf("│    ├─ %s : Initializing...\n",
                                   (strncmp(prim_hls->url, "http", 4) == 0) ? "CDN Upload Speed" : "Disk Write Speed");
                        }
                        if (strncmp(prim_hls->url, "http", 4) == 0) {
                            printf("│    ├─ TCP Uplink (CDN) : Retrans: %u lost/resend | In-Flight Lost: %u | RTT: %5.1fms (Jitter: %4.1fms)\n",
                                   prim_hls->tcp_retrans, prim_hls->tcp_lost, prim_hls->tcp_rtt_ms, prim_hls->tcp_rttvar_ms);
                        }
                        printf("│    └─ Origin Health    : %s\n", prim_hls->hls_state.origin_health_str);
                    } else if (num_hls_outputs > 1) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_HLS) continue;
                            char total_str[32];
                            format_bytes(out_ctxs[j].bytes_written, total_str, sizeof(total_str));
                            printf("│  \033[1;35m[HLS #%d]\033[0m        : %7.1f kbps | %5.1f pkts/s | Seg: #%llu | Total: %s\n",
                                   out_ctxs[j].type_index, out_ctxs[j].bitrate_kbps, out_ctxs[j].pps,
                                   (unsigned long long)out_ctxs[j].hls_state.segments_completed, total_str);
                            printf("│    ├─ Target   : %s\n", out_ctxs[j].url);
                            if (strncmp(out_ctxs[j].url, "http", 4) == 0) {
                                printf("│    ├─ Uplink   : Retrans: %u | Lost: %u | RTT: %5.1fms (Jitter: %4.1fms)\n",
                                       out_ctxs[j].tcp_retrans, out_ctxs[j].tcp_lost, out_ctxs[j].tcp_rtt_ms, out_ctxs[j].tcp_rttvar_ms);
                            }
                            printf("│    └─ Health   : %s\n", out_ctxs[j].hls_state.origin_health_str);
                        }
                    }

                    /* SRT Egress Display */
                    if (num_srt_outputs == 1 && prim_srt) {
                        char total_srt_str[32];
                        format_bytes(prim_srt->bytes_written, total_srt_str, sizeof(total_srt_str));
                        printf("│  \033[1;33m[SRT Egress ]\033[0m  : %7.1f kbps  | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                               prim_srt->bitrate_kbps, prim_srt->video_fps, prim_srt->audio_pps, total_srt_str);
                        if (prim_srt->is_srt_listener) {
                            printf("│    ├─ Mode         : Multi-Client Listener (Active Clients: %d)\n", prim_srt->srt_client_count);
                        } else {
                            printf("│    ├─ Mode         : Point-to-Point Caller\n");
                        }
                        printf("│    ├─ Target       : %s\n", prim_srt->url);
                        printf("│    ├─ SRT Transport: RTT: %5.1fms | Loss/s: %d (Tot: %d) | Retrans/s: %d (Tot: %d) | NAK/s: %d (Tot: %d)\n",
                               prim_srt->srt_rtt_ms,
                               prim_srt->srt_loss_interval, prim_srt->srt_loss_total,
                               prim_srt->srt_retrans_interval, prim_srt->srt_retrans_total,
                               prim_srt->srt_nack_interval, prim_srt->srt_nack_total);
                        printf("│    ├─ ARQ Health   : Success: %5.1f%% | Drops/s: %d (Tot: %d) | Bandwidth: %5.1f Mbps | Snd Buf: %dms\n",
                               prim_srt->srt_arq_success_pct,
                               prim_srt->srt_drop_interval, prim_srt->srt_drop_total,
                               prim_srt->srt_mbps_bandwidth, prim_srt->srt_snd_buf_ms);
                        printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms | Peak: %5.2fms\n",
                               prim_srt->write_avg_ms, prim_srt->write_min_ms, prim_srt->write_max_ms,
                               (double)prim_srt->write_latency_peak_us / 1000.0);
                    } else if (num_srt_outputs > 1) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_SRT) continue;
                            char total_str[32];
                            format_bytes(out_ctxs[j].bytes_written, total_str, sizeof(total_str));
                            printf("│  \033[1;33m[SRT #%d]\033[0m        : %7.1f kbps | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                                   out_ctxs[j].type_index, out_ctxs[j].bitrate_kbps, out_ctxs[j].video_fps,
                                   out_ctxs[j].audio_pps, total_str);
                            if (out_ctxs[j].is_srt_listener) {
                                printf("│    ├─ Mode         : Multi-Client Listener (Active Clients: %d)\n", out_ctxs[j].srt_client_count);
                            } else {
                                printf("│    ├─ Mode         : Point-to-Point Caller\n");
                            }
                            printf("│    ├─ Target       : %s\n", out_ctxs[j].url);
                            printf("│    ├─ SRT Transport: RTT: %5.1fms | Loss/s: %d | Retrans/s: %d | NAK/s: %d | ARQ: %.1f%%\n",
                                   out_ctxs[j].srt_rtt_ms,
                                   out_ctxs[j].srt_loss_interval,
                                   out_ctxs[j].srt_retrans_interval,
                                   out_ctxs[j].srt_nack_interval,
                                   out_ctxs[j].srt_arq_success_pct);
                            printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                    }

                    /* RTP Egress Display */
                    if (num_rtp_outputs == 1 && prim_rtp) {
                        char total_rtp_out_str[32];
                        format_bytes(prim_rtp->bytes_written, total_rtp_out_str, sizeof(total_rtp_out_str));
                        printf("│  \033[1;32m[RTP Egress ]\033[0m  : %7.1f kbps  | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                               prim_rtp->bitrate_kbps, prim_rtp->video_fps, prim_rtp->audio_pps, total_rtp_out_str);
                        printf("│    ├─ Target       : %s (RFC 2250 RTP/MPEG-TS)\n", prim_rtp->url);
                        printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                               prim_rtp->write_avg_ms, prim_rtp->write_min_ms, prim_rtp->write_max_ms);
                    } else if (num_rtp_outputs > 1) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_RTP) continue;
                            char total_str[32];
                            format_bytes(out_ctxs[j].bytes_written, total_str, sizeof(total_str));
                            printf("│  \033[1;32m[RTP #%d]\033[0m        : %7.1f kbps | %5.1f fps (V) | Out: %s\n",
                                   out_ctxs[j].type_index, out_ctxs[j].bitrate_kbps, out_ctxs[j].video_fps, total_str);
                            printf("│    ├─ Target       : %s\n", out_ctxs[j].url);
                            printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                    }

                    /* UDP / Multicast Egress Display */
                    if (num_udp_outputs == 1 && prim_udp) {
                        char total_udp_out_str[32];
                        format_bytes(prim_udp->bytes_written, total_udp_out_str, sizeof(total_udp_out_str));
                        printf("│  \033[1;32m[UDP Egress ]\033[0m  : %7.1f kbps  | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                               prim_udp->bitrate_kbps, prim_udp->video_fps, prim_udp->audio_pps, total_udp_out_str);
                        printf("│    ├─ Target       : %s (Multicast TS)\n", prim_udp->url);
                        printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                               prim_udp->write_avg_ms, prim_udp->write_min_ms, prim_udp->write_max_ms);
                    } else if (num_udp_outputs > 1) {
                        for (int j = 0; j < num_outputs; j++) {
                            if (out_ctxs[j].type != OUTPUT_TYPE_UDP) continue;
                            char total_str[32];
                            format_bytes(out_ctxs[j].bytes_written, total_str, sizeof(total_str));
                            printf("│  \033[1;32m[UDP #%d]\033[0m        : %7.1f kbps | %5.1f fps (V) | Out: %s\n",
                                   out_ctxs[j].type_index, out_ctxs[j].bitrate_kbps, out_ctxs[j].video_fps, total_str);
                            printf("│    ├─ Target       : %s\n", out_ctxs[j].url);
                            printf("│    └─ Latency      : Avg: %5.2fms | Min: %5.2fms | Max: %5.2fms\n",
                                   out_ctxs[j].write_avg_ms, out_ctxs[j].write_min_ms, out_ctxs[j].write_max_ms);
                        }
                    }

                    printf("│  \033[1;37m[Stream Pacing]\033[0m : Speed: %4.2fx | Max Gap: %5.1fms | Wall Lead: %+lldms\n",
                           stats.cur_speed_factor, max_gap_ms, (long long)stats.cur_wall_lead_ms);
                    printf("│  \033[1;35m[Track Silence]\033[0m : Video: %4lldms | Audio: %4lldms\n",
                           (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms);
                    printf("│  \033[1;36m[A/V & Sync ]\033[0m   : Video PTS: %6.2fs | Audio PTS: %6.2fs | Drift: %+lld ms (Mode: %s)\n",
                           (stats.cur_video_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_video_pts_ms / 1000.0 : 0.0),
                           (stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_audio_pts_ms / 1000.0 : 0.0),
                           (long long)av_drift_ms, sync_type_to_string(config->sync_type));

                    if (num_rtmp_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0) {
                        const char *health_color = (strcmp(stats.youtube_health_status, "HEALTHY") == 0) ? "\033[1;32m" :
                            (strstr(stats.youtube_health_status, "BUFFER RISK") ? "\033[1;31m" : "\033[1;33m");
                        printf("│  \033[1;33m[Stream Health ]\033[0m: %s%s\033[0m (%s | KF Age: %.1fs | Lat: %.1fms)\n",
                               health_color, stats.youtube_health_status,
                               gop_str, stats.cur_keyframe_age_sec, write_avg_ms);
                    }

                    if (monitor.enabled) {
                        printf("│  \033[1;33m[Confidence Mon]\033[0m: %llu frames (%d fps) -> %s\n",
                               (unsigned long long)stats.monitor_frames_written, config->monitor_fps, config->monitor_file);
                    }
                    printf("\033[1;36m└─────────────────────────────────────────────────────────────────────────────────────────\033[0m\n");
                    fflush(stdout);
                } else {
                    /* Compact Single-line Interactive Live Dashboard */
                    char uptime_str[32];
                    format_time(uptime_sec, uptime_str, sizeof(uptime_str));

                    char mon_badge[64] = "";
                    if (monitor.enabled) {
                        snprintf(mon_badge, sizeof(mon_badge), " │ \033[33mMon:\033[0m %llu", (unsigned long long)stats.monitor_frames_written);
                    }

                    char in_badge[96];
                    if (is_sdi_input) {
                        snprintf(in_badge, sizeof(in_badge), "\033[32mSDI-IN:\033[0m %llu f (%4.1f fps)",
                                 (unsigned long long)stats.sdi_frames_read, stats.sdi_in_fps);
                    } else if (is_srt_input) {
                        if (stats.srt_in_loss_interval > 0 || stats.srt_in_retrans_interval > 0) {
                            snprintf(in_badge, sizeof(in_badge), "\033[32mSRT-Ingress:\033[0m %5.0f kbps (\033[33mRTT:%.0fms L:%d R:%d ARQ:%.0f%%\033[0m)",
                                     udp_bitrate_kbps, stats.srt_in_rtt_ms, stats.srt_in_loss_interval,
                                     stats.srt_in_retrans_interval, stats.srt_in_arq_success_pct);
                        } else {
                            snprintf(in_badge, sizeof(in_badge), "\033[32mSRT-Ingress:\033[0m %5.0f kbps (\033[36mRTT:%.0fms\033[0m)",
                                     udp_bitrate_kbps, stats.srt_in_rtt_ms);
                        }
                    } else if (is_rtp_input) {
                        if (stats.cur_interval_loss_pct > 0.05) {
                            snprintf(in_badge, sizeof(in_badge), "\033[32mRTP:\033[0m %5.0f kbps (\033[31mLoss: %.1f%%\033[0m)",
                                     udp_bitrate_kbps, stats.cur_interval_loss_pct);
                        } else {
                            snprintf(in_badge, sizeof(in_badge), "\033[32mRTP:\033[0m %5.0f kbps", udp_bitrate_kbps);
                        }
                    } else if (stats.cur_interval_loss_pct > 0.05) {
                        snprintf(in_badge, sizeof(in_badge), "\033[32mUDP:\033[0m %5.0f kbps (\033[31mLoss: %.1f%%\033[0m)",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct);
                    } else {
                        snprintf(in_badge, sizeof(in_badge), "\033[32mUDP:\033[0m %5.0f kbps", udp_bitrate_kbps);
                    }

                    char rtmp_badge[64] = "";
                    if (num_rtmp_outputs == 1 && prim_rtmp) {
                        snprintf(rtmp_badge, sizeof(rtmp_badge), " │ \033[34mRTMP:\033[0m %5.0f kbps (%4.1f fps)",
                                 prim_rtmp->bitrate_kbps, prim_rtmp->video_fps);
                    } else if (num_rtmp_outputs > 1) {
                        snprintf(rtmp_badge, sizeof(rtmp_badge), " │ \033[34mRTMP (%d):\033[0m %5.0f kbps",
                                 num_rtmp_outputs, prim_rtmp ? prim_rtmp->bitrate_kbps : 0.0);
                    }

                    char hls_badge[64] = "";
                    if (num_hls_outputs == 1 && prim_hls) {
                        snprintf(hls_badge, sizeof(hls_badge), " │ \033[35mHLS:\033[0m %5.0f kbps (Seg #%llu)",
                                 prim_hls->bitrate_kbps, (unsigned long long)prim_hls->hls_state.segments_completed);
                    } else if (num_hls_outputs > 1) {
                        snprintf(hls_badge, sizeof(hls_badge), " │ \033[35mHLS (%d):\033[0m %5.0f kbps",
                                 num_hls_outputs, prim_hls ? prim_hls->bitrate_kbps : 0.0);
                    }

                    char srt_badge[128] = "";
                    if (num_srt_outputs == 1 && prim_srt) {
                        if (prim_srt->srt_loss_interval > 0 || prim_srt->srt_retrans_interval > 0) {
                            snprintf(srt_badge, sizeof(srt_badge), " │ \033[33mSRT-Egress:\033[0m %5.0f kbps (%4.1f fps \033[31mRTT:%.0fms L:%d R:%d ARQ:%.0f%%\033[0m)",
                                     prim_srt->bitrate_kbps, prim_srt->video_fps,
                                     prim_srt->srt_rtt_ms, prim_srt->srt_loss_interval,
                                     prim_srt->srt_retrans_interval, prim_srt->srt_arq_success_pct);
                        } else {
                            snprintf(srt_badge, sizeof(srt_badge), " │ \033[33mSRT-Egress:\033[0m %5.0f kbps (%4.1f fps \033[36mRTT:%.0fms\033[0m)",
                                     prim_srt->bitrate_kbps, prim_srt->video_fps, prim_srt->srt_rtt_ms);
                        }
                    } else if (num_srt_outputs > 1) {
                        snprintf(srt_badge, sizeof(srt_badge), " │ \033[33mSRT-Egress (%d):\033[0m %5.0f kbps (\033[36mRTT:%.0fms\033[0m)",
                                 num_srt_outputs, prim_srt ? prim_srt->bitrate_kbps : 0.0, prim_srt ? prim_srt->srt_rtt_ms : 0.0);
                    }

                    char rtp_out_badge[64] = "";
                    if (num_rtp_outputs == 1 && prim_rtp) {
                        snprintf(rtp_out_badge, sizeof(rtp_out_badge), " │ \033[32mRTP-OUT:\033[0m %5.0f kbps (%4.1f fps)",
                                 prim_rtp->bitrate_kbps, prim_rtp->video_fps);
                    } else if (num_rtp_outputs > 1) {
                        snprintf(rtp_out_badge, sizeof(rtp_out_badge), " │ \033[32mRTP-OUT (%d):\033[0m %5.0f kbps",
                                 num_rtp_outputs, prim_rtp ? prim_rtp->bitrate_kbps : 0.0);
                    }

                    char udp_out_badge[64] = "";
                    if (num_udp_outputs == 1 && prim_udp) {
                        snprintf(udp_out_badge, sizeof(udp_out_badge), " │ \033[32mUDP-OUT:\033[0m %5.0f kbps (%4.1f fps)",
                                 prim_udp->bitrate_kbps, prim_udp->video_fps);
                    } else if (num_udp_outputs > 1) {
                        snprintf(udp_out_badge, sizeof(udp_out_badge), " │ \033[32mUDP-OUT (%d):\033[0m %5.0f kbps",
                                 num_udp_outputs, prim_udp ? prim_udp->bitrate_kbps : 0.0);
                    }

                    char sdi_out_badge[64] = "";
                    if (num_sdi_outputs > 0) {
                        snprintf(sdi_out_badge, sizeof(sdi_out_badge), " │ \033[36mSDI:\033[0m %4.1f fps", stats.sdi_out_fps);
                    }
                    char trans_badge[64] = "";
                    if (transcoder) {
                        snprintf(trans_badge, sizeof(trans_badge), " │ \033[33mTenc:\033[0m %4.1fms", (double)stats.transcode_latency_us / 1000.0);
                    }

                    char status_badge[128];
                    if (num_rtmp_outputs > 0 || num_srt_outputs > 0 || num_rtp_outputs > 0) {
                        if (strcmp(stats.youtube_health_status, "HEALTHY") == 0) {
                            snprintf(status_badge, sizeof(status_badge), "\033[32mHEALTHY 🟢\033[0m");
                        } else if (strstr(stats.youtube_health_status, "BUFFER RISK")) {
                            snprintf(status_badge, sizeof(status_badge), "\033[31m%s 🔴\033[0m", stats.youtube_health_status);
                        } else {
                            snprintf(status_badge, sizeof(status_badge), "\033[33m%s 🟡\033[0m", stats.youtube_health_status);
                        }
                    } else {
                        snprintf(status_badge, sizeof(status_badge), "\033[32mOK 🟢\033[0m");
                    }

                    printf("\r\033[36m[%s]\033[0m %s%s%s%s%s%s%s%s%s │ \033[1;37mSpd:\033[0m %4.2fx │ %s   ",
                           uptime_str, in_badge, rtmp_badge, hls_badge, srt_badge, rtp_out_badge, udp_out_badge, sdi_out_badge, trans_badge, mon_badge,
                           stats.cur_speed_factor, status_badge);
                    fflush(stdout);
                }

                stats.last_stats_time_us = now_us;
                stats.last_stats_video_pts_ms = stats.cur_video_pts_ms;
                stats.max_pacing_gap_us = 0;
                stats.last_udp_bytes = stats.udp_bytes_read;
                stats.last_udp_packets = stats.udp_packets_read;
                stats.last_udp_lost_packets = stats.udp_lost_packets;

                for (int j = 0; j < num_outputs; j++) {
                    out_ctxs[j].last_bytes = out_ctxs[j].bytes_written;
                    out_ctxs[j].last_packets = out_ctxs[j].packets_written;
                    out_ctxs[j].last_video_pkts = out_ctxs[j].video_packets_written;
                    out_ctxs[j].last_audio_pkts = out_ctxs[j].audio_packets_written;
                    out_ctxs[j].write_latency_min_us = INT64_MAX;
                    out_ctxs[j].write_latency_max_us = 0;
                    out_ctxs[j].write_latency_sum_us = 0;
                    out_ctxs[j].write_latency_count = 0;
                }
            }
        }
    }

    printf("\n");
    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].type == OUTPUT_TYPE_SDI && out_ctxs[i].sdi_out) {
            sdi_output_close(&out_ctxs[i].sdi_out);
        } else if (out_ctxs[i].ofmt_ctx) {
            if (out_ctxs[i].type == OUTPUT_TYPE_RTMP) {
                rtmp_close_egress(&out_ctxs[i]);
            } else if (out_ctxs[i].type == OUTPUT_TYPE_HLS) {
                hls_close_egress(&out_ctxs[i]);
            } else if (out_ctxs[i].type == OUTPUT_TYPE_SRT) {
                srt_close_egress(&out_ctxs[i]);
            } else if (out_ctxs[i].type == OUTPUT_TYPE_RTP) {
                LOG_INFO("[RTP #%d] Finalizing RTP stream and writing trailer...", out_ctxs[i].type_index);
                av_write_trailer(out_ctxs[i].ofmt_ctx);
            } else if (out_ctxs[i].type == OUTPUT_TYPE_UDP) {
                LOG_INFO("[UDP #%d] Finalizing MPEG-TS stream and writing trailer...", out_ctxs[i].type_index);
                av_write_trailer(out_ctxs[i].ofmt_ctx);
            }
        }
    }

cleanup:
    confidence_monitor_cleanup(&monitor);
    av_log_set_callback(av_log_default_callback);
    LOG_INFO("Cleaning up pipeline resources...");
    if (dec_frame) {
        av_frame_free(&dec_frame);
    }
    if (enc_pkt) {
        av_packet_free(&enc_pkt);
    }
    if (dec_aframe) {
        av_frame_free(&dec_aframe);
    }
    if (enc_a_pkt) {
        av_packet_free(&enc_a_pkt);
    }
    if (transcoder) {
        transcode_engine_free(&transcoder);
    }
    if (a_transcoder) {
        audio_transcode_engine_free(&a_transcoder);
    }
    if (sdi_in) {
        sdi_input_close(&sdi_in);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].sdi_out) {
            sdi_output_close(&out_ctxs[i].sdi_out);
        }
        if (out_ctxs[i].pkt) {
            av_packet_free(&out_ctxs[i].pkt);
        }
        if (out_ctxs[i].ofmt_ctx) {
            if (!(out_ctxs[i].ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                if (out_ctxs[i].ofmt_ctx->flags & AVFMT_FLAG_CUSTOM_IO) {
                    if (out_ctxs[i].ofmt_ctx->pb) {
                        if (out_ctxs[i].ofmt_ctx->pb->buffer) {
                            av_freep(&out_ctxs[i].ofmt_ctx->pb->buffer);
                        }
                        avio_context_free(&out_ctxs[i].ofmt_ctx->pb);
                    }
                } else {
                    avio_closep(&out_ctxs[i].ofmt_ctx->pb);
                }
            }
            avformat_free_context(out_ctxs[i].ofmt_ctx);
            out_ctxs[i].ofmt_ctx = NULL;
        }
    }
    if (stream_mapping) {
        av_freep(&stream_mapping);
    }
    if (ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }
    avformat_network_deinit();
    LOG_INFO("All egress outputs closed. Pipeline stopped cleanly.");
    return ret;
}
