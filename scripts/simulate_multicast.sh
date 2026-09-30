#!/usr/bin/env bash
# simulate_multicast.sh
# Broadcasts a synthetic H.264/AAC MPEG-TS stream over UDP to simulate a live multicast feed.

set -euo pipefail

DEST_URL="${1:-udp://127.0.0.1:1234}"
BITRATE="${2:-2500k}"
RESOLUTION="${3:-1280x720}"
FPS="${4:-30}"

echo "================================================================="
echo "   Multicast Stream Simulator (H.264 + AAC over MPEG-TS/UDP)    "
echo "================================================================="
echo "Destination : ${DEST_URL}"
echo "Resolution  : ${RESOLUTION} @ ${FPS} fps"
echo "Bitrate     : ${BITRATE}"
echo "Press Ctrl+C to stop broadcasting."
echo "================================================================="

ffmpeg -re -f lavfi \
    -i "testsrc=size=${RESOLUTION}:rate=${FPS}" \
    -f lavfi \
    -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset veryfast -g 30 -keyint_min 30 -b:v "${BITRATE}" -maxrate "${BITRATE}" -bufsize "${BITRATE}" -pix_fmt yuv420p \
    -c:a aac -b:a 128k -ar 44100 -ac 2 \
    -f mpegts \
    -mpegts_flags +initial_discontinuity \
    "${DEST_URL}?pkt_size=1316&buffer_size=6553600"
