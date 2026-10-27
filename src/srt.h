#ifndef SRT_H
#define SRT_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>

struct OutputContext;
struct PipelineConfig;
struct PipelineStats;

/* Extracts underlying SRTSOCKET file descriptor from AVIOContext opaque handle */
int srt_extract_socket(AVIOContext *pb);

/* Configures AVDictionary connection options for SRT Ingress */
int srt_setup_ingress_options(AVDictionary **opts, const struct PipelineConfig *config);

/* Opens SRT egress connection and writes MPEG-TS header */
int srt_open_egress(struct OutputContext *oc, const struct PipelineConfig *config,
                    int64_t *last_packet_time_us);

/* Polls libsrt QoS metrics for SRT Ingress and populates PipelineStats */
void srt_update_ingress_stats(int srt_sock, struct PipelineStats *stats);

/* Polls libsrt QoS metrics for SRT Egress and populates OutputContext */
void srt_update_egress_stats(struct OutputContext *oc);

/* Finalizes stream trailer and closes SRT egress */
void srt_close_egress(struct OutputContext *oc);

#endif /* SRT_H */
