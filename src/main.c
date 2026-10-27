#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "utils.h"
#include "sdi_decklink.h"
#include "transcode.h"

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

static int64_t parse_bitrate_str(const char *str) {
  if (!str || str[0] == '\0') return 4000000;
  char *end = NULL;
  double val = strtod(str, &end);
  if (!end || *end == '\0') return (int64_t)val;
  if (*end == 'M' || *end == 'm') return (int64_t)(val * 1000000.0);
  if (*end == 'K' || *end == 'k') return (int64_t)(val * 1000.0);
  return (int64_t)val;
}

static AVRational parse_framerate_str(const char *str) {
  if (!str || str[0] == '\0') return (AVRational){0, 0};
  if (strcmp(str, "59.94") == 0 || strcmp(str, "5994") == 0) return (AVRational){60000, 1001};
  if (strcmp(str, "29.97") == 0 || strcmp(str, "2997") == 0) return (AVRational){30000, 1001};
  if (strcmp(str, "23.976") == 0 || strcmp(str, "23.98") == 0) return (AVRational){24000, 1001};
  if (strcmp(str, "60") == 0) return (AVRational){60, 1};
  if (strcmp(str, "50") == 0) return (AVRational){50, 1};
  if (strcmp(str, "30") == 0) return (AVRational){30, 1};
  if (strcmp(str, "25") == 0) return (AVRational){25, 1};
  if (strcmp(str, "24") == 0) return (AVRational){24, 1};
  const char *slash = strchr(str, '/');
  if (slash) {
    int num = atoi(str);
    int den = atoi(slash + 1);
    if (num > 0 && den > 0) return (AVRational){num, den};
  }
  double fps = atof(str);
  if (fps >= 59.93 && fps <= 59.95) return (AVRational){60000, 1001};
  if (fps >= 29.96 && fps <= 29.98) return (AVRational){30000, 1001};
  if (fps >= 23.97 && fps <= 23.99) return (AVRational){24000, 1001};
  if (fps > 0) return (AVRational){(int)(fps * 1000 + 0.5), 1000};
  return (AVRational){0, 0};
}

static void print_usage(const char *prog_name) {
  printf("========================================================================\n");
  printf("   %s %s - Broadcast SDI & IP Streaming Pipeline\n", PIPELINE_NAME, PIPELINE_VERSION);
  printf("   Low-Latency H.264/H.265 Transcode, Blackmagic SDI, Multicast, SRT, RTMP\n");
  printf("========================================================================\n\n");
  printf("Usage: %s [OPTIONS]\n\n", prog_name);
  printf("Ingress & Egress Options:\n");
  printf("  -i, --input <url>          Ingress source: UDP Multicast, RTP, SRT, or Blackmagic SDI\n");
  printf("                             Examples:\n");
  printf("                               udp://@239.255.0.1:1234\n");
  printf("                               rtp://@239.255.0.1:5004  or  rtp://127.0.0.1:5004\n");
  printf("                               srt://:9000?mode=listener\n");
  printf("                               sdi:1  or  \"sdi:DeckLink Duo (1)\"\n");
  printf("  -o, --output <type> <url>  Add egress target. Type: 'rtmp', 'hls', 'srt', 'sdi', 'udp', or 'rtp'.\n");
  printf("                             Can be repeated for multiple concurrent outputs.\n");
  printf("                             Examples:\n");
  printf("                               -o rtp rtp://239.255.0.1:5004\n");
  printf("                               -o udp udp://226.1.1.1:9000\n");
  printf("                               -o rtmp rtmp://127.0.0.1:1935/live/stream\n");
  printf("                               -o srt srt://127.0.0.1:9000?mode=caller\n");
  printf("                               -o sdi \"DeckLink Duo (2)\"  (or -o sdi 2)\n");
  printf("                               -o hls http://origin.cdn.com/live/stream.m3u8\n\n");
  printf("Blackmagic DeckLink SDI Options:\n");
  printf("  --list-sdi, --list-devices Enumerate all installed DeckLink cards, signal lock & modes\n");
  printf("  --sdi-mode <mode>          SDI capture mode: auto, 1080p60, 1080p5994, 1080i50, etc. (Default: auto)\n");
  printf("  --sdi-channels <num>       SDI embedded audio channels: 2 or 8 (Default: 2)\n");
  printf("  --sdi-test-bars            Generate SMPTE colorbars when no SDI carrier / signal detected\n");
  printf("  --sync-type <type>         A/V sync mode: hardware, self (Default: hardware)\n");
  printf("                             hardware: lock timestamps to Blackmagic DeckLink hardware clock\n");
  printf("                             self: generate monotonic frame-count and sample-count timestamps\n\n");
  printf("Low-Latency Transcode & Codec Options (H.264 & H.265/HEVC):\n");
  printf("  --vcodec <codec>           Video codec: copy, h264, hevc, h264_nvenc, hevc_nvenc\n");
  printf("                             (Default: copy / passthrough, auto-enables for SDI)\n");
  printf("  --fps, --framerate <fps>   Target video framerate: 59.94, 60, 50, 29.97, 25, 24, 60000/1001\n");
  printf("                             (Default: auto-detected from hardware signal)\n");
  printf("  --vbitrate <rate>          Video target bitrate (e.g. 4M, 6M, 8000k, default: 4M)\n");
  printf("  --rc, --rate-control <rc>  Rate control mode: cbr, vbr (Default: cbr)\n");
  printf("  --maxrate <rate>           Maximum video bitrate for VBR (e.g. 6M, default: 1.5x vbitrate)\n");
  printf("  --bufsize <bytes>          VBV buffer size (e.g. 2M, default: 500ms for CBR, 2s for VBR)\n");
  printf("  --preset <preset>          Encoder preset: ultrafast, superfast, p1, p2 (Default: ultrafast / p1)\n");
  printf("  --tune <tune>              Encoder tuning: zerolatency, ull (Default: zerolatency)\n");
  printf("  --b-frames <0-3>           Maximum B-frames (Default: 0 for zero latency)\n");
  printf("  --gop <frames>             Keyframe / GOP interval (Default: 30)\n");
  printf("  --intra-refresh            Enable intra-refresh (smooth packet rate, no I-frame bursts)\n");
  printf("  --low-latency              One-click preset: 0 B-frames, zerolatency, 80ms SRT latency\n\n");
  printf("Audio Transcode & Codec Options (AAC, Opus, MP2):\n");
  printf("  --acodec <codec>           Audio codec: copy, aac, opus, mp2\n");
  printf("                             (Default: copy / passthrough, auto-enables AAC for SDI)\n");
  printf("  --abitrate <rate>          Audio target bitrate (e.g. 128k, 192k, 256k, default: 192k)\n");
  printf("  --achannels <num>          Audio channels: 1 or 2 (Default: 2 / stereo)\n");
  printf("  --asample-rate <hz>        Audio sample rate: 48000, 44100 (Default: 48000)\n\n");
  printf("Network & Buffer Options:\n");
  printf("  -a, --local-addr <ip>      Local network interface IP for multicast bind (e.g. 192.168.1.100)\n");
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
  printf("SRT Protocol Options:\n");
  printf("  --srt <url>                SRT destination URL (alias for -o srt <url>, supports multiple)\n");
  printf("  --srt-latency <ms>         Target latency in milliseconds (Default: 200, low-latency: 80)\n");
  printf("  --srt-passphrase <pass>    AES passphrase for encryption (min 10 chars)\n");
  printf("  --srt-streamid <id>        SRT stream ID string\n");
  printf("  --srt-mode <mode>          SRT connection mode: caller, listener, rendezvous\n");
  printf("  --srt-max-clients <num>    Max concurrent clients for SRT listener server (Default: 32)\n\n");
  printf("RTP Protocol Options:\n");
  printf("  --rtp <url>                RTP destination URL (alias for -o rtp <url>, supports multiple)\n\n");
  printf("Confidence Monitor Options:\n");
  printf("  --monitor-file <path>      Output path for JPEG confidence monitor (e.g. /tmp/monitor.jpg)\n");
  printf("  --monitor-fps <num>        Capture rate in FPS (Default: 5)\n");
  printf("  --monitor-scale <WxH>      Scale snapshot resolution (e.g. 640x360, default: native)\n");
  printf("  --monitor-quality <1-31>   JPEG quality (Default: 4, 1=highest)\n\n");
  printf("Examples:\n");
  printf("  # 1. SDI Ingest -> Hardware H.265 (HEVC NVENC) -> Low-Latency SRT Egress\n");
  printf("  %s -i \"sdi:DeckLink Duo (1)\" -o srt srt://192.168.1.100:9000 --vcodec hevc_nvenc --low-latency\n\n", prog_name);
  printf("  # 2. SDI Ingest -> Multi-Client SRT Listener (1-to-many broadcast server for VLC / ffplay)\n");
  printf("  %s -i \"sdi:DeckLink Duo (1)\" -o srt \"srt://0.0.0.0:9000?mode=listener\" --vcodec h264_nvenc\n\n", prog_name);
  printf("  # 3. Dual SRT Targets: Local Multi-Client Listener + Remote Cloud Gateway Caller\n");
  printf("  %s -i udp://@239.255.0.1:1234 -o srt \"srt://0.0.0.0:9000?mode=listener\" -o srt \"srt://10.0.0.50:9000?mode=caller\"\n\n", prog_name);
  printf("  # 4. SDI Ingest -> H.264 Transcode -> Multicast UDP (MPEG-TS) with Test Bars fallback\n");
  printf("  %s -i \"sdi:DeckLink Duo (3)\" -o udp udp://226.1.1.1:9000 --vcodec h264 --sdi-test-bars\n\n", prog_name);
  printf("  # 5. SRT Ingest -> Low-Delay Decode -> SDI Playout (Downlink Monitor)\n");
  printf("  %s -i \"srt://0.0.0.0:9000?mode=listener\" -o sdi \"DeckLink Duo (2)\"\n\n", prog_name);
  printf("  # 6. RTP Ingest -> Transcode -> Dual Egress (RTP RFC 2250 + SRT Caller)\n");
  printf("  %s -i rtp://@239.255.0.1:5004 -o rtp rtp://239.255.0.2:5004 -o srt srt://10.0.0.50:9000 --vcodec h264\n\n", prog_name);
  printf("  # 7. List all installed DeckLink SDI hardware ports\n");
  printf("  %s --list-sdi\n", prog_name);
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
      {"srt", required_argument, 0, 1020},
      {"srt-latency", required_argument, 0, 1021},
      {"srt-passphrase", required_argument, 0, 1022},
      {"srt-streamid", required_argument, 0, 1023},
      {"srt-mode", required_argument, 0, 1024},
      {"srt-max-clients", required_argument, 0, 1026},
      {"udp", required_argument, 0, 1025},
      {"multicast", required_argument, 0, 1025},
      {"rtp", required_argument, 0, 1028},
      {"list-sdi", no_argument, 0, 1030},
      {"list-devices", no_argument, 0, 1030},
      {"sdi-mode", required_argument, 0, 1031},
      {"sdi-channels", required_argument, 0, 1032},
      {"sdi-test-bars", no_argument, 0, 1033},
      {"test-bars", no_argument, 0, 1033},
      {"draw-bars", no_argument, 0, 1033},
      {"vcodec", required_argument, 0, 1040},
      {"video-codec", required_argument, 0, 1040},
      {"vbitrate", required_argument, 0, 1041},
      {"preset", required_argument, 0, 1042},
      {"tune", required_argument, 0, 1043},
      {"gop", required_argument, 0, 1044},
      {"b-frames", required_argument, 0, 1045},
      {"intra-refresh", no_argument, 0, 1046},
      {"low-latency", no_argument, 0, 1047},
      {"rc", required_argument, 0, 1048},
      {"rate-control", required_argument, 0, 1048},
      {"maxrate", required_argument, 0, 1049},
      {"bufsize", required_argument, 0, 1054},
      {"acodec", required_argument, 0, 1050},
      {"audio-codec", required_argument, 0, 1050},
      {"abitrate", required_argument, 0, 1051},
      {"audio-bitrate", required_argument, 0, 1051},
      {"achannels", required_argument, 0, 1052},
      {"asample-rate", required_argument, 0, 1053},
      {"ar", required_argument, 0, 1053},
      {"fps", required_argument, 0, 1055},
      {"framerate", required_argument, 0, 1055},
      {"sync-type", required_argument, 0, 1060},
      {"sync", required_argument, 0, 1060},
      {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "i:o:a:f:b:t:kns:ldjvhV", long_options,
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
      } else if (strcasecmp(optarg, "srt") == 0) {
        type = OUTPUT_TYPE_SRT;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing URL argument after '-o srt'");
          return 1;
        }
      } else if (strcasecmp(optarg, "udp") == 0 || strcasecmp(optarg, "multicast") == 0) {
        type = OUTPUT_TYPE_UDP;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing URL argument after '-o udp'");
          return 1;
        }
      } else if (strcasecmp(optarg, "rtp") == 0) {
        type = OUTPUT_TYPE_RTP;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing URL argument after '-o rtp'");
          return 1;
        }
      } else if (strcasecmp(optarg, "sdi") == 0) {
        type = OUTPUT_TYPE_SDI;
        if (optind < argc && argv[optind][0] != '-') {
          url = argv[optind++];
        } else {
          LOG_ERROR("Missing Device argument after '-o sdi'");
          return 1;
        }
      } else if (strncmp(optarg, "sdi:", 4) == 0) {
        type = OUTPUT_TYPE_SDI;
        url = optarg + 4;
      } else if (strncmp(optarg, "srt://", 6) == 0) {
        type = OUTPUT_TYPE_SRT;
        url = optarg;
      } else if (strncmp(optarg, "rtp://", 6) == 0) {
        type = OUTPUT_TYPE_RTP;
        url = optarg;
      } else if (strncmp(optarg, "udp://", 6) == 0) {
        type = OUTPUT_TYPE_UDP;
        url = optarg;
      } else if (strncmp(optarg, "rtmp://", 7) == 0 || strncmp(optarg, "rtmps://", 8) == 0) {
        type = OUTPUT_TYPE_RTMP;
        url = optarg;
      } else if (strncmp(optarg, "http://", 7) == 0 || strncmp(optarg, "https://", 8) == 0 ||
                 (strlen(optarg) >= 5 && strcmp(optarg + strlen(optarg) - 5, ".m3u8") == 0)) {
        type = OUTPUT_TYPE_HLS;
        url = optarg;
      } else if (strncasecmp(optarg, "sdi=", 4) == 0 || strncasecmp(optarg, "sdi ", 4) == 0) {
        type = OUTPUT_TYPE_SDI;
        url = optarg + 4;
        while (*url == ' ') url++;
      } else if (strncasecmp(optarg, "srt=", 4) == 0 || strncasecmp(optarg, "srt ", 4) == 0) {
        type = OUTPUT_TYPE_SRT;
        url = optarg + 4;
        while (*url == ' ') url++;
      } else if (strncasecmp(optarg, "rtp=", 4) == 0 || strncasecmp(optarg, "rtp ", 4) == 0) {
        type = OUTPUT_TYPE_RTP;
        url = optarg + 4;
        while (*url == ' ') url++;
      } else if (strncasecmp(optarg, "udp=", 4) == 0 || strncasecmp(optarg, "udp ", 4) == 0) {
        type = OUTPUT_TYPE_UDP;
        url = optarg + 4;
        while (*url == ' ') url++;
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

      char rtp_buf[1024];
      if (type == OUTPUT_TYPE_RTP && strncmp(url, "rtp://", 6) != 0) {
        snprintf(rtp_buf, sizeof(rtp_buf), "rtp://%s", url);
        url = rtp_buf;
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
      if (config.hls_time <= 0) config.hls_time = 2;
      break;
    case 1004:
      config.hls_list_size = atoi(optarg);
      if (config.hls_list_size <= 0) config.hls_list_size = 5;
      break;
    case 1005:
      strncpy(config.hls_segment_filename, optarg, sizeof(config.hls_segment_filename) - 1);
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
    case 1020:
      add_output(&config, OUTPUT_TYPE_SRT, optarg);
      break;
    case 1021:
      config.srt_latency_ms = atoi(optarg);
      break;
    case 1022:
      strncpy(config.srt_passphrase, optarg, sizeof(config.srt_passphrase) - 1);
      break;
    case 1023:
      strncpy(config.srt_streamid, optarg, sizeof(config.srt_streamid) - 1);
      break;
    case 1024:
      strncpy(config.srt_mode, optarg, sizeof(config.srt_mode) - 1);
      break;
    case 1026:
      config.srt_max_clients = atoi(optarg);
      if (config.srt_max_clients <= 0) config.srt_max_clients = 32;
      break;
    case 1025:
      add_output(&config, OUTPUT_TYPE_UDP, optarg);
      break;
    case 1028: {
      char rtp_buf[1024];
      const char *target_url = optarg;
      if (strncmp(target_url, "rtp://", 6) != 0) {
        snprintf(rtp_buf, sizeof(rtp_buf), "rtp://%s", target_url);
        target_url = rtp_buf;
      }
      add_output(&config, OUTPUT_TYPE_RTP, target_url);
      break;
    }
    case 1030:
      sdi_list_devices(NULL, 0);
      return 0;
    case 1031:
      strncpy(config.sdi_mode, optarg, sizeof(config.sdi_mode) - 1);
      break;
    case 1032:
      config.sdi_channels = atoi(optarg);
      if (config.sdi_channels <= 0) config.sdi_channels = 2;
      break;
    case 1033:
      config.sdi_test_bars = true;
      break;
    case 1040:
      config.transcode.vcodec = transcode_parse_codec_name(optarg);
      strncpy(config.transcode.vcodec_name, optarg, sizeof(config.transcode.vcodec_name) - 1);
      break;
    case 1041:
      config.transcode.vbitrate = parse_bitrate_str(optarg);
      break;
    case 1042:
      strncpy(config.transcode.preset, optarg, sizeof(config.transcode.preset) - 1);
      break;
    case 1043:
      strncpy(config.transcode.tune, optarg, sizeof(config.transcode.tune) - 1);
      break;
    case 1044:
      config.transcode.gop_size = atoi(optarg);
      break;
    case 1045:
      config.transcode.b_frames = atoi(optarg);
      break;
    case 1046:
      config.transcode.intra_refresh = true;
      break;
    case 1047:
      config.transcode.low_delay = true;
      config.transcode.b_frames = 0;
      config.srt_latency_ms = 80;
      break;
    case 1048:
      config.transcode.rc = transcode_parse_rc_name(optarg);
      strncpy(config.transcode.rc_name, optarg, sizeof(config.transcode.rc_name) - 1);
      break;
    case 1049:
      config.transcode.maxrate = parse_bitrate_str(optarg);
      break;
    case 1054:
      config.transcode.bufsize = parse_bitrate_str(optarg);
      break;
    case 1050:
      config.audio_transcode.acodec = transcode_parse_audio_codec_name(optarg);
      strncpy(config.audio_transcode.acodec_name, optarg, sizeof(config.audio_transcode.acodec_name) - 1);
      break;
    case 1051:
      config.audio_transcode.abitrate = (int)parse_bitrate_str(optarg);
      break;
    case 1052:
      config.audio_transcode.channels = atoi(optarg);
      break;
    case 1053:
      config.audio_transcode.sample_rate = atoi(optarg);
      break;
    case 1055:
      config.transcode.framerate = parse_framerate_str(optarg);
      break;
    case 1060:
      config.sync_type = parse_sync_type(optarg);
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  /* Support positional output URLs (e.g. trailing "udp://..." without -o flag) */
  while (optind < argc) {
    const char *arg = argv[optind++];
    OutputType type = OUTPUT_TYPE_RTMP;
    if (strncmp(arg, "udp://", 6) == 0) {
      type = OUTPUT_TYPE_UDP;
    } else if (strncmp(arg, "rtp://", 6) == 0) {
      type = OUTPUT_TYPE_RTP;
    } else if (strncmp(arg, "srt://", 6) == 0) {
      type = OUTPUT_TYPE_SRT;
    } else if (strncmp(arg, "sdi:", 4) == 0) {
      type = OUTPUT_TYPE_SDI;
      arg += 4;
    } else if (strncmp(arg, "http://", 7) == 0 || strncmp(arg, "https://", 8) == 0 ||
               (strlen(arg) >= 5 && strcmp(arg + strlen(arg) - 5, ".m3u8") == 0)) {
      type = OUTPUT_TYPE_HLS;
    } else if (strncmp(arg, "rtmp://", 7) == 0 || strncmp(arg, "rtmps://", 8) == 0) {
      type = OUTPUT_TYPE_RTMP;
    }
    add_output(&config, type, arg);
  }

  /* Default to RTMP if no output was explicitly specified */
  if (config.num_outputs == 0) {
    if (config.output_url[0] != '\0') {
      add_output(&config, OUTPUT_TYPE_RTMP, config.output_url);
    } else {
      add_output(&config, OUTPUT_TYPE_RTMP, "rtmp://127.0.0.1:1935/live/stream");
    }
  }

  /* Auto-enable transcode if ingesting raw SDI without an explicit vcodec */
  if (strncmp(config.input_url, "sdi:", 4) == 0 && config.transcode.vcodec == TRANSCODE_CODEC_COPY) {
    config.transcode.vcodec = TRANSCODE_CODEC_H264;
    strncpy(config.transcode.vcodec_name, "h264", sizeof(config.transcode.vcodec_name) - 1);
  }

  /* Sync legacy fallback fields */
  for (int i = 0; i < config.num_outputs; i++) {
    if (config.outputs[i].type == OUTPUT_TYPE_RTMP && config.output_url[0] == '\0') {
      strncpy(config.output_url, config.outputs[i].url, sizeof(config.output_url) - 1);
    }
    if (config.outputs[i].type == OUTPUT_TYPE_HLS && config.hls_url[0] == '\0') {
      strncpy(config.hls_url, config.outputs[i].url, sizeof(config.hls_url) - 1);
    }
    if (config.outputs[i].type == OUTPUT_TYPE_SRT && config.srt_url[0] == '\0') {
      strncpy(config.srt_url, config.outputs[i].url, sizeof(config.srt_url) - 1);
    }
    if (config.outputs[i].type == OUTPUT_TYPE_RTP && config.rtp_url[0] == '\0') {
      strncpy(config.rtp_url, config.outputs[i].url, sizeof(config.rtp_url) - 1);
    }
  }

  if (!config.json_telemetry) {
    /* Print startup banner */
    printf("\n\033[1;36m========================================================================\033[0m\n");
    printf("\033[1;36m   %s %s - Broadcast SDI & Low-Latency IP Pipeline\033[0m\n", PIPELINE_NAME, PIPELINE_VERSION);
    printf("\033[1;36m========================================================================\033[0m\n");
    if (strncmp(config.input_url, "sdi:", 4) == 0) {
      printf("  Input SDI Port       : %s (Mode: %s, %d ch audio)\n",
             config.input_url + 4, config.sdi_mode, config.sdi_channels);
    } else if (strncmp(config.input_url, "srt://", 6) == 0) {
      printf("  Input SRT URL        : %s\n", config.input_url);
    } else if (strncmp(config.input_url, "rtp://", 6) == 0) {
      printf("  Input RTP URL        : %s (RFC 2250 RTP/MPEG-TS)\n", config.input_url);
    } else {
      printf("  Input Multicast URL  : %s\n", config.input_url);
    }

    for (int i = 0; i < config.num_outputs; i++) {
      if (config.outputs[i].type == OUTPUT_TYPE_RTMP) {
        printf("  Output RTMP [%d]     : %s\n", i + 1, config.outputs[i].url);
      } else if (config.outputs[i].type == OUTPUT_TYPE_HLS) {
        printf("  Output HLS [%d]      : %s (Method: %s, %ds chunks, list size %d)\n",
               i + 1, config.outputs[i].url, config.hls_method, config.hls_time, config.hls_list_size);
      } else if (config.outputs[i].type == OUTPUT_TYPE_SRT) {
        bool is_listener = (strstr(config.outputs[i].url, "mode=listener") != NULL ||
                            (config.srt_mode[0] != '\0' && strcasecmp(config.srt_mode, "listener") == 0) ||
                            strstr(config.outputs[i].url, "0.0.0.0") != NULL ||
                            strstr(config.outputs[i].url, "srt://:") != NULL);
        if (strstr(config.outputs[i].url, "mode=caller") != NULL) is_listener = false;
        printf("  Output SRT [%d]      : %s (%s%s, Latency: %dms%s%s)\n",
               i + 1, config.outputs[i].url,
               is_listener ? "Multi-Client Server" : "Point-to-Point Caller",
               is_listener ? " [1-to-many]" : "",
               config.srt_latency_ms,
               config.srt_mode[0] ? ", Mode: " : "", config.srt_mode[0] ? config.srt_mode : "");
      } else if (config.outputs[i].type == OUTPUT_TYPE_SDI) {
        printf("  Output SDI [%d]      : %s (Zero-delay baseband playout)\n",
               i + 1, config.outputs[i].url);
      } else if (config.outputs[i].type == OUTPUT_TYPE_UDP) {
        printf("  Output UDP [%d]      : %s (MPEG-TS Multicast)\n",
               i + 1, config.outputs[i].url);
      } else if (config.outputs[i].type == OUTPUT_TYPE_RTP) {
        printf("  Output RTP [%d]      : %s (RFC 2250 RTP/MPEG-TS)\n",
               i + 1, config.outputs[i].url);
      }
    }

    if (config.sdi_test_bars) {
      printf("  SDI Test Bars        : ENABLED (SMPTE 75%% Color Bars on NO CARRIER)\n");
    }

    if (config.transcode.vcodec != TRANSCODE_CODEC_COPY) {
      if (config.transcode.rc == TRANSCODE_RC_VBR) {
        printf("  Video Transcoder     : \033[1;32m%s\033[0m (Target: %d kbps, Max: %d kbps, %s, GOP: %d, B-frames: %d, %s)\n",
               transcode_get_codec_name(config.transcode.vcodec),
               (int)(config.transcode.vbitrate / 1000),
               (int)((config.transcode.maxrate > 0 ? config.transcode.maxrate : (int64_t)(config.transcode.vbitrate * 1.5)) / 1000),
               transcode_get_rc_name(config.transcode.rc),
               config.transcode.gop_size,
               config.transcode.b_frames,
               config.transcode.intra_refresh ? "Intra-Refresh ON" : "Intra-Refresh OFF");
      } else {
        printf("  Video Transcoder     : \033[1;32m%s\033[0m (%d kbps, %s, GOP: %d, B-frames: %d, %s)\n",
               transcode_get_codec_name(config.transcode.vcodec),
               (int)(config.transcode.vbitrate / 1000),
               transcode_get_rc_name(config.transcode.rc),
               config.transcode.gop_size,
               config.transcode.b_frames,
               config.transcode.intra_refresh ? "Intra-Refresh ON" : "Intra-Refresh OFF");
      }
    } else {
      printf("  Video Transcoder     : \033[1;33mPASSTHROUGH / ZERO-COPY\033[0m (0%% CPU Transcode Overhead)\n");
    }

    if (config.audio_transcode.acodec != TRANSCODE_AUDIO_COPY) {
      printf("  Audio Transcoder     : \033[1;32m%s\033[0m (%d kbps, %d Hz, %d ch)\n",
             transcode_get_audio_codec_name(config.audio_transcode.acodec),
             (int)(config.audio_transcode.abitrate / 1000),
             config.audio_transcode.sample_rate,
             config.audio_transcode.channels);
    } else {
      printf("  Audio Transcoder     : \033[1;33mPASSTHROUGH / COPY\033[0m (0%% CPU Transcode Overhead)\n");
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
    printf("\033[1;36m========================================================================\033[0m\n\n");
  }

  setup_signal_handlers(on_signal);

  int ret = pipeline_run(&config);
  return (ret == 0) ? 0 : 1;
}
