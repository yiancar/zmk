/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_led_strip_mock

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

struct led_strip_mock_config {
    size_t chain_length;
};

static int led_strip_mock_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                     size_t num_pixels) {
    return 0;
}

static size_t led_strip_mock_length(const struct device *dev) {
    const struct led_strip_mock_config *config = dev->config;
    return config->chain_length;
}

static const struct led_strip_driver_api led_strip_mock_api = {
    .update_rgb = led_strip_mock_update_rgb,
    .length = led_strip_mock_length,
};

#define LED_STRIP_MOCK_DEFINE(inst)                                                                \
    static const struct led_strip_mock_config led_strip_mock_config_##inst = {                     \
        .chain_length = DT_INST_PROP(inst, chain_length),                                          \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &led_strip_mock_config_##inst, POST_KERNEL,      \
                          CONFIG_LED_STRIP_INIT_PRIORITY, &led_strip_mock_api);

DT_INST_FOREACH_STATUS_OKAY(LED_STRIP_MOCK_DEFINE)
