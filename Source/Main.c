#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define PROP_NAME  "debug.tracing.screen_brightness"   // used if present (override)
#define STATE_PROP "debug.tracing.screen_state"
#define LOG_TAG    "YamadaOBright"

// Framework brightness source on A17 (logged on EVERY change, 0..8191 scale):
//   DpcExtImpl[display=0]: Animating brightness: hdrBrightness target=A->B,sdrBrightness target=A->B,...
// (LocalDisplayDeviceExtImpl.setDisplayBrightness is only logged early after boot, so it is not used.)
#define LOGCAT_CMD  "logcat -b system -b main -v brief -T 1 -e 'Animating brightness' 2>/dev/null"
#define LOGCAT_INIT "logcat -b system -b main -d -v brief -e 'Animating brightness' 2>/dev/null"

static const char* BL_DIRS[] = {
    "/sys/class/leds/lcd-backlight",
    "/sys/class/backlight/panel0-backlight",
    "/sys/class/leds/lcd-backlight-1",
    NULL
};
#define DEFAULT_HW_MAX   2047
#define REF_SCALE        4095
#define FLOOR_OFFSET_REF 100
// AOD brightness is derived from the current (auto) brightness level, so it follows
// ambient light: aod = level * pct / 100, clamped to [min, max] (values on the 4095 scale).
// Live tuning (root):  setprop persist.yamada.aod.percent 25
//                      setprop persist.yamada.aod.min 80
//                      setprop persist.yamada.aod.max 400
#define AOD_PCT_DEF      25
#define AOD_MIN_DEF_REF  80
#define AOD_MAX_DEF_REF  400

static int g_evfd = -1;

static int read_int_from_file(const char* path, int def) {
    FILE* f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}
static int file_exists(const char* p) { return access(p, F_OK) == 0; }

static float get_float_prop(const char* name, float def) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(name, value) > 0) return atof(value);
    return def;
}
static int get_screen_state(void) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(STATE_PROP, value) > 0) return atoi(value);
    return 2;
}

static int get_int_prop(const char* name, int def) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(name, value) > 0) return atoi(value);
    return def;
}

static int aod_brightness(int level, int hw_min, int hw_max) {
    int pct  = get_int_prop("persist.yamada.aod.percent", AOD_PCT_DEF);
    int amin = get_int_prop("persist.yamada.aod.min", (AOD_MIN_DEF_REF * hw_max) / REF_SCALE);
    int amax = get_int_prop("persist.yamada.aod.max", (AOD_MAX_DEF_REF * hw_max) / REF_SCALE);
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    if (amin < 1) amin = 1;
    if (amax > hw_max) amax = hw_max;
    if (amax < amin) amax = amin;
    int v = (int)(((long)level * pct) / 100);
    (void)hw_min;
    if (v < amin) v = amin;
    if (v > amax) v = amax;
    return v;
}

static int open_backlight(const char* dir) {
    char path[160];
    snprintf(path, sizeof(path), "%s/brightness", dir);
    return open(path, O_WRONLY);
}
static void write_backlight_fd(int fd, int v) {
    if (fd < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", v);
    pwrite(fd, buf, len, 0);
}

static int calculate_brightness(float p, int hw_min, int hw_max, int in_min, int in_max) {
    if (in_max <= in_min) return hw_min;
    if (p < in_min) p = in_min;
    if (p > in_max) p = in_max;
    float n = (p - in_min) / (float)(in_max - in_min);
    float curved = powf(n, 2.2f);
    return hw_min + (int)roundf(curved * (float)(hw_max - hw_min));
}

// Wakes the main loop whenever any system property changes (screen_state etc.)
static void* prop_watcher(void* arg) {
    (void)arg;
    uint32_t serial = 0;
    struct timespec no_wait = {0, 0};
    __system_property_wait(NULL, 0, &serial, &no_wait);
    for (;;) {
        uint32_t old = serial;
        __system_property_wait(NULL, old, &serial, NULL);
        if (serial != old) {
            uint64_t one = 1;
            if (write(g_evfd, &one, sizeof(one)) < 0) { /* ignore */ }
        }
    }
    return NULL;
}

// --- logcat reader (raw read(), never stdio reads, so poll() is reliable) ---
static FILE* g_lc = NULL;
static char g_buf[4096];
static size_t g_len = 0;

static int logcat_open(void) {
    g_lc = popen(LOGCAT_CMD, "r");
    g_len = 0;
    if (!g_lc) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "popen logcat failed: %d", errno); return -1; }
    int fd = fileno(g_lc);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "logcat reader started fd=%d", fd);
    return fd;
}
static void logcat_close(void) {
    if (g_lc) { pclose(g_lc); g_lc = NULL; }
    g_len = 0;
}

// Returns true if a new level was parsed into *level
static bool parse_line(const char* line, float* level) {
    const char* p = strstr(line, "sdrBrightness target=");
    if (!p) return false;
    p = strstr(p, "->");
    if (!p) return false;
    p += 2;
    char* end = NULL;
    float v = strtof(p, &end);
    if (end == p || v <= 0.0f) return false;   // ignore -1 / 0 (screen off)
    *level = v;
    return true;
}

// One-shot: last known level from the existing log buffer
static float logcat_initial_level(void) {
    float level = 0.0f;
    FILE* f = popen(LOGCAT_INIT, "r");
    if (!f) return level;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        float v;
        if (parse_line(line, &v)) level = v;
    }
    pclose(f);
    return level;
}

static bool logcat_pump(int fd, float* level) {
    bool got = false;
    ssize_t n = read(fd, g_buf + g_len, sizeof(g_buf) - 1 - g_len);
    if (n <= 0) return false;
    g_len += (size_t)n;
    g_buf[g_len] = 0;
    char* start = g_buf;
    char* nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = 0;
        if (parse_line(start, level)) got = true;
        start = nl + 1;
    }
    size_t rest = g_len - (size_t)(start - g_buf);
    memmove(g_buf, start, rest);
    g_len = rest;
    if (g_len >= sizeof(g_buf) - 1) g_len = 0;   // overlong line, drop
    return got;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting Yamada OBright (ruby, DpcExtImpl logcat source)");

    const char* dir = BL_DIRS[0];
    for (int i = 0; BL_DIRS[i]; i++) {
        char p[160];
        snprintf(p, sizeof(p), "%s/brightness", BL_DIRS[i]);
        if (file_exists(p)) { dir = BL_DIRS[i]; break; }
    }
    char p[160];
    snprintf(p, sizeof(p), "%s/max_hw_brightness", dir);
    int hw_max = file_exists(p) ? read_int_from_file(p, 0) : 0;
    if (hw_max <= 0) {
        snprintf(p, sizeof(p), "%s/max_brightness", dir);
        hw_max = read_int_from_file(p, DEFAULT_HW_MAX);
    }
    snprintf(p, sizeof(p), "%s/min_brightness", dir);
    int hw_min = read_int_from_file(p, 1);

    int input_max = (int)get_float_prop("sys.oplus.multibrightness", 8191.0f);
    int input_min = (int)get_float_prop("sys.oplus.multibrightness.min", 222.0f);

    hw_min += (FLOOR_OFFSET_REF * hw_max) / REF_SCALE;
    if (hw_min >= hw_max) hw_min = hw_max / 20 + 1;

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "dir=%s hw_min=%d hw_max=%d in=[%d..%d]", dir, hw_min, hw_max, input_min, input_max);

    int fd = open_backlight(dir);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "open backlight fd=%d errno=%d", fd, fd < 0 ? errno : 0);

    g_evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_t th;
    pthread_create(&th, NULL, prop_watcher, NULL);

    int lcfd = logcat_open();
    long last_lc_try = now_ms();

    float log_level = logcat_initial_level();      // from logcat
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "initial level from logcat=%.1f", log_level);
    float cur_prop = 0.0f;       // effective input level
    int prev_state = get_screen_state();
    int prev_bright = hw_min;
    int last_written = -1;
    int wake_ticks = 0;

    {
        float pp = get_float_prop(PROP_NAME, 0.0f);
        cur_prop = pp > 0 ? pp : log_level;
        if (cur_prop > 0) prev_bright = calculate_brightness(cur_prop, hw_min, hw_max, input_min, input_max);
        if (prev_bright < hw_min) prev_bright = hw_min;
        if (prev_bright > hw_max) prev_bright = hw_max;
    }
    last_written = (prev_state == 1 || prev_state == 0) ? 0 : prev_bright;
    write_backlight_fd(fd, last_written);

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds].fd = g_evfd; fds[nfds].events = POLLIN; nfds++;
        if (lcfd >= 0) { fds[nfds].fd = lcfd; fds[nfds].events = POLLIN; nfds++; }

        int timeout = wake_ticks > 0 ? 50 : (lcfd < 0 ? 3000 : -1);
        int r = poll(fds, nfds, timeout);

        bool props_changed = false, level_changed = false;
        if (r > 0) {
            if (fds[0].revents & POLLIN) {
                uint64_t v; if (read(g_evfd, &v, sizeof(v)) < 0) { /* ignore */ }
                props_changed = true;
            }
            if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                float lv;
                if (logcat_pump(lcfd, &lv)) { log_level = lv; level_changed = true; }
                else if (fds[1].revents & (POLLHUP | POLLERR)) {
                    logcat_close(); lcfd = -1; last_lc_try = now_ms();
                    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "logcat reader died, will retry");
                }
            }
        }
        if (lcfd < 0 && now_ms() - last_lc_try >= 3000) {
            lcfd = logcat_open();
            last_lc_try = now_ms();
        }
        if (fd < 0) fd = open_backlight(dir);

        bool changed = props_changed || level_changed;
        float new_prop = cur_prop;
        int cur_state = prev_state;
        int cur_bright = prev_bright;

        if (changed) {
            float pp = get_float_prop(PROP_NAME, 0.0f);
            new_prop = pp > 0 ? pp : log_level;
            cur_state = get_screen_state();
            if (new_prop > 0)
                cur_bright = calculate_brightness(new_prop, hw_min, hw_max, input_min, input_max);
            if (cur_bright < hw_min) cur_bright = hw_min;
            if (cur_bright > hw_max) cur_bright = hw_max;
        }

        if (cur_state != prev_state) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "state %d -> %d", prev_state, cur_state);
            wake_ticks = (cur_state == 2 || cur_state == 3 || cur_state == 4) ? 15 : 0;
        }

        int val;
        if (cur_state == 1 || cur_state == 0) val = 0;
        else if (cur_state == 3 || cur_state == 4) val = aod_brightness(cur_bright, hw_min, hw_max);
        else val = cur_bright;
        if (val > hw_max) val = hw_max;

        if (val != last_written) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "write %d (state=%d level=%.0f)", val, cur_state, new_prop);
            write_backlight_fd(fd, val);
            last_written = val;
        } else if (wake_ticks > 0 && cur_state >= 2 && cur_state <= 4 && !changed) {
            wake_ticks--;
            int wobble = (val > hw_min) ? val - 1 : val + 1;
            write_backlight_fd(fd, wobble);
            write_backlight_fd(fd, val);
        }

        prev_bright = cur_bright;
        prev_state = cur_state;
        cur_prop = new_prop;
    }
    return 0;
}
