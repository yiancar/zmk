/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include <zmk/event_manager.h>
#include <zmk/rgb_underglow.h>

struct zmk_rgb_underglow_state_changed {
    struct zmk_rgb_underglow_config config;
};

struct zmk_rgb_underglow_unsaved_changes_changed {
    bool unsaved_changes;
};

ZMK_EVENT_DECLARE(zmk_rgb_underglow_state_changed);
ZMK_EVENT_DECLARE(zmk_rgb_underglow_unsaved_changes_changed);
