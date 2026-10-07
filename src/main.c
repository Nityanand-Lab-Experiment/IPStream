#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "utils.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void on_signal(int sig) {
  (void)sig;
  LOG_WARN("\nReceived interrupt signal. Initiating graceful shutdown...");
  pipeline_request_stop();
}

static void add_output(PipelineConfig *config, OutputType type, const char *url) {
  if (!config || !url || url[0] == '\0') return;
  if (config->num_outputs >= MAX_OUTPUTS) {
    LOG_WARN("Maximum number of outputs (%d) reached. Ignoring '%s'", MAX_OUTPUTS, url);
    return;
  }
  config->outputs[config->num_outputs].type = type;
  strncpy(config->outputs[config->num_outputs].url, url, sizeof(config->outputs[config->num_outputs].url) - 1);
  config->outputs[config->num_outputs].url[sizeof(config->outputs[config->num_outputs].url) - 1] = '\0';
  config->num_outputs++;
}

static void print_version(void) {
  printf("%s version %s\n", PIPELINE_NAME, PIPELINE_VERSION);
}

static void print_usage(const char *prog_name) {
  printf("========================================================================\n");
  printf("   %s %s - Multicast to RTMP / HLS Zero-Encoding Streaming Pipeline\n", PIPELINE_NAME, PIPELINE_VERSION);
  printf("========================================================================\n\n");
  printf("Usage: %s [OPTIONS]\n\n", prog_name);
  printf("Options:\n");
  printf("  -i, --input <url>          Multicast UDP input URL\n");
  printf("                             (Default: udp://@239.255.0.1:1234)\n");
  printf("  -o, --output <type> <url>  Add egress target. Type: 'rtmp' or 'hls'.\n");
  printf("                             Can be repeated for multiple concurrent outputs.\n");
  printf("                             Examples:\n");
  printf("                               -o rtmp rtmp://127.0.0.1:1935/live/stream\n");
  printf("                               -o hls http://origin.cdn.com/live/stream.m3u8\n");
  printf("                               -o rtmp rtmp://... -o hls http://...\n");
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
  printf("  -V, --version              Display version information\n");
  printf("  -h, --help                 Display this help message\n\n");
  printf("HLS Options:\n");
  printf("  --hls <url>                HLS destination playlist URL or path (alias for -o hls <url>)\n");
  printf("  --hls-method <method>      HTTP method for HLS upload (Default: PUT, or POST)\n");
  printf("  --hls-time <sec>           Target segment duration in seconds (Default: 2)\n");
  printf("  --hls-list-size <num>      Max segments kept in live playlist (Default: 5)\n");
  printf("  --hls-segment <pattern>    Custom segment pattern (e.g. http://origin/live/stream_%%05d.ts)\n");
  printf("  --hls-fmp4                 Use fragmented MP4 (fMP4) chunks instead of MPEG-TS\n");
  printf("  --hls-no-delete            Do not delete expired segments from disk\n\n");
  printf("Confidence Monitor Options:\n");
  printf("  --monitor-file <path>      Output path for JPEG confidence monitor (e.g. /tmp/monitor.jpg)\n");
  printf("  --monitor-fps <num>        Capture rate in FPS (Default: 5)\n");
  printf("  --monitor-scale <WxH>      Scale snapshot resolution (e.g. 640x360, default: native)\n");
  printf("  --monitor-quality <1-31>   JPEG quality (Default: 4, 1=highest)\n\n");
  printf("Examples:\n");
  printf("  # Option 1: Only RTMP\n");
  printf("  %s -i udp://@239.255.0.1:1234 -o rtmp rtmp://127.0.0.1:1935/live/stream\n\n", prog_name);
  printf("  # Option 2: Only HLS\n");
  printf("  %s -i udp://@239.255.0.1:1234 -o hls http://origin.cdn.com/live/stream.m3u8\n\n", prog_name);
  printf("  # Option 3: Dual Output (RTMP + HLS)\n");
  printf("  %s -i udp://@239.255.0.1:1234 -o rtmp rtmp://127.0.0.1:1935/live/stream -o hls http://origin/live.m3u8\n\n", prog_name);
  printf("  # Multiple Outputs (2 RTMP + 2 HLS)\n");
  printf("  %s -i udp://@239.255.0.1:1234 -o rtmp rtmp://yt1/live/key -o rtmp rtmp://yt2/live/key -o hls http://cdn1/s.m3u8 -o hls /var/www/s.m3u8\n", prog_name);
  printf("========================================================================\n");
}

int main(int argc, char **argv) {
  PipelineConfig config;
  pipeline_config_init_defaults(&config);

  static struct option long_options[] = {
      {"input", required_argument, 0, 'i'},
      {"output", required_argument, 0, 'o'},
      {"local-addr", required_argument, 0, 'a'},
      {"fifo-size", required_argument, 0, 'f'},
      {"buffer-size", required_argument, 0, 'b'},
      {"timeout", required_argument, 0, 't'},
      {"no-wait-keyframe", no_argument, 0, 'k'},
      {"no-normalize-pts", no_argument, 0, 'n'},
      {"stats-interval", required_argument, 0, 's'},
      {"log-stats", no_argument, 0, 'l'},
      {"detailed", no_argument, 0, 'd'},
      {"json", no_argument, 0, 'j'},
      {"verbose", no_argument, 0, 'v'},
      {"version", no_argument, 0, 'V'},
      {"help", no_argument, 0, 'h'},
      {"hls", required_argument, 0, 1001},
      {"hls-method", required_argument, 0, 1002},
      {"hls-time", required_argument, 0, 1003},
      {"hls-list-size", required_argument, 0, 1004},
      {"hls-segment", required_argument, 0, 1005},
      {"hls-fmp4", no_argument, 0, 1006},
      {"hls-no-delete", no_argument, 0, 1007},
      {"monitor-file", required_argument, 0, 1010},
      {"monitor-fps", required_argument, 0, 1011},
      {"monitor-scale", required_argument, 0, 1012},
      {"monitor-quality", required_argument, 0, 1013},
      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "i:o:a:f:b:t:knsl:djvhV", long_options,
                            NULL)) != -1) {
    switch (opt) {
    case 'V':
      print_version();
      return 0;
    case 'i':
      strncpy(config.input_url, optarg, sizeof(config.input_url) - 1);
      break;
    case 'o': {
      OutputType type = OUTPUT_TYPE_RTMP;
      const char *url = NULL;

      if (strcasecmp(optarg, "rtmp") == 0) {
        type = OUTPUT_TYPE_RTMP;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing URL argument after '-o rtmp'");
          return 1;
        }
      } else if (strcasecmp(optarg, "hls") == 0) {
        type = OUTPUT_TYPE_HLS;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing URL argument after '-o hls'");
          return 1;
        }
      } else if (strncmp(optarg, "rtmp://", 7) == 0 || strncmp(optarg, "rtmps://", 8) == 0) {
        type = OUTPUT_TYPE_RTMP;
        url = optarg;
      } else if (strncmp(optarg, "http://", 7) == 0 || strncmp(optarg, "https://", 8) == 0 ||
                 (strlen(optarg) >= 5 && strcmp(optarg + strlen(optarg) - 5, ".m3u8") == 0)) {
        type = OUTPUT_TYPE_HLS;
        url = optarg;
      } else if (strncasecmp(optarg, "rtmp=", 5) == 0 || strncasecmp(optarg, "rtmp ", 5) == 0) {
        type = OUTPUT_TYPE_RTMP;
        url = optarg + 5;
        while (*url == ' ') url++;
      } else if (strncasecmp(optarg, "hls=", 4) == 0 || strncasecmp(optarg, "hls ", 4) == 0) {
        type = OUTPUT_TYPE_HLS;
        url = optarg + 4;
        while (*url == ' ') url++;
      } else {
        type = OUTPUT_TYPE_RTMP;
        url = optarg;
      }

      add_output(&config, type, url);
      break;
    }
    case 'a':
      strncpy(config.local_addr, optarg, sizeof(config.local_addr) - 1);
      break;
    case 'f':
      config.fifo_size = (atoi(optarg) * 1024 * 1024) / 188;
      if (config.fifo_size <= 0)
        config.fifo_size = (50 * 1024 * 1024) / 188;
      break;
    case 'b':
      config.buffer_size = atoi(optarg) * 1024 * 1024;
      if (config.buffer_size <= 0)
        config.buffer_size = 16 * 1024 * 1024;
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
      if (config.stats_interval_ms < 100)
        config.stats_interval_ms = 100;
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
    case 1001:
      add_output(&config, OUTPUT_TYPE_HLS, optarg);
      break;
    case 1002:
      strncpy(config.hls_method, optarg, sizeof(config.hls_method) - 1);
      break;
    case 1003:
      config.hls_time = atoi(optarg);
      if (config.hls_time <= 0)
        config.hls_time = 2;
      break;
    case 1004:
      config.hls_list_size = atoi(optarg);
      if (config.hls_list_size <= 0)
        config.hls_list_size = 5;
      break;
    case 1005:
      strncpy(config.hls_segment_filename, optarg,
              sizeof(config.hls_segment_filename) - 1);
      break;
    case 1006:
      config.hls_fmp4 = true;
      break;
    case 1007:
      config.hls_delete_segments = false;
      break;
    case 1010:
      strncpy(config.monitor_file, optarg, sizeof(config.monitor_file) - 1);
      break;
    case 1011:
      config.monitor_fps = atoi(optarg);
      if (config.monitor_fps <= 0)
        config.monitor_fps = 5;
      break;
    case 1012: {
      int w = 0, h = 0;
      if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        config.monitor_width = w;
        config.monitor_height = h;
      }
      break;
    }
    case 1013:
      config.monitor_quality = atoi(optarg);
      if (config.monitor_quality < 1 || config.monitor_quality > 31)
        config.monitor_quality = 4;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  /* Default to RTMP if no output was explicitly specified */
  if (config.num_outputs == 0) {
    if (config.output_url[0] != '\0') {
      add_output(&config, OUTPUT_TYPE_RTMP, config.output_url);
    } else {
      add_output(&config, OUTPUT_TYPE_RTMP, "rtmp://127.0.0.1:1935/live/stream");
    }
  }

  /* Sync legacy fallback fields */
  for (int i = 0; i < config.num_outputs; i++) {
    if (config.outputs[i].type == OUTPUT_TYPE_RTMP && config.output_url[0] == '\0') {
      strncpy(config.output_url, config.outputs[i].url, sizeof(config.output_url) - 1);
    }
    if (config.outputs[i].type == OUTPUT_TYPE_HLS && config.hls_url[0] == '\0') {
      strncpy(config.hls_url, config.outputs[i].url, sizeof(config.hls_url) - 1);
    }
  }

  if (!config.json_telemetry) {
    /* Print startup banner */
    printf("\n\033[1;36m========================================================================\033[0m\n");
    printf("\033[1;36m   %s %s - Multicast -> RTMP / HLS Remuxing (Zero-Encoding)\033[0m\n", PIPELINE_NAME, PIPELINE_VERSION);
    printf("\033[1;36m========================================================================\033[0m\n");
    printf("  Input Multicast URL  : %s\n", config.input_url);
    for (int i = 0; i < config.num_outputs; i++) {
      if (config.outputs[i].type == OUTPUT_TYPE_RTMP) {
        printf("  Output RTMP [%d]     : %s\n", i + 1, config.outputs[i].url);
      } else {
        printf("  Output HLS [%d]      : %s (Method: %s, %ds chunks, list size %d)\n",
               i + 1, config.outputs[i].url, config.hls_method, config.hls_time, config.hls_list_size);
      }
    }
    if (strlen(config.monitor_file) > 0) {
      if (config.monitor_width > 0 && config.monitor_height > 0) {
        printf("  Confidence Monitor   : %s (%d fps, %dx%d, Q:%d)\n",
               config.monitor_file, config.monitor_fps, config.monitor_width,
               config.monitor_height, config.monitor_quality);
      } else {
        printf("  Confidence Monitor   : %s (%d fps, native res, Q:%d)\n",
               config.monitor_file, config.monitor_fps, config.monitor_quality);
      }
    }
    if (strlen(config.local_addr) > 0) {
      printf("  Multicast Bind IP    : %s\n", config.local_addr);
    }
    printf("  UDP FIFO Size        : %d MB (~%d packets)\n",
           (int)((long long)config.fifo_size * 188 / (1024 * 1024)),
           config.fifo_size);
    printf("  Socket Buffer Size   : %d MB\n",
           config.buffer_size / (1024 * 1024));
    printf("  Read Timeout         : %d seconds\n", config.timeout_sec);
    printf("  Wait First Keyframe  : %s\n",
           config.wait_keyframe ? "YES (clean start)" : "NO");
    printf("  Normalize Timestamps : %s\n",
           config.normalize_pts ? "YES (starts at 0.0s)" : "NO");
    printf("  Health Diagnostics   : %s (Buffering & GOP monitor)\n",
           config.enable_diagnostics ? "ACTIVE" : "DISABLED");
    printf("  Telemetry Mode       : %s\n",
           config.log_telemetry
               ? "Standard Timestamped Console Logs"
               : (config.detailed_telemetry ? "Detailed Multi-line Cards"
                                            : "Dynamic Single-line"));
    printf("  Encoding Mode        : PASSTHROUGH / REMUX (0%% CPU Transcode Overhead)\n");
    printf("\033[1;36m========================================================================\033[0m\n\n");
  }

  setup_signal_handlers(on_signal);

  int ret = pipeline_run(&config);
  return (ret == 0) ? 0 : 1;
}
