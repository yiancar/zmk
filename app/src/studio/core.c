/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/studio/core.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_FACTORY_UNLOCK_ONCE)
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);
#endif

ZMK_EVENT_IMPL(zmk_studio_core_lock_state_changed);

static enum zmk_studio_core_lock_state state = IS_ENABLED(CONFIG_ZMK_STUDIO_LOCKING)
                                                   ? ZMK_STUDIO_CORE_LOCK_STATE_LOCKED
                                                   : ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;

enum zmk_studio_core_lock_state zmk_studio_core_get_lock_state(void) { return state; }

static void set_state(enum zmk_studio_core_lock_state new_state) {
    if (state == new_state) {
        return;
    }

    state = new_state;

    raise_zmk_studio_core_lock_state_changed(
        (struct zmk_studio_core_lock_state_changed){.state = state});
}

#if CONFIG_ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC > 0

static void core_idle_lock_timeout_cb(struct k_work *work) { zmk_studio_core_lock(); }

K_WORK_DELAYABLE_DEFINE(core_idle_lock_timeout, core_idle_lock_timeout_cb);

void zmk_studio_core_reschedule_lock_timeout() {
    k_work_reschedule(&core_idle_lock_timeout, K_SECONDS(CONFIG_ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC));
}

#else

void zmk_studio_core_reschedule_lock_timeout() {}

#endif

void zmk_studio_core_unlock() {
    set_state(ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED);

    zmk_studio_core_reschedule_lock_timeout();
}

void zmk_studio_core_lock() { set_state(ZMK_STUDIO_CORE_LOCK_STATE_LOCKED); }

#if IS_ENABLED(CONFIG_ZMK_STUDIO_FACTORY_UNLOCK_ONCE)

#define FACTORY_UNLOCK_SETTINGS_SUBTREE "studio"
#define FACTORY_UNLOCK_SETTINGS_NAME "factory_unlock_consumed"
#define FACTORY_UNLOCK_SETTINGS_KEY                                                               \
    FACTORY_UNLOCK_SETTINGS_SUBTREE "/" FACTORY_UNLOCK_SETTINGS_NAME
#define FACTORY_UNLOCK_CONSUMED_VALUE 0xA5

static bool factory_settings_loaded;
static bool factory_marker_consumed;
static bool factory_marker_invalid;
static bool factory_unlock_attempted;

static K_MUTEX_DEFINE(factory_unlock_mutex);

static int factory_unlock_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                       void *cb_arg) {
    if (!settings_name_steq(name, FACTORY_UNLOCK_SETTINGS_NAME, NULL)) {
        return 0;
    }

    uint8_t value;
    if (len != sizeof(value)) {
        factory_marker_invalid = true;
        LOG_ERR("Invalid factory unlock marker size");
        return -EINVAL;
    }

    ssize_t read_len = read_cb(cb_arg, &value, sizeof(value));
    if (read_len != sizeof(value) || value != FACTORY_UNLOCK_CONSUMED_VALUE) {
        factory_marker_invalid = true;
        LOG_ERR("Invalid factory unlock marker");
        return -EINVAL;
    }

    factory_marker_consumed = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(studio_factory_unlock, FACTORY_UNLOCK_SETTINGS_SUBTREE, NULL,
                               factory_unlock_settings_set, NULL, NULL);

static void factory_unlock_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&factory_unlock_mutex, K_FOREVER);

    if (!factory_settings_loaded || factory_marker_consumed || factory_marker_invalid ||
        factory_unlock_attempted || !zmk_usb_is_hid_ready() ||
        zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB) {
        goto unlock;
    }

    factory_unlock_attempted = true;
    const uint8_t consumed = FACTORY_UNLOCK_CONSUMED_VALUE;
    int err = settings_save_one(FACTORY_UNLOCK_SETTINGS_KEY, &consumed, sizeof(consumed));
    if (err) {
        LOG_ERR("Failed to persist factory unlock marker: %d", err);
        goto unlock;
    }

    factory_marker_consumed = true;

    if (!zmk_usb_is_hid_ready() ||
        zmk_endpoint_get_selected().transport != ZMK_TRANSPORT_USB) {
        LOG_WRN("USB disconnected while consuming factory unlock marker");
        goto unlock;
    }

    LOG_INF("Factory unlock marker consumed; unlocking Studio for this USB session");
    zmk_studio_core_unlock();

unlock:
    k_mutex_unlock(&factory_unlock_mutex);
}

K_WORK_DEFINE(factory_unlock_work, factory_unlock_work_handler);

static void schedule_factory_unlock_if_usb(void) {
    if (zmk_usb_is_hid_ready() &&
        zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        k_work_submit(&factory_unlock_work);
    }
}

void zmk_studio_core_factory_settings_loaded(int settings_load_result) {
    if (settings_load_result) {
        LOG_ERR("Settings failed to load; factory unlock remains disabled: %d", settings_load_result);
        return;
    }

    k_mutex_lock(&factory_unlock_mutex, K_FOREVER);
    if (factory_marker_invalid) {
        k_mutex_unlock(&factory_unlock_mutex);
        LOG_ERR("Invalid factory unlock marker; factory unlock remains disabled");
        return;
    }
    factory_settings_loaded = true;
    k_mutex_unlock(&factory_unlock_mutex);
    schedule_factory_unlock_if_usb();
}

void zmk_studio_core_factory_cancel_unlock() {
    k_mutex_lock(&factory_unlock_mutex, K_FOREVER);
    factory_unlock_attempted = true;
    k_mutex_unlock(&factory_unlock_mutex);
}

static int factory_unlock_endpoint_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *event = as_zmk_endpoint_changed(eh);
    if (event && event->endpoint.transport == ZMK_TRANSPORT_USB) {
        schedule_factory_unlock_if_usb();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(studio_factory_unlock, factory_unlock_endpoint_listener);
ZMK_SUBSCRIPTION(studio_factory_unlock, zmk_endpoint_changed);

#endif
