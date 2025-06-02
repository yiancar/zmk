/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>

#include <zmk/event_manager.h>
#include <zmk/rgb_underglow.h>

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

struct zmk_rgb_underglow_tick_event {
    struct led_rgb *pixels;
    struct rgb_underglow_state state;
};

ZMK_EVENT_DECLARE(zmk_rgb_underglow_tick_event);