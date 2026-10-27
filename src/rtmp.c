#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "rtmp.h"
#include "pipeline.h"
#include "utils.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/time.h>

int rtmp_find_tcp_socket_fd(const int *exclude_fds, int num_exclude) {
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

int rtmp_open_egress(OutputContext *oc, int *established_tcp_fds, int *num_established_tcp,
                     int64_t *last_packet_time_us) {
    if (!oc || !oc->ofmt_ctx) return -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    AVDictionary *output_opts = NULL;
    av_dict_set(&output_opts, "flvflags", "no_duration_filesize+aac_seq_header_detect", 0);
    av_dict_set(&output_opts, "rtmp_live", "live", 0);
    av_dict_set(&output_opts, "rtmp_buffer", "100", 0); /* 100ms low-latency buffer */
    av_dict_set(&output_opts, "enhanced_flv", "1", 0);  /* Enhanced RTMP for H.265/HEVC */

    int64_t conn_start_us = get_time_us();
    int ret = 0;
    if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        LOG_INFO("[RTMP #%d] Initiating connection to: %s ...", oc->type_index, oc->url);
        if (last_packet_time_us) {
            *last_packet_time_us = av_gettime_relative();
        }
        ret = avio_open2(&oc->ofmt_ctx->pb, oc->url, AVIO_FLAG_WRITE,
                         &oc->ofmt_ctx->interrupt_callback, &output_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[RTMP #%d] Connection Failed to '%s': %s", oc->type_index, oc->url, err_buf);
            av_dict_free(&output_opts);
            return ret;
        }
    }
    oc->conn_time_ms = (get_time_us() - conn_start_us) / 1000;
    int fd = rtmp_find_tcp_socket_fd(established_tcp_fds, num_established_tcp ? *num_established_tcp : 0);
    if (fd >= 0) {
        oc->tcp_fd = fd;
        if (established_tcp_fds && num_established_tcp && *num_established_tcp < MAX_OUTPUTS) {
            established_tcp_fds[(*num_established_tcp)++] = fd;
        }
        LOG_INFO("[RTMP #%d] Connected: TCP socket (fd=%d) established in %lld ms. TCP network loss & RTT active.",
                 oc->type_index, fd, (long long)oc->conn_time_ms);
    } else {
        LOG_INFO("[RTMP #%d] Connected: TCP socket established in %lld ms.",
                 oc->type_index, (long long)oc->conn_time_ms);
    }

    int64_t handshake_start_us = get_time_us();
    LOG_INFO("[RTMP #%d] Performing RTMP handshake & publishing stream metadata...", oc->type_index);
    ret = avformat_write_header(oc->ofmt_ctx, &output_opts);
    av_dict_free(&output_opts);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("[RTMP #%d] Handshake / Stream Header rejected: %s", oc->type_index, err_buf);
        return ret;
    }
    oc->handshake_time_ms = (get_time_us() - handshake_start_us) / 1000;
    LOG_INFO("[RTMP #%d] Handshake confirmed in %lld ms! RTMP state: STREAMING.",
             oc->type_index, (long long)oc->handshake_time_ms);
    return 0;
}

void rtmp_update_tcp_stats(OutputContext *oc) {
    if (!oc || oc->tcp_fd < 0) return;
    struct tcp_info tinfo;
    socklen_t tlen = sizeof(tinfo);
    if (getsockopt(oc->tcp_fd, IPPROTO_TCP, TCP_INFO, &tinfo, &tlen) == 0) {
        oc->tcp_retrans = tinfo.tcpi_total_retrans;
        oc->tcp_lost = tinfo.tcpi_lost;
        oc->tcp_rtt_ms = (double)tinfo.tcpi_rtt / 1000.0;
        oc->tcp_rttvar_ms = (double)tinfo.tcpi_rttvar / 1000.0;
    }
}

void rtmp_close_egress(OutputContext *oc) {
    if (!oc || !oc->ofmt_ctx) return;
    LOG_INFO("[RTMP #%d] Finalizing stream and writing trailer...", oc->type_index);
    av_write_trailer(oc->ofmt_ctx);
    if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&oc->ofmt_ctx->pb);
    }
}
