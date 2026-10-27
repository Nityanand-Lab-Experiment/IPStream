#ifndef HLS_H
#define HLS_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>

#define MAX_OUTPUTS 16

typedef struct HlsTelemetryState {
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

struct OutputContext;
struct PipelineConfig;

void hls_init_telemetry_state(HlsTelemetryState *hls, const char *url, const struct PipelineConfig *config);
void hls_install_custom_io(struct OutputContext *oc);
int hls_open_egress(struct OutputContext *oc, const struct PipelineConfig *config, int64_t *last_packet_time_us);
void hls_update_tcp_stats(HlsTelemetryState *hls);
void hls_track_video_pts(HlsTelemetryState *hls, int64_t pts);
void hls_close_egress(struct OutputContext *oc);

#endif /* HLS_H */
