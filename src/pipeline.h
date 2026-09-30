#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char input_url[1024];
    char output_url[1024];
    char local_addr[128];      /* Local interface IP for multicast bind */
    int fifo_size;              /* UDP fifo size in bytes (e.g., 50MB) */
    int buffer_size;            /* UDP socket receive buffer in bytes (e.g., 16MB) */
    int overrun_nonfatal;       /* 1 to survive UDP buffer overruns */
    int timeout_sec;            /* Network read timeout in seconds */
    bool wait_keyframe;         /* Wait for first video keyframe before muxing */
    bool normalize_pts;         /* Offset PTS/DTS to start from 0 */
    bool print_stats;           /* Display live throughput & packet statistics */
    int stats_interval_ms;      /* Interval in ms for live stats display (default: 1000) */
    bool json_telemetry;        /* Output telemetry as JSON each second */
    bool detailed_telemetry;    /* Output multi-line telemetry dashboard each second */
} PipelineConfig;

typedef struct {
    /* UDP Ingress Telemetry */
    uint64_t udp_packets_read;
    uint64_t udp_bytes_read;
    uint64_t udp_dropped_pre_keyframe;
    uint64_t last_udp_bytes;
    uint64_t last_udp_packets;

    /* RTMP Egress Telemetry */
    uint64_t rtmp_packets_written;
    uint64_t rtmp_video_packets_written;
    uint64_t rtmp_audio_packets_written;
    uint64_t rtmp_bytes_written;
    uint64_t last_rtmp_bytes;
    uint64_t last_rtmp_video_pkts;
    uint64_t last_rtmp_audio_pkts;

    /* Current Output Timestamps (milliseconds) */
    int64_t cur_video_pts_ms;
    int64_t cur_audio_pts_ms;

    /* Timing */
    int64_t start_time_us;
    int64_t last_stats_time_us;
} PipelineStats;

/**
 * Initialize default configuration settings.
 */
void pipeline_config_init_defaults(PipelineConfig *config);

/**
 * Run the zero-encoding multicast-to-RTMP remuxing pipeline.
 * Blocks until stopped or error occurs.
 *
 * @param config Pointer to configured pipeline options.
 * @return 0 on success, negative error code on failure.
 */
int pipeline_run(const PipelineConfig *config);

/**
 * Request graceful termination of running pipeline.
 */
void pipeline_request_stop(void);

#endif /* PIPELINE_H */
