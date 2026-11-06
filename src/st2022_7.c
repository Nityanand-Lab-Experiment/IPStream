#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "st2022_7.h"
#include "pipeline.h"
#include "logger.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define ST2022_RING_SIZE 8192
#define MAX_PAYLOAD_SIZE 2048

typedef struct {
    uint64_t ext_seq;
    uint32_t rtp_ts;
    int path_origin;               /* 1 = Path A, 2 = Path B */
    int64_t recv_time_us[3];       /* [1] = Path A, [2] = Path B */
    uint8_t payload[MAX_PAYLOAD_SIZE];
    int payload_len;
    bool occupied;
} St2022Slot;

struct St2022Context {
    int sock_a;
    int sock_b;
    pthread_t thread_a;
    pthread_t thread_b;
    volatile bool stop_requested;

    int skew_ms;
    int buffer_size_a;
    int buffer_size_b;
    char mode[16];

    /* Ring Buffer & Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    St2022Slot slots[ST2022_RING_SIZE];
    
    bool initialized;
    uint16_t last_ref_seq;
    uint64_t last_ref_ext_seq;
    uint64_t playout_seq;
    int64_t stream_start_time_us;
    int64_t first_pkt_time_us;
    int64_t gap_start_us;
    bool playout_started;

    /* Leftover bytes for partial AVIO reads */
    uint8_t leftover_buf[MAX_PAYLOAD_SIZE];
    int leftover_len;
    int leftover_offset;

    /* Diagnostics & Stats */
    St2022Stats stats;
    int64_t last_active_a_us;
    int64_t last_active_b_us;
};

static int parse_ip_port(const char *url, char *host, size_t host_len, int *port) {
    if (!url || !host || !port) return -1;
    const char *p = strstr(url, "://");
    if (p) p += 3;
    else p = url;

    if (*p == '@') p++;

    const char *colon = strrchr(p, ':');
    if (!colon) return -1;

    size_t hlen = colon - p;
    if (hlen >= host_len) hlen = host_len - 1;
    strncpy(host, p, hlen);
    host[hlen] = '\0';

    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

static int open_udp_socket(const char *url, const char *local_addr, int rcvbuf_bytes, const char *path_label) {
    char host[128] = {0};
    int port = 0;
    if (parse_ip_port(url, host, sizeof(host), &port) < 0) {
        LOG_ERROR("[ST 2022-7 %s] Failed to parse URL: %s", path_label, url);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_ERROR("[ST 2022-7 %s] socket() creation failed: %s", path_label, strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    if (rcvbuf_bytes > 0) {
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes, sizeof(rcvbuf_bytes));
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms read timeout */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct in_addr mcast_addr;
    bool is_mcast = (inet_aton(host, &mcast_addr) && IN_MULTICAST(ntohl(mcast_addr.s_addr)));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);

    if (is_mcast) {
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (local_addr && strlen(local_addr) > 0) {
            bind_addr.sin_addr.s_addr = inet_addr(local_addr);
        } else {
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    }

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERROR("[ST 2022-7 %s] Failed to bind to port %d: %s", path_label, port, strerror(errno));
        close(sock);
        return -1;
    }

    if (is_mcast) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = mcast_addr;
        if (local_addr && strlen(local_addr) > 0) {
            mreq.imr_interface.s_addr = inet_addr(local_addr);
            LOG_INFO("[ST 2022-7 %s] Joining multicast %s:%d on interface %s",
                     path_label, host, port, local_addr);
        } else {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            LOG_INFO("[ST 2022-7 %s] Joining multicast %s:%d on default interface",
                     path_label, host, port);
        }

        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            LOG_ERROR("[ST 2022-7 %s] IP_ADD_MEMBERSHIP failed for %s: %s",
                      path_label, host, strerror(errno));
            close(sock);
            return -1;
        }
    } else {
        LOG_INFO("[ST 2022-7 %s] Bound unicast UDP receiver on %s:%d",
                 path_label, (local_addr && strlen(local_addr) > 0) ? local_addr : "0.0.0.0", port);
    }

    return sock;
}

static void st2022_ring_insert(St2022Context *ctx, int path, uint16_t rtp_seq, uint32_t rtp_ts,
                               const uint8_t *payload, int payload_len, int64_t now_us) {
    if (payload_len <= 0 || payload_len > MAX_PAYLOAD_SIZE) return;

    pthread_mutex_lock(&ctx->lock);

    if (path == 1) {
        ctx->stats.pkts_path_a++;
        ctx->stats.bytes_path_a += payload_len;
        ctx->last_active_a_us = now_us;
        ctx->stats.path_a_active = true;
    } else {
        ctx->stats.pkts_path_b++;
        ctx->stats.bytes_path_b += payload_len;
        ctx->last_active_b_us = now_us;
        ctx->stats.path_b_active = true;
    }

    if (!ctx->initialized) {
        ctx->initialized = true;
        ctx->last_ref_seq = rtp_seq;
        ctx->last_ref_ext_seq = rtp_seq;
        ctx->playout_seq = rtp_seq;
        ctx->first_pkt_time_us = now_us;
        ctx->stream_start_time_us = now_us;
    }

    /* Unpack 16-bit RTP Sequence to 64-bit Extended Monotonic Sequence */
    int16_t diff = (int16_t)(rtp_seq - ctx->last_ref_seq);
    uint64_t ext_seq = ctx->last_ref_ext_seq + diff;
    if (diff > 0 && diff < 16384) {
        ctx->last_ref_seq = rtp_seq;
        ctx->last_ref_ext_seq = ext_seq;
    }

    /* Check if packet is already played out */
    if (ctx->playout_started && ext_seq < ctx->playout_seq) {
        ctx->stats.duplicates_dropped++;
        pthread_mutex_unlock(&ctx->lock);
        return;
    }

    size_t idx = ext_seq % ST2022_RING_SIZE;
    St2022Slot *slot = &ctx->slots[idx];

    if (slot->occupied && slot->ext_seq == ext_seq) {
        /* Duplicate packet arriving from the other redundant path */
        ctx->stats.duplicates_dropped++;
        slot->recv_time_us[path] = now_us;

        /* Measure differential path skew */
        int other_path = 3 - path;
        if (slot->recv_time_us[other_path] > 0) {
            double delta_ms = (double)(slot->recv_time_us[2] - slot->recv_time_us[1]) / 1000.0;
            ctx->stats.differential_skew_ms = delta_ms;
        }

        pthread_mutex_unlock(&ctx->lock);
        return;
    }

    /* Clean old slot if re-used */
    slot->ext_seq = ext_seq;
    slot->rtp_ts = rtp_ts;
    slot->path_origin = path;
    slot->recv_time_us[path] = now_us;
    slot->recv_time_us[3 - path] = 0;
    memcpy(slot->payload, payload, payload_len);
    slot->payload_len = payload_len;
    slot->occupied = true;

    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

static void *st2022_path_worker(void *arg, int path, int sock) {
    St2022Context *ctx = (St2022Context *)arg;
    uint8_t buffer[65536];

    while (!ctx->stop_requested) {
        ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            continue;
        }

        int64_t now_us = get_time_us();
        bool is_rtp = false;
        uint16_t seq = 0;
        uint32_t ts = 0;
        const uint8_t *payload = buffer;
        int payload_len = (int)n;

        /* Check for RFC 2250 RTP header */
        if (n >= 12 && (buffer[0] & 0xC0) == 0x80) {
            uint8_t pt = buffer[1] & 0x7F;
            if (pt == 33 || (n - 12) % 188 == 0) { /* RFC 2250 MPEG-TS */
                is_rtp = true;
                seq = (buffer[2] << 8) | buffer[3];
                ts = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                     ((uint32_t)buffer[6] << 8) | buffer[7];
                payload = buffer + 12;
                payload_len = (int)n - 12;
            }
        }

        if (!is_rtp) {
            /* Raw MPEG-TS without RTP header: derive rolling sequence from continuity counters */
            if (n >= 188 && buffer[0] == 0x47) {
                static uint16_t s_fallback_seq[3] = {0, 0, 0};
                seq = s_fallback_seq[path]++;
            } else {
                continue;
            }
        }

        st2022_ring_insert(ctx, path, seq, ts, payload, payload_len, now_us);
    }

    return NULL;
}

static void *path_a_thread_func(void *arg) {
    St2022Context *ctx = (St2022Context *)arg;
    return st2022_path_worker(ctx, 1, ctx->sock_a);
}

static void *path_b_thread_func(void *arg) {
    St2022Context *ctx = (St2022Context *)arg;
    return st2022_path_worker(ctx, 2, ctx->sock_b);
}

St2022Context *st2022_7_open(const PipelineConfig *config) {
    if (!config || !config->enable_st2022_7) return NULL;

    St2022Context *ctx = (St2022Context *)calloc(1, sizeof(St2022Context));
    if (!ctx) return NULL;

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    ctx->skew_ms = (config->hitless_skew_ms > 0) ? config->hitless_skew_ms : 100;
    ctx->buffer_size_a = (config->buffer_size > 0) ? config->buffer_size : (16 * 1024 * 1024);
    ctx->buffer_size_b = (config->buffer_size_b > 0) ? config->buffer_size_b : ctx->buffer_size_a;
    snprintf(ctx->mode, sizeof(ctx->mode), "%s", config->hitless_mode[0] ? config->hitless_mode : "auto");

    ctx->sock_a = open_udp_socket(config->input_url, config->local_addr, ctx->buffer_size_a, "Path A");
    if (ctx->sock_a < 0) {
        LOG_ERROR("[ST 2022-7] Failed to open Path A socket: %s", config->input_url);
        free(ctx);
        return NULL;
    }

    ctx->sock_b = open_udp_socket(config->input_b_url, config->local_addr_b, ctx->buffer_size_b, "Path B");
    if (ctx->sock_b < 0) {
        LOG_ERROR("[ST 2022-7] Failed to open Path B socket: %s", config->input_b_url);
        close(ctx->sock_a);
        free(ctx);
        return NULL;
    }

    ctx->stop_requested = false;
    if (pthread_create(&ctx->thread_a, NULL, path_a_thread_func, ctx) != 0) {
        LOG_ERROR("[ST 2022-7] Failed to create Path A thread");
        close(ctx->sock_a);
        close(ctx->sock_b);
        free(ctx);
        return NULL;
    }

    if (pthread_create(&ctx->thread_b, NULL, path_b_thread_func, ctx) != 0) {
        LOG_ERROR("[ST 2022-7] Failed to create Path B thread");
        ctx->stop_requested = true;
        pthread_join(ctx->thread_a, NULL);
        close(ctx->sock_a);
        close(ctx->sock_b);
        free(ctx);
        return NULL;
    }

    LOG_INFO("[ST 2022-7] Seamless Protection Switching engaged. Skew Tolerance Buffer: %d ms", ctx->skew_ms);
    return ctx;
}

int st2022_7_read_packet(void *opaque, uint8_t *buf, int buf_size) {
    St2022Context *ctx = (St2022Context *)opaque;
    if (!ctx || !buf || buf_size <= 0) return AVERROR(EINVAL);

    int bytes_copied = 0;

    /* 1. Drain leftover bytes from previous packet if available */
    if (ctx->leftover_len > 0) {
        int avail = ctx->leftover_len - ctx->leftover_offset;
        int to_copy = (avail < buf_size) ? avail : buf_size;
        memcpy(buf, ctx->leftover_buf + ctx->leftover_offset, to_copy);
        ctx->leftover_offset += to_copy;
        bytes_copied += to_copy;
        if (ctx->leftover_offset >= ctx->leftover_len) {
            ctx->leftover_len = 0;
            ctx->leftover_offset = 0;
        }
        if (bytes_copied >= buf_size) return bytes_copied;
    }

    while (bytes_copied < buf_size && !ctx->stop_requested) {
        pthread_mutex_lock(&ctx->lock);

        int64_t now_us = get_time_us();

        /* Playout Startup Delay Check */
        if (!ctx->playout_started) {
            if (!ctx->initialized || (now_us - ctx->first_pkt_time_us) < ((int64_t)ctx->skew_ms * 1000LL)) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 10000000; /* 10ms */
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000;
                }
                pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts);
                pthread_mutex_unlock(&ctx->lock);
                continue;
            }
            ctx->playout_started = true;
            LOG_INFO("[ST 2022-7] Playout buffer primed (%d ms skew window elapsed). Stream playout active.",
                     ctx->skew_ms);
        }

        size_t idx = ctx->playout_seq % ST2022_RING_SIZE;
        St2022Slot *slot = &ctx->slots[idx];

        if (slot->occupied && slot->ext_seq == ctx->playout_seq) {
            /* Packet is ready for playout */
            if (slot->path_origin == 2) {
                ctx->stats.pkts_used_b++;
                /* Salvaged from Path B when Path A dropped/delayed */
                if (slot->recv_time_us[1] == 0) {
                    ctx->stats.pkts_salvaged++;
                }
            } else {
                ctx->stats.pkts_used_a++;
            }

            int space = buf_size - bytes_copied;
            if (slot->payload_len <= space) {
                memcpy(buf + bytes_copied, slot->payload, slot->payload_len);
                bytes_copied += slot->payload_len;
            } else {
                memcpy(buf + bytes_copied, slot->payload, space);
                bytes_copied += space;
                int remaining = slot->payload_len - space;
                memcpy(ctx->leftover_buf, slot->payload + space, remaining);
                ctx->leftover_len = remaining;
                ctx->leftover_offset = 0;
            }

            slot->occupied = false;
            ctx->playout_seq++;
            ctx->stats.pkts_output++;
            ctx->gap_start_us = 0;
            pthread_mutex_unlock(&ctx->lock);
            break;
        } else {
            /* Slot is missing. Has it exceeded skew tolerance wait time? */
            int64_t wait_limit_us = (int64_t)ctx->skew_ms * 1000LL;

            if (ctx->last_ref_ext_seq > ctx->playout_seq) {
                if (ctx->gap_start_us == 0) {
                    ctx->gap_start_us = now_us;
                }
                if ((now_us - ctx->gap_start_us) > wait_limit_us ||
                    (ctx->last_ref_ext_seq - ctx->playout_seq) > (unsigned int)(ctx->skew_ms * 10)) {
                    /* Packet unrecoverable on BOTH Path A and Path B. Advance to prevent stall */
                    ctx->stats.gaps_unrecoverable++;
                    ctx->playout_seq++;
                    ctx->gap_start_us = 0;
                    pthread_mutex_unlock(&ctx->lock);
                    continue;
                }
            } else {
                /* At head of stream, waiting for sender to emit next packet */
                ctx->gap_start_us = 0;
            }

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000; /* 5ms wait */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&ctx->cond, &ctx->lock, &ts);
            pthread_mutex_unlock(&ctx->lock);
        }
    }

    if (ctx->stop_requested && bytes_copied == 0) {
        return AVERROR_EOF;
    }
    return bytes_copied;
}

void st2022_7_get_stats(St2022Context *ctx, St2022Stats *stats) {
    if (!ctx || !stats) return;
    pthread_mutex_lock(&ctx->lock);
    *stats = ctx->stats;
    int64_t now_us = get_time_us();
    stats->path_a_active = (now_us - ctx->last_active_a_us < 2000000LL);
    stats->path_b_active = (now_us - ctx->last_active_b_us < 2000000LL);
    if (ctx->initialized && ctx->last_ref_ext_seq >= ctx->playout_seq) {
        stats->buffer_depth_pkts = (int)(ctx->last_ref_ext_seq - ctx->playout_seq);
    } else {
        stats->buffer_depth_pkts = 0;
    }
    pthread_mutex_unlock(&ctx->lock);
}

void st2022_7_request_stop(St2022Context *ctx) {
    if (!ctx) return;
    ctx->stop_requested = true;

    /* Wake any thread blocked in st2022_7_read_packet() */
    pthread_mutex_lock(&ctx->lock);
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

void st2022_7_close(St2022Context **ctx_ptr) {
    if (!ctx_ptr || !*ctx_ptr) return;
    St2022Context *ctx = *ctx_ptr;
    ctx->stop_requested = true;

    pthread_mutex_lock(&ctx->lock);
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);

    pthread_join(ctx->thread_a, NULL);
    pthread_join(ctx->thread_b, NULL);

    if (ctx->sock_a >= 0) close(ctx->sock_a);
    if (ctx->sock_b >= 0) close(ctx->sock_b);

    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cond);

    free(ctx);
    *ctx_ptr = NULL;
    LOG_INFO("[ST 2022-7] Seamless Redundancy engine released successfully.");
}
