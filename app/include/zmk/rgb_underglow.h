/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZMK_RGB_UNDERGLOW_HUE_MIN 0
#define ZMK_RGB_UNDERGLOW_HUE_MAX 359
#define ZMK_RGB_UNDERGLOW_SAT_MIN 0
#define ZMK_RGB_UNDERGLOW_SAT_MAX 100
#define ZMK_RGB_UNDERGLOW_BRT_MIN 0
#define ZMK_RGB_UNDERGLOW_BRT_MAX 100
#define ZMK_RGB_UNDERGLOW_SPD_MIN 1
#define ZMK_RGB_UNDERGLOW_SPD_MAX 5

enum zmk_rgb_underglow_effect {
    ZMK_RGB_UNDERGLOW_EFFECT_SOLID,
    ZMK_RGB_UNDERGLOW_EFFECT_BREATHE,
    ZMK_RGB_UNDERGLOW_EFFECT_SPECTRUM,
    ZMK_RGB_UNDERGLOW_EFFECT_SWIRL,
    ZMK_RGB_UNDERGLOW_EFFECT_COUNT,
};

struct zmk_led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

struct zmk_rgb_underglow_config {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t effect;
    bool on;
};

int zmk_rgb_underglow_toggle(void);
int zmk_rgb_underglow_get_state(bool *state);
int zmk_rgb_underglow_on(void);
int zmk_rgb_underglow_off(void);
int zmk_rgb_underglow_cycle_effect(int direction);
int zmk_rgb_underglow_calc_effect(int direction);
int zmk_rgb_underglow_select_effect(int effect);
struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction);
int zmk_rgb_underglow_change_hue(int direction);
int zmk_rgb_underglow_change_sat(int direction);
int zmk_rgb_underglow_change_brt(int direction);
int zmk_rgb_underglow_change_spd(int direction);
int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color);

bool zmk_rgb_underglow_validate_config_values(uint32_t hue, uint32_t saturation,
                                              uint32_t brightness, uint32_t effect, uint32_t speed);
int zmk_rgb_underglow_get_config(struct zmk_rgb_underglow_config *config);
int zmk_rgb_underglow_preview_config(const struct zmk_rgb_underglow_config *config);
bool zmk_rgb_underglow_has_unsaved_changes(void);
int zmk_rgb_underglow_save_preview(void);
int zmk_rgb_underglow_discard_preview(void);
int zmk_rgb_underglow_reset_settings(void);
