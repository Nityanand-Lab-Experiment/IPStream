# ixgIPStream (v4.1) - Multicast to RTMP / HLS Streaming Pipeline (Zero-Encoding)

A high-performance, production-ready C streaming pipeline that ingests a live **UDP Multicast (MPEG-TS)** stream and forwards it to **RTMP servers** and/or **HLS CDNs** using native FFmpeg libraries (`libavformat`, `libavcodec`, `libavutil`) **without decoding or re-encoding** (Remuxing / Stream Copy).

---

## Key Highlights & Features

- **Zero-Encoding / Passthrough Remuxing**:
  Direct packet copy from MPEG-TS into FLV/RTMP (`av_read_frame` -> `av_packet_rescale_ts` -> `av_interleaved_write_frame`). Preserves 100% of the original video/audio quality with near-zero CPU and memory usage.
- **Keyframe Synchronization**:
  Gating mechanism discards intermediate B/P-frames and audio packets until the first video IDR Keyframe is received. This guarantees that connected players start playback immediately with a clean, glitch-free image.
- **Timestamp Normalization & Monotonicity Protection**:
  Live multicast feeds often carry arbitrary, multi-hour PCR/PTS values. This pipeline offsets timestamps to start cleanly from `0.000s` and strictly enforces monotonic non-decreasing DTS (`dts <= pts`), preventing RTMP server rejects.
- **Multicast UDP Buffer Management**:
  Includes configurable internal FIFO queues and OS socket receive buffer sizing (`buffer_size`, `fifo_size`, `overrun_nonfatal`) to eliminate packet drop caused by network jitter.
- **Network Timeout & Graceful Shutdown**:
  Monitors stream activity via an `AVIOInterruptCB` callback. Catches `SIGINT`/`SIGTERM` to cleanly write the FLV trailer and close RTMP sessions without corrupting the server state.
- **Live Terminal Telemetry**:
  Interactive, real-time status output displaying uptime, current FPS, instantaneous bitrate (kbps), audio/video packet counters, and total transferred data.

---

## Architecture Flow

```
┌───────────────────────────────────────┐
│ Live Multicast Source                 │
│ (udp://@239.255.0.1:1234, MPEG-TS)    │
└──────────────────┬────────────────────┘
                   │
                   ▼ [libavformat: UDP Demuxer]
   ┌─────────────────────────────────────────┐
   │ - Kernel Socket Buffer Tuning (SO_RCV) │
   │ - FIFO Queue (overrun_nonfatal=1)       │
   │ - Stream Probing & Codec Parameter Copy │
   └───────────────┬─────────────────────────┘
                   │
                   ▼ [Zero-Encoding Packet Pipeline]
   ┌─────────────────────────────────────────┐
   │ 1. Filter: Audio & Video streams only   │
   │ 2. Gate: Wait for first Video Keyframe  │
   │ 3. Normalize: Offset PTS/DTS to 0.0s    │
   │ 4. Enforce: Monotonic non-decreasing DTS│
   │ 5. Rescale: 90kHz TS -> 1ms FLV timebase│
   └───────────────┬─────────────────────────┘
                   │
                   ▼ [libavformat: FLV/RTMP Muxer]
┌───────────────────────────────────────┐
│ Destination RTMP Server               │
│ (rtmp://server:1935/live/stream)      │
└───────────────────────────────────────┘
```

---

## Project Structure

```
.
├── Makefile                     # Build targets (all, clean, test)
├── CMakeLists.txt               # CMake configuration
├── README.md                    # Documentation
├── src/
│   ├── main.c                   # CLI argument parsing, banner, entry point
│   ├── pipeline.c               # Core zero-encoding remuxing engine
│   ├── pipeline.h               # Configuration structs & public API
│   ├── utils.c                  # Logging, signals, formatting, timers
│   └── utils.h                  # Logging macros & utility declarations
└── scripts/
    ├── simulate_multicast.sh    # Generates synthetic MPEG-TS multicast feed
    ├── simulate_rtmp_server.sh  # Lightweight local RTMP receiver
    └── test_end_to_end.sh       # Automated integration test suite
```

---

## Prerequisites

- **GCC** or **Clang** with C11 support
- **pkg-config**
- **FFmpeg libraries (version 4.x, 5.x, 6.x, or 7.x)**:
  - `libavformat-dev`
  - `libavcodec-dev`
  - `libavutil-dev`
  - `libswscale-dev`

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

---

## Building the Pipeline

### Option 1: Using Makefile
```bash
make
```

### Option 2: Using CMake
```bash
mkdir -p build && cd build
cmake ..
make
```

Binary `ixgIPStream` will be created in the current directory.

---

## Usage

```bash
./ixgIPStream [OPTIONS]
```

### Command Line Options

| Flag | Long Option | Default | Description |
|------|-------------|---------|-------------|
| `-i` | `--input <url>` | `udp://@239.255.0.1:1234` | Multicast UDP input URL (MPEG-TS) |
| `-o` | `--output <type> <url>` | `rtmp://127.0.0.1:1935/live/stream` | Destination output. Type: `rtmp` or `hls`. Can be repeated for multiple outputs. |
| `-a` | `--local-addr <ip>` | `""` | Local network interface IP to bind for IGMP join |
| `-f` | `--fifo-size <mb>` | `50` | UDP FIFO queue size in Megabytes |
| `-b` | `--buffer-size <mb>`| `16` | Kernel UDP socket receive buffer in Megabytes |
| `-t` | `--timeout <sec>` | `10` | Inactivity read timeout in seconds |
| `-k` | `--no-wait-keyframe` | `false` | Start streaming immediately without waiting for keyframe |
| `-n` | `--no-normalize-pts` | `false` | Disable offsetting PTS/DTS to 0 |
| `-s` | `--stats-interval <ms>`| `1000` | Telemetry refresh interval in milliseconds |
| `-l` | `--log-stats` | `false` | Output telemetry as timestamped standard LOG lines |
| `-d` | `--detailed` | `false` | Output multi-line telemetry cards each second |
| `-j` | `--json` | `false` | Output 1-second JSON telemetry stream to stdout |
| `-v` | `--verbose` | `false` | Enable detailed debug logs |
| `-V` | `--version` | — | Display version information (`v4.1`) |
| `-h` | `--help` | — | Display help menu |

### Output Target Configurations

The pipeline dynamically tailors telemetry based on configured outputs:

1. **Option 1: Only RTMP**
   ```bash
   ./ixgIPStream -i udp://@239.255.0.1:1234 -o rtmp rtmp://127.0.0.1:1935/live/stream
   ```
   *Matrix: Displays only UDP ingress and RTMP egress metrics (FPS, bitrate, TCP retrans, RTT, write latency, YouTube health). HLS metrics are omitted.*

2. **Option 2: Only HLS**
   ```bash
   ./ixgIPStream -i udp://@239.255.0.1:1234 -o hls http://origin.cdn.com/live/stream.m3u8
   ```
   *Matrix: Displays only UDP ingress and HLS egress metrics (bitrate, pps, active segment, upload speed factor, TCP retrans, RTT, origin health). RTMP metrics and YouTube health are omitted.*

3. **Option 3: Dual Output (RTMP + HLS)**
   ```bash
   ./ixgIPStream -i udp://@239.255.0.1:1234 -o rtmp rtmp://127.0.0.1:1935/live/stream -o hls http://origin/live.m3u8
   ```
   *Matrix: Displays comprehensive telemetry for both RTMP and HLS streams concurrently.*

4. **Multiple Concurrent Outputs (e.g. 2 RTMP + 2 HLS)**
   ```bash
   ./ixgIPStream -i udp://@239.255.0.1:1234 \
       -o rtmp rtmp://yt-primary/live/key \
       -o rtmp rtmp://yt-backup/live/key \
       -o hls http://cdn1/live/stream.m3u8 \
       -o hls /var/www/html/live/stream.m3u8
   ```
   *Matrix: Fan-out distribution with independent per-egress metrics (`[RTMP #1]`, `[RTMP #2]`, `[HLS #1]`, `[HLS #2]`).*

### HLS Configuration Options

| Long Option | Default | Description |
|-------------|---------|-------------|
| `--hls <url>` | `""` | HLS destination playlist URL or path (alias for `-o hls <url>`) |
| `--hls-method <method>` | `PUT` | HTTP method for remote HLS upload (`PUT` or `POST`) |
| `--hls-time <sec>` | `2` | Target segment duration in seconds |
| `--hls-list-size <num>` | `5` | Maximum number of segments kept in the live playlist |
| `--hls-segment <pattern>` | auto | Custom segment naming pattern (e.g. `http://origin/live/stream_%05d.ts`) |
| `--hls-fmp4` | `false` | Use Fragmented MP4 (`.m4s`) chunks instead of MPEG-TS (`.ts`) |
| `--hls-no-delete` | `false` | Do not delete expired segments from disk |

#### HLS Telemetry Matrix (`--detailed` Mode)
When HLS output is enabled, the pipeline outputs real-time segment and CDN metrics:
```text
│  [HLS Egress]
│    ├─ Target           : http://origin.cdn.com/live/stream.m3u8 (Method: PUT)
│    ├─ Data Egress      : 2450.0 kbps | 110.5 pkts/s | Total: 48.2 MB
│    ├─ Active Segment   : stream_00024.ts (Size: 612.4 KB, Duration: 2.00s)
│    ├─ Segments Pushed  : 24 completed | Window: 5 segments in playlist
│    ├─ CDN Upload Speed : 85.2ms per segment (23.5x real-time margin)
│    ├─ TCP Uplink (CDN) : Retrans: 0 lost/resend | In-Flight Lost: 0 | RTT:  18.5ms (Jitter:  2.1ms)
│    └─ Origin Health    : HTTP 200 OK (0 errors, 0 retries)
```

### Confidence Monitor Options (Real-Time Snapshot Preview)

| Long Option | Default | Description |
|-------------|---------|-------------|
| `--monitor-file <path>` | `""` | Output path for JPEG confidence monitor (e.g. `/tmp/monitor.jpg`) |
| `--monitor-fps <num>` | `5` | Capture rate in FPS (default: 5 frames/sec) |
| `--monitor-scale <WxH>` | auto | Output resolution (e.g. `640x360`, default: source resolution) |
| `--monitor-quality <1-31>` | `4` | MJPEG quantization factor (`1`=highest quality, `31`=lowest) |

---

## RTMP Connection Status & YouTube Buffering Diagnostics

The pipeline actively monitors both network layers and prints warnings with actionable reasons when YouTube buffering conditions are detected:

| YouTube Buffering Root Cause | Pipeline Diagnostic Alert in Log |
| :--- | :--- |
| **Underfeeding (Delivery Speed < 1.0x)** | `[BUFFER RISK] Underfeeding RTMP! Media delivery speed is X.X (< 1.0x). YouTube player buffer is draining!` |
| **Track Starvation (Video Frozen / Audio-only)** | `[BUFFER RISK] Video track stalled for X ms while audio continues! YouTube player will freeze.` |
| **GOP / Keyframe interval > 4.0s** | `[BUFFER RISK] Keyframe interval is X.Xs (> 4.0s). YouTube recommends 2-4s keyframes!` |
| **Waiting for Keyframe (In-flight Delay)** | `[BUFFER RISK] No video keyframe received for X.Xs! YouTube is waiting for an IDR keyframe to segment chunks.` |
| **Source Packet Starvation (FPS < 15)**| `[BUFFER RISK] Video rate dropped to X.X fps! YouTube will buffer due to packet starvation.` |
| **Multicast UDP Packet Loss (Gaps)** | `[BUFFER RISK] Video timestamp gap of X ms detected! Multicast UDP packet loss likely.` |
| **Uplink Internet Congestion** | `[BUFFER RISK] RTMP socket write blocked for X ms! Uplink internet bandwidth congestion detected.` |
| **Severe A/V Timestamp Drift** | `[BUFFER RISK] Excessive A/V sync drift: +X ms. Player desync may cause buffering!` |
| **Multicast Packet Loss (CC Drops)** | `[BUFFER RISK] UDP Multicast loss: X TS packets dropped in last second (Loss: X.X%)!` |
| **RTMP Uplink TCP Loss** | `[BUFFER RISK] RTMP uplink TCP loss: X packets lost in transit to YouTube!` |
| **HLS Uplink TCP Loss** | `[BUFFER RISK] HLS uplink TCP loss: X packets lost in transit to CDN/origin!` |

---

## Packet Loss Monitoring (UDP Multicast, RTMP TCP & HLS TCP)

The pipeline features multi-layer transport packet loss tracking:

1. **UDP Multicast Packet Loss (Application Layer - MPEG-TS)**:
   - Tracks 4-bit MPEG-TS Continuity Counter (CC) sequence jumps.
   - Calculates exact dropped TS packet counts (`udp_lost_packets`), discontinuity error events (`udp_cc_errors`), and instantaneous/lifetime packet loss percentages.
2. **RTMP & HLS Uplink Loss, RTT & Jitter (Transport Layer - Linux `TCP_INFO`)**:
   - Discovers active TCP sockets from `/proc/self/fd`.
   - Queries the Linux kernel TCP stack for exact TCP retransmissions (`tcpi_total_retrans`), in-flight lost packets (`tcpi_lost`), Round-Trip Time (`tcpi_rtt`), and latency Jitter variance (`tcpi_rttvar`) to the remote ingest and CDN origin.
   - Handles HTTP connection churn across chunk uploads seamlessly, preserving monotonic retransmit counts across the entire broadcast.

### Logging Telemetry to Files or Systemd
To write timestamped console logs suitable for `tee`, log files, or `systemd`:
```bash
./ixgIPStream -i "udp://@239.255.0.1:1234" -o "rtmp://a.rtmp.youtube.com/live2/XXXX" --log-stats | tee -a stream.log
```

**Sample `--log-stats` Line Output:**
```text
2026-10-01 14:10:01 [INFO]  UDP: 264.1 kbps (Loss: 0.00%, 0 pkts) | RTMP: 264.1 kbps (25.0 fps, Retrans: 0, RTT: 0.0ms) | Lat: 0.02/0.04/0.21ms | Silence(V/A): 0/239ms | Speed: 1.00x | Drift: +68ms | GOP: 1.0s | YouTube: HEALTHY | Out: 83.06 KB
```

- **`Lat: min/avg/max ms`**: Socket write latency per packet interval. Values > 400ms indicate uplink network congestion or socket buffer stall.
- **`Silence(V/A): V_ms/A_ms`**: Elapsed time in milliseconds since the last packet write for Video and Audio tracks. Detects asymmetric stream stalls (e.g. video freezing while audio continues).


---

## Examples

### 1. Typical Production Multicast to RTMP
```bash
./ixgIPStream \
    -i "udp://@239.255.10.50:5000" \
    -a "192.168.1.100" \
    -o rtmp "rtmp://rtmp.mycdn.com/live/my_stream_key"
```

### 2. Streaming to YouTube Live (Only RTMP)
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o rtmp "rtmp://a.rtmp.youtube.com/live2/<YOUR_STREAM_KEY>"
```

### 3. Streaming Only HLS to CDN
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o hls "http://origin.cdn.com/live/stream.m3u8" \
    --hls-method PUT \
    --hls-time 2
```

### 4. Simultaneous Dual Output (RTMP + HLS)
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o rtmp "rtmp://a.rtmp.youtube.com/live2/<STREAM_KEY>" \
    -o hls "/var/www/html/live/stream.m3u8" \
    --hls-time 2 \
    --hls-list-size 5
```

### 5. Multi-Egress Fan-Out (2 RTMP + 2 HLS)
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o rtmp "rtmp://primary.youtube.com/live2/<KEY1>" \
    -o rtmp "rtmp://backup.youtube.com/live2/<KEY2>" \
    -o hls "http://cdn1.origin.com/live/stream.m3u8" \
    -o hls "http://cdn2.origin.com/live/stream.m3u8" \
    --log-stats
```

### 6. Streaming with 5 FPS Confidence Monitor
Outputs RTMP and atomically updates a JPEG image 5 times per second for web dashboards or multiviewer displays:
```bash
./ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o "rtmp://a.rtmp.youtube.com/live2/<STREAM_KEY>" \
    --monitor-file "/var/www/html/monitor/live_preview.jpg" \
    --monitor-fps 5 \
    --monitor-scale 640x360 \
    --log-stats
```
*Note*: The file write is **atomic** (writes to `.tmp` then executes a POSIX `rename()`), ensuring web browsers and monitoring tools never read partial or corrupted JPEG frames.

---

## Quick Start: Testing Locally (End-to-End)

An automated verification test script is included that starts a local RTMP server, generates a synthetic multicast stream, remuxes the stream with `ixgIPStream`, and verifies the output:

```bash
./scripts/test_end_to_end.sh
```

### Manual Testing with 3 Terminals:

#### Terminal 1: Start Local RTMP Receiver
```bash
./scripts/simulate_rtmp_server.sh rtmp://127.0.0.1:1935/live/stream
```

#### Terminal 2: Start Synthetic Multicast Broadcaster
```bash
./scripts/simulate_multicast.sh udp://127.0.0.1:1234
```

#### Terminal 3: Run the Pipeline
```bash
./ixgIPStream -i udp://127.0.0.1:1234 -o rtmp://127.0.0.1:1935/live/stream
```

You can view the live stream in VLC or ffplay:
```bash
ffplay rtmp://127.0.0.1:1935/live/stream
```

---

## Network & System Tuning

For reliable reception of high-bitrate multicast streams over Linux networks:

1. **Increase Linux Maximum Socket Receive Buffer**:
   ```bash
   sudo sysctl -w net.core.rmem_max=67108864
   sudo sysctl -w net.core.rmem_default=33554432
   ```
2. **Multicast Reverse Path Filtering (rp_filter)**:
   In multi-NIC Linux systems, IGMP multicast packets might be dropped if rp_filter is strict:
   ```bash
   sudo sysctl -w net.ipv4.conf.all.rp_filter=2
   sudo sysctl -w net.ipv4.conf.default.rp_filter=2
   ```

---

## Codec Compatibility

Because this pipeline operates in **zero-encoding passthrough mode** (direct remuxing):
- **Video**: Source must be **H.264 (AVC)** (or HEVC/H.265 if streaming to Enhanced RTMP servers).
- **Audio**: Source must be **AAC** or **MP3**.

*Note*: Broadcast feeds carrying legacy MPEG-2 Video or AC-3 audio cannot be packaged into a standard Flash Video (FLV) RTMP container without transcoding. The pipeline automatically inspects codecs at startup and logs an alert if an unsupported codec is detected.
