# Development Plan

Language:
- English: `DEVELOPMENT_PLAN.md`
- Russian: [`RU_DEVELOPMENT_PLAN.md`](RU_DEVELOPMENT_PLAN.md)

## Goal

Build a stable and flexible RTSP camera solution for Luckfox boards, with clean MQTT control, solid Frigate and Home Assistant integration, and a path toward on-device detection.

## 0. [High Priority] Migrate the Main Video Pipeline to VI -> VPSS -> VENC

### Goal

Replace the current manual `VI -> RGA copy/resize -> VENC` approach with a more hardware-native `VI -> VPSS -> VENC` pipeline.

### Expected Outcome

- Lower CPU usage
- Less jitter on the main stream
- Better long-uptime stability
- Cleaner separation between main and sub-stream processing

### Work Items

- Add VPSS group and channel initialization for two branches:
  - `VPSS Ch0`: passthrough for the main stream
  - `VPSS Ch1`: scaling path for the sub stream, `640x360` by default
- Bind RK MPI modules:
  - `VI ch0 -> VPSS grp0`
  - `VPSS ch0 -> VENC ch0` for the main stream
  - `VPSS ch1 -> VENC ch1` for the sub stream
- Move mirror and flip into VPSS, or use ISP / sensor-level transform where supported
- Simplify the main loop so it primarily reads from VENC and forwards frames to RTSP
- Preserve runtime control of FPS and bitrate for main and sub streams through MQTT
- Recalculate GOP safely when FPS changes at runtime
- Add a fallback mode:
  - if VPSS setup or bind fails, allow the current RGA-based path to run as a backup mode

### Critical Considerations

- Startup and shutdown ordering for modules and bind/unbind operations must be correct to avoid leaked buffers and stuck restarts
- VI, VPSS, and VENC buffer depth must be tuned to prevent backpressure where sub-stream load affects the main stream
- PTS and time references must remain monotonic and consistent for stable RTSP playback in Frigate and FFmpeg
- MIS5001 and SC3336 compatibility must be preserved, especially for mirror, flip, exposure, and day/night behavior
- Diagnostics should expose the active pipeline mode, frame drops, stall events, and recovery timing

### Validation After Migration

Functional checks:
- Main RTSP stream at `/live/0` and sub stream at `/live/1` run simultaneously
- MQTT FPS and bitrate commands apply without restarting the process
- Mirror, flip, and day/night behavior remain correct on both supported sensors

Performance checks:
- CPU usage drops below the current baseline for a typical `2K main + 640x360 sub` profile
- Main-stream jitter and frame-time spikes are reduced
- Dropped frames do not increase when Frigate and VLC are connected at the same time

Reliability checks:
- Soak test for `24-72` hours without stalls or memory degradation
- Watchdog and restart path recover streaming correctly
- Service shutdown completes cleanly without stale binds or settings corruption

## 1. Microphone Validation

- Test USB, I2C, SPI, and analog microphone options where applicable
- Evaluate audio quality, latency, signal-to-noise ratio, artifacts, and real-world stability
- Select the most practical streaming path and AEC / AGC / HPF defaults for a stable release
- Document the results and finalize the `audio_mpi` technical profile

## 2. 3D-Printed Enclosure

- Design an enclosure that accounts for thermals, antenna clearance, connectors, and cable management
- Add dust protection and, where needed, basic condensation resistance
- Store STL files and assembly notes under `deploy/case/`

## 3. RTSP Authentication

- Add username and password protection for RTSP streams
- Support auth modes compatible with FFmpeg and Frigate
- Expose MQTT-configurable options such as `rtsp_user`, `rtsp_pass`, and `rtsp_auth_mode`
- Document setup, client compatibility, and fallback behavior

## 4. MQTT Data Cleanup and Expansion

- Review `state` and `telemetry` payloads and remove stale or redundant fields
- Add higher-value fields such as:
  - `stream_bandwidth_actual`
  - `substream_fps_effective`
  - `cpu_temp`, `soc_temp`, `voltage`, `power_w`
  - `video_dropped_frames`, `packet_loss` where feasible
  - `audio_status`, `mic_level`, `audio_error`
- Add `telemetry_mode=light|detailed`
- Keep the documented contract in sync in [`MQTT_CONTRACT.md`](MQTT_CONTRACT.md)

## 5. NPU-Based On-Device Detection

- Build an MVP for local person or motion detection on the sub stream using RKNN
- Create a pipeline such as `capture -> preprocess/postprocess -> inference -> MQTT alert`
- Keep power and thermal load under control with options like `detection_interval`, `skip_frames`, and `low_power`
- Add configuration for `edge_detection_enabled` and model selection
- Continue Frigate integration using MQTT events plus the RTSP detect stream

## 6. Additional Ideas

- OTA updates with a rollback-safe backup partition
- Lightweight local web UI over HTTPS for configuration, diagnostics, and live preview
- Power-saving modes: sleep, wake-on-motion, reduced FPS, reduced bitrate
- `camera_rtsp.json` v2 with versioning, checksum, and migration logic
- CI coverage for unit, integration, lint, and static analysis

## Priority Order

1. Critical: migrate to `VI -> VPSS -> VENC` and validate stability and performance
2. Security and correctness: RTSP auth, MQTT contract quality, robust settings persistence
3. Reliability: monitoring, telemetry, graceful shutdown, and clearer error-state handling
4. UX and hardware: enclosure work and polished documentation
5. Advanced feature set: on-device NPU detection