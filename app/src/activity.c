/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>

#include <zmk/pm.h>

#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_TEST_ACTIVITY)
static bool test_usb_power_present;
#endif

bool is_usb_power_present(void) {
#if IS_ENABLED(CONFIG_ZMK_TEST_ACTIVITY)
    return test_usb_power_present;
#elif IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
}

static atomic_t activity_state = ATOMIC_INIT(ZMK_ACTIVITY_ACTIVE);

static uint32_t activity_last_uptime;
static K_MUTEX_DEFINE(activity_mutex);

#define MAX_IDLE_MS CONFIG_ZMK_IDLE_TIMEOUT

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
#define MAX_SLEEP_MS CONFIG_ZMK_IDLE_SLEEP_TIMEOUT
#endif

static int raise_event(enum zmk_activity_state state) {
    return raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = state});
}

static int set_state_locked(enum zmk_activity_state state) {
    enum zmk_activity_state previous_state = atomic_get(&activity_state);
    if (previous_state == state) {
        return 0;
    }

    atomic_set(&activity_state, state);
    int err = raise_event(state);
    if (err >= 0) {
        return err;
    }

    atomic_set(&activity_state, previous_state);
    int rollback_err = raise_event(previous_state);
    if (rollback_err < 0) {
        LOG_ERR("Failed to restore activity state after listener error: %d", rollback_err);
    }
    return err;
}

enum zmk_activity_state zmk_activity_get_state(void) { return atomic_get(&activity_state); }

int zmk_activity_note_activity(void) {
    k_mutex_lock(&activity_mutex, K_FOREVER);
    uint32_t previous_uptime = activity_last_uptime;
    activity_last_uptime = k_uptime_get();
    int err = set_state_locked(ZMK_ACTIVITY_ACTIVE);
    if (err < 0) {
        activity_last_uptime = previous_uptime;
    }
    k_mutex_unlock(&activity_mutex);
    return err;
}

static int activity_event_listener(const zmk_event_t *eh) { return zmk_activity_note_activity(); }

void activity_work_handler(struct k_work *work) {
    k_mutex_lock(&activity_mutex, K_FOREVER);
    int32_t current = k_uptime_get();
    int32_t inactive_time = current - activity_last_uptime;
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (inactive_time > MAX_SLEEP_MS && !is_usb_power_present()) {
        // Put devices in suspend power mode before sleeping
        if (set_state_locked(ZMK_ACTIVITY_SLEEP) < 0) {
            k_mutex_unlock(&activity_mutex);
            return;
        }

        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            k_mutex_unlock(&activity_mutex);
            return;
        }

        sys_poweroff();
    } else
#endif /* IS_ENABLED(CONFIG_ZMK_SLEEP) */
        if (inactive_time > MAX_IDLE_MS) {
            set_state_locked(ZMK_ACTIVITY_IDLE);
        }
    k_mutex_unlock(&activity_mutex);
}

#if IS_ENABLED(CONFIG_ZMK_TEST_ACTIVITY)
void zmk_activity_test_set_usb_power_present(bool powered) {
    k_mutex_lock(&activity_mutex, K_FOREVER);
    test_usb_power_present = powered;
    k_mutex_unlock(&activity_mutex);
}

void zmk_activity_test_set_inactive_time(int32_t inactive_ms) {
    k_mutex_lock(&activity_mutex, K_FOREVER);
    activity_last_uptime = k_uptime_get() - inactive_ms;
    k_mutex_unlock(&activity_mutex);
}

void zmk_activity_test_run_work(void) { activity_work_handler(NULL); }
#endif

K_WORK_DEFINE(activity_work, activity_work_handler);

void activity_expiry_function(struct k_timer *_timer) { k_work_submit(&activity_work); }

K_TIMER_DEFINE(activity_timer, activity_expiry_function, NULL);

static int activity_init(void) {
    activity_last_uptime = k_uptime_get();

    k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);
ZMK_SUBSCRIPTION(activity, zmk_sensor_event);

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static void note_activity_work_cb(struct k_work *_work) { zmk_activity_note_activity(); }

K_WORK_DEFINE(note_activity_work, note_activity_work_cb);

static void activity_input_listener(struct input_event *ev, void *user_data) {
    k_work_submit(&note_activity_work);
}

INPUT_CALLBACK_DEFINE(NULL, activity_input_listener, NULL);

#endif

SYS_INIT(activity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
