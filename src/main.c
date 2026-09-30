#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void on_signal(int sig) {
    (void)sig;
    LOG_WARN("\nReceived interrupt signal. Initiating graceful shutdown...");
    pipeline_request_stop();
}

static void print_usage(const char *prog_name) {
    printf("========================================================================\n");
    printf("   FFmpeg Multicast to RTMP Zero-Encoding Streaming Pipeline\n");
    printf("========================================================================\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -i, --input <url>          Multicast UDP input URL\n");
    printf("                             (Default: udp://@239.255.0.1:1234)\n");
    printf("  -o, --output <url>         RTMP output URL\n");
    printf("                             (Default: rtmp://127.0.0.1:1935/live/stream)\n");
    printf("  -a, --local-addr <ip>      Local network interface IP for multicast bind\n");
    printf("                             (e.g., 192.168.1.100)\n");
    printf("  -f, --fifo-size <mb>       Internal UDP FIFO queue size in MB (Default: 50)\n");
    printf("  -b, --buffer-size <mb>     Kernel UDP socket receive buffer in MB (Default: 16)\n");
    printf("  -t, --timeout <sec>        Network read timeout in seconds (Default: 10)\n");
    printf("  -k, --no-wait-keyframe     Start immediately without waiting for first keyframe\n");
    printf("  -n, --no-normalize-pts     Do not offset PTS/DTS to start at 0\n");
    printf("  -s, --stats-interval <ms>  Interval for live telemetry in ms (Default: 1000)\n");
    printf("  -l, --log-stats            Print telemetry as standard timestamped LOG lines\n");
    printf("  -d, --detailed             Display detailed multi-line telemetry cards each second\n");
    printf("  -j, --json                 Output telemetry as 1-second JSON objects (stdout)\n");
    printf("  -v, --verbose              Enable debug logging\n");
    printf("  -h, --help                 Display this help message\n\n");
    printf("Examples:\n");
    printf("  %s -i udp://@239.255.0.1:1234 -o rtmp://127.0.0.1:1935/live/stream\n", prog_name);
    printf("  %s -i udp://@239.255.0.1:1234 -o rtmp://a.rtmp.youtube.com/live2/XXXX --log-stats\n", prog_name);
    printf("  %s -i udp://@239.255.0.1:1234 -o rtmp://127.0.0.1:1935/live/stream --detailed\n", prog_name);
    printf("========================================================================\n");
}

int main(int argc, char **argv) {
    PipelineConfig config;
    pipeline_config_init_defaults(&config);

    static struct option long_options[] = {
        {"input",             required_argument, 0, 'i'},
        {"output",            required_argument, 0, 'o'},
        {"local-addr",        required_argument, 0, 'a'},
        {"fifo-size",         required_argument, 0, 'f'},
        {"buffer-size",       required_argument, 0, 'b'},
        {"timeout",           required_argument, 0, 't'},
        {"no-wait-keyframe",  no_argument,       0, 'k'},
        {"no-normalize-pts",  no_argument,       0, 'n'},
        {"stats-interval",    required_argument, 0, 's'},
        {"log-stats",         no_argument,       0, 'l'},
        {"detailed",          no_argument,       0, 'd'},
        {"json",              no_argument,       0, 'j'},
        {"verbose",           no_argument,       0, 'v'},
        {"help",              no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:a:f:b:t:knsl:djvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                strncpy(config.input_url, optarg, sizeof(config.input_url) - 1);
                break;
            case 'o':
                strncpy(config.output_url, optarg, sizeof(config.output_url) - 1);
                break;
            case 'a':
                strncpy(config.local_addr, optarg, sizeof(config.local_addr) - 1);
                break;
            case 'f':
                config.fifo_size = atoi(optarg) * 1024 * 1024;
                if (config.fifo_size <= 0) config.fifo_size = 50 * 1024 * 1024;
                break;
            case 'b':
                config.buffer_size = atoi(optarg) * 1024 * 1024;
                if (config.buffer_size <= 0) config.buffer_size = 16 * 1024 * 1024;
                break;
            case 't':
                config.timeout_sec = atoi(optarg);
                break;
            case 'k':
                config.wait_keyframe = false;
                break;
            case 'n':
                config.normalize_pts = false;
                break;
            case 's':
                config.stats_interval_ms = atoi(optarg);
                if (config.stats_interval_ms < 100) config.stats_interval_ms = 100;
                break;
            case 'l':
                config.log_telemetry = true;
                break;
            case 'd':
                config.detailed_telemetry = true;
                break;
            case 'j':
                config.json_telemetry = true;
                break;
            case 'v':
                log_set_level(LOG_LVL_DEBUG);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!config.json_telemetry) {
        /* Print startup banner */
        printf("\n\033[1;36m========================================================================\033[0m\n");
        printf("\033[1;36m   Multicast -> RTMP Remuxing Pipeline (Zero-Encoding)\033[0m\n");
        printf("\033[1;36m========================================================================\033[0m\n");
        printf("  Input Multicast URL  : %s\n", config.input_url);
        printf("  Output RTMP URL      : %s\n", config.output_url);
        if (strlen(config.local_addr) > 0) {
            printf("  Multicast Bind IP    : %s\n", config.local_addr);
        }
        printf("  UDP FIFO Size        : %d MB\n", config.fifo_size / (1024 * 1024));
        printf("  Socket Buffer Size   : %d MB\n", config.buffer_size / (1024 * 1024));
        printf("  Read Timeout         : %d seconds\n", config.timeout_sec);
        printf("  Wait First Keyframe  : %s\n", config.wait_keyframe ? "YES (clean start)" : "NO");
        printf("  Normalize Timestamps : %s\n", config.normalize_pts ? "YES (starts at 0.0s)" : "NO");
        printf("  Health Diagnostics   : %s (Buffering & GOP monitor)\n",
               config.enable_diagnostics ? "ACTIVE" : "DISABLED");
        printf("  Telemetry Mode       : %s\n",
               config.log_telemetry ? "Standard Timestamped Console Logs" :
               (config.detailed_telemetry ? "Detailed Multi-line Cards" : "Dynamic Single-line"));
        printf("  Encoding Mode        : PASSTHROUGH / REMUX (0%% CPU Transcode Overhead)\n");
        printf("\033[1;36m========================================================================\033[0m\n\n");
    }

    setup_signal_handlers(on_signal);

    int ret = pipeline_run(&config);
    return (ret == 0) ? 0 : 1;
}
