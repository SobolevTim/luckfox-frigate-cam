/*
 * isp_control.cc — rkaiq uAPI2 ISP parameter control.
 *
 * Initialises the AIQ context directly so that runtime adjustments
 * (brightness, contrast, WB, day/night …) work without restarting the stream.
 */

#include "isp_control.h"
#include "camera_mpi.h"   /* for STREAM_WIDTH / HEIGHT defaults */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Internal state ─────────────────────────────────────────────────────── */
static rk_aiq_sys_ctx_t *g_ctx    = NULL;
static camera_settings_t g_cfg;
static pthread_mutex_t   g_lock   = PTHREAD_MUTEX_INITIALIZER;
static const char       *DEFAULT_SETTINGS_FILE = ISP_SETTINGS_FILE;
static int               g_sensor_flip_supported = 0;

typedef struct {
    int valid;
    int saved_fps;
    int saved_bitrate_kbps;
} day_hw_profile_t;

static day_hw_profile_t g_day_hw;

rk_aiq_sys_ctx_t *isp_get_ctx(void) { return g_ctx; }
int isp_sensor_supports_flip(void) { return g_sensor_flip_supported; }

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static unsigned int clamp_u32(unsigned int value,
                              unsigned int min_value,
                              unsigned int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int xcam_status_to_int(const char *op_name, XCamReturn rc) {
    if (rc != XCAM_RETURN_NO_ERROR) {
        printf("[ISP] %s err=%d\n", op_name, rc);
        return -1;
    }
    return 0;
}

static void sanitize_settings(camera_settings_t *cfg) {
    if (!cfg) return;

    cfg->brightness        = clamp_u32(cfg->brightness, ISP_MIN_BRIGHTNESS, ISP_MAX_BRIGHTNESS);
    cfg->contrast          = clamp_u32(cfg->contrast, ISP_MIN_CONTRAST, ISP_MAX_CONTRAST);
    cfg->saturation        = clamp_u32(cfg->saturation, ISP_MIN_SATURATION, ISP_MAX_SATURATION);
    cfg->hue               = clamp_u32(cfg->hue, ISP_MIN_HUE, ISP_MAX_HUE);
    cfg->sharpness         = clamp_u32(cfg->sharpness, ISP_MIN_SHARPNESS, ISP_MAX_SHARPNESS);
    cfg->daynight          = (cfg->daynight == DAYNIGHT_GRAY) ? DAYNIGHT_GRAY : DAYNIGHT_COLOR;
    cfg->wb_preset         = (cfg->wb_preset >= WB_AUTO && cfg->wb_preset <= WB_SHADE)
                             ? cfg->wb_preset : WB_AUTO;
    cfg->mirror            = cfg->mirror ? 1 : 0;
    cfg->flip              = cfg->flip ? 1 : 0;
    cfg->anti_flicker_en   = cfg->anti_flicker_en ? 1 : 0;
    cfg->anti_flicker_mode = (cfg->anti_flicker_mode == 1) ? 1 : 0;
    cfg->bitrate_kbps      = clamp_int(cfg->bitrate_kbps, ISP_MIN_BITRATE_KBPS, ISP_MAX_BITRATE_KBPS);
    cfg->fps               = clamp_int(cfg->fps, ISP_MIN_FPS, ISP_MAX_FPS);
    cfg->sub_bitrate_kbps  = clamp_int(cfg->sub_bitrate_kbps, ISP_MIN_SUB_BITRATE_KBPS, ISP_MAX_SUB_BITRATE_KBPS);
    cfg->sub_fps           = clamp_int(cfg->sub_fps, ISP_MIN_SUB_FPS, ISP_MAX_SUB_FPS);
    cfg->night_mode        = cfg->night_mode ? 1 : 0;
}

static void set_defaults(void) {
    g_cfg.brightness      = ISP_DEFAULT_BRIGHTNESS;
    g_cfg.contrast        = ISP_DEFAULT_CONTRAST;
    g_cfg.saturation      = ISP_DEFAULT_SATURATION;
    g_cfg.hue             = ISP_DEFAULT_HUE;
    g_cfg.sharpness       = ISP_DEFAULT_SHARPNESS;
    g_cfg.daynight        = DAYNIGHT_COLOR;
    g_cfg.wb_preset       = WB_AUTO;
    g_cfg.mirror          = 0;
    g_cfg.flip            = 0;
    g_cfg.anti_flicker_en = 1;
    g_cfg.anti_flicker_mode = 0; /* 50 Hz */
    g_cfg.bitrate_kbps    = ISP_DEFAULT_BITRATE_KBPS;
    g_cfg.fps             = ISP_DEFAULT_FPS;
    g_cfg.sub_bitrate_kbps = ISP_DEFAULT_SUB_BITRATE_KBPS;
    g_cfg.sub_fps          = ISP_DEFAULT_SUB_FPS;
    g_cfg.night_mode       = 0;
    memset(&g_day_hw, 0, sizeof(g_day_hw));
}

static int cache_day_hw_profile(void) {
    if (!g_ctx) return -1;

    pthread_mutex_lock(&g_lock);
    if (g_day_hw.valid) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_day_hw.saved_fps         = g_cfg.fps;
    g_day_hw.saved_bitrate_kbps = g_cfg.bitrate_kbps;
    g_day_hw.valid = 1;
    pthread_mutex_unlock(&g_lock);

    printf("[ISP] cached day profile: fps=%d bitrate=%d\n",
           g_day_hw.saved_fps, g_day_hw.saved_bitrate_kbps);
    return 0;
}

/*
 * Night HW profile: do NOT override ANR/TNR/DRC/HLC/AE speed.
 * The ISP 3A engine with the sensor's IQ tuning file already handles
 * low-light NR/exposure.  Manual overrides conflict with it and make
 * the image worse.  We only switch to grayscale and confirm AE AUTO.
 */
static int apply_night_hw_profile(void) {
    if (!g_ctx) return -1;

    int ok = 0;

    /* Ensure AE is in AUTO so the sensor can use longer exposure times */
    ok |= xcam_status_to_int("setExpMode(OP_AUTO)",
                             rk_aiq_uapi2_setExpMode(g_ctx, OP_AUTO));

    return ok;
}

static int restore_day_hw_profile(void) {
    if (!g_ctx) return -1;

    int ok = 0;

    /* AE AUTO */
    ok |= xcam_status_to_int("setExpMode(OP_AUTO)",
                             rk_aiq_uapi2_setExpMode(g_ctx, OP_AUTO));

    /* Force AWB back to AUTO — critical after GRAY→COLOR transition,
     * otherwise AWB can get stuck with wrong colour matrix (purple tint). */
    ok |= xcam_status_to_int("setWBMode(OP_AUTO)",
                             rk_aiq_uapi2_setWBMode(g_ctx, OP_AUTO));

    pthread_mutex_lock(&g_lock);
    memset(&g_day_hw, 0, sizeof(g_day_hw));
    pthread_mutex_unlock(&g_lock);

    return ok;
}

/* Try to obtain the sensor entity name.
 * On RV1106/Luckfox the ISP streams from /dev/video0 or /dev/video11. */
static const char *find_sensor_entity_name(void) {
    static const char *vd_candidates[] = {
        "/dev/video0", "/dev/video11", "/dev/video1", NULL
    };
    for (int i = 0; vd_candidates[i]; i++) {
        const char *name = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(vd_candidates[i]);
        if (name && name[0] != '\0') {
            printf("[ISP] sensor entity: %s  (from %s)\n", name, vd_candidates[i]);
            return name;
        }
    }
    /* Fallback: enumerate */
    static rk_aiq_static_info_t info;
    if (rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info) == XCAM_RETURN_NO_ERROR) {
        printf("[ISP] sensor (enum): %s\n", info.sensor_info.sensor_name);
        return info.sensor_info.sensor_name;
    }
    return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int isp_init(int cam_id, const char *iq_dir, int width, int height) {
    (void)cam_id;

    set_defaults();
    isp_load_settings(DEFAULT_SETTINGS_FILE); /* overlay with saved values */
    sanitize_settings(&g_cfg);

    const char *sns = find_sensor_entity_name();
    if (!sns) {
        fprintf(stderr, "[ISP] ERROR: cannot detect sensor entity name\n");
        return -1;
    }

    g_ctx = rk_aiq_uapi2_sysctl_init(sns, iq_dir, NULL, NULL);
    if (!g_ctx) {
        fprintf(stderr, "[ISP] ERROR: rk_aiq_uapi2_sysctl_init failed\n");
        return -1;
    }

    XCamReturn rc = rk_aiq_uapi2_sysctl_prepare(
                        g_ctx, (uint32_t)width, (uint32_t)height,
                        RK_AIQ_WORKING_MODE_NORMAL);
    if (rc != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[ISP] ERROR: sysctl_prepare failed (%d)\n", rc);
        rk_aiq_uapi2_sysctl_deinit(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    rc = rk_aiq_uapi2_sysctl_start(g_ctx);
    if (rc != XCAM_RETURN_NO_ERROR) {
        fprintf(stderr, "[ISP] ERROR: sysctl_start failed (%d)\n", rc);
        rk_aiq_uapi2_sysctl_deinit(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    /* Ensure AE and AWB are in AUTO mode after start.
     * Without this, some sensors (e.g. SC3336) may boot with the ISP 3A engine
     * in an indeterminate state, causing overexposure or wrong white balance. */
    {
        XCamReturn ae_rc = rk_aiq_uapi2_setExpMode(g_ctx, OP_AUTO);
        printf("[ISP] force AE → AUTO: %s\n",
               ae_rc == XCAM_RETURN_NO_ERROR ? "ok" : "FAILED");

        XCamReturn wb_rc = rk_aiq_uapi2_setWBMode(g_ctx, OP_AUTO);
        printf("[ISP] force AWB → AUTO: %s\n",
               wb_rc == XCAM_RETURN_NO_ERROR ? "ok" : "FAILED");
    }

    /* Force TNR (Temporal Noise Reduction) OFF.
     * The IQ profile auto-enables TNR in low light which causes ghosting
     * on moving objects — they leave semi-transparent trails and blend with
     * the background, making motion detection impossible.
     * For a surveillance camera, sharp moving objects are more important
     * than smooth noise.  setMTNRStrth(true, 0) = manual override to zero. */
    {
        XCamReturn tnr_rc = rk_aiq_uapi2_setMTNRStrth(g_ctx, true, 0);
        printf("[ISP] force TNR → OFF: %s\n",
               tnr_rc == XCAM_RETURN_NO_ERROR ? "ok" : "FAILED");
    }

    /* Probe sensor mirror/flip support.
     * SC3336 exposes V4L2_CID_HFLIP/VFLIP, MIS5001 does not.
     * A no-op setMirrorFlip(false,false) succeeds on any sensor, so we must
     * test with an actual change and verify the readback to be sure. */
    {
        bool probe_ok = false;
        XCamReturn mf_rc = rk_aiq_uapi2_setMirrorFlip(g_ctx, true, false, 0);
        if (mf_rc == XCAM_RETURN_NO_ERROR) {
            bool rb_mirror = false, rb_flip = false;
            XCamReturn get_rc = rk_aiq_uapi2_getMirrorFlip(g_ctx, &rb_mirror, &rb_flip);
            if (get_rc == XCAM_RETURN_NO_ERROR && rb_mirror) {
                probe_ok = true;
            }
            /* Reset to default state regardless */
            rk_aiq_uapi2_setMirrorFlip(g_ctx, false, false, 0);
        }
        g_sensor_flip_supported = probe_ok ? 1 : 0;
        printf("[ISP] sensor mirror/flip support: %s (probe set_rc=%d)\n",
               g_sensor_flip_supported ? "YES (ISP API)" : "NO (RGA fallback)",
               mf_rc);
    }

    /* Diagnostic: verify AE mode is active */
    {
        opMode_t cur_ae = OP_MANUAL;
        if (rk_aiq_uapi2_getExpMode(g_ctx, &cur_ae) == XCAM_RETURN_NO_ERROR)
            printf("[ISP] AE mode verify: %s\n", cur_ae == OP_AUTO ? "AUTO" : "MANUAL");
        else
            printf("[ISP] AE mode verify: query failed\n");
    }

    /* Apply settings (saved or default) */
    isp_apply_settings(&g_cfg);

    printf("[ISP] ready — sensor=%s  IQ dir=%s\n", sns, iq_dir);
    return 0;
}

void isp_stop(void) {
    if (g_ctx) {
        isp_save_settings(DEFAULT_SETTINGS_FILE);
        rk_aiq_uapi2_sysctl_stop(g_ctx, false);
        rk_aiq_uapi2_sysctl_deinit(g_ctx);
        g_ctx = NULL;
        g_sensor_flip_supported = 0;
        memset(&g_day_hw, 0, sizeof(g_day_hw));
        printf("[ISP] stopped\n");
    }
}

/* ── Individual setters ─────────────────────────────────────────────────── */

int isp_set_brightness(unsigned int level) {
    if (!g_ctx) return -1;
    level = clamp_u32(level, ISP_MIN_BRIGHTNESS, ISP_MAX_BRIGHTNESS);
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setBrightness(g_ctx, level);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.brightness = level;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setBrightness(%u) err=%d\n", level, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_contrast(unsigned int level) {
    if (!g_ctx) return -1;
    level = clamp_u32(level, ISP_MIN_CONTRAST, ISP_MAX_CONTRAST);
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setContrast(g_ctx, level);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.contrast = level;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setContrast(%u) err=%d\n", level, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_saturation(unsigned int level) {
    if (!g_ctx) return -1;
    level = clamp_u32(level, ISP_MIN_SATURATION, ISP_MAX_SATURATION);
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setSaturation(g_ctx, level);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.saturation = level;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setSaturation(%u) err=%d\n", level, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_hue(unsigned int level) {
    if (!g_ctx) return -1;
    level = clamp_u32(level, ISP_MIN_HUE, ISP_MAX_HUE);
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setHue(g_ctx, level);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.hue = level;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setHue(%u) err=%d\n", level, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_sharpness(unsigned int level) {
    if (!g_ctx) return -1;
    level = clamp_u32(level, ISP_MIN_SHARPNESS, ISP_MAX_SHARPNESS);
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setSharpness(g_ctx, level);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.sharpness = level;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setSharpness(%u) err=%d\n", level, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_daynight(daynight_mode_t mode) {
    if (!g_ctx) return -1;
    mode = (mode == DAYNIGHT_GRAY) ? DAYNIGHT_GRAY : DAYNIGHT_COLOR;
    pthread_mutex_lock(&g_lock);
    rk_aiq_gray_mode_t gm = (mode == DAYNIGHT_GRAY)
                             ? RK_AIQ_GRAY_MODE_ON
                             : RK_AIQ_GRAY_MODE_OFF;
    XCamReturn rc = rk_aiq_uapi2_setGrayMode(g_ctx, gm);
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.daynight = mode;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setGrayMode(%d) err=%d\n", mode, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_wb_preset(wb_preset_t preset) {
    if (!g_ctx) return -1;
    if (preset < WB_AUTO || preset > WB_SHADE)
        preset = WB_AUTO;
    pthread_mutex_lock(&g_lock);
    XCamReturn rc;
    if (preset == WB_AUTO) {
        rc = rk_aiq_uapi2_setWBMode(g_ctx, OP_AUTO);
    } else {
        rc = rk_aiq_uapi2_setWBMode(g_ctx, OP_MANUAL);
        if (rc == XCAM_RETURN_NO_ERROR) {
            rk_aiq_wb_scene_t s;
            switch (preset) {
                case WB_INCANDESCENT:     s = RK_AIQ_WBCT_INCANDESCENT;     break;
                case WB_FLUORESCENT:      s = RK_AIQ_WBCT_FLUORESCENT;      break;
                case WB_WARM_FLUORESCENT: s = RK_AIQ_WBCT_WARM_FLUORESCENT; break;
                case WB_DAYLIGHT:         s = RK_AIQ_WBCT_DAYLIGHT;         break;
                case WB_CLOUDY:           s = RK_AIQ_WBCT_CLOUDY_DAYLIGHT;  break;
                case WB_TWILIGHT:         s = RK_AIQ_WBCT_TWILIGHT;         break;
                case WB_SHADE:            s = RK_AIQ_WBCT_SHADE;            break;
                default:                  s = RK_AIQ_WBCT_DAYLIGHT;         break;
            }
            rc = rk_aiq_uapi2_setMWBScene(g_ctx, s);
        }
    }
    if (rc == XCAM_RETURN_NO_ERROR)
        g_cfg.wb_preset = preset;
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setWBPreset(%d) err=%d\n", preset, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_mirror_flip(int mirror, int flip) {
    mirror = mirror ? 1 : 0;
    flip   = flip ? 1 : 0;

    /*
     * Try sensor-level mirror/flip first (via rkaiq → V4L2).
     * SC3336 supports V4L2_CID_HFLIP / V4L2_CID_VFLIP, MIS5001 does not.
     * When the sensor handles it, the VI output is already flipped and the
     * RGA capture loop must NOT apply a second transform (skip_frm_cnt=4
     * tells the ISP to skip 4 frames while the sensor reconfigures).
     * If the ISP API fails or the readback doesn't match, we fall back to
     * RGA transform in the main loop.
     */
    if (g_ctx && g_sensor_flip_supported) {
        XCamReturn rc = rk_aiq_uapi2_setMirrorFlip(g_ctx,
                                                    (bool)mirror, (bool)flip, 4);
        if (rc == XCAM_RETURN_NO_ERROR) {
            /* Verify the sensor actually accepted the values */
            bool rb_mirror = false, rb_flip = false;
            XCamReturn get_rc = rk_aiq_uapi2_getMirrorFlip(g_ctx, &rb_mirror, &rb_flip);
            if (get_rc == XCAM_RETURN_NO_ERROR &&
                (int)rb_mirror == mirror && (int)rb_flip == flip) {
                printf("[ISP] mirror=%d flip=%d applied via sensor (ISP API, verified)\n",
                       mirror, flip);
            } else {
                printf("[ISP] setMirrorFlip(%d,%d) ISP readback mismatch "
                       "(got mirror=%d flip=%d), falling back to RGA\n",
                       mirror, flip, (int)rb_mirror, (int)rb_flip);
                g_sensor_flip_supported = 0;
            }
        } else {
            printf("[ISP] setMirrorFlip(%d,%d) ISP err=%d, falling back to RGA\n",
                   mirror, flip, rc);
            g_sensor_flip_supported = 0; /* downgrade: stop trying ISP path */
        }
    } else {
        printf("[ISP] mirror=%d flip=%d (will be applied via RGA)\n",
               mirror, flip);
    }

    pthread_mutex_lock(&g_lock);
    g_cfg.mirror = mirror;
    g_cfg.flip   = flip;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int isp_set_anti_flicker(int enable, int mode) {
    if (!g_ctx) return -1;
    enable = enable ? 1 : 0;
    mode   = (mode == 1) ? 1 : 0;
    pthread_mutex_lock(&g_lock);
    XCamReturn rc = rk_aiq_uapi2_setAntiFlickerEn(g_ctx, (bool)enable);
    if (rc == XCAM_RETURN_NO_ERROR && enable) {
        antiFlickerMode_t afm = (mode == 1) ? ANTIFLICKER_AUTO_MODE
                                            : ANTIFLICKER_NORMAL_MODE;
        rc = rk_aiq_uapi2_setAntiFlickerMode(g_ctx, afm);
    }
    if (rc == XCAM_RETURN_NO_ERROR) {
        g_cfg.anti_flicker_en   = enable;
        g_cfg.anti_flicker_mode = mode;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc != XCAM_RETURN_NO_ERROR)
        printf("[ISP] setAntiFlicker(%d,%d) err=%d\n", enable, mode, rc);
    return (rc == XCAM_RETURN_NO_ERROR) ? 0 : -1;
}

int isp_set_bitrate_kbps(int bitrate_kbps) {
    bitrate_kbps = clamp_int(bitrate_kbps, ISP_MIN_BITRATE_KBPS, ISP_MAX_BITRATE_KBPS);
    int ret = venc_set_bitrate(VENC_CHN_ID, bitrate_kbps);
    if (ret == 0) {
        pthread_mutex_lock(&g_lock);
        g_cfg.bitrate_kbps = bitrate_kbps;
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

int isp_set_fps(int fps) {
    fps = clamp_int(fps, ISP_MIN_FPS, ISP_MAX_FPS);
    int ret = venc_set_fps(VENC_CHN_ID, fps, fps);
    if (ret == 0) {
        pthread_mutex_lock(&g_lock);
        g_cfg.fps = fps;
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

int isp_set_sub_bitrate_kbps(int bitrate_kbps) {
    bitrate_kbps = clamp_int(bitrate_kbps, ISP_MIN_SUB_BITRATE_KBPS, ISP_MAX_SUB_BITRATE_KBPS);
    int ret = venc_set_bitrate(VENC_SUB_CHN_ID, bitrate_kbps);
    if (ret == 0) {
        pthread_mutex_lock(&g_lock);
        g_cfg.sub_bitrate_kbps = bitrate_kbps;
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

int isp_set_sub_fps(int fps) {
    fps = clamp_int(fps, ISP_MIN_SUB_FPS, ISP_MAX_SUB_FPS);
    int ret = venc_set_fps(VENC_SUB_CHN_ID, fps, fps);
    if (ret == 0) {
        pthread_mutex_lock(&g_lock);
        g_cfg.sub_fps = fps;
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

int isp_set_night_mode(int enabled) {
    enabled = enabled ? 1 : 0;
    int ok = 0;

    if (enabled)
        cache_day_hw_profile();

    if (enabled) {
        /* Night mode: switch to grayscale and lower FPS for longer exposure.
         * Do NOT override brightness/contrast/sharpness/NR/DRC/HLC — the ISP
         * 3A engine with the sensor IQ tuning profile already handles
         * low-light optimisation.  Manual overrides conflict with it. */
        ok |= isp_set_daynight    (DAYNIGHT_GRAY);
        ok |= isp_set_fps         (ISP_NIGHT_FPS);
        ok |= isp_set_bitrate_kbps(ISP_NIGHT_BITRATE_KBPS);
        ok |= apply_night_hw_profile();
    } else {
        /* Day restore: read cached values, then reset HW, then restore FPS.
         * Must read g_day_hw BEFORE restore_day_hw_profile clears it. */
        pthread_mutex_lock(&g_lock);
        int day_fps     = g_day_hw.valid ? g_day_hw.saved_fps          : ISP_DEFAULT_FPS;
        int day_bitrate = g_day_hw.valid ? g_day_hw.saved_bitrate_kbps : ISP_DEFAULT_BITRATE_KBPS;
        pthread_mutex_unlock(&g_lock);

        ok |= isp_set_daynight    (DAYNIGHT_COLOR);
        ok |= restore_day_hw_profile();
        ok |= isp_set_fps         (day_fps);
        ok |= isp_set_bitrate_kbps(day_bitrate);
    }

    pthread_mutex_lock(&g_lock);
    g_cfg.night_mode = enabled;
    pthread_mutex_unlock(&g_lock);

    printf("[ISP] night_mode → %s\n", enabled ? "ON" : "OFF");
    return ok;
}

/* ── Bulk ────────────────────────────────────────────────────────────────── */

void isp_get_settings(camera_settings_t *out) {
    pthread_mutex_lock(&g_lock);
    sanitize_settings(&g_cfg);
    *out = g_cfg;
    pthread_mutex_unlock(&g_lock);
}

int isp_apply_settings(const camera_settings_t *s) {
    camera_settings_t sanitized = *s;
    sanitize_settings(&sanitized);

    int ok = 0;
    ok |= isp_set_brightness(sanitized.brightness);
    ok |= isp_set_contrast  (sanitized.contrast);
    ok |= isp_set_saturation(sanitized.saturation);
    ok |= isp_set_hue       (sanitized.hue);
    ok |= isp_set_sharpness (sanitized.sharpness);
    ok |= isp_set_daynight  (sanitized.daynight);
    ok |= isp_set_wb_preset (sanitized.wb_preset);
    ok |= isp_set_mirror_flip(sanitized.mirror, sanitized.flip);
    ok |= isp_set_anti_flicker(sanitized.anti_flicker_en, sanitized.anti_flicker_mode);

    pthread_mutex_lock(&g_lock);
    g_cfg.bitrate_kbps = sanitized.bitrate_kbps;
    g_cfg.fps          = sanitized.fps;
    g_cfg.sub_bitrate_kbps = sanitized.sub_bitrate_kbps;
    g_cfg.sub_fps          = sanitized.sub_fps;
    g_cfg.night_mode       = sanitized.night_mode;
    pthread_mutex_unlock(&g_lock);

    /* If night mode was persisted, re-apply the night preset on top */
    if (sanitized.night_mode)
        isp_set_night_mode(1);

    return ok;
}

/* ── Persistence ─────────────────────────────────────────────────────────── */

void isp_save_settings(const char *filepath) {
    pthread_mutex_lock(&g_lock);
    sanitize_settings(&g_cfg);
    FILE *f = fopen(filepath, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"brightness\": %u,\n",        g_cfg.brightness);
        fprintf(f, "  \"contrast\": %u,\n",           g_cfg.contrast);
        fprintf(f, "  \"saturation\": %u,\n",         g_cfg.saturation);
        fprintf(f, "  \"hue\": %u,\n",                g_cfg.hue);
        fprintf(f, "  \"sharpness\": %u,\n",          g_cfg.sharpness);
        fprintf(f, "  \"daynight\": %d,\n",           (int)g_cfg.daynight);
        fprintf(f, "  \"wb_preset\": %d,\n",          (int)g_cfg.wb_preset);
        fprintf(f, "  \"mirror\": %d,\n",             g_cfg.mirror);
        fprintf(f, "  \"flip\": %d,\n",               g_cfg.flip);
        fprintf(f, "  \"anti_flicker_en\": %d,\n",    g_cfg.anti_flicker_en);
        fprintf(f, "  \"anti_flicker_mode\": %d,\n",  g_cfg.anti_flicker_mode);
        fprintf(f, "  \"bitrate_kbps\": %d,\n",       g_cfg.bitrate_kbps);
        fprintf(f, "  \"fps\": %d,\n",                 g_cfg.fps);
        fprintf(f, "  \"sub_bitrate_kbps\": %d,\n",   g_cfg.sub_bitrate_kbps);
        fprintf(f, "  \"sub_fps\": %d,\n",              g_cfg.sub_fps);
        fprintf(f, "  \"night_mode\": %d\n",             g_cfg.night_mode);
        fprintf(f, "}\n");
        fclose(f);
        printf("[ISP] settings saved → %s\n", filepath);
    } else {
        printf("[ISP] WARNING: cannot save settings to %s\n", filepath);
    }
    pthread_mutex_unlock(&g_lock);
}

/* Minimal flat-JSON parser: handles integer values only, one per line. */
int isp_load_settings(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    char line[128];
    int  loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int  val;
        /* Match:  "key": number */
        if (sscanf(line, " \"%63[^\"]\": %d", key, &val) != 2) continue;
        loaded++;
        if      (!strcmp(key,"brightness"))        g_cfg.brightness        = (unsigned)val;
        else if (!strcmp(key,"contrast"))          g_cfg.contrast          = (unsigned)val;
        else if (!strcmp(key,"saturation"))        g_cfg.saturation        = (unsigned)val;
        else if (!strcmp(key,"hue"))               g_cfg.hue               = (unsigned)val;
        else if (!strcmp(key,"sharpness"))         g_cfg.sharpness         = (unsigned)val;
        else if (!strcmp(key,"daynight"))          g_cfg.daynight          = (daynight_mode_t)val;
        else if (!strcmp(key,"wb_preset"))         g_cfg.wb_preset         = (wb_preset_t)val;
        else if (!strcmp(key,"mirror"))            g_cfg.mirror            = val;
        else if (!strcmp(key,"flip"))              g_cfg.flip              = val;
        else if (!strcmp(key,"anti_flicker_en"))   g_cfg.anti_flicker_en   = val;
        else if (!strcmp(key,"anti_flicker_mode")) g_cfg.anti_flicker_mode = val;
        else if (!strcmp(key,"bitrate_kbps"))      g_cfg.bitrate_kbps      = val;
        else if (!strcmp(key,"fps"))               g_cfg.fps               = val;
        else if (!strcmp(key,"sub_bitrate_kbps"))  g_cfg.sub_bitrate_kbps  = val;
        else if (!strcmp(key,"sub_fps"))           g_cfg.sub_fps           = val;
        else if (!strcmp(key,"night_mode"))        g_cfg.night_mode        = val;
    }
    fclose(f);
    sanitize_settings(&g_cfg);
    if (loaded > 0)
        printf("[ISP] loaded %d setting(s) from %s\n", loaded, filepath);
    return 0;
}
