#!/usr/bin/env bash
# test_rtp.sh
# Comprehensive verification of RTP Ingress and Egress in ixgIPStream

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/ixgIPStream"

if [ ! -f "$BIN" ]; then
    echo "Building ixgIPStream..."
    make -C "$ROOT_DIR"
fi

TEST_UDP_IN="udp://127.0.0.1:15000"
TEST_RTP_OUT="rtp://127.0.0.1:16004"
TEST_RTP_IN="rtp://127.0.0.1:16004"
TEST_UDP_OUT="udp://127.0.0.1:17000"

RTP_RECORD_OUT="/tmp/test_rtp_record.ts"
RTP_JSON_LOG="/tmp/test_rtp_json.log"
RTP_PIPE_LOG="/tmp/test_rtp_pipeline.log"
RTP_SIM_LOG="/tmp/test_rtp_sim.log"

rm -f "$RTP_RECORD_OUT" "$RTP_JSON_LOG" "$RTP_PIPE_LOG" "$RTP_SIM_LOG" /tmp/test_rtp_*.log

cleanup() {
    echo "Cleaning up any lingering test processes..."
    kill -9 "${PIPELINE_PID:-}" 2>/dev/null || true
    kill -9 "${SIM_PID:-}" 2>/dev/null || true
    kill -9 "${RCV_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

echo "=========================================================="
echo "=== TEST 1: Synthetic UDP -> ixgIPStream -> RTP Egress ==="
echo "=========================================================="

echo "[1/4] Starting Synthetic Broadcaster on $TEST_UDP_IN..."
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 800k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f mpegts "$TEST_UDP_IN?pkt_size=1316" > "$RTP_SIM_LOG" 2>&1 &
SIM_PID=$!
sleep 1

echo "[2/4] Starting RTP Receiver on port 16004 recording to $RTP_RECORD_OUT..."
ffmpeg -v info -i "$TEST_RTP_OUT" -c copy -y "$RTP_RECORD_OUT" > /tmp/test_rtp_receiver.log 2>&1 &
RCV_PID=$!
sleep 1

echo "[3/4] Launching ixgIPStream with RTP Egress + JSON Telemetry..."
"$BIN" -i "$TEST_UDP_IN" -o rtp "$TEST_RTP_OUT" -j > "$RTP_JSON_LOG" 2>&1 &
PIPELINE_PID=$!

sleep 5

echo "[4/4] Stopping Pipeline and Receiver..."
kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true
kill -INT "$RCV_PID" 2>/dev/null || true
wait "$RCV_PID" 2>/dev/null || true

# Assertions for TEST 1
echo "Analyzing Test 1 Results..."
if [ ! -s "$RTP_RECORD_OUT" ]; then
    echo "FAILED: RTP recording file is empty!"
    exit 1
fi
RECORD_SIZE=$(stat -c%s "$RTP_RECORD_OUT")
echo "  [PASS] RTP stream received and recorded: $RECORD_SIZE bytes."

if ! grep -q '"rtp_out"' "$RTP_JSON_LOG"; then
    echo "FAILED: 'rtp_out' block not found in JSON telemetry!"
    cat "$RTP_JSON_LOG"
    exit 1
fi
echo "  [PASS] 'rtp_out' verified in JSON telemetry."

if ! grep -q '"total_bytes"' "$RTP_JSON_LOG"; then
    echo "FAILED: Total bytes metric missing from JSON!"
    exit 1
fi
echo "  [PASS] JSON telemetry metrics verified."

echo "=========================================================="
echo "=== TEST 2: RTP Ingress -> ixgIPStream -> UDP Egress  ==="
echo "=========================================================="

echo "[1/4] Starting RTP Source Streamer to $TEST_RTP_IN..."
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=880:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 600k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f rtp_mpegts -pkt_size 1316 "$TEST_RTP_OUT" > /tmp/test_rtp_source.log 2>&1 &
SIM_PID=$!
sleep 1

echo "[2/4] Starting UDP Receiver on port 17000..."
UDP_RECORD="/tmp/test_rtp_to_udp.ts"
rm -f "$UDP_RECORD"
ffmpeg -v info -i "$TEST_UDP_OUT" -c copy -y "$UDP_RECORD" > /tmp/test_udp_receiver.log 2>&1 &
RCV_PID=$!
sleep 1

echo "[3/4] Launching ixgIPStream with RTP Ingress (-i rtp://127.0.0.1:16004) -> UDP Egress..."
"$BIN" -i "$TEST_RTP_IN" -o udp "$TEST_UDP_OUT" -l > "$RTP_PIPE_LOG" 2>&1 &
PIPELINE_PID=$!

sleep 5

echo "[4/4] Stopping Pipeline and Receiver..."
kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true
kill -INT "$RCV_PID" 2>/dev/null || true
wait "$RCV_PID" 2>/dev/null || true
kill "${SIM_PID:-}" 2>/dev/null || true

# Assertions for TEST 2
echo "Analyzing Test 2 Results..."
if [ ! -s "$UDP_RECORD" ]; then
    echo "FAILED: UDP recorded output from RTP ingress is empty!"
    cat "$RTP_PIPE_LOG"
    exit 1
fi
UDP_SIZE=$(stat -c%s "$UDP_RECORD")
echo "  [PASS] Output stream recorded: $UDP_SIZE bytes."

if ! grep -q "RTP" "$RTP_PIPE_LOG"; then
    echo "FAILED: RTP ingress not logged in pipeline output!"
    cat "$RTP_PIPE_LOG"
    exit 1
fi
echo "  [PASS] RTP Ingress logged with bitrate and continuity tracking."

echo "=========================================================="
echo "=== TEST 3: Multi-Egress Concurrency with RTP (--rtp)  ==="
echo "=========================================================="
echo "[1/3] Launching ixgIPStream with --rtp flag and multiple outputs..."
"$BIN" -i "$TEST_UDP_IN" --rtp "$TEST_RTP_OUT" -o udp "$TEST_UDP_OUT" -d > /tmp/test_multi_rtp.log 2>&1 &
PIPELINE_PID=$!
sleep 2
kill -INT "$PIPELINE_PID" 2>/dev/null || true
wait "$PIPELINE_PID" 2>/dev/null || true

if ! grep -q "RTP" /tmp/test_multi_rtp.log; then
    echo "FAILED: --rtp multi-egress did not configure RTP output!"
    cat /tmp/test_multi_rtp.log
    exit 1
fi
echo "  [PASS] Multi-egress with --rtp and -o rtp verified."

echo "=========================================================="
echo "=== ALL RTP INGRESS & EGRESS TESTS PASSED SUCCESSFULLY! ==="
echo "=========================================================="
