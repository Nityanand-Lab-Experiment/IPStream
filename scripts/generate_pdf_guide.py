#!/usr/bin/env python3
"""
Generate comprehensive ixgIPStream v4.1 Telemetry and Log Guide PDF using WeasyPrint.
Includes exhaustive documentation of all log formats:
1. Startup & Lifecycle Configuration Logs
2. Mode 1: Interactive Live Console Dashboard (All Variations)
3. Mode 2: Standard Timestamped Log Stream (--log-stats / -l) (All Variations)
4. Mode 3: Multi-Line Diagnostic Card (--detailed / -d) (Single & Multi-Egress)
5. Mode 4: Structured JSON Telemetry Stream (--json / -j) (Schema & Examples)
6. Mode 5: Diagnostic Warning Logs ([WARN]) & Critical Error Catalog
7. Mode 6: Systemd Journal & Enterprise File Logging Integration
8. Diagnostic Lookup & Troubleshooting Runbook
"""

import os
import shutil
import subprocess

HTML_CONTENT = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>ixgIPStream v4.1 - Telemetry & Log Guide</title>
<style>
  @page {
    size: A4;
    margin: 15mm 13mm 15mm 13mm;
    @top-right {
      content: "ixgIPStream v4.1 Telemetry & Log Guide";
      font-size: 7.5pt;
      color: #64748b;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    }
    @bottom-left {
      content: "Production Operational Runbook & Reference • Zero-Encoding Pipeline";
      font-size: 7.5pt;
      color: #94a3b8;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    }
    @bottom-right {
      content: "Page " counter(page) " of " counter(pages);
      font-size: 7.5pt;
      color: #64748b;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    }
  }

  @page:first {
    @top-right { content: none; }
    @bottom-left { content: none; }
    @bottom-right { content: none; }
    margin: 0;
  }

  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    font-size: 8.5pt;
    line-height: 1.42;
    color: #1e293b;
    background-color: #ffffff;
    margin: 0;
    padding: 0;
  }

  /* Cover Page */
  .cover {
    background: linear-gradient(145deg, #070c18 0%, #0f172a 45%, #050a14 100%);
    color: #ffffff;
    padding: 55mm 20mm 20mm 20mm;
    box-sizing: border-box;
    page-break-after: always;
    min-height: 297mm;
  }

  .cover-badge {
    display: inline-block;
    background: rgba(14, 165, 233, 0.16);
    border: 1px solid rgba(56, 189, 248, 0.45);
    color: #38bdf8;
    padding: 4px 14px;
    border-radius: 9999px;
    font-size: 8pt;
    font-weight: 700;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    margin-bottom: 22px;
  }

  .cover h1 {
    font-size: 32pt;
    font-weight: 800;
    line-height: 1.12;
    margin: 0 0 16px 0;
    color: #ffffff;
    letter-spacing: -0.03em;
  }

  .cover h1 span {
    color: #38bdf8;
  }

  .cover p.subtitle {
    font-size: 12.5pt;
    color: #94a3b8;
    line-height: 1.5;
    margin: 0 0 45px 0;
    max-width: 630px;
  }

  .cover-meta {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 14px;
    max-width: 580px;
    border-top: 1px solid rgba(255, 255, 255, 0.12);
    padding-top: 24px;
  }

  .cover-meta-item {
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 6px;
    padding: 10px 14px;
  }

  .cover-meta-label {
    font-size: 7pt;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: #64748b;
    margin-bottom: 2px;
  }

  .cover-meta-value {
    font-size: 9.5pt;
    font-weight: 600;
    color: #f1f5f9;
  }

  /* Headings */
  h2 {
    font-size: 14pt;
    font-weight: 700;
    color: #0f172a;
    border-bottom: 2px solid #0284c7;
    padding-bottom: 4px;
    margin: 16px 0 10px 0;
    page-break-after: avoid;
    display: flex;
    align-items: center;
  }

  h2 .section-num {
    background: #0284c7;
    color: #ffffff;
    font-size: 9pt;
    font-weight: 700;
    padding: 2px 7px;
    border-radius: 4px;
    margin-right: 8px;
  }

  h3 {
    font-size: 10.5pt;
    font-weight: 700;
    color: #1e293b;
    margin: 12px 0 5px 0;
    page-break-after: avoid;
  }

  h4 {
    font-size: 9pt;
    font-weight: 700;
    color: #334155;
    margin: 9px 0 4px 0;
    page-break-after: avoid;
  }

  p {
    margin: 0 0 7px 0;
  }

  /* Terminals / Code Blocks */
  .terminal {
    background: #090e17;
    border: 1px solid #1e293b;
    border-radius: 6px;
    overflow: hidden;
    margin: 6px 0 9px 0;
    page-break-inside: avoid;
  }

  .terminal-header {
    background: #111827;
    border-bottom: 1px solid #1f2937;
    padding: 4px 9px;
    display: flex;
    align-items: center;
  }

  .terminal-dots {
    display: inline-flex;
    gap: 4px;
    margin-right: 8px;
  }

  .dot {
    width: 6.5px;
    height: 6.5px;
    border-radius: 50%;
    display: inline-block;
  }
  .dot-red { background: #ef4444; }
  .dot-yellow { background: #f59e0b; }
  .dot-green { background: #10b981; }

  .terminal-title {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 7pt;
    color: #94a3b8;
  }

  .terminal-body {
    padding: 6px 9px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 6.5pt;
    line-height: 1.34;
    color: #f1f5f9;
    white-space: pre-wrap;
    word-break: break-all;
  }

  /* Terminal Syntax Colors */
  .c-cyan   { color: #38bdf8; font-weight: 600; }
  .c-green  { color: #4ade80; font-weight: 600; }
  .c-yellow { color: #facc15; font-weight: 600; }
  .c-red    { color: #f87171; font-weight: 600; }
  .c-blue   { color: #60a5fa; font-weight: 600; }
  .c-purple { color: #c084fc; font-weight: 600; }
  .c-dim    { color: #64748b; }
  .c-white  { color: #f8fafc; font-weight: 600; }
  .c-bold   { font-weight: 700; }

  /* Tables */
  table {
    width: 100%;
    border-collapse: collapse;
    margin: 6px 0 10px 0;
    font-size: 7.8pt;
    page-break-inside: avoid;
  }

  th {
    background: #f1f5f9;
    color: #0f172a;
    font-weight: 700;
    text-align: left;
    padding: 5px 7px;
    border: 1px solid #cbd5e1;
    font-size: 7.6pt;
  }

  td {
    padding: 4px 7px;
    border: 1px solid #e2e8f0;
    vertical-align: top;
    line-height: 1.32;
  }

  tr:nth-child(even) td {
    background-color: #f8fafc;
  }

  code {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 7.3pt;
    background: #f1f5f9;
    color: #0284c7;
    padding: 1px 4px;
    border-radius: 3px;
    border: 1px solid #e2e8f0;
  }

  /* Callouts & Cards */
  .callout {
    border-radius: 5px;
    padding: 7px 11px;
    margin: 7px 0 10px 0;
    font-size: 7.8pt;
    page-break-inside: avoid;
  }

  .callout-info {
    background: #f0f9ff;
    border-left: 3px solid #0284c7;
    color: #0369a1;
  }

  .callout-warn {
    background: #fffbeb;
    border-left: 3px solid #f59e0b;
    color: #b45309;
  }

  .callout-alert {
    background: #fef2f2;
    border-left: 3px solid #ef4444;
    color: #b91c1c;
  }

  .warn-card {
    border: 1px solid #e2e8f0;
    border-left: 3px solid #f59e0b;
    border-radius: 4px;
    padding: 6px 9px;
    margin: 5px 0 8px 0;
    background: #ffffff;
    page-break-inside: avoid;
  }

  .warn-card.alert-card {
    border-left-color: #ef4444;
  }

  .warn-header {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    margin-bottom: 2px;
  }

  .warn-title {
    font-weight: 700;
    color: #0f172a;
    font-size: 8.2pt;
  }

  .warn-source {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 6.8pt;
    color: #64748b;
    background: #f1f5f9;
    padding: 1px 5px;
    border-radius: 3px;
  }

  .warn-body {
    font-size: 7.5pt;
    color: #334155;
    line-height: 1.34;
  }

  .page-break {
    page-break-after: always;
  }
</style>
</head>
<body>

<!-- ========================================== -->
<!-- PAGE 1: COVER PAGE                         -->
<!-- ========================================== -->
<div class="cover">
  <div>
    <div class="cover-badge">ixgIPStream v4.1 • Engineering Runbook</div>
    <h1>ixgIPStream v4.1<br><span>Telemetry & Log Guide</span></h1>
    <p class="subtitle">Complete technical reference for operational monitoring, live telemetry matrices, JSON streaming, kernel TCP diagnostics, and buffer risk prevention across RTMP and HLS.</p>
  </div>

  <div class="cover-meta">
    <div class="cover-meta-item">
      <div class="cover-meta-label">Binary Executable</div>
      <div class="cover-meta-value">ixgIPStream (v4.1)</div>
    </div>
    <div class="cover-meta-item">
      <div class="cover-meta-label">Architecture</div>
      <div class="cover-meta-value">Zero-Encoding Remuxer (0% CPU)</div>
    </div>
    <div class="cover-meta-item">
      <div class="cover-meta-label">Ingress Protocol</div>
      <div class="cover-meta-value">UDP IGMP Multicast (MPEG-TS)</div>
    </div>
    <div class="cover-meta-item">
      <div class="cover-meta-label">Egress Protocols</div>
      <div class="cover-meta-value">Multi-RTMP / Multi-HLS CDN (PUT/POST)</div>
    </div>
    <div class="cover-meta-item">
      <div class="cover-meta-label">Kernel Observability</div>
      <div class="cover-meta-value">MPEG-TS CC Loss + TCP_INFO Socket Telemetry</div>
    </div>
    <div class="cover-meta-item">
      <div class="cover-meta-label">Telemetry Formats</div>
      <div class="cover-meta-value">Interactive, --log-stats, --detailed, --json</div>
    </div>
  </div>
</div>

<!-- ========================================== -->
<!-- PAGE 2: ARCHITECTURE & LIFECYCLE LOGS      -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">1</span> Telemetry Engine Architecture & Lifecycle Logs</h2>
  <p>The <strong>ixgIPStream v4.1</strong> pipeline ingests live MPEG-TS UDP multicast streams and forwards them concurrently to RTMP ingests (e.g. YouTube Live, Twitch, SRS) and HLS CDNs without decoding or re-encoding. To ensure 24/7 broadcast reliability without viewer buffering, the observability engine tracks metrics across four layers:</p>

  <table>
    <thead>
      <tr>
        <th style="width: 22%;">Pipeline Layer</th>
        <th style="width: 26%;">Diagnostic Mechanism</th>
        <th style="width: 26%;">Key Telemetry Metrics</th>
        <th style="width: 26%;">Buffering Prevention Focus</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><strong>UDP Ingress</strong><br>(Multicast)</td>
        <td>MPEG-TS Continuity Counter (CC) sequence evaluation (4-bit 0&ndash;15).</td>
        <td><code>udp_loss_pct</code>, <code>udp_lost_packets</code>, <code>udp_cc_errors</code>, <code>pps</code></td>
        <td>Detects upstream switch drops, Ethernet errors, and socket buffer overruns before remuxing.</td>
      </tr>
      <tr>
        <td><strong>RTMP Egress</strong><br>(TCP Uplink)</td>
        <td>Linux Kernel <code>TCP_INFO</code> socket inspection via <code>getsockopt()</code>.</td>
        <td><code>tcp_retrans</code>, <code>tcp_lost</code>, <code>tcp_rtt_ms</code>, <code>tcp_rttvar_ms</code></td>
        <td>Monitors uplink ISP packet loss, route changes, bufferbloat, and connection health to RTMP servers.</td>
      </tr>
      <tr>
        <td><strong>HLS Egress</strong><br>(CDN Chunk Upload)</td>
        <td>POSIX microsecond upload timer + persistent socket FD tracking.</td>
        <td><code>upload_time_ms</code>, <code>speed_factor</code>, <code>origin_health</code>, <code>tcp_retrans</code></td>
        <td>Guarantees that HTTP PUT/POST segment uploads complete within real-time margins (e.g. &gt; 5x speed).</td>
      </tr>
      <tr>
        <td><strong>Media Sync & Pacing</strong></td>
        <td>Delta PTS vs. Monotonic Wall Clock + A/V timestamp subtraction.</td>
        <td><code>cur_speed_factor</code>, <code>cur_wall_lead_ms</code>, <code>av_drift_ms</code>, <code>silence_ms</code></td>
        <td>Prevents player buffer drainage (&lt; 1.0x speed), lip-sync drift, and asymmetric video/audio freezes.</td>
      </tr>
    </tbody>
  </table>

  <h3>Startup & Lifecycle Log Sequence</h3>
  <p>When <code>ixgIPStream</code> starts, it validates configuration parameters, negotiates multicast IGMP membership, analyzes streams, and initializes muxers:</p>

  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Console Startup Logs: ./ixgIPStream -i udp://@239.255.0.1:1234 -o rtmp rtmp://... -o hls http://...</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">========================================================================</span>
<span class="c-white">   ixgIPStream v4.1 - Multicast to RTMP / HLS Zero-Encoding Streaming Pipeline</span>
<span class="c-cyan">========================================================================</span>
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span> Configuration:
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>   Input URL            : udp://@239.255.0.1:1234
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>   Outputs (2 configured):
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>     [0] RTMP -> rtmp://a.rtmp.youtube.com/live2/xxxx-xxxx
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>     [1] HLS  -> http://origin.cdn.com/live/stream.m3u8 (Method: PUT, Seg: 2s, List: 5)
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>   UDP Buffer / FIFO    : 16 MB kernel buffer, 50 MB FIFO queue (278528 packets)
<span class="c-dim">2026-10-05 18:00:00</span> <span class="c-green">[INFO] </span>   Telemetry Mode       : Interactive Console Dashboard (1000 ms)
<span class="c-dim">2026-10-05 18:00:01</span> <span class="c-green">[INFO] </span> Successfully joined multicast group udp://@239.255.0.1:1234
<span class="c-dim">2026-10-05 18:00:01</span> <span class="c-green">[INFO] </span> Input stream format: MPEG-TS (bitrate: 2650 kbps, 2 streams detected)
<span class="c-dim">2026-10-05 18:00:01</span> <span class="c-green">[INFO] </span>   Stream #0: Video -> h264 (High) (yuv420p), 1920x1080 [SAR 1:1 DAR 16:9], 25.00 fps
<span class="c-dim">2026-10-05 18:00:01</span> <span class="c-green">[INFO] </span>   Stream #1: Audio -> aac (LC), 48000 Hz, stereo, fltp, 128 kbps
<span class="c-dim">2026-10-05 18:00:01</span> <span class="c-green">[INFO] </span> Waiting for first video IDR keyframe to synchronize egress muxers...
<span class="c-dim">2026-10-05 18:00:02</span> <span class="c-green">[INFO] </span> Received first video IDR keyframe. Normalizing PTS/DTS offsets (PTS=0.000s).
<span class="c-dim">2026-10-05 18:00:02</span> <span class="c-green">[INFO] </span> RTMP connection established: rtmp://a.rtmp.youtube.com/live2/xxxx-xxxx
<span class="c-dim">2026-10-05 18:00:02</span> <span class="c-green">[INFO] </span> HLS muxer initialized: http://origin.cdn.com/live/stream.m3u8 (fMP4=No)
<span class="c-dim">2026-10-05 18:00:02</span> <span class="c-green">[INFO] </span> Streaming active. Telemetry reporting running at 1000 ms intervals.</div>
  </div>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 3: MODE 1 (INTERACTIVE DASHBOARD)     -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">2</span> Format 1: Interactive Live Console Dashboard (Default)</h2>
  <p>The default interactive mode is optimized for terminal operators and Network Operations Centers (NOC). It updates dynamically in-place via carriage returns (<code>\r</code>) every second without flooding the console scrollback.</p>

  <h3>Variant A: Option 1 &mdash; Only RTMP Egress</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Terminal: ./ixgIPStream -i udp://@239.255.0.1:1234 -o rtmp rtmp://a.rtmp.youtube.com/live2/XXXX</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">[00:04:12]</span> <span class="c-green">UDP:</span>  2450 kbps (Loss: 0.0%) │ <span class="c-blue">RTMP:</span>  2450 kbps (25.0 fps, Retrans: 0) │ <span class="c-white">Spd:</span> 1.00x │ <span class="c-purple">KF:</span> 1.2s │ <span class="c-green">HEALTHY 🟢</span></div>
  </div>

  <h3>Variant B: Option 2 &mdash; Only HLS Egress</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Terminal: ./ixgIPStream -i udp://@239.255.0.1:1234 -o hls http://origin.cdn.com/live/stream.m3u8</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">[00:04:12]</span> <span class="c-green">UDP:</span>  2450 kbps (Loss: 0.0%) │ <span class="c-purple">HLS:</span>  2450 kbps (214.2 pps, Seg #14, Retrans: 0) │ <span class="c-white">Spd:</span> 1.00x │ <span class="c-green">OK 🟢</span></div>
  </div>

  <h3>Variant C: Option 3 &mdash; Dual Output (RTMP + HLS)</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Terminal: ./ixgIPStream -i udp://@239.255.0.1:1234 -o rtmp rtmp://... -o hls http://...</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">[00:04:12]</span> <span class="c-green">UDP:</span> 2450 kbps (Loss: 0.0%) │ <span class="c-blue">RTMP:</span> 2450 kbps (25 fps) │ <span class="c-purple">HLS:</span> 2450 kbps (214 pps, Seg #14) │ <span class="c-white">Spd:</span> 1.00x │ <span class="c-green">HEALTHY 🟢</span></div>
  </div>

  <h3>Variant D: Multi-Egress Fan-Out (2x RTMP + 2x HLS) + Confidence Monitor</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Terminal: ./ixgIPStream -i udp://... -o rtmp ... -o rtmp ... -o hls ... -o hls ... --monitor-file /tmp/m.jpg</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">[00:04:12]</span> <span class="c-green">UDP:</span>  2450 kbps │ <span class="c-blue">RTMP (2):</span>  2450 kbps │ <span class="c-purple">HLS (2):</span>  2450 kbps │ <span class="c-yellow">Mon:</span> 1260 │ <span class="c-white">Spd:</span> 1.00x │ <span class="c-green">HEALTHY 🟢</span></div>
  </div>

  <h4>Exhaustive Metric Matrix for Interactive Mode:</h4>
  <table>
    <thead>
      <tr>
        <th style="width: 20%;">Column</th>
        <th style="width: 25%;">Sample Value</th>
        <th style="width: 55%;">Technical Meaning & Thresholds</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><code>[Uptime]</code></td>
        <td><code>[00:04:12]</code></td>
        <td>Active stream runtime in <code>[HH:MM:SS]</code> since receiving the first video keyframe.</td>
      </tr>
      <tr>
        <td><code>UDP: kbps (Loss)</code></td>
        <td><code>2450 kbps (Loss: 0.0%)</code></td>
        <td>Instantaneous multicast reception bitrate and 1-second interval MPEG-TS packet drop percentage.</td>
      </tr>
      <tr>
        <td><code>RTMP: kbps (fps)</code></td>
        <td><code>2450 kbps (25.0 fps)</code></td>
        <td>Actual outbound throughput to RTMP destination and current video frame rate.</td>
      </tr>
      <tr>
        <td><code>Retrans: N</code></td>
        <td><code>Retrans: 0</code></td>
        <td>TCP segments retransmitted to ingest server (queried via kernel <code>TCP_INFO</code>). &gt; 10 indicates network drop.</td>
      </tr>
      <tr>
        <td><code>HLS: kbps (pps)</code></td>
        <td><code>2450 kbps (214.2 pps)</code></td>
        <td>HLS upload throughput and packet delivery rate to disk or remote CDN endpoint.</td>
      </tr>
      <tr>
        <td><code>Seg #N</code></td>
        <td><code>Seg #14</code></td>
        <td>Sequence number of the most recently finalized media segment chunk uploaded to the CDN.</td>
      </tr>
      <tr>
        <td><code>Spd: X.XXx</code></td>
        <td><code>1.00x</code></td>
        <td>Real-time delivery ratio (&Delta;PTS / &Delta;Wall Clock). Values &lt; 0.90x trigger immediate buffer risk warnings.</td>
      </tr>
      <tr>
        <td><code>KF: N.Ns</code></td>
        <td><code>1.2s</code></td>
        <td>Age of current GOP: elapsed seconds since the last IDR keyframe was transmitted.</td>
      </tr>
      <tr>
        <td><code>Mon: N</code></td>
        <td><code>Mon: 1260</code></td>
        <td>Total JPEG snapshot frames atomically written to disk by the background confidence monitor.</td>
      </tr>
      <tr>
        <td><code>Health Badge</code></td>
        <td><code>HEALTHY 🟢</code> / <code>BUFFER RISK 🔴</code></td>
        <td>Composite real-time ingest health state evaluating pacing, keyframes, socket latency, and TCP retransmits.</td>
      </tr>
    </tbody>
  </table>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 4: MODE 2 (STANDARD TIMESTAMPED LOGS) -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">3</span> Format 2: Standard Timestamped Log Stream (<code>--log-stats</code> / <code>-l</code>)</h2>
  <p>When running under <code>systemd</code>, enterprise syslog collectors, or redirected via <code>tee</code> to log files, <code>--log-stats</code> formats every 1-second interval as a standard, UTC/local timestamped log entry. The matrix adapts dynamically to configured targets:</p>

  <h3>Variant A: RTMP-Only Ingest (YouTube Live Target)</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Log Output: ./ixgIPStream -i udp://... -o rtmp rtmp://... --log-stats | tee -a stream.log</div>
    </div>
    <div class="terminal-body"><span class="c-dim">2026-10-05 18:04:01</span> <span class="c-green">[INFO] </span> UDP: 2450.2 kbps (Loss: 0.00%, 0 pkts) | RTMP: 2450.2 kbps (25.0 fps, Retrans: 0, RTT: 12.4ms) | Lat: 0.02/0.05/0.24ms | Silence(V/A): 0/162ms | Speed: 1.00x | Drift: +4ms | GOP: 2.0s | YouTube: HEALTHY | Out: 73.50 MB
<span class="c-dim">2026-10-05 18:04:02</span> <span class="c-green">[INFO] </span> UDP: 2449.8 kbps (Loss: 0.00%, 0 pkts) | RTMP: 2449.8 kbps (25.0 fps, Retrans: 0, RTT: 12.5ms) | Lat: 0.02/0.04/0.21ms | Silence(V/A): 0/210ms | Speed: 1.00x | Drift: +5ms | GOP: 2.0s | YouTube: HEALTHY | Out: 73.81 MB</div>
  </div>

  <h3>Variant B: HLS-Only Ingest (Origin CDN Target)</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Log Output: ./ixgIPStream -i udp://... -o hls http://... --log-stats</div>
    </div>
    <div class="terminal-body"><span class="c-dim">2026-10-05 18:04:01</span> <span class="c-green">[INFO] </span> UDP: 2450.2 kbps (Loss: 0.00%, 0 pkts) | HLS: 2450.2 kbps (214.2 pps, 73.50 MB, Retrans: 0, RTT: 8.2ms) | Seg: #14 (2.0s, 42.1ms) | Speed: 1.00x | Drift: +4ms | Out: 73.50 MB
<span class="c-dim">2026-10-05 18:04:02</span> <span class="c-green">[INFO] </span> UDP: 2449.8 kbps (Loss: 0.00%, 0 pkts) | HLS: 2449.8 kbps (214.2 pps, 73.81 MB, Retrans: 0, RTT: 8.3ms) | Seg: #15 (2.0s, 39.4ms) | Speed: 1.00x | Drift: +5ms | Out: 73.81 MB</div>
  </div>

  <h3>Variant C: Dual Output (RTMP + HLS Concurrently)</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Log Output: ./ixgIPStream -i udp://... -o rtmp ... -o hls ... --log-stats</div>
    </div>
    <div class="terminal-body"><span class="c-dim">2026-10-05 18:04:01</span> <span class="c-green">[INFO] </span> UDP: 2450.2 kbps (Loss: 0.00%, 0 pkts) | RTMP: 2450.2 kbps (25.0 fps, Retrans: 0, RTT: 12.4ms) | HLS: 2450.2 kbps (214.2 pps, Seg #14, Retrans: 0) | Lat: 0.02/0.05/0.24ms | Speed: 1.00x | Drift: +4ms | GOP: 2.0s | YouTube: HEALTHY</div>
  </div>

  <h4>Complete Metric Key for <code>--log-stats</code> Entries:</h4>
  <table>
    <thead>
      <tr>
        <th style="width: 22%;">Field Token</th>
        <th style="width: 28%;">Example String</th>
        <th style="width: 50%;">Operational Diagnostics & Verification</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><strong><code>UDP</code></strong></td>
        <td><code>2450.2 kbps (Loss: 0.00%, 0 pkts)</code></td>
        <td>Multicast ingress bitrate, 1-sec packet loss percentage, and cumulative lifetime lost TS packets.</td>
      </tr>
      <tr>
        <td><strong><code>RTMP</code></strong></td>
        <td><code>2450.2 kbps (25.0 fps, Retrans: 0, RTT: 12.4ms)</code></td>
        <td>RTMP egress rate, video FPS, kernel TCP retransmissions, and real-time Round-Trip Time to the RTMP host.</td>
      </tr>
      <tr>
        <td><strong><code>HLS</code></strong></td>
        <td><code>2450.2 kbps (214.2 pps, Seg #14, Retrans: 0)</code></td>
        <td>HLS upload bitrate, packet rate, current completed media chunk, and CDN TCP retransmission count.</td>
      </tr>
      <tr>
        <td><strong><code>Lat: min/avg/max</code></strong></td>
        <td><code>0.02/0.05/0.24ms</code></td>
        <td>Execution duration of <code>av_interleaved_write_frame()</code>. High max values (&gt; 300ms) signal socket buffer saturation.</td>
      </tr>
      <tr>
        <td><strong><code>Silence(V/A)</code></strong></td>
        <td><code>0/210ms</code></td>
        <td>Milliseconds since last packet write for Video and Audio tracks. Detects asymmetric stream lockups.</td>
      </tr>
      <tr>
        <td><strong><code>Speed</code></strong></td>
        <td><code>1.00x</code></td>
        <td>Delivery speed ratio (&Delta;PTS / &Delta;Wall Clock). Must remain &ge; 1.00x to prevent viewer buffering.</td>
      </tr>
      <tr>
        <td><strong><code>Drift</code></strong></td>
        <td><code>+5ms</code></td>
        <td>Timestamp difference between Video PTS and Audio PTS. Values &gt; &plusmn;600ms cause lip-sync buffering.</td>
      </tr>
      <tr>
        <td><strong><code>GOP</code></strong></td>
        <td><code>2.0s</code></td>
        <td>Measured cadence between consecutive IDR keyframes. YouTube strictly recommends 2.0 &ndash; 4.0s.</td>
      </tr>
      <tr>
        <td><strong><code>YouTube</code></strong></td>
        <td><code>HEALTHY</code></td>
        <td>Automated health classifier (<code>HEALTHY</code>, <code>BUFFER RISK: Underfeeding</code>, <code>BUFFER RISK: High Jitter</code>, etc.).</td>
      </tr>
      <tr>
        <td><strong><code>Out</code></strong></td>
        <td><code>73.81 MB</code></td>
        <td>Cumulative total payload transferred to the egress target since stream initialization.</td>
      </tr>
    </tbody>
  </table>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 5: MODE 3 (DETAILED DIAGNOSTIC CARDS) -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">4</span> Format 3: Multi-Line Diagnostic Card (<code>--detailed</code> / <code>-d</code>)</h2>
  <p>Generates an exhaustive, multi-line ASCII card every second. Designed for deep diagnostics, link commissioning, and root-cause analysis during live network instability.</p>

  <h3>Complete Multi-Egress Diagnostic Card (2 RTMP + 1 HLS + Monitor)</h3>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Console Output: ./ixgIPStream ... -o rtmp ... -o rtmp ... -o hls ... --detailed</div>
    </div>
    <div class="terminal-body"><span class="c-cyan">┌── [INFO @ 00:08:45] (RTMP: 2 active, HLS: 1 active) ──────────────────────────────────────────────────────────</span>
│  <span class="c-green">[UDP Ingress]</span>   :  2450.8 kbps  |  218.4 pkts/s  | In: 154.2 MB | Loss: 0.00% (0 dropped, 0 CC errs)
│  <span class="c-blue">[RTMP #1]</span>       :  2450.8 kbps |  25.0 fps (V) |  43.2 pps (A) | Out: 154.2 MB
│    ├─ Target   : rtmp://primary.youtube.com/live2/xxxx-xxxx
│    ├─ Uplink   : Retrans: 0 lost/resend | Lost: 0 | RTT:  14.2ms (Jitter:  2.1ms)
│    └─ Latency  : Avg:  0.05ms | Min:  0.02ms | Max:  0.28ms
│  <span class="c-blue">[RTMP #2]</span>       :  2450.8 kbps |  25.0 fps (V) |  43.2 pps (A) | Out: 154.2 MB
│    ├─ Target   : rtmp://backup.youtube.com/live2/yyyy-yyyy
│    ├─ Uplink   : Retrans: 1 lost/resend | Lost: 0 | RTT:  18.6ms (Jitter:  3.4ms)
│    └─ Latency  : Avg:  0.07ms | Min:  0.03ms | Max:  0.34ms
│  <span class="c-purple">[HLS Egress]</span>
│    ├─ Target           : http://origin.cdn.com/live/stream.m3u8 (Method: PUT)
│    ├─ Data Egress      :  2450.8 kbps |  218.4 pkts/s | Total: 154.2 MB
│    ├─ Active Segment   : stream_00042.ts (Size: 612.4 KB, Duration: 2.00s)
│    ├─ Segments Pushed  : 42 completed | Window: 5 segments in playlist
│    ├─ CDN Upload Speed : 38.4ms per segment (52.1x real-time margin)
│    ├─ TCP Uplink (CDN) : Retrans: 0 lost/resend | In-Flight Lost: 0 | RTT:  11.2ms (Jitter:  1.8ms)
│    └─ Origin Health    : HEALTHY
│  <span class="c-white">[Stream Pacing]</span> : Speed: 1.00x | Max Gap:  40.2ms | Wall Lead: +320ms
│  <span class="c-purple">[Track Silence]</span> : Video:   12ms | Audio:   18ms
│  <span class="c-cyan">[A/V & Sync ]</span>   : Video PTS: 525.20s | Audio PTS: 525.19s | Drift: +8 ms
│  <span class="c-yellow">[YouTube Health]</span>: <span class="c-green">HEALTHY</span> (GOP: 2.00s | KF Age: 1.1s | Lat: 0.05ms)
│  <span class="c-yellow">[Confidence Mon]</span>: 2625 frames (5 fps) -> /var/www/html/monitor.jpg
<span class="c-cyan">└─────────────────────────────────────────────────────────────────────────────────────────</span></div>
  </div>

  <h4>Key Diagnostic Subsystems Highlighted in Card Mode:</h4>
  <table>
    <thead>
      <tr>
        <th style="width: 25%;">Section</th>
        <th style="width: 35%;">Critical Fields</th>
        <th style="width: 40%;">Diagnostic Utility</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><strong><code>[UDP Ingress]</code></strong></td>
        <td><code>Loss: 0.00% (0 dropped, 0 CC errs)</code></td>
        <td>Differentiates network drops before ingest vs. uplink drops after remuxing.</td>
      </tr>
      <tr>
        <td><strong><code>[TCP Uplink]</code></strong></td>
        <td><code>Retrans: 0 | Lost: 0 | RTT | Jitter</code></td>
        <td>Identifies ISP routing spikes, transatlantic lag, and congested network interfaces.</td>
      </tr>
      <tr>
        <td><strong><code>[HLS Egress]</code></strong></td>
        <td><code>CDN Upload Speed | Real-time Margin</code></td>
        <td>Ensures chunk uploads finish in &lt; 200ms; values &gt; 1500ms cause CDN player stall.</td>
      </tr>
      <tr>
        <td><strong><code>[Stream Pacing]</code></strong></td>
        <td><code>Speed: 1.00x | Wall Lead: +320ms</code></td>
        <td>Measures encoder headroom. Positive lead ensures the player buffer never drains.</td>
      </tr>
      <tr>
        <td><strong><code>[Confidence Mon]</code></strong></td>
        <td><code>frames written (fps) -> path</code></td>
        <td>Validates atomic JPEG preview generation for multiviewer dashboards.</td>
      </tr>
    </tbody>
  </table>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 6: MODE 4 (STRUCTURED JSON STREAM)    -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">5</span> Format 4: Structured JSON Telemetry Stream (<code>--json</code> / <code>-j</code>)</h2>
  <p>Emits one valid, compact JSON object per second to <code>stdout</code>. Perfectly suited for piping directly into <code>jq</code>, Vector, FluentBit, Logstash, Datadog, or Prometheus parsers:</p>

  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Terminal: ./ixgIPStream ... --json | jq .</div>
    </div>
    <div class="terminal-body">{
  <span class="c-blue">"uptime_sec"</span>: 525,
  <span class="c-blue">"rtmp_status"</span>: <span class="c-green">"STREAMING"</span>,
  <span class="c-blue">"youtube_health"</span>: {
    <span class="c-blue">"status"</span>: <span class="c-green">"HEALTHY"</span>,
    <span class="c-blue">"speed_factor"</span>: 1.00,
    <span class="c-blue">"keyframe_age_sec"</span>: 1.10,
    <span class="c-blue">"max_pacing_gap_ms"</span>: 40.2,
    <span class="c-blue">"wall_lead_ms"</span>: 320,
    <span class="c-blue">"video_silence_ms"</span>: 12,
    <span class="c-blue">"audio_silence_ms"</span>: 18
  },
  <span class="c-blue">"udp_in"</span>: {
    <span class="c-blue">"bitrate_kbps"</span>: 2450.8,
    <span class="c-blue">"pps"</span>: 218.4,
    <span class="c-blue">"total_packets"</span>: 114660,
    <span class="c-blue">"total_bytes"</span>: 161671280,
    <span class="c-blue">"packet_loss"</span>: { <span class="c-blue">"lost_packets"</span>: 0, <span class="c-blue">"cc_errors"</span>: 0, <span class="c-blue">"total_loss_pct"</span>: 0.00, <span class="c-blue">"interval_loss_pct"</span>: 0.00 }
  },
  <span class="c-blue">"rtmp_out"</span>: {
    <span class="c-blue">"target"</span>: <span class="c-green">"rtmp://a.rtmp.youtube.com/live2/xxxx"</span>,
    <span class="c-blue">"bitrate_kbps"</span>: 2450.8,
    <span class="c-blue">"video_fps"</span>: 25.0,
    <span class="c-blue">"audio_pps"</span>: 43.2,
    <span class="c-blue">"av_drift_ms"</span>: 8,
    <span class="c-blue">"keyframe_interval_sec"</span>: 2.00,
    <span class="c-blue">"tcp_loss"</span>: { <span class="c-blue">"retrans_packets"</span>: 0, <span class="c-blue">"lost_packets"</span>: 0, <span class="c-blue">"rtt_ms"</span>: 14.2, <span class="c-blue">"rttvar_ms"</span>: 2.1 },
    <span class="c-blue">"write_latency"</span>: { <span class="c-blue">"avg_ms"</span>: 0.05, <span class="c-blue">"min_ms"</span>: 0.02, <span class="c-blue">"max_ms"</span>: 0.28, <span class="c-blue">"peak_ms"</span>: 1.45 }
  },
  <span class="c-blue">"hls_out"</span>: {
    <span class="c-blue">"target"</span>: <span class="c-green">"http://origin.cdn.com/live/stream.m3u8"</span>,
    <span class="c-blue">"method"</span>: <span class="c-green">"PUT"</span>,
    <span class="c-blue">"bitrate_kbps"</span>: 2450.8,
    <span class="c-blue">"pps"</span>: 218.4,
    <span class="c-blue">"segments_completed"</span>: 42,
    <span class="c-blue">"active_segment"</span>: { <span class="c-blue">"name"</span>: <span class="c-green">"stream_00042.ts"</span>, <span class="c-blue">"size_bytes"</span>: 627096, <span class="c-blue">"duration_sec"</span>: 2.00, <span class="c-blue">"upload_time_ms"</span>: 38.4, <span class="c-blue">"upload_speed_factor"</span>: 52.1 },
    <span class="c-blue">"origin_health"</span>: <span class="c-green">"HEALTHY"</span>,
    <span class="c-blue">"tcp_uplink"</span>: { <span class="c-blue">"retrans_packets"</span>: 0, <span class="c-blue">"lost_packets"</span>: 0, <span class="c-blue">"rtt_ms"</span>: 11.2, <span class="c-blue">"rttvar_ms"</span>: 1.8 }
  },
  <span class="c-blue">"confidence_monitor"</span>: { <span class="c-blue">"file"</span>: <span class="c-green">"/var/www/html/monitor.jpg"</span>, <span class="c-blue">"fps"</span>: 5, <span class="c-blue">"frames_written"</span>: 2625 }
}</div>
  </div>

  <h4>JSON Schema Field Dictionary:</h4>
  <table>
    <thead>
      <tr>
        <th style="width: 28%;">JSON Key Path</th>
        <th style="width: 14%;">Type</th>
        <th style="width: 58%;">Description & Units</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><code>youtube_health.status</code></td>
        <td><code>string</code></td>
        <td>Real-time safety state (<code>HEALTHY</code>, <code>BUFFER RISK: Underfeeding</code>, <code>BUFFER RISK: Video Frozen</code>).</td>
      </tr>
      <tr>
        <td><code>youtube_health.speed_factor</code></td>
        <td><code>float</code></td>
        <td>Calculated delivery ratio (&Delta;PTS / &Delta;Wall Clock). Values &lt; 0.90x trigger alerting.</td>
      </tr>
      <tr>
        <td><code>udp_in.packet_loss.cc_errors</code></td>
        <td><code>integer</code></td>
        <td>Number of 188-byte MPEG-TS packets dropped prior to entering the pipeline.</td>
      </tr>
      <tr>
        <td><code>rtmp_out.tcp_loss.rtt_ms</code></td>
        <td><code>float</code></td>
        <td>Real-time TCP Round-Trip Time to the RTMP destination (queried directly via kernel <code>TCP_INFO</code>).</td>
      </tr>
      <tr>
        <td><code>hls_out.active_segment.upload_time_ms</code></td>
        <td><code>float</code></td>
        <td>Microsecond precision HTTP PUT upload duration for the most recently completed chunk.</td>
      </tr>
      <tr>
        <td><code>rtmp_outputs[] / hls_outputs[]</code></td>
        <td><code>array</code></td>
        <td>Generated automatically in place of <code>rtmp_out</code>/<code>hls_out</code> when multiple outputs are configured.</td>
      </tr>
    </tbody>
  </table>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 7: MODE 5 (DIAGNOSTIC WARNINGS PART 1)-->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">6</span> Format 5: Diagnostic Warnings Reference (<code>[WARN]</code>)</h2>
  <p>The pipeline evaluates stream parameters every second and emits structured, timestamped warning logs to <code>stderr</code> whenever an anomaly threatens transmission or viewer playback.</p>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">1. UDP Multicast Continuity Loss</span>
      <span class="warn-source">src/pipeline.c:1435</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:01</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] UDP Multicast loss: 42 TS packets dropped in last second (Loss: 3.15%)!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Host network interface or local switch dropped UDP multicast packets before read (MPEG-TS CC gap).<br>
      <strong>Remedy:</strong> Increase kernel socket buffer (<code>-b 32</code>), increase FIFO (<code>-f 100</code>), or verify IGMP snooping.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">2. RTMP Uplink TCP Loss & Retransmissions</span>
      <span class="warn-source">src/pipeline.c:1442</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:02</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] RTMP uplink TCP loss: 14 packets lost in transit to YouTube!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Linux kernel detected outbound TCP packet loss and resent segments to YouTube's ingest endpoint.<br>
      <strong>Remedy:</strong> Check upload internet bandwidth; ensure uplink connection is not saturated by other host workloads.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">3. HLS Uplink TCP Loss (CDN Chunk Upload)</span>
      <span class="warn-source">src/pipeline.c:1481</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:03</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] HLS uplink TCP loss: 8 packets lost in transit to CDN/origin!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> HTTP PUT/POST chunk uploads encountered packet drops on the route to the CDN ingest edge.<br>
      <strong>Remedy:</strong> Verify CDN edge latency (<code>RTT</code>); check if the target CDN ingest cluster has regional congestion.
    </div>
  </div>

  <div class="warn-card alert-card">
    <div class="warn-header">
      <span class="warn-title">4. Video Track Stalled (Asymmetric Stream Freeze)</span>
      <span class="warn-source">src/pipeline.c:1447</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:04</span> <span class="c-red">[WARN] </span> <span class="c-red">[BUFFER RISK] Video track stalled for 1250 ms while audio continues! YouTube player will freeze.</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Audio continues flowing but video packets stopped arriving (&gt; 600ms). Viewers experience a frozen picture.<br>
      <strong>Remedy:</strong> Check source SDI/HDMI encoder hardware, camera capture lock, or video hardware capture card.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">5. Underfeeding Egress (Delivery Speed &lt; 1.0x)</span>
      <span class="warn-source">src/pipeline.c:1451</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:05</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] Underfeeding RTMP! Media delivery speed is 0.74x (&lt; 1.0x). YouTube player buffer is draining!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Media timestamp progression (&Delta;PTS) is slower than real-time. Players consume buffer faster than data arrives.<br>
      <strong>Remedy:</strong> Inspect upstream broadcast feed for dropped input frames or severe source CPU throttling.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">6. Keyframe Starvation (Waiting for IDR Frame)</span>
      <span class="warn-source">src/pipeline.c:1455</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:06</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] No video keyframe received for 5.2s! YouTube is waiting for an IDR keyframe to segment chunks.</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> YouTube Live requires an IDR keyframe every 2&ndash;4 seconds to split DASH/HLS segments.<br>
      <strong>Remedy:</strong> Set upstream encoder keyframe cadence to a fixed 2.0s interval (e.g. <code>-g 50 -keyint_min 50</code> at 25 fps).
    </div>
  </div>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 8: DIAGNOSTIC WARNINGS PART 2 & SYSLOG-->
<!-- ========================================== -->
<div>
  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">7. Excessive Keyframe Interval (GOP &gt; 4.0s)</span>
      <span class="warn-source">src/pipeline.c:845</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:07</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] Keyframe interval is 6.50s (> 4.0s). YouTube recommends 2-4s keyframes. Viewers may experience buffering!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Upstream encoder is transmitting keyframes too infrequently, causing YouTube Live to emit buffering alerts.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">8. Video Frame Rate Drop</span>
      <span class="warn-source">src/pipeline.c:1460</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:08</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] Video rate dropped to 11.4 fps! YouTube will buffer due to packet starvation.</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Video frame rate dropped below 15 fps after the broadcast was already running stably.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">9. RTMP Socket Write Blocked (Uplink Bufferbloat)</span>
      <span class="warn-source">src/pipeline.c:962</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:09</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] RTMP socket write blocked for 480 ms! Uplink internet bandwidth congestion detected.</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Linux TCP socket buffer filled completely, blocking execution of <code>av_interleaved_write_frame()</code> for &gt; 300ms.
    </div>
  </div>

  <div class="warn-card">
    <div class="warn-header">
      <span class="warn-title">10. Excessive A/V Sync Drift</span>
      <span class="warn-source">src/pipeline.c:1465</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:10</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">[BUFFER RISK] Excessive A/V sync drift: +740 ms. Player audio/video desync may cause buffering!</span></div>
    </div>
    <div class="warn-body">
      <strong>Root Cause:</strong> Audio PTS and Video PTS diverged by &gt; 600ms at the source encoder, risking player A/V buffer stalls.
    </div>
  </div>

  <div class="warn-card alert-card">
    <div class="warn-header">
      <span class="warn-title">11. Critical Lifecycle & Recovery Errors (<code>[ERROR]</code>)</span>
      <span class="warn-source">src/pipeline.c:1080 & main.c:14</span>
    </div>
    <div class="terminal" style="margin: 3px 0 4px 0;">
      <div class="terminal-body" style="padding: 4px 8px;"><span class="c-dim">2026-10-05 18:10:11</span> <span class="c-red">[ERROR]</span> <span class="c-red">Network read timeout exceeded (10 seconds with no data). Initiating reconnection...</span>
<span class="c-dim">2026-10-05 18:10:12</span> <span class="c-yellow">[WARN] </span> <span class="c-yellow">Received interrupt signal (SIGINT). Initiating graceful shutdown...</span>
<span class="c-dim">2026-10-05 18:10:13</span> <span class="c-green">[INFO] </span> Stream finalized cleanly. Total runtime: 01:24:12, Total bytes out: 1.48 GB.</div>
    </div>
    <div class="warn-body">
      <strong>Safety Handlers:</strong> Catches <code>SIGINT</code>/<code>SIGTERM</code>, flushes pending FIFO queues, writes valid FLV/HLS trailers, and cleanly tears down TCP sockets without corrupting CDN state.
    </div>
  </div>

  <h2><span class="section-num">7</span> Format 6: Systemd Journal & Enterprise File Logging</h2>
  <div class="terminal">
    <div class="terminal-header">
      <div class="terminal-dots"><span class="dot dot-red"></span><span class="dot dot-yellow"></span><span class="dot dot-green"></span></div>
      <div class="terminal-title">Systemd Unit Configuration: /etc/systemd/system/ixgIPStream.service</div>
    </div>
    <div class="terminal-body">[Unit]
Description=ixgIPStream v4.1 Zero-Encoding Streaming Pipeline
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=broadcast
ExecStart=/usr/local/bin/ixgIPStream \
    -i "udp://@239.255.0.1:1234" \
    -o rtmp "rtmp://a.rtmp.youtube.com/live2/xxxx-xxxx" \
    -o hls "http://origin.cdn.com/live/stream.m3u8" \
    -f 100 \
    -b 32 \
    -t 10 \
    --log-stats
Restart=always
RestartSec=3s
LimitNOFILE=65536
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target</div>
  </div>

  <div class="callout callout-info" style="margin-top: 5px;">
    <strong>Journal Monitoring Commands:</strong><br>
    • Real-time log monitoring: <code>journalctl -u ixgIPStream -f -o cat</code><br>
    • Filter for all buffer anomalies: <code>journalctl -u ixgIPStream -g "BUFFER RISK" --since "1 hour ago"</code>
  </div>
</div>

<div class="page-break"></div>

<!-- ========================================== -->
<!-- PAGE 9: TROUBLESHOOTING MATRIX & SUMMARY   -->
<!-- ========================================== -->
<div>
  <h2><span class="section-num">8</span> Diagnostic Lookup & Operational Troubleshooting Matrix</h2>
  <p>Rapidly diagnose viewer complaints, CDN alerts, or YouTube Live health changes using this operational reference table:</p>

  <table>
    <thead>
      <tr>
        <th style="width: 23%;">Observed Symptom</th>
        <th style="width: 25%;">Log Indicator / Metric</th>
        <th style="width: 26%;">Underlying Root Cause</th>
        <th style="width: 26%;">Recommended Operational Action</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><strong>Player Buffering Spinner</strong></td>
        <td><code>Speed &lt; 0.90x</code><br><code>[WARN] Underfeeding RTMP</code></td>
        <td>Multicast input feed is delivering media frames slower than wall clock time.</td>
        <td>Inspect upstream camera/encoder CPU; ensure source is not throttled or dropping input frames.</td>
      </tr>
      <tr>
        <td><strong>Player Buffering Spinner</strong></td>
        <td><code>Retrans &gt; 50</code><br><code>Lat &gt; 350ms</code></td>
        <td>Uplink internet congestion; packets dropped between host and RTMP server.</td>
        <td>Verify dedicated uplink bandwidth; check for other outbound network traffic saturation.</td>
      </tr>
      <tr>
        <td><strong>Video Freezes While Audio Plays</strong></td>
        <td><code>Silence(V/A): 1400/12ms</code><br><code>[WARN] Video track stalled</code></td>
        <td>Asymmetric input starvation: camera or video capture card locked up.</td>
        <td>Restart video source hardware; check SDI/HDMI physical cable connections.</td>
      </tr>
      <tr>
        <td><strong>YouTube Ingest Quality Degraded</strong></td>
        <td><code>GOP &gt; 4.5s</code><br><code>KF Age &gt; 4.0s</code></td>
        <td>Keyframe interval too long; YouTube cannot finalize HLS chunks in time.</td>
        <td>Reconfigure upstream encoder to a strict fixed 2.0s GOP (e.g. 50 frames at 25 fps).</td>
      </tr>
      <tr>
        <td><strong>HLS CDN Playback Errors</strong></td>
        <td><code>origin_health: TIMEOUT</code><br><code>Upload Speed &gt; 1500ms</code></td>
        <td>HTTP PUT/POST requests to CDN origin taking longer than segment duration.</td>
        <td>Check CDN origin health; switch to closer CDN ingest edge or check WAN egress routing.</td>
      </tr>
      <tr>
        <td><strong>Video Macroblocking & Glitches</strong></td>
        <td><code>UDP Loss &gt; 1.0%</code><br><code>CC Errors &gt; 0</code></td>
        <td>Local Ethernet packet drops or OS kernel UDP receive buffer overruns.</td>
        <td>Increase socket buffer: <code>-b 32</code>; increase FIFO: <code>-f 100</code>; check switch IGMP snooping.</td>
      </tr>
      <tr>
        <td><strong>Lip-Sync Desynchronization</strong></td>
        <td><code>Drift &gt; +600ms</code></td>
        <td>Audio and Video sampling clocks have drifted apart at the broadcast source.</td>
        <td>Lock source hardware encoder to a common reference clock (PCR / Genlock); reboot encoder.</td>
      </tr>
    </tbody>
  </table>

  <h3>Comprehensive CLI Reference Summary (ixgIPStream v4.1)</h3>
  <table>
    <thead>
      <tr>
        <th style="width: 25%;">CLI Flag</th>
        <th style="width: 25%;">Log / Telemetry Mode</th>
        <th style="width: 50%;">Recommended Deployment Context</th>
      </tr>
    </thead>
    <tbody>
      <tr>
        <td><code>(default)</code></td>
        <td>Interactive Dashboard</td>
        <td>Interactive terminal usage, testing, and manual stream monitoring.</td>
      </tr>
      <tr>
        <td><code>-l, --log-stats</code></td>
        <td>Standard Timestamped Log</td>
        <td>Production systemd services, daemon logging, file logs with <code>tee</code>, syslog.</td>
      </tr>
      <tr>
        <td><code>-d, --detailed</code></td>
        <td>Multi-Line Diagnostic Card</td>
        <td>Commissioning new links, troubleshooting unstable CDNs, deep diagnostics.</td>
      </tr>
      <tr>
        <td><code>-j, --json</code></td>
        <td>Structured JSON Stream</td>
        <td>Automated observability pipelines: Vector, FluentBit, Datadog, Prometheus, Grafana.</td>
      </tr>
      <tr>
        <td><code>-v, --verbose</code></td>
        <td>Debug Diagnostics</td>
        <td>Low-level FFmpeg packet tracing, demuxer/muxer timing inspection.</td>
      </tr>
      <tr>
        <td><code>-V, --version</code></td>
        <td>Version Information</td>
        <td>Prints <code>ixgIPStream version v4.1</code> and exits with code 0.</td>
      </tr>
    </tbody>
  </table>

  <div class="callout callout-info" style="margin-top: 10px;">
    <strong>ixgIPStream v4.1 Certification:</strong> This operational guide was compiled for <code>ixgIPStream v4.1</code> with native FFmpeg libraries (<code>libavformat</code>, <code>libavcodec</code>, <code>libavutil</code>, <code>libswscale</code>).
  </div>
</div>

</body>
</html>
"""

def generate_pdf():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.dirname(script_dir)
    out_pdf = os.path.join(root_dir, "ixgIPStream_Telemetry_and_Log_Guide.pdf")
    alt_pdf = os.path.join(root_dir, "Multicast2RTMP_Telemetry_and_Log_Guide.pdf")
    tmp_html = "/tmp/ixgipstream_guide.html"
    
    with open(tmp_html, "w", encoding="utf-8") as f:
        f.write(HTML_CONTENT)
    
    print(f"Generating PDF: {out_pdf} ...")
    weasyprint_bin = "/home/nityanand-pc/.local/bin/weasyprint"
    subprocess.run([weasyprint_bin, tmp_html, out_pdf], check=True)
    
    # Also update the legacy-named PDF file for complete backwards-compatibility
    shutil.copyfile(out_pdf, alt_pdf)
    
    size = os.path.getsize(out_pdf)
    print(f"Success! PDF generated at {out_pdf} ({size} bytes)")
    print(f"Also updated: {alt_pdf}")

if __name__ == "__main__":
    generate_pdf()
