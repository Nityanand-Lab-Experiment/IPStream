#ifndef RTMP_H
#define RTMP_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>

struct OutputContext;
struct PipelineConfig;

/* Scans /proc/self/fd for established TCP socket connection */
int rtmp_find_tcp_socket_fd(const int *exclude_fds, int num_exclude);

/* Opens RTMP egress connection, discovers TCP socket, and performs handshake */
int rtmp_open_egress(struct OutputContext *oc, int *established_tcp_fds, int *num_established_tcp,
                     int64_t *last_packet_time_us);

/* Queries kernel TCP_INFO on established RTMP socket to update retrans, lost, RTT and jitter */
void rtmp_update_tcp_stats(struct OutputContext *oc);

/* Finalizes stream trailer and closes RTMP connection */
void rtmp_close_egress(struct OutputContext *oc);

#endif /* RTMP_H */
