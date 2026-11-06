#ifndef ST2022_7_H
#define ST2022_7_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct St2022Stats {
    uint64_t pkts_path_a;
    uint64_t pkts_path_b;
    uint64_t bytes_path_a;
    uint64_t bytes_path_b;
    uint64_t pkts_output;
    uint64_t pkts_used_a;          /* Packets selected and used from Path 0 (Path A) */
    uint64_t pkts_used_b;          /* Packets selected and used from Path 1 (Path B) */
    uint64_t pkts_salvaged;        /* Packets missing on Path A, recovered from Path B */
    uint64_t duplicates_dropped;   /* Duplicate packets discarded */
    uint64_t gaps_unrecoverable;   /* Sequence gaps missing on both paths */
    double differential_skew_ms;   /* Arrival time delta (Path B vs Path A) */
    bool path_a_active;
    bool path_b_active;
    int buffer_depth_pkts;
} St2022Stats;

typedef struct St2022Context St2022Context;

struct PipelineConfig;

/**
 * Initialize and open SMPTE ST 2022-7 dual receiver for Path A and Path B.
 *
 * @param config Pipeline configuration containing input_url, input_b_url, local_addr, local_addr_b, etc.
 * @return Opaque St2022Context pointer, or NULL on error.
 */
St2022Context *st2022_7_open(const struct PipelineConfig *config);

/**
 * AVIO read callback: delivers the seamless, de-duplicated MPEG-TS packet stream to FFmpeg.
 *
 * @param opaque St2022Context pointer.
 * @param buf Destination buffer allocated by FFmpeg.
 * @param buf_size Size of destination buffer.
 * @return Number of bytes read, or AVERROR on error/EOF.
 */
int st2022_7_read_packet(void *opaque, uint8_t *buf, int buf_size);

/**
 * Query current ST 2022-7 health and path statistics.
 */
void st2022_7_get_stats(St2022Context *ctx, St2022Stats *stats);

/**
 * Request the ST 2022-7 receiver to stop.
 * Wakes blocked st2022_7_read_packet() calls so the pipeline can unwind.
 * Safe to call from a signal handler context (async-signal-safe path).
 */
void st2022_7_request_stop(St2022Context *ctx);

/**
 * Stop receiver threads and release all ST 2022-7 resources and ring buffers.
 */
void st2022_7_close(St2022Context **ctx);

#ifdef __cplusplus
}
#endif

#endif /* ST2022_7_H */
