# Pyramid C5 RF / timing automation

## Purpose

The project is a local RF measurement and timing assistant, not only an HTML
dashboard. User priority: automate detection of RF changes and adjustment of
timing thresholds. Use CPU statistics, replay and labelled measurements first;
use the NPU when a tested model improves detection or reduces operator work.
Details and proposed phases: docs/automation-plan.md.

## Current priority (2026-11-14)

Implement C5 four-VTX comparison measurement first, then evaluate AI/NPU using
the saved evidence. Read `../../docs/four-wave-comparison.md` for the ten states,
raw scanner power, linear-addition comparison, validity and acceptance contract.
The ten-state comparison mode is planned, not implemented. The subsequent user instruction authorized the 0.2.2 scanner-recording, independent-label and training-source foundation; it is implemented and deployed. Read docs/0.2.2-recording-labels.md. Preserve native records and keep units and label provenance distinct.

## Boundaries

- C5MK 1.2.5+xiaoc5 on the explicitly configured USB serial. Host/serial/auth settings are local.
  The separate Tab5 and its C5 are outside this application's device ownership.
- Version 0.2.2 preserves native archives and adds scanner payloads, a separate append-only human-label ledger and training-source export. AI classification,
  automatic threshold adoption and MADSYSTEM publication are not implemented.
- Preserve receiver dB versus RH 1-254 units, missing data, CRC/RF errors and
  measurement provenance. No dBm/SNR/interference-percentage or VTX-fault claims.
- Moving drones normally change received strength. Do not make a fixed-position
  normal level the primary flight-health or timing criterion.
- Learn gate crossings versus non-gate peaks using independent labels. Existing
  detections or MADSYSTEM laps from the same RF input are not independent truth.
- Anomaly alerts and timing threshold control are separate decisions. Suspected
  interference is not a valid reason to silently raise thresholds or drop laps.
- Candidate thresholds need shadow comparison, versioned changes and rollback.
  The initial deployment applies approved settings at clear/race boundaries;
  mid-race adaptation is future work requiring separate continuity validation,
  because C5 threshold writes cancel an in-progress crossing.
- Current RH snapshots/extrema do not represent an archived full 1kHz trace.
  No lossless replay or native-equivalent recalculation claim without proof.
- One C5 radio cannot provide uninterrupted gate sampling and wide sweeps at
  once. NPU speed does not change this observation limit.

## Verification

Run `python -m unittest -v test_protocol test_recording test_portability test_scanner test_scanner_http` and `node --check ui.js` and `node --check labels-ui.js` after
UI/protocol changes. Check the actual Pyramid and browser, not only local mocks.
RF ON/OFF is not gate-pass accuracy. Report unverified physical passes, USB
reconnection and whole-device restart separately from service restart tests.
Preserve 0.1.0 evidence and firmware backups; use a new version for the next
runtime deployment. Keep protocol attribution and CC BY-NC-SA 4.0 notice.
