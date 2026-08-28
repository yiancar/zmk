/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };

enum zmk_activity_state zmk_activity_get_state(void);
int zmk_activity_note_activity(void);

#if defined(CONFIG_ZMK_TEST_ACTIVITY)
void zmk_activity_test_set_usb_power_present(bool powered);
void zmk_activity_test_set_inactive_time(int32_t inactive_ms);
void zmk_activity_test_run_work(void);
#endif
