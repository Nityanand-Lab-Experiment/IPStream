#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <libavformat/avformat.h>

#include "logger.h"
#include "hls.h"
#include "rtmp.h"
#include "srt.h"
#include "transcode.h"
#include "sdi_decklink.h"

#define PIPELINE_NAME "ixgIPStream"
#define PIPELINE_VERSION "v4.2.0"
#define MAX_OUTPUTS 16
#define MAX_STREAMS 32

typedef enum {
    OUTPUT_TYPE_RTMP = 1,
    OUTPUT_TYPE_HLS  = 2,
    OUTPUT_TYPE_SRT  = 3,
    OUTPUT_TYPE_SDI  = 4,
    OUTPUT_TYPE_UDP  = 5,
    OUTPUT_TYPE_RTP  = 6
} OutputType;

typedef struct {
    OutputType type;
    char url[1024];
} OutputTarget;

typedef enum SyncType {
    SYNC_TYPE_HARDWARE = 0, /* Synchronize to DeckLink hardware clock (default) */
    SYNC_TYPE_SELF = 1      /* Self-generated monotonic sequence clock (frame/sample counters) */
} SyncType;

typedef struct PipelineConfig {
    char input_url[1024];
    char output_url[1024]; /* Backwards-compatibility fallback / primary RTMP */
    char rtp_url[1024];    /* Backwards-compatibility fallback / primary RTP */
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
    int stats_interval_ms;   /* Interval in ms for live stats display (default: 1000) */
    bool json_telemetry;     /* Output telemetry as JSON each second */
    bool detailed_telemetry; /* Output multi-line telemetry dashboard each second */
    bool log_telemetry;      /* Output telemetry as timestamped LOG entries */
    bool enable_diagnostics; /* Real-time YouTube buffering & health diagnostics */

    /* HLS Output Settings */
    char hls_url[1024];              /* Backwards-compatibility fallback / primary HLS */
    char hls_method[16];             /* HTTP method for HLS upload (e.g., "PUT", "POST") */
    int hls_time;                    /* Target segment duration in seconds (default: 2) */
    int hls_list_size;               /* Number of segments retained in playlist (default: 5) */
    bool hls_delete_segments;        /* Delete expired segments from disk (default: true) */
    char hls_segment_filename[1024]; /* Custom segment pattern (e.g. "chunk_%05d.ts") */
    bool hls_fmp4;                   /* Fragmented MP4 vs MPEG-TS */

    /* SRT Protocol Settings (Input & Output) */
    char srt_url[1024];              /* Backwards-compatibility fallback / primary SRT */
    int srt_latency_ms;              /* Target SRT latency in ms (default: 200) */
    char srt_passphrase[128];        /* AES encryption passphrase */
    char srt_streamid[512];          /* SRT stream ID */
    char srt_mode[16];               /* "caller", "listener", "rendezvous" */
    int srt_max_clients;             /* Max concurrent clients for SRT listener server (default: 32) */

    /* Confidence Monitor Settings (JPEG Snapshot Egress) */
    char monitor_file[1024]; /* Output path for confidence monitor JPEG (e.g., /tmp/monitor.jpg) */
    int monitor_fps;         /* Capture rate in FPS (default: 5) */
    int monitor_width;       /* Optional scale width (0 = source width) */
    int monitor_height;      /* Optional scale height (0 = source height) */
    int monitor_quality;     /* JPEG quality 1-31 (default: 4) */

    /* Blackmagic SDI Ingest & Playout Settings */
    char sdi_mode[32];       /* "auto", "1080p5994", "1080p60", "1080i50", etc. */
    int sdi_channels;        /* Audio channel count (default: 2) */
    bool sdi_test_bars;      /* Generate colorbars when NO CARRIER / no input signal */

    /* A/V Synchronization Mode */
    SyncType sync_type;      /* SYNC_TYPE_HARDWARE (default) or SYNC_TYPE_SELF */

    /* Low-Latency Video Transcoding Settings */
    TranscodeConfig transcode;

    /* Audio Transcoding Settings */
    AudioTranscodeConfig audio_transcode;
} PipelineConfig;

typedef struct OutputContext {
    OutputType type;
    char url[1024];
    int index;                 /* Global output index */
    int type_index;            /* 1-based index within its type (e.g. RTMP #1, RTMP #2, SDI #1) */
    AVFormatContext *ofmt_ctx;
    AVPacket *pkt;
    int64_t stream_last_dts[MAX_STREAMS];

    SdiOutputContext *sdi_out; /* SDI playout context if type == OUTPUT_TYPE_SDI */

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

    int srt_sock;
    bool is_srt_listener;
    void *srt_listener;
    int srt_client_count;
    bool failed;
    double srt_rtt_ms;
    int srt_nack_interval;
    int srt_nack_total;
    int srt_loss_interval;
    int srt_loss_total;
    int srt_retrans_interval;
    int srt_retrans_total;
    int srt_drop_interval;
    int srt_drop_total;
    double srt_arq_success_pct;
    double srt_mbps_bandwidth;
    int srt_snd_buf_ms;
} OutputContext;

typedef struct PipelineStats {
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

    /* SRT Egress Telemetry */
    uint64_t srt_packets_written;
    uint64_t srt_video_packets_written;
    uint64_t srt_audio_packets_written;
    uint64_t srt_bytes_written;
    uint64_t last_srt_bytes;
    uint64_t last_srt_video_pkts;
    uint64_t last_srt_audio_pkts;
    int srt_sock;
    bool srt_is_listener;
    int srt_client_count;
    double srt_rtt_ms;
    int srt_nack_interval;
    int srt_nack_total;
    int srt_loss_interval;
    int srt_loss_total;
    int srt_retrans_interval;
    int srt_retrans_total;
    int srt_drop_interval;
    int srt_drop_total;
    double srt_arq_success_pct;
    double srt_mbps_bandwidth;
    int srt_snd_buf_ms;

    /* RTP Egress Telemetry */
    uint64_t rtp_packets_written;
    uint64_t rtp_video_packets_written;
    uint64_t rtp_audio_packets_written;
    uint64_t rtp_bytes_written;

    /* Confidence Monitor Telemetry */
    uint64_t monitor_frames_written;
    int64_t last_monitor_write_time_us;

    /* UDP / SRT Ingress Telemetry & Packet Loss */
    uint64_t udp_packets_read;
    uint64_t udp_bytes_read;
    uint64_t udp_dropped_pre_keyframe;
    uint64_t last_udp_bytes;
    uint64_t last_udp_packets;
    uint64_t udp_lost_packets;      /* Total MPEG-TS packets dropped (CC discontinuities) */
    uint64_t udp_cc_errors;         /* Total Continuity Counter error events */
    double udp_loss_pct;            /* Lifetime loss percentage: lost / (received + lost) * 100 */
    uint64_t last_udp_lost_packets; /* Lost packets at previous stats tick */
    double cur_interval_loss_pct;   /* Loss percentage in current 1-second interval */

    /* SRT Ingress Specific Transport Stats */
    int srt_in_sock;
    double srt_in_rtt_ms;
    int srt_in_nack_interval;
    int srt_in_nack_total;
    int srt_in_loss_interval;
    int srt_in_loss_total;
    int srt_in_retrans_interval;
    int srt_in_retrans_total;
    int srt_in_drop_interval;
    int srt_in_drop_total;
    double srt_in_arq_success_pct;
    double srt_in_rcv_rate_mbps;
    int srt_in_rcv_buf_ms;

    /* RTMP Egress Telemetry & TCP Loss */
    uint64_t rtmp_packets_written;
    uint64_t rtmp_video_packets_written;
    uint64_t rtmp_audio_packets_written;
    uint64_t rtmp_bytes_written;
    uint64_t last_rtmp_bytes;
    uint64_t last_rtmp_video_pkts;
    uint64_t last_rtmp_audio_pkts;
    int rtmp_tcp_fd;           /* Underlying TCP socket fd (-1 if unavailable) */
    uint32_t rtmp_tcp_retrans; /* Total TCP packets retransmitted (packet loss to server) */
    uint32_t rtmp_tcp_lost;    /* Packets currently lost in transit */
    double rtmp_tcp_rtt_ms;    /* Round-Trip Time to RTMP server in milliseconds */
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
    int64_t write_latency_min_us;  /* Minimum write latency in interval (microseconds) */
    int64_t write_latency_max_us;  /* Maximum write latency in interval (microseconds) */
    int64_t write_latency_sum_us;  /* Sum of all write latencies in interval */
    uint64_t write_latency_count;  /* Number of writes in interval */
    int64_t write_latency_peak_us; /* All-time peak write latency */

    /* RTMP Pacing & YouTube Live Health Diagnostics */
    int64_t first_video_pts_ms;        /* Baseline video PTS for wall-lead calculation */
    int64_t last_stats_video_pts_ms;   /* Video PTS at previous telemetry tick (for speed ratio) */
    int64_t last_video_write_time_us;  /* Monotonic timestamp of last video write */
    int64_t last_audio_write_time_us;  /* Monotonic timestamp of last audio write */
    int64_t last_packet_write_time_us; /* Monotonic timestamp of last packet write (any track) */
    int64_t max_pacing_gap_us;         /* Longest gap between consecutive packet writes in interval */
    double cur_speed_factor;           /* Real-time media speed: delta Video PTS / delta Wall-clock */
    double cur_keyframe_age_sec;       /* Time elapsed since last keyframe */
    int64_t cur_wall_lead_ms;          /* Stream headroom: (Media elapsed) - (Wall clock elapsed) */
    int64_t cur_video_silence_ms;      /* Milliseconds since last video packet sent */
    int64_t cur_audio_silence_ms;      /* Milliseconds since last audio packet sent */
    char youtube_health_status[64];    /* Composite YouTube health status string */
    /* SDI Ingest & Playout Diagnostics */
    uint64_t sdi_frames_read;
    uint64_t sdi_frames_written;
    double sdi_in_fps;
    double sdi_out_fps;
    int64_t last_sdi_read_time_us;
    int64_t last_sdi_write_time_us;

    /* Low-Latency Transcode Diagnostics */
    int64_t transcode_latency_us;  /* Microseconds taken by last encode cycle */
    double transcode_avg_ms;

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
 * Parse string into SyncType ("hardware", "hw", "self", "gen", "self-generated").
 */
SyncType parse_sync_type(const char *str);

/**
 * Get display string for SyncType.
 */
const char *sync_type_to_string(SyncType type);

/**
 * Request graceful termination of running pipeline.
 */
void pipeline_request_stop(void);

#endif /* PIPELINE_H */
