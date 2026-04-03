cmake_minimum_required(VERSION 3.10)

# ─── Toolchain selection ───────────────────────────────────────────────────────
if(DEFINED LIBC_TYPE)
    message(STATUS "libc type: ${LIBC_TYPE}")
else()
    message(FATAL_ERROR "No libc type specified. Use -DLIBC_TYPE=glibc or uclibc")
endif()

set(SDK_PATH $ENV{LUCKFOX_SDK_PATH})
set(GLIBC_COMPILER $ENV{GLIBC_COMPILER})

if(${LIBC_TYPE} STREQUAL "uclibc")
    if(NOT SDK_PATH)
        message(FATAL_ERROR "Set LUCKFOX_SDK_PATH env. e.g.: export LUCKFOX_SDK_PATH=/home/user/luckfox-pico")
    endif()
    set(CMAKE_C_COMPILER
        "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
    set(CMAKE_CXX_COMPILER
        "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")
elseif(${LIBC_TYPE} STREQUAL "glibc")
    if(NOT GLIBC_COMPILER)
        message(FATAL_ERROR "Set GLIBC_COMPILER env. e.g.: export GLIBC_COMPILER=/opt/arm-linux-gnueabihf/bin/arm-linux-gnueabihf-")
    endif()
    set(CMAKE_C_COMPILER   "${GLIBC_COMPILER}gcc")
    set(CMAKE_CXX_COMPILER "${GLIBC_COMPILER}g++")
else()
    message(FATAL_ERROR "Unknown LIBC_TYPE: ${LIBC_TYPE}")
endif()

project(luckfox_camera_rtsp CXX C)

# ─── Optional audio path (Pico Ultra/Ultra W) ──────────────────────────────
option(ENABLE_AUDIO "Enable RTSP audio track (G.711A via AI/AENC)" OFF)
set(AUDIO_SAMPLE_RATE 16000 CACHE STRING "Audio sample rate (supported: 8000 or 16000)")
# RV1106 built-in codec requires stereo (2ch) capture; 1ch always fails with
# RK_MPI_AI_EnableChn error 0xa00a8010.  Keep 2 here.
set(AUDIO_CHANNELS 2 CACHE STRING "Audio channel count (supported: 1 or 2)")

# ─── Camera profile / stream defaults ───────────────────────────────────────
set(CAMERA_SENSOR "MIS5001" CACHE STRING "Camera sensor profile: MIS5001 or SC3336")
set_property(CACHE CAMERA_SENSOR PROPERTY STRINGS MIS5001 SC3336)
string(TOUPPER "${CAMERA_SENSOR}" CAMERA_SENSOR_UPPER)

if(CAMERA_SENSOR_UPPER STREQUAL "MIS5001")
    set(PROFILE_STREAM_WIDTH 2592)
    set(PROFILE_STREAM_HEIGHT 1944)
    set(PROFILE_STREAM_FPS 25)
    set(PROFILE_SUB_STREAM_WIDTH 640)
    set(PROFILE_SUB_STREAM_HEIGHT 360)
elseif(CAMERA_SENSOR_UPPER STREQUAL "SC3336")
    # 1080p profile for SC3336 (16:9), preserving aspect ratio.
    set(PROFILE_STREAM_WIDTH 1920)
    set(PROFILE_STREAM_HEIGHT 1080)
    set(PROFILE_STREAM_FPS 25)
    set(PROFILE_SUB_STREAM_WIDTH 640)
    set(PROFILE_SUB_STREAM_HEIGHT 360)
else()
    message(FATAL_ERROR "Unsupported CAMERA_SENSOR='${CAMERA_SENSOR}'. Use MIS5001 or SC3336")
endif()

if(NOT DEFINED STREAM_WIDTH)
    set(STREAM_WIDTH ${PROFILE_STREAM_WIDTH})
endif()
if(NOT DEFINED STREAM_HEIGHT)
    set(STREAM_HEIGHT ${PROFILE_STREAM_HEIGHT})
endif()
if(NOT DEFINED STREAM_FPS)
    set(STREAM_FPS ${PROFILE_STREAM_FPS})
endif()
if(NOT DEFINED SUB_STREAM_WIDTH)
    set(SUB_STREAM_WIDTH ${PROFILE_SUB_STREAM_WIDTH})
endif()
if(NOT DEFINED SUB_STREAM_HEIGHT)
    set(SUB_STREAM_HEIGHT ${PROFILE_SUB_STREAM_HEIGHT})
endif()
if(NOT DEFINED SUB_STREAM_FPS)
    set(SUB_STREAM_FPS 10)
endif()
if(NOT DEFINED SUB_STREAM_BITRATE_KBPS)
    set(SUB_STREAM_BITRATE_KBPS 512)
endif()

if(STREAM_WIDTH LESS 1 OR STREAM_HEIGHT LESS 1 OR STREAM_FPS LESS 1)
    message(FATAL_ERROR "STREAM_WIDTH/HEIGHT/FPS must be positive values")
endif()
if(SUB_STREAM_WIDTH LESS 1 OR SUB_STREAM_HEIGHT LESS 1 OR SUB_STREAM_FPS LESS 1)
    message(FATAL_ERROR "SUB_STREAM_WIDTH/HEIGHT/FPS must be positive values")
endif()

if(ENABLE_AUDIO)
    if(NOT (AUDIO_SAMPLE_RATE EQUAL 8000 OR AUDIO_SAMPLE_RATE EQUAL 16000))
        message(FATAL_ERROR "AUDIO_SAMPLE_RATE must be 8000 or 16000")
    endif()
    if(NOT (AUDIO_CHANNELS EQUAL 1 OR AUDIO_CHANNELS EQUAL 2))
        message(FATAL_ERROR "AUDIO_CHANNELS must be 1 or 2")
    endif()
endif()

# ─── Paths ────────────────────────────────────────────────────────────────────
# SDK headers and prebuilt libraries are bundled in sdk/ by default.
# Override with cmake -DMPI_SDK_DIR=... or env MPI_SDK_DIR if needed.
if(DEFINED MPI_SDK_DIR)
    set(SDK_DIR "${MPI_SDK_DIR}")
elseif(DEFINED ENV{MPI_SDK_DIR})
    set(SDK_DIR "$ENV{MPI_SDK_DIR}")
else()
    set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
endif()

if(NOT EXISTS "${SDK_DIR}")
    message(FATAL_ERROR
        "SDK dir not found at:\n  ${SDK_DIR}\n"
        "The bundled sdk/ directory should be present in the project root.\n"
        "Or set it explicitly:\n"
        "  cmake -DMPI_SDK_DIR=/path/to/sdk")
endif()

set(SDK_INC "${SDK_DIR}/include")
set(SDK_LIB "${SDK_DIR}/lib/${LIBC_TYPE}")

# ─── Compile flags ────────────────────────────────────────────────────────────
add_definitions(
    -DRV1106_1103
    -DISP_HW_V30
    -DRKPLATFORM=ON
    -DARCH64=OFF
    -DUAPI2
    -D_LARGEFILE_SOURCE
    -D_LARGEFILE64_SOURCE
    -D_FILE_OFFSET_BITS=64
)

add_compile_options(
    -g
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -O2
)

# ─── Libraries ────────────────────────────────────────────────────────────────
link_directories(${SDK_LIB})

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${SDK_LIB}:/usr/lib")

# ─── Sources ──────────────────────────────────────────────────────────────────
set(SRC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/camera_mpi.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/isp_control.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt_client.cc
)

if(ENABLE_AUDIO)
    list(APPEND SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/audio_mpi.cc
    )
endif()

add_executable(${PROJECT_NAME} ${SRC_FILES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    STREAM_WIDTH=${STREAM_WIDTH}
    STREAM_HEIGHT=${STREAM_HEIGHT}
    STREAM_FPS=${STREAM_FPS}    SUB_STREAM_WIDTH=${SUB_STREAM_WIDTH}
    SUB_STREAM_HEIGHT=${SUB_STREAM_HEIGHT}
    SUB_STREAM_FPS=${SUB_STREAM_FPS}
    SUB_STREAM_BITRATE_KBPS=${SUB_STREAM_BITRATE_KBPS}    CAMERA_SENSOR_PROFILE=\"${CAMERA_SENSOR_UPPER}\"
    ENABLE_AUDIO_STREAM=$<IF:$<BOOL:${ENABLE_AUDIO}>,1,0>
    AUDIO_SAMPLE_RATE=${AUDIO_SAMPLE_RATE}
    AUDIO_CHANNELS=${AUDIO_CHANNELS}
)

# ─── Include dirs ─────────────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SDK_INC}
    ${SDK_INC}/rknn
    ${SDK_INC}/librga
    ${SDK_INC}/rkaiq
    ${SDK_INC}/rkaiq/uAPI2
    ${SDK_INC}/rkaiq/common
    ${SDK_INC}/rkaiq/xcore
    ${SDK_INC}/rkaiq/algos
    ${SDK_INC}/rkaiq/iq_parser
    ${SDK_INC}/rkaiq/iq_parser_v2
    ${SDK_INC}/rkaiq/smartIr
)

# ─── Link libraries ───────────────────────────────────────────────────────────
if(${LIBC_TYPE} STREQUAL "uclibc")
    target_link_libraries(${PROJECT_NAME}
        sample_comm
        rockit
        rockchip_mpp
        rkaiq
        pthread
        rtsp
        rga
        m
        dl
        rt
    )
elseif(${LIBC_TYPE} STREQUAL "glibc")
    target_link_libraries(${PROJECT_NAME}
        sample_comm
        rockit
        rockchip_mpp
        rkaiq
        pthread
        rtsp
        stdc++
        func_none
        c
        m
        rt
        dl
        rga
    )
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install/${LIBC_TYPE}")
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# Copy shared libs so the binary is fully self-contained
install(DIRECTORY ${SDK_LIB}/
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
    PATTERN "cmake" EXCLUDE
    PATTERN "pkgconfig" EXCLUDE
)

message(STATUS "Project:   ${PROJECT_NAME}")
message(STATUS "SDK dir:   ${SDK_DIR}")
message(STATUS "libc type: ${LIBC_TYPE}")
message(STATUS "sensor:    ${CAMERA_SENSOR_UPPER}")
message(STATUS "stream:    ${STREAM_WIDTH}x${STREAM_HEIGHT} @ ${STREAM_FPS}fps")
message(STATUS "sub:       ${SUB_STREAM_WIDTH}x${SUB_STREAM_HEIGHT} @ ${SUB_STREAM_FPS}fps ${SUB_STREAM_BITRATE_KBPS}kbps")
if(ENABLE_AUDIO)
    message(STATUS "audio:     ON (G711A, ${AUDIO_SAMPLE_RATE} Hz, ${AUDIO_CHANNELS} ch)")
else()
    message(STATUS "audio:     OFF")
endif()
