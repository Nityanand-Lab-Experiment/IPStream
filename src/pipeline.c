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

#define MAX_STREAMS 32

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

static int find_tcp_socket_fd(const int *exclude_fds, int num_exclude) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *entry;
    int found_fd = -1;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        int fd = atoi(entry->d_name);
        bool excluded = false;
        if (exclude_fds && num_exclude > 0) {
            for (int i = 0; i < num_exclude; i++) {
                if (exclude_fds[i] >= 0 && fd == exclude_fds[i]) {
                    excluded = true;
                    break;
                }
            }
        }
        if (excluded) continue;
        struct tcp_info info;
        socklen_t len = sizeof(info);
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0 && info.tcpi_state == TCP_ESTABLISHED) {
            found_fd = fd;
            break;
        }
    }
    closedir(d);
    return found_fd;
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
    config->monitor_file[0] = '\0';
    config->monitor_fps = 5;
    config->monitor_width = 0;
    config->monitor_height = 0;
    config->monitor_quality = 4;
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
                                    AVFormatContext *ifmt_ctx, int video_stream_idx) {
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

    if (video_stream_idx < 0 || video_stream_idx >= (int)ifmt_ctx->nb_streams) {
        LOG_WARN("[MONITOR] Video stream index invalid, confidence monitor disabled");
        mon->enabled = false;
        return;
    }

    AVStream *vstream = ifmt_ctx->streams[video_stream_idx];
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
    mon->jpeg_pkt = av_packet_alloc();
    if (!mon->raw_frame || !mon->jpeg_pkt) {
        LOG_WARN("[MONITOR] Failed to allocate frames/packets for confidence monitor");
        mon->enabled = false;
        return;
    }

    if (vstream->codecpar->width > 0 && vstream->codecpar->height > 0) {
        confidence_monitor_init_encoder(mon, vstream->codecpar->width, vstream->codecpar->height);
    }

    LOG_INFO("[MONITOR] Confidence monitor active: %s (Target: %d fps, Q: %d)",
             mon->filepath, mon->fps, mon->quality);
}

static void confidence_monitor_process(ConfidenceMonitor *mon, const AVPacket *pkt, PipelineStats *stats) {
    if (!mon || !mon->enabled || !mon->dec_ctx) return;

    int ret = avcodec_send_packet(mon->dec_ctx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return;
    }

    while (avcodec_receive_frame(mon->dec_ctx, mon->raw_frame) == 0) {
        int64_t now_us = get_time_us();
        if (now_us - mon->last_capture_time_us >= mon->interval_us) {
            mon->last_capture_time_us = now_us;

            if (!mon->enc_ctx) {
                confidence_monitor_init_encoder(mon, mon->raw_frame->width, mon->raw_frame->height);
            }
            if (!mon->enc_ctx || !mon->scaled_frame) continue;

            if (mon->sws_ctx && (mon->last_src_w != mon->raw_frame->width ||
                                 mon->last_src_h != mon->raw_frame->height ||
                                 mon->last_src_fmt != mon->raw_frame->format)) {
                sws_freeContext(mon->sws_ctx);
                mon->sws_ctx = NULL;
            }
            if (!mon->sws_ctx) {
                mon->last_src_w = mon->raw_frame->width;
                mon->last_src_h = mon->raw_frame->height;
                mon->last_src_fmt = mon->raw_frame->format;
                mon->sws_ctx = sws_getContext(
                    mon->raw_frame->width, mon->raw_frame->height, mon->raw_frame->format,
                    mon->enc_ctx->width, mon->enc_ctx->height, mon->enc_ctx->pix_fmt,
                    SWS_BILINEAR, NULL, NULL, NULL);
            }

            if (mon->sws_ctx) {
                sws_scale(mon->sws_ctx,
                          (const uint8_t * const *)mon->raw_frame->data, mon->raw_frame->linesize,
                          0, mon->raw_frame->height,
                          mon->scaled_frame->data, mon->scaled_frame->linesize);
                mon->scaled_frame->pts = mon->frames_written;

                ret = avcodec_send_frame(mon->enc_ctx, mon->scaled_frame);
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

typedef struct {
    char target_url[1024];
    char method[16];
    int target_duration_sec;
    int list_size;

    char active_segment_name[256];
    int64_t active_segment_open_time_us;
    int64_t active_segment_start_pts;
    int64_t cur_video_pts;
    AVRational video_time_base;

    uint64_t segments_completed;
    int64_t last_segment_size_bytes;
    double last_segment_duration_sec;
    double last_segment_upload_ms;
    double last_segment_speed_factor;

    uint64_t http_errors;
    uint64_t http_retries;
    int last_http_status;
    char origin_health_str[128];

    int tcp_fd;
    int exclude_fds[MAX_OUTPUTS];
    int num_exclude_fds;
    uint32_t base_retrans;
    uint32_t base_lost;
    uint32_t last_socket_retrans;
    uint32_t last_socket_lost;
    uint32_t total_retrans;
    uint32_t total_lost;
    double rtt_ms;
    double rttvar_ms;

    int (*orig_io_open)(struct AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options);
    int (*orig_io_close2)(struct AVFormatContext *s, AVIOContext *pb);
} HlsTelemetryState;

typedef struct {
    OutputType type;
    char url[1024];
    int index;                 /* Global output index */
    int type_index;            /* 1-based index within its type (e.g. RTMP #1, RTMP #2) */
    AVFormatContext *ofmt_ctx;
    AVPacket *pkt;
    int64_t stream_last_dts[MAX_STREAMS];

    int tcp_fd;
    int64_t conn_time_ms;
    int64_t handshake_time_ms;

    uint64_t packets_written;
    uint64_t video_packets_written;
    uint64_t audio_packets_written;
    uint64_t bytes_written;

    uint64_t last_bytes;
    uint64_t last_packets;
    uint64_t last_video_pkts;
    uint64_t last_audio_pkts;
    int64_t last_write_time_us;
    int64_t last_video_write_time_us;
    int64_t last_audio_write_time_us;

    int64_t write_latency_min_us;
    int64_t write_latency_max_us;
    int64_t write_latency_sum_us;
    uint64_t write_latency_count;
    int64_t write_latency_peak_us;

    uint32_t tcp_retrans;
    uint32_t tcp_lost;
    double tcp_rtt_ms;
    double tcp_rttvar_ms;

    double bitrate_kbps;
    double pps;
    double video_fps;
    double audio_pps;
    double write_avg_ms;
    double write_min_ms;
    double write_max_ms;

    HlsTelemetryState hls_state;
} OutputContext;

static void update_hls_tcp_info(HlsTelemetryState *hls) {
    if (!hls) return;
    if (strncmp(hls->target_url, "http://", 7) != 0 && strncmp(hls->target_url, "https://", 8) != 0) {
        hls->tcp_fd = -1;
        return;
    }

    struct tcp_info info;
    socklen_t len = sizeof(info);

    bool valid = false;
    if (hls->tcp_fd >= 0) {
        if (getsockopt(hls->tcp_fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
            valid = (info.tcpi_state == TCP_ESTABLISHED);
        }
    }

    if (!valid) {
        if (hls->tcp_fd >= 0) {
            hls->base_retrans += hls->last_socket_retrans;
            hls->base_lost += hls->last_socket_lost;
            hls->last_socket_retrans = 0;
            hls->last_socket_lost = 0;
            hls->tcp_fd = -1;
        }
        hls->total_retrans = hls->base_retrans;
        hls->total_lost = hls->base_lost;
        int new_fd = find_tcp_socket_fd(hls->exclude_fds, hls->num_exclude_fds);
        if (new_fd >= 0 && getsockopt(new_fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
            if (info.tcpi_state == TCP_ESTABLISHED) {
                hls->tcp_fd = new_fd;
                valid = true;
            }
        }
    }

    if (valid) {
        hls->last_socket_retrans = info.tcpi_total_retrans;
        hls->last_socket_lost = info.tcpi_lost;
        hls->total_retrans = hls->base_retrans + info.tcpi_total_retrans;
        hls->total_lost = hls->base_lost + info.tcpi_lost;
        hls->rtt_ms = (double)info.tcpi_rtt / 1000.0;
        hls->rttvar_ms = (double)info.tcpi_rttvar / 1000.0;
    }
}

static int hls_custom_io_open(struct AVFormatContext *s, AVIOContext **pb, const char *url,
                              int flags, AVDictionary **options) {
    HlsTelemetryState *hls = (s && s->opaque) ? (HlsTelemetryState *)s->opaque : NULL;
    int ret = -1;

    if (hls && hls->orig_io_open) {
        ret = hls->orig_io_open(s, pb, url, flags, options);
    } else {
        ret = avio_open2(pb, url, flags, &s->interrupt_callback, options);
    }

    if (hls && url) {
        const char *dot = strrchr(url, '.');
        bool is_segment = (dot && (strcmp(dot, ".ts") == 0 || strcmp(dot, ".m4s") == 0));

        if (is_segment) {
            const char *slash = strrchr(url, '/');
            const char *name = slash ? slash + 1 : url;
            snprintf(hls->active_segment_name, sizeof(hls->active_segment_name), "%s", name);
            hls->active_segment_open_time_us = get_time_us();
            hls->active_segment_start_pts = hls->cur_video_pts;
        }

        if (ret < 0) {
            hls->http_errors++;
            snprintf(hls->origin_health_str, sizeof(hls->origin_health_str),
                     "ERROR %d (%llu errors)", ret, (unsigned long long)hls->http_errors);
        }
    }

    return ret;
}

static int hls_custom_io_close2(struct AVFormatContext *s, AVIOContext *pb) {
    HlsTelemetryState *hls = (s && s->opaque) ? (HlsTelemetryState *)s->opaque : NULL;
    int64_t seg_bytes = 0;
    if (pb) {
        seg_bytes = avio_tell(pb);
    }

    if (hls && (strncmp(hls->target_url, "http://", 7) == 0 || strncmp(hls->target_url, "https://", 8) == 0)) {
        update_hls_tcp_info(hls);
    }

    int ret = 0;
    if (hls && hls->orig_io_close2) {
        ret = hls->orig_io_close2(s, pb);
    } else if (pb) {
        ret = avio_closep(&pb);
    }

    if (hls) {
        int64_t now_us = get_time_us();
        if (hls->active_segment_open_time_us > 0) {
            int64_t elapsed_us = now_us - hls->active_segment_open_time_us;
            hls->last_segment_upload_ms = (double)elapsed_us / 1000.0;
            if (hls->last_segment_upload_ms < 0.1) hls->last_segment_upload_ms = 0.1;

            if (seg_bytes > 0) {
                hls->last_segment_size_bytes = seg_bytes;
            }

            if (hls->cur_video_pts != AV_NOPTS_VALUE && hls->active_segment_start_pts != AV_NOPTS_VALUE &&
                hls->cur_video_pts > hls->active_segment_start_pts && hls->video_time_base.den > 0) {
                hls->last_segment_duration_sec = (double)(hls->cur_video_pts - hls->active_segment_start_pts) *
                                                 av_q2d(hls->video_time_base);
            } else {
                hls->last_segment_duration_sec = (double)hls->target_duration_sec;
            }
            if (hls->last_segment_duration_sec <= 0.0) {
                hls->last_segment_duration_sec = (double)hls->target_duration_sec;
            }

            hls->last_segment_speed_factor = (hls->last_segment_duration_sec * 1000.0) / hls->last_segment_upload_ms;
            hls->segments_completed++;
            hls->active_segment_open_time_us = 0;

            if (ret < 0) {
                hls->http_errors++;
                snprintf(hls->origin_health_str, sizeof(hls->origin_health_str),
                         "UPLOAD ERR (%llu errors)", (unsigned long long)hls->http_errors);
            } else {
                if (strncmp(hls->target_url, "http://", 7) == 0 || strncmp(hls->target_url, "https://", 8) == 0) {
                    snprintf(hls->origin_health_str, sizeof(hls->origin_health_str),
                             "HTTP 200 OK (%llu errors, %llu retries)",
                             (unsigned long long)hls->http_errors, (unsigned long long)hls->http_retries);
                } else {
                    snprintf(hls->origin_health_str, sizeof(hls->origin_health_str),
                             "HTTP 200 OK (0 errors, 0 retries)");
                }
            }
        }
    }

    return ret;
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

    int *stream_mapping = NULL;
    int nb_mapped_streams = 0;
    int video_stream_idx = -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};

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

    /* 2. Configure UDP Multicast Input Options */
    AVDictionary *input_opts = NULL;
    char opt_str[64];

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
    int64_t udp_start_us = get_time_us();
    ret = avformat_open_input(&ifmt_ctx, config->input_url, NULL, &input_opts);
    av_dict_free(&input_opts);

    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("[UDP STATUS] Failed to bind/open multicast input '%s': %s", config->input_url, err_buf);
        LOG_ERROR("[UDP DIAGNOSTIC] Check if IGMP join is allowed or if another process is holding port.");
        goto cleanup;
    }
    int64_t udp_conn_time_ms = (get_time_us() - udp_start_us) / 1000;
    LOG_INFO("[UDP STATUS] Multicast socket bound and active in %lld ms.", udp_conn_time_ms);

    /* 3. Probe stream information */
    LOG_INFO("Probing input stream information...");
    interrupt_ctx.last_packet_time_us = av_gettime_relative();
    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("Failed to retrieve input stream info: %s", err_buf);
        goto cleanup;
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
    }
    if (num_targets == 0) {
        LOG_ERROR("No output destination configured! Specify -o rtmp <url> or -o hls <url>");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    num_outputs = num_targets;

    int rtmp_counter = 0;
    int hls_counter = 0;
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
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = &interrupt_ctx;
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
            out_ctxs[i].ofmt_ctx->interrupt_callback.callback = interrupt_cb;
            out_ctxs[i].ofmt_ctx->interrupt_callback.opaque = &interrupt_ctx;

            HlsTelemetryState *hls = &out_ctxs[i].hls_state;
            hls->tcp_fd = -1;
            snprintf(hls->target_url, sizeof(hls->target_url), "%s", out_ctxs[i].url);
            snprintf(hls->method, sizeof(hls->method), "%s", config->hls_method);
            hls->target_duration_sec = config->hls_time > 0 ? config->hls_time : 2;
            hls->list_size = config->hls_list_size > 0 ? config->hls_list_size : 5;
            hls->active_segment_start_pts = AV_NOPTS_VALUE;
            hls->cur_video_pts = AV_NOPTS_VALUE;
            strncpy(hls->origin_health_str, "HTTP 200 OK (0 errors, 0 retries)", sizeof(hls->origin_health_str) - 1);

            hls->orig_io_open = out_ctxs[i].ofmt_ctx->io_open;
            hls->orig_io_close2 = out_ctxs[i].ofmt_ctx->io_close2;
            out_ctxs[i].ofmt_ctx->io_open = hls_custom_io_open;
            out_ctxs[i].ofmt_ctx->io_close2 = hls_custom_io_close2;
            out_ctxs[i].ofmt_ctx->opaque = hls;
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
            if (num_rtmp_outputs > 0 && in_codecpar->codec_id != AV_CODEC_ID_AAC &&
                in_codecpar->codec_id != AV_CODEC_ID_MP3) {
                const AVCodecDescriptor *d = avcodec_descriptor_get(in_codecpar->codec_id);
                LOG_WARN("Audio codec '%s' may not be supported by standard FLV/RTMP without transcoding!",
                         d ? d->name : "unknown");
            }
        }

        for (int j = 0; j < num_outputs; j++) {
            AVStream *out_stream = avformat_new_stream(out_ctxs[j].ofmt_ctx, NULL);
            if (!out_stream) {
                LOG_ERROR("Failed to allocate output stream for output #%d input #%d", j, i);
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
            if (ret < 0) {
                LOG_ERROR("Failed to copy codec parameters for stream #%d", i);
                goto cleanup;
            }
            out_stream->codecpar->codec_tag = 0;
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "[%s #%d Output]",
                     out_ctxs[j].type == OUTPUT_TYPE_RTMP ? "RTMP" : "HLS", out_ctxs[j].type_index);
            log_stream_info(out_stream, out_stream->index, prefix);
        }

        stream_mapping[i] = nb_mapped_streams++;
    }

    if (nb_mapped_streams == 0) {
        LOG_ERROR("No audio or video streams found in input source!");
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }

    /* Initialize Confidence Monitor (if configured) */
    if (config->monitor_file[0] != '\0') {
        confidence_monitor_init(&monitor, config, ifmt_ctx, video_stream_idx);
    }

    /* 6. Open Output Connections */
    int established_tcp_fds[MAX_OUTPUTS];
    int num_established_tcp = 0;

    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].type == OUTPUT_TYPE_RTMP) {
            AVDictionary *output_opts = NULL;
            av_dict_set(&output_opts, "flvflags", "no_duration_filesize+aac_seq_header_detect", 0);
            av_dict_set(&output_opts, "rtmp_live", "live", 0);
            av_dict_set(&output_opts, "rtmp_buffer", "1000", 0);

            int64_t conn_start_us = get_time_us();
            if (!(out_ctxs[i].ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                LOG_INFO("[RTMP #%d] Initiating connection to: %s ...", out_ctxs[i].type_index, out_ctxs[i].url);
                interrupt_ctx.last_packet_time_us = av_gettime_relative();
                ret = avio_open2(&out_ctxs[i].ofmt_ctx->pb, out_ctxs[i].url, AVIO_FLAG_WRITE,
                                 &out_ctxs[i].ofmt_ctx->interrupt_callback, &output_opts);
                if (ret < 0) {
                    av_strerror(ret, err_buf, sizeof(err_buf));
                    LOG_ERROR("[RTMP #%d] Connection Failed to '%s': %s", out_ctxs[i].type_index, out_ctxs[i].url, err_buf);
                    av_dict_free(&output_opts);
                    goto cleanup;
                }
            }
            out_ctxs[i].conn_time_ms = (get_time_us() - conn_start_us) / 1000;
            int fd = find_tcp_socket_fd(established_tcp_fds, num_established_tcp);
            if (fd >= 0) {
                out_ctxs[i].tcp_fd = fd;
                established_tcp_fds[num_established_tcp++] = fd;
                LOG_INFO("[RTMP #%d] Connected: TCP socket (fd=%d) established in %lld ms. TCP network loss & RTT active.",
                         out_ctxs[i].type_index, fd, out_ctxs[i].conn_time_ms);
            } else {
                LOG_INFO("[RTMP #%d] Connected: TCP socket established in %lld ms.",
                         out_ctxs[i].type_index, out_ctxs[i].conn_time_ms);
            }

            int64_t handshake_start_us = get_time_us();
            LOG_INFO("[RTMP #%d] Performing RTMP handshake & publishing stream metadata...", out_ctxs[i].type_index);
            ret = avformat_write_header(out_ctxs[i].ofmt_ctx, &output_opts);
            av_dict_free(&output_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[RTMP #%d] Handshake / Stream Header rejected: %s", out_ctxs[i].type_index, err_buf);
                goto cleanup;
            }
            out_ctxs[i].handshake_time_ms = (get_time_us() - handshake_start_us) / 1000;
            LOG_INFO("[RTMP #%d] Handshake confirmed in %lld ms! RTMP state: STREAMING.",
                     out_ctxs[i].type_index, out_ctxs[i].handshake_time_ms);
        } else if (out_ctxs[i].type == OUTPUT_TYPE_HLS) {
            AVDictionary *hls_opts = NULL;
            char hls_time_str[16], hls_list_size_str[16];
            snprintf(hls_time_str, sizeof(hls_time_str), "%d", config->hls_time > 0 ? config->hls_time : 2);
            snprintf(hls_list_size_str, sizeof(hls_list_size_str), "%d", config->hls_list_size > 0 ? config->hls_list_size : 5);

            av_dict_set(&hls_opts, "hls_time", hls_time_str, 0);
            av_dict_set(&hls_opts, "hls_list_size", hls_list_size_str, 0);
            if (config->hls_delete_segments) {
                av_dict_set(&hls_opts, "hls_flags", "delete_segments+split_by_time+omit_endlist", 0);
            } else {
                av_dict_set(&hls_opts, "hls_flags", "split_by_time+omit_endlist", 0);
            }

            if (config->hls_method[0] != '\0') {
                av_dict_set(&hls_opts, "method", config->hls_method, 0);
            }

            if (strncmp(out_ctxs[i].url, "http://", 7) == 0 || strncmp(out_ctxs[i].url, "https://", 8) == 0) {
                av_dict_set(&hls_opts, "http_persistent", "1", 0);
            }

            if (config->hls_fmp4) {
                av_dict_set(&hls_opts, "hls_segment_type", "fmp4", 0);
            }

            if (config->hls_segment_filename[0] != '\0') {
                av_dict_set(&hls_opts, "hls_segment_filename", config->hls_segment_filename, 0);
            } else if (strncmp(out_ctxs[i].url, "http://", 7) == 0 || strncmp(out_ctxs[i].url, "https://", 8) == 0) {
                char seg_url[2048];
                const char *dot = strrchr(out_ctxs[i].url, '.');
                const char *ext = config->hls_fmp4 ? ".m4s" : ".ts";
                if (dot && strcmp(dot, ".m3u8") == 0) {
                    int prefix_len = dot - out_ctxs[i].url;
                    snprintf(seg_url, sizeof(seg_url), "%.*s_%%05d%s", prefix_len, out_ctxs[i].url, ext);
                } else {
                    snprintf(seg_url, sizeof(seg_url), "%s_%%05d%s", out_ctxs[i].url, ext);
                }
                av_dict_set(&hls_opts, "hls_segment_filename", seg_url, 0);
            }

            int64_t conn_start_us = get_time_us();
            if (!(out_ctxs[i].ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                LOG_INFO("[HLS #%d] Initiating HLS egress: %s (Method: %s) ...",
                         out_ctxs[i].type_index, out_ctxs[i].url, config->hls_method[0] ? config->hls_method : "FILE");
                interrupt_ctx.last_packet_time_us = av_gettime_relative();
                ret = avio_open2(&out_ctxs[i].ofmt_ctx->pb, out_ctxs[i].url, AVIO_FLAG_WRITE,
                                 &out_ctxs[i].ofmt_ctx->interrupt_callback, &hls_opts);
                if (ret < 0) {
                    av_strerror(ret, err_buf, sizeof(err_buf));
                    LOG_ERROR("[HLS #%d] Connection/Open Failed to '%s': %s", out_ctxs[i].type_index, out_ctxs[i].url, err_buf);
                    av_dict_free(&hls_opts);
                    goto cleanup;
                }
            }
            out_ctxs[i].conn_time_ms = (get_time_us() - conn_start_us) / 1000;

            if (strncmp(out_ctxs[i].url, "http://", 7) == 0 || strncmp(out_ctxs[i].url, "https://", 8) == 0) {
                int fd = find_tcp_socket_fd(established_tcp_fds, num_established_tcp);
                if (fd >= 0) {
                    out_ctxs[i].tcp_fd = fd;
                    out_ctxs[i].hls_state.tcp_fd = fd;
                    established_tcp_fds[num_established_tcp++] = fd;
                    LOG_INFO("[HLS #%d] Connected: TCP socket (fd=%d) established to CDN.", out_ctxs[i].type_index, fd);
                }
            }

            LOG_INFO("[HLS #%d] Writing HLS playlist headers & metadata...", out_ctxs[i].type_index);
            ret = avformat_write_header(out_ctxs[i].ofmt_ctx, &hls_opts);
            av_dict_free(&hls_opts);
            if (ret < 0) {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("[HLS #%d] HLS Header write rejected: %s", out_ctxs[i].type_index, err_buf);
                goto cleanup;
            }
            LOG_INFO("[HLS #%d] HLS publishing active. Target: %s", out_ctxs[i].type_index, out_ctxs[i].url);
        }
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
    LOG_INFO("Pipeline active: Streaming UDP Multicast -> %d Active Outputs [Zero Encoding]%s",
             num_outputs, monitor.enabled ? " + Confidence Monitor" : "");
    for (int i = 0; i < num_outputs; i++) {
        LOG_INFO("  Egress #%d: [%s] %s", i + 1,
                 out_ctxs[i].type == OUTPUT_TYPE_RTMP ? "RTMP" : "HLS", out_ctxs[i].url);
    }
    LOG_INFO("Telemetry active: Reporting per-egress bandwidth, packets, TCP & jitter");
    LOG_INFO("==================================================================");

    /* 7. Allocate packets and state tracking */
    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    for (int i = 0; i < num_outputs; i++) {
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

    /* 8. Core Remuxing Loop */
    while (!s_stop_requested) {
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
            continue;
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
        for (int j = 0; j < num_outputs; j++) {
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
                LOG_ERROR("[%s #%d] Write Error: %s",
                         out_ctxs[j].type == OUTPUT_TYPE_RTMP ? "RTMP" : "HLS", out_ctxs[j].type_index, err_buf);
                av_packet_unref(out_pkt);
                output_error = true;
                break;
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
            if (j == 0) {
                if (is_video) {
                    stats.cur_video_pts_ms = (out_ctxs[0].type == OUTPUT_TYPE_RTMP) ? packet_pts : pts_in_ms;
                    stats.last_video_write_time_us = write_finish_us;
                    if (stats.first_video_pts_ms == AV_NOPTS_VALUE) {
                        stats.first_video_pts_ms = stats.cur_video_pts_ms;
                    }
                } else {
                    stats.cur_audio_pts_ms = (out_ctxs[0].type == OUTPUT_TYPE_RTMP) ? packet_pts : pts_in_ms;
                    stats.last_audio_write_time_us = write_finish_us;
                }
            }

            av_packet_unref(out_pkt);
        }

        av_packet_unref(pkt);
        if (output_error) {
            break;
        }

        /* Telemetry Reporting Each Second */
        if (config->print_stats) {
            int64_t now_us = get_time_us();
            int64_t elapsed_stats_us = now_us - stats.last_stats_time_us;

            if (elapsed_stats_us >= ((int64_t)config->stats_interval_ms * 1000LL)) {
                double elapsed_sec = (double)elapsed_stats_us / 1000000.0;
                int64_t uptime_sec = (now_us - stats.start_time_us) / 1000000LL;

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

                /* Per-Output Metrics & TCP Stats */
                for (int j = 0; j < num_outputs; j++) {
                    OutputContext *oc = &out_ctxs[j];
                    oc->bitrate_kbps = ((double)(oc->bytes_written - oc->last_bytes) * 8.0) / (elapsed_sec * 1000.0);
                    oc->pps = (double)(oc->packets_written - oc->last_packets) / elapsed_sec;
                    oc->video_fps = (double)(oc->video_packets_written - oc->last_video_pkts) / elapsed_sec;
                    oc->audio_pps = (double)(oc->audio_packets_written - oc->last_audio_pkts) / elapsed_sec;

                    oc->write_avg_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_sum_us / (double)oc->write_latency_count / 1000.0 : 0.0;
                    oc->write_min_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_min_us / 1000.0 : 0.0;
                    oc->write_max_ms = oc->write_latency_count > 0 ?
                        (double)oc->write_latency_max_us / 1000.0 : 0.0;

                    if (oc->type == OUTPUT_TYPE_RTMP) {
                        if (oc->tcp_fd >= 0) {
                            struct tcp_info tinfo;
                            socklen_t tlen = sizeof(tinfo);
                            if (getsockopt(oc->tcp_fd, IPPROTO_TCP, TCP_INFO, &tinfo, &tlen) == 0) {
                                oc->tcp_retrans = tinfo.tcpi_total_retrans;
                                oc->tcp_lost = tinfo.tcpi_lost;
                                oc->tcp_rtt_ms = (double)tinfo.tcpi_rtt / 1000.0;
                                oc->tcp_rttvar_ms = (double)tinfo.tcpi_rttvar / 1000.0;
                            }
                        }
                    } else if (oc->type == OUTPUT_TYPE_HLS) {
                        update_hls_tcp_info(&oc->hls_state);
                        oc->tcp_fd = oc->hls_state.tcp_fd;
                        oc->tcp_retrans = oc->hls_state.total_retrans;
                        oc->tcp_lost = oc->hls_state.total_lost;
                        oc->tcp_rtt_ms = oc->hls_state.rtt_ms;
                        oc->tcp_rttvar_ms = oc->hls_state.rttvar_ms;
                    }
                }

                /* Primary Output pointers */
                OutputContext *prim_rtmp = NULL;
                OutputContext *prim_hls = NULL;
                for (int j = 0; j < num_outputs; j++) {
                    if (out_ctxs[j].type == OUTPUT_TYPE_RTMP && !prim_rtmp) prim_rtmp = &out_ctxs[j];
                    if (out_ctxs[j].type == OUTPUT_TYPE_HLS && !prim_hls) prim_hls = &out_ctxs[j];
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

                double write_avg_ms = prim_rtmp ? prim_rtmp->write_avg_ms : (prim_hls ? prim_hls->write_avg_ms : 0.0);
                double write_min_ms = prim_rtmp ? prim_rtmp->write_min_ms : (prim_hls ? prim_hls->write_min_ms : 0.0);
                double write_max_ms = prim_rtmp ? prim_rtmp->write_max_ms : (prim_hls ? prim_hls->write_max_ms : 0.0);
                double max_gap_ms = (double)stats.max_pacing_gap_us / 1000.0;

                /* Media Delivery Speed Factor */
                if (stats.cur_video_pts_ms != AV_NOPTS_VALUE && stats.last_stats_video_pts_ms != AV_NOPTS_VALUE) {
                    double delta_pts_sec = (double)(stats.cur_video_pts_ms - stats.last_stats_video_pts_ms) / 1000.0;
                    stats.cur_speed_factor = (elapsed_sec > 0.0) ? (delta_pts_sec / elapsed_sec) : 1.0;
                    if (stats.cur_speed_factor < 0.0) stats.cur_speed_factor = 0.0;
                } else if (stats.cur_video_pts_ms != AV_NOPTS_VALUE) {
                    stats.cur_speed_factor = 1.0;
                } else {
                    stats.cur_speed_factor = 0.0;
                }

                /* Keyframe age */
                if (stats.last_keyframe_time_us > 0) {
                    stats.cur_keyframe_age_sec = (double)(now_us - stats.last_keyframe_time_us) / 1000000.0;
                } else {
                    stats.cur_keyframe_age_sec = 0.0;
                }

                /* Silence tracking */
                int64_t last_v_write = prim_rtmp ? prim_rtmp->last_video_write_time_us : (prim_hls ? prim_hls->last_video_write_time_us : 0);
                int64_t last_a_write = prim_rtmp ? prim_rtmp->last_audio_write_time_us : (prim_hls ? prim_hls->last_audio_write_time_us : 0);
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

                /* YouTube Health Evaluation (if RTMP is active) */
                if (num_rtmp_outputs > 0) {
                    bool is_stalled_video = (stats.cur_video_silence_ms > 600 &&
                                             (prim_rtmp->audio_packets_written - prim_rtmp->last_audio_pkts) > 5);
                    bool is_underfeeding = (prim_rtmp->video_packets_written > 30 && stats.cur_speed_factor < 0.88);
                    bool is_kf_delayed = (stats.cur_keyframe_age_sec > 4.2);
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
                }

                /* Trigger active warnings when diagnostics enabled */
                if (config->enable_diagnostics) {
                    if (interval_lost > 0) {
                        LOG_WARN("[BUFFER RISK] UDP Multicast loss: %llu TS packets dropped in last second (Loss: %.2f%%)!",
                                 (unsigned long long)interval_lost, stats.cur_interval_loss_pct);
                    }
                    if (prim_rtmp && prim_rtmp->tcp_lost > 0) {
                        LOG_WARN("[BUFFER RISK] RTMP uplink TCP loss: %u packets lost in transit!",
                                 prim_rtmp->tcp_lost);
                    }
                    if (prim_hls && prim_hls->tcp_lost > 0) {
                        LOG_WARN("[BUFFER RISK] HLS uplink TCP loss: %u packets lost in transit to CDN/origin!",
                                 prim_hls->tcp_lost);
                    }
                }

                if (config->json_telemetry) {
                    /* Structured JSON Telemetry Output (1 per second) */
                    printf("{\"uptime_sec\":%lld", (long long)uptime_sec);
                    if (num_rtmp_outputs > 0) {
                        printf(",\"rtmp_status\":\"STREAMING\",\"youtube_health\":{\"status\":\"%s\",\"speed_factor\":%.2f,\"keyframe_age_sec\":%.2f,\"max_pacing_gap_ms\":%.1f,\"wall_lead_ms\":%lld,\"video_silence_ms\":%lld,\"audio_silence_ms\":%lld}",
                               stats.youtube_health_status, stats.cur_speed_factor, stats.cur_keyframe_age_sec,
                               max_gap_ms, (long long)stats.cur_wall_lead_ms,
                               (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms);
                    }
                    printf(",\"udp_in\":{\"bitrate_kbps\":%.1f,\"pps\":%.1f,\"total_packets\":%llu,\"total_bytes\":%llu,\"dropped_pre_keyframe\":%llu,\"packet_loss\":{\"lost_packets\":%llu,\"cc_errors\":%llu,\"total_loss_pct\":%.2f,\"interval_loss_pct\":%.2f}}",
                           udp_bitrate_kbps, udp_pps,
                           (unsigned long long)stats.udp_packets_read, (unsigned long long)stats.udp_bytes_read,
                           (unsigned long long)stats.udp_dropped_pre_keyframe,
                           (unsigned long long)stats.udp_lost_packets,
                           (unsigned long long)stats.udp_cc_errors,
                           stats.udp_loss_pct, stats.cur_interval_loss_pct);

                    /* RTMP Output(s) in JSON */
                    if (num_rtmp_outputs == 1 && prim_rtmp) {
                        printf(",\"rtmp_out\":{\"target\":\"%s\",\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"video_pts_ms\":%lld,\"audio_pts_ms\":%lld,\"av_drift_ms\":%lld,\"keyframe_interval_sec\":%.2f,\"total_packets\":%llu,\"total_bytes\":%llu,\"tcp_loss\":{\"retrans_packets\":%u,\"lost_packets\":%u,\"rtt_ms\":%.1f,\"rttvar_ms\":%.1f},\"write_latency\":{\"avg_ms\":%.2f,\"min_ms\":%.2f,\"max_ms\":%.2f,\"peak_ms\":%.2f}}",
                               prim_rtmp->url, prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->audio_pps,
                               (long long)(stats.cur_video_pts_ms != AV_NOPTS_VALUE ? stats.cur_video_pts_ms : 0),
                               (long long)(stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? stats.cur_audio_pts_ms : 0),
                               (long long)av_drift_ms, stats.last_keyframe_interval_sec,
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

                    if (num_rtmp_outputs > 0 && num_hls_outputs > 0) {
                        /* Dual / Multiple Outputs */
                        char rtmp_str[128] = "";
                        char hls_str[128] = "";
                        if (prim_rtmp) {
                            snprintf(rtmp_str, sizeof(rtmp_str), "RTMP: %.1f kbps (%.1f fps, Retrans: %u, RTT: %.1fms)",
                                     prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->tcp_retrans, prim_rtmp->tcp_rtt_ms);
                        }
                        if (prim_hls) {
                            snprintf(hls_str, sizeof(hls_str), "HLS: %.1f kbps (%.1f pps, Seg #%llu, Retrans: %u)",
                                     prim_hls->bitrate_kbps, prim_hls->pps,
                                     (unsigned long long)prim_hls->hls_state.segments_completed,
                                     prim_hls->tcp_retrans);
                        }
                        LOG_INFO("UDP: %.1f kbps (Loss: %.2f%%, %llu pkts) | %s | %s%s | Lat: %.2f/%.2f/%.2fms | Speed: %.2fx | Drift: %+lldms | GOP: %.1fs | YouTube: %s",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct, (unsigned long long)stats.udp_lost_packets,
                                 rtmp_str, hls_str, mon_str,
                                 write_min_ms, write_avg_ms, write_max_ms,
                                 stats.cur_speed_factor, (long long)av_drift_ms, stats.last_keyframe_interval_sec,
                                 stats.youtube_health_status);
                    } else if (num_rtmp_outputs > 0) {
                        /* RTMP Only */
                        char total_rtmp_str[32];
                        format_bytes(prim_rtmp ? prim_rtmp->bytes_written : 0, total_rtmp_str, sizeof(total_rtmp_str));
                        LOG_INFO("UDP: %.1f kbps (Loss: %.2f%%, %llu pkts) | RTMP: %.1f kbps (%.1f fps, Retrans: %u, RTT: %.1fms)%s | Lat: %.2f/%.2f/%.2fms | Silence(V/A): %lld/%lldms | Speed: %.2fx | Drift: %+lldms | GOP: %.1fs | YouTube: %s | Out: %s",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct, (unsigned long long)stats.udp_lost_packets,
                                 prim_rtmp ? prim_rtmp->bitrate_kbps : 0.0,
                                 prim_rtmp ? prim_rtmp->video_fps : 0.0,
                                 prim_rtmp ? prim_rtmp->tcp_retrans : 0,
                                 prim_rtmp ? prim_rtmp->tcp_rtt_ms : 0.0,
                                 mon_str,
                                 write_min_ms, write_avg_ms, write_max_ms,
                                 (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms,
                                 stats.cur_speed_factor, (long long)av_drift_ms, stats.last_keyframe_interval_sec,
                                 stats.youtube_health_status, total_rtmp_str);
                    } else {
                        /* HLS Only */
                        char total_hls_str[32];
                        format_bytes(prim_hls ? prim_hls->bytes_written : 0, total_hls_str, sizeof(total_hls_str));
                        char hls_tcp_str[64] = "";
                        if (prim_hls && strncmp(prim_hls->url, "http", 4) == 0) {
                            snprintf(hls_tcp_str, sizeof(hls_tcp_str), ", Retrans: %u, RTT: %.1fms",
                                     prim_hls->tcp_retrans, prim_hls->tcp_rtt_ms);
                        }
                        char seg_summary[64] = "";
                        if (prim_hls && prim_hls->hls_state.segments_completed > 0) {
                            snprintf(seg_summary, sizeof(seg_summary), " | Seg: #%llu (%.1fs, %.1fms)",
                                     (unsigned long long)prim_hls->hls_state.segments_completed,
                                     prim_hls->hls_state.last_segment_duration_sec,
                                     prim_hls->hls_state.last_segment_upload_ms);
                        }
                        LOG_INFO("UDP: %.1f kbps (Loss: %.2f%%, %llu pkts) | HLS: %.1f kbps (%.1f pps, %s%s)%s%s | Speed: %.2fx | Drift: %+lldms | Out: %s",
                                 udp_bitrate_kbps, stats.cur_interval_loss_pct, (unsigned long long)stats.udp_lost_packets,
                                 prim_hls ? prim_hls->bitrate_kbps : 0.0,
                                 prim_hls ? prim_hls->pps : 0.0,
                                 total_hls_str, hls_tcp_str, seg_summary, mon_str,
                                 stats.cur_speed_factor, (long long)av_drift_ms, total_hls_str);
                    }
                } else if (config->detailed_telemetry) {
                    /* Multi-line Telemetry Card */
                    char uptime_str[32], total_udp_str[32];
                    format_time(uptime_sec, uptime_str, sizeof(uptime_str));
                    format_bytes(stats.udp_bytes_read, total_udp_str, sizeof(total_udp_str));

                    char title_outputs[128];
                    if (num_rtmp_outputs > 0 && num_hls_outputs > 0) {
                        snprintf(title_outputs, sizeof(title_outputs), "RTMP: %d active, HLS: %d active",
                                 num_rtmp_outputs, num_hls_outputs);
                    } else if (num_rtmp_outputs > 0) {
                        snprintf(title_outputs, sizeof(title_outputs), "RTMP: %d active (STREAMING)", num_rtmp_outputs);
                    } else {
                        snprintf(title_outputs, sizeof(title_outputs), "HLS: %d active (STREAMING)", num_hls_outputs);
                    }

                    printf("\n\033[1;36m┌── [INFO @ %s] (%s) ──────────────────────────────────────────────────────────\033[0m\n",
                           uptime_str, title_outputs);
                    printf("│  \033[1;32m[UDP Ingress]\033[0m   : %7.1f kbps  | %5.1f pkts/s  | In: %s | Loss: %.2f%% (%llu dropped, %llu CC errs)\n",
                           udp_bitrate_kbps, udp_pps, total_udp_str, stats.udp_loss_pct,
                           (unsigned long long)stats.udp_lost_packets, (unsigned long long)stats.udp_cc_errors);

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

                    printf("│  \033[1;37m[Stream Pacing]\033[0m : Speed: %4.2fx | Max Gap: %5.1fms | Wall Lead: %+lldms\n",
                           stats.cur_speed_factor, max_gap_ms, (long long)stats.cur_wall_lead_ms);
                    printf("│  \033[1;35m[Track Silence]\033[0m : Video: %4lldms | Audio: %4lldms\n",
                           (long long)stats.cur_video_silence_ms, (long long)stats.cur_audio_silence_ms);
                    printf("│  \033[1;36m[A/V & Sync ]\033[0m   : Video PTS: %6.2fs | Audio PTS: %6.2fs | Drift: %+lld ms\n",
                           (stats.cur_video_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_video_pts_ms / 1000.0 : 0.0),
                           (stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_audio_pts_ms / 1000.0 : 0.0),
                           (long long)av_drift_ms);

                    if (num_rtmp_outputs > 0) {
                        const char *health_color = (strcmp(stats.youtube_health_status, "HEALTHY") == 0) ? "\033[1;32m" :
                            (strstr(stats.youtube_health_status, "BUFFER RISK") ? "\033[1;31m" : "\033[1;33m");
                        printf("│  \033[1;33m[YouTube Health]\033[0m: %s%s\033[0m (GOP: %.2fs | KF Age: %.1fs | Lat: %.1fms)\n",
                               health_color, stats.youtube_health_status,
                               stats.last_keyframe_interval_sec, stats.cur_keyframe_age_sec, write_avg_ms);
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

                    if (num_rtmp_outputs > 0 && num_hls_outputs > 0) {
                        /* Option 3: Dual / Multiple Output */
                        char health_badge[128];
                        if (strcmp(stats.youtube_health_status, "HEALTHY") == 0) {
                            snprintf(health_badge, sizeof(health_badge), "\033[32mHEALTHY 🟢\033[0m");
                        } else if (strstr(stats.youtube_health_status, "BUFFER RISK")) {
                            snprintf(health_badge, sizeof(health_badge), "\033[31m%s 🔴\033[0m", stats.youtube_health_status);
                        } else {
                            snprintf(health_badge, sizeof(health_badge), "\033[33m%s 🟡\033[0m", stats.youtube_health_status);
                        }

                        char hls_tcp_badge[48] = "";
                        if (prim_hls && strncmp(prim_hls->url, "http", 4) == 0 && prim_hls->tcp_retrans > 0) {
                            snprintf(hls_tcp_badge, sizeof(hls_tcp_badge), ", Retrans: %u", prim_hls->tcp_retrans);
                        }

                        if (num_rtmp_outputs == 1 && num_hls_outputs == 1) {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps (Loss: %.1f%%) │ \033[34mRTMP:\033[0m %5.0f kbps (%4.1f fps) │ \033[35mHLS:\033[0m %5.0f kbps (%4.1f pps, Seg #%llu%s)%s │ \033[1;37mSpd:\033[0m %4.2fx │ %s   ",
                                   uptime_str,
                                   udp_bitrate_kbps, stats.cur_interval_loss_pct,
                                   prim_rtmp->bitrate_kbps, prim_rtmp->video_fps,
                                   prim_hls->bitrate_kbps, prim_hls->pps,
                                   (unsigned long long)prim_hls->hls_state.segments_completed,
                                   hls_tcp_badge, mon_badge,
                                   stats.cur_speed_factor, health_badge);
                        } else {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps │ \033[34mRTMP (%d):\033[0m %5.0f kbps │ \033[35mHLS (%d):\033[0m %5.0f kbps%s │ \033[1;37mSpd:\033[0m %4.2fx │ %s   ",
                                   uptime_str,
                                   udp_bitrate_kbps,
                                   num_rtmp_outputs, prim_rtmp ? prim_rtmp->bitrate_kbps : 0.0,
                                   num_hls_outputs, prim_hls ? prim_hls->bitrate_kbps : 0.0,
                                   mon_badge, stats.cur_speed_factor, health_badge);
                        }
                    } else if (num_rtmp_outputs > 0) {
                        /* Option 1: Only RTMP */
                        char health_badge[128];
                        if (strcmp(stats.youtube_health_status, "HEALTHY") == 0) {
                            snprintf(health_badge, sizeof(health_badge), "\033[32mHEALTHY 🟢\033[0m");
                        } else if (strstr(stats.youtube_health_status, "BUFFER RISK")) {
                            snprintf(health_badge, sizeof(health_badge), "\033[31m%s 🔴\033[0m", stats.youtube_health_status);
                        } else {
                            snprintf(health_badge, sizeof(health_badge), "\033[33m%s 🟡\033[0m", stats.youtube_health_status);
                        }

                        if (num_rtmp_outputs == 1 && prim_rtmp) {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps (Loss: %.1f%%) │ \033[34mRTMP:\033[0m %5.0f kbps (%4.1f fps, Retrans: %u)%s │ \033[1;37mSpd:\033[0m %4.2fx │ \033[35mKF:\033[0m %3.1fs │ %s   ",
                                   uptime_str,
                                   udp_bitrate_kbps, stats.cur_interval_loss_pct,
                                   prim_rtmp->bitrate_kbps, prim_rtmp->video_fps, prim_rtmp->tcp_retrans,
                                   mon_badge, stats.cur_speed_factor, stats.cur_keyframe_age_sec, health_badge);
                        } else {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps │ \033[34mRTMP (%d outputs):\033[0m %5.0f kbps (%4.1f fps)%s │ \033[1;37mSpd:\033[0m %4.2fx │ %s   ",
                                   uptime_str,
                                   udp_bitrate_kbps,
                                   num_rtmp_outputs, prim_rtmp ? prim_rtmp->bitrate_kbps : 0.0,
                                   prim_rtmp ? prim_rtmp->video_fps : 0.0,
                                   mon_badge, stats.cur_speed_factor, health_badge);
                        }
                    } else {
                        /* Option 2: Only HLS */
                        char hls_tcp_badge[48] = "";
                        if (prim_hls && strncmp(prim_hls->url, "http", 4) == 0) {
                            if (prim_hls->tcp_retrans > 0) {
                                snprintf(hls_tcp_badge, sizeof(hls_tcp_badge), ", Retrans: %u", prim_hls->tcp_retrans);
                            } else if (prim_hls->tcp_rtt_ms > 0.0) {
                                snprintf(hls_tcp_badge, sizeof(hls_tcp_badge), ", RTT: %.0fms", prim_hls->tcp_rtt_ms);
                            }
                        }

                        if (num_hls_outputs == 1 && prim_hls) {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps (Loss: %.1f%%) │ \033[35mHLS:\033[0m %5.0f kbps (%4.1f pps, Seg #%llu%s)%s │ \033[1;37mSpd:\033[0m %4.2fx │ \033[32mOK 🟢\033[0m   ",
                                   uptime_str,
                                   udp_bitrate_kbps, stats.cur_interval_loss_pct,
                                   prim_hls->bitrate_kbps, prim_hls->pps,
                                   (unsigned long long)prim_hls->hls_state.segments_completed,
                                   hls_tcp_badge, mon_badge, stats.cur_speed_factor);
                        } else {
                            printf("\r\033[36m[%s]\033[0m \033[32mUDP:\033[0m %5.0f kbps │ \033[35mHLS (%d outputs):\033[0m %5.0f kbps (%4.1f pps)%s │ \033[1;37mSpd:\033[0m %4.2fx │ \033[32mOK 🟢\033[0m   ",
                                   uptime_str,
                                   udp_bitrate_kbps,
                                   num_hls_outputs, prim_hls ? prim_hls->bitrate_kbps : 0.0,
                                   prim_hls ? prim_hls->pps : 0.0,
                                   mon_badge, stats.cur_speed_factor);
                        }
                    }
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
        if (out_ctxs[i].ofmt_ctx) {
            LOG_INFO("[%s #%d] Finalizing stream and writing trailer...",
                     out_ctxs[i].type == OUTPUT_TYPE_RTMP ? "RTMP" : "HLS", out_ctxs[i].type_index);
            av_write_trailer(out_ctxs[i].ofmt_ctx);
        }
    }

cleanup:
    confidence_monitor_cleanup(&monitor);
    av_log_set_callback(av_log_default_callback);
    LOG_INFO("Cleaning up pipeline resources...");
    if (pkt) {
        av_packet_free(&pkt);
    }
    for (int i = 0; i < num_outputs; i++) {
        if (out_ctxs[i].pkt) {
            av_packet_free(&out_ctxs[i].pkt);
        }
        if (out_ctxs[i].ofmt_ctx) {
            if (!(out_ctxs[i].ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&out_ctxs[i].ofmt_ctx->pb);
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
