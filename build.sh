#!/bin/bash
# Build script for luckfox_camera_rtsp (Stable RTSP for Frigate + HA control)
# Uses uclibc toolchain from LUCKFOX_SDK_PATH.

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)

SENSOR_PROFILE="${CAMERA_SENSOR:-MIS5001}"
AUDIO_BUILD="${ENABLE_AUDIO:-off}"
AUDIO_SAMPLE_RATE="${AUDIO_SAMPLE_RATE:-16000}"
AUDIO_CHANNELS="${AUDIO_CHANNELS:-2}"

print_usage() {
    echo "Usage: $0 [clean] [--sensor MIS5001|SC3336] [--audio on|off] [--sample-rate 8000|16000] [--channels 1|2]"
    echo ""
    echo "Examples:"
    echo "  $0"
    echo "  $0 --sensor SC3336"
    echo "  $0 --sensor MIS5001 --audio on"
    echo "  $0 --sensor MIS5001 --audio on --sample-rate 16000"
    echo "  $0 --sensor MIS5001 --audio on --sample-rate 16000 --channels 2"
    echo "  $0 clean"
}

while [ $# -gt 0 ]; do
    case "$1" in
        clean)
            rm -rf "${ROOT_PWD}/build" "${ROOT_PWD}/install"
            echo "Cleaned build/ and install/"
            exit 0
            ;;
        --sensor)
            shift
            if [ -z "$1" ]; then
                echo "ERROR: --sensor requires a value"
                print_usage
                exit 1
            fi
            SENSOR_PROFILE="$1"
            ;;
        --audio)
            shift
            if [ -z "$1" ]; then
                echo "ERROR: --audio requires on or off"
                print_usage
                exit 1
            fi
            AUDIO_BUILD="$1"
            ;;
        --sample-rate)
            shift
            if [ -z "$1" ]; then
                echo "ERROR: --sample-rate requires 8000 or 16000"
                print_usage
                exit 1
            fi
            AUDIO_SAMPLE_RATE="$1"
            ;;
        --channels)
            shift
            if [ -z "$1" ]; then
                echo "ERROR: --channels requires 1 or 2"
                print_usage
                exit 1
            fi
            AUDIO_CHANNELS="$1"
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
    shift
done

SENSOR_PROFILE=$(echo "$SENSOR_PROFILE" | tr '[:lower:]' '[:upper:]')
case "$SENSOR_PROFILE" in
    MIS5001|SC3336)
        ;;
    *)
        echo "ERROR: Unsupported sensor profile '$SENSOR_PROFILE'."
        echo "Supported: MIS5001, SC3336"
        exit 1
        ;;
esac

AUDIO_BUILD=$(echo "$AUDIO_BUILD" | tr '[:upper:]' '[:lower:]')
case "$AUDIO_BUILD" in
    on|off)
        ;;
    *)
        echo "ERROR: Unsupported --audio value '$AUDIO_BUILD'."
        echo "Supported: on, off"
        exit 1
        ;;
esac

case "$AUDIO_SAMPLE_RATE" in
    8000|16000)
        ;;
    *)
        echo "ERROR: Unsupported --sample-rate value '$AUDIO_SAMPLE_RATE'."
        echo "Supported: 8000, 16000"
        exit 1
        ;;
esac

case "$AUDIO_CHANNELS" in
    1|2)
        ;;
    *)
        echo "ERROR: Unsupported --channels value '$AUDIO_CHANNELS'."
        echo "Supported: 1, 2 (RV1106 requires 2 for onboard mic)"
        exit 1
        ;;
esac

if [ "$AUDIO_BUILD" = "on" ]; then
    CMAKE_AUDIO_FLAG="ON"
else
    CMAKE_AUDIO_FLAG="OFF"
fi

LIBC="uclibc"

echo ""
echo "=== Luckfox Camera RTSP Build (${LIBC}) ==="
echo "Sensor profile: ${SENSOR_PROFILE}"
echo "Audio: ${AUDIO_BUILD} (${AUDIO_SAMPLE_RATE} Hz, ${AUDIO_CHANNELS} ch)"
echo ""

# ── Check toolchain ──────────────────────────────────────────────────────────
if [ -z "$LUCKFOX_SDK_PATH" ]; then
    echo "ERROR: LUCKFOX_SDK_PATH is not set."
    echo "  export LUCKFOX_SDK_PATH=<luckfox-pico SDK path>"
    exit 1
fi

TOOLCHAIN="${LUCKFOX_SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "ERROR: Toolchain not found at:"
    echo "  ${TOOLCHAIN}"
    echo "Check that LUCKFOX_SDK_PATH points to the correct SDK root."
    exit 1
fi
echo "Toolchain: ${TOOLCHAIN}"

# ── SDK dir (headers + prebuilt libs, bundled in sdk/) ────────────────────────
SCRIPT_DIR="${ROOT_PWD}"

if [ -z "${MPI_SDK_DIR}" ]; then
    BUNDLED_SDK="${SCRIPT_DIR}/sdk"
    if [ -d "${BUNDLED_SDK}" ]; then
        export MPI_SDK_DIR="${BUNDLED_SDK}"
    else
        echo ""
        echo "ERROR: Cannot find bundled sdk/ directory"
        echo "  Expected at: ${BUNDLED_SDK}"
        echo ""
        echo "Clone the repo with LFS or set the path manually:"
        echo "  export MPI_SDK_DIR=/path/to/sdk"
        echo "  ./build.sh"
        exit 1
    fi
fi
echo "SDK:       ${MPI_SDK_DIR}"

# ── Build ────────────────────────────────────────────────────────────────────
BUILD_DIR="${ROOT_PWD}/build"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DLIBC_TYPE="$LIBC" \
    -DMPI_SDK_DIR="${MPI_SDK_DIR}" \
    -DCAMERA_SENSOR="${SENSOR_PROFILE}" \
    -DENABLE_AUDIO="${CMAKE_AUDIO_FLAG}" \
    -DAUDIO_SAMPLE_RATE="${AUDIO_SAMPLE_RATE}" \
    -DAUDIO_CHANNELS="${AUDIO_CHANNELS}" \
    -DCMAKE_BUILD_TYPE=Release

if [ $? -ne 0 ]; then
    echo "CMake configuration failed!"
    exit 1
fi

if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
else
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
fi

make -j"$JOBS"

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

make install

echo ""
echo "=== Build successful ==="
echo "Binary location: ${ROOT_PWD}/install/${LIBC}/bin/luckfox_camera_rtsp"
echo ""
echo "Deploy to device:"
echo "  scp -r ${ROOT_PWD}/install/${LIBC}/ root@<CAMERA_IP>:/opt/camera_rtsp/"
echo ""
echo "Run on device:"
echo "  /opt/camera_rtsp/bin/luckfox_camera_rtsp"
echo ""
echo "RTSP main:    rtsp://<CAMERA_IP>:554/live/0"
echo "RTSP sub:     rtsp://<CAMERA_IP>:554/live/1  (detect)"
if [ "$AUDIO_BUILD" = "on" ]; then
    echo "RTSP audio:   G.711A (PCMA) @ ${AUDIO_SAMPLE_RATE} Hz ${AUDIO_CHANNELS}ch, main stream only"
fi
