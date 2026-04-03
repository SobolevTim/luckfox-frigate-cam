#pragma once
/*
 * camera_mpi.h — VI (video input) and VENC (hardware H.264 encoder) helpers
 *
 * Pipeline:
 *   Camera → ISP → VI (NV12/YUV420SP) → VENC (H.264) → RTSP
 *
 * No OpenCV, no CPU YUV→RGB conversion — everything runs in hardware.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <stdint.h>
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_mb.h"
#include "rk_debug.h"
#include "rk_common.h"

/* ── Stream parameters (can be overridden at compile time) ────────────────── */
#ifndef STREAM_WIDTH
#define STREAM_WIDTH    2592
#endif
#ifndef STREAM_HEIGHT
#define STREAM_HEIGHT   1944
#endif
#ifndef STREAM_FPS
#define STREAM_FPS      25
#endif
#ifndef CAMERA_SENSOR_PROFILE
#define CAMERA_SENSOR_PROFILE "MIS5001"
#endif
#ifndef DEFAULT_BITRATE_KBPS
#define DEFAULT_BITRATE_KBPS  10240
#endif

/* ── Sub stream defaults (compile-time, overridable via CMake) ────────────── */
#ifndef SUB_STREAM_WIDTH
#define SUB_STREAM_WIDTH    640
#endif
#ifndef SUB_STREAM_HEIGHT
#define SUB_STREAM_HEIGHT   360
#endif
#ifndef SUB_STREAM_FPS
#define SUB_STREAM_FPS      10
#endif
#ifndef SUB_STREAM_BITRATE_KBPS
#define SUB_STREAM_BITRATE_KBPS 512
#endif

#define VENC_CHN_ID       0
#define VENC_SUB_CHN_ID   1
#define VI_DEV_ID         0
#define VI_CHN_ID         0

/* ── Utility ──────────────────────────────────────────────────────────────── */
RK_U64 get_now_us(void);

/* ── VI ───────────────────────────────────────────────────────────────────── */
int vi_dev_init(void);
int vi_chn_init(int dev_id, int chn_id, int width, int height);
void vi_deinit(int dev_id, int chn_id);

/* ── VENC ─────────────────────────────────────────────────────────────────── */
/*
 * type         — RK_VIDEO_ID_AVC (H.264) or RK_VIDEO_ID_HEVC (H.265)
 * bitrate_kbps — CBR target bitrate in kilo-bits per second
 * fps          — output frame rate
 * gop          — keyframe interval in frames
 */
int  venc_init(int chn_id, int width, int height,
               RK_CODEC_ID_E type, int bitrate_kbps, int fps, int gop);
int  venc_set_bitrate(int chn_id, int bitrate_kbps);
int  venc_set_fps(int chn_id, int fps, int gop);
void venc_deinit(int chn_id);

/* ── RGA hardware copy (NV12 DMA, zero CPU load) ─────────────────────────── */
/*
 * Copy NV12 frame using RGA DMA engine (no CPU involvement).
 * Returns 0 on success, -1 on error.
 */
int rga_copy_nv12(void *src_vaddr, void *dst_vaddr, int width, int height);

/*
 * Copy NV12 frame with optional mirror (horizontal flip) and/or flip
 * (vertical flip) applied in the same RGA operation.
 *   mirror=1  →  IM_HAL_TRANSFORM_FLIP_H
 *   flip=1    →  IM_HAL_TRANSFORM_FLIP_V
 *   both=1    →  ROT_180 (equivalent to H+V flip, single RGA pass)
 * Falls back to plain rga_copy_nv12 when both are 0.
 * Returns 0 on success, -1 on error (caller should memcpy as fallback).
 */
int rga_copy_nv12_transform(void *src_vaddr, void *dst_vaddr,
                             int width, int height, int mirror, int flip);

/* ── RGA hardware resize (NV12 → NV12) ───────────────────────────────────── */
/*
 * Resize NV12 frame using RGA hardware engine.
 * Returns 0 on success, -1 on error.
 */
int rga_resize_nv12(void *src_vaddr, int src_w, int src_h,
                    void *dst_vaddr, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif
