# ixgIPStream (v4.2) - Broadcast SDI & IP Streaming Pipeline

A high-performance, broadcast-grade C/C++ streaming pipeline for **Blackmagic DeckLink SDI**, **UDP Multicast (MPEG-TS)**, **RTP (RFC 2250)**, and **SRT (Secure Reliable Transport)**. 

`ixgIPStream` supports both **Zero-Encoding Passthrough (Remuxing / Stream Copy)** with 0% CPU transcode overhead and an **Ultra-Low-Latency Hardware & Software Transcode Engine** (NVIDIA NVENC GPU & x264/x265 CPU). It concurrently distributes live feeds across **RTMP/RTMPS**, **HLS**, **SRT (Caller & Multi-Client Broadcast Server)**, **Blackmagic SDI Playout**, **UDP Multicast**, and **RFC 2250 RTP**, accompanied by real-time transport QoS analytics and an atomic **Confidence Monitor** snapshot engine.

---

## Key Highlights & Features

- **Dual-Mode Processing Engine**:
  - **Zero-Encoding Passthrough (Default)**: Direct packet remuxing (`av_read_frame` -> `av_packet_rescale_ts` -> `av_interleaved_write_frame`). Preserves 100% video/audio quality with microsecond latency and near-zero CPU/memory footprint.
  - **Ultra-Low-Latency Transcoder**: Low-delay video decoding and encoding using **NVIDIA NVENC GPU** (`h264_nvenc`, `hevc_nvenc`) and **CPU** (`libx264`, `libx265`). Features 0 B-frames, zero frame lookahead, slice threading, intra-refresh, and VBV CBR/VBR rate control.
- **Blackmagic DeckLink SDI Ingest & Playout (DeckLink SDK v14.2)**:
  - Native C++ hardware bridge interfacing directly with Blackmagic hardware (`BMSDK_14_2`).
  - Automatic SDI carrier lock and video format detection (1080p60, 1080p59.94, 1080i50, 720p60, etc.).
  - Zero-delay SDI baseband playout (`-o sdi <device>`).
  - Automatic fallback generator: Displays SMPTE 75% color bars when carrier loss occurs (`--sdi-test-bars`).
- **Broadcast A/V Clock Synchronization & Framerate Locking**:
  - **Hardware Clock Mode (`--sync-type hardware`)**: Anchors both video and audio timestamps to the DeckLink hardware clock (`GetStreamTime` & `GetPacketTime`).
  - **Self-Generated Monotonic Clock (`--sync-type self`)**: Generates pure synthetic monotonic frame-counter ($N \times \text{frame\_duration}$) and sample-counter ($S / 48000\text{s}$) timestamps from $t = 0$, completely immune to input jitter and clock drift.
  - **Auto NTSC Pull-Down Framerate Lock (`--fps auto`)**: Automatically differentiates 59.94p (`fd == 2002`) from 60.00p (`fd == 2000`) on wire, eliminating the 1.0 ms/s cumulative pull-down drift.
- **Audio Transcoding & Resampling Engine**:
  - Built-in low-latency audio transcoding supporting **AAC**, **Opus**, and **MP2** (`--acodec`).
  - Automatic resampling to 48kHz S16 stereo for SDI hardware playout.
- **Multi-Client SRT Listener (1-to-Many Broadcast Server)**:
  - Embedded multi-client SRT listener server supporting up to 32 concurrent clients.
  - Caches MPEG-TS PAT/PMT PSI tables and sends them immediately upon client connection.
  - Client connections and disconnections do not interrupt ongoing streams or other viewers.
- **Bi-Directional RTP Support (RFC 2250 / MP2T)**:
  - **RTP Ingest**: Ingests RTP streams (`rtp://@...`) with dynamic jitter reordering buffer (`reorder_queue_size=500`).
  - **RTP Egress**: Transmits standard RFC 2250 datagrams (12-byte RTP headers, PT=33, 7 TS packets / MTU = 1316 bytes payload) with periodic PAT/PMT table resending.
- **Multi-Egress Fan-Out (Up to 16 Concurrent Targets)**:
  - Fan out a single ingress stream to any combination of RTMP, HLS, SRT, SDI, UDP, and RTP destinations simultaneously.
- **Timestamp Normalization & Keyframe Gating**:
  - Discards intermediate non-keyframe packets and audio until the first video IDR keyframe arrives, ensuring clean player startups.
  - Normalizes arbitrary multicast/SRT PCR/PTS timestamps to cleanly begin at `0.000s`, enforcing strictly monotonic non-decreasing DTS (`dts <= pts`).
- **Real-Time YouTube Buffering & Health Diagnostics**:
  - Deep diagnostics engine detects underfeeding (<1.0x delivery speed), track starvation, GOP > 4.0s, video frame drops, and uplink TCP socket stalls.
- **Multi-Layer Transport QoS Analytics**:
  - **MPEG-TS CC Drops**: Continuity counter gap tracking for UDP multicast and RTP.
  - **SRT ARQ Statistics**: Queries `libsrt` `srt_bstats` (RTT, NAKs, packet loss, ARQ recovery %, bandwidth, buffer latency).
  - **TCP Uplink QoS**: Scans Linux kernel `TCP_INFO` via `/proc/self/fd` to measure exact TCP retransmissions, in-flight lost packets, RTT, and jitter variance.
- **Integrated Atomic Confidence Monitor**:
  - Background worker captures frames at a configurable rate (e.g. 5 FPS), scales optionally, and writes JPEG previews atomically via `.tmp` and POSIX `rename()` for multiviewer web dashboards.

---

## Architecture Flow

```text
┌────────────────────────────────────────────────────────────────────────┐
│ Ingress Sources                                                        │
│ - Blackmagic DeckLink SDI  (sdi:1 or "sdi:DeckLink Duo (1)")           │
│ - UDP Multicast (MPEG-TS)  (udp://@239.255.0.1:1234)                   │
│ - RTP Stream (RFC 2250)    (rtp://@239.255.0.1:5004)                   │
│ - SRT Stream (Caller/List) (srt://:9000?mode=listener)                 │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
       ┌─────────────────────────────────────────────────────────┐
       │ Ingress Demuxer & Signal Processing                     │
       │ - DeckLink SDI Hardware Bridge (BMSDK 14.2)             │
       │ - Auto-Format Detection & SMPTE 75% Colorbars Fallback  │
       │ - Kernel Socket Buffer Tuning & Jitter Buffer           │
       │ - Clock Synchronization (Hardware vs Self-Monotonic)    │
       └────────────────────────────┬────────────────────────────┘
                                    │
                 ┌──────────────────┴──────────────────┐
                 │                                     │
                 ▼ [Branch 1: Passthrough]             ▼ [Branch 2: Transcode]
   ┌───────────────────────────┐         ┌───────────────────────────────┐
   │ Zero-Encoding Remux       │         │ Ultra-Low-Latency Transcoder  │
   │ - Direct packet copy      │         │ - H.264 / HEVC / NVENC GPU    │
   │ - First Keyframe Gating   │         │ - 0 B-frames, Intra-Refresh   │
   │ - Monotonic DTS / PTS = 0 │         │ - Audio: AAC / Opus / MP2     │
   │ - Rescale Timebases       │         │ - 48kHz Resampler for SDI     │
   └─────────────┬─────────────┘         └───────────────┬───────────────┘
                 │                                       │
                 └──────────────────┬────────────────────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              │                     │                     │
              ▼                     ▼                     ▼
┌───────────────────────────┐ ┌───────────┐ ┌───────────────────────────┐
│ Multi-Egress Distribution │ │ Telemetry │ │ Confidence Monitor        │
├───────────────────────────┤ ├───────────┤ ├───────────────────────────┤
│ • RTMP/RTMPS (YouTube/FB) │ │ • Term UI │ │ • Scaled MJPEG snapshot   │
│ • HLS (TS / fMP4 HTTP)    │ │ • Cards   │ │ • Atomic POSIX rename     │
│ • SRT (1-to-many & Caller)│ │ • JSON    │ │ • Output: /tmp/preview.jpg│
│ • Blackmagic SDI Playout  │ │ • Logs    │ └───────────────────────────┘
│ • UDP Multicast (MPEG-TS) │ └───────────┘
│ • RTP (RFC 2250 MP2T)     │
└───────────────────────────┘
```

---

## Project Structure

```text
.
├── CMakeLists.txt               # CMake configuration (v4.2)
├── Makefile                     # Standard Makefile (targets: all, clean, test)
├── README.md                    # Project documentation
├── BMSDK_14_2/                  # Bundled Blackmagic DeckLink SDK v14.2 Linux
│   └── Linux/include/
│       ├── DeckLinkAPI.h
│       └── DeckLinkAPIDispatch.cpp
├── src/
│   ├── main.c                   # CLI parser, banner, pipeline coordinator
│   ├── pipeline.c               # Core streaming loop, egress routing, telemetry
│   ├── pipeline.h               # Pipeline configuration structures and public API
│   ├── transcode.c              # Low-latency video (x264/x265/NVENC) & audio transcode
│   ├── transcode.h              # Transcode API definitions
│   ├── sdi_decklink.cpp         # DeckLink SDI hardware bridge & SMPTE test bars
│   ├── sdi_decklink.h           # DeckLink SDI API interface
│   ├── rtmp.c                   # RTMP connection & TCP_INFO socket stats
│   ├── rtmp.h                   # RTMP helper declarations
│   ├── hls.c                    # HLS segmenter, HTTP PUT/POST, custom I/O callbacks
│   ├── hls.h                    # HLS state and telemetry definitions
│   ├── srt.c                    # SRT caller, multi-client listener & ARQ stats
│   ├── srt.h                    # SRT helper declarations
│   ├── logger.c                 # Thread-safe logging subsystem
│   ├── logger.h                 # Log macros (DEBUG, INFO, WARN, ERROR)
│   ├── utils.c                  # Timers, signals, formatting, POSIX helpers
│   └── utils.h                  # Utility prototypes
└── scripts/
    ├── simulate_multicast.sh    # Generates synthetic MPEG-TS multicast test feed
    ├── simulate_rtmp_server.sh  # Lightweight local RTMP receiver
    ├── test_end_to_end.sh       # Automated multicast-to-RTMP integration test
    ├── test_rtp.sh              # Automated RFC 2250 RTP Ingress & Egress test
    ├── test_srt_multiclient.sh  # Multi-client concurrent SRT listener broadcast test
    ├── test_srt_telemetry.sh    # SRT ARQ loss & telemetry verification test
    └── generate_pdf_guide.py    # Automated PDF telemetry & log guide generator
```

---

## Prerequisites & Installation

### Operating System & Toolchain
- **Linux** (Ubuntu 20.04+, Debian 11+, Rocky Linux / RHEL 8+)
- **GCC / G++** with C11 and C++11 support
- **pkg-config**

### Required Libraries
- **FFmpeg development libraries** (`>= 4.x`, fully tested on `5.x`, `6.x`, `7.x`):
  - `libavformat-dev`
  - `libavcodec-dev`
  - `libavutil-dev`
  - `libswscale-dev`
  - `libswresample-dev`
- **SRT (Secure Reliable Transport)**:
  - `libsrt-openssl-dev` or `libsrt-dev` (`>= 1.4.0`)
- **Blackmagic DeckLink Driver (Desktop Video)**:
  - Required only if using physical SDI capture cards. The DeckLink SDK headers and dispatcher (`BMSDK_14_2`) are already bundled with this repository.

#### On Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    pkg-config \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libsrt-openssl-dev
```

---

## Building the Pipeline

### Option 1: Using Makefile (Fastest)
```bash
make -j$(nproc)
```

To clean build artifacts:
```bash
make clean
```

### Option 2: Using CMake
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The resulting executable `ixgIPStream` is generated in the root directory (or `build/`).

---

## Command Line Reference

```bash
./ixgIPStream [OPTIONS]
```

### 1. Ingress & Egress Options

| Flag | Long Option | Default | Description |
| :--- | :--- | :--- | :--- |
| `-i` | `--input <url>` | `udp://@239.255.0.1:1234` | Ingress source URL. Supports UDP Multicast (`udp://...`), RTP (`rtp://...`), SRT (`srt://...`), or Blackmagic SDI (`sdi:1`, `sdi:DeckLink Duo (1)`). |
| `-o` | `--output <type> <url>` | `rtmp://127.0.0.1:1935/live/stream` | Add egress target. Types: `rtmp`, `hls`, `srt`, `sdi`, `udp`, `rtp`. Can be repeated up to 16 times for simultaneous multi-protocol fan-out. |
| `-a` | `--local-addr <ip>` | `""` | Local network interface IP to bind for IGMP multicast join / local socket bind. |
| `-f` | `--fifo-size <mb>` | `50` | Internal UDP FIFO queue size in Megabytes. |
| `-b` | `--buffer-size <mb>`| `16` | Kernel UDP socket receive buffer in Megabytes (`SO_RCVBUF`). |
| `-t` | `--timeout <sec>` | `10` | Network inactivity read timeout in seconds. |
| `-k` | `--no-wait-keyframe` | `false` | Start streaming immediately without waiting for the first IDR video keyframe. |
| `-n` | `--no-normalize-pts` | `false` | Disable offsetting PTS/DTS to start at `0.000s`. |
| `-s` | `--stats-interval <ms>`| `1000` | Telemetry refresh interval in milliseconds. |
| `-l` | `--log-stats` | `false` | Print telemetry as standard timestamped log lines (ideal for `tee` and `systemd`). |
| `-d` | `--detailed` | `false` | Display detailed multi-line telemetry cards each second in the terminal. |
| `-j` | `--json` | `false` | Output 1-second JSON telemetry objects to stdout for monitoring agents. |
| `-v` | `--verbose` | `false` | Enable verbose debug logging (`LOG_LVL_DEBUG`). |
| `-V` | `--version` | — | Display version information (`v4.2.0`). |
| `-h` | `--help` | — | Display comprehensive help menu. |

---

### 2. Blackmagic DeckLink SDI Options

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--list-sdi`, `--list-devices` | — | Scan and display all installed Blackmagic DeckLink cards, signal lock status, detected resolution, and framerate, then exit. |
| `--sdi-mode <mode>` | `auto` | SDI video capture mode: `auto`, `1080p60`, `1080p5994`, `1080i50`, `720p60`, etc. |
| `--sdi-channels <num>` | `2` | SDI embedded audio channels: `2` (stereo) or `8` (multichannel surround). |
| `--sdi-test-bars` | `false` | Automatically generate SMPTE 75% color bars when carrier loss or no SDI input signal is detected. |
| `--sync-type <type>` | `hardware` | A/V sync mode: `hardware` (anchor timestamps to DeckLink hardware clock) or `self` (synthetic monotonic frame/sample counters starting at $t=0$). |

---

### 3. Low-Latency Video Transcoder Options (H.264 & HEVC)

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--vcodec <codec>` | `copy` | Video encoder: `copy` (passthrough), `h264` (libx264), `hevc` (libx265), `h264_nvenc` (NVIDIA H.264), `hevc_nvenc` (NVIDIA HEVC). Automatically defaults to `h264` when ingesting raw SDI. |
| `--fps, --framerate <fps>` | `auto` | Target video transcode framerate (`59.94`, `60`, `50`, `29.97`, `25`, `24`, `60000/1001`). `auto` automatically locks 59.94p vs 60.00p based on input hardware wire duration. |
| `--vbitrate <rate>` | `4M` | Target video encoding bitrate (e.g. `4M`, `6M`, `8000k`). |
| `--rc, --rate-control <rc>` | `cbr` | Rate control mode: `cbr` (Constant Bitrate) or `vbr` (Variable Bitrate). |
| `--maxrate <rate>` | `1.5x vbitrate` | Maximum video bitrate for VBR mode. |
| `--bufsize <bytes>` | `500ms/2s` | VBV buffer size (500ms for CBR, 2s for VBR). |
| `--preset <preset>` | `ultrafast / p1`| Encoder preset (`ultrafast`, `superfast` for CPU; `p1`-`p7` for NVENC). |
| `--tune <tune>` | `zerolatency` | Encoder tuning (`zerolatency`, `ull`). |
| `--b-frames <0-3>` | `0` | Maximum consecutive B-frames (set to `0` for zero-latency streaming). |
| `--gop <frames>` | `30` | Keyframe / GOP interval in frames. |
| `--intra-refresh` | `false` | Enable intra-refresh (smooth packet distribution across P-frames, eliminating I-frame network spikes). |
| `--low-latency` | `false` | One-click low-latency preset: sets 0 B-frames, zerolatency tuning, and 80ms SRT latency. |

---

### 4. Audio Transcoder Options (AAC, Opus, MP2)

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--acodec <codec>` | `copy` | Audio codec: `copy` (passthrough), `aac`, `opus`, `mp2`. Auto-enables `aac` for SDI ingest. |
| `--abitrate <rate>` | `192k` | Audio bitrate (e.g. `128k`, `192k`, `256k`). |
| `--achannels <num>` | `2` | Audio channels: `1` (mono) or `2` (stereo). |
| `--asample-rate <hz>` | `48000` | Audio sample rate: `48000` or `44100`. |

---

### 5. SRT Protocol Options

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--srt <url>` | `""` | SRT destination URL (alias for `-o srt <url>`). |
| `--srt-latency <ms>` | `200` | Target recovery buffer latency in milliseconds (`--low-latency` sets `80`). |
| `--srt-passphrase <pass>`| `""` | AES passphrase for encrypted connections (10-79 characters). |
| `--srt-streamid <id>` | `""` | Stream ID for multi-tenant SRT gateways and dispatchers. |
| `--srt-mode <mode>` | `caller` | SRT connection mode: `caller`, `listener`, or `rendezvous`. |
| `--srt-max-clients <num>`| `32` | Maximum concurrent clients for the SRT listener broadcast server. |

---

### 6. RTP & UDP Protocol Options

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--rtp <url>` | `""` | RTP destination URL (alias for `-o rtp <url>`, transmits RFC 2250 MP2T). |
| `--udp <url>`, `--multicast <url>` | `""` | UDP / Multicast destination URL (alias for `-o udp <url>`). |

---

### 7. HLS Protocol Options

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--hls <url>` | `""` | HLS destination playlist URL or filesystem path (alias for `-o hls <url>`). |
| `--hls-method <method>` | `PUT` | HTTP method for remote HLS upload (`PUT` or `POST`). |
| `--hls-time <sec>` | `2` | Target segment duration in seconds. |
| `--hls-list-size <num>` | `5` | Maximum number of segments retained in the live playlist. |
| `--hls-segment <pattern>`| `auto` | Custom segment naming pattern (e.g. `http://origin/live/stream_%05d.ts`). |
| `--hls-fmp4` | `false` | Use Fragmented MP4 (`.m4s`) chunks instead of MPEG-TS (`.ts`). |
| `--hls-no-delete` | `false` | Retain expired segments on disk instead of unlinking them. |

---

### 8. Confidence Monitor Options (JPEG Preview)

| Long Option | Default | Description |
| :--- | :--- | :--- |
| `--monitor-file <path>` | `""` | Destination path for atomic JPEG preview snapshot (e.g. `/tmp/monitor.jpg`). |
| `--monitor-fps <num>` | `5` | Capture rate in frames per second. |
| `--monitor-scale <WxH>` | Native | Output preview resolution (e.g. `640x360`). Defaults to source resolution. |
| `--monitor-quality <1-31>`| `4` | MJPEG quantization quality factor (`1` = highest quality, `31` = lowest). |

---

## Production Examples

### 1. SDI Ingest to Low-Latency SRT (Hardware HEVC NVENC)
Ingest an uncompressed baseband feed from a DeckLink card, encode in hardware H.265 using NVIDIA NVENC, and stream over SRT with 80ms buffer latency:
```bash
./ixgIPStream \
    -i "sdi:DeckLink Duo (1)" \
    -o srt "srt://192.168.1.100:9000?mode=caller" \
    --vcodec hevc_nvenc \
    --vbitrate 6M \
    --low-latency
```

### 2. SDI Ingest to Multi-Client SRT Listener (1-to-Many Server)
Run an embedded broadcast server directly on the machine. Multiple VLC instances, OBS studios, or hardware IRD decoders can connect concurrently to `srt://<ip>:9000`:
```bash
./ixgIPStream \
    -i "sdi:DeckLink Duo (1)" \
    -o srt "srt://0.0.0.0:9000?mode=listener" \
    --vcodec h264_nvenc \
    --vbitrate 4M \
    --srt-max-clients 32
```

### 3. IP / SRT Ingest to Baseband SDI Playout (Downlink / Multiviewer)
Receive a remote SRT stream in listener mode and output uncompressed baseband video/audio directly to a DeckLink SDI monitor:
```bash
./ixgIPStream \
    -i "srt://0.0.0.0:9000?mode=listener" \
    -o sdi "DeckLink Duo (2)"
```

### 4. SDI Ingest with SMPTE 75% Color Bars Fallback
Ingest SDI baseband and stream to UDP multicast. If the SDI cable is disconnected or video carrier is lost, the pipeline automatically generates and encodes SMPTE 75% color bars:
```bash
./ixgIPStream \
    -i "sdi:DeckLink Duo (3)" \
    -o udp "udp://226.1.1.1:9000" \
    --vcodec h264 \
    --sdi-test-bars
```

### 5. Multicast UDP to YouTube Live (RTMP) with Confidence Monitor
Zero-encoding passthrough from an MPEG-TS multicast feed to YouTube Live, updating an atomic 5 FPS JPEG snapshot preview for a web UI:
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o rtmp "rtmp://a.rtmp.youtube.com/live2/<YOUR_STREAM_KEY>" \
    --monitor-file "/var/www/html/live_preview.jpg" \
    --monitor-fps 5 \
    --monitor-scale 640x360 \
    --detailed
```

### 6. RTP Ingress (RFC 2250) to Multi-Protocol Fan-Out
Ingest an RTP stream with dynamic jitter reordering and distribute concurrently to YouTube (RTMP), a multi-client SRT listener, and a secondary RTP multicast destination:
```bash
./ixgIPStream \
    -i "rtp://@239.255.0.1:5004" \
    -o rtmp "rtmp://a.rtmp.youtube.com/live2/<KEY>" \
    -o srt "srt://0.0.0.0:9000?mode=listener" \
    -o rtp "rtp://239.255.0.3:5004"
```

### 7. Dual SRT Targets (Local Broadcast Server + Remote Cloud Gateway)
Simultaneously run a local SRT broadcast server while pushing an upstream caller connection to a cloud ingress endpoint:
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o srt "srt://0.0.0.0:9000?mode=listener" \
    -o srt "srt://cloud-egress.cdn.com:9001?mode=caller"
```

### 8. Enumerate Installed Blackmagic DeckLink Devices
List all physical cards, detected video carrier modes, and audio channels:
```bash
./ixgIPStream --list-sdi
```

---

## Real-Time Telemetry & Transport Analytics

`ixgIPStream` provides four distinct telemetry visualization modes:

### 1. Multi-Line Status Dashboard (`--detailed`)
Outputs dedicated transport metrics cards for each active ingress and egress protocol every second:
```text
┌─── [ixgIPStream v4.2.0 Telemetry Dashboard] ──────────────────────────────────────────┐
│  Uptime: 00:02:45 | Source Ingress: 4250.2 kbps | 25.0 fps | A/V Drift: +4ms          │
├───────────────────────────────────────────────────────────────────────────────────────┤
│  [UDP Ingress]   : 4250.2 kbps | 352.1 pkts/s | Loss: 0.00% (0 dropped, 0 CC errs)    │
│  [RTMP Egress #1]: 4210.0 kbps | 25.0 fps (V) | Latency: 0.04ms (Peak: 0.22ms)        │
│    ├─ Target       : rtmp://a.rtmp.youtube.com/live2/xxxx                             │
│    ├─ TCP Uplink   : Retrans: 0 | Lost: 0 | RTT: 18.2ms (Jitter: 1.4ms)               │
│    └─ Health       : HEALTHY (GOP: 1.2s, Speed: 1.00x)                                │
│  [SRT Egress #2] : 4210.0 kbps | Clients: 3 | Latency: 0.03ms                         │
│    ├─ Target       : srt://0.0.0.0:9000?mode=listener (Multi-Client Server)           │
│    ├─ SRT Transport: RTT: 12.1ms | Loss/s: 0 | Retrans/s: 0 | NAK/s: 0                │
│    └─ ARQ Health   : Success: 100.0% | Bandwidth: 142.5 Mbps | Buffer: 18ms           │
│  [Monitor Egress]: /tmp/monitor.jpg (5 fps, 640x360, Q:4, Frame: #825)                │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

### 2. Standard Timestamped Logs (`--log-stats`)
Ideal for piping to `tee`, storing in log files, or streaming to `systemd` / centralized log collectors:
```text
2026-10-27 18:00:01 [INFO] UDP: 4250.2 kbps (Loss: 0.00%, 0 pkts) | RTMP: 4210.0 kbps (25.0 fps, Retrans: 0, RTT: 18.2ms) | Lat: 0.02/0.04/0.22ms | Silence(V/A): 0/20ms | Speed: 1.00x | Drift: +4ms | GOP: 1.2s | YouTube: HEALTHY | Out: 32.5 MB
```

### 3. Streaming JSON Telemetry (`--json`)
Emits a structured JSON object to stdout every second for Grafana, Prometheus, or real-time web agents:
```json
{"uptime_s":165,"input":{"type":"udp","bitrate_kbps":4250.2,"loss_pct":0.0,"cc_errors":0},"outputs":[{"type":"rtmp","url":"rtmp://127.0.0.1:1935/live","bitrate_kbps":4210.0,"fps":25.0,"tcp_retrans":0,"rtt_ms":18.2,"write_lat_avg_ms":0.04},{"type":"srt","url":"srt://0.0.0.0:9000","clients":3,"rtt_ms":12.1,"arq_pct":100.0}]}
```

---

## YouTube Buffering Root-Cause Diagnostics

The pipeline actively monitors streaming health metrics and prints actionable warnings when conditions that could cause playback buffering are detected:

| YouTube Buffering Root Cause | Pipeline Diagnostic Alert in Log |
| :--- | :--- |
| **Underfeeding (Delivery Speed < 1.0x)** | `[BUFFER RISK] Underfeeding RTMP! Media delivery speed is X.X (< 1.0x). YouTube player buffer is draining!` |
| **Track Starvation (Video Frozen / Audio-only)** | `[BUFFER RISK] Video track stalled for X ms while audio continues! YouTube player will freeze.` |
| **GOP / Keyframe interval > 4.0s** | `[BUFFER RISK] Keyframe interval is X.Xs (> 4.0s). YouTube recommends 2-4s keyframes!` |
| **Waiting for Keyframe (In-flight Delay)** | `[BUFFER RISK] No video keyframe received for X.Xs! YouTube is waiting for an IDR keyframe to segment chunks.` |
| **Source Packet Starvation (FPS < 15)** | `[BUFFER RISK] Video rate dropped to X.X fps! YouTube will buffer due to packet starvation.` |
| **Multicast UDP Packet Loss (Gaps)** | `[BUFFER RISK] Video timestamp gap of X ms detected! Multicast UDP packet loss likely.` |
| **Uplink Internet Congestion** | `[BUFFER RISK] RTMP socket write blocked for X ms! Uplink internet bandwidth congestion detected.` |
| **Severe A/V Timestamp Drift** | `[BUFFER RISK] Excessive A/V sync drift: +X ms. Player desync may cause buffering!` |
| **Multicast TS Packet Loss (CC Drops)** | `[BUFFER RISK] UDP Multicast loss: X TS packets dropped in last second (Loss: X.X%)!` |
| **SRT Ingress / Egress Packet Loss** | `[BUFFER RISK] SRT Ingress/Egress loss: X packets lost in last second (Drops: X, ARQ: X.X%)!` |
| **RTMP Uplink TCP Loss** | `[BUFFER RISK] RTMP uplink TCP loss: X packets lost in transit to YouTube!` |
| **HLS Uplink TCP Loss** | `[BUFFER RISK] HLS uplink TCP loss: X packets lost in transit to CDN/origin!` |

---

## Verification & Automated Test Suites

The repository contains automated test suites located in `scripts/`:

### 1. End-to-End Multicast to RTMP Test
Spawns a local RTMP server, starts a synthetic broadcaster, executes `ixgIPStream`, and verifies packets and trailer finalization:
```bash
./scripts/test_end_to_end.sh
```

### 2. RTP RFC 2250 Ingress & Egress Test
Verifies RTP packet generation, payload compliance (PT=33, 1316 bytes MTU), PAT/PMT table insertion, and multi-protocol fan-out:
```bash
./scripts/test_rtp.sh
```

### 3. Multi-Client SRT Listener Test
Launches the pipeline as an SRT broadcast server, connects 4 concurrent receivers, verifies PAT/PMT caching, and tests graceful disconnections:
```bash
./scripts/test_srt_multiclient.sh
```

### 4. SRT ARQ QoS & Telemetry Test
Validates `libsrt` bstats metrics gathering, RTT tracking, and NAK/retransmission tracking:
```bash
./scripts/test_srt_telemetry.sh
```

---

## Linux System & Network Tuning

For reliable high-bitrate multicast and low-latency streaming:

### 1. Increase Kernel Socket Receive Buffer
```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.rmem_default=33554432
```

### 2. Disable Strict Reverse Path Filtering (Multicast Multi-NIC)
On Linux systems with multiple network interfaces, disable strict RP filtering to prevent the kernel from dropping IGMP multicast datagrams:
```bash
sudo sysctl -w net.ipv4.conf.all.rp_filter=2
sudo sysctl -w net.ipv4.conf.default.rp_filter=2
```

---

## Documentation & Telemetry Guides

For detailed protocol deep dives, metric explanations, and architectural diagrams:
- **[ixgIPStream Telemetry and Log Guide (PDF)](file:///home/nityanand-pc/Desktop/ixgIPStreaming/ixgIPStream_Telemetry_and_Log_Guide.pdf)**: Complete reference for terminal telemetry cards, packet loss equations, and transport diagnostic alerts.
- **[PDF Generator Script](file:///home/nityanand-pc/Desktop/ixgIPStreaming/scripts/generate_pdf_guide.py)**: Python ReportLab script used to compile the PDF guide.
