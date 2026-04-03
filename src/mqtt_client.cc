/*
 * mqtt_client.cc — Minimal MQTT 3.1.1 client with Home Assistant Discovery.
 *
 * No external dependencies — raw POSIX TCP sockets + pthreads only.
 *
 * Topics used:
 *   <node_id>/availability          retained "online" / LWT "offline"
 *   <node_id>/state                 retained JSON with camera settings
 *   <node_id>/telemetry             non-retained runtime/diagnostic JSON
 *   <node_id>/ack                   non-retained command result JSON
 *   <node_id>/+/set                 command topics (wildcard subscription)
 *
 * HA Discovery topics (retained, published on every connect):
 *   homeassistant/number/<node_id>/<param>/config
 *   homeassistant/select/<node_id>/<param>/config
 *   homeassistant/switch/<node_id>/<param>/config
 */

#include "mqtt_client.h"
#include "isp_control.h"
#include "camera_mpi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

static int parse_int_payload(const char *value, int *out_value)
{
    char *end = NULL;
    long parsed = strtol(value, &end, 10);

    if (end == value || *end != '\0')
        return -1;

    *out_value = (int)parsed;
    return 0;
}

/* ── Tunables ────────────────────────────────────────────────────────────── */
#define MQTT_KEEPALIVE_S        20   /* broker keepalive (seconds)          */
#define MQTT_PING_INTERVAL_S    10   /* how often we send PINGREQ           */
#define MQTT_RECONNECT_MIN_S     5   /* initial reconnect back-off          */
#define MQTT_RECONNECT_MAX_S    60   /* maximum reconnect back-off          */
#define MQTT_TELEMETRY_INTERVAL_S 30 /* telemetry / state refresh interval  */
#define MQTT_DISCOVERY_REFRESH_S   0 /* periodic HA discovery refresh off   */
#define MQTT_BUF_SIZE         8192   /* packet scratch buffer               */
#define MQTT_CONNACK_TIMEOUT_S   5   /* timeout waiting for CONNACK         */
#define MQTT_TELEMETRY_EXPIRE_S ((MQTT_TELEMETRY_INTERVAL_S * 2) + 5)

/* ── Globals ─────────────────────────────────────────────────────────────── */
static mqtt_config_t    g_cfg;
static int              g_sock      = -1;
static volatile int     g_running   = 0;
static pthread_t        g_thread;
static pthread_mutex_t  g_write_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  g_stats_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct timespec  g_start_mono = {0, 0};
static mqtt_runtime_stats_t g_runtime_stats = {-1, -1, -1, -1};

typedef struct {
    unsigned long long idle;
    unsigned long long total;
    int                valid;
} cpu_sample_t;

typedef struct {
    const char *board_status;
    int cpu_usage_pct;
    int memory_usage_pct;
    int memory_used_mb;
    int board_uptime_s;
    int runtime_s;
    int soc_temp_c;
    int root_fs_usage_pct;
    int root_fs_avail_mb;
    int userdata_fs_usage_pct;
    int userdata_fs_avail_mb;
    int oem_fs_usage_pct;
    int oem_fs_avail_mb;
} telemetry_t;

static cpu_sample_t g_cpu_sample = {0, 0, 0};

static void runtime_stats_snapshot(mqtt_runtime_stats_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_stats_mtx);
    *out = g_runtime_stats;
    pthread_mutex_unlock(&g_stats_mtx);
}

void mqtt_update_runtime_stats(const mqtt_runtime_stats_t *stats)
{
    if (!stats) return;
    pthread_mutex_lock(&g_stats_mtx);
    g_runtime_stats = *stats;
    pthread_mutex_unlock(&g_stats_mtx);
}

#if ENABLE_AUDIO_STREAM
static volatile int g_audio_runtime_enabled = 0;
/* Raw ALSA values matching codec_hw_init defaults in audio_mpi.cc */
static int g_audio_adc_alc_left_gain  = 23;   /* ADC ALC Left Volume, range 0-31, 0 dB at 6 */
static int g_audio_adc_mic_left_gain  = 3;    /* ADC MIC Left Gain,   range 0-3  */
static int g_audio_hpf_enabled        = 1;    /* ADC HPF Cut-off: 1=On, 0=Off    */
static char g_audio_adc_micbias_voltage[32] = "VREFx0_975";
static char g_audio_adc_mode[32]            = "SingadcL";

static int run_amixer_cset(const char *control_name, const char *value)
{
    if (!control_name || control_name[0] == '\0' || !value || value[0] == '\0') {
        return -1;
    }

    const size_t control_len = strlen(control_name);
    if (control_len > 80) {
        return -1;
    }

    char name_arg[86];
    memcpy(name_arg, "name=", 5);
    memcpy(name_arg + 5, control_name, control_len + 1);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        execlp("amixer", "amixer", "cset", name_arg, value, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int audio_apply_control_num(const char *control_name, int value)
{
    if (!control_name || control_name[0] == '\0') return -1;

    char value_arg[16];
    int n = snprintf(value_arg, sizeof(value_arg), "%d", value);
    if (n <= 0 || n >= (int)sizeof(value_arg)) {
        return -1;
    }

    return run_amixer_cset(control_name, value_arg);
}

static int audio_apply_control_text(const char *control_name, const char *value)
{
    return run_amixer_cset(control_name, value);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * MQTT 3.1.1 packet builders
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Encode MQTT variable-length integer into buf, returns byte count (1-4). */
static int encode_varlen(uint8_t *buf, int val)
{
    int i = 0;
    do {
        buf[i] = (uint8_t)(val % 128);
        val    /= 128;
        if (val > 0) buf[i] |= 0x80;
        i++;
    } while (val > 0);
    return i;
}

/* Helpers to write into a growing uint8_t buffer */
static inline void put_u16(uint8_t *b, int &pos, uint16_t v)
{
    b[pos++] = (uint8_t)(v >> 8);
    b[pos++] = (uint8_t)(v & 0xFF);
}

static inline void put_str(uint8_t *b, int &pos, const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    put_u16(b, pos, len);
    memcpy(b + pos, s, len);
    pos += (int)len;
}

/*
 * Assemble: [fixed_hdr] [varlen(payload_len)] [payload]
 * Returns total packet length or -1 if buf too small.
 */
static int assemble(uint8_t *out, size_t outsz,
                    uint8_t fh, const uint8_t *payload, int plen)
{
    uint8_t rl[4];
    int     rl_bytes = encode_varlen(rl, plen);
    int     total    = 1 + rl_bytes + plen;
    if ((int)outsz < total) return -1;
    int pos = 0;
    out[pos++] = fh;
    memcpy(out + pos, rl, rl_bytes); pos += rl_bytes;
    memcpy(out + pos, payload, plen);
    return total;
}

/* Build CONNECT packet. Returns total length. */
static int build_connect(uint8_t *buf, size_t bufsz)
{
    uint8_t pl[512];
    int     pi = 0;

    put_str(pl, pi, "MQTT");   /* protocol name  */
    pl[pi++] = 0x04;           /* protocol level */

    /* Connect flags */
    uint8_t flags = 0x02;      /* clean session  */
    int has_will = 1;          /* always set LWT */
    int has_user = (g_cfg.username[0] != '\0');
    int has_pass = (has_user && g_cfg.password[0] != '\0');
    if (has_will) flags |= (0x04 | 0x20); /* will flag + will retain */
    if (has_user) flags |= 0x80;
    if (has_pass) flags |= 0x40;
    pl[pi++] = flags;

    /* Keepalive (big-endian) */
    pl[pi++] = (uint8_t)(MQTT_KEEPALIVE_S >> 8);
    pl[pi++] = (uint8_t)(MQTT_KEEPALIVE_S & 0xFF);

    /* Client ID */
    put_str(pl, pi, g_cfg.node_id);

    /* LWT: <node_id>/availability = "offline", retain */
    if (has_will) {
        char will_topic[128];
        snprintf(will_topic, sizeof(will_topic), "%s/availability", g_cfg.node_id);
        put_str(pl, pi, will_topic);
        put_str(pl, pi, "offline");
    }
    if (has_user) put_str(pl, pi, g_cfg.username);
    if (has_pass) put_str(pl, pi, g_cfg.password);

    return assemble(buf, bufsz, 0x10, pl, pi);
}

/* Build PUBLISH packet (QoS 0). retain=1 → retained message. */
static int build_publish(uint8_t *buf, size_t bufsz,
                         const char *topic, const char *payload, int retain)
{
    int tlen = (int)strlen(topic);
    int plen = (int)strlen(payload);
    uint8_t tmp[MQTT_BUF_SIZE];
    int pos = 0;
    put_u16(tmp, pos, (uint16_t)tlen);
    memcpy(tmp + pos, topic,   tlen); pos += tlen;
    memcpy(tmp + pos, payload, plen); pos += plen;
    uint8_t fh = (uint8_t)(0x30 | (retain ? 0x01 : 0x00));
    return assemble(buf, bufsz, fh, tmp, pos);
}

/* Build SUBSCRIBE packet (single topic, QoS 0). */
static int build_subscribe(uint8_t *buf, size_t bufsz,
                            const char *topic, uint16_t pkt_id)
{
    uint8_t tmp[512];
    int pos = 0;
    put_u16(tmp, pos, pkt_id);
    put_str(tmp, pos, topic);
    tmp[pos++] = 0x00; /* QoS 0 */
    return assemble(buf, bufsz, 0x82, tmp, pos);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Socket helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Write exactly len bytes to socket, mutex-protected. Returns 0 or -1. */
static int mqtt_send(const uint8_t *data, int len)
{
    pthread_mutex_lock(&g_write_mtx);
    int sent = 0;
    while (sent < len) {
        int r = (int)write(g_sock, data + sent, (size_t)(len - sent));
        if (r <= 0) {
            pthread_mutex_unlock(&g_write_mtx);
            return -1;
        }
        sent += r;
    }
    pthread_mutex_unlock(&g_write_mtx);
    return 0;
}

/* Publish a message (thread-safe). Returns 0 or -1. */
static int do_publish(const char *topic, const char *payload, int retain)
{
    if (g_sock < 0) return -1;
    uint8_t buf[MQTT_BUF_SIZE];
    int len = build_publish(buf, sizeof(buf), topic, payload, retain);
    if (len < 0) { fprintf(stderr, "[MQTT] publish buf too small\n"); return -1; }
    return mqtt_send(buf, len);
}

static int publish_availability(const char *status)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/availability", g_cfg.node_id);
    return do_publish(topic, status, 1);
}

/* Read exactly len bytes from socket. Returns 0 or -1. */
static int recv_exact(uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = (int)recv(g_sock, buf + got, (size_t)(len - got), 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* Decode MQTT variable-length remaining length. Returns 0 or -1. */
static int read_varlen(int *out)
{
    int mult = 1, val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (recv_exact(&b, 1) != 0) return -1;
        val  += (b & 0x7F) * mult;
        mult *= 128;
        if (!(b & 0x80)) break;
    }
    *out = val;
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t buf_sz)
{
    if (!path || !buf || buf_sz == 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    if (n == 0) return -1;

    buf[n] = '\0';
    return 0;
}

static void trim_trailing_whitespace(char *buf)
{
    if (!buf) return;

    size_t len = strlen(buf);
    while (len > 0) {
        char c = buf[len - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
        buf[--len] = '\0';
    }
}

static int read_cpu_usage_pct(int *out_pct)
{
    char buf[256];
    if (read_text_file("/proc/stat", buf, sizeof(buf)) != 0)
        return -1;

    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    int matched = sscanf(buf,
                         "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                         &user, &nice, &system, &idle,
                         &iowait, &irq, &softirq, &steal);
    if (matched < 4)
        return -1;

    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

    if (!g_cpu_sample.valid) {
        g_cpu_sample.idle  = idle_all;
        g_cpu_sample.total = total;
        g_cpu_sample.valid = 1;
        *out_pct = 0;
        return 0;
    }

    unsigned long long delta_total = total - g_cpu_sample.total;
    unsigned long long delta_idle  = idle_all - g_cpu_sample.idle;

    g_cpu_sample.idle  = idle_all;
    g_cpu_sample.total = total;

    if (delta_total == 0) {
        *out_pct = 0;
        return 0;
    }

    unsigned long long busy = (delta_total > delta_idle) ? (delta_total - delta_idle) : 0;
    *out_pct = (int)((busy * 100ULL) / delta_total);
    return 0;
}

static int read_memory_usage(int *out_used_mb, int *out_usage_pct)
{
    char buf[1024];
    if (read_text_file("/proc/meminfo", buf, sizeof(buf)) != 0)
        return -1;

    unsigned long total_kb = 0;
    unsigned long available_kb = 0;
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        unsigned long value = 0;
        if (sscanf(line, "MemTotal: %lu kB", &value) == 1)
            total_kb = value;
        else if (sscanf(line, "MemAvailable: %lu kB", &value) == 1)
            available_kb = value;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (total_kb == 0)
        return -1;

    unsigned long used_kb = (available_kb <= total_kb) ? (total_kb - available_kb) : 0;
    *out_used_mb = (int)(used_kb / 1024UL);
    *out_usage_pct = (int)((used_kb * 100UL) / total_kb);
    return 0;
}

static int read_board_uptime_s(int *out_seconds)
{
    struct sysinfo info;
    if (sysinfo(&info) != 0)
        return -1;

    *out_seconds = (int)info.uptime;
    return 0;
}

static int read_soc_temp_c(int *out_temp_c)
{
    char buf[64];
    if (read_text_file("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf)) != 0)
        return -1;

    trim_trailing_whitespace(buf);

    int milli_c = 0;
    if (parse_int_payload(buf, &milli_c) != 0)
        return -1;

    *out_temp_c = milli_c / 1000;
    return 0;
}

static int read_fs_usage(const char *mount_point, int *out_usage_pct, int *out_avail_mb)
{
    if (!mount_point || !out_usage_pct || !out_avail_mb)
        return -1;

    struct statvfs fs;
    if (statvfs(mount_point, &fs) != 0)
        return -1;

    unsigned long long total = (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_frsize;
    unsigned long long avail = (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
    unsigned long long used = (total > avail) ? (total - avail) : 0;

    if (total == 0)
        return -1;

    *out_usage_pct = (int)((used * 100ULL) / total);
    *out_avail_mb = (int)(avail / (1024ULL * 1024ULL));
    return 0;
}

static int read_runtime_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    if (g_start_mono.tv_sec == 0 && g_start_mono.tv_nsec == 0)
        return 0;

    time_t sec = now.tv_sec - g_start_mono.tv_sec;
    long nsec = now.tv_nsec - g_start_mono.tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }

    if (sec < 0)
        return 0;
    return (int)sec;
}

static void read_telemetry(telemetry_t *t)
{
    memset(t, 0, sizeof(*t));
    t->board_status = "online";
    t->cpu_usage_pct = -1;
    t->memory_usage_pct = -1;
    t->memory_used_mb = -1;
    t->board_uptime_s = -1;
    t->runtime_s = read_runtime_seconds();
    t->soc_temp_c = -1;
    t->root_fs_usage_pct = -1;
    t->root_fs_avail_mb = -1;
    t->userdata_fs_usage_pct = -1;
    t->userdata_fs_avail_mb = -1;
    t->oem_fs_usage_pct = -1;
    t->oem_fs_avail_mb = -1;

    read_cpu_usage_pct(&t->cpu_usage_pct);
    read_memory_usage(&t->memory_used_mb, &t->memory_usage_pct);
    read_board_uptime_s(&t->board_uptime_s);
    read_soc_temp_c(&t->soc_temp_c);
    read_fs_usage("/", &t->root_fs_usage_pct, &t->root_fs_avail_mb);
    read_fs_usage("/userdata", &t->userdata_fs_usage_pct, &t->userdata_fs_avail_mb);
    read_fs_usage("/oem", &t->oem_fs_usage_pct, &t->oem_fs_avail_mb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * State JSON + publish
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *WB_NAMES[] = {
    "auto","incandescent","fluorescent","warm_fluorescent",
    "daylight","cloudy","twilight","shade"
};

static void build_state_json(char *buf, int bufsz)
{
    camera_settings_t s;
    isp_get_settings(&s);

    int wb = (int)s.wb_preset;
    if (wb < 0 || wb > 7) wb = 0;

    snprintf(buf, (size_t)bufsz,
        "{"
        "\"brightness\":%u,"
        "\"contrast\":%u,"
        "\"saturation\":%u,"
        "\"hue\":%u,"
        "\"sharpness\":%u,"
        "\"daynight\":\"%s\","
        "\"wb_preset\":\"%s\","
        "\"mirror\":\"%s\","
        "\"flip\":\"%s\","
        "\"anti_flicker_en\":\"%s\","
        "\"anti_flicker_mode\":\"%s\","
        "\"bitrate_kbps\":%d,"
        "\"fps\":%d,"
        "\"sub_bitrate_kbps\":%d,"
        "\"sub_fps\":%d,"
        "\"night_mode\":\"%s\""
    #if ENABLE_AUDIO_STREAM
        ",\"audio_runtime_enabled\":\"%s\""
        ",\"audio_adc_alc_left_gain\":%d"
        ",\"audio_adc_mic_left_gain\":%d"
        ",\"audio_hpf\":\"%s\""
        ",\"audio_adc_micbias_voltage\":\"%s\""
        ",\"audio_adc_mode\":\"%s\""
    #endif
        "}",
        s.brightness,
        s.contrast,
        s.saturation,
        s.hue,
        s.sharpness,
        (s.daynight == DAYNIGHT_GRAY) ? "grayscale" : "color",
        WB_NAMES[wb],
        s.mirror           ? "ON" : "OFF",
        s.flip             ? "ON" : "OFF",
        s.anti_flicker_en  ? "ON" : "OFF",
        s.anti_flicker_mode ? "auto" : "50hz",
        s.bitrate_kbps,
        s.fps,
        s.sub_bitrate_kbps,
        s.sub_fps,
        s.night_mode ? "ON" : "OFF"
    #if ENABLE_AUDIO_STREAM
        ,
        g_audio_runtime_enabled ? "ON" : "OFF",
        g_audio_adc_alc_left_gain,
        g_audio_adc_mic_left_gain,
        g_audio_hpf_enabled ? "ON" : "OFF",
        g_audio_adc_micbias_voltage,
        g_audio_adc_mode
    #endif
    );
}

static void build_telemetry_json(char *buf, int bufsz)
{
    telemetry_t t;
    mqtt_runtime_stats_t rs;
    read_telemetry(&t);
    runtime_stats_snapshot(&rs);

    snprintf(buf, (size_t)bufsz,
        "{"
        "\"board_status\":\"%s\","
        "\"cpu_usage_pct\":%d,"
        "\"memory_usage_pct\":%d,"
        "\"memory_used_mb\":%d,"
        "\"board_uptime_s\":%d,"
        "\"runtime_s\":%d,"
        "\"soc_temp_c\":%d,"
        "\"actual_fps\":%d,"
        "\"actual_bitrate_kbps\":%d,"
        "\"sub_actual_fps\":%d,"
        "\"sub_actual_bitrate_kbps\":%d,"
        "\"root_fs_usage_pct\":%d,"
        "\"root_fs_avail_mb\":%d,"
        "\"userdata_fs_usage_pct\":%d,"
        "\"userdata_fs_avail_mb\":%d,"
        "\"oem_fs_usage_pct\":%d,"
        "\"oem_fs_avail_mb\":%d"
        "}",
        t.board_status,
        t.cpu_usage_pct,
        t.memory_usage_pct,
        t.memory_used_mb,
        t.board_uptime_s,
        t.runtime_s,
        t.soc_temp_c,
        rs.actual_fps,
        rs.actual_bitrate_kbps,
        rs.sub_actual_fps,
        rs.sub_actual_bitrate_kbps,
        t.root_fs_usage_pct,
        t.root_fs_avail_mb,
        t.userdata_fs_usage_pct,
        t.userdata_fs_avail_mb,
        t.oem_fs_usage_pct,
        t.oem_fs_avail_mb
    );
}

void mqtt_publish_state(void)
{
    char topic[128], payload[1024];
    snprintf(topic, sizeof(topic), "%s/state", g_cfg.node_id);
    build_state_json(payload, (int)sizeof(payload));
    do_publish(topic, payload, 1 /* retain */);
}

void mqtt_publish_telemetry(void)
{
    char topic[128], payload[1024];
    snprintf(topic, sizeof(topic), "%s/telemetry", g_cfg.node_id);
    build_telemetry_json(payload, (int)sizeof(payload));
    do_publish(topic, payload, 0 /* non-retained */);
}

void mqtt_set_audio_runtime_enabled(int enabled)
{
#if ENABLE_AUDIO_STREAM
    g_audio_runtime_enabled = enabled ? 1 : 0;
#else
    (void)enabled;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HA MQTT Discovery builders
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Device JSON fragment (reused in every discovery message) */
static void device_json(char *buf, int bufsz)
{
    snprintf(buf, (size_t)bufsz,
        "\"device\":{\"identifiers\":[\"%s\"],"
        "\"name\":\"%s\","
    "\"model\":\"RV1106 Camera (%s)\","
        "\"manufacturer\":\"Luckfox\"}",
    g_cfg.node_id, g_cfg.device_name, CAMERA_SENSOR_PROFILE);
}

static void publish_sensor(const char *param, const char *name,
                           const char *state_topic,
                           const char *val_tmpl, const char *unit,
                           const char *device_class, const char *state_class,
                           const char *icon, const char *entity_category,
                           int display_precision,
                           const char *availability_topic_override = NULL,
                           int expire_after_s = 0)
{
    char dtopic[256], dev[512], payload[2048];
    int pos = 0;
    const char *availability_topic = availability_topic_override != NULL
                                   ? availability_topic_override
                                   : "";
    char default_availability_topic[128];

    if (availability_topic_override == NULL) {
        snprintf(default_availability_topic, sizeof(default_availability_topic),
                 "%s/availability", g_cfg.node_id);
        availability_topic = default_availability_topic;
    }

    snprintf(dtopic, sizeof(dtopic),
             "homeassistant/sensor/%s/%s/config", g_cfg.node_id, param);
    device_json(dev, sizeof(dev));

    pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"value_template\":\"%s\"",
        name,
        g_cfg.node_id, param,
        state_topic, val_tmpl);

    if (availability_topic[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"availability_topic\":\"%s\"",
                        availability_topic);

    if (unit && unit[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class && device_class[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"device_class\":\"%s\"", device_class);
    if (state_class && state_class[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"state_class\":\"%s\"", state_class);
    if (icon && icon[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"icon\":\"%s\"", icon);
    if (entity_category && entity_category[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"entity_category\":\"%s\"", entity_category);
    if (display_precision >= 0)
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"suggested_display_precision\":%d", display_precision);
    if (expire_after_s > 0)
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"expire_after\":%d", expire_after_s);

    snprintf(payload + pos, sizeof(payload) - (size_t)pos,
             ",%s}", dev);
    int rc = do_publish(dtopic, payload, 1);
    printf("[MQTT] discovery sensor %-24s  %s\n",
           dtopic + strlen("homeassistant/sensor/"),
           rc == 0 ? "OK" : "FAILED");
}

static void publish_binary_sensor(const char *param, const char *name,
                                  const char *state_topic,
                                  const char *val_tmpl,
                                  const char *payload_on,
                                  const char *payload_off,
                                  const char *device_class,
                                  const char *icon,
                                  const char *entity_category,
                                  const char *availability_topic_override = NULL,
                                  int expire_after_s = 0)
{
    char dtopic[256], dev[512], payload[1024];
    int pos = 0;
    const char *availability_topic = availability_topic_override != NULL
                                   ? availability_topic_override
                                   : "";
    char default_availability_topic[128];

    if (availability_topic_override == NULL) {
        snprintf(default_availability_topic, sizeof(default_availability_topic),
                 "%s/availability", g_cfg.node_id);
        availability_topic = default_availability_topic;
    }

    snprintf(dtopic, sizeof(dtopic),
             "homeassistant/binary_sensor/%s/%s/config", g_cfg.node_id, param);
    device_json(dev, sizeof(dev));

    pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"value_template\":\"%s\","
        "\"payload_on\":\"%s\","
        "\"payload_off\":\"%s\"",
        name,
        g_cfg.node_id, param,
        state_topic, val_tmpl,
        payload_on,
        payload_off);

    if (availability_topic[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"availability_topic\":\"%s\"",
                        availability_topic);

    if (device_class && device_class[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"device_class\":\"%s\"", device_class);
    if (icon && icon[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"icon\":\"%s\"", icon);
    if (entity_category && entity_category[0] != '\0')
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"entity_category\":\"%s\"", entity_category);
    if (expire_after_s > 0)
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                        ",\"expire_after\":%d", expire_after_s);

    snprintf(payload + pos, sizeof(payload) - (size_t)pos,
             ",%s}", dev);
    int rc = do_publish(dtopic, payload, 1);
    printf("[MQTT] discovery bsensor %-22s  %s\n",
           dtopic + strlen("homeassistant/binary_sensor/"),
           rc == 0 ? "OK" : "FAILED");
}

static void publish_number(const char *param, const char *name,
                           int min, int max)
{
    char dtopic[256], dev[512], payload[1024];
    snprintf(dtopic,  sizeof(dtopic),
             "homeassistant/number/%s/%s/config", g_cfg.node_id, param);
    device_json(dev, sizeof(dev));
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"{{ value_json.%s }}\","
        "\"command_topic\":\"%s/%s/set\","
        "\"min\":%d,\"max\":%d,\"step\":1,"
        "\"availability_topic\":\"%s/availability\","
        "%s}",
        name,
        g_cfg.node_id, param,
        g_cfg.node_id, param,
        g_cfg.node_id, param,
        min, max,
        g_cfg.node_id,
        dev);
    do_publish(dtopic, payload, 1);
}

static void publish_select(const char *param, const char *name,
                           const char *val_tmpl, const char *options_json)
{
    char dtopic[256], dev[512], payload[1024];
    snprintf(dtopic, sizeof(dtopic),
             "homeassistant/select/%s/%s/config", g_cfg.node_id, param);
    device_json(dev, sizeof(dev));
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"%s\","
        "\"command_topic\":\"%s/%s/set\","
        "\"options\":%s,"
        "\"availability_topic\":\"%s/availability\","
        "%s}",
        name,
        g_cfg.node_id, param,
        g_cfg.node_id, val_tmpl,
        g_cfg.node_id, param,
        options_json,
        g_cfg.node_id,
        dev);
    do_publish(dtopic, payload, 1);
}

static void publish_switch(const char *param, const char *name,
                           const char *val_tmpl)
{
    char dtopic[256], dev[512], payload[1024];
    snprintf(dtopic, sizeof(dtopic),
             "homeassistant/switch/%s/%s/config", g_cfg.node_id, param);
    device_json(dev, sizeof(dev));
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s/state\","
        "\"value_template\":\"%s\","
        "\"command_topic\":\"%s/%s/set\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"availability_topic\":\"%s/availability\","
        "%s}",
        name,
        g_cfg.node_id, param,
        g_cfg.node_id, val_tmpl,
        g_cfg.node_id, param,
        g_cfg.node_id,
        dev);
    do_publish(dtopic, payload, 1);
}

static void delete_discovery(const char *component, const char *param)
{
    char dtopic[256];
    snprintf(dtopic, sizeof(dtopic),
             "homeassistant/%s/%s/%s/config", component, g_cfg.node_id, param);
    do_publish(dtopic, "", 1);
}

static void delete_sensor_discovery(const char *param)
{
    delete_discovery("sensor", param);
}

static void delete_binary_sensor_discovery(const char *param)
{
    delete_discovery("binary_sensor", param);
}

static void delete_number_discovery(const char *param)
{
    delete_discovery("number", param);
}

static void delete_select_discovery(const char *param)
{
    delete_discovery("select", param);
}

static void delete_switch_discovery(const char *param)
{
    delete_discovery("switch", param);
}

static void publish_all_discovery(void)
{
    char state_topic[128];
    char telemetry_topic[128];
    char availability_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/state", g_cfg.node_id);
    snprintf(telemetry_topic, sizeof(telemetry_topic), "%s/telemetry", g_cfg.node_id);
    snprintf(availability_topic, sizeof(availability_topic), "%s/availability", g_cfg.node_id);

    /* Remove obsolete telemetry entities from previous releases. */
    delete_sensor_discovery("vi_error_count");
    delete_sensor_discovery("venc_error_count");
    delete_sensor_discovery("rtsp_client_count");

    /* Numbers */
    publish_number("brightness",   "Brightness",     0, 255);
    publish_number("contrast",     "Contrast",       0, 255);
    publish_number("saturation",   "Saturation",     0, 255);
    publish_number("hue",          "Hue",            0, 255);
    publish_number("sharpness",    "Sharpness",      0, 100);
    publish_number("bitrate_kbps", "Bitrate (kbps)", 1000, 20000);
    publish_number("fps",          "FPS",            10, 30);

    /* Sub stream controls */
    publish_number("sub_bitrate_kbps", "Sub Bitrate (kbps)", 100, 5000);
    publish_number("sub_fps",          "Sub FPS",            5, 30);

    /* Selects */
    publish_select("daynight", "Day/Night Mode",
        "{{ value_json.daynight }}",
        "[\"color\",\"grayscale\"]");

    publish_select("wb_preset", "White Balance",
        "{{ value_json.wb_preset }}",
        "[\"auto\",\"incandescent\",\"fluorescent\",\"warm_fluorescent\","
        "\"daylight\",\"cloudy\",\"twilight\",\"shade\"]");

    publish_select("anti_flicker_mode", "Anti-flicker Freq",
        "{{ value_json.anti_flicker_mode }}",
        "[\"50hz\",\"auto\"]");

    /* Switches */
    publish_switch("mirror",         "Mirror",
        "{{ value_json.mirror }}");
    publish_switch("flip",           "Flip",
        "{{ value_json.flip }}");
    publish_switch("anti_flicker_en","Anti-flicker",
        "{{ value_json.anti_flicker_en }}");
    publish_switch("night_mode",     "Night Mode",
        "{{ value_json.night_mode }}");

#if ENABLE_AUDIO_STREAM
    publish_binary_sensor("audio_runtime_enabled", "Audio Runtime",
        state_topic,
        "{{ value_json.audio_runtime_enabled }}",
        "ON", "OFF",
        NULL, "mdi:microphone", "diagnostic");

    publish_number("audio_adc_alc_left_gain", "Audio ALC Left Gain", 0, 31);
    publish_number("audio_adc_mic_left_gain", "Audio MIC Left Gain", 0, 3);

    publish_switch("audio_hpf", "Audio HPF",
        "{{ value_json.audio_hpf }}");

    publish_select("audio_adc_micbias_voltage", "Audio MICBIAS Voltage",
        "{{ value_json.audio_adc_micbias_voltage }}",
        "[\"VREFx0_8\",\"VREFx0_825\",\"VREFx0_85\",\"VREFx0_875\","
        "\"VREFx0_9\",\"VREFx0_925\",\"VREFx0_95\",\"VREFx0_975\"]");

    publish_select("audio_adc_mode", "Audio ADC Mode",
        "{{ value_json.audio_adc_mode }}",
        "[\"SingadcL\",\"DiffadcL\",\"DiffadcR\",\"SingadcR\",\"SingadcLR\",\"DiffadcLR\"]");
#else
    delete_binary_sensor_discovery("audio_runtime_enabled");
    delete_number_discovery("audio_adc_alc_left_gain");
    delete_number_discovery("audio_adc_mic_left_gain");
    delete_switch_discovery("audio_hpf");
    delete_select_discovery("audio_adc_micbias_voltage");
    delete_select_discovery("audio_adc_mode");
#endif

    /* Telemetry sensors */
    publish_binary_sensor("board_status", "Board Status",
        availability_topic,
        "{{ value }}",
        "online", "offline",
        "connectivity", "mdi:lan-connect", "diagnostic",
        "", 0);
    publish_sensor("cpu_usage_pct", "CPU Usage",
        telemetry_topic,
        "{{ value_json.cpu_usage_pct }}",
        "%", NULL, "measurement", "mdi:cpu-64-bit", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("memory_usage_pct", "Memory Usage",
        telemetry_topic,
        "{{ value_json.memory_usage_pct }}",
        "%", NULL, "measurement", "mdi:memory", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("memory_used_mb", "Memory Used",
        telemetry_topic,
        "{{ value_json.memory_used_mb }}",
        "MB", "data_size", "measurement", "mdi:memory", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("board_uptime_s", "Board Uptime",
        telemetry_topic,
        "{{ value_json.board_uptime_s }}",
        "s", "duration", "measurement", "mdi:timer-outline", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("runtime_s", "Camera Runtime",
        telemetry_topic,
        "{{ value_json.runtime_s }}",
        "s", "duration", "measurement", "mdi:camera-timer", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("soc_temp_c", "SoC Temperature",
        telemetry_topic,
        "{{ value_json.soc_temp_c }}",
        "°C", "temperature", "measurement", "mdi:thermometer", "diagnostic",
        1, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("actual_fps", "Actual FPS",
        telemetry_topic,
        "{{ value_json.actual_fps }}",
        "fps", NULL, "measurement", "mdi:speedometer", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("actual_bitrate_kbps", "Actual Bitrate",
        telemetry_topic,
        "{{ value_json.actual_bitrate_kbps }}",
        "kbps", NULL, "measurement", "mdi:wifi", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("sub_actual_fps", "Sub Actual FPS",
        telemetry_topic,
        "{{ value_json.sub_actual_fps }}",
        "fps", NULL, "measurement", "mdi:speedometer-slow", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("sub_actual_bitrate_kbps", "Sub Actual Bitrate",
        telemetry_topic,
        "{{ value_json.sub_actual_bitrate_kbps }}",
        "kbps", NULL, "measurement", "mdi:wifi", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("root_fs_usage_pct", "Root FS Usage",
        telemetry_topic,
        "{{ value_json.root_fs_usage_pct }}",
        "%", NULL, "measurement", "mdi:harddisk", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("root_fs_avail_mb", "Root FS Available",
        telemetry_topic,
        "{{ value_json.root_fs_avail_mb }}",
        "MB", "data_size", "measurement", "mdi:harddisk", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("userdata_fs_usage_pct", "Userdata FS Usage",
        telemetry_topic,
        "{{ value_json.userdata_fs_usage_pct }}",
        "%", NULL, "measurement", "mdi:database", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("userdata_fs_avail_mb", "Userdata FS Available",
        telemetry_topic,
        "{{ value_json.userdata_fs_avail_mb }}",
        "MB", "data_size", "measurement", "mdi:database", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("oem_fs_usage_pct", "OEM FS Usage",
        telemetry_topic,
        "{{ value_json.oem_fs_usage_pct }}",
        "%", NULL, "measurement", "mdi:chip", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
    publish_sensor("oem_fs_avail_mb", "OEM FS Available",
        telemetry_topic,
        "{{ value_json.oem_fs_avail_mb }}",
        "MB", "data_size", "measurement", "mdi:chip", "diagnostic",
        0, NULL, MQTT_TELEMETRY_EXPIRE_S);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Command dispatcher
 * ═══════════════════════════════════════════════════════════════════════════ */

static void publish_ack(const char *param, const char *status, const char *message)
{
    char topic[128];
    char payload[512];
    snprintf(topic, sizeof(topic), "%s/ack", g_cfg.node_id);
    snprintf(payload, sizeof(payload),
             "{\"param\":\"%s\",\"status\":\"%s\",\"message\":\"%s\"}",
             param ? param : "unknown",
             status ? status : "error",
             message ? message : "unspecified");
    do_publish(topic, payload, 0);
}

static int parse_int_in_range(const char *value, int min_v, int max_v, int *out_value)
{
    if (parse_int_payload(value, out_value) != 0)
        return -1;
    if (*out_value < min_v || *out_value > max_v)
        return -1;
    return 0;
}

/*
 * Called when a PUBLISH arrives on <node_id>/+/set.
 * topic   — full topic string
 * payload — message bytes (not null-terminated)
 * pay_len — payload length
 */
static void dispatch_command(const char *topic,
                              const char *payload_raw, int pay_len)
{
    /* Extract parameter name between first '/' after node_id and '/set' */
    const char *p = topic + strlen(g_cfg.node_id) + 1;
    const char *slash = strchr(p, '/');
    if (!slash) return;

    char param[64] = {0};
    int  plen = (int)(slash - p);
    if (plen <= 0 || plen >= (int)sizeof(param)) return;
    memcpy(param, p, plen);

    /* Null-terminate payload */
    char val[256] = {0};
    int  vlen = pay_len < (int)sizeof(val) - 1 ? pay_len : (int)sizeof(val) - 1;
    memcpy(val, payload_raw, vlen);

    printf("[MQTT] cmd  %s = \"%s\"\n", param, val);

#if ENABLE_AUDIO_STREAM
    if (strncmp(param, "audio_", 6) == 0 && !g_audio_runtime_enabled) {
        printf("[MQTT] audio command ignored: microphone is not running\n");
        publish_ack(param, "error", "audio_runtime_disabled");
        mqtt_publish_state();
        return;
    }
#endif

    /* Snapshot current settings — partial updates leave everything else alone */
    camera_settings_t prev, next;
    isp_get_settings(&prev);
    next = prev;
    int parsed = 0;

    /* ── Numeric params ──────────────────────────────────────────────────── */
    if      (!strcmp(param, "brightness")) {
        if (parse_int_in_range(val, 0, 255, &parsed) != 0) {
            publish_ack(param, "error", "brightness_range_0_255");
            return;
        }
        next.brightness = (unsigned)parsed;
    }
    else if (!strcmp(param, "contrast")) {
        if (parse_int_in_range(val, 0, 255, &parsed) != 0) {
            publish_ack(param, "error", "contrast_range_0_255");
            return;
        }
        next.contrast = (unsigned)parsed;
    }
    else if (!strcmp(param, "saturation")) {
        if (parse_int_in_range(val, 0, 255, &parsed) != 0) {
            publish_ack(param, "error", "saturation_range_0_255");
            return;
        }
        next.saturation = (unsigned)parsed;
    }
    else if (!strcmp(param, "hue")) {
        if (parse_int_in_range(val, 0, 255, &parsed) != 0) {
            publish_ack(param, "error", "hue_range_0_255");
            return;
        }
        next.hue = (unsigned)parsed;
    }
    else if (!strcmp(param, "sharpness")) {
        if (parse_int_in_range(val, 0, 100, &parsed) != 0) {
            publish_ack(param, "error", "sharpness_range_0_100");
            return;
        }
        next.sharpness = (unsigned)parsed;
    }
    else if (!strcmp(param, "bitrate_kbps")) {
        if (parse_int_in_range(val, 1000, 20000, &parsed) != 0) {
            publish_ack(param, "error", "bitrate_range_1000_20000");
            return;
        }
        next.bitrate_kbps = parsed;
    }
    else if (!strcmp(param, "fps")) {
        if (parse_int_in_range(val, 10, 30, &parsed) != 0) {
            publish_ack(param, "error", "fps_range_10_30");
            return;
        }
        next.fps = parsed;
    }
    else if (!strcmp(param, "sub_bitrate_kbps")) {
        if (parse_int_in_range(val, 100, 5000, &parsed) != 0) {
            publish_ack(param, "error", "sub_bitrate_range_100_5000");
            return;
        }
        next.sub_bitrate_kbps = parsed;
    }
    else if (!strcmp(param, "sub_fps")) {
        if (parse_int_in_range(val, 5, 30, &parsed) != 0) {
            publish_ack(param, "error", "sub_fps_range_5_30");
            return;
        }
        next.sub_fps = parsed;
    }

    /* ── String / enum params ────────────────────────────────────────────── */
    else if (!strcmp(param, "daynight")) {
        if (!strcmp(val, "grayscale") || !strcmp(val, "1")) {
            next.daynight = DAYNIGHT_GRAY;
        } else if (!strcmp(val, "color") || !strcmp(val, "0")) {
            next.daynight = DAYNIGHT_COLOR;
        } else {
            publish_ack(param, "error", "daynight_expected_color_or_grayscale");
            return;
        }
    }
    else if (!strcmp(param, "wb_preset")) {
        int found = 0;
        for (int i = 0; i < 8; i++) {
            if (!strcmp(val, WB_NAMES[i])) {
                next.wb_preset = (wb_preset_t)i;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (parse_int_in_range(val, 0, 7, &parsed) != 0) {
                publish_ack(param, "error", "wb_preset_expected_name_or_0_7");
                return;
            }
            next.wb_preset = (wb_preset_t)parsed;
        }
    }
    else if (!strcmp(param, "mirror")) {
        if (!strcmp(val, "ON") || !strcmp(val, "1")) next.mirror = 1;
        else if (!strcmp(val, "OFF") || !strcmp(val, "0")) next.mirror = 0;
        else {
            publish_ack(param, "error", "mirror_expected_ON_OFF");
            return;
        }
    }
    else if (!strcmp(param, "flip")) {
        if (!strcmp(val, "ON") || !strcmp(val, "1")) next.flip = 1;
        else if (!strcmp(val, "OFF") || !strcmp(val, "0")) next.flip = 0;
        else {
            publish_ack(param, "error", "flip_expected_ON_OFF");
            return;
        }
    }
    else if (!strcmp(param, "anti_flicker_en")) {
        if (!strcmp(val, "ON") || !strcmp(val, "1")) next.anti_flicker_en = 1;
        else if (!strcmp(val, "OFF") || !strcmp(val, "0")) next.anti_flicker_en = 0;
        else {
            publish_ack(param, "error", "anti_flicker_en_expected_ON_OFF");
            return;
        }
    }
    else if (!strcmp(param, "anti_flicker_mode")) {
        if (!strcmp(val, "auto") || !strcmp(val, "1")) next.anti_flicker_mode = 1;
        else if (!strcmp(val, "50hz") || !strcmp(val, "0")) next.anti_flicker_mode = 0;
        else {
            publish_ack(param, "error", "anti_flicker_mode_expected_50hz_or_auto");
            return;
        }
    }
    else if (!strcmp(param, "night_mode")) {
        int enable;
        if (!strcmp(val, "ON") || !strcmp(val, "1")) enable = 1;
        else if (!strcmp(val, "OFF") || !strcmp(val, "0")) enable = 0;
        else {
            publish_ack(param, "error", "night_mode_expected_ON_OFF");
            return;
        }
        if (isp_set_night_mode(enable) != 0) {
            publish_ack(param, "error", "night_mode_apply_failed");
            mqtt_publish_state();
            return;
        }
        isp_save_settings(ISP_SETTINGS_FILE);
        mqtt_publish_state();
        publish_ack(param, "ok", "applied");
        return;
    }
#if ENABLE_AUDIO_STREAM
    else if (!strcmp(param, "audio_adc_alc_left_gain")) {
        /* ADC ALC Left Volume: range 0-31; 0 dB = 6, step 1.5 dB, max +37.5 dB at 31 */
        if (parse_int_in_range(val, 0, 31, &parsed) != 0) {
            publish_ack(param, "error", "audio_adc_alc_left_gain_range_0_31");
            return;
        }

        if (audio_apply_control_num("ADC ALC Left Volume", parsed) == 0) {
            g_audio_adc_alc_left_gain = parsed;
            publish_ack(param, "ok", "applied");
        } else {
            printf("[MQTT] failed to apply ADC ALC Left Volume=%d\n", parsed);
            publish_ack(param, "error", "audio_control_apply_failed");
        }

        mqtt_publish_state();
        return;
    }
    else if (!strcmp(param, "audio_adc_mic_left_gain")) {
        /* ADC MIC Left Gain: range 0-3; 0=off, 1=0 dB, 2=20 dB, 3=max */
        if (parse_int_in_range(val, 0, 3, &parsed) != 0) {
            publish_ack(param, "error", "audio_adc_mic_left_gain_range_0_3");
            return;
        }

        if (audio_apply_control_num("ADC MIC Left Gain", parsed) == 0) {
            g_audio_adc_mic_left_gain = parsed;
            publish_ack(param, "ok", "applied");
        } else {
            printf("[MQTT] failed to apply ADC MIC Left Gain=%d\n", parsed);
            publish_ack(param, "error", "audio_control_apply_failed");
        }

        mqtt_publish_state();
        return;
    }
    else if (!strcmp(param, "audio_hpf")) {
        /* ADC HPF Cut-off: On removes DC offset and sub-bass rumble */
        int enable;
        if (!strcmp(val, "ON") || !strcmp(val, "On") || !strcmp(val, "1")) enable = 1;
        else if (!strcmp(val, "OFF") || !strcmp(val, "Off") || !strcmp(val, "0")) enable = 0;
        else {
            publish_ack(param, "error", "audio_hpf_expected_ON_OFF");
            return;
        }

        if (audio_apply_control_text("ADC HPF Cut-off", enable ? "On" : "Off") == 0) {
            g_audio_hpf_enabled = enable;
            publish_ack(param, "ok", "applied");
        } else {
            printf("[MQTT] failed to apply ADC HPF Cut-off=%s\n", enable ? "On" : "Off");
            publish_ack(param, "error", "audio_control_apply_failed");
        }

        mqtt_publish_state();
        return;
    }
    else if (!strcmp(param, "audio_adc_micbias_voltage")) {
        /* Full range of valid MICBIAS voltages per RV1106 codec datasheet */
        const char *allowed[] = {
            "VREFx0_8", "VREFx0_825", "VREFx0_85", "VREFx0_875",
            "VREFx0_9", "VREFx0_925", "VREFx0_95", "VREFx0_975"
        };
        int allowed_ok = 0;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
            if (!strcmp(val, allowed[i])) {
                allowed_ok = 1;
                break;
            }
        }
        if (!allowed_ok) {
            printf("[MQTT] invalid audio_adc_micbias_voltage: %s\n", val);
            publish_ack(param, "error", "audio_adc_micbias_voltage_invalid");
            return;
        }

        if (audio_apply_control_text("ADC MICBIAS Voltage", val) == 0) {
            strncpy(g_audio_adc_micbias_voltage, val, sizeof(g_audio_adc_micbias_voltage) - 1);
            g_audio_adc_micbias_voltage[sizeof(g_audio_adc_micbias_voltage) - 1] = '\0';
            publish_ack(param, "ok", "applied");
        } else {
            printf("[MQTT] failed to apply ADC MICBIAS Voltage=%s\n", val);
            publish_ack(param, "error", "audio_control_apply_failed");
        }

        mqtt_publish_state();
        return;
    }
    else if (!strcmp(param, "audio_adc_mode")) {
        /* DiffadcL = differential (not useful for onboard mic); SingadcL = single-ended */
        const char *allowed[] = {"SingadcL", "DiffadcL", "DiffadcR", "SingadcR", "SingadcLR", "DiffadcLR"};
        int allowed_ok = 0;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
            if (!strcmp(val, allowed[i])) {
                allowed_ok = 1;
                break;
            }
        }
        if (!allowed_ok) {
            printf("[MQTT] invalid audio_adc_mode: %s\n", val);
            publish_ack(param, "error", "audio_adc_mode_invalid");
            return;
        }

        if (audio_apply_control_text("ADC Mode", val) == 0) {
            strncpy(g_audio_adc_mode, val, sizeof(g_audio_adc_mode) - 1);
            g_audio_adc_mode[sizeof(g_audio_adc_mode) - 1] = '\0';
            publish_ack(param, "ok", "applied");
        } else {
            printf("[MQTT] failed to apply ADC Mode=%s\n", val);
            publish_ack(param, "error", "audio_control_apply_failed");
        }

        mqtt_publish_state();
        return;
    }
#endif
    else {
        printf("[MQTT] unknown param: %s\n", param);
        publish_ack(param, "error", "unknown_param");
        return;
    }

    /* ── Apply only changed fields to avoid redundant ISP writes ─────────── */
    int apply_failed = 0;

    if (next.brightness != prev.brightness) apply_failed |= (isp_set_brightness(next.brightness) != 0);
    if (next.contrast   != prev.contrast)   apply_failed |= (isp_set_contrast  (next.contrast) != 0);
    if (next.saturation != prev.saturation) apply_failed |= (isp_set_saturation(next.saturation) != 0);
    if (next.hue        != prev.hue)        apply_failed |= (isp_set_hue       (next.hue) != 0);
    if (next.sharpness  != prev.sharpness)  apply_failed |= (isp_set_sharpness (next.sharpness) != 0);
    if (next.daynight   != prev.daynight)   apply_failed |= (isp_set_daynight  (next.daynight) != 0);
    if (next.wb_preset  != prev.wb_preset)  apply_failed |= (isp_set_wb_preset (next.wb_preset) != 0);

    if (next.mirror != prev.mirror || next.flip != prev.flip)
        apply_failed |= (isp_set_mirror_flip(next.mirror, next.flip) != 0);

    if (next.anti_flicker_en   != prev.anti_flicker_en ||
        next.anti_flicker_mode != prev.anti_flicker_mode)
        apply_failed |= (isp_set_anti_flicker(next.anti_flicker_en, next.anti_flicker_mode) != 0);

    if (next.bitrate_kbps != prev.bitrate_kbps)
        apply_failed |= (isp_set_bitrate_kbps(next.bitrate_kbps) != 0);

    if (next.fps != prev.fps)
        apply_failed |= (isp_set_fps(next.fps) != 0);

    if (next.sub_bitrate_kbps != prev.sub_bitrate_kbps)
        apply_failed |= (isp_set_sub_bitrate_kbps(next.sub_bitrate_kbps) != 0);

    if (next.sub_fps != prev.sub_fps)
        apply_failed |= (isp_set_sub_fps(next.sub_fps) != 0);

    /* Persist and report new state only after a successful apply. */
    if (!apply_failed)
        isp_save_settings(ISP_SETTINGS_FILE);
    mqtt_publish_state();
    publish_ack(param, apply_failed ? "error" : "ok",
                apply_failed ? "apply_failed" : "applied");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Packet receive loop
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Read and dispatch ONE incoming MQTT packet.
 * Returns 0 = ok, -1 = connection error (caller should reconnect).
 */
static int process_one_packet(void)
{
    uint8_t fh;
    if (recv_exact(&fh, 1) != 0) return -1;
    uint8_t ptype = (fh >> 4);

    int remaining = 0;
    if (read_varlen(&remaining) != 0) return -1;

    /* Read payload into scratch buffer; drain oversized packets safely */
    uint8_t buf[MQTT_BUF_SIZE];
    if (remaining > (int)sizeof(buf)) {
        uint8_t discard[256];
        int left = remaining;
        while (left > 0) {
            int chunk = left < (int)sizeof(discard) ? left : (int)sizeof(discard);
            if (recv_exact(discard, chunk) != 0) return -1;
            left -= chunk;
        }
        return 0; /* ignore oversized */
    }
    if (remaining > 0 && recv_exact(buf, remaining) != 0) return -1;

    switch (ptype) {
    case 2: /* CONNACK */
        if (remaining >= 2 && buf[1] != 0x00) {
            printf("[MQTT] CONNACK refused (code %d)\n", (int)buf[1]);
            return -1;
        }
        printf("[MQTT] Connected to %s:%d\n",
               g_cfg.broker_host, g_cfg.broker_port);
        break;

    case 3: { /* PUBLISH — incoming command */
        if (remaining < 2) break;
        int ti = 0;
        uint16_t tlen = (uint16_t)(((uint16_t)buf[ti] << 8) | buf[ti+1]);
        ti += 2;
        if (tlen > (uint16_t)(remaining - 2) || tlen >= 256) break;
        char topic[256];
        memcpy(topic, buf + ti, tlen);
        topic[tlen] = '\0';
        ti += tlen;
        /* QoS 0 → no packet-id; payload starts here */
        int pay_len = remaining - ti;

        /*
         * HA birth message: when HA (re)starts MQTT integration it publishes
         * "online" to homeassistant/status.  We must re-send all discovery
         * configs so HA re-creates any entities it lost after restart.
         */
        if (strcmp(topic, "homeassistant/status") == 0 &&
            pay_len >= 6 &&
            memcmp(buf + ti, "online", 6) == 0)
        {
            printf("[MQTT] HA came online — republishing discovery\n");
            publish_all_discovery();
            mqtt_publish_state();
            mqtt_publish_telemetry();
            break;
        }

        dispatch_command(topic, (const char *)(buf + ti), pay_len);
        break;
    }

    case 9:  /* SUBACK  — ignore */
        break;
    case 13: /* PINGRESP — ignore */
        break;
    default:
        printf("[MQTT] unhandled packet type %d\n", (int)ptype);
        break;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Connect / reconnect
 * ═══════════════════════════════════════════════════════════════════════════ */

static int mqtt_connect(void)
{
    /* Resolve broker hostname */
    struct hostent *he = gethostbyname(g_cfg.broker_host);
    if (!he) {
        fprintf(stderr, "[MQTT] cannot resolve '%s'\n", g_cfg.broker_host);
        return -1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("[MQTT] socket"); return -1; }

    /* Disable Nagle's algorithm — reduces MQTT command latency by up to 200 ms */
    {
        int flag = 1;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_cfg.broker_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[MQTT] connect");
        close(g_sock); g_sock = -1;
        return -1;
    }

    /* Send CONNECT */
    {
        uint8_t pkt[512];
        int len = build_connect(pkt, sizeof(pkt));
        if (len < 0 || mqtt_send(pkt, len) != 0) {
            close(g_sock); g_sock = -1;
            return -1;
        }
    }

    /* Wait for CONNACK with a short timeout */
    {
        struct timeval tv = {MQTT_CONNACK_TIMEOUT_S, 0};
        setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int rc = process_one_packet();
        tv.tv_sec = 0;
        setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); /* clear */
        if (rc != 0) { close(g_sock); g_sock = -1; return -1; }
    }

    /* Publish availability = online (retained) */
    publish_availability("online");

    /* Subscribe to all command topics at once via '+' wildcard */
    {
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/+/set", g_cfg.node_id);
        uint8_t pkt[256];
        int len = build_subscribe(pkt, sizeof(pkt), sub_topic, 1);
        if (len > 0) mqtt_send(pkt, len);
        printf("[MQTT] Subscribed to %s\n", sub_topic);
    }

    /*
     * Subscribe to HA birth message: homeassistant/status.
     * When HA (re)starts its MQTT integration it publishes "online" here.
     * We must re-publish all discovery configs in response so that HA
     * picks up entities it may have lost after a restart.
     */
    {
        uint8_t pkt[256];
        int len = build_subscribe(pkt, sizeof(pkt), "homeassistant/status", 2);
        if (len > 0) mqtt_send(pkt, len);
        printf("[MQTT] Subscribed to homeassistant/status (HA birth message)\n");
    }

    /* Push all HA Discovery configs (retained) */
    publish_all_discovery();

    /* Publish current settings state (retained) */
    mqtt_publish_state();
    mqtt_publish_telemetry();

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Background thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *mqtt_thread_fn(void *arg)
{
    (void)arg;
    int reconnect_delay = MQTT_RECONNECT_MIN_S;
    time_t last_telemetry_publish = 0;
    time_t last_discovery_publish = 0;
    int discovery_refresh_interval_s = g_cfg.discovery_refresh_s;
    if (discovery_refresh_interval_s <= 0)
        discovery_refresh_interval_s = MQTT_DISCOVERY_REFRESH_S;

    while (g_running) {

        /* ── (Re)connect ───────────────────────────────────────────────── */
        if (mqtt_connect() != 0) {
            printf("[MQTT] Retrying in %ds...\n", reconnect_delay);
            for (int i = 0; i < reconnect_delay && g_running; i++) sleep(1);
            reconnect_delay = reconnect_delay * 2;
            if (reconnect_delay > MQTT_RECONNECT_MAX_S)
                reconnect_delay = MQTT_RECONNECT_MAX_S;
            continue;
        }
        reconnect_delay = MQTT_RECONNECT_MIN_S; /* reset on success */
        last_telemetry_publish = time(NULL);
        last_discovery_publish = time(NULL);

        /* ── Receive / keepalive loop ───────────────────────────────────── */
        time_t last_ping = time(NULL);

        while (g_running) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(g_sock, &rfds);
            struct timeval tv = {5, 0}; /* wake every 5 s to check PING */
            int r = select(g_sock + 1, &rfds, NULL, NULL, &tv);

            if (!g_running) break;

            if (r < 0) {
                if (errno == EINTR) continue;
                perror("[MQTT] select");
                break;
            }

            if (r > 0) {
                if (process_one_packet() != 0) {
                    printf("[MQTT] Connection lost\n");
                    break;
                }
            }

            /* PINGREQ keepalive */
            if (time(NULL) - last_ping >= MQTT_PING_INTERVAL_S) {
                uint8_t ping[2] = {0xC0, 0x00};
                if (mqtt_send(ping, 2) != 0) {
                    printf("[MQTT] PING failed — reconnecting\n");
                    break;
                }
                last_ping = time(NULL);
            }

            if (time(NULL) - last_telemetry_publish >= MQTT_TELEMETRY_INTERVAL_S) {
                mqtt_publish_telemetry();
                last_telemetry_publish = time(NULL);
            }

            if (discovery_refresh_interval_s > 0 &&
                time(NULL) - last_discovery_publish >= discovery_refresh_interval_s) {
                publish_all_discovery();
                last_discovery_publish = time(NULL);
            }
        }

        /* ── Clean disconnect ───────────────────────────────────────────── */
        if (g_sock >= 0) {
            if (!g_running)
                publish_availability("offline");
            uint8_t disc[2] = {0xE0, 0x00};
            mqtt_send(disc, 2);
            close(g_sock);
            g_sock = -1;
        }

        if (g_running) {
            printf("[MQTT] Reconnecting in %ds...\n", reconnect_delay);
            for (int i = 0; i < reconnect_delay && g_running; i++) sleep(1);
            reconnect_delay = reconnect_delay * 2;
            if (reconnect_delay > MQTT_RECONNECT_MAX_S)
                reconnect_delay = MQTT_RECONNECT_MAX_S;
        }
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

void mqtt_config_init(mqtt_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->broker_host, "127.0.0.1",      sizeof(cfg->broker_host) - 1);
    cfg->broker_port = 1883;
    strncpy(cfg->node_id,     "luckfox_camera", sizeof(cfg->node_id)     - 1);
    strncpy(cfg->device_name, "Luckfox Camera", sizeof(cfg->device_name) - 1);
    cfg->discovery_refresh_s = MQTT_DISCOVERY_REFRESH_S;
}

int mqtt_client_start(const mqtt_config_t *cfg)
{
    if (g_running) return 0;
    g_cfg     = *cfg;
    clock_gettime(CLOCK_MONOTONIC, &g_start_mono);
    g_cpu_sample.valid = 0;
    g_running = 1;
    int err = pthread_create(&g_thread, NULL, mqtt_thread_fn, NULL);
    if (err) {
        fprintf(stderr, "[MQTT] pthread_create failed: %d\n", err);
        g_running = 0;
        return -1;
    }
    return 0;
}

void mqtt_client_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}
