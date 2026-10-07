#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#define PIPELINE_NAME "ixgIPStream"
#define PIPELINE_VERSION "v4.1.1"
#define MAX_OUTPUTS 16

typedef enum { OUTPUT_TYPE_RTMP = 1, OUTPUT_TYPE_HLS = 2 } OutputType;

typedef struct {
  OutputType type;
  char url[1024];
} OutputTarget;

typedef struct {
  char input_url[1024];
  char output_url[1024]; /* Backwards-compatibility fallback / primary RTMP */
  OutputTarget outputs[MAX_OUTPUTS];
  int num_outputs;
  char local_addr[128];    /* Local interface IP for multicast bind */
  int fifo_size;           /* UDP fifo size in 188-byte packets */
  int buffer_size;         /* UDP socket receive buffer in bytes (e.g., 16MB) */
  int overrun_nonfatal;    /* 1 to survive UDP buffer overruns */
  int timeout_sec;         /* Network read timeout in seconds */
  bool wait_keyframe;      /* Wait for first video keyframe before muxing */
  bool normalize_pts;      /* Offset PTS/DTS to start from 0 */
  bool print_stats;        /* Display live throughput & packet statistics */
  int stats_interval_ms;   /* Interval in ms for live stats display (default:
                              1000) */
  bool json_telemetry;     /* Output telemetry as JSON each second */
  bool detailed_telemetry; /* Output multi-line telemetry dashboard each second
                            */
  bool log_telemetry;      /* Output telemetry as timestamped LOG entries */
  bool
      enable_diagnostics; /* Real-time YouTube buffering & health diagnostics */

  /* HLS Output Settings */
  char hls_url[1024];  /* Backwards-compatibility fallback / primary HLS */
  char hls_method[16]; /* HTTP method for HLS upload (e.g., "PUT", "POST") */
  int hls_time;        /* Target segment duration in seconds (default: 2) */
  int hls_list_size; /* Number of segments retained in playlist (default: 5) */
  bool hls_delete_segments; /* Delete expired segments from disk (default: true)
                             */
  char hls_segment_filename[1024]; /* Custom segment pattern (e.g.
                                      "chunk_%05d.ts") */
  bool hls_fmp4;                   /* Fragmented MP4 vs MPEG-TS */

  /* Confidence Monitor Settings (JPEG Snapshot Egress) */
  char monitor_file[1024]; /* Output path for confidence monitor JPEG (e.g.,
                              /tmp/monitor.jpg) */
  int monitor_fps;         /* Capture rate in FPS (default: 5) */
  int monitor_width;       /* Optional scale width (0 = source width) */
  int monitor_height;      /* Optional scale height (0 = source height) */
  int monitor_quality;     /* JPEG quality 1-31 (default: 4) */
} PipelineConfig;

typedef struct {
  /* Connection Diagnostics */
  int64_t rtmp_connect_time_ms;
  int64_t rtmp_handshake_time_ms;

  /* HLS Egress Telemetry */
  uint64_t hls_packets_written;
  uint64_t hls_bytes_written;
  uint64_t last_hls_bytes;
  uint64_t last_hls_packets;
  int64_t last_hls_write_time_us;
  uint64_t hls_segments_completed;
  char hls_active_segment[256];
  int64_t hls_last_segment_size;
  double hls_last_segment_duration_sec;
  double hls_last_segment_upload_ms;
  double hls_last_segment_speed_factor;
  char hls_origin_health[128];
  int hls_tcp_fd;
  uint32_t hls_tcp_retrans;
  uint32_t hls_tcp_lost;
  double hls_tcp_rtt_ms;
  double hls_tcp_rttvar_ms;

  /* Confidence Monitor Telemetry */
  uint64_t monitor_frames_written;
  int64_t last_monitor_write_time_us;

  /* UDP Ingress Telemetry & Packet Loss */
  uint64_t udp_packets_read;
  uint64_t udp_bytes_read;
  uint64_t udp_dropped_pre_keyframe;
  uint64_t last_udp_bytes;
  uint64_t last_udp_packets;
  uint64_t
      udp_lost_packets; /* Total MPEG-TS packets dropped (CC discontinuities) */
  uint64_t udp_cc_errors; /* Total Continuity Counter error events */
  double udp_loss_pct; /* Lifetime loss percentage: lost / (received + lost) *
                          100 */
  uint64_t last_udp_lost_packets; /* Lost packets at previous stats tick */
  double
      cur_interval_loss_pct; /* Loss percentage in current 1-second interval */

  /* RTMP Egress Telemetry & TCP Loss */
  uint64_t rtmp_packets_written;
  uint64_t rtmp_video_packets_written;
  uint64_t rtmp_audio_packets_written;
  uint64_t rtmp_bytes_written;
  uint64_t last_rtmp_bytes;
  uint64_t last_rtmp_video_pkts;
  uint64_t last_rtmp_audio_pkts;
  int rtmp_tcp_fd;           /* Underlying TCP socket fd (-1 if unavailable) */
  uint32_t rtmp_tcp_retrans; /* Total TCP packets retransmitted (packet loss to
                                server) */
  uint32_t rtmp_tcp_lost;    /* Packets currently lost in transit */
  double rtmp_tcp_rtt_ms; /* Round-Trip Time to RTMP server in milliseconds */
  double rtmp_tcp_rttvar_ms; /* RTT Jitter (variance) in milliseconds */

  /* Current Output Timestamps (milliseconds) */
  int64_t cur_video_pts_ms;
  int64_t cur_audio_pts_ms;
  int64_t last_video_dts_ms;

  /* Keyframe / GOP Diagnostics for YouTube */
  int64_t last_keyframe_time_us;
  double last_keyframe_interval_sec;
  uint64_t total_keyframes_seen;
  uint64_t num_timestamp_gaps;
  uint64_t num_socket_slow_writes;

  /* RTMP Write Latency (per-interval, reset each telemetry tick) */
  int64_t write_latency_min_us;  /* Minimum write latency in interval
                                    (microseconds) */
  int64_t write_latency_max_us;  /* Maximum write latency in interval
                                    (microseconds) */
  int64_t write_latency_sum_us;  /* Sum of all write latencies in interval */
  uint64_t write_latency_count;  /* Number of writes in interval */
  int64_t write_latency_peak_us; /* All-time peak write latency */

  /* RTMP Pacing & YouTube Live Health Diagnostics */
  int64_t first_video_pts_ms; /* Baseline video PTS for wall-lead calculation */
  int64_t last_stats_video_pts_ms; /* Video PTS at previous telemetry tick (for
                                      speed ratio) */
  int64_t
      last_video_write_time_us; /* Monotonic timestamp of last video write */
  int64_t
      last_audio_write_time_us; /* Monotonic timestamp of last audio write */
  int64_t last_packet_write_time_us; /* Monotonic timestamp of last packet write
                                        (any track) */
  int64_t max_pacing_gap_us; /* Longest gap between consecutive packet writes in
                                interval */
  double cur_speed_factor;   /* Real-time media speed: delta Video PTS / delta
                                Wall-clock */
  double cur_keyframe_age_sec; /* Time elapsed since last keyframe */
  int64_t cur_wall_lead_ms;    /* Stream headroom: (Media elapsed) - (Wall clock
                                  elapsed) */
  int64_t cur_video_silence_ms; /* Milliseconds since last video packet sent */
  int64_t cur_audio_silence_ms; /* Milliseconds since last audio packet sent */
  char youtube_health_status[64]; /* Composite YouTube health status string */

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
