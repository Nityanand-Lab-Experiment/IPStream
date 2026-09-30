#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "utils.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define MAX_STREAMS 32

static volatile sig_atomic_t s_stop_requested = 0;

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
    strncpy(config->output_url, "rtmp://127.0.0.1:1935/live/stream", sizeof(config->output_url) - 1);
    config->local_addr[0] = '\0';
    config->fifo_size = 50 * 1024 * 1024;       /* 50 MB FIFO buffer */
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

int pipeline_run(const PipelineConfig *config) {
    int ret = 0;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVPacket *pkt = NULL;
    int *stream_mapping = NULL;
    int nb_mapped_streams = 0;
    int video_stream_idx = -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};

    InterruptContext interrupt_ctx = {
        .last_packet_time_us = 0,
        .timeout_sec = config->timeout_sec
    };

    s_stop_requested = 0;

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

    /* 4. Allocate RTMP Output Context */
    LOG_INFO("Preparing RTMP output target: %s", config->output_url);
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", config->output_url);
    if (!ofmt_ctx) {
        LOG_ERROR("Could not allocate output format context for 'flv': %d", ret);
        ret = AVERROR_UNKNOWN;
        goto cleanup;
    }

    ofmt_ctx->interrupt_callback.callback = interrupt_cb;
    ofmt_ctx->interrupt_callback.opaque = &interrupt_ctx;

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

        /* Verify standard RTMP/FLV codec compatibility */
        if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (in_codecpar->codec_id != AV_CODEC_ID_H264 &&
                in_codecpar->codec_id != AV_CODEC_ID_HEVC) {
                const AVCodecDescriptor *d = avcodec_descriptor_get(in_codecpar->codec_id);
                LOG_WARN("Video codec '%s' may not be supported by standard FLV/RTMP without transcoding!",
                         d ? d->name : "unknown");
            }
            if (video_stream_idx < 0) {
                video_stream_idx = i;
            }
        } else if (in_codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (in_codecpar->codec_id != AV_CODEC_ID_AAC &&
                in_codecpar->codec_id != AV_CODEC_ID_MP3) {
                const AVCodecDescriptor *d = avcodec_descriptor_get(in_codecpar->codec_id);
                LOG_WARN("Audio codec '%s' may not be supported by standard FLV/RTMP without transcoding!",
                         d ? d->name : "unknown");
            }
        }

        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            LOG_ERROR("Failed to allocate output stream for input #%d", i);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            LOG_ERROR("Failed to copy codec parameters for stream #%d", i);
            goto cleanup;
        }

        /* Clear codec tag to let muxer set container-specific tag */
        out_stream->codecpar->codec_tag = 0;
        stream_mapping[i] = nb_mapped_streams++;

        log_stream_info(out_stream, out_stream->index, "[Output]");
    }

    if (nb_mapped_streams == 0) {
        LOG_ERROR("No audio or video streams found in input source!");
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }

    /* 6. Open RTMP Output Connection */
    AVDictionary *output_opts = NULL;
    av_dict_set(&output_opts, "flvflags", "no_duration_filesize+aac_seq_header_detect", 0);
    av_dict_set(&output_opts, "rtmp_live", "live", 0);
    av_dict_set(&output_opts, "rtmp_buffer", "1000", 0);

    int64_t rtmp_conn_start_us = get_time_us();
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        LOG_INFO("[RTMP STATUS] Initiating connection to: %s ...", config->output_url);
        interrupt_ctx.last_packet_time_us = av_gettime_relative();
        ret = avio_open2(&ofmt_ctx->pb, config->output_url, AVIO_FLAG_WRITE,
                         &ofmt_ctx->interrupt_callback, &output_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[RTMP STATUS] Connection Failed to '%s': %s", config->output_url, err_buf);
            LOG_ERROR("[RTMP DIAGNOSTIC] Check stream key, firewall port 1935, or RTMP destination reachability.");
            av_dict_free(&output_opts);
            goto cleanup;
        }
    }
    int64_t rtmp_conn_time_ms = (get_time_us() - rtmp_conn_start_us) / 1000;
    LOG_INFO("[RTMP STATUS] Connected: TCP socket to RTMP endpoint established in %lld ms.", rtmp_conn_time_ms);

    /* 7. Write FLV/RTMP Stream Header */
    int64_t rtmp_handshake_start_us = get_time_us();
    LOG_INFO("[RTMP STATUS] Performing RTMP handshake & publishing stream metadata...");
    ret = avformat_write_header(ofmt_ctx, &output_opts);
    av_dict_free(&output_opts);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("[RTMP STATUS] Handshake / Stream Header rejected: %s", err_buf);
        LOG_ERROR("[RTMP DIAGNOSTIC] Verify YouTube stream key and ingest server URL.");
        goto cleanup;
    }
    int64_t rtmp_handshake_time_ms = (get_time_us() - rtmp_handshake_start_us) / 1000;
    LOG_INFO("[RTMP STATUS] Handshake confirmed in %lld ms! RTMP connection state: STREAMING.",
             rtmp_handshake_time_ms);

    LOG_INFO("==================================================================");
    LOG_INFO("Pipeline active: Streaming UDP Multicast -> RTMP [Zero Encoding]");
    LOG_INFO("Health diagnostics active: Monitoring buffering, GOP, and network sync");
    LOG_INFO("==================================================================");

    /* 8. Allocate packet and state tracking */
    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    PipelineStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.start_time_us = get_time_us();
    stats.last_stats_time_us = stats.start_time_us;
    stats.cur_video_pts_ms = AV_NOPTS_VALUE;
    stats.cur_audio_pts_ms = AV_NOPTS_VALUE;
    stats.last_video_dts_ms = AV_NOPTS_VALUE;
    stats.last_keyframe_time_us = 0;
    stats.rtmp_connect_time_ms = rtmp_conn_time_ms;
    stats.rtmp_handshake_time_ms = rtmp_handshake_time_ms;

    bool seen_first_keyframe = !config->wait_keyframe;
    int64_t stream_first_pts[MAX_STREAMS];
    int64_t stream_first_dts[MAX_STREAMS];
    int64_t stream_last_dts[MAX_STREAMS];

    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_first_pts[i] = AV_NOPTS_VALUE;
        stream_first_dts[i] = AV_NOPTS_VALUE;
        stream_last_dts[i] = AV_NOPTS_VALUE;
    }

    /* 9. Core Remuxing Loop */
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
        AVStream *out_stream = ofmt_ctx->streams[out_idx];
        bool is_video = (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        /* Keyframe Detection and GOP Interval Diagnostics */
        if (is_video && (pkt->flags & AV_PKT_FLAG_KEY)) {
            int64_t now_kf_us = get_time_us();
            if (stats.last_keyframe_time_us > 0) {
                double kf_interval = (double)(now_kf_us - stats.last_keyframe_time_us) / 1000000.0;
                stats.last_keyframe_interval_sec = kf_interval;

                /* YouTube buffering cause: Keyframe interval > 4.0 seconds */
                if (config->enable_diagnostics && kf_interval > 4.2) {
                    LOG_WARN("[BUFFER RISK] Keyframe interval is %.2fs (> 4.0s). YouTube recommends 2-4s keyframes. Viewers may experience buffering!",
                             kf_interval);
                }
            }
            stats.last_keyframe_time_us = now_kf_us;
            stats.total_keyframes_seen++;
        }

        /* Wait for first video keyframe to prevent visual glitches */
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
                /* Drop audio packets until first video keyframe arrives to keep A/V in sync */
                stats.udp_dropped_pre_keyframe++;
                av_packet_unref(pkt);
                continue;
            }
        }

        /* Timestamp Normalization & Rescaling */
        if (config->normalize_pts) {
            if (out_idx < MAX_STREAMS) {
                if (stream_first_dts[out_idx] == AV_NOPTS_VALUE) {
                    stream_first_dts[out_idx] = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                    stream_first_pts[out_idx] = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
                }

                int64_t dts_offset = (pkt->dts != AV_NOPTS_VALUE) ?
                                     (pkt->dts - stream_first_dts[out_idx]) : 0;
                int64_t pts_offset = (pkt->pts != AV_NOPTS_VALUE) ?
                                     (pkt->pts - stream_first_pts[out_idx]) : dts_offset;

                pkt->pts = av_rescale_q_rnd(pts_offset, in_stream->time_base, out_stream->time_base,
                                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                pkt->dts = av_rescale_q_rnd(dts_offset, in_stream->time_base, out_stream->time_base,
                                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                if (pkt->duration > 0) {
                    pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
                }
            }
        } else {
            /* Standard time base rescaling */
            av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        }

        /* Timestamp Gap Diagnostics (Packet Loss Detector) */
        if (is_video && config->enable_diagnostics && stats.last_video_dts_ms != AV_NOPTS_VALUE) {
            int64_t dts_delta = pkt->dts - stats.last_video_dts_ms;
            /* If timestamp gap between frames is > 250ms (for ~25/30fps typical delta is 33-40ms) */
            if (dts_delta > 250) {
                stats.num_timestamp_gaps++;
                LOG_WARN("[BUFFER RISK] Video timestamp gap of %lld ms detected! Multicast UDP packet loss likely.",
                         (long long)dts_delta);
            }
        }

        /* Strict Monotonicity Enforcement for FLV/RTMP container */
        if (out_idx < MAX_STREAMS) {
            if (stream_last_dts[out_idx] != AV_NOPTS_VALUE && pkt->dts <= stream_last_dts[out_idx]) {
                pkt->dts = stream_last_dts[out_idx] + 1;
            }
            if (pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            stream_last_dts[out_idx] = pkt->dts;
        }

        if (is_video) {
            stats.last_video_dts_ms = pkt->dts;
        }

        pkt->stream_index = out_idx;
        pkt->pos = -1;

        int packet_size = pkt->size;
        int64_t packet_pts = pkt->pts;

        /* Write packet to RTMP destination with latency timing */
        int64_t write_start_us = get_time_us();
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        int64_t write_duration_ms = (get_time_us() - write_start_us) / 1000;

        /* Uplink Bottleneck Diagnostics */
        if (config->enable_diagnostics && write_duration_ms > 300) {
            stats.num_socket_slow_writes++;
            LOG_WARN("[BUFFER RISK] RTMP socket write blocked for %lld ms! Uplink internet bandwidth congestion detected.",
                     (long long)write_duration_ms);
        }

        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[RTMP STATUS] Connection Lost / Write Error: %s", err_buf);
            LOG_ERROR("[RTMP DIAGNOSTIC] RTMP server dropped the connection (Check internet stability or stream limits).");
            av_packet_unref(pkt);
            break;
        }

        stats.rtmp_packets_written++;
        stats.rtmp_bytes_written += packet_size;
        if (is_video) {
            stats.rtmp_video_packets_written++;
            stats.cur_video_pts_ms = packet_pts;
        } else {
            stats.rtmp_audio_packets_written++;
            stats.cur_audio_pts_ms = packet_pts;
        }

        av_packet_unref(pkt);

        /* Telemetry Reporting Each Second */
        if (config->print_stats) {
            int64_t now_us = get_time_us();
            int64_t elapsed_stats_us = now_us - stats.last_stats_time_us;

            if (elapsed_stats_us >= ((int64_t)config->stats_interval_ms * 1000LL)) {
                double elapsed_sec = (double)elapsed_stats_us / 1000000.0;
                int64_t uptime_sec = (now_us - stats.start_time_us) / 1000000LL;

                /* UDP Ingress Metrics */
                double udp_bitrate_kbps = ((double)(stats.udp_bytes_read - stats.last_udp_bytes) * 8.0) /
                                          (elapsed_sec * 1000.0);
                double udp_pps = (double)(stats.udp_packets_read - stats.last_udp_packets) / elapsed_sec;

                /* RTMP Egress Metrics */
                double rtmp_bitrate_kbps = ((double)(stats.rtmp_bytes_written - stats.last_rtmp_bytes) * 8.0) /
                                           (elapsed_sec * 1000.0);
                double rtmp_video_fps = (double)(stats.rtmp_video_packets_written - stats.last_rtmp_video_pkts) / elapsed_sec;
                double rtmp_audio_pps = (double)(stats.rtmp_audio_packets_written - stats.last_rtmp_audio_pkts) / elapsed_sec;

                /* A/V Sync Drift */
                int64_t av_drift_ms = 0;
                if (stats.cur_video_pts_ms != AV_NOPTS_VALUE && stats.cur_audio_pts_ms != AV_NOPTS_VALUE) {
                    av_drift_ms = stats.cur_video_pts_ms - stats.cur_audio_pts_ms;
                }

                /* YouTube Starvation Diagnostic (FPS < 15 or A/V drift > 500ms) */
                if (config->enable_diagnostics) {
                    if (stats.rtmp_video_packets_written > 30 && rtmp_video_fps < 15.0) {
                        LOG_WARN("[BUFFER RISK] Video rate dropped to %.1f fps! YouTube will buffer due to packet starvation.",
                                 rtmp_video_fps);
                    }
                    if (llabs(av_drift_ms) > 600) {
                        LOG_WARN("[BUFFER RISK] Excessive A/V sync drift: %+lld ms. Player audio/video desync may cause buffering!",
                                 (long long)av_drift_ms);
                    }
                }

                if (config->json_telemetry) {
                    /* Structured JSON Telemetry Output (1 per second) */
                    printf("{\"uptime_sec\":%lld,\"rtmp_status\":\"STREAMING\",\"udp_in\":{\"bitrate_kbps\":%.1f,\"pps\":%.1f,\"total_packets\":%llu,\"total_bytes\":%llu,\"dropped_pre_keyframe\":%llu},\"rtmp_out\":{\"bitrate_kbps\":%.1f,\"video_fps\":%.1f,\"audio_pps\":%.1f,\"video_pts_ms\":%lld,\"audio_pts_ms\":%lld,\"av_drift_ms\":%lld,\"keyframe_interval_sec\":%.2f,\"total_packets\":%llu,\"total_bytes\":%llu}}\n",
                           (long long)uptime_sec,
                           udp_bitrate_kbps, udp_pps,
                           (unsigned long long)stats.udp_packets_read, (unsigned long long)stats.udp_bytes_read,
                           (unsigned long long)stats.udp_dropped_pre_keyframe,
                           rtmp_bitrate_kbps, rtmp_video_fps, rtmp_audio_pps,
                           (long long)(stats.cur_video_pts_ms != AV_NOPTS_VALUE ? stats.cur_video_pts_ms : 0),
                           (long long)(stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? stats.cur_audio_pts_ms : 0),
                           (long long)av_drift_ms,
                           stats.last_keyframe_interval_sec,
                           (unsigned long long)stats.rtmp_packets_written, (unsigned long long)stats.rtmp_bytes_written);
                    fflush(stdout);
                } else if (config->log_telemetry) {
                    /* Standard Timestamped Console Log Entry */
                    char total_rtmp_str[32];
                    format_bytes(stats.rtmp_bytes_written, total_rtmp_str, sizeof(total_rtmp_str));
                    LOG_INFO("[TELEMETRY] UDP: %.1f kbps (%.1f pps) | RTMP: %.1f kbps | %.1f fps (V) | %.1f pps (A) | Drift: %+lldms | GOP: %.1fs | Out: %s",
                             udp_bitrate_kbps, udp_pps, rtmp_bitrate_kbps, rtmp_video_fps, rtmp_audio_pps,
                             (long long)av_drift_ms, stats.last_keyframe_interval_sec, total_rtmp_str);
                } else if (config->detailed_telemetry) {
                    /* Multi-line Telemetry Card */
                    char uptime_str[32], total_udp_str[32], total_rtmp_str[32];
                    format_time(uptime_sec, uptime_str, sizeof(uptime_str));
                    format_bytes(stats.udp_bytes_read, total_udp_str, sizeof(total_udp_str));
                    format_bytes(stats.rtmp_bytes_written, total_rtmp_str, sizeof(total_rtmp_str));

                    printf("\n\033[1;36m┌── [TELEMETRY REPORT @ %s] (RTMP: STREAMING) ────────────────────\033[0m\n", uptime_str);
                    printf("│  \033[1;32m[UDP Ingress]\033[0m : %7.1f kbps  | %5.1f pkts/s  | In: %s (%llu pkts)\n",
                           udp_bitrate_kbps, udp_pps, total_udp_str, (unsigned long long)stats.udp_packets_read);
                    printf("│  \033[1;34m[RTMP Egress]\033[0m : %7.1f kbps  | %5.1f fps (V) | %5.1f pps (A) | Out: %s\n",
                           rtmp_bitrate_kbps, rtmp_video_fps, rtmp_audio_pps, total_rtmp_str);
                    printf("│  \033[1;33m[A/V & Sync ]\033[0m : Video PTS: %6.2fs | Audio PTS: %6.2fs | Drift: %+lld ms | GOP: %.2fs\n",
                           (stats.cur_video_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_video_pts_ms / 1000.0 : 0.0),
                           (stats.cur_audio_pts_ms != AV_NOPTS_VALUE ? (double)stats.cur_audio_pts_ms / 1000.0 : 0.0),
                           (long long)av_drift_ms, stats.last_keyframe_interval_sec);
                    printf("\033[1;36m└─────────────────────────────────────────────────────────────────────────────\033[0m\n");
                    fflush(stdout);
                } else {
                    /* Compact Single-line Interactive Live Dashboard */
                    char uptime_str[32], total_rtmp_str[32];
                    format_time(uptime_sec, uptime_str, sizeof(uptime_str));
                    format_bytes(stats.rtmp_bytes_written, total_rtmp_str, sizeof(total_rtmp_str));

                    printf("\r\033[36m[%s]\033[0m \033[32mUDP In:\033[0m %6.1f kbps (%4.1f pps) │ \033[34mRTMP Out:\033[0m %6.1f kbps (%4.1f fps V, %4.1f pps A) │ \033[33mDrift:\033[0m %+3lldms │ \033[35mTotal:\033[0m %s   ",
                           uptime_str,
                           udp_bitrate_kbps, udp_pps,
                           rtmp_bitrate_kbps, rtmp_video_fps, rtmp_audio_pps,
                           (long long)av_drift_ms,
                           total_rtmp_str);
                    fflush(stdout);
                }

                stats.last_stats_time_us = now_us;
                stats.last_udp_bytes = stats.udp_bytes_read;
                stats.last_udp_packets = stats.udp_packets_read;
                stats.last_rtmp_bytes = stats.rtmp_bytes_written;
                stats.last_rtmp_video_pkts = stats.rtmp_video_packets_written;
                stats.last_rtmp_audio_pkts = stats.rtmp_audio_packets_written;
            }
        }
    }

    printf("\n");
    LOG_INFO("[RTMP STATUS] Finalizing RTMP stream and writing trailer...");
    if (ofmt_ctx) {
        av_write_trailer(ofmt_ctx);
    }

cleanup:
    LOG_INFO("Cleaning up pipeline resources...");
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (stream_mapping) {
        av_freep(&stream_mapping);
    }
    if (ofmt_ctx) {
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
        }
        avformat_free_context(ofmt_ctx);
    }
    if (ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }
    avformat_network_deinit();
    LOG_INFO("[RTMP STATUS] Connection Closed. Pipeline stopped cleanly.");
    return ret;
}
