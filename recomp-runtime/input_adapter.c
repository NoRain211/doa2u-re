/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_adapter.h"
#include "stop_report.h"
#include "kernel_abi.h"

#include <stdio.h>
#include <string.h>

enum {
    XINIT_DEVICES_ADDRESS = 0x003b87b3u,
    XINIT_DEVICES_THUNK_ADDRESS = 0x003b990eu,
    XGET_DEVICES_ADDRESS = 0x003b9913u,
    XGET_DEVICE_CHANGES_ADDRESS = 0x003b9935u,
    XINPUT_OPEN_ADDRESS = 0x003b9e12u,
    XINPUT_CLOSE_ADDRESS = 0x003b9e68u,
    XINPUT_GET_CAPABILITIES_ADDRESS = 0x003b9e74u,
    XINPUT_GET_STATE_ADDRESS = 0x003ba04cu,
    XINPUT_SET_STATE_ADDRESS = 0x003ba0bfu,
    XAPI_GAMEPAD_DEVICE_TYPE = 0x003b845cu,
    XINPUT_CAPABILITIES_SIZE = 25u,
    XINPUT_CAPABILITIES_RUMBLE_OFFSET = 0x15u,
    XINPUT_CAPABILITIES_RUMBLE_SIZE = 4u,
    XINPUT_STATE_SIZE = 22u,
    XINPUT_FEEDBACK_LEFT_MOTOR_OFFSET = 0x42u,
    XINPUT_FEEDBACK_RIGHT_MOTOR_OFFSET = 0x44u,
    ERROR_SUCCESS = 0u,
    ERROR_INVALID_PARAMETER = 0x57u,
    ERROR_DEVICE_NOT_CONNECTED = 0x48fu,
};

static RecompInputModel input_model;
static RecompInputFeedbackSink feedback_sink;
static RecompInputSampleSource input_source;

static uint32_t stack_argument(uint32_t entry_esp, uint32_t index)
{
    return *recomp_memory_u32(entry_esp + 4u + index * 4u);
}

static bool gamepad_type(uint32_t address)
{
    return address == XAPI_GAMEPAD_DEVICE_TYPE;
}

static void finish(uint32_t entry_esp, uint32_t argument_count, uint32_t result)
{
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry_esp + 4u + argument_count * 4u;
}

/* Port 0 stays connected for the keyboard; ports 1-3 follow the source. */
static void refresh_connections(void)
{
    RecompInputGamepad ignored;

    for (uint32_t port = 1u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        recomp_input_set_connected(
            &input_model,
            port,
            input_source != NULL && input_source(port, &ignored));
    }
}

void recomp_input_adapter_reset(void)
{
    /* A neutral port-0 pad is the bring-up policy even without a host source. */
    recomp_input_reset(&input_model, 1u);
    feedback_sink = NULL;
    input_source = NULL;
}

void recomp_input_adapter_set_source(RecompInputSampleSource source)
{
    input_source = source;
}

void recomp_input_adapter_set_feedback_sink(RecompInputFeedbackSink sink)
{
    feedback_sink = sink;
}

const RecompInputModel *recomp_input_adapter_model(void)
{
    return &input_model;
}

static void xinit_devices_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    /* Native XInput owns device allocation; Xbox USB preallocation is unused.
       Retain the host sample source installed by the runner. */
    recomp_input_reset(&input_model, 1u);
    fprintf(stderr, "recomp input: XInitDevices using native input model\n");
    finish(entry_esp, 2u, 0u);
}

static void xget_devices_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t type = stack_argument(entry_esp, 0u);
    uint32_t mask = 0u;

    if (gamepad_type(type)) {
        refresh_connections();
        mask = recomp_input_get_devices(&input_model);
    }
    finish(entry_esp, 1u, mask);
}

static void xget_device_changes_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t type = stack_argument(entry_esp, 0u);
    uint32_t insertions_address = stack_argument(entry_esp, 1u);
    uint32_t removals_address = stack_argument(entry_esp, 2u);
    uint32_t insertions = 0u;
    uint32_t removals = 0u;
    bool changed = false;

    if (gamepad_type(type) && insertions_address != 0u &&
        removals_address != 0u) {
        refresh_connections();
        changed = recomp_input_get_device_changes(
            &input_model, &insertions, &removals);
        *recomp_memory_u32(insertions_address) = insertions;
        *recomp_memory_u32(removals_address) = removals;
    }
    finish(entry_esp, 3u, changed ? 1u : 0u);
}

static void xinput_open_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t type = stack_argument(entry_esp, 0u);
    uint32_t port = stack_argument(entry_esp, 1u);
    uint32_t slot = stack_argument(entry_esp, 2u);
    refresh_connections();
    uint32_t handle = gamepad_type(type) && slot == 0u
        ? recomp_input_open(&input_model, port)
        : 0u;

    if (handle != 0u) {
        fprintf(stderr, "recomp input: opened gamepad port=%u\n", (unsigned)port);
    }
    finish(entry_esp, 4u, handle);
}

static void xinput_close_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t handle = stack_argument(entry_esp, 0u);
    uint32_t port;

    recomp_input_close(&input_model, handle);
    /* A closed port no longer receives feedback, so stop its motors. */
    if (feedback_sink != NULL && recomp_input_handle_port(handle, &port)) {
        feedback_sink(port, 0u, 0u);
    }
    finish(entry_esp, 1u, ERROR_SUCCESS);
}

static void xinput_get_capabilities_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t handle = stack_argument(entry_esp, 0u);
    uint32_t output = stack_argument(entry_esp, 1u);
    uint32_t packet;
    RecompInputGamepad gamepad;
    uint32_t result = ERROR_DEVICE_NOT_CONNECTED;

    if (output == 0u) {
        result = ERROR_INVALID_PARAMETER;
    } else if (recomp_input_get_state(
            &input_model, handle, &packet, &gamepad)) {
        recomp_guest_memset(output, 0, XINPUT_CAPABILITIES_SIZE);
        *(uint8_t *)(void *)recomp_memory_i8(output) = 1u;
        /* Two report-header bytes precede the packed gamepad capabilities. */
        recomp_guest_memset(output + 3u, 0xff, 18u);
        *recomp_memory_u16(output + 3u) = 0x00ffu;
        /* Report both motors as a real pad does. Controller Settings turns
           vibration off for a pad whose rumble capability reads zero. */
        recomp_guest_memset(
            output + XINPUT_CAPABILITIES_RUMBLE_OFFSET,
            0xff,
            XINPUT_CAPABILITIES_RUMBLE_SIZE);
        result = ERROR_SUCCESS;
    }
    finish(entry_esp, 2u, result);
}

static void write_gamepad(uint32_t output, const RecompInputGamepad *gamepad)
{
    *recomp_memory_u16(output) = gamepad->buttons;
    for (uint32_t i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i) {
        *(uint8_t *)(void *)recomp_memory_i8(output + 2u + i) =
            gamepad->analog_buttons[i];
    }
    *recomp_memory_u16(output + 10u) = (uint16_t)gamepad->thumb_lx;
    *recomp_memory_u16(output + 12u) = (uint16_t)gamepad->thumb_ly;
    *recomp_memory_u16(output + 14u) = (uint16_t)gamepad->thumb_rx;
    *recomp_memory_u16(output + 16u) = (uint16_t)gamepad->thumb_ry;
}

static void xinput_get_state_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t handle = stack_argument(entry_esp, 0u);
    uint32_t output = stack_argument(entry_esp, 1u);
    uint32_t packet;
    uint32_t port;
    RecompInputGamepad gamepad;
    uint32_t result = ERROR_DEVICE_NOT_CONNECTED;

    if (output == 0u) {
        result = ERROR_INVALID_PARAMETER;
    } else {
        if (input_source != NULL && recomp_input_handle_port(handle, &port) &&
            input_model.ports[port].open) {
            RecompInputGamepad sampled = {0};

            if (input_source(port, &sampled)) {
                (void)recomp_input_set_gamepad(
                    &input_model, handle, &sampled);
            } else if (port != 0u) {
                recomp_input_set_connected(&input_model, port, false);
            }
        }
        if (recomp_input_get_state(
                &input_model, handle, &packet, &gamepad)) {
            recomp_guest_memset(output, 0, XINPUT_STATE_SIZE);
            *recomp_memory_u32(output) = packet;
            write_gamepad(output + 4u, &gamepad);
            result = ERROR_SUCCESS;
        } else if (recomp_input_handle_port(handle, &port)) {
            *recomp_memory_u32(output) = input_model.ports[port].packet_number;
            write_gamepad(output + 4u, &input_model.ports[port].gamepad);
        }
    }
    finish(entry_esp, 2u, result);
}

static void xinput_set_state_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t handle = stack_argument(entry_esp, 0u);
    uint32_t feedback = stack_argument(entry_esp, 1u);
    uint32_t port;
    uint32_t result = ERROR_INVALID_PARAMETER;

    if (feedback != 0u) {
        uint16_t left = *recomp_memory_u16(
            feedback + XINPUT_FEEDBACK_LEFT_MOTOR_OFFSET);
        uint16_t right = *recomp_memory_u16(
            feedback + XINPUT_FEEDBACK_RIGHT_MOTOR_OFFSET);

        result = recomp_input_set_feedback(
            &input_model, handle, left, right)
            ? ERROR_SUCCESS
            : ERROR_DEVICE_NOT_CONNECTED;
        if (result == ERROR_SUCCESS && feedback_sink != NULL &&
            recomp_input_handle_port(handle, &port)) {
            feedback_sink(port, left, right);
        }
    }
    if (feedback != 0u) {
        *recomp_memory_u32(feedback) = result;
        uint32_t event = *recomp_memory_u32(feedback + 4u);
        if (event != 0u) {
            /* Complete synchronously through the native kernel's event handle. */
            recomp_runtime.registers.esp = entry_esp - 12u;
            *recomp_memory_u32(entry_esp - 8u) = event;
            *recomp_memory_u32(entry_esp - 4u) = 0u;
            recomp_kernel_thread(225u)();
            if (recomp_runtime.registers.eax != 0u) {
                recomp_stop(2, "input:feedback-event-invalid");
            }
        }
    }
    finish(entry_esp, 2u, result);
}

RecompFunction recomp_input_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case XINIT_DEVICES_ADDRESS:
    case XINIT_DEVICES_THUNK_ADDRESS:
        return xinit_devices_adapter;
    case XGET_DEVICES_ADDRESS:
        return xget_devices_adapter;
    case XGET_DEVICE_CHANGES_ADDRESS:
        return xget_device_changes_adapter;
    case XINPUT_OPEN_ADDRESS:
        return xinput_open_adapter;
    case XINPUT_CLOSE_ADDRESS:
        return xinput_close_adapter;
    case XINPUT_GET_CAPABILITIES_ADDRESS:
        return xinput_get_capabilities_adapter;
    case XINPUT_GET_STATE_ADDRESS:
        return xinput_get_state_adapter;
    case XINPUT_SET_STATE_ADDRESS:
        return xinput_set_state_adapter;
    default:
        return NULL;
    }
}
