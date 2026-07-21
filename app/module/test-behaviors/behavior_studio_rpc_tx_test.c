/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_studio_rpc_tx_test

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/studio/rpc.h>

#include "../../src/studio/msg_framing.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TEST_REQUEST_ID FRAMING_SOF
#define TEST_DRAIN_CHUNK_SIZE 3
#define TEST_RESPONSE_BUFFER_SIZE 256

static enum studio_framing_state response_framing_state;
static uint8_t response_buffer[TEST_RESPONSE_BUFFER_SIZE];
static size_t response_len;
static size_t raw_response_len;
static bool saw_escaped_byte;

static bool decode_behavior_id(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    size_t *behavior_count = *arg;
    uint32_t behavior_id;

    ARG_UNUSED(field);

    if (!pb_decode_varint(stream, &behavior_id)) {
        return false;
    }

    (*behavior_count)++;
    return true;
}

static void validate_response(void) {
    size_t behavior_count = 0;
    zmk_studio_Response response = zmk_studio_Response_init_zero;
    response.type.request_response.subsystem.behaviors.response_type.list_all_behaviors.behaviors
        .funcs.decode = decode_behavior_id;
    response.type.request_response.subsystem.behaviors.response_type.list_all_behaviors.behaviors
        .arg = &behavior_count;

    pb_istream_t stream = pb_istream_from_buffer(response_buffer, response_len);
    bool passed = pb_decode(&stream, &zmk_studio_Response_msg, &response);
    passed = passed && response.which_type == zmk_studio_Response_request_response_tag;
    passed = passed && response.type.request_response.request_id == TEST_REQUEST_ID;
    passed = passed && response.type.request_response.which_subsystem ==
                           zmk_studio_RequestResponse_behaviors_tag;
    passed = passed && response.type.request_response.subsystem.behaviors.which_response_type ==
                           zmk_behaviors_Response_list_all_behaviors_tag;
    passed = passed && behavior_count > 0;
    passed = passed && raw_response_len > CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE;
    passed = passed && saw_escaped_byte;

    LOG_INF("Studio RPC TX streaming: %s", passed ? "PASS" : "FAIL");
}

static void capture_response_byte(uint8_t byte) {
    raw_response_len++;
    if (response_framing_state == FRAMING_STATE_AWAITING_DATA && byte == FRAMING_ESC) {
        saw_escaped_byte = true;
    }

    if (studio_framing_process_byte(&response_framing_state, byte)) {
        if (response_len < ARRAY_SIZE(response_buffer)) {
            response_buffer[response_len++] = byte;
        }
    } else if (response_framing_state == FRAMING_STATE_EOF) {
        validate_response();
    }
}

static void drain_tx_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(drain_tx_work, drain_tx_work_handler);

static void drain_tx_work_handler(struct k_work *work) {
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
    uint8_t *buf;
    uint32_t len = ring_buf_get_claim(tx_buf, &buf, TEST_DRAIN_CHUNK_SIZE);

    for (uint32_t i = 0; i < len; i++) {
        capture_response_byte(buf[i]);
    }

    ring_buf_get_finish(tx_buf, len);
    if (len > 0) {
        zmk_rpc_tx_notify_space_available();
    }

    if (ring_buf_size_get(tx_buf) > 0) {
        k_work_reschedule(&drain_tx_work, K_MSEC(1));
    }
}

static void test_tx_notify(struct ring_buf *tx_buf, size_t added, bool message_done,
                           void *user_data) {
    ARG_UNUSED(tx_buf);
    ARG_UNUSED(user_data);

    if (added > 0 || message_done) {
        k_work_reschedule(&drain_tx_work, K_NO_WAIT);
    }
}

ZMK_RPC_TRANSPORT(test, ZMK_TRANSPORT_NONE, NULL, NULL, NULL, test_tx_notify);

static bool write_request_byte(struct ring_buf *rx_buf, uint8_t byte) {
    if (byte == FRAMING_SOF || byte == FRAMING_ESC || byte == FRAMING_EOF) {
        uint8_t escape = FRAMING_ESC;
        if (ring_buf_put(rx_buf, &escape, 1) != 1) {
            return false;
        }
    }

    return ring_buf_put(rx_buf, &byte, 1) == 1;
}

static int send_test_request(void) {
    zmk_studio_Request request = zmk_studio_Request_init_zero;
    request.request_id = TEST_REQUEST_ID;
    request.which_subsystem = zmk_studio_Request_behaviors_tag;
    request.subsystem.behaviors.which_request_type =
        zmk_behaviors_Request_list_all_behaviors_tag;
    request.subsystem.behaviors.request_type.list_all_behaviors = true;

    uint8_t request_buffer[32];
    pb_ostream_t stream = pb_ostream_from_buffer(request_buffer, sizeof(request_buffer));
    if (!pb_encode(&stream, &zmk_studio_Request_msg, &request)) {
        return -EINVAL;
    }

    struct ring_buf *rx_buf = zmk_rpc_get_rx_buf();
    uint8_t framing_byte = FRAMING_SOF;
    if (ring_buf_put(rx_buf, &framing_byte, 1) != 1) {
        return -ENOSPC;
    }

    for (size_t i = 0; i < stream.bytes_written; i++) {
        if (!write_request_byte(rx_buf, request_buffer[i])) {
            return -ENOSPC;
        }
    }

    framing_byte = FRAMING_EOF;
    if (ring_buf_put(rx_buf, &framing_byte, 1) != 1) {
        return -ENOSPC;
    }

    zmk_rpc_rx_notify();
    return 0;
}

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    int err = send_test_request();
    if (err < 0) {
        LOG_ERR("Failed to send Studio RPC streaming test request: %d", err);
    }
    return err;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return 0;
}

static const struct behavior_driver_api behavior_studio_rpc_tx_test_driver_api = {
    .binding_pressed = on_binding_pressed,
    .binding_released = on_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_studio_rpc_tx_test_driver_api);
