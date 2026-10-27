#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "hls.h"
#include "rtmp.h"
#include "pipeline.h"
#include "utils.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <libavutil/time.h>

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
        hls_update_tcp_stats(hls);
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
            if (hls->cur_video_pts != AV_NOPTS_VALUE && hls->active_segment_start_pts != AV_NOPTS_VALUE &&
                hls->cur_video_pts > hls->active_segment_start_pts) {
                int64_t delta_pts = hls->cur_video_pts - hls->active_segment_start_pts;
                double seg_dur_sec = (double)delta_pts * av_q2d(hls->video_time_base);
                hls->last_segment_duration_sec = seg_dur_sec;
                if (elapsed_us > 0) {
                    hls->last_segment_speed_factor = seg_dur_sec / ((double)elapsed_us / 1000000.0);
                }
            } else {
                hls->last_segment_duration_sec = (double)hls->target_duration_sec;
                if (elapsed_us > 0) {
                    hls->last_segment_speed_factor = (double)hls->target_duration_sec / ((double)elapsed_us / 1000000.0);
                }
            }
        }
        if (seg_bytes > 0) {
            hls->last_segment_size_bytes = seg_bytes;
            hls->segments_completed++;
        }
        snprintf(hls->origin_health_str, sizeof(hls->origin_health_str),
                 "HTTP 200 OK (%llu errors, %llu retries)",
                 (unsigned long long)hls->http_errors, (unsigned long long)hls->http_retries);
    }

    return ret;
}

void hls_init_telemetry_state(HlsTelemetryState *hls, const char *url, const PipelineConfig *config) {
    if (!hls) return;
    memset(hls, 0, sizeof(*hls));
    hls->tcp_fd = -1;
    snprintf(hls->target_url, sizeof(hls->target_url), "%s", url ? url : "");
    snprintf(hls->method, sizeof(hls->method), "%s", (config && config->hls_method[0]) ? config->hls_method : "PUT");
    hls->target_duration_sec = (config && config->hls_time > 0) ? config->hls_time : 2;
    hls->list_size = (config && config->hls_list_size > 0) ? config->hls_list_size : 5;
    hls->active_segment_start_pts = AV_NOPTS_VALUE;
    hls->cur_video_pts = AV_NOPTS_VALUE;
    strncpy(hls->origin_health_str, "HTTP 200 OK (0 errors, 0 retries)", sizeof(hls->origin_health_str) - 1);
}

void hls_install_custom_io(OutputContext *oc) {
    if (!oc || !oc->ofmt_ctx) return;
    HlsTelemetryState *hls = &oc->hls_state;
    hls->orig_io_open = oc->ofmt_ctx->io_open;
    hls->orig_io_close2 = oc->ofmt_ctx->io_close2;
    oc->ofmt_ctx->io_open = hls_custom_io_open;
    oc->ofmt_ctx->io_close2 = hls_custom_io_close2;
    oc->ofmt_ctx->opaque = hls;
}

void hls_update_tcp_stats(HlsTelemetryState *hls) {
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
        int new_fd = rtmp_find_tcp_socket_fd(hls->exclude_fds, hls->num_exclude_fds);
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

int hls_open_egress(OutputContext *oc, const PipelineConfig *config, int64_t *last_packet_time_us) {
    if (!oc || !oc->ofmt_ctx) return -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    AVDictionary *hls_opts = NULL;
    char time_str[16], list_size_str[16];
    snprintf(time_str, sizeof(time_str), "%d", config->hls_time > 0 ? config->hls_time : 2);
    snprintf(list_size_str, sizeof(list_size_str), "%d", config->hls_list_size > 0 ? config->hls_list_size : 5);
    av_dict_set(&hls_opts, "hls_time", time_str, 0);
    av_dict_set(&hls_opts, "hls_list_size", list_size_str, 0);
    if (config->hls_delete_segments) {
        av_dict_set(&hls_opts, "hls_flags", "delete_segments+split_by_time+omit_endlist", 0);
    } else {
        av_dict_set(&hls_opts, "hls_flags", "split_by_time+omit_endlist", 0);
    }

    if (config->hls_method[0] != '\0') {
        av_dict_set(&hls_opts, "method", config->hls_method, 0);
    }

    if (strncmp(oc->url, "http://", 7) == 0 || strncmp(oc->url, "https://", 8) == 0) {
        av_dict_set(&hls_opts, "http_persistent", "1", 0);
    }

    if (config->hls_fmp4) {
        av_dict_set(&hls_opts, "hls_segment_type", "fmp4", 0);
    }

    if (config->hls_segment_filename[0] != '\0') {
        av_dict_set(&hls_opts, "hls_segment_filename", config->hls_segment_filename, 0);
    } else if (strncmp(oc->url, "http://", 7) == 0 || strncmp(oc->url, "https://", 8) == 0) {
        char seg_url[2048];
        const char *dot = strrchr(oc->url, '.');
        const char *ext = config->hls_fmp4 ? ".m4s" : ".ts";
        if (dot && strcmp(dot, ".m3u8") == 0) {
            int prefix_len = dot - oc->url;
            snprintf(seg_url, sizeof(seg_url), "%.*s_%%05d%s", prefix_len, oc->url, ext);
        } else {
            snprintf(seg_url, sizeof(seg_url), "%s_%%05d%s", oc->url, ext);
        }
        av_dict_set(&hls_opts, "hls_segment_filename", seg_url, 0);
    }

    int64_t conn_start_us = get_time_us();
    int ret = 0;
    if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        LOG_INFO("[HLS #%d] Initiating HLS egress: %s (Method: %s) ...",
                 oc->type_index, oc->url,
                 (strncmp(oc->url, "http", 4) == 0) ? (config->hls_method[0] ? config->hls_method : "PUT") : "FILE");
        if (last_packet_time_us) {
            *last_packet_time_us = av_gettime_relative();
        }
        ret = avio_open2(&oc->ofmt_ctx->pb, oc->url, AVIO_FLAG_WRITE,
                         &oc->ofmt_ctx->interrupt_callback, &hls_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[HLS #%d] Connection/Open Failed to '%s': %s", oc->type_index, oc->url, err_buf);
            av_dict_free(&hls_opts);
            return ret;
        }
    }
    oc->conn_time_ms = (get_time_us() - conn_start_us) / 1000;
    if (strncmp(oc->url, "http", 4) == 0) {
        hls_update_tcp_stats(&oc->hls_state);
        oc->tcp_fd = oc->hls_state.tcp_fd;
        if (oc->tcp_fd >= 0) {
            LOG_INFO("[HLS #%d] Connected: TCP socket (fd=%d) established to CDN.", oc->type_index, oc->tcp_fd);
        }
    }

    LOG_INFO("[HLS #%d] Writing HLS playlist headers & metadata...", oc->type_index);
    ret = avformat_write_header(oc->ofmt_ctx, &hls_opts);
    av_dict_free(&hls_opts);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("[HLS #%d] HLS Playlist Header rejected: %s", oc->type_index, err_buf);
        return ret;
    }
    LOG_INFO("[HLS #%d] HLS publishing active. Target: %s", oc->type_index, oc->url);
    return 0;
}

void hls_track_video_pts(HlsTelemetryState *hls, int64_t pts) {
    if (hls) {
        hls->cur_video_pts = pts;
    }
}

void hls_close_egress(OutputContext *oc) {
    if (!oc || !oc->ofmt_ctx) return;
    LOG_INFO("[HLS #%d] Finalizing stream and writing trailer...", oc->type_index);
    av_write_trailer(oc->ofmt_ctx);
    if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&oc->ofmt_ctx->pb);
    }
}
