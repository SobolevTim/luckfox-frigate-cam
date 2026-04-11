/*
 * main.cc — Stable RTSP camera for Frigate + Home Assistant control
 *
 * Pipeline (all hardware, no CPU colour conversion):
 *   Camera (sensor profile selected at build time) → ISP (rkaiq)
 *   → VI NV12 → VENC H.264 → RTSP :554/live/0   (main stream)
 *   → VI NV12 → RGA resize → VENC H.264 → RTSP :554/live/1   (sub stream)
 *
 * Control:
 *   MQTT  <broker>/luckfox_camera/+/set   (HA auto-discovered via MQTT Discovery)
 *
 * ISP settings are persisted to /etc/camera_rtsp.json.
 *
 * Usage:
 *   luckfox_camera_rtsp [options]
 *   Options:
 *     --mqtt-host <ip>    MQTT broker address  (default: 127.0.0.1)
 *     --mqtt-port <port>  MQTT broker port     (default: 1883)
 *     --mqtt-user <user>  MQTT username        (optional)
 *     --mqtt-pass <pass>  MQTT password        (optional)
 *     --mqtt-id   <id>    MQTT client ID / topic prefix  (default: luckfox_camera)
 *     --mqtt-name <name>  HA friendly device name        (default: Luckfox Camera)
 *     --mqtt-discovery-refresh <sec>
 *                          periodic HA discovery refresh (default: 0 = disabled)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "camera_mpi.h"
#include "isp_control.h"
#include "mqtt_client.h"
#include "rtsp_demo.h"
#if ENABLE_AUDIO_STREAM
#include "audio_mpi.h"
#endif

/* ── Configuration ───────────────────────────────────────────────────────── */
#define RTSP_PORT        554
#define RTSP_PATH        "/live/0"
#define RTSP_SUB_PATH    "/live/1"
#define IQ_FILES_DIR     "/etc/iqfiles"
#define APP_HEARTBEAT_FILE "/tmp/camera_rtsp/heartbeat"
#define APP_HEARTBEAT_DIR  "/tmp/camera_rtsp"

/* Maximum consecutive VI errors before logging a warning */
#define MAX_VI_ERRORS    10
#define MAX_VENC_SEND_ERRORS 10
#define MAX_VENC_GET_ERRORS  10
#define STREAM_STALL_TIMEOUT_S 20
#define HEARTBEAT_UPDATE_INTERVAL_US 1000000
#define SUB_VENC_SEND_TIMEOUT_MS 100
#define SUB_VENC_GET_TIMEOUT_MS  100

#if ENABLE_AUDIO_STREAM
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 8000
#endif
#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS 1
#endif
#endif

/* ── Globals ─────────────────────────────────────────────────────────────── */
static volatile int g_quit = 0;
static volatile int g_heartbeat_running = 0;
static pthread_t g_heartbeat_thread;

static int env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') return 0;
    return strcmp(value, "0") != 0;
}

static int parse_cli_int_range(const char *text, int min_v, int max_v, int *out)
{
    if (!text || !out)
        return -1;

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value < min_v || value > max_v)
        return -1;

    *out = (int)value;
    return 0;
}

static void ensure_heartbeat_dir(void) {
    if (mkdir(APP_HEARTBEAT_DIR, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[MAIN] WARNING: failed to create heartbeat dir %s: %s\n",
                APP_HEARTBEAT_DIR, strerror(errno));
    }
}

static void update_heartbeat_file(void) {
    FILE *f = fopen(APP_HEARTBEAT_FILE, "w");
    if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)get_now_us());
    fclose(f);
}

static void *heartbeat_thread_fn(void *arg) {
    (void)arg;
    while (g_heartbeat_running && !g_quit) {
        update_heartbeat_file();
        usleep(HEARTBEAT_UPDATE_INTERVAL_US);
    }
    return NULL;
}

static void sig_handler(int signo) {
    (void)signo;
    printf("\n[MAIN] Signal received, shutting down...\n");
    g_quit = 1;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Stop the default Luckfox camera daemon so it doesn't claim the sensor */
    if (!env_flag_enabled("LUCKFOX_SKIP_RKLUNCH_STOP")) {
        system("RkLunch-stop.sh");
        sleep(1); /* give the daemon time to release resources */
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN); /* ignore broken-pipe on RTSP socket writes */

    int exit_code = 0;

    ensure_heartbeat_dir();
    g_heartbeat_running = 1;
    if (pthread_create(&g_heartbeat_thread, NULL, heartbeat_thread_fn, NULL) != 0) {
        fprintf(stderr, "[MAIN] WARNING: heartbeat thread start failed, continuing without it\n");
        g_heartbeat_running = 0;
    }

    /* ── Parse CLI arguments ─────────────────────────────────────────────── */
    mqtt_config_t mqtt_cfg;
    mqtt_config_init(&mqtt_cfg);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mqtt-host") && i+1 < argc)
            strncpy(mqtt_cfg.broker_host, argv[++i], sizeof(mqtt_cfg.broker_host)-1);
        else if (!strcmp(argv[i], "--mqtt-port") && i+1 < argc) {
            if (parse_cli_int_range(argv[++i], 1, 65535, &mqtt_cfg.broker_port) != 0) {
                fprintf(stderr, "Invalid --mqtt-port value. Expected integer in range 1..65535\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--mqtt-user") && i+1 < argc)
            strncpy(mqtt_cfg.username, argv[++i], sizeof(mqtt_cfg.username)-1);
        else if (!strcmp(argv[i], "--mqtt-pass") && i+1 < argc)
            strncpy(mqtt_cfg.password, argv[++i], sizeof(mqtt_cfg.password)-1);
        else if (!strcmp(argv[i], "--mqtt-id") && i+1 < argc)
            strncpy(mqtt_cfg.node_id, argv[++i], sizeof(mqtt_cfg.node_id)-1);
        else if (!strcmp(argv[i], "--mqtt-name") && i+1 < argc)
            strncpy(mqtt_cfg.device_name, argv[++i], sizeof(mqtt_cfg.device_name)-1);
        else if (!strcmp(argv[i], "--mqtt-discovery-refresh") && i+1 < argc) {
            if (parse_cli_int_range(argv[++i], 0, 86400, &mqtt_cfg.discovery_refresh_s) != 0) {
                fprintf(stderr, "Invalid --mqtt-discovery-refresh value. Expected integer in range 0..86400\n");
                return 1;
            }
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s [--mqtt-host H] [--mqtt-port P] "
                            "[--mqtt-user U] [--mqtt-pass P] "
                            "[--mqtt-id ID] [--mqtt-name NAME] "
                            "[--mqtt-discovery-refresh SEC]\n", argv[0]);
            return 1;
        }
    }

    printf("=======================================================\n");
    printf(" Luckfox Camera RTSP  |  %dx%d  H.264  %d fps\n",
           STREAM_WIDTH, STREAM_HEIGHT, STREAM_FPS);
    printf(" Sub stream:  %dx%d  H.264  %d fps  %d kbps\n",
           SUB_STREAM_WIDTH, SUB_STREAM_HEIGHT,
           SUB_STREAM_FPS, SUB_STREAM_BITRATE_KBPS);
    printf(" Sensor profile: %s\n", CAMERA_SENSOR_PROFILE);
    printf(" RTSP main: rtsp://<IP>:%d%s\n", RTSP_PORT, RTSP_PATH);
    printf(" RTSP sub:  rtsp://<IP>:%d%s\n", RTSP_PORT, RTSP_SUB_PATH);
    printf(" MQTT : %s:%d  topic prefix: %s\n",
           mqtt_cfg.broker_host, mqtt_cfg.broker_port, mqtt_cfg.node_id);
    if (mqtt_cfg.discovery_refresh_s > 0)
        printf(" MQTT discovery periodic refresh: %d s\n", mqtt_cfg.discovery_refresh_s);
    else
        printf(" MQTT discovery periodic refresh: disabled\n");
#if ENABLE_AUDIO_STREAM
    printf(" AUDIO: enabled (G711A, %d Hz, %d ch)\n", AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
#else
    printf(" AUDIO: disabled\n");
#endif
    printf(" BUILD: " __DATE__ " " __TIME__ " [sensor-diagnostics enabled]\n");
    printf("=======================================================\n");

    /* ── 1. ISP init (must come before VI) ─────────────────────────────── */
    if (isp_init(0, IQ_FILES_DIR, STREAM_WIDTH, STREAM_HEIGHT) != 0) {
        fprintf(stderr, "[MAIN] ISP init failed. Is the IQ file dir correct?\n");
        fprintf(stderr, "[MAIN] Expected: %s\n", IQ_FILES_DIR);
        return 1;
    }

    /* Read saved/default settings (bitrate, fps, etc.) */
    camera_settings_t cfg;
    isp_get_settings(&cfg);
    const int stream_gop = cfg.fps;

    /* ── 2. RK MPI system init ──────────────────────────────────────────── */
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        fprintf(stderr, "[MAIN] RK_MPI_SYS_Init failed\n");
        isp_stop();
        return 1;
    }

    /* ── 3. VI (video input) ────────────────────────────────────────────── */
    if (vi_dev_init() != 0) {
        fprintf(stderr, "[MAIN] VI device init failed\n");
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }
    if (vi_chn_init(VI_DEV_ID, VI_CHN_ID, STREAM_WIDTH, STREAM_HEIGHT) != 0) {
        fprintf(stderr, "[MAIN] VI channel init failed\n");
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* Re-apply saved mirror/flip now that VI channel is ready.
     * During isp_init() the VI channel didn't exist yet, so the VI API
     * call failed and the settings were marked for RGA fallback.
     * Now that the channel is up, give the VI ISP hardware path a chance. */
    {
        camera_settings_t boot_cfg;
        isp_get_settings(&boot_cfg);
        if (boot_cfg.mirror || boot_cfg.flip)
            isp_set_mirror_flip(boot_cfg.mirror, boot_cfg.flip);
    }

    /* ── 4. VENC (hardware H.264 encoder — main stream) ───────────────── */
    if (venc_init(VENC_CHN_ID, STREAM_WIDTH, STREAM_HEIGHT,
                  RK_VIDEO_ID_AVC,
                  cfg.bitrate_kbps, cfg.fps, stream_gop) != 0) {
        fprintf(stderr, "[MAIN] VENC main init failed\n");
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* ── 4b. VENC (hardware H.264 encoder — sub stream) ────────────────── */
    const int sub_gop = cfg.sub_fps;
    if (venc_init(VENC_SUB_CHN_ID, SUB_STREAM_WIDTH, SUB_STREAM_HEIGHT,
                  RK_VIDEO_ID_AVC,
                  cfg.sub_bitrate_kbps, cfg.sub_fps, sub_gop) != 0) {
        fprintf(stderr, "[MAIN] VENC sub init failed\n");
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* ── 5. Intermediate NV12 buffers (VI → VENC copy buffers) ────────── */
    /*
     * We use our own DMA-capable MB blocks as the VENC input buffers.
     * This decouples the VI buffer lifecycle from VENC, preventing
     * buffer starvation under heavy RTSP load.
     *
     * The copy from VI → our buffer uses RGA DMA (rga_copy_nv12),
     * which offloads the 5.5 MB/frame transfer from the CPU to the
     * RGA engine — critical on a single Cortex-A7 @ 1.2 GHz.
     *
     * TODO: For further optimization, consider replacing this entire
     * manual pipeline with VI → VPSS → VENC hardware binding:
     *   VPSS Ch0 (passthrough) → VENC0 (main stream)
     *   VPSS Ch1 (scaled)      → VENC1 (sub stream)
     * This would eliminate both the copy and the manual RGA resize.
     */

    /* Main stream buffer */
    MB_POOL_CONFIG_S pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.u64MBSize   = (RK_U64)(STREAM_WIDTH * STREAM_HEIGHT * 3 / 2);
    pool_cfg.u32MBCnt    = 2;  /* double-buffer */
    pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    MB_POOL mb_pool = RK_MPI_MB_CreatePool(&pool_cfg);
    if (mb_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "[MAIN] MB pool creation failed\n");
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    MB_BLK mb_blk = RK_MPI_MB_GetMB(mb_pool,
                                     (RK_U32)(STREAM_WIDTH * STREAM_HEIGHT * 3 / 2),
                                     RK_TRUE);
    if (mb_blk == MB_INVALID_HANDLE) {
        fprintf(stderr, "[MAIN] MB pool allocation failed for main stream\n");
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    void *mb_vaddr = RK_MPI_MB_Handle2VirAddr(mb_blk);
    if (!mb_vaddr) {
        fprintf(stderr, "[MAIN] MB handle to virtual address failed for main stream\n");
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* Sub stream buffer (smaller resolution, separate DMA pool) */
    MB_POOL_CONFIG_S sub_pool_cfg;
    memset(&sub_pool_cfg, 0, sizeof(sub_pool_cfg));
    sub_pool_cfg.u64MBSize   = (RK_U64)(SUB_STREAM_WIDTH * SUB_STREAM_HEIGHT * 3 / 2);
    sub_pool_cfg.u32MBCnt    = 2;
    sub_pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    MB_POOL sub_mb_pool = RK_MPI_MB_CreatePool(&sub_pool_cfg);
    if (sub_mb_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "[MAIN] Sub MB pool creation failed\n");
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    MB_BLK sub_mb_blk = RK_MPI_MB_GetMB(sub_mb_pool,
                                          (RK_U32)(SUB_STREAM_WIDTH * SUB_STREAM_HEIGHT * 3 / 2),
                                          RK_TRUE);
    if (sub_mb_blk == MB_INVALID_HANDLE) {
        fprintf(stderr, "[MAIN] MB pool allocation failed for sub stream\n");
        RK_MPI_MB_DestroyPool(sub_mb_pool);
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    void *sub_mb_vaddr = RK_MPI_MB_Handle2VirAddr(sub_mb_blk);
    if (!sub_mb_vaddr) {
        fprintf(stderr, "[MAIN] MB handle to virtual address failed for sub stream\n");
        RK_MPI_MB_ReleaseMB(sub_mb_blk);
        RK_MPI_MB_DestroyPool(sub_mb_pool);
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* VENC input frame descriptor — main */
    VIDEO_FRAME_INFO_S venc_frame;
    memset(&venc_frame, 0, sizeof(venc_frame));
    venc_frame.stVFrame.u32Width     = (RK_U32)STREAM_WIDTH;
    venc_frame.stVFrame.u32Height    = (RK_U32)STREAM_HEIGHT;
    venc_frame.stVFrame.u32VirWidth  = (RK_U32)STREAM_WIDTH;
    venc_frame.stVFrame.u32VirHeight = (RK_U32)STREAM_HEIGHT;
    venc_frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    venc_frame.stVFrame.pMbBlk       = mb_blk;

    /* VENC input frame descriptor — sub */
    VIDEO_FRAME_INFO_S sub_venc_frame;
    memset(&sub_venc_frame, 0, sizeof(sub_venc_frame));
    sub_venc_frame.stVFrame.u32Width     = (RK_U32)SUB_STREAM_WIDTH;
    sub_venc_frame.stVFrame.u32Height    = (RK_U32)SUB_STREAM_HEIGHT;
    sub_venc_frame.stVFrame.u32VirWidth  = (RK_U32)SUB_STREAM_WIDTH;
    sub_venc_frame.stVFrame.u32VirHeight = (RK_U32)SUB_STREAM_HEIGHT;
    sub_venc_frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    sub_venc_frame.stVFrame.pMbBlk       = sub_mb_blk;

    /* ── 6. RTSP server ────────────────────────────────────────────────── */
    rtsp_demo_handle rtsp_handle = create_rtsp_demo(RTSP_PORT);
    if (!rtsp_handle) {
        fprintf(stderr, "[MAIN] RTSP server creation failed\n");
        RK_MPI_MB_ReleaseMB(sub_mb_blk);
        RK_MPI_MB_DestroyPool(sub_mb_pool);
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    /* Main stream session: /live/0 */
    rtsp_session_handle rtsp_session = rtsp_new_session(rtsp_handle, RTSP_PATH);
    if (!rtsp_session) {
        fprintf(stderr, "[MAIN] RTSP main session creation failed\n");
        rtsp_del_demo(rtsp_handle);
        RK_MPI_MB_ReleaseMB(sub_mb_blk);
        RK_MPI_MB_DestroyPool(sub_mb_pool);
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    rtsp_set_video(rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());

    /* Sub stream session: /live/1 (video-only, no audio) */
    rtsp_session_handle rtsp_sub_session = rtsp_new_session(rtsp_handle, RTSP_SUB_PATH);
    if (!rtsp_sub_session) {
        fprintf(stderr, "[MAIN] RTSP sub session creation failed\n");
        rtsp_del_demo(rtsp_handle);
        RK_MPI_MB_ReleaseMB(sub_mb_blk);
        RK_MPI_MB_DestroyPool(sub_mb_pool);
        RK_MPI_MB_ReleaseMB(mb_blk);
        RK_MPI_MB_DestroyPool(mb_pool);
        venc_deinit(VENC_SUB_CHN_ID);
        venc_deinit(VENC_CHN_ID);
        vi_deinit(VI_DEV_ID, VI_CHN_ID);
        RK_MPI_SYS_Exit();
        isp_stop();
        return 1;
    }

    rtsp_set_video(rtsp_sub_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(rtsp_sub_session, rtsp_get_reltime(), rtsp_get_ntptime());

#if ENABLE_AUDIO_STREAM
    audio_rtsp_ctx_t audio_ctx;
    memset(&audio_ctx, 0, sizeof(audio_ctx));
    int audio_enabled = 0;

    if (audio_rtsp_init(&audio_ctx, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS) == 0) {
        rtsp_set_audio(rtsp_session, RTSP_CODEC_ID_AUDIO_G711A, NULL, 0);
        rtsp_set_audio_sample_rate(rtsp_session, audio_ctx.sample_rate);
        rtsp_set_audio_channels(rtsp_session, audio_ctx.channels);
        rtsp_sync_audio_ts(rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());
        audio_enabled = 1;
    } else {
        fprintf(stderr, "[MAIN] WARNING: audio init failed, continuing with video-only stream\n");
    }

    mqtt_set_audio_runtime_enabled(audio_enabled);
#else
    mqtt_set_audio_runtime_enabled(0);
#endif

    /* ── 7. MQTT control client ─────────────────────────────────────────── */
    if (mqtt_client_start(&mqtt_cfg) != 0) {
        fprintf(stderr, "[MAIN] WARNING: MQTT client failed to start. "
                        "HA control will not be available.\n");
        /* Non-fatal — RTSP stream still works without MQTT */
    }

    /* ── 8. Main capture loop ──────────────────────────────────────────── */
    VENC_STREAM_S      venc_stream;
    VENC_STREAM_S      sub_venc_stream;
    VIDEO_FRAME_INFO_S vi_frame;
    RK_U32             time_ref     = 0;
    RK_U32             sub_time_ref = 0;
    int                vi_errors    = 0;

    mqtt_runtime_stats_t runtime_stats;
    memset(&runtime_stats, 0, sizeof(runtime_stats));
    mqtt_update_runtime_stats(&runtime_stats);

    time_t stats_window_start = time(NULL);
    int stats_frames_window = 0;
    unsigned long long stats_bytes_window = 0;
    int sub_stats_frames_window = 0;
    unsigned long long sub_stats_bytes_window = 0;

    /*
     * Sub stream frame decimation.
     * We use an accumulator to evenly distribute sub frames across
     * the main stream frame rate:
     *   sub_acc += sub_fps;
     *   if (sub_acc >= main_fps) { encode sub; sub_acc -= main_fps; }
     * Rates are cached and refreshed each stats cycle (every 1 sec).
     */
    int sub_frame_acc = 0;
    int cached_main_fps = cfg.fps;
    int cached_sub_fps  = cfg.sub_fps;
    int cached_mirror   = cfg.mirror;
    int cached_flip     = cfg.flip;
    time_t last_vi_ok_ts = time(NULL);
    time_t last_main_stream_ok_ts = time(NULL);
    int venc_send_errors = 0;
    int venc_get_errors = 0;

    memset(&venc_stream, 0, sizeof(venc_stream));
    memset(&sub_venc_stream, 0, sizeof(sub_venc_stream)); /* zero before any goto cleanup */
    venc_stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    if (!venc_stream.pstPack) {
        fprintf(stderr, "[MAIN] malloc venc pack failed\n");
        goto cleanup;
    }

    sub_venc_stream.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    if (!sub_venc_stream.pstPack) {
        fprintf(stderr, "[MAIN] malloc sub venc pack failed\n");
        free(venc_stream.pstPack);
        venc_stream.pstPack = NULL;
        goto cleanup;
    }

    printf("[MAIN] Streaming started. Press Ctrl+C to stop.\n");
    update_heartbeat_file();

    while (!g_quit) {
        /* 8.1 Capture VI frame (YUV420SP / NV12) */
        int ret = RK_MPI_VI_GetChnFrame(VI_DEV_ID, VI_CHN_ID, &vi_frame, 2000 /*ms*/);
        if (ret != RK_SUCCESS) {
            vi_errors++;
            if (vi_errors % MAX_VI_ERRORS == 0)
                printf("[MAIN] WARNING: %d consecutive VI errors (0x%x)\n",
                       vi_errors, ret);
            if (time(NULL) - last_vi_ok_ts >= STREAM_STALL_TIMEOUT_S) {
                fprintf(stderr, "[MAIN] FATAL: VI stalled for >= %d s, forcing restart\n",
                        STREAM_STALL_TIMEOUT_S);
                exit_code = 2;
                break;
            }
            usleep(10000); /* 10 ms back-off */
            continue;
        }
        vi_errors = 0;
        last_vi_ok_ts = time(NULL);

        /* 8.2 DMA copy NV12 data to our buffer via RGA (zero CPU load).
         * On Cortex-A7 @ 1.2 GHz the CPU memcpy of a 2592x1944 NV12 frame
         * (~7.6 MB) takes ~11 ms — 27% of the 40 ms frame budget at 25 fps.
         * RGA DMA copy frees the CPU entirely during the transfer.
         *
         * If the sensor handles mirror/flip natively (SC3336), we skip the
         * RGA transform and do a plain DMA copy instead. */
        void *vi_vaddr = RK_MPI_MB_Handle2VirAddr(vi_frame.stVFrame.pMbBlk);
        {
            int rga_mirror = cached_mirror;
            int rga_flip   = cached_flip;
            if (isp_sensor_supports_flip()) {
                /* Sensor already applied flip/mirror — don't double-flip */
                rga_mirror = 0;
                rga_flip   = 0;
            }
            if (rga_copy_nv12_transform(vi_vaddr, mb_vaddr,
                                        STREAM_WIDTH, STREAM_HEIGHT,
                                        rga_mirror, rga_flip) != 0) {
                /* Fallback to CPU copy if RGA fails */
                memcpy(mb_vaddr, vi_vaddr, (size_t)(STREAM_WIDTH * STREAM_HEIGHT * 3 / 2));
            }
        }

        /* 8.3 Release VI frame ASAP so VI buffer pool doesn't stall */
        ret = RK_MPI_VI_ReleaseChnFrame(VI_DEV_ID, VI_CHN_ID, &vi_frame);
        if (ret != RK_SUCCESS)
            printf("[MAIN] WARNING: VI release frame failed 0x%x\n", ret);

        /* 8.4 Set timestamps on the VENC input frame */
        venc_frame.stVFrame.u32TimeRef = time_ref++;
        venc_frame.stVFrame.u64PTS     = get_now_us();

        /* 8.5 Send NV12 frame to main hardware encoder */
        ret = RK_MPI_VENC_SendFrame(VENC_CHN_ID, &venc_frame, 2000);
        if (ret != RK_SUCCESS) {
            venc_send_errors++;
            printf("[MAIN] WARNING: VENC send frame failed 0x%x\n", ret);
            if (venc_send_errors >= MAX_VENC_SEND_ERRORS) {
                fprintf(stderr, "[MAIN] FATAL: repeated VENC send failures (%d), forcing restart\n",
                        venc_send_errors);
                exit_code = 3;
                break;
            }
            continue;
        }
        venc_send_errors = 0;

        /* 8.6 Retrieve the encoded H.264 NAL unit (main) */
        ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &venc_stream, 2000 /*ms*/);
        if (ret != RK_SUCCESS) {
            venc_get_errors++;
            printf("[MAIN] WARNING: VENC get stream failed 0x%x\n", ret);
            if (venc_get_errors >= MAX_VENC_GET_ERRORS ||
                time(NULL) - last_main_stream_ok_ts >= STREAM_STALL_TIMEOUT_S) {
                fprintf(stderr, "[MAIN] FATAL: main stream stalled (get failures=%d), forcing restart\n",
                        venc_get_errors);
                exit_code = 4;
                break;
            }
            continue;
        }
        venc_get_errors = 0;
        last_main_stream_ok_ts = time(NULL);

        stats_frames_window++;
        stats_bytes_window += (unsigned long long)venc_stream.pstPack->u32Len;

        /* 8.7 Transmit main stream over RTSP */
        if (rtsp_handle && rtsp_session) {
            void *pData = RK_MPI_MB_Handle2VirAddr(venc_stream.pstPack->pMbBlk);
            rtsp_tx_video(rtsp_session,
                          (uint8_t *)pData,
                          venc_stream.pstPack->u32Len,
                          venc_stream.pstPack->u64PTS);

#if ENABLE_AUDIO_STREAM
            if (audio_enabled) {
                (void)audio_rtsp_send_pending(&audio_ctx, rtsp_session, 8);
            }
#endif
        }

        /* 8.8 Release main encoded stream buffer */
        ret = RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &venc_stream);
        if (ret != RK_SUCCESS)
            printf("[MAIN] WARNING: VENC release stream failed 0x%x\n", ret);

        /* 8.9 Sub stream: frame decimation + RGA resize + encode + RTSP tx */
        {
            int main_fps = cached_main_fps;
            int s_fps    = cached_sub_fps;
            if (main_fps < 1) main_fps = 1;
            if (s_fps < 1) s_fps = 1;

            sub_frame_acc += s_fps;
            if (sub_frame_acc >= main_fps) {
                sub_frame_acc -= main_fps;

                /* RGA hardware resize: main NV12 → sub NV12 */
                if (rga_resize_nv12(mb_vaddr, STREAM_WIDTH, STREAM_HEIGHT,
                                    sub_mb_vaddr, SUB_STREAM_WIDTH, SUB_STREAM_HEIGHT) == 0) {

                    sub_venc_frame.stVFrame.u32TimeRef = sub_time_ref++;
                    sub_venc_frame.stVFrame.u64PTS     = get_now_us();

                    ret = RK_MPI_VENC_SendFrame(VENC_SUB_CHN_ID, &sub_venc_frame,
                                                SUB_VENC_SEND_TIMEOUT_MS);
                    if (ret == RK_SUCCESS) {
                        ret = RK_MPI_VENC_GetStream(VENC_SUB_CHN_ID, &sub_venc_stream,
                                                    SUB_VENC_GET_TIMEOUT_MS);
                        if (ret == RK_SUCCESS) {
                            sub_stats_frames_window++;
                            sub_stats_bytes_window += (unsigned long long)sub_venc_stream.pstPack->u32Len;

                            if (rtsp_handle && rtsp_sub_session) {
                                void *pSubData = RK_MPI_MB_Handle2VirAddr(sub_venc_stream.pstPack->pMbBlk);
                                rtsp_tx_video(rtsp_sub_session,
                                              (uint8_t *)pSubData,
                                              sub_venc_stream.pstPack->u32Len,
                                              sub_venc_stream.pstPack->u64PTS);
                            }
                            RK_MPI_VENC_ReleaseStream(VENC_SUB_CHN_ID, &sub_venc_stream);
                        } else {
                            printf("[MAIN] WARNING: SUB VENC get stream failed 0x%x\n", ret);
                        }
                    } else {
                        printf("[MAIN] WARNING: SUB VENC send frame failed 0x%x\n", ret);
                    }
                }
            }
        }

        /* RTSP event processing (handles both sessions) */
        if (rtsp_handle)
            rtsp_do_event(rtsp_handle);

        /* 8.10 Update statistics */
        time_t now = time(NULL);
        if (now - stats_window_start >= 1) {
            int elapsed_s = (int)(now - stats_window_start);
            if (elapsed_s < 1) elapsed_s = 1;

            runtime_stats.actual_fps = stats_frames_window / elapsed_s;
            runtime_stats.actual_bitrate_kbps = (int)((stats_bytes_window * 8ULL) / (1000ULL * (unsigned long long)elapsed_s));
            runtime_stats.sub_actual_fps = sub_stats_frames_window / elapsed_s;
            runtime_stats.sub_actual_bitrate_kbps = (int)((sub_stats_bytes_window * 8ULL) / (1000ULL * (unsigned long long)elapsed_s));
            mqtt_update_runtime_stats(&runtime_stats);

            /* Refresh cached FPS rates for sub stream decimation */
            {
                camera_settings_t cur;
                isp_get_settings(&cur);
                cached_main_fps = cur.fps;
                cached_sub_fps  = cur.sub_fps;
                cached_mirror   = cur.mirror;
                cached_flip     = cur.flip;
            }

            stats_frames_window = 0;
            stats_bytes_window = 0;
            sub_stats_frames_window = 0;
            sub_stats_bytes_window = 0;
            stats_window_start = now;
            update_heartbeat_file();
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────────────── */
    printf("[MAIN] Stopping...\n");
    free(sub_venc_stream.pstPack);
    free(venc_stream.pstPack);

cleanup:
    g_quit = 1; /* ensure all background threads see exit flag */

    if (g_heartbeat_running) {
        g_heartbeat_running = 0;
        pthread_join(g_heartbeat_thread, NULL);
    }

    mqtt_client_stop();

#if ENABLE_AUDIO_STREAM
    if (audio_enabled) {
        audio_rtsp_deinit(&audio_ctx);
    }
#endif

    if (rtsp_handle)
        rtsp_del_demo(rtsp_handle);

    RK_MPI_MB_ReleaseMB(sub_mb_blk);
    RK_MPI_MB_DestroyPool(sub_mb_pool);
    RK_MPI_MB_ReleaseMB(mb_blk);
    RK_MPI_MB_DestroyPool(mb_pool);

    venc_deinit(VENC_SUB_CHN_ID);
    venc_deinit(VENC_CHN_ID);
    vi_deinit(VI_DEV_ID, VI_CHN_ID);
    RK_MPI_SYS_Exit();

    isp_stop(); /* saves settings to disk */

    unlink(APP_HEARTBEAT_FILE);

    printf("[MAIN] Stopped cleanly.\n");
    return exit_code;
}
