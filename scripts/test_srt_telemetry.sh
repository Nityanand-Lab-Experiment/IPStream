#!/usr/bin/env bash
# test_srt_telemetry.sh
# End-to-end automated verification of SRT transport telemetry & confidence monitor.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/ixgIPStream"

if [ ! -f "$BIN" ]; then
    echo "Building ixgIPStream..."
    make -C "$ROOT_DIR"
fi

TEST_UDP="udp://127.0.0.1:12346"
SRT_EGRESS="srt://127.0.0.1:19390?mode=caller"
SRT_LISTEN="srt://127.0.0.1:19390?mode=listener"
MONITOR_FILE="/tmp/test_srt_monitor.jpg"
RECORD_OUT="/tmp/test_srt_record.ts"
rm -f "$RECORD_OUT" "$MONITOR_FILE" /tmp/srt_receiver.log /tmp/srt_pipeline.log /tmp/srt_sim.log

echo "=========================================================="
echo "=== TEST 1: UDP Multicast Ingress -> SRT Egress QoS    ==="
echo "=========================================================="

echo "[1/4] Starting SRT Receiver (listener mode) on port 19390..."
ffmpeg -v info -i "$SRT_LISTEN" -c copy -y "$RECORD_OUT" > /tmp/srt_receiver.log 2>&1 &
SRT_RCV_PID=$!
sleep 1

cleanup() {
    echo "Cleaning up processes..."
    kill -INT "${PIPELINE_PID:-}" 2>/dev/null || true
    kill "${SIM_PID:-}" 2>/dev/null || true
    kill "${SRT_RCV_PID:-}" 2>/dev/null || true
    kill "${SRT_IN_PID:-}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

echo "[2/4] Starting Synthetic Multicast Broadcaster on $TEST_UDP..."
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 800k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f mpegts "$TEST_UDP?pkt_size=1316" > /tmp/srt_sim.log 2>&1 &
SIM_PID=$!
sleep 1

echo "[3/4] Running ixgIPStream with SRT Egress + Confidence Monitor + Detailed Telemetry for 5s..."
"$BIN" -i "$TEST_UDP" -o srt "$SRT_EGRESS" -d --monitor-file "$MONITOR_FILE" --monitor-fps 2 > /tmp/srt_pipeline.log 2>&1 &
PIPELINE_PID=$!

sleep 5

echo "[4/4] Stopping Pipeline..."
kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true
sleep 1
kill -INT "$SRT_RCV_PID" 2>/dev/null || true
wait "$SRT_RCV_PID" 2>/dev/null || true

echo "--- Checking Detailed Telemetry Log for SRT Transport Metrics ---"
grep -E "SRT Transport|ARQ Health|Confidence Mon|SRT socket" /tmp/srt_pipeline.log || {
    echo "ERROR: Missing SRT transport metrics in detailed log!"
    cat /tmp/srt_pipeline.log
    exit 1
}

if [ -s "$MONITOR_FILE" ]; then
    echo "SUCCESS: Confidence monitor JPEG generated: $(ls -lh "$MONITOR_FILE")"
else
    echo "ERROR: Confidence monitor file not generated!"
    cat /tmp/srt_pipeline.log
    exit 1
fi

if [ -s "$RECORD_OUT" ]; then
    echo "SUCCESS: Recorded SRT MPEG-TS stream generated: $(ls -lh "$RECORD_OUT")"
else
    echo "ERROR: Recorded SRT output empty!"
    cat /tmp/srt_receiver.log
    exit 1
fi

echo "=========================================================="
echo "=== TEST 2: UDP -> SRT Egress with JSON Telemetry Stream ==="
echo "=========================================================="

rm -f /tmp/srt_json.log
ffmpeg -v info -i "$SRT_LISTEN" -c copy -y /tmp/test_srt_record_json.ts > /dev/null 2>&1 &
SRT_RCV_PID=$!
sleep 1

"$BIN" -i "$TEST_UDP" -o srt "$SRT_EGRESS" -j --monitor-file "$MONITOR_FILE" > /tmp/srt_json.log 2>&1 &
PIPELINE_PID=$!
sleep 4

kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true
kill -INT "$SRT_RCV_PID" 2>/dev/null || true
wait "$SRT_RCV_PID" 2>/dev/null || true

echo "--- Checking JSON Telemetry for srt_transport and confidence_monitor ---"
grep "srt_transport" /tmp/srt_json.log | head -n 2
grep "confidence_monitor" /tmp/srt_json.log | head -n 1

echo "=========================================================="
echo "=== TEST 3: SRT Ingress -> SRT Egress (SRT-to-SRT)     ==="
echo "=========================================================="

SRT_IN_PORT="19391"
SRT_IN_LISTEN="srt://127.0.0.1:$SRT_IN_PORT?mode=listener"
SRT_IN_CALLER="srt://127.0.0.1:$SRT_IN_PORT?mode=caller"
SRT_OUT_PORT="19392"
SRT_OUT_LISTEN="srt://127.0.0.1:$SRT_OUT_PORT?mode=listener"
SRT_OUT_CALLER="srt://127.0.0.1:$SRT_OUT_PORT?mode=caller"

echo "[1/4] Starting SRT Out Receiver on port $SRT_OUT_PORT..."
ffmpeg -v info -i "$SRT_OUT_LISTEN" -c copy -y /tmp/test_srt2srt.ts > /dev/null 2>&1 &
SRT_RCV_PID=$!
sleep 1

echo "[2/4] Starting SRT In Source (feeder) on port $SRT_IN_PORT (listener)..."
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 800k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f mpegts "$SRT_IN_LISTEN" > /dev/null 2>&1 &
SRT_IN_PID=$!
sleep 2

echo "[3/4] Running ixgIPStream in SRT-to-SRT mode with JSON telemetry..."
rm -f /tmp/srt2srt_json.log
"$BIN" -i "$SRT_IN_CALLER" -o srt "$SRT_OUT_CALLER" -j --monitor-file "$MONITOR_FILE" > /tmp/srt2srt_json.log 2>&1 &
PIPELINE_PID=$!
sleep 4

kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true
kill -INT "$SRT_RCV_PID" 2>/dev/null || true
wait "$SRT_RCV_PID" 2>/dev/null || true
kill -INT "$SRT_IN_PID" 2>/dev/null || true
wait "$SRT_IN_PID" 2>/dev/null || true

echo "--- Checking SRT-to-SRT JSON Telemetry ---"
grep "srt_in" /tmp/srt2srt_json.log | head -n 2
grep "srt_out" /tmp/srt2srt_json.log | head -n 2

echo ""
echo "=========================================================="
echo "=== ALL SRT TELEMETRY & CONFIDENCE MONITOR TESTS PASSED ==="
echo "=========================================================="
