/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <pb_encode.h>

#include <zmk/events/rgb_underglow_state_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_RPC_SUBSYSTEM(lighting)

#define LIGHTING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(lighting, type, __VA_ARGS__)
#define LIGHTING_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(lighting, type, __VA_ARGS__)

struct lighting_effect_descriptor {
    uint32_t id;
    const char *display_name;
};

static const struct lighting_effect_descriptor underglow_effects[] = {
    {.id = ZMK_RGB_UNDERGLOW_EFFECT_SOLID, .display_name = "Solid"},
    {.id = ZMK_RGB_UNDERGLOW_EFFECT_BREATHE, .display_name = "Breathe"},
    {.id = ZMK_RGB_UNDERGLOW_EFFECT_SPECTRUM, .display_name = "Spectrum"},
    {.id = ZMK_RGB_UNDERGLOW_EFFECT_SWIRL, .display_name = "Swirl"},
};

static bool is_underglow_target(zmk_lighting_LightingTarget target) {
    return target == zmk_lighting_LightingTarget_LIGHTING_TARGET_UNDERGLOW;
}

static bool underglow_state_is_valid(const zmk_lighting_State *state) {
    return zmk_rgb_underglow_validate_config_values(state->hue, state->saturation,
                                                    state->brightness, state->effect, state->speed);
}

static zmk_lighting_TargetState
target_state_from_config(const struct zmk_rgb_underglow_config *config) {
    zmk_lighting_TargetState target_state = zmk_lighting_TargetState_init_zero;
    target_state.target = zmk_lighting_LightingTarget_LIGHTING_TARGET_UNDERGLOW;
    target_state.has_state = true;
    target_state.state.on = config->on;
    target_state.state.hue = config->color.h;
    target_state.state.saturation = config->color.s;
    target_state.state.brightness = config->color.b;
    target_state.state.effect = config->effect;
    target_state.state.speed = config->animation_speed;
    return target_state;
}

static struct zmk_rgb_underglow_config
config_from_target_state(const zmk_lighting_TargetState *target_state) {
    return (struct zmk_rgb_underglow_config){
        .color =
            {
                .h = (uint16_t)target_state->state.hue,
                .s = (uint8_t)target_state->state.saturation,
                .b = (uint8_t)target_state->state.brightness,
            },
        .animation_speed = (uint8_t)target_state->state.speed,
        .effect = (uint8_t)target_state->state.effect,
        .on = target_state->state.on,
    };
}

static bool encode_effect_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct lighting_effect_descriptor *effect = *arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, (const pb_byte_t *)effect->display_name,
                            strlen(effect->display_name));
}

static bool encode_effects(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (size_t i = 0; i < ARRAY_SIZE(underglow_effects); i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_lighting_Effect effect = zmk_lighting_Effect_init_zero;
        effect.id = underglow_effects[i].id;
        effect.display_name.funcs.encode = encode_effect_name;
        effect.display_name.arg = (void *)&underglow_effects[i];

        if (!pb_encode_submessage(stream, &zmk_lighting_Effect_msg, &effect)) {
            return false;
        }
    }

    return true;
}

static zmk_studio_Response get_capabilities(const zmk_studio_Request *req) {
    const zmk_lighting_TargetRequest *request =
        &req->subsystem.lighting.request_type.get_capabilities;
    if (!is_underglow_target(request->target)) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_lighting_Capabilities capabilities = zmk_lighting_Capabilities_init_zero;
    capabilities.target = zmk_lighting_LightingTarget_LIGHTING_TARGET_UNDERGLOW;
    capabilities.supports_on_off = true;
    capabilities.has_hue = true;
    capabilities.hue = (zmk_lighting_ScalarRange){
        .min = ZMK_RGB_UNDERGLOW_HUE_MIN,
        .max = ZMK_RGB_UNDERGLOW_HUE_MAX,
        .step = 1,
    };
    capabilities.saturation = (zmk_lighting_ScalarRange){
        .min = ZMK_RGB_UNDERGLOW_SAT_MIN,
        .max = ZMK_RGB_UNDERGLOW_SAT_MAX,
        .step = 1,
    };
    capabilities.has_saturation = true;
    capabilities.brightness = (zmk_lighting_ScalarRange){
        .min = ZMK_RGB_UNDERGLOW_BRT_MIN,
        .max = ZMK_RGB_UNDERGLOW_BRT_MAX,
        .step = 1,
    };
    capabilities.has_brightness = true;
    capabilities.speed = (zmk_lighting_ScalarRange){
        .min = ZMK_RGB_UNDERGLOW_SPD_MIN,
        .max = ZMK_RGB_UNDERGLOW_SPD_MAX,
        .step = 1,
    };
    capabilities.has_speed = true;
    capabilities.effects.funcs.encode = encode_effects;

    return LIGHTING_RESPONSE(get_capabilities, capabilities);
}

static zmk_studio_Response get_state(const zmk_studio_Request *req) {
    const zmk_lighting_TargetRequest *request = &req->subsystem.lighting.request_type.get_state;
    if (!is_underglow_target(request->target)) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    struct zmk_rgb_underglow_config config;
    if (zmk_rgb_underglow_get_config(&config) < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return LIGHTING_RESPONSE(get_state, target_state_from_config(&config));
}

static zmk_studio_Response set_preview_state(const zmk_studio_Request *req) {
    const zmk_lighting_TargetState *request =
        &req->subsystem.lighting.request_type.set_preview_state;
    if (!is_underglow_target(request->target) || !request->has_state ||
        !underglow_state_is_valid(&request->state)) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    struct zmk_rgb_underglow_config config = config_from_target_state(request);
    if (zmk_rgb_underglow_preview_config(&config) < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return LIGHTING_RESPONSE(set_preview_state, target_state_from_config(&config));
}

static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    return LIGHTING_RESPONSE(check_unsaved_changes, zmk_rgb_underglow_has_unsaved_changes());
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    return zmk_rgb_underglow_save_preview() < 0 ? ZMK_RPC_SIMPLE_ERR(GENERIC)
                                                : LIGHTING_RESPONSE(save_changes, true);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    if (zmk_rgb_underglow_discard_preview() < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    struct zmk_rgb_underglow_config config;
    if (zmk_rgb_underglow_get_config(&config) < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return LIGHTING_RESPONSE(discard_changes, target_state_from_config(&config));
}

ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_capabilities, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, get_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, set_preview_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(lighting, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(lighting, zmk_rgb_underglow_reset_settings);

static int lighting_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *notification) {
    const struct zmk_rgb_underglow_state_changed *state_event =
        as_zmk_rgb_underglow_state_changed(eh);
    if (state_event) {
        *notification =
            LIGHTING_NOTIFICATION(state_changed, target_state_from_config(&state_event->config));
        return 0;
    }

    const struct zmk_rgb_underglow_unsaved_changes_changed *dirty_event =
        as_zmk_rgb_underglow_unsaved_changes_changed(eh);
    if (dirty_event) {
        *notification =
            LIGHTING_NOTIFICATION(unsaved_changes_status_changed, dirty_event->unsaved_changes);
        return 0;
    }

    return -ENOTSUP;
}

ZMK_RPC_EVENT_MAPPER(lighting, lighting_event_mapper, zmk_rgb_underglow_state_changed,
                     zmk_rgb_underglow_unsaved_changes_changed);
