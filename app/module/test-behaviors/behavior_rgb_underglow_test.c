/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_rgb_underglow_test

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>
#include <zmk/rgb_underglow.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC) || IS_ENABLED(CONFIG_ZMK_TEST_STUDIO_LIGHTING)
#include <zmk/studio/rpc.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

extern int zmk_rgb_underglow_test_set_auto_off_idle(bool active);
extern int zmk_rgb_underglow_test_set_auto_off_usb(bool active);
extern int zmk_rgb_underglow_test_get_output_state(bool *on);

#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
#define RGB_SETTINGS_KEY "rgb/underglow/state"

static struct {
    bool valid;
    struct rgb_underglow_state state;
    uint32_t write_count;
} settings_test_storage;

static ssize_t settings_test_read(void *cb_arg, void *data, size_t len) {
    const struct rgb_underglow_state *stored = cb_arg;
    size_t read_len = MIN(len, sizeof(*stored));

    memcpy(data, stored, read_len);
    return read_len;
}

static int settings_test_load(struct settings_store *store,
                              const struct settings_load_arg *load_arg) {
    ARG_UNUSED(store);

    if (!settings_test_storage.valid) {
        return 0;
    }

    return settings_call_set_handler(RGB_SETTINGS_KEY, sizeof(settings_test_storage.state),
                                     settings_test_read, &settings_test_storage.state, load_arg);
}

static int settings_test_save(struct settings_store *store, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(store);

    if (strcmp(name, RGB_SETTINGS_KEY) != 0) {
        return 0;
    }

    settings_test_storage.write_count++;
    if (!value || val_len == 0) {
        settings_test_storage.valid = false;
        return 0;
    }
    if (val_len != sizeof(settings_test_storage.state)) {
        return -EINVAL;
    }

    memcpy(&settings_test_storage.state, value, val_len);
    settings_test_storage.valid = true;
    return 0;
}

static const struct settings_store_itf settings_test_itf = {
    .csi_load = settings_test_load,
    .csi_save = settings_test_save,
};

static struct settings_store settings_test_store = {
    .cs_itf = &settings_test_itf,
};

int settings_backend_init(void) {
    settings_src_register(&settings_test_store);
    settings_dst_register(&settings_test_store);
    return 0;
}
#endif

static bool configs_equal(const struct zmk_rgb_underglow_config *lhs,
                          const struct zmk_rgb_underglow_config *rhs) {
    return lhs->color.h == rhs->color.h && lhs->color.s == rhs->color.s &&
           lhs->color.b == rhs->color.b && lhs->animation_speed == rhs->animation_speed &&
           lhs->effect == rhs->effect && lhs->on == rhs->on;
}

static void log_result(const char *name, bool passed) {
    LOG_INF("RGB transaction %s: %s", name, passed ? "PASS" : "FAIL");
}

static void test_preview_discard(void) {
    struct zmk_rgb_underglow_config baseline;
    struct zmk_rgb_underglow_config preview;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&baseline) == 0;
    preview = baseline;
    preview.color.h = baseline.color.h == ZMK_RGB_UNDERGLOW_HUE_MAX ? ZMK_RGB_UNDERGLOW_HUE_MAX - 1
                                                                    : ZMK_RGB_UNDERGLOW_HUE_MAX;
    preview.color.s = 42;
    preview.color.b = 37;
    preview.animation_speed = ZMK_RGB_UNDERGLOW_SPD_MAX;
    preview.effect = ZMK_RGB_UNDERGLOW_EFFECT_SWIRL;

    passed = passed && zmk_rgb_underglow_preview_config(&preview) == 0;
    passed = passed && zmk_rgb_underglow_has_unsaved_changes();
    passed =
        passed && zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&preview, &actual);
    passed = passed && zmk_rgb_underglow_discard_preview() == 0;
    passed = passed && !zmk_rgb_underglow_has_unsaved_changes();
    passed =
        passed && zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&baseline, &actual);

    log_result("discard", passed);
}

static void test_preview_save(void) {
    struct zmk_rgb_underglow_config preview;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&preview) == 0;
    preview.color.h = (preview.color.h + 1) % (ZMK_RGB_UNDERGLOW_HUE_MAX + 1);

    passed = passed && zmk_rgb_underglow_preview_config(&preview) == 0;
    passed = passed && zmk_rgb_underglow_save_preview() == 0;
    passed = passed && !zmk_rgb_underglow_has_unsaved_changes();
    passed = passed && zmk_rgb_underglow_discard_preview() == 0;
    passed =
        passed && zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&preview, &actual);

    log_result("save", passed);
}

static void test_physical_change_joins_preview(void) {
    struct zmk_rgb_underglow_config baseline;
    struct zmk_rgb_underglow_config preview;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&baseline) == 0;
    preview = baseline;
    preview.color.h = (preview.color.h + 1) % (ZMK_RGB_UNDERGLOW_HUE_MAX + 1);

    passed = passed && zmk_rgb_underglow_preview_config(&preview) == 0;
    passed = passed && zmk_rgb_underglow_change_brt(
                           preview.color.b == ZMK_RGB_UNDERGLOW_BRT_MAX ? -1 : 1) == 0;
    passed = passed && zmk_rgb_underglow_has_unsaved_changes();
    passed = passed && zmk_rgb_underglow_discard_preview() == 0;
    passed =
        passed && zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&baseline, &actual);

    log_result("physical-discard", passed);
}

static void test_invalid_preview(void) {
    struct zmk_rgb_underglow_config baseline;
    struct zmk_rgb_underglow_config invalid;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&baseline) == 0;
    invalid = baseline;
    invalid.color.h = ZMK_RGB_UNDERGLOW_HUE_MAX + 1;

    passed = passed && zmk_rgb_underglow_preview_config(&invalid) == -EINVAL;
    passed = passed && !zmk_rgb_underglow_has_unsaved_changes();
    passed =
        passed && zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&baseline, &actual);

    log_result("validation", passed);
}

static void test_wide_value_validation(void) {
    bool passed =
        zmk_rgb_underglow_validate_config_values(
            ZMK_RGB_UNDERGLOW_HUE_MAX, ZMK_RGB_UNDERGLOW_SAT_MAX, ZMK_RGB_UNDERGLOW_BRT_MAX,
            ZMK_RGB_UNDERGLOW_EFFECT_SWIRL, ZMK_RGB_UNDERGLOW_SPD_MAX) &&
        !zmk_rgb_underglow_validate_config_values(
            UINT16_MAX + 1U, ZMK_RGB_UNDERGLOW_SAT_MAX, ZMK_RGB_UNDERGLOW_BRT_MAX,
            ZMK_RGB_UNDERGLOW_EFFECT_SOLID, ZMK_RGB_UNDERGLOW_SPD_MIN) &&
        !zmk_rgb_underglow_validate_config_values(
            ZMK_RGB_UNDERGLOW_HUE_MIN, UINT8_MAX + 1U, ZMK_RGB_UNDERGLOW_BRT_MAX,
            ZMK_RGB_UNDERGLOW_EFFECT_SOLID, ZMK_RGB_UNDERGLOW_SPD_MIN) &&
        !zmk_rgb_underglow_validate_config_values(
            ZMK_RGB_UNDERGLOW_HUE_MIN, ZMK_RGB_UNDERGLOW_SAT_MAX, UINT8_MAX + 1U,
            ZMK_RGB_UNDERGLOW_EFFECT_SOLID, ZMK_RGB_UNDERGLOW_SPD_MIN) &&
        !zmk_rgb_underglow_validate_config_values(
            ZMK_RGB_UNDERGLOW_HUE_MIN, ZMK_RGB_UNDERGLOW_SAT_MAX, ZMK_RGB_UNDERGLOW_BRT_MAX,
            UINT8_MAX + 1U, ZMK_RGB_UNDERGLOW_SPD_MIN) &&
        !zmk_rgb_underglow_validate_config_values(
            ZMK_RGB_UNDERGLOW_HUE_MIN, ZMK_RGB_UNDERGLOW_SAT_MAX, ZMK_RGB_UNDERGLOW_BRT_MAX,
            ZMK_RGB_UNDERGLOW_EFFECT_SOLID, UINT8_MAX + 1U);

    log_result("wide-validation", passed);
}

static void test_auto_off_not_persisted(void) {
    struct zmk_rgb_underglow_config preview;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_on() == 0;
    passed = zmk_rgb_underglow_get_config(&preview) == 0 && passed;
    preview.color.h = (preview.color.h + 1) % (ZMK_RGB_UNDERGLOW_HUE_MAX + 1);
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_idle(true) == 0 && passed;
    passed = zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&preview, &actual) &&
             actual.on && passed;
    passed = zmk_rgb_underglow_save_preview() == 0 && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_idle(false) == 0 && passed;
    passed = zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&preview, &actual) &&
             actual.on && !zmk_rgb_underglow_has_unsaved_changes() && passed;

    /* Always release the synthetic runtime reason so later tests cannot inherit it. */
    zmk_rgb_underglow_test_set_auto_off_idle(false);
    log_result("auto-off-save", passed);
}

#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
static void test_persistence_writes(void) {
    struct zmk_rgb_underglow_config physical;
    struct zmk_rgb_underglow_config preview;
    struct zmk_rgb_underglow_config saved;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&physical) == 0;
    settings_test_storage.write_count = 0;

    physical.color.h = (physical.color.h + 1) % (ZMK_RGB_UNDERGLOW_HUE_MAX + 1);
    passed = zmk_rgb_underglow_set_hsb(physical.color) == 0 && passed;
    passed = settings_test_storage.write_count == 0 && passed;

    preview = physical;
    preview.color.s = preview.color.s == ZMK_RGB_UNDERGLOW_SAT_MAX ? ZMK_RGB_UNDERGLOW_SAT_MAX - 1
                                                                   : ZMK_RGB_UNDERGLOW_SAT_MAX;
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    /* Entering preview flushes the one pending debounced physical change. */
    passed = settings_test_storage.write_count == 1 && passed;

    preview.color.b = preview.color.b == ZMK_RGB_UNDERGLOW_BRT_MAX ? ZMK_RGB_UNDERGLOW_BRT_MAX - 1
                                                                   : ZMK_RGB_UNDERGLOW_BRT_MAX;
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    passed = settings_test_storage.write_count == 1 && passed;

    preview.animation_speed = preview.animation_speed == ZMK_RGB_UNDERGLOW_SPD_MAX
                                  ? ZMK_RGB_UNDERGLOW_SPD_MIN
                                  : preview.animation_speed + 1;
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    passed = settings_test_storage.write_count == 1 && passed;

    passed = zmk_rgb_underglow_save_preview() == 0 && passed;
    passed = settings_test_storage.write_count == 2 && passed;
    saved = preview;

    /* Saving a clean transaction must not rewrite the same state. */
    passed = zmk_rgb_underglow_preview_config(&saved) == 0 && passed;
    passed = !zmk_rgb_underglow_has_unsaved_changes() && passed;
    passed = zmk_rgb_underglow_save_preview() == 0 && passed;
    passed = settings_test_storage.write_count == 2 && passed;

    preview = saved;
    preview.color.h = (preview.color.h + 1) % (ZMK_RGB_UNDERGLOW_HUE_MAX + 1);
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    passed = zmk_rgb_underglow_discard_preview() == 0 && passed;
    passed = settings_test_storage.write_count == 2 && passed;

    /* Simulate reboot-time settings loading while an unsaved preview is active. */
    passed = zmk_rgb_underglow_preview_config(&preview) == 0 && passed;
    passed = settings_load_subtree("rgb/underglow") == 0 && passed;
    passed = zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&saved, &actual) && passed;
    passed = !zmk_rgb_underglow_has_unsaved_changes() && settings_test_storage.write_count == 2 &&
             passed;

    log_result("persistence", passed);
}
#endif

static void test_effective_output(void) {
    bool output = false;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_test_set_auto_off_idle(false) == 0;
    passed = zmk_rgb_underglow_test_set_auto_off_usb(false) == 0 && passed;
    passed = zmk_rgb_underglow_on() == 0 && passed;
    passed = zmk_rgb_underglow_test_get_output_state(&output) == 0 && output && passed;

    passed = zmk_rgb_underglow_test_set_auto_off_idle(true) == 0 && passed;
    passed = zmk_rgb_underglow_test_get_output_state(&output) == 0 && !output && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_usb(true) == 0 && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_idle(false) == 0 && passed;
    passed = zmk_rgb_underglow_test_get_output_state(&output) == 0 && !output && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_usb(false) == 0 && passed;
    passed = zmk_rgb_underglow_test_get_output_state(&output) == 0 && output && passed;

    passed = zmk_rgb_underglow_test_set_auto_off_idle(true) == 0 && passed;
    passed = zmk_rgb_underglow_off() == 0 && passed;
    passed = zmk_rgb_underglow_test_set_auto_off_idle(false) == 0 && passed;
    passed = zmk_rgb_underglow_test_get_output_state(&output) == 0 && !output && passed;
    passed = zmk_rgb_underglow_get_config(&actual) == 0 && !actual.on && passed;

    /* Leave a predictable desired and effective state for any later checks. */
    zmk_rgb_underglow_test_set_auto_off_idle(false);
    zmk_rgb_underglow_test_set_auto_off_usb(false);
    zmk_rgb_underglow_on();
    log_result("effective-output", passed);
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC) || IS_ENABLED(CONFIG_ZMK_TEST_STUDIO_LIGHTING)
static bool rpc_response_is_error(const zmk_studio_Response *response) {
    return response->which_type == zmk_studio_Response_request_response_tag &&
           response->type.request_response.which_subsystem == zmk_studio_RequestResponse_meta_tag &&
           response->type.request_response.subsystem.meta.which_response_type ==
               zmk_meta_Response_simple_error_tag;
}

static bool invoke_preview_rpc(uint32_t hue, uint32_t saturation, uint32_t brightness,
                               uint32_t effect, uint32_t speed) {
    zmk_studio_Request request = zmk_studio_Request_init_zero;
    request.which_subsystem = zmk_studio_Request_lighting_tag;
    request.subsystem.lighting.which_request_type = zmk_lighting_Request_set_preview_state_tag;

    zmk_lighting_TargetState *target = &request.subsystem.lighting.request_type.set_preview_state;
    target->target = zmk_lighting_LightingTarget_LIGHTING_TARGET_UNDERGLOW;
    target->has_state = true;
    target->state.on = true;
    target->state.hue = hue;
    target->state.saturation = saturation;
    target->state.brightness = brightness;
    target->state.effect = effect;
    target->state.speed = speed;

    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem_handler, handler) {
        if (handler->subsystem_choice == zmk_studio_Request_lighting_tag &&
            handler->request_choice == zmk_lighting_Request_set_preview_state_tag) {
            zmk_studio_Response response = handler->func(&request);
            return rpc_response_is_error(&response);
        }
    }

    return false;
}

static void test_rpc_validation(void) {
    struct zmk_rgb_underglow_config baseline;
    struct zmk_rgb_underglow_config actual;

    bool passed = zmk_rgb_underglow_get_config(&baseline) == 0;
    const uint32_t invalid_values[][5] = {
        {ZMK_RGB_UNDERGLOW_HUE_MAX + 1U, baseline.color.s, baseline.color.b, baseline.effect,
         baseline.animation_speed},
        {UINT16_MAX + 1U, baseline.color.s, baseline.color.b, baseline.effect,
         baseline.animation_speed},
        {baseline.color.h, ZMK_RGB_UNDERGLOW_SAT_MAX + 1U, baseline.color.b, baseline.effect,
         baseline.animation_speed},
        {baseline.color.h, UINT8_MAX + 1U, baseline.color.b, baseline.effect,
         baseline.animation_speed},
        {baseline.color.h, baseline.color.s, ZMK_RGB_UNDERGLOW_BRT_MAX + 1U, baseline.effect,
         baseline.animation_speed},
        {baseline.color.h, baseline.color.s, UINT8_MAX + 1U, baseline.effect,
         baseline.animation_speed},
        {baseline.color.h, baseline.color.s, baseline.color.b, ZMK_RGB_UNDERGLOW_EFFECT_COUNT,
         baseline.animation_speed},
        {baseline.color.h, baseline.color.s, baseline.color.b, UINT8_MAX + 1U,
         baseline.animation_speed},
        {baseline.color.h, baseline.color.s, baseline.color.b, baseline.effect,
         ZMK_RGB_UNDERGLOW_SPD_MIN - 1U},
        {baseline.color.h, baseline.color.s, baseline.color.b, baseline.effect,
         ZMK_RGB_UNDERGLOW_SPD_MAX + 1U},
        {baseline.color.h, baseline.color.s, baseline.color.b, baseline.effect, UINT8_MAX + 1U},
    };

    for (size_t i = 0; i < ARRAY_SIZE(invalid_values); i++) {
        passed =
            invoke_preview_rpc(invalid_values[i][0], invalid_values[i][1], invalid_values[i][2],
                               invalid_values[i][3], invalid_values[i][4]) &&
            passed;
        passed = zmk_rgb_underglow_get_config(&actual) == 0 && configs_equal(&baseline, &actual) &&
                 !zmk_rgb_underglow_has_unsaved_changes() && passed;
    }

    log_result("rpc-validation", passed);
}
#endif

#define EXTENDED_TEST_STACK_SIZE 4096

K_THREAD_STACK_DEFINE(extended_test_stack, EXTENDED_TEST_STACK_SIZE);
static struct k_thread extended_test_thread;

static void run_extended_tests(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);

    /* These tests flush system work, so they cannot run from the kscan mock's system work item. */
    test_auto_off_not_persisted();
#if IS_ENABLED(CONFIG_SETTINGS_CUSTOM)
    test_persistence_writes();
#endif
    test_effective_output();
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC) || IS_ENABLED(CONFIG_ZMK_TEST_STUDIO_LIGHTING)
    test_rpc_validation();
#endif
}

static void start_extended_tests(void) {
    k_thread_create(&extended_test_thread, extended_test_stack,
                    K_THREAD_STACK_SIZEOF(extended_test_stack), run_extended_tests, NULL, NULL,
                    NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
}

static int on_rgb_underglow_test_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case 0:
        test_preview_discard();
        break;
    case 1:
        test_preview_save();
        break;
    case 2:
        test_physical_change_joins_preview();
        break;
    case 3:
        test_invalid_preview();
        test_wide_value_validation();
        start_extended_tests();
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int on_rgb_underglow_test_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api behavior_rgb_underglow_test_driver_api = {
    .binding_pressed = on_rgb_underglow_test_pressed,
    .binding_released = on_rgb_underglow_test_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_rgb_underglow_test_driver_api);
