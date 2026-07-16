/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/rgb_underglow_state_changed.h>
#include <zmk/events/rgb_underglow_tick.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_PERIOD (ZMK_RGB_UNDERGLOW_HUE_MAX + 1)

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

static const struct device *led_strip;
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/*
 * The state mutex protects the runtime state and transaction bookkeeping. The transaction mutex
 * serializes persistent mutations with the transition into Studio preview mode, so a pending
 * debounced change cannot slip between the initial flush and the preview baseline snapshot.
 */
K_MUTEX_DEFINE(underglow_state_mutex);
K_MUTEX_DEFINE(underglow_transaction_mutex);

static struct rgb_underglow_state state;
static struct rgb_underglow_state transaction_baseline;
static bool transaction_active;
static bool transaction_dirty;

enum rgb_underglow_auto_off_reason {
    RGB_UNDERGLOW_AUTO_OFF_IDLE = BIT(0),
    RGB_UNDERGLOW_AUTO_OFF_USB = BIT(1),
};

static uint8_t auto_off_reasons;
static bool output_on;

#if IS_ENABLED(CONFIG_SETTINGS)
static struct rgb_underglow_state pending_save_state;
static struct k_work_delayable underglow_save_work;
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static int zmk_rgb_underglow_on_with_persist(bool persist_state);
static int zmk_rgb_underglow_off_with_persist(bool persist_state);

static struct rgb_underglow_state default_state(void) {
    return (struct rgb_underglow_state){
        .color =
            {
                .h = CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
                .s = CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
                .b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
            },
        .animation_speed = CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        .current_effect = CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        .animation_step = 0,
        .on = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START),
    };
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void sanitize_state(struct rgb_underglow_state *value) {
    value->color.h %= HUE_PERIOD;
    value->color.s = MIN(value->color.s, ZMK_RGB_UNDERGLOW_SAT_MAX);
    value->color.b = MIN(value->color.b, ZMK_RGB_UNDERGLOW_BRT_MAX);
    value->animation_speed =
        CLAMP(value->animation_speed, ZMK_RGB_UNDERGLOW_SPD_MIN, ZMK_RGB_UNDERGLOW_SPD_MAX);
    if (value->current_effect >= ZMK_RGB_UNDERGLOW_EFFECT_COUNT) {
        value->current_effect = ZMK_RGB_UNDERGLOW_EFFECT_SOLID;
    }
    value->animation_step = 0;
}
#endif

static struct zmk_rgb_underglow_config config_from_state(const struct rgb_underglow_state *value) {
    return (struct zmk_rgb_underglow_config){
        .color = value->color,
        .animation_speed = value->animation_speed,
        .effect = value->current_effect,
        .on = value->on,
    };
}

static void apply_config_to_state(struct rgb_underglow_state *target,
                                  const struct zmk_rgb_underglow_config *config) {
    target->color = config->color;
    target->animation_speed = config->animation_speed;
    target->current_effect = config->effect;
    target->animation_step = 0;
    target->on = config->on;
}

static bool configs_equal(const struct rgb_underglow_state *lhs,
                          const struct rgb_underglow_state *rhs) {
    return lhs->color.h == rhs->color.h && lhs->color.s == rhs->color.s &&
           lhs->color.b == rhs->color.b && lhs->animation_speed == rhs->animation_speed &&
           lhs->current_effect == rhs->current_effect && lhs->on == rhs->on;
}

bool zmk_rgb_underglow_validate_config_values(uint32_t hue, uint32_t saturation,
                                              uint32_t brightness, uint32_t effect,
                                              uint32_t speed) {
    return hue <= ZMK_RGB_UNDERGLOW_HUE_MAX && saturation <= ZMK_RGB_UNDERGLOW_SAT_MAX &&
           brightness <= ZMK_RGB_UNDERGLOW_BRT_MAX && speed >= ZMK_RGB_UNDERGLOW_SPD_MIN &&
           speed <= ZMK_RGB_UNDERGLOW_SPD_MAX && effect < ZMK_RGB_UNDERGLOW_EFFECT_COUNT;
}

static bool config_is_valid(const struct zmk_rgb_underglow_config *config) {
    return config && zmk_rgb_underglow_validate_config_values(config->color.h, config->color.s,
                                                              config->color.b, config->effect,
                                                              config->animation_speed);
}

static void raise_state_changed(const struct zmk_rgb_underglow_config *config) {
    raise_zmk_rgb_underglow_state_changed(
        (struct zmk_rgb_underglow_state_changed){.config = *config});
}

static void raise_dirty_changed(bool dirty) {
    raise_zmk_rgb_underglow_unsaved_changes_changed(
        (struct zmk_rgb_underglow_unsaved_changes_changed){.unsaved_changes = dirty});
}

static bool update_transaction_dirty_locked(void) {
    if (!transaction_active) {
        return false;
    }

    bool dirty = !configs_equal(&state, &transaction_baseline);
    if (dirty == transaction_dirty) {
        return false;
    }

    transaction_dirty = dirty;
    return true;
}

static bool update_output_state_locked(bool *target_on) {
    *target_on = state.on && auto_off_reasons == 0;
    if (*target_on == output_on) {
        return false;
    }

    output_on = *target_on;
    return true;
}

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b /
                ZMK_RGB_UNDERGLOW_BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / ZMK_RGB_UNDERGLOW_BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)ZMK_RGB_UNDERGLOW_BRT_MAX);
    float s = hsb.s / ((float)ZMK_RGB_UNDERGLOW_SAT_MAX);
    float f = hsb.h / ((float)HUE_PERIOD) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    return (struct led_rgb){.r = r * 255, .g = g * 255, .b = b * 255};
}

static void zmk_rgb_underglow_effect_solid(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;
    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step = (state.animation_step + state.animation_speed) % HUE_PERIOD;
}

static void zmk_rgb_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_PERIOD / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_PERIOD;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step = (state.animation_step + state.animation_speed * 2) % HUE_PERIOD;
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    if (!output_on) {
        k_mutex_unlock(&underglow_state_mutex);
        return;
    }

    switch (state.current_effect) {
    case ZMK_RGB_UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case ZMK_RGB_UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case ZMK_RGB_UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case ZMK_RGB_UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
    }

    struct rgb_underglow_state tick_state = state;
    k_mutex_unlock(&underglow_state_mutex);

    raise_zmk_rgb_underglow_tick_event(
        (struct zmk_rgb_underglow_tick_event){.pixels = pixels, .state = tick_state});

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to turn off the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

static int apply_power_state(bool on, bool persist_ext_power) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = on ? ext_power_enable_with_persist(ext_power, persist_ext_power)
                    : ext_power_disable_with_persist(ext_power, persist_ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to %s EXT_POWER: %d", on ? "enable" : "disable", rc);
        }
    }
#endif

    if (on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    } else {
        k_timer_stop(&underglow_tick);
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);
    }

    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void zmk_rgb_underglow_save_state_work(struct k_work *work) {
    struct rgb_underglow_state snapshot;

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    snapshot = pending_save_state;
    k_mutex_unlock(&underglow_state_mutex);
    snapshot.animation_step = 0;

    int err = settings_save_one("rgb/underglow/state", &snapshot, sizeof(snapshot));
    if (err < 0) {
        LOG_ERR("Failed to save RGB underglow state: %d", err);
        return;
    }
}

static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "state", &next) || next) {
        return -ENOENT;
    }

    if (len != sizeof(state)) {
        return -EINVAL;
    }

    struct rgb_underglow_state loaded_state;
    int rc = read_cb(cb_arg, &loaded_state, sizeof(loaded_state));
    if (rc < 0) {
        return rc;
    }
    sanitize_state(&loaded_state);

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    state = loaded_state;
    pending_save_state = loaded_state;
    transaction_baseline = loaded_state;
    transaction_active = false;
    transaction_dirty = false;
    bool target_on;
    bool output_changed = update_output_state_locked(&target_on);
    k_mutex_unlock(&underglow_state_mutex);
    int power_err = output_changed ? apply_power_state(target_on, false) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    return power_err;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);
#endif

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = default_state();

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        auto_off_reasons |= RGB_UNDERGLOW_AUTO_OFF_IDLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (!zmk_usb_is_powered()) {
        auto_off_reasons |= RGB_UNDERGLOW_AUTO_OFF_USB;
    }
#endif

    transaction_baseline = state;
    output_on = state.on && auto_off_reasons == 0;

#if IS_ENABLED(CONFIG_SETTINGS)
    pending_save_state = state;
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

    return apply_power_state(output_on, false);
}

static int save_state_locked(bool *dirty_changed, bool *dirty) {
#if IS_ENABLED(CONFIG_SETTINGS)
    bool schedule_save = false;
#endif

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    if (transaction_active) {
        *dirty_changed = update_transaction_dirty_locked();
        *dirty = transaction_dirty;
    } else {
#if IS_ENABLED(CONFIG_SETTINGS)
        pending_save_state = state;
        schedule_save = true;
#endif
    }
    k_mutex_unlock(&underglow_state_mutex);

#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = schedule_save ? k_work_reschedule(&underglow_save_work,
                                                K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE))
                            : 0;
#else
    int ret = 0;
#endif

    return MIN(ret, 0);
}

int zmk_rgb_underglow_save_state(void) {
    bool dirty_changed = false;
    bool dirty = false;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    int ret = save_state_locked(&dirty_changed, &dirty);
    k_mutex_unlock(&underglow_transaction_mutex);

    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }

    return ret;
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip) {
        return -ENODEV;
    }
    if (!on_off) {
        return -EINVAL;
    }

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    *on_off = state.on;
    k_mutex_unlock(&underglow_state_mutex);
    return 0;
}

int zmk_rgb_underglow_get_config(struct zmk_rgb_underglow_config *config) {
    if (!led_strip) {
        return -ENODEV;
    }
    if (!config) {
        return -EINVAL;
    }

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    *config = config_from_state(&state);
    k_mutex_unlock(&underglow_state_mutex);
    return 0;
}

static int set_on_with_persist(bool on, bool persist) {
    if (!led_strip) {
        return -ENODEV;
    }

    struct zmk_rgb_underglow_config config;
    bool dirty_changed = false;
    bool dirty = false;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    bool persist_ext_power = persist && !transaction_active;
    state.on = on;
    state.animation_step = 0;
    config = config_from_state(&state);
    bool target_on;
    bool output_changed = update_output_state_locked(&target_on);
    k_mutex_unlock(&underglow_state_mutex);
    int save_err = persist ? save_state_locked(&dirty_changed, &dirty) : 0;
    int power_err = output_changed ? apply_power_state(target_on, persist_ext_power) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }
    if (power_err < 0) {
        return power_err;
    }
    return save_err;
}

static int zmk_rgb_underglow_on_with_persist(bool persist_state) {
    return set_on_with_persist(true, persist_state);
}

static int zmk_rgb_underglow_off_with_persist(bool persist_state) {
    return set_on_with_persist(false, persist_state);
}

int zmk_rgb_underglow_on(void) { return zmk_rgb_underglow_on_with_persist(true); }

int zmk_rgb_underglow_off(void) { return zmk_rgb_underglow_off_with_persist(true); }

int zmk_rgb_underglow_calc_effect(int direction) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    int effect = (state.current_effect + ZMK_RGB_UNDERGLOW_EFFECT_COUNT + direction) %
                 ZMK_RGB_UNDERGLOW_EFFECT_COUNT;
    k_mutex_unlock(&underglow_state_mutex);
    return effect;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip) {
        return -ENODEV;
    }
    if (effect < 0 || effect >= ZMK_RGB_UNDERGLOW_EFFECT_COUNT) {
        return -EINVAL;
    }

    struct zmk_rgb_underglow_config config;
    bool dirty_changed = false;
    bool dirty = false;
    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    state.current_effect = effect;
    state.animation_step = 0;
    config = config_from_state(&state);
    k_mutex_unlock(&underglow_state_mutex);
    int err = save_state_locked(&dirty_changed, &dirty);
    k_mutex_unlock(&underglow_transaction_mutex);

    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }
    return err;
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    bool on;
    int err = zmk_rgb_underglow_get_state(&on);
    return err < 0 ? err : (on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on());
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > ZMK_RGB_UNDERGLOW_HUE_MAX || color.s > ZMK_RGB_UNDERGLOW_SAT_MAX ||
        color.b > ZMK_RGB_UNDERGLOW_BRT_MAX) {
        return -ENOTSUP;
    }

    struct zmk_rgb_underglow_config config;
    bool dirty_changed = false;
    bool dirty = false;
    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    state.color = color;
    config = config_from_state(&state);
    k_mutex_unlock(&underglow_state_mutex);
    int err = save_state_locked(&dirty_changed, &dirty);
    k_mutex_unlock(&underglow_transaction_mutex);

    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }
    return err;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    struct zmk_led_hsb color = state.color;
    k_mutex_unlock(&underglow_state_mutex);

    color.h += HUE_PERIOD + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_PERIOD;
    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    struct zmk_led_hsb color = state.color;
    k_mutex_unlock(&underglow_state_mutex);

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    color.s = CLAMP(s, ZMK_RGB_UNDERGLOW_SAT_MIN, ZMK_RGB_UNDERGLOW_SAT_MAX);
    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    struct zmk_led_hsb color = state.color;
    k_mutex_unlock(&underglow_state_mutex);

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, ZMK_RGB_UNDERGLOW_BRT_MIN, ZMK_RGB_UNDERGLOW_BRT_MAX);
    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip) {
        return -ENODEV;
    }
    return zmk_rgb_underglow_set_hsb(zmk_rgb_underglow_calc_hue(direction));
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip) {
        return -ENODEV;
    }
    return zmk_rgb_underglow_set_hsb(zmk_rgb_underglow_calc_sat(direction));
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip) {
        return -ENODEV;
    }
    return zmk_rgb_underglow_set_hsb(zmk_rgb_underglow_calc_brt(direction));
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip) {
        return -ENODEV;
    }

    struct zmk_rgb_underglow_config config;
    bool dirty_changed = false;
    bool dirty = false;
    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    int speed = state.animation_speed + direction;
    state.animation_speed = CLAMP(speed, ZMK_RGB_UNDERGLOW_SPD_MIN, ZMK_RGB_UNDERGLOW_SPD_MAX);
    config = config_from_state(&state);
    k_mutex_unlock(&underglow_state_mutex);
    int err = save_state_locked(&dirty_changed, &dirty);
    k_mutex_unlock(&underglow_transaction_mutex);

    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }
    return err;
}

int zmk_rgb_underglow_preview_config(const struct zmk_rgb_underglow_config *config) {
    if (!led_strip) {
        return -ENODEV;
    }
    if (!config_is_valid(config)) {
        return -EINVAL;
    }

    bool dirty_changed = false;
    bool dirty;
    bool target_on;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    if (!transaction_active) {
#if IS_ENABLED(CONFIG_SETTINGS)
        struct k_work_sync sync;
        k_work_flush_delayable(&underglow_save_work, &sync);
#endif

        k_mutex_lock(&underglow_state_mutex, K_FOREVER);
        transaction_baseline = state;
        transaction_active = true;
        transaction_dirty = false;
        k_mutex_unlock(&underglow_state_mutex);
    }

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    apply_config_to_state(&state, config);
    dirty_changed = update_transaction_dirty_locked();
    dirty = transaction_dirty;
    bool output_changed = update_output_state_locked(&target_on);
    k_mutex_unlock(&underglow_state_mutex);
    int power_err = output_changed ? apply_power_state(target_on, false) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    raise_state_changed(config);
    if (dirty_changed) {
        raise_dirty_changed(dirty);
    }
    return power_err;
}

bool zmk_rgb_underglow_has_unsaved_changes(void) {
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    bool dirty = transaction_active && transaction_dirty;
    k_mutex_unlock(&underglow_state_mutex);
    return dirty;
}

int zmk_rgb_underglow_save_preview(void) {
    bool dirty_changed = false;
    int err = 0;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    bool active = transaction_active;
    bool dirty = transaction_dirty;
#if IS_ENABLED(CONFIG_SETTINGS)
    struct rgb_underglow_state snapshot = state;
    snapshot.animation_step = 0;
#endif
    k_mutex_unlock(&underglow_state_mutex);

    if (active && dirty) {
#if IS_ENABLED(CONFIG_SETTINGS)
        err = settings_save_one("rgb/underglow/state", &snapshot, sizeof(snapshot));
#endif
    }

    if (err >= 0 && active) {
        k_mutex_lock(&underglow_state_mutex, K_FOREVER);
        transaction_baseline = state;
        transaction_active = false;
        dirty_changed = transaction_dirty;
        transaction_dirty = false;
        k_mutex_unlock(&underglow_state_mutex);
    }
    k_mutex_unlock(&underglow_transaction_mutex);

    if (dirty_changed) {
        raise_dirty_changed(false);
    }
    return err;
}

int zmk_rgb_underglow_discard_preview(void) {
    struct zmk_rgb_underglow_config config;
    bool active;
    bool dirty_changed = false;
    bool target_on = false;
    bool output_changed = false;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    active = transaction_active;
    if (active) {
        state = transaction_baseline;
        config = config_from_state(&state);
        transaction_active = false;
        dirty_changed = transaction_dirty;
        transaction_dirty = false;
        output_changed = update_output_state_locked(&target_on);
    }
    k_mutex_unlock(&underglow_state_mutex);
    int power_err = output_changed ? apply_power_state(target_on, false) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    if (!active) {
        return 0;
    }

    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(false);
    }
    return power_err;
}

int zmk_rgb_underglow_reset_settings(void) {
    struct rgb_underglow_state defaults = default_state();

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
#if IS_ENABLED(CONFIG_SETTINGS)
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&underglow_save_work, &sync);
    int err = settings_delete("rgb/underglow/state");
    if (err < 0) {
        k_mutex_unlock(&underglow_transaction_mutex);
        return err;
    }
#else
    int err = 0;
#endif

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    state = defaults;
    transaction_baseline = defaults;
#if IS_ENABLED(CONFIG_SETTINGS)
    pending_save_state = defaults;
#endif
    bool dirty_changed = transaction_dirty;
    transaction_active = false;
    transaction_dirty = false;
    bool target_on;
    bool output_changed = update_output_state_locked(&target_on);
    k_mutex_unlock(&underglow_state_mutex);
    int power_err = output_changed ? apply_power_state(target_on, false) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    struct zmk_rgb_underglow_config config = config_from_state(&defaults);
    raise_state_changed(&config);
    if (dirty_changed) {
        raise_dirty_changed(false);
    }
    return power_err < 0 ? power_err : err;
}

static int set_auto_off_reason(enum rgb_underglow_auto_off_reason reason, bool active) {
    bool target_on;

    k_mutex_lock(&underglow_transaction_mutex, K_FOREVER);
    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    if (active) {
        auto_off_reasons |= reason;
    } else {
        auto_off_reasons &= ~reason;
    }
    bool output_changed = update_output_state_locked(&target_on);
    k_mutex_unlock(&underglow_state_mutex);
    int err = output_changed ? apply_power_state(target_on, false) : 0;
    k_mutex_unlock(&underglow_transaction_mutex);

    return err;
}

#if IS_ENABLED(CONFIG_ZMK_TEST_BEHAVIORS)
int zmk_rgb_underglow_test_set_auto_off_idle(bool active) {
    return set_auto_off_reason(RGB_UNDERGLOW_AUTO_OFF_IDLE, active);
}

int zmk_rgb_underglow_test_set_auto_off_usb(bool active) {
    return set_auto_off_reason(RGB_UNDERGLOW_AUTO_OFF_USB, active);
}

int zmk_rgb_underglow_test_get_output_state(bool *on) {
    if (!on) {
        return -EINVAL;
    }

    k_mutex_lock(&underglow_state_mutex, K_FOREVER);
    *on = output_on;
    k_mutex_unlock(&underglow_state_mutex);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
static int rgb_underglow_event_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return set_auto_off_reason(RGB_UNDERGLOW_AUTO_OFF_IDLE,
                                   zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return set_auto_off_reason(RGB_UNDERGLOW_AUTO_OFF_USB, !zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
