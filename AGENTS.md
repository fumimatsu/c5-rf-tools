# C5 RF Tools

- Read `docs/handoff.md`, then the target application's AGENTS.md. This is one repository with independent Tab5 and Pyramid applications; do not merge their runtime or release versions.
- `apps/tab5` owns the ESP32-P4 display firmware. `apps/pyramid` owns the Linux host app. C5MK RF firmware is an external dependency; it is not modified by normal UI/host work.
- Do not claim receiver dB is dBm/SNR/quality. Preserve missing/invalid samples and distinguish RH 1..254 from scanner dB. One C5 cannot continuously time gates while sweeping the wide band.
- Keep real serials, IP/auth settings, flash backups and measurements under ignored `.local/` or on the device. Never replace recorded measurements with simulation.
- Local tests/builds do not flash hardware. Before flashing or deploying, identify the exact device, compare current source/runtime versions and preserve rollback/state. No other COM device is in scope.
- Run the relevant Python tests + JS syntax check for Pyramid, or ESP-IDF 5.4.2 build for Tab5. Report actual device verification separately.
- Next approved feature (2026-11-14): Pyramid four-VTX comparison measurement, then AI/NPU evaluation. Read `docs/four-wave-comparison.md`; this supersedes the earlier gate-label-first priority. 0.2.2 now implements scanner recording and independent human-label infrastructure by the subsequent user instruction. The ten-state comparison wizard, threshold candidates and AI/NPU remain later work. Read apps/pyramid/docs/0.2.2-recording-labels.md for the current runtime and evidence. Do not silently implement adaptive thresholds or publish timing to MADSYSTEM.
- Preserve LICENSE, notices, upstream attribution and separate third-party licences.
