/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_rgb_underglow_test

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/rgb_underglow.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

extern int zmk_rgb_underglow_test_set_auto_off_idle(bool active);

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
        test_auto_off_not_persisted();
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
