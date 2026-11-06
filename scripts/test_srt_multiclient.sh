#!/usr/bin/env bash
# test_srt_multiclient.sh
# Comprehensive verification of:
# 1. Multi-Client SRT Listener Server (1-to-many broadcasting, dynamic client join/leave)
# 2. Multiple SRT Egress Outputs (concurrent listener + caller targets)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$ROOT_DIR/ixgIPStream"

if [ ! -f "$BIN" ]; then
    echo "Building ixgIPStream..."
    make -C "$ROOT_DIR"
fi

TEST_UDP="udp://127.0.0.1:12355"
SRT_LISTEN_PORT="19495"
SRT_LISTEN_URL="srt://127.0.0.1:$SRT_LISTEN_PORT?mode=listener"
SRT_CLIENT_URL="srt://127.0.0.1:$SRT_LISTEN_PORT"

CLIENT1_OUT="/tmp/srt_client1.ts"
CLIENT2_OUT="/tmp/srt_client2.ts"
CLIENT3_OUT="/tmp/srt_client3.ts"
CLIENT4_OUT="/tmp/srt_client4.ts"
rm -f "$CLIENT1_OUT" "$CLIENT2_OUT" "$CLIENT3_OUT" "$CLIENT4_OUT" /tmp/srt_mc_pipeline.log /tmp/srt_mc_sim.log /tmp/srt_client*.log

cleanup() {
    echo "Cleaning up test processes..."
    kill -INT "${PIPELINE_PID:-}" 2>/dev/null || true
    kill -INT "${CLIENT1_PID:-}" 2>/dev/null || true
    kill -INT "${CLIENT2_PID:-}" 2>/dev/null || true
    kill -INT "${CLIENT3_PID:-}" 2>/dev/null || true
    kill -INT "${CLIENT4_PID:-}" 2>/dev/null || true
    kill -INT "${DUAL_RCV_PID:-}" 2>/dev/null || true
    kill "${SIM_PID:-}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

echo "========================================================================"
echo "=== PART 1: Multi-Client SRT Listener Server (1-to-Many Broadcast)   ==="
echo "========================================================================"

echo "[1/7] Starting Synthetic Multicast Broadcaster on $TEST_UDP..."
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 25 -keyint_min 25 -b:v 800k -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ar 44100 -ac 2 \
    -f mpegts "$TEST_UDP?pkt_size=1316" > /tmp/srt_mc_sim.log 2>&1 &
SIM_PID=$!
sleep 1

echo "[2/7] Starting ixgIPStream with SRT Multi-Client Listener on port $SRT_LISTEN_PORT..."
"$BIN" -i "$TEST_UDP" -o srt "$SRT_LISTEN_URL" -l > /tmp/srt_mc_pipeline.log 2>&1 &
PIPELINE_PID=$!
sleep 2

echo "[3/7] Connecting Client #1 and Client #2 simultaneously..."
ffmpeg -v info -i "$SRT_CLIENT_URL" -c copy -y "$CLIENT1_OUT" > /tmp/srt_client1.log 2>&1 &
CLIENT1_PID=$!

ffmpeg -v info -i "$SRT_CLIENT_URL" -c copy -y "$CLIENT2_OUT" > /tmp/srt_client2.log 2>&1 &
CLIENT2_PID=$!

sleep 3

echo "[4/7] Connecting Client #3 (third simultaneous client)..."
ffmpeg -v info -i "$SRT_CLIENT_URL" -c copy -y "$CLIENT3_OUT" > /tmp/srt_client3.log 2>&1 &
CLIENT3_PID=$!

sleep 3

echo "[5/7] Verifying Client #1, #2, #3 are all actively receiving data..."
if ! kill -0 "$PIPELINE_PID" 2>/dev/null; then
    echo "ERROR: ixgIPStream terminated unexpectedly!"
    cat /tmp/srt_mc_pipeline.log
    exit 1
fi

grep -E "Client #1 connected|Client #2 connected|Client #3 connected" /tmp/srt_mc_pipeline.log || {
    echo "ERROR: Expected connections in pipeline log!"
    cat /tmp/srt_mc_pipeline.log
    exit 1
}

echo "[6/7] Disconnecting Client #1 (testing client leave resilience)..."
kill -INT "$CLIENT1_PID" 2>/dev/null || true
wait "$CLIENT1_PID" 2>/dev/null || true
sleep 2

echo "Connecting Client #4 (late joiner test while Client #2 and #3 continue)..."
ffmpeg -v info -i "$SRT_CLIENT_URL" -c copy -y "$CLIENT4_OUT" > /tmp/srt_client4.log 2>&1 &
CLIENT4_PID=$!
sleep 3

echo "[7/7] Stopping all clients and verifying recordings..."
kill -INT "$CLIENT2_PID" "$CLIENT3_PID" "$CLIENT4_PID" "$PIPELINE_PID" 2>/dev/null || true
wait "$CLIENT2_PID" "$CLIENT3_PID" "$CLIENT4_PID" "$PIPELINE_PID" 2>/dev/null || true

for f in "$CLIENT1_OUT" "$CLIENT2_OUT" "$CLIENT3_OUT" "$CLIENT4_OUT"; do
    if [ -s "$f" ]; then
        echo "SUCCESS: Recorded stream verified: $f ($(ls -lh "$f" | awk '{print $5}'))"
    else
        echo "ERROR: Output file empty: $f"
        cat /tmp/srt_mc_pipeline.log
        exit 1
    fi
done

echo ""
echo "========================================================================"
echo "=== PART 2: Multiple Concurrent SRT Outputs (Listener + Caller)       ==="
echo "========================================================================"

SRT_DUAL_LISTEN="srt://127.0.0.1:19496?mode=listener"
SRT_DUAL_CALLER="srt://127.0.0.1:19497?mode=caller"
SRT_DUAL_RECEIVER="srt://127.0.0.1:19497?mode=listener"
DUAL_OUT1="/tmp/srt_dual_out1.ts"
DUAL_OUT2="/tmp/srt_dual_out2.ts"
rm -f "$DUAL_OUT1" "$DUAL_OUT2" /tmp/srt_dual_pipeline.log

echo "[1/4] Starting SRT Receiver for Target #2 (Caller -> Receiver) on port 19497..."
ffmpeg -v info -i "$SRT_DUAL_RECEIVER" -c copy -y "$DUAL_OUT2" > /dev/null 2>&1 &
DUAL_RCV_PID=$!
sleep 1

echo "[2/4] Starting ixgIPStream with DUAL SRT Outputs:"
echo "      Output #1: Listener on port 19496"
echo "      Output #2: Caller to port 19497"
"$BIN" -i "$TEST_UDP" -o srt "$SRT_DUAL_LISTEN" -o srt "$SRT_DUAL_CALLER" -l > /tmp/srt_dual_pipeline.log 2>&1 &
PIPELINE_PID=$!
sleep 2

echo "[3/4] Connecting player to Target #1 (Listener on port 19496)..."
ffmpeg -v info -i "srt://127.0.0.1:19496" -c copy -y "$DUAL_OUT1" > /dev/null 2>&1 &
CLIENT1_PID=$!
sleep 4

echo "[4/4] Stopping dual pipeline and verifying both output streams..."
kill -INT "$CLIENT1_PID" "$PIPELINE_PID" "$DUAL_RCV_PID" 2>/dev/null || true
wait "$CLIENT1_PID" "$PIPELINE_PID" "$DUAL_RCV_PID" 2>/dev/null || true

if [ -s "$DUAL_OUT1" ] && [ -s "$DUAL_OUT2" ]; then
    echo "SUCCESS: Target #1 (Listener) stream: $DUAL_OUT1 ($(ls -lh "$DUAL_OUT1" | awk '{print $5}'))"
    echo "SUCCESS: Target #2 (Caller) stream:   $DUAL_OUT2 ($(ls -lh "$DUAL_OUT2" | awk '{print $5}'))"
else
    echo "ERROR: One or both dual SRT outputs failed!"
    cat /tmp/srt_dual_pipeline.log
    exit 1
fi

echo ""
echo "========================================================================"
echo "=== ALL MULTI-CLIENT & MULTI-TARGET SRT TESTS PASSED SUCCESSFULLY!   ==="
echo "========================================================================"
