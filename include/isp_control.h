#pragma once
/*
 * isp_control.h — Runtime ISP / image-parameter control via rkaiq uAPI2.
 *
 * Initialises rkaiq directly (stores rk_aiq_sys_ctx_t* internally) so that
 * every parameter can be adjusted live without restarting the pipeline.
 *
 * Settings are persisted to /etc/camera_rtsp.json and reloaded on boot.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_types.h"
#include "rk_aiq_user_api_common.h"

/* ── Day / Night mode ────────────────────────────────────────────────────── */
typedef enum {
    DAYNIGHT_COLOR = 0,   /* colour (day) — ISP full pipeline  */
    DAYNIGHT_GRAY  = 1,   /* grayscale (night / IR-cut removed) */
} daynight_mode_t;

/* ── White-balance presets ───────────────────────────────────────────────── */
typedef enum {
    WB_AUTO             = 0,
    WB_INCANDESCENT     = 1,
    WB_FLUORESCENT      = 2,
    WB_WARM_FLUORESCENT = 3,
    WB_DAYLIGHT         = 4,
    WB_CLOUDY           = 5,
    WB_TWILIGHT         = 6,
    WB_SHADE            = 7,
} wb_preset_t;

/* ── All adjustable parameters ───────────────────────────────────────────── */
typedef struct {
    unsigned int  brightness;        /* 0-255, default 128  */
    unsigned int  contrast;          /* 0-255, default 128  */
    unsigned int  saturation;        /* 0-255, default 128  */
    unsigned int  hue;               /* 0-255, default 128  */
    unsigned int  sharpness;         /* 0-100, default 50   */
    daynight_mode_t daynight;        /* DAYNIGHT_COLOR / GRAY */
    wb_preset_t   wb_preset;         /* white-balance preset */
    int           mirror;            /* 0=off  1=on */
    int           flip;              /* 0=off  1=on */
    int           anti_flicker_en;   /* 0=off  1=on */
    int           anti_flicker_mode; /* 0=50 Hz  1=auto */
        int           bitrate_kbps;      /* encoder CBR bitrate */
    int           fps;               /* encoder frame-rate  */
    int           sub_bitrate_kbps;  /* sub stream encoder bitrate */
    int           sub_fps;           /* sub stream frame-rate     */
    int           night_mode;        /* 0=day (defaults)  1=night (low-light preset) */
} camera_settings_t;

/* ── Defaults ────────────────────────────────────────────────────────────── */
#define ISP_MIN_BRIGHTNESS          0U
#define ISP_MAX_BRIGHTNESS          255U
#define ISP_MIN_CONTRAST            0U
#define ISP_MAX_CONTRAST            255U
#define ISP_MIN_SATURATION          0U
#define ISP_MAX_SATURATION          255U
#define ISP_MIN_HUE                 0U
#define ISP_MAX_HUE                 255U
#define ISP_MIN_SHARPNESS           0U
#define ISP_MAX_SHARPNESS           100U
#define ISP_MIN_BITRATE_KBPS        1000
#define ISP_MAX_BITRATE_KBPS        20000
#define ISP_MIN_FPS                 10
#define ISP_MAX_FPS                 30

#define ISP_DEFAULT_BRIGHTNESS      128
#define ISP_DEFAULT_CONTRAST        128
#define ISP_DEFAULT_SATURATION      128
#define ISP_DEFAULT_HUE             128
#define ISP_DEFAULT_SHARPNESS       50
#define ISP_DEFAULT_BITRATE_KBPS    10240
#define ISP_DEFAULT_FPS             25
#define ISP_MIN_SUB_BITRATE_KBPS    100
#define ISP_MAX_SUB_BITRATE_KBPS    5000
#define ISP_MIN_SUB_FPS             5
#define ISP_MAX_SUB_FPS             30
#define ISP_DEFAULT_SUB_BITRATE_KBPS 512
#define ISP_DEFAULT_SUB_FPS          10
#define ISP_SETTINGS_FILE           "/etc/camera_rtsp.json"

/* Night-mode preset values (optimised for MIS5001 in near-darkness) */
#define ISP_NIGHT_BRIGHTNESS        165
#define ISP_NIGHT_CONTRAST           92
#define ISP_NIGHT_SATURATION          0
#define ISP_NIGHT_SHARPNESS          22
#define ISP_NIGHT_FPS                12
#define ISP_NIGHT_BITRATE_KBPS    12288
#define ISP_NIGHT_ANR_LEVEL          80

/* Day rollback values for stable night->day transition */
#define ISP_DAY_ANR_LEVEL           50

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
/*
 * isp_init — auto-detects sensor, starts rkaiq, applies saved/default settings.
 * Must be called BEFORE vi_dev_init / RK_MPI_SYS_Init.
 */
int  isp_init(int cam_id, const char *iq_dir, int width, int height);
void isp_stop(void);

/* ── Individual setters (thread-safe) ───────────────────────────────────── */
int isp_set_brightness   (unsigned int level);   /* 0-255 */
int isp_set_contrast     (unsigned int level);   /* 0-255 */
int isp_set_saturation   (unsigned int level);   /* 0-255 */
int isp_set_hue          (unsigned int level);   /* 0-255 */
int isp_set_sharpness    (unsigned int level);   /* 0-100 */
int isp_set_daynight     (daynight_mode_t mode);
int isp_set_wb_preset    (wb_preset_t preset);
int isp_set_mirror_flip  (int mirror, int flip);
int isp_set_anti_flicker (int enable, int mode); /* mode: 0=50Hz 1=auto */
int isp_set_bitrate_kbps (int bitrate_kbps);
int isp_set_fps          (int fps);
int isp_set_sub_bitrate_kbps(int bitrate_kbps);
int isp_set_sub_fps         (int fps);
int isp_set_night_mode      (int enabled);     /* applies day/night preset */

/* ── Bulk get / apply ────────────────────────────────────────────────────── */
void isp_get_settings   (camera_settings_t *out);
int  isp_apply_settings (const camera_settings_t *s);

/* ── Persistence ─────────────────────────────────────────────────────────── */
void isp_save_settings (const char *filepath);
int  isp_load_settings (const char *filepath);  /* returns 0 if file found */

/* ── Context accessor (for advanced use) ────────────────────────────────── */
rk_aiq_sys_ctx_t *isp_get_ctx(void);

/* Returns 1 if the sensor driver supports hardware mirror/flip (via V4L2),
 * 0 if software RGA fallback must be used. Probed once at isp_init(). */
int isp_sensor_supports_flip(void);

#ifdef __cplusplus
}
#endif
