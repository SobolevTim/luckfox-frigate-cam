# Luckfox Camera RTSP

Stable RTSP camera firmware for Luckfox Pico boards, designed for Frigate and Home Assistant integration.

Language:
- English: `README.md`, `DEVELOPMENT_PLAN.md`
- Russian: [`RU_README.md`](RU_README.md), [`RU_DEVELOPMENT_PLAN.md`](RU_DEVELOPMENT_PLAN.md)

## Overview

This project provides a stable H.264 RTSP stream for **MIS5001** and **SC3336** camera sensors on **Luckfox Pico Ultra W** and related **RV1106** boards.

The video path is hardware-driven end to end: **ISP -> VI -> VENC**. There are no overlays and no CPU-based YUV to RGB conversion in the main streaming path.

Image parameters and stream settings are controlled over **MQTT**, with automatic **Home Assistant MQTT Discovery**. After the camera connects to the broker, Home Assistant can create the device and entities automatically.

> Audio support is currently experimental. The onboard microphone is available only on **Luckfox Pico Ultra** class boards, and sound quality is not considered final yet.

---

## Features

| Feature | Status |
|---|---|
| RTSP H.264 main stream (sensor-native profile: 2K or 1080p) | Yes |
| RTSP sub stream for Frigate detect (RGA downscale) | Yes |
| RTSP audio (G.711A / PCMA) | Experimental |
| Hardware ISP via rkaiq | Yes |
| No CPU-based YUV to RGB conversion | Yes |
| Brightness / Contrast / Saturation / Hue | Yes |
| Sharpness | Yes |
| Day / night grayscale mode | Yes |
| White balance: auto + 7 presets | Yes |
| Mirror / Flip | Yes |
| Anti-flicker: 50 Hz / auto | Yes |
| Runtime bitrate updates | Yes |
| Runtime FPS updates | Yes |
| MQTT + Home Assistant auto-discovery | Yes |
| Input validation and rejection of invalid values | Yes |
| Persistent settings storage | Yes |
| Graceful shutdown on SIGINT / SIGTERM | Yes |

---

## Requirements

### Hardware

- Primary validated platform: **Luckfox Pico Max (RV1106)**
- Additional validated platform: **Luckfox Pico Ultra W (RV1106)** with integrated Wi-Fi
- Supported sensors: **MIS5001** or **SC3336**
- Recommended network: Ethernet LAN for fixed installations; Pico Ultra W can also use Wi-Fi
- Base OS: standard Luckfox **Buildroot + BusyBox** image

### Platform and Firmware

- Boards run the standard Luckfox Buildroot-based system image.
- Flashing instructions and official firmware images are documented in the Luckfox wiki:
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/Flash-image
- Wi-Fi configuration for Pico Ultra W is documented here:
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/WiFi-BT#1-wifi
- For stable Frigate and Home Assistant deployments, a static IP is recommended:
  https://wiki.luckfox.com/Luckfox-Pico-Ultra/Autostart#4-configuring-a-static-ip

### Luckfox Pico Boards

- `Luckfox Pico Max`: RV1106G3, 256 MB DDR3L, 256 MB SPI NAND, MIPI CSI 2-lane, RGB666 DPI, 100M Ethernet, USB 2.0 Host/Device, 26 GPIO
- `Luckfox Pico Ultra W`: RV1106G3, 256 MB DDR3L, 8 GB eMMC, MIPI CSI 2-lane, RGB666 DPI, 2.4 GHz Wi-Fi 6, Bluetooth 5.2/BLE, USB 2.0 Host/Device, 33 GPIO

### Audio

- The onboard microphone is available only on **Luckfox Pico Ultra** hardware.
- Building with `--audio on` enables **G.711A (PCMA)** in the main RTSP stream.
- Audio support is still experimental and should not be considered production-finished.

### SDK Dependencies

Rockchip MPI headers and libraries are already included in the repository under `sdk/`.
Only the Luckfox SDK toolchain path is required for building: `LUCKFOX_SDK_PATH`.

---

## Build

```bash
# 1. Point to the Luckfox SDK toolchain
export LUCKFOX_SDK_PATH=<path-to-luckfox-sdk>

# 2. Build with the default sensor profile (MIS5001)
chmod +x build.sh
./build.sh

# 3. Build for SC3336 (1080p)
./build.sh --sensor SC3336

# 4. Build with audio enabled (experimental)
./build.sh --sensor MIS5001 --audio on
```

Sensor profiles:
- `MIS5001` (default): `2592x1944 @ 25 fps`
- `SC3336`: `1920x1080 @ 25 fps`

The main stream can be overridden via CMake:
`-DSTREAM_WIDTH=... -DSTREAM_HEIGHT=... -DSTREAM_FPS=...`

Sub-stream defaults are `640x360 @ 10 fps, 512 kbps` and can be overridden with:
`-DSUB_STREAM_WIDTH=... -DSUB_STREAM_HEIGHT=... -DSUB_STREAM_FPS=... -DSUB_STREAM_BITRATE_KBPS=...`

Audio-related CMake options:
- `-DENABLE_AUDIO=ON|OFF`
- `-DAUDIO_SAMPLE_RATE=8000|16000`
- `-DAUDIO_CHANNELS=1|2`

The resulting binary is generated at `install/uclibc/bin/luckfox_camera_rtsp`.

---

## Deploy to Device

```bash
# Copy the runtime bundle to the board
scp -r install/uclibc/ root@<CAMERA_IP>:/opt/camera_rtsp/

# Start the service manually
ssh root@<CAMERA_IP>
/opt/camera_rtsp/bin/luckfox_camera_rtsp \
  --mqtt-host <BROKER_IP> \
  --mqtt-port 1883 \
  --mqtt-user <user> \
  --mqtt-pass <password>
```

### Runtime Parameters

| Parameter | Default | Description |
|---|---|---|
| `--mqtt-host` | `127.0.0.1` | MQTT broker IP or hostname |
| `--mqtt-port` | `1883` | Broker TCP port (`1..65535`) |
| `--mqtt-user` | none | MQTT username |
| `--mqtt-pass` | none | MQTT password |
| `--mqtt-id` | `luckfox_camera` | MQTT client ID and topic prefix |
| `--mqtt-name` | `Luckfox Camera` | Display name in Home Assistant |
| `--mqtt-discovery-refresh` | `0` | Discovery republish interval in seconds; `0` disables it |

### Autostart via init.d

Examples are provided in `deploy/`:

```bash
cp deploy/run_camera.sh.example deploy/run_camera.sh
cp deploy/S99camera.example deploy/S99camera

# Edit deploy/run_camera.sh and set MQTT_HOST, MQTT_USER, MQTT_PASS, etc.

scp deploy/run_camera.sh root@<CAMERA_IP>:/opt/camera_rtsp/run_camera.sh
scp deploy/S99camera root@<CAMERA_IP>:/etc/init.d/S99camera
ssh root@<CAMERA_IP> chmod +x /opt/camera_rtsp/run_camera.sh /etc/init.d/S99camera
```

Example variables in `run_camera.sh`:

```sh
MQTT_HOST="<BROKER_IP>"
MQTT_USER="<user>"
MQTT_PASS="<password>"
MQTT_ID="luckfox_camera"
MQTT_NAME="Luckfox Camera"
```

---

## Frigate Integration

Recommended starting profile:

- Main stream: H.264, `2592x1944`, `25 fps`, `10240 kbps`
- Sub stream: H.264, `640x360`, `10 fps`, `512 kbps`
- Effective `sub_fps` cannot exceed the current main stream FPS
- GOP is automatically aligned to the active FPS, approximately one I-frame per second
- With `--audio on`, RTSP audio is exposed only on the main stream

```yaml
# frigate/config.yaml
cameras:
  luckfox:
    ffmpeg:
      inputs:
        - path: rtsp://<CAMERA_IP>:554/live/0
          roles:
            - record
            # - audio
        - path: rtsp://<CAMERA_IP>:554/live/1
          roles:
            - detect
    detect:
      width: 640
      height: 360
      fps: 10
```

---

## MQTT Control

The camera publishes **MQTT Discovery** on each successful broker connection.
Home Assistant can automatically create the **Luckfox Camera** device with all supported entities.

### Topics

| Topic | Purpose |
|---|---|
| `luckfox_camera/availability` | `online` / `offline` via LWT |
| `luckfox_camera/state` | retained JSON with camera settings |
| `luckfox_camera/telemetry` | non-retained JSON with diagnostics and runtime metrics |
| `luckfox_camera/ack` | non-retained command acknowledgements (`ok` / `error`) |
| `luckfox_camera/<param>/set` | control commands |

Detailed topic contract and payload examples are documented in [`MQTT_CONTRACT.md`](MQTT_CONTRACT.md).

Additional telemetry storage fields:
- `root_fs_usage_pct`, `root_fs_avail_mb`
- `userdata_fs_usage_pct`, `userdata_fs_avail_mb`
- `oem_fs_usage_pct`, `oem_fs_avail_mb`

`runtime_s` is calculated from monotonic time, so it does not go negative if the system clock changes.

Telemetry sensors in Home Assistant are published with `expire_after`, so runtime metrics such as FPS, uptime, and similar counters automatically become unavailable when the board stops sending fresh updates.

When built with `--audio on`, the `state` payload also includes audio fields:
- `audio_runtime_enabled`
- `audio_adc_alc_left_gain`
- `audio_adc_mic_left_gain`
- `audio_hpf`
- `audio_adc_micbias_voltage`
- `audio_adc_mode`

Audio-related MQTT commands are applied only when `audio_runtime_enabled=ON`.

### Manual Control Examples

```bash
# Subscribe to the current state
mosquitto_sub -h <BROKER_IP> -t 'luckfox_camera/state'

# Set brightness
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/brightness/set -m 100

# Force night mode
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/night_mode/set -m ON

# Toggle grayscale mode directly
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/daynight/set -m grayscale

# White balance preset
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/wb_preset/set -m daylight

# Enable mirror
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/mirror/set -m ON

# Reduce bitrate
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/bitrate_kbps/set -m 4096

# Lower FPS
mosquitto_pub -h <BROKER_IP> -t luckfox_camera/fps/set -m 15
```

All numeric values are validated. Out-of-range or invalid payloads are rejected, and the previous state is preserved.

### Parameter Matrix

| Parameter | Values | Description |
|---|---|---|
| `brightness` | `0`-`255` | Image brightness |
| `contrast` | `0`-`255` | Image contrast |
| `saturation` | `0`-`255` | Image saturation |
| `hue` | `0`-`255` | Image hue |
| `sharpness` | `0`-`100` | Image sharpness |
| `daynight` | `color` / `grayscale` | Day-night mode |
| `wb_preset` | `auto`, `incandescent`, `fluorescent`, `warm_fluorescent`, `daylight`, `cloudy`, `twilight`, `shade` | White balance preset |
| `mirror` | `ON` / `OFF` | Horizontal mirror |
| `flip` | `ON` / `OFF` | Vertical flip |
| `anti_flicker_en` | `ON` / `OFF` | Anti-flicker enable |
| `anti_flicker_mode` | `50hz` / `auto` | Anti-flicker mode |
| `night_mode` | `ON` / `OFF` | Night profile: grayscale + lower FPS + higher bitrate |
| `bitrate_kbps` | `1000`-`20000` | Video bitrate |
| `fps` | `10`-`30` | Main stream FPS; GOP follows FPS |
| `sub_fps` | `5`-`30` | Sub-stream FPS; effectively capped by main FPS |
| `audio_adc_alc_left_gain` | `0`-`31` | ADC PGA gain |
| `audio_adc_mic_left_gain` | `0`-`3` | MIC booster gain |
| `audio_hpf` | `ON` / `OFF` | High-pass filter |
| `audio_adc_micbias_voltage` | `VREFx0_8` to `VREFx0_975` | Microphone bias voltage |
| `audio_adc_mode` | `SingadcL`, `DiffadcL`, `SingadcR`, ... | ADC capture mode |

### Default Values

| Parameter | Value |
|---|---|
| Resolution | `MIS5001: 2592x1944`, `SC3336: 1920x1080` |
| Codec | `H.264` |
| FPS | `25` |
| GOP | `25` |
| Bitrate | `10240 kbps` |

---

## Home Assistant Integration

1. Configure MQTT in Home Assistant under **Settings -> Devices & Services -> MQTT**.
2. Start the camera with `--mqtt-host <broker-ip>`.
3. After a few seconds, Home Assistant should discover **Luckfox Camera** automatically.

Example automations are available in [`ha_integration/configuration.yaml`](ha_integration/configuration.yaml).

---

## Project Layout

```text
├── build.sh
├── CMakeLists.txt
├── README.md
├── RU_README.md
├── DEVELOPMENT_PLAN.md
├── RU_DEVELOPMENT_PLAN.md
├── MQTT_CONTRACT.md
├── deploy/
│   ├── S99camera.example
│   └── run_camera.sh.example
├── ha_integration/
│   └── configuration.yaml
├── sdk/
│   ├── include/
│   └── lib/
├── include/
│   ├── audio_mpi.h
│   ├── camera_mpi.h
│   ├── isp_control.h
│   └── mqtt_client.h
└── src/
    ├── main.cc
    ├── audio_mpi.cc
    ├── camera_mpi.cc
    ├── isp_control.cc
    └── mqtt_client.cc
```

---

## Notes

- The current implementation is focused on stable RTSP streaming and robust MQTT-based control.
- Audio remains experimental.
- The forward-looking roadmap is documented in [`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md).