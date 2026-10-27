#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <srt/srt.h>

#include "srt.h"
#include "pipeline.h"
#include "utils.h"
#include "logger.h"

#define SRT_MAX_CLIENTS 32
#define SRT_DEFAULT_LATENCY_MS 120
#define SRT_TS_PKT_SIZE 1316 /* 7 x 188-byte TS packets */
#define SRT_AVIO_BUF_SIZE (1316 * 10) /* 13160 bytes */

/* FFmpeg internal URLContext & SRTContext dummy mapping for socket inspection */
typedef struct URLContextDummy {
    const AVClass *av_class;
    const void *prot;
    void *priv_data;
} URLContextDummy;

typedef struct SRTContextDummy {
    const AVClass *class;
    int fd;
} SRTContextDummy;

typedef struct SrtClient {
    SRTSOCKET sock;
    char ip[64];
    int port;
    int64_t connect_time_us;
    uint64_t bytes_sent;
    uint64_t packets_sent;
    SRT_TRACEBSTATS bstats;
    bool active;
} SrtClient;

typedef struct SrtListenerContext {
    SRTSOCKET listen_sock;
    int port;
    char bind_ip[64];
    SrtClient clients[SRT_MAX_CLIENTS];
    int max_clients;
    int client_count;
    pthread_mutex_t lock;
    pthread_t accept_thread;
    volatile bool running;

    /* Cached initial MPEG-TS headers (PAT/PMT/SDT) for late-joining clients */
    uint8_t *cached_header;
    int cached_header_size;
    int cached_header_capacity;

    OutputContext *oc;
} SrtListenerContext;

typedef struct SrtUrlParams {
    char host[256];
    int port;
    char mode[32];         /* "listener", "caller", "rendezvous" */
    int latency_ms;
    char passphrase[128];
    char streamid[512];
    int pbkeylen;
    int max_clients;
    bool is_listener;
} SrtUrlParams;

static void srt_parse_url_params(const char *url, const PipelineConfig *config, SrtUrlParams *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->port = 9000;
    p->latency_ms = (config && config->srt_latency_ms > 0) ? config->srt_latency_ms : SRT_DEFAULT_LATENCY_MS;
    p->max_clients = (config && config->srt_max_clients > 0) ? config->srt_max_clients : SRT_MAX_CLIENTS;
    if (p->max_clients > SRT_MAX_CLIENTS) p->max_clients = SRT_MAX_CLIENTS;
    p->pbkeylen = 16;
    if (config) {
        if (config->srt_mode[0] != '\0') {
            snprintf(p->mode, sizeof(p->mode), "%s", config->srt_mode);
        }
        if (config->srt_passphrase[0] != '\0') {
            snprintf(p->passphrase, sizeof(p->passphrase), "%s", config->srt_passphrase);
        }
        if (config->srt_streamid[0] != '\0') {
            snprintf(p->streamid, sizeof(p->streamid), "%s", config->srt_streamid);
        }
    }

    if (!url || url[0] == '\0') {
        snprintf(p->host, sizeof(p->host), "0.0.0.0");
        p->is_listener = true;
        return;
    }

    char url_buf[1024];
    snprintf(url_buf, sizeof(url_buf), "%s", url);

    char *start = url_buf;
    if (strncmp(start, "srt://", 6) == 0) {
        start += 6;
    }

    char *query = strchr(start, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (start[0] == '[') {
        char *close_bracket = strchr(start, ']');
        if (close_bracket) {
            *close_bracket = '\0';
            snprintf(p->host, sizeof(p->host), "%.*s", (int)(sizeof(p->host) - 1), start + 1);
            if (*(close_bracket + 1) == ':') {
                p->port = atoi(close_bracket + 2);
            }
        }
    } else {
        char *colon = strrchr(start, ':');
        if (colon) {
            *colon = '\0';
            snprintf(p->host, sizeof(p->host), "%.*s", (int)(sizeof(p->host) - 1), start);
            p->port = atoi(colon + 1);
        } else {
            snprintf(p->host, sizeof(p->host), "%.*s", (int)(sizeof(p->host) - 1), start);
        }
    }

    if (p->host[0] == '\0' || strcmp(p->host, "*") == 0) {
        snprintf(p->host, sizeof(p->host), "0.0.0.0");
    }
    if (p->port <= 0) p->port = 9000;

    if (query && query[0] != '\0') {
        char *token = strtok(query, "&");
        while (token) {
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                char *key = token;
                char *val = eq + 1;
                if (strcasecmp(key, "mode") == 0) {
                    strncpy(p->mode, val, sizeof(p->mode) - 1);
                } else if (strcasecmp(key, "latency") == 0) {
                    int lat = atoi(val);
                    if (lat > 100000) lat /= 1000;
                    if (lat > 0) p->latency_ms = lat;
                } else if (strcasecmp(key, "passphrase") == 0) {
                    strncpy(p->passphrase, val, sizeof(p->passphrase) - 1);
                } else if (strcasecmp(key, "streamid") == 0 || strcasecmp(key, "srt_streamid") == 0) {
                    strncpy(p->streamid, val, sizeof(p->streamid) - 1);
                } else if (strcasecmp(key, "pbkeylen") == 0) {
                    int k = atoi(val);
                    if (k == 16 || k == 24 || k == 32) p->pbkeylen = k;
                } else if (strcasecmp(key, "maxclients") == 0 || strcasecmp(key, "max_clients") == 0) {
                    int mc = atoi(val);
                    if (mc > 0 && mc <= SRT_MAX_CLIENTS) p->max_clients = mc;
                }
            }
            token = strtok(NULL, "&");
        }
    }

    if (strcasecmp(p->mode, "listener") == 0) {
        p->is_listener = true;
    } else if (strcasecmp(p->mode, "caller") == 0) {
        p->is_listener = false;
    } else {
        if (strcmp(p->host, "0.0.0.0") == 0 || strcmp(p->host, "::") == 0) {
            p->is_listener = true;
        } else {
            p->is_listener = false;
        }
    }
}

static void srt_custom_log_handler(void *opaque, int level, const char *file, int line, const char *area, const char *message) {
    (void)opaque;
    (void)file;
    (void)line;
    (void)area;
    if (!message) return;

    /* Filter out expected non-blocking notifications that libsrt reports at error level */
    if (strstr(message, "no pending connection") ||
        strstr(message, "Resource temporarily unavailable") ||
        strstr(message, "Operation not supported: Non-blocking")) {
        return;
    }

    /* Only forward genuine critical/error messages */
    if (level <= 3) { /* LOG_CRIT or LOG_ERR */
        LOG_WARN("[libsrt] %s", message);
    }
}

static void srt_init_subsystem(void) {
    static bool inited = false;
    if (!inited) {
        srt_startup();
        srt_setloghandler(NULL, srt_custom_log_handler);
        inited = true;
    }
}

int srt_extract_socket(AVIOContext *pb) {
    if (!pb || !pb->opaque) return -1;
    URLContextDummy *uc = (URLContextDummy *)pb->opaque;
    if (!uc->priv_data) return -1;
    SRTContextDummy *sc = (SRTContextDummy *)uc->priv_data;
    if (sc->fd <= 0) return -1;
    return sc->fd;
}

int srt_setup_ingress_options(AVDictionary **opts, const PipelineConfig *config) {
    if (!opts || !config) return -1;
    srt_init_subsystem();
    av_dict_set(opts, "transtype", "live", 0);
    av_dict_set(opts, "smoother", "live", 0);

    if (config->srt_latency_ms > 0) {
        char opt_str[32];
        snprintf(opt_str, sizeof(opt_str), "%d", config->srt_latency_ms * 1000);
        av_dict_set(opts, "latency", opt_str, 0);
    }
    if (config->srt_passphrase[0] != '\0') {
        av_dict_set(opts, "passphrase", config->srt_passphrase, 0);
    }
    if (config->srt_streamid[0] != '\0') {
        av_dict_set(opts, "streamid", config->srt_streamid, 0);
    }
    if (config->srt_mode[0] != '\0') {
        av_dict_set(opts, "mode", config->srt_mode, 0);
    }
    if (config->timeout_sec > 0) {
        char opt_str[32];
        snprintf(opt_str, sizeof(opt_str), "%d", config->timeout_sec * 1000000);
        av_dict_set(opts, "timeout", opt_str, 0);
    }
    return 0;
}

static int srt_listener_write_packet_cb(void *opaque, uint8_t *buf, int size) {
    SrtListenerContext *ctx = (SrtListenerContext *)opaque;
    if (!ctx || size <= 0) return size;

    pthread_mutex_lock(&ctx->lock);

    /* Cache initial headers up to 8KB (PAT, PMT, SDT) for late-joining clients */
    if (ctx->cached_header_size < 8192) {
        int to_copy = size;
        if (ctx->cached_header_size + to_copy > ctx->cached_header_capacity) {
            int new_cap = ctx->cached_header_capacity == 0 ? 8192 : ctx->cached_header_capacity * 2;
            uint8_t *new_buf = (uint8_t *)realloc(ctx->cached_header, new_cap);
            if (new_buf) {
                ctx->cached_header = new_buf;
                ctx->cached_header_capacity = new_cap;
            }
        }
        if (ctx->cached_header && ctx->cached_header_size + to_copy <= ctx->cached_header_capacity) {
            memcpy(ctx->cached_header + ctx->cached_header_size, buf, to_copy);
            ctx->cached_header_size += to_copy;
        }
    }

    /* Send data to all active clients in chunks <= 1316 bytes (SRT Live MTU payload) */
    for (int i = 0; i < ctx->max_clients; i++) {
        if (!ctx->clients[i].active || ctx->clients[i].sock == SRT_INVALID_SOCK) continue;

        int offset = 0;
        bool client_failed = false;
        while (offset < size) {
            int chunk = size - offset;
            if (chunk > SRT_TS_PKT_SIZE) chunk = SRT_TS_PKT_SIZE;

            int sent = srt_send(ctx->clients[i].sock, (const char *)buf + offset, chunk);
            if (sent < 0) {
                int err = srt_getlasterror(NULL);
                if (err == SRT_EASYNCSND) {
                    /* Non-blocking socket send buffer full: skip remainder for this client to prevent blocking others */
                    break;
                }
                SRT_SOCKSTATUS st = srt_getsockstate(ctx->clients[i].sock);
                if (st != SRTS_CONNECTED || err == SRT_ECONNLOST || err == SRT_ENOCONN ||
                    err == SRT_ESOCKFAIL || err == SRT_ESCLOSED || err == SRT_ECONNFAIL || err == SRT_EINVSOCK) {
                    client_failed = true;
                    break;
                }
            } else {
                ctx->clients[i].bytes_sent += sent;
                ctx->clients[i].packets_sent++;
            }
            offset += chunk;
        }

        if (client_failed) {
            LOG_INFO("[SRT Listener :%d] Client #%d (%s:%d) disconnected.",
                     ctx->port, i + 1, ctx->clients[i].ip, ctx->clients[i].port);
            srt_close(ctx->clients[i].sock);
            ctx->clients[i].active = false;
            ctx->clients[i].sock = SRT_INVALID_SOCK;
            ctx->client_count--;
            if (ctx->oc) {
                ctx->oc->srt_client_count = ctx->client_count;
            }
        }
    }

    pthread_mutex_unlock(&ctx->lock);

    /* Always return full size so FFmpeg's av_interleaved_write_frame never fails when clients drop */
    return size;
}

static void *srt_listener_accept_loop(void *arg) {
    SrtListenerContext *ctx = (SrtListenerContext *)arg;
    if (!ctx) return NULL;

    int eid = srt_epoll_create();
    if (eid < 0) {
        LOG_ERROR("[SRT Listener :%d] Failed to create SRT epoll: %s",
                  ctx->port, srt_getlasterror_str());
        return NULL;
    }

    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (srt_epoll_add_usock(eid, ctx->listen_sock, &events) < 0) {
        LOG_ERROR("[SRT Listener :%d] Failed to add listen socket to SRT epoll: %s",
                  ctx->port, srt_getlasterror_str());
        srt_epoll_release(eid);
        return NULL;
    }

    while (ctx->running) {
        SRT_EPOLL_EVENT ready_events[4];
        int num_ready = srt_epoll_uwait(eid, ready_events, 4, 150); /* 150ms timeout */
        if (!ctx->running) break;

        if (num_ready < 0) {
            int err = srt_getlasterror(NULL);
            if (err == SRT_ETIMEOUT) {
                continue;
            }
            if (!ctx->running) break;
            usleep(20000);
            continue;
        }

        if (num_ready == 0) {
            continue; /* Timeout with no incoming connection pending */
        }

        for (int i = 0; i < num_ready; i++) {
            if (!ctx->running) break;
            if (ready_events[i].fd == ctx->listen_sock && (ready_events[i].events & SRT_EPOLL_IN)) {
                struct sockaddr_storage client_sa;
                int sa_len = sizeof(client_sa);
                SRTSOCKET client_sock = srt_accept(ctx->listen_sock, (struct sockaddr *)&client_sa, &sa_len);
                if (client_sock == SRT_INVALID_SOCK) {
                    continue;
                }

                /* Non-blocking send: if one client's buffer is slow/full, it won't block the pipeline or other clients! */
                int snd_syn = 0;
                srt_setsockopt(client_sock, 0, SRTO_SNDSYN, &snd_syn, sizeof(snd_syn));

                /* Set live mode and packet size */
                int transtype = SRTT_LIVE;
                srt_setsockopt(client_sock, 0, SRTO_TRANSTYPE, &transtype, sizeof(transtype));
                int plsize = SRT_TS_PKT_SIZE;
                srt_setsockopt(client_sock, 0, SRTO_PAYLOADSIZE, &plsize, sizeof(plsize));

                /* Extract peer IP address and port */
                char ip_str[64] = "unknown";
                int port = 0;
                if (client_sa.ss_family == AF_INET) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)&client_sa;
                    inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                    port = ntohs(sin->sin_port);
                } else if (client_sa.ss_family == AF_INET6) {
                    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&client_sa;
                    inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
                    port = ntohs(sin6->sin6_port);
                }

                pthread_mutex_lock(&ctx->lock);

                int slot = -1;
                for (int c = 0; c < ctx->max_clients; c++) {
                    if (!ctx->clients[c].active) {
                        slot = c;
                        break;
                    }
                }

                if (slot >= 0) {
                    ctx->clients[slot].sock = client_sock;
                    snprintf(ctx->clients[slot].ip, sizeof(ctx->clients[slot].ip), "%s", ip_str);
                    ctx->clients[slot].port = port;
                    ctx->clients[slot].connect_time_us = get_time_us();
                    ctx->clients[slot].bytes_sent = 0;
                    ctx->clients[slot].packets_sent = 0;
                    memset(&ctx->clients[slot].bstats, 0, sizeof(ctx->clients[slot].bstats));
                    ctx->clients[slot].active = true;
                    ctx->client_count++;
                    if (ctx->oc) {
                        ctx->oc->srt_client_count = ctx->client_count;
                    }

                    LOG_INFO("[SRT Listener :%d] Client #%d connected from %s:%d (Total active clients: %d)",
                             ctx->port, slot + 1, ip_str, port, ctx->client_count);

                    /* Burst initial cached PAT/PMT/SDT headers so client player decodes immediately! */
                    if (ctx->cached_header && ctx->cached_header_size > 0) {
                        int sent = srt_send(client_sock, (const char *)ctx->cached_header, ctx->cached_header_size);
                        if (sent > 0) {
                            ctx->clients[slot].bytes_sent += sent;
                        }
                    }
                } else {
                    LOG_WARN("[SRT Listener :%d] Rejected connection from %s:%d: Maximum clients reached (%d)",
                             ctx->port, ip_str, port, ctx->max_clients);
                    srt_close(client_sock);
                }

                pthread_mutex_unlock(&ctx->lock);
            }
        }
    }

    srt_epoll_release(eid);
    return NULL;
}

int srt_open_egress(OutputContext *oc, const PipelineConfig *config, int64_t *last_packet_time_us) {
    if (!oc || !config) return -1;
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};

    srt_init_subsystem();

    SrtUrlParams params;
    srt_parse_url_params(oc->url, config, &params);

    if (params.is_listener) {
        /* Multi-Client SRT Listener Server */
        int64_t conn_start_us = get_time_us();

        SrtListenerContext *ctx = (SrtListenerContext *)calloc(1, sizeof(SrtListenerContext));
        if (!ctx) {
            LOG_ERROR("[SRT #%d] Failed to allocate SrtListenerContext", oc->type_index);
            return AVERROR(ENOMEM);
        }
        ctx->port = params.port;
        snprintf(ctx->bind_ip, sizeof(ctx->bind_ip), "%s", params.host);
        ctx->max_clients = params.max_clients;
        ctx->oc = oc;
        ctx->running = true;
        pthread_mutex_init(&ctx->lock, NULL);

        for (int i = 0; i < SRT_MAX_CLIENTS; i++) {
            ctx->clients[i].sock = SRT_INVALID_SOCK;
            ctx->clients[i].active = false;
        }

        ctx->listen_sock = srt_create_socket();
        if (ctx->listen_sock == SRT_INVALID_SOCK) {
            LOG_ERROR("[SRT #%d] Failed to create SRT socket: %s", oc->type_index, srt_getlasterror_str());
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return -1;
        }

        int transtype = SRTT_LIVE;
        srt_setsockopt(ctx->listen_sock, 0, SRTO_TRANSTYPE, &transtype, sizeof(transtype));

        int latency = params.latency_ms;
        srt_setsockopt(ctx->listen_sock, 0, SRTO_LATENCY, &latency, sizeof(latency));
        srt_setsockopt(ctx->listen_sock, 0, SRTO_PEERLATENCY, &latency, sizeof(latency));
        srt_setsockopt(ctx->listen_sock, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));

        int plsize = SRT_TS_PKT_SIZE;
        srt_setsockopt(ctx->listen_sock, 0, SRTO_PAYLOADSIZE, &plsize, sizeof(plsize));

        if (params.passphrase[0] != '\0') {
            srt_setsockopt(ctx->listen_sock, 0, SRTO_PASSPHRASE, params.passphrase, strlen(params.passphrase));
            int pbkeylen = params.pbkeylen;
            srt_setsockopt(ctx->listen_sock, 0, SRTO_PBKEYLEN, &pbkeylen, sizeof(pbkeylen));
        }

        if (params.streamid[0] != '\0') {
            srt_setsockopt(ctx->listen_sock, 0, SRTO_STREAMID, params.streamid, strlen(params.streamid));
        }

        /* Non-blocking accept */
        int syn = 0;
        srt_setsockopt(ctx->listen_sock, 0, SRTO_RCVSYN, &syn, sizeof(syn));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(params.port);
        if (inet_pton(AF_INET, params.host, &sa.sin_addr) <= 0) {
            sa.sin_addr.s_addr = INADDR_ANY;
        }

        if (srt_bind(ctx->listen_sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            LOG_ERROR("[SRT #%d] Failed to bind SRT listener to %s:%d: %s",
                      oc->type_index, params.host, params.port, srt_getlasterror_str());
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return -1;
        }

        if (srt_listen(ctx->listen_sock, 16) == SRT_ERROR) {
            LOG_ERROR("[SRT #%d] Failed to listen on SRT socket %s:%d: %s",
                      oc->type_index, params.host, params.port, srt_getlasterror_str());
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return -1;
        }

        /* Start background client accept thread */
        if (pthread_create(&ctx->accept_thread, NULL, srt_listener_accept_loop, ctx) != 0) {
            LOG_ERROR("[SRT #%d] Failed to start SRT accept thread", oc->type_index);
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return -1;
        }

        /* Configure Custom AVIO for MPEG-TS multiplexer */
        uint8_t *avio_buf = (uint8_t *)av_malloc(SRT_AVIO_BUF_SIZE);
        if (!avio_buf) {
            LOG_ERROR("[SRT #%d] Failed to allocate AVIO buffer", oc->type_index);
            ctx->running = false;
            pthread_join(ctx->accept_thread, NULL);
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return AVERROR(ENOMEM);
        }

        AVIOContext *pb = avio_alloc_context(avio_buf, SRT_AVIO_BUF_SIZE, 1, ctx,
                                             NULL, srt_listener_write_packet_cb, NULL);
        if (!pb) {
            av_free(avio_buf);
            LOG_ERROR("[SRT #%d] Failed to allocate AVIOContext", oc->type_index);
            ctx->running = false;
            pthread_join(ctx->accept_thread, NULL);
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return AVERROR(ENOMEM);
        }

        oc->ofmt_ctx->pb = pb;
        oc->ofmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

        /* Write MPEG-TS stream headers (PAT/PMT/SDT tables) */
        AVDictionary *ts_opts = NULL;
        av_dict_set(&ts_opts, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
        av_dict_set(&ts_opts, "pcr_period", "20", 0);
        av_dict_set(&ts_opts, "pat_period", "0.1", 0); /* 100ms refresh for late-joining players */
        av_dict_set(&ts_opts, "sdt_period", "0.25", 0);
        int ret = avformat_write_header(oc->ofmt_ctx, &ts_opts);
        av_dict_free(&ts_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[SRT #%d] SRT Stream Header rejected: %s", oc->type_index, err_buf);
            ctx->running = false;
            pthread_join(ctx->accept_thread, NULL);
            srt_close(ctx->listen_sock);
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return ret;
        }

        oc->is_srt_listener = true;
        oc->srt_listener = ctx;
        oc->srt_sock = ctx->listen_sock;
        oc->srt_client_count = 0;
        oc->srt_arq_success_pct = 100.0;
        oc->conn_time_ms = (get_time_us() - conn_start_us) / 1000;
        if (last_packet_time_us) {
            *last_packet_time_us = av_gettime_relative();
        }

        LOG_INFO("[SRT #%d] Multi-client SRT Listener server active: %s (Listening on %s:%d, Max clients: %d)",
                 oc->type_index, oc->url, params.host, params.port, ctx->max_clients);
        return 0;
    }

    /* Standard SRT Caller Mode (Point-to-Point) */
    oc->is_srt_listener = false;
    oc->srt_listener = NULL;
    oc->srt_client_count = 1;

    AVDictionary *srt_opts = NULL;
    av_dict_set(&srt_opts, "transtype", "live", 0);
    av_dict_set(&srt_opts, "pkt_size", "1316", 0);

    if (params.latency_ms > 0) {
        char lat_str[32];
        snprintf(lat_str, sizeof(lat_str), "%d", params.latency_ms * 1000);
        av_dict_set(&srt_opts, "latency", lat_str, 0);
    }
    if (params.passphrase[0] != '\0') {
        av_dict_set(&srt_opts, "passphrase", params.passphrase, 0);
    }
    if (params.streamid[0] != '\0') {
        av_dict_set(&srt_opts, "streamid", params.streamid, 0);
    }
    av_dict_set(&srt_opts, "mode", "caller", 0);

    int64_t conn_start_us = get_time_us();
    if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        LOG_INFO("[SRT #%d] Initiating SRT caller egress: %s ...", oc->type_index, oc->url);
        if (last_packet_time_us) {
            *last_packet_time_us = av_gettime_relative();
        }
        int ret = avio_open2(&oc->ofmt_ctx->pb, oc->url, AVIO_FLAG_WRITE,
                             &oc->ofmt_ctx->interrupt_callback, &srt_opts);
        if (ret < 0) {
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("[SRT #%d] Connection/Open Failed to '%s': %s", oc->type_index, oc->url, err_buf);
            av_dict_free(&srt_opts);
            return ret;
        }
    }
    oc->conn_time_ms = (get_time_us() - conn_start_us) / 1000;
    LOG_INFO("[SRT #%d] Connected to remote SRT receiver in %lld ms.",
             oc->type_index, (long long)oc->conn_time_ms);

    oc->srt_sock = srt_extract_socket(oc->ofmt_ctx->pb);
    if (oc->srt_sock > 0) {
        LOG_INFO("[SRT #%d] Extracted SRT socket fd=%d for QoS telemetry.",
                 oc->type_index, oc->srt_sock);
    }

    LOG_INFO("[SRT #%d] Writing MPEG-TS headers & stream metadata...", oc->type_index);
    int ret = avformat_write_header(oc->ofmt_ctx, &srt_opts);
    av_dict_free(&srt_opts);
    if (ret < 0) {
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("[SRT #%d] SRT Stream Header rejected: %s", oc->type_index, err_buf);
        return ret;
    }
    if (oc->srt_sock <= 0) {
        oc->srt_sock = srt_extract_socket(oc->ofmt_ctx->pb);
    }
    oc->srt_arq_success_pct = 100.0;
    LOG_INFO("[SRT #%d] SRT caller publishing active. Target: %s", oc->type_index, oc->url);
    return 0;
}

void srt_update_ingress_stats(int srt_sock, PipelineStats *stats) {
    if (!stats || srt_sock <= 0) return;
    stats->srt_in_sock = srt_sock;
    SRT_TRACEBSTATS in_bstats;
    if (srt_bstats((SRTSOCKET)srt_sock, &in_bstats, 1) == 0) {
        stats->srt_in_rtt_ms = in_bstats.msRTT;
        stats->srt_in_nack_interval = in_bstats.pktSentNAK;
        stats->srt_in_nack_total = in_bstats.pktSentNAKTotal;
        stats->srt_in_loss_interval = in_bstats.pktRcvLoss;
        stats->srt_in_loss_total = in_bstats.pktRcvLossTotal;
        stats->srt_in_retrans_interval = in_bstats.pktRcvRetrans;
        stats->srt_in_retrans_total += in_bstats.pktRcvRetrans;
        stats->srt_in_drop_interval = in_bstats.pktRcvDrop;
        stats->srt_in_drop_total = in_bstats.pktRcvDropTotal;
        stats->srt_in_rcv_rate_mbps = in_bstats.mbpsRecvRate;
        stats->srt_in_rcv_buf_ms = in_bstats.msRcvBuf;

        int retrans_eval = stats->srt_in_retrans_total;
        int drop_eval = in_bstats.pktRcvDropTotal;
        if (retrans_eval + drop_eval > 0) {
            double rate = 100.0 * (1.0 - (double)drop_eval / (double)(retrans_eval + drop_eval));
            if (rate < 0.0) rate = 0.0;
            if (rate > 100.0) rate = 100.0;
            stats->srt_in_arq_success_pct = rate;
        } else {
            stats->srt_in_arq_success_pct = 100.0;
        }
    }
}

void srt_update_egress_stats(OutputContext *oc) {
    if (!oc) return;

    if (oc->is_srt_listener && oc->srt_listener) {
        SrtListenerContext *ctx = (SrtListenerContext *)oc->srt_listener;
        pthread_mutex_lock(&ctx->lock);

        oc->srt_client_count = ctx->client_count;

        if (ctx->client_count == 0) {
            oc->srt_rtt_ms = 0.0;
            oc->srt_nack_interval = 0;
            oc->srt_loss_interval = 0;
            oc->srt_retrans_interval = 0;
            oc->srt_drop_interval = 0;
            oc->srt_mbps_bandwidth = 0.0;
            oc->srt_arq_success_pct = 100.0;
            oc->srt_snd_buf_ms = 0;
            pthread_mutex_unlock(&ctx->lock);
            return;
        }

        double total_rtt = 0.0;
        int total_nack_interval = 0;
        int total_nack_total = 0;
        int total_loss_interval = 0;
        int total_loss_total = 0;
        int total_retrans_interval = 0;
        int total_retrans_total = 0;
        int total_drop_interval = 0;
        int total_drop_total = 0;
        double total_bandwidth = 0.0;
        int max_snd_buf = 0;
        int active_evaluated = 0;

        for (int i = 0; i < ctx->max_clients; i++) {
            if (!ctx->clients[i].active || ctx->clients[i].sock == SRT_INVALID_SOCK) continue;

            SRT_TRACEBSTATS bstats;
            if (srt_bstats(ctx->clients[i].sock, &bstats, 1) == 0) {
                ctx->clients[i].bstats = bstats;
                total_rtt += bstats.msRTT;
                total_nack_interval += bstats.pktRecvNAK;
                total_nack_total += bstats.pktRecvNAKTotal;
                total_loss_interval += bstats.pktSndLoss;
                total_loss_total += bstats.pktSndLossTotal;
                total_retrans_interval += bstats.pktRetrans;
                total_retrans_total += bstats.pktRetransTotal;
                total_drop_interval += bstats.pktSndDrop;
                total_drop_total += bstats.pktSndDropTotal;
                total_bandwidth += bstats.mbpsBandwidth;
                if (bstats.msSndBuf > max_snd_buf) max_snd_buf = bstats.msSndBuf;
                active_evaluated++;
            }
        }

        if (active_evaluated > 0) {
            oc->srt_rtt_ms = total_rtt / active_evaluated;
            oc->srt_nack_interval = total_nack_interval;
            oc->srt_nack_total = total_nack_total;
            oc->srt_loss_interval = total_loss_interval;
            oc->srt_loss_total = total_loss_total;
            oc->srt_retrans_interval = total_retrans_interval;
            oc->srt_retrans_total = total_retrans_total;
            oc->srt_drop_interval = total_drop_interval;
            oc->srt_drop_total = total_drop_total;
            oc->srt_mbps_bandwidth = total_bandwidth;
            oc->srt_snd_buf_ms = max_snd_buf;

            if (total_retrans_total + total_drop_total > 0) {
                double rate = 100.0 * (1.0 - (double)total_drop_total / (double)(total_retrans_total + total_drop_total));
                if (rate < 0.0) rate = 0.0;
                if (rate > 100.0) rate = 100.0;
                oc->srt_arq_success_pct = rate;
            } else {
                oc->srt_arq_success_pct = 100.0;
            }
        }

        pthread_mutex_unlock(&ctx->lock);
        return;
    }

    /* Caller mode stats */
    if (oc->srt_sock <= 0 && oc->ofmt_ctx && oc->ofmt_ctx->pb) {
        oc->srt_sock = srt_extract_socket(oc->ofmt_ctx->pb);
    }
    if (oc->srt_sock > 0) {
        SRT_TRACEBSTATS bstats;
        if (srt_bstats((SRTSOCKET)oc->srt_sock, &bstats, 1) == 0) {
            oc->srt_rtt_ms = bstats.msRTT;
            oc->srt_nack_interval = bstats.pktRecvNAK;
            oc->srt_nack_total = bstats.pktRecvNAKTotal;
            oc->srt_loss_interval = bstats.pktSndLoss;
            oc->srt_loss_total = bstats.pktSndLossTotal;
            oc->srt_retrans_interval = bstats.pktRetrans;
            oc->srt_retrans_total = bstats.pktRetransTotal;
            oc->srt_drop_interval = bstats.pktSndDrop;
            oc->srt_drop_total = bstats.pktSndDropTotal;
            oc->srt_mbps_bandwidth = bstats.mbpsBandwidth;
            oc->srt_snd_buf_ms = bstats.msSndBuf;

            int retrans_eval = bstats.pktRetransTotal;
            int drop_eval = bstats.pktSndDropTotal;
            if (retrans_eval + drop_eval > 0) {
                double rate = 100.0 * (1.0 - (double)drop_eval / (double)(retrans_eval + drop_eval));
                if (rate < 0.0) rate = 0.0;
                if (rate > 100.0) rate = 100.0;
                oc->srt_arq_success_pct = rate;
            } else {
                oc->srt_arq_success_pct = 100.0;
            }
        }
    }
}

void srt_close_egress(OutputContext *oc) {
    if (!oc || !oc->ofmt_ctx) return;

    if (oc->is_srt_listener && oc->srt_listener) {
        SrtListenerContext *ctx = (SrtListenerContext *)oc->srt_listener;
        ctx->running = false;

        /* Stop listening socket to unblock any pending accept */
        if (ctx->listen_sock != SRT_INVALID_SOCK) {
            srt_close(ctx->listen_sock);
            ctx->listen_sock = SRT_INVALID_SOCK;
        }

        /* Wait for accept thread to exit cleanly */
        if (ctx->accept_thread) {
            pthread_join(ctx->accept_thread, NULL);
            ctx->accept_thread = 0;
        }

        /* Write trailer to active clients */
        av_write_trailer(oc->ofmt_ctx);

        pthread_mutex_lock(&ctx->lock);
        for (int i = 0; i < ctx->max_clients; i++) {
            if (ctx->clients[i].active && ctx->clients[i].sock != SRT_INVALID_SOCK) {
                srt_close(ctx->clients[i].sock);
                ctx->clients[i].active = false;
                ctx->clients[i].sock = SRT_INVALID_SOCK;
            }
        }
        ctx->client_count = 0;
        if (ctx->cached_header) {
            free(ctx->cached_header);
            ctx->cached_header = NULL;
            ctx->cached_header_size = 0;
        }
        pthread_mutex_unlock(&ctx->lock);
        pthread_mutex_destroy(&ctx->lock);

        /* Free custom AVIO buffer and context */
        if (oc->ofmt_ctx->pb) {
            if (oc->ofmt_ctx->pb->buffer) {
                av_free(oc->ofmt_ctx->pb->buffer);
                oc->ofmt_ctx->pb->buffer = NULL;
            }
            avio_context_free(&oc->ofmt_ctx->pb);
            oc->ofmt_ctx->pb = NULL;
        }

        free(ctx);
        oc->srt_listener = NULL;
        oc->is_srt_listener = false;
        LOG_INFO("[SRT #%d] Multi-client SRT Listener stopped.", oc->type_index);
    } else {
        /* Standard Caller mode */
        av_write_trailer(oc->ofmt_ctx);
        if (!(oc->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&oc->ofmt_ctx->pb);
        }
        LOG_INFO("[SRT #%d] SRT caller egress closed.", oc->type_index);
    }
}
