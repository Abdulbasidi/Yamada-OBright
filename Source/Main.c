#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define PROP_NAME  "debug.tracing.screen_brightness"
#define STATE_PROP "debug.tracing.screen_state"
#define LOG_TAG    "YamadaOBright"

// ---- ruby (MT6877, Redmi Note 12 Pro 5G) ----
// MTK exposes the backlight as a LED class device. Candidates are tried in order.
static const char* BL_DIRS[] = {
    "/sys/class/leds/lcd-backlight",
    "/sys/class/backlight/panel0-backlight",
    "/sys/class/leds/lcd-backlight-1",
    NULL
};
#define DEFAULT_HW_MAX   2047   // MTK default; overridden by sysfs if readable
#define REF_SCALE        4095   // scale the original constants were tuned for (100 floor, 400 AOD)
#define FLOOR_OFFSET_REF 100
#define AOD_MIN_REF      400

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

int main(void) {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting Yamada OBright (ruby)");

    // Pick backlight dir
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

    // Floor/AOD scaled from the 4095-range values to the real hw range
    hw_min += (FLOOR_OFFSET_REF * hw_max) / REF_SCALE;
    if (hw_min >= hw_max) hw_min = hw_max / 20 + 1;
    int aod_min = (AOD_MIN_REF * hw_max) / REF_SCALE;

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "dir=%s hw_min=%d hw_max=%d in=[%d..%d] aod_min=%d", dir, hw_min, hw_max, input_min, input_max, aod_min);

    int fd = open_backlight(dir);
    if (fd < 0) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "open backlight failed, will retry");

    uint32_t serial = 0;
    struct timespec no_wait = {0, 0};
    __system_property_wait(NULL, 0, &serial, &no_wait);

    float cur_prop = get_float_prop(PROP_NAME, 0.0f);
    int prev_state = get_screen_state();
    int prev_bright = (cur_prop == 0.0f) ? hw_min
                    : calculate_brightness(cur_prop, hw_min, hw_max, input_min, input_max);
    if (prev_bright < hw_min) prev_bright = hw_min;
    if (prev_bright > hw_max) prev_bright = hw_max;

    int last_written = -1;
    int wake_ticks = 0;

    if (prev_state == 1 || prev_state == 0) last_written = 0;
    else last_written = prev_bright;
    write_backlight_fd(fd, last_written);

    for (;;) {
        uint32_t old = serial;
        if (wake_ticks > 0) {
            struct timespec t = {0, 50000000};
            __system_property_wait(NULL, old, &serial, &t);
        } else {
            __system_property_wait(NULL, old, &serial, NULL);
        }

        // retry open if it failed at start (e.g. perms not ready yet)
        if (fd < 0) fd = open_backlight(dir);

        bool changed = (serial != old);
        float new_prop = cur_prop;
        int cur_state = prev_state;
        int cur_bright = prev_bright;

        if (changed) {
            new_prop = get_float_prop(PROP_NAME, cur_prop);
            cur_state = get_screen_state();
            if (new_prop != 0.0f)
                cur_bright = calculate_brightness(new_prop, hw_min, hw_max, input_min, input_max);
            if (cur_bright < hw_min) cur_bright = hw_min;
            if (cur_bright > hw_max) cur_bright = hw_max;
        }

        if (cur_state != prev_state)
            wake_ticks = (cur_state == 2 || cur_state == 3 || cur_state == 4) ? 15 : 0;

        int val;
        if (cur_state == 1 || cur_state == 0) val = 0;
        else if (cur_state == 3 || cur_state == 4) val = (cur_bright < aod_min) ? aod_min : cur_bright;
        else val = cur_bright;
        if (val > hw_max) val = hw_max;

        if (val != last_written) {
            write_backlight_fd(fd, val);
            last_written = val;
        } else if (wake_ticks > 0 && (cur_state >= 2 && cur_state <= 4) && !changed) {
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
