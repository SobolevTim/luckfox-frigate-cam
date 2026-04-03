#pragma once
/*
 * mqtt_client.h — Minimal MQTT 3.1.1 client with Home Assistant Discovery.
 *
 * No external dependencies: raw POSIX TCP + pthreads only.
 *
 * On each (re)connect the client:
 *   1. Sends LWT "offline" as Will message (auto-published by broker on disconnect)
 *   2. Publishes "online" to <node_id>/availability  (retained)
 *   3. Publishes HA MQTT Discovery configs for all 12 controls (retained)
 *   4. Publishes current ISP state to <node_id>/state (retained)
 *   5. Publishes telemetry to <node_id>/telemetry (non-retained)
 *   6. Subscribes to <node_id>/+/set  (wildcard for all commands)
 *
 * Incoming commands are applied immediately via isp_control API and the
 * updated state is re-published.  Settings are persisted automatically.
 *
 * CLI options (see main.cc):
 *   --mqtt-host <ip>    broker address (default 127.0.0.1)
 *   --mqtt-port <port>  broker TCP port (default 1883)
 *   --mqtt-user <user>  optional username
 *   --mqtt-pass <pass>  optional password
 *   --mqtt-id   <id>    MQTT client ID / topic prefix (default luckfox_camera)
 *   --mqtt-name <name>  friendly device name shown in HA (default Luckfox Camera)
 *   --mqtt-discovery-refresh <sec> periodic discovery republish, 0=disabled
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char broker_host[256]; /* MQTT broker IP or hostname    */
    int  broker_port;      /* default 1883                  */
    char username[128];    /* optional                      */
    char password[128];    /* optional                      */
    char node_id[64];      /* topic prefix + HA device id   */
    char device_name[128]; /* friendly name shown in HA     */
    int  discovery_refresh_s; /* 0 disables periodic refresh */
} mqtt_config_t;

typedef struct {
    int actual_fps;
    int actual_bitrate_kbps;
    int sub_actual_fps;
    int sub_actual_bitrate_kbps;
} mqtt_runtime_stats_t;

/* Fill cfg with defaults (broker=127.0.0.1:1883, node_id="luckfox_camera") */
void mqtt_config_init(mqtt_config_t *cfg);

/* Start MQTT background thread.  Returns 0 on success. */
int  mqtt_client_start(const mqtt_config_t *cfg);

/* Stop background thread and cleanly disconnect from broker. */
void mqtt_client_stop(void);

/*
 * Publish current ISP settings to <node_id>/state as retained JSON.
 * Thread-safe — can be called from any thread.
 * Called automatically after each command; no need to call manually.
 */
void mqtt_publish_state(void);

/* Publish runtime telemetry to <node_id>/telemetry as non-retained JSON. */
void mqtt_publish_telemetry(void);

/* Update runtime counters consumed by telemetry payload builder. */
void mqtt_update_runtime_stats(const mqtt_runtime_stats_t *stats);

/*
 * Inform MQTT layer whether audio capture is currently active.
 * When disabled, audio tuning commands are ignored and only reported in state.
 */
void mqtt_set_audio_runtime_enabled(int enabled);

#ifdef __cplusplus
}
#endif
