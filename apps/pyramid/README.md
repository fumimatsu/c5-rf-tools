# C5 Pyramid Monitor 0.2.3

AI Pyramid-Pro / Ubuntu 22.04 / Python 3.10 + system pySerial 3.5.
Target: an explicitly selected XIAO ESP32-C5 running unchanged C5MK 1.2.5+xiaoc5. Device serial is provided locally, not hardcoded.
The Tab5 C5 and FC are outside this application. This directory is separate from
the unrelated Blackbox Flight Analyzer checkout.

## Running

`python3 server.py --serial <C5_SERIAL> --data .local/data --bind 127.0.0.1 --port 8765`

Portable setup and deployment: ../../docs/development.md. For LAN operation pass --bind 0.0.0.0.
Version 0.2.2 includes the portable serial/environment settings and is deployed on the existing Pyramid.
[Scanner archives, independent labels, AI-source export, evidence and rollback](docs/0.2.2-recording-labels.md).

Version 0.2.3 changes the waterfall to blue/cyan/green/yellow/red with a numeric legend and gray hatching for missing samples. Absolute range: −10..+45 receiver dB; background delta: 0..+30 dB. Red means higher received power, not a fault. This version is source-only: the disconnected Pyramid remains on 0.2.2. The service template targets the future `/opt/c5-monitor/0.2.3` deployment. [Verification and deployment boundaries](../../docs/jet-waterfalls.md).

The systemd unit runs with DynamicUser and dialout, owns only this C5 serial
device, and stores config, background and trial passes in its StateDirectory.
The serial number is checked, not just Espressif VID/PID. Disconnect/reconnect
returns to monitor with timing disarmed and background adoption cleared.
HTTP is LAN-only operational software; no Internet exposure or cloud service
is configured. Control POSTs require the page token and matching Host/Origin.
No C5 flashing, arbitrary shell commands or arbitrary filesystem APIs exist.

## Modes

- Monitor: four selectable R/A/B/E/F channels. Equal-frequency aliases cannot
  occupy multiple seats. Linear-power averages in 100ms bins; missing/flagged
  samples remain missing. Receiver dB is not calibrated dBm or video quality.
- Spectrum/WF: 5645-5917MHz; 4MHz (69 points) or 2MHz (137). Measures eight
  frequencies per batch, checks returned frequencies, settles 25ms and collects
  25ms per batch. Straight segments and visible sample dots, no Gaussian peaks.
  Changing mode/profile clears the displayed spectrum/waterfall epoch.
- Background: explicit all-VTX-off confirmation, five complete 2MHz sweeps.
  Only complete valid sweeps can replace the previous background. Saved context
  includes device, firmware and g30/o10/bw1/n256. Old background is not adopted
  automatically; placement/antenna/environment still need operator confirmation.
- Timing trial: C5 native RotorHazard API36, four selected seats and four disabled
  seats. Enter/Exit are RH 1-254 units, NOT receiver dB. Polls validate checksums,
  selected index, frequencies and threshold readback. No simultaneous sweep.
  C5 decides crossing and plateau-midpoint time; host subtracts reported peak age
  from the request midpoint, retaining RTT in each event. 1ms protocol resolution
  is not a verified 1ms physical timing accuracy. A delayed poll/counter jump
  produces a rejected event and resets the interval baseline. First pass has
  no interval. Minimum-interval rejection does not advance the accepted baseline.
  USB loss, reconfiguration or restart disarms trial recording. Browser closure
  does not stop the service. Clock mapping across separate gates is unimplemented.

`passes.jsonl` contains local experimental events, including rejection reasons.
No broker credentials or MADSYSTEM destination have been configured. No events
are sent to MADSYSTEM. Its MQTT compatibility adapter, race start/stop contract,
checkpoint/pilot mapping and physical gate acceptance remain subsequent work.
The local MADSYSTEM FPVSIMMqttReceiver uses intervalMs for CP1, but does not use
peakTsMs and treats CP2+ as arrival-time checkpoint notifications; precise remote
checkpoint timing requires a receiver contract change, not just a JSON rename.

## Verification

`python3 -m unittest -v test_protocol test_recording test_portability test_scanner test_scanner_http`

Tests cover CRC failure/resynchronization, truncated frames, counter wrap,
late/multiple events, minimum interval, missing controls, duplicate frequencies,
stale values and explicit adoption of a saved background. `node --check ui.js` and `node --check labels-ui.js` checks the browser script. Actual Pyramid USB, mode switching, HTTP UI and physical
RF/gate validation must be recorded separately; no simulation is shown as RF.

Original HANDOFF_CODEX.md in FPVGateC5MK describes an earlier UART-based Pyramid
plan. This application implements the USB direction selected in the conversation.
Official Pyramid specs call one USB-A internal, but the actual user's FPC cables
already lead to external connectors; no free internal USB-A is assumed.

Licence and original protocol attribution: NOTICE.txt.

## 0.2.0: native archive and desktop review

Four channel panels, thresholds and timed extrema remain visible beside recording
selection and live diagnostics. Layout expands above 2200px; smaller screens stack.
In 計測・記録, 計測履歴を記録開始 starts a session; 記録停止 finalizes it.
Select an archive in the right column and choose 選択記録を再生. Seeking and 1x
playback only read the saved session and do not reconfigure or stop the receiver.
Live service/recording diagnostics remain labelled live during replay.

Each session is a SQLite database in StateDirectory/recordings, schema
c5-native-recording/1. It contains settings, serial/firmware, host monotonic origin,
raw 16-byte RH snapshots, counter/age/peak/nadir/crossing, timed history extrema,
host receipt time/RTT, surrounding C5 millisecond clock reads, and detector events.
The C5 clock bracket is diagnostic, not a proven cross-device synchronization.
Clock wrap is unsigned 32-bit. Saturated ages, inconsistent held spans or >100ms
clock windows are flagged. >500ms poll gaps break drawn lines; existing >1s pass
gap rejection remains unchanged. History extrema are selected snapshots, not all
1kHz samples; replay never reruns the native detector or tests new thresholds.

The bounded asynchronous sample writer commits approximately every 0.5s. Queue
overflow increments dropped and preserves sequence gaps; such archives finish
incomplete. Storage failures are visible and stop recording. Graceful service stop
drains the queue. Abrupt power loss can lose the latest uncommitted fraction;
unfinished archives are shown interrupted. No deletion/retention job is enabled.
Monitor disk space before long sessions. The legacy passes.jsonl event append
remains for the previous download path.

GET /api/recordings lists the latest 40 sessions; /<id>/window returns a bounded
read-only window (maximum 12000 rows with explicit truncated flag). /<id>/export
downloads NDJSON including metadata. Archives retain channel/threshold context.
The paragraph above describes the retained native format. Version 0.2.2 adds independent human labels and a separate scanner format; see the linked 0.2.2 contract. Automatic threshold adoption, AI classification and MADSYSTEM sending remain unimplemented. Physical gate accuracy remains untested.
