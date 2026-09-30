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
    config->fifo_size = 50 * 1024 * 1024;      /* 50 MB FIFO buffer */
    config->buffer_size = 16 * 1024 * 1024;    /* 16 MB OS UDP receive buffer */
    config->overrun_nonfatal = 1;
    config->timeout_sec = 10;                  /* 10 seconds timeout */
    config->wait_keyframe = true;              /* Synchronize on first keyframe */
    config->normalize_pts = true;              /* Offset timestamps starting at 0 */
    config->print_stats = true;
    config->stats_interval_ms = 1000;          /* 1 second */
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

    LOG_INFO("Connecting to multicast source: %s", config->input_url);
    ret = avformat_open_input(&ifmt_ctx, config->input_url, NULL, &input_opts);
    av_dict_free(&input_opts);

    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("Failed to open multicast input '%s': %s", config->input_url, err_buf);
        goto cleanup;
    }
    LOG_INFO("Connected to multicast input source successfully.");

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

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        LOG_INFO("Opening RTMP socket connection to: %s", config->output_url);
        interrupt_ctx.last_packet_time_us = av_gettime_relative();
        ret = avio_open2(&ofmt_ctx->pb, config->output_url, AVIO_FLAG_WRITE,
                         &ofmt_ctx->interrupt_callback, &output_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("Could not open RTMP destination '%s': %s", config->output_url, err_buf);
            av_dict_free(&output_opts);
            goto cleanup;
        }
    }

    /* 7. Write FLV/RTMP Stream Header */
    LOG_INFO("Writing RTMP stream header (Remuxing / Zero-Encoding)...");
    ret = avformat_write_header(ofmt_ctx, &output_opts);
    av_dict_free(&output_opts);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("Error writing RTMP header: %s", err_buf);
        goto cleanup;
    }

    LOG_INFO("==================================================================");
    LOG_INFO("Pipeline active: Streaming UDP Multicast -> RTMP [Zero Encoding]");
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
                LOG_INFO("End of input stream reached (EOF).");
                ret = 0;
            } else if (s_stop_requested) {
                LOG_INFO("Stop requested by user signal.");
                ret = 0;
            } else {
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG_ERROR("Error reading frame from input: %s", err_buf);
            }
            break;
        }

        interrupt_ctx.last_packet_time_us = av_gettime_relative();
        stats.total_packets_read++;

        int in_idx = pkt->stream_index;
        if (in_idx < 0 || in_idx >= (int)ifmt_ctx->nb_streams || stream_mapping[in_idx] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        int out_idx = stream_mapping[in_idx];
        AVStream *in_stream = ifmt_ctx->streams[in_idx];
        AVStream *out_stream = ofmt_ctx->streams[out_idx];
        bool is_video = (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        /* Wait for first video keyframe to prevent visual glitches */
        if (!seen_first_keyframe) {
            if (is_video) {
                if (pkt->flags & AV_PKT_FLAG_KEY) {
                    seen_first_keyframe = true;
                    LOG_INFO("First video keyframe received (PTS=%lld). Starting transmission.",
                             (long long)pkt->pts);
                } else {
                    stats.dropped_pre_keyframe++;
                    av_packet_unref(pkt);
                    continue;
                }
            } else {
                /* Drop audio packets until first video keyframe arrives to keep A/V in sync */
                stats.dropped_pre_keyframe++;
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

        pkt->stream_index = out_idx;
        pkt->pos = -1;

        int packet_size = pkt->size;
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("Error writing packet to RTMP destination: %s", err_buf);
            av_packet_unref(pkt);
            break;
        }

        stats.total_packets_written++;
        stats.total_bytes_written += packet_size;
        if (is_video) {
            stats.video_packets_written++;
        } else {
            stats.audio_packets_written++;
        }

        av_packet_unref(pkt);

        /* Live throughput & statistics reporting */
        if (config->print_stats) {
            int64_t now_us = get_time_us();
            int64_t elapsed_stats_us = now_us - stats.last_stats_time_us;

            if (elapsed_stats_us >= ((int64_t)config->stats_interval_ms * 1000LL)) {
                double elapsed_sec = (double)elapsed_stats_us / 1000000.0;
                double fps = (double)(stats.video_packets_written - stats.last_stats_video_pkts) / elapsed_sec;
                double bitrate_kbps = ((double)(stats.total_bytes_written - stats.last_stats_bytes) * 8.0) /
                                      (elapsed_sec * 1000.0);

                char total_bytes_str[32];
                format_bytes(stats.total_bytes_written, total_bytes_str, sizeof(total_bytes_str));

                char uptime_str[32];
                format_time((now_us - stats.start_time_us) / 1000000LL, uptime_str, sizeof(uptime_str));

                printf("\r\033[36m[STREAMING]\033[0m Uptime: %s | Video: %5.1f fps (%llu pkts) | Audio: %llu pkts | Bitrate: %7.1f kbps | Total: %s   ",
                       uptime_str, fps, (unsigned long long)stats.video_packets_written,
                       (unsigned long long)stats.audio_packets_written, bitrate_kbps, total_bytes_str);
                fflush(stdout);

                stats.last_stats_time_us = now_us;
                stats.last_stats_bytes = stats.total_bytes_written;
                stats.last_stats_video_pkts = stats.video_packets_written;
            }
        }
    }

    printf("\n");
    LOG_INFO("Finalizing RTMP stream and writing trailer...");
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
    LOG_INFO("Pipeline stopped cleanly.");
    return ret;
}
