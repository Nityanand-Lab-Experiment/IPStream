#!/usr/bin/env bash
# simulate_rtmp_server.sh
# Runs a lightweight local RTMP receiver server using FFmpeg listen mode.

set -euo pipefail

RTMP_URL="${1:-rtmp://127.0.0.1:1935/live/stream}"

echo "================================================================="
echo "   Local RTMP Test Server (FFmpeg Listen Mode)                  "
echo "================================================================="
echo "Listening on : ${RTMP_URL}"
echo "Waiting for incoming RTMP stream..."
echo "Press Ctrl+C to stop."
echo "================================================================="

ffmpeg -listen 1 -f flv -i "${RTMP_URL}" -c copy -f null -
