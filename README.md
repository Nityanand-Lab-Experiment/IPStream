# FFmpeg Multicast to RTMP Streaming Pipeline (Zero-Encoding)

A high-performance, production-ready C streaming pipeline that ingests a live **UDP Multicast (MPEG-TS)** stream and forwards it to an **RTMP server** (e.g., SRS, NGINX-RTMP, YouTube Live, Twitch, Wowza) using the native FFmpeg libraries (`libavformat`, `libavcodec`, `libavutil`) **without decoding or re-encoding** (Remuxing / Stream Copy).

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

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libavformat-dev libavcodec-dev libavutil-dev
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

Binary `multicast2rtmp` will be created in the current directory.

---

## Usage

```bash
./multicast2rtmp [OPTIONS]
```

### Command Line Options

| Flag | Long Option | Default | Description |
|------|-------------|---------|-------------|
| `-i` | `--input` | `udp://@239.255.0.1:1234` | Multicast UDP input URL (MPEG-TS) |
| `-o` | `--output` | `rtmp://127.0.0.1:1935/live/stream` | Destination RTMP URL |
| `-a` | `--local-addr` | `""` | Local network interface IP to bind for IGMP join |
| `-f` | `--fifo-size` | `50` | UDP FIFO queue size in Megabytes |
| `-b` | `--buffer-size`| `16` | Kernel UDP socket receive buffer in Megabytes |
| `-t` | `--timeout` | `10` | Inactivity read timeout in seconds |
| `-k` | `--no-wait-keyframe` | `false` | Start streaming immediately without waiting for keyframe |
| `-n` | `--no-normalize-pts` | `false` | Disable offsetting PTS/DTS to 0 |
| `-s` | `--stats-interval` | `1000` | Telemetry refresh interval in milliseconds |
| `-v` | `--verbose` | `false` | Enable detailed debug logs |
| `-h` | `--help` | — | Display help menu |

---

## Examples

### 1. Typical Production Multicast to RTMP
```bash
./multicast2rtmp \
    -i "udp://@239.255.10.50:5000" \
    -a "192.168.1.100" \
    -o "rtmp://rtmp.mycdn.com/live/my_stream_key"
```

### 2. Streaming to YouTube Live
```bash
./multicast2rtmp \
    -i "udp://@239.255.0.1:1234" \
    -o "rtmp://a.rtmp.youtube.com/live2/<YOUR_STREAM_KEY>"
```

### 3. High-Bitrate 4K Stream with Sized Buffers
```bash
./multicast2rtmp \
    -i "udp://@239.255.0.1:1234" \
    -f 100 \
    -b 32 \
    -o "rtmp://10.0.0.20:1935/live/4k_feed"
```

---

## Quick Start: Testing Locally (End-to-End)

An automated verification test script is included that starts a local RTMP server, generates a synthetic multicast stream, remuxes the stream with `multicast2rtmp`, and verifies the output:

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
./multicast2rtmp -i udp://127.0.0.1:1234 -o rtmp://127.0.0.1:1935/live/stream
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
