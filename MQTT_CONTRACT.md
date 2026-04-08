# MQTT Contract

This document defines MQTT topics and payloads for `luckfox_camera_rtsp`.

## Topic Prefix

All runtime topics are under:

- `<node_id>/...`

Default `node_id`: `luckfox_camera`

## RTSP Streams

- **Main stream**: `rtsp://<IP>:554/live/0` — full resolution, for Frigate `record` role
- **Sub stream**: `rtsp://<IP>:554/live/1` — low resolution, for Frigate `detect` role (video-only, no audio)

## Core Topics

- `<node_id>/availability` (retained)
  - payload: `online` or `offline`
  - `offline` is published explicitly on clean shutdown and by broker using MQTT LWT on unclean disconnect.

- `<node_id>/state` (retained)
  - camera configuration and modes only.
  - no volatile telemetry fields.

- `<node_id>/telemetry` (non-retained)
  - runtime diagnostics and stream counters.

- `<node_id>/ack` (non-retained)
  - command processing result.
  - payload JSON: `{"param":"...","status":"ok|error","message":"..."}`

- `<node_id>/<param>/set`
  - command topic for updates.

## Home Assistant Discovery

Discovery topics are retained and published:

1. On each successful MQTT connect.
2. On `homeassistant/status = online`.
3. Optionally by interval when `--mqtt-discovery-refresh <sec>` is set to a value greater than `0`.

Default behavior: periodic discovery refresh is disabled.

## `state` Payload

Example:

```json
{
  "brightness": 128,
  "contrast": 128,
  "saturation": 128,
  "hue": 128,
  "sharpness": 50,
  "daynight": "color",
  "wb_preset": "auto",
  "mirror": "OFF",
  "flip": "OFF",
  "anti_flicker_en": "ON",
  "anti_flicker_mode": "50hz",
  "bitrate_kbps": 8000,
  "fps": 25,
  "sub_bitrate_kbps": 512,
  "sub_fps": 10,
  "night_mode": "OFF"
}
```

Sub stream resolution (`sub_width`/`sub_height`) is set at compile time and is not adjustable via MQTT.

Audio fields are included in `state` only when audio support is enabled (`--audio on`). Audio is present only on the main stream. **Audio support is experimental** — available only on Luckfox Pico Ultra (built-in microphone); sound quality is not yet satisfactory.

When audio support is disabled in the build, retained audio discovery entities are explicitly deleted on the next MQTT connect so Home Assistant does not keep stale controls from an older audio-enabled firmware.

## `telemetry` Payload

Example:

```json
{
  "board_status": "online",
  "cpu_usage_pct": 26,
  "memory_usage_pct": 41,
  "memory_used_mb": 83,
  "board_uptime_s": 15432,
  "runtime_s": 743,
  "soc_temp_c": 57,
  "actual_fps": 24,
  "actual_bitrate_kbps": 7810,
  "sub_actual_fps": 10,
  "sub_actual_bitrate_kbps": 498,
  "root_fs_usage_pct": 8,
  "root_fs_avail_mb": 5324,
  "userdata_fs_usage_pct": 0,
  "userdata_fs_avail_mb": 228,
  "oem_fs_usage_pct": 11,
  "oem_fs_avail_mb": 415
}
```

Notes:

- `runtime_s` is calculated from monotonic clock (`CLOCK_MONOTONIC`) and does not depend on wall-clock jumps.
- Telemetry is published periodically and on connect.
- Home Assistant telemetry entities are published with `expire_after`, so stale values become unavailable if fresh telemetry stops arriving.

## Command Validation

Invalid command payloads do not change camera state.

Examples:

- `brightness`: integer range `0..255`
- `sharpness`: integer range `0..100`
- `bitrate_kbps`: integer range `1000..20000`
- `fps`: integer range `10..30`
- `sub_bitrate_kbps`: integer range `100..5000`
- `sub_fps`: integer range `5..30` (effective stream output is capped by current main `fps`)
- `mirror`, `flip`, `anti_flicker_en`: `ON/OFF` or `1/0`
- `daynight`: `color` or `grayscale`
- `anti_flicker_mode`: `50hz` or `auto`
- `night_mode`: `ON` or `OFF`
  - `ON`: applies low-light ISP profile:
    - grayscale,
    - main stream `fps=15`, `bitrate_kbps=12288`,
    - AE mode forced to AUTO.
  - `OFF`: restores day profile (cached `fps`/`bitrate`), switches back to color and re-applies safe day HW profile (AE/AWB AUTO).
- `wb_preset`: one of named presets or integer `0..7`

Each command publishes `<node_id>/ack` with status `ok` or `error`.
