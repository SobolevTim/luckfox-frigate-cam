#pragma once
/*
 * audio_mpi.h - AI/AENC helpers for RTSP audio streaming.
 *
 * This module is intentionally minimal and tuned for Buildroot targets:
 * - Audio input from onboard codec via AI (hw:0,0)
 * - Encoding to G.711 A-law (PCMA) in hardware/MPP path
 * - Low overhead and broad RTSP/NVR compatibility (Frigate friendly)
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "rk_mpi_ai.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rtsp_demo.h"

typedef struct audio_rtsp_ctx_s {
    AUDIO_DEV ai_dev;
    AI_CHN ai_chn;
    AENC_CHN aenc_chn;
    int sample_rate;
    int channels;
    int initialized;
    int bound;
} audio_rtsp_ctx_t;

/* Initialize AI + AENC and bind AI->AENC. Returns 0 on success. */
int audio_rtsp_init(audio_rtsp_ctx_t *ctx, int sample_rate, int channels);

/* Pull and send up to max_packets encoded audio frames to RTSP. */
int audio_rtsp_send_pending(audio_rtsp_ctx_t *ctx,
                            rtsp_session_handle rtsp_session,
                            int max_packets);

/* Stop and release audio resources; safe to call on partially initialized ctx. */
void audio_rtsp_deinit(audio_rtsp_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
