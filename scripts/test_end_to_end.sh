#!/usr/bin/env bash
# test_end_to_end.sh
# End-to-end automated verification of the multicast2rtmp zero-encoding pipeline.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/multicast2rtmp"

if [ ! -f "$BIN" ]; then
    echo "Building multicast2rtmp..."
    make -C "$ROOT_DIR"
fi

TEST_UDP="udp://127.0.0.1:12345"
TEST_RTMP="rtmp://127.0.0.1:19355/live/stream"
RECORD_OUT="/tmp/test_rtmp_record.flv"
rm -f "$RECORD_OUT"

echo "=== [1/4] Starting Local RTMP Receiver on $TEST_RTMP ==="
ffmpeg -v info -listen 1 -f flv -i "$TEST_RTMP" -c copy -y "$RECORD_OUT" > /tmp/rtmp_server.log 2>&1 &
SERVER_PID=$!
sleep 1

cleanup() {
    echo "Cleaning up processes..."
    kill -INT "$PIPELINE_PID" 2>/dev/null || true
    kill "$SIM_PID" 2>/dev/null || true
    kill "$SERVER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

echo "=== [2/4] Starting Synthetic Multicast Broadcaster on $TEST_UDP ==="
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 800k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f mpegts "$TEST_UDP?pkt_size=1316" > /tmp/multicast_sim.log 2>&1 &
SIM_PID=$!
sleep 1

echo "=== [3/4] Running multicast2rtmp Pipeline for 5 seconds ==="
"$BIN" -i "$TEST_UDP" -o "$TEST_RTMP" -f 10 -b 4 -t 5 &
PIPELINE_PID=$!

sleep 5

echo "=== [4/4] Sending Graceful Stop Signal (SIGINT) ==="
kill -INT "$PIPELINE_PID"
wait "$PIPELINE_PID" || true

sleep 1
kill -INT "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

echo "=== Verifying Recorded Stream Output ==="
if [ -s "$RECORD_OUT" ]; then
    echo "SUCCESS: Recorded FLV file generated:"
    ls -lh "$RECORD_OUT"
    ffmpeg -i "$RECORD_OUT" 2>&1 | grep -E "Duration|Stream #" || true
    echo "Zero-encoding Multicast to RTMP pipeline test PASSED!"
else
    echo "ERROR: Output file not created or empty!"
    cat /tmp/rtmp_server.log
    exit 1
fi
