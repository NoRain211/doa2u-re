#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "input_keyboard_win32.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

static int pad_source(const char *name)
{
    for (int i = 0; recomp_input_host_source_name(true, i) != NULL; ++i)
        if (strcmp(recomp_input_host_source_name(true, i), name) == 0) return i;
    return -1;
}

static uint32_t rumbled[2];

static bool SDLCALL record_rumble(void *userdata, Uint16 low, Uint16 high)
{
    rumbled[(uintptr_t)userdata] = (uint32_t)low << 16 | high;
    return true;
}

static SDL_Joystick *attach(uintptr_t index, SDL_JoystickID *id)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.userdata = (void *)index;
    desc.Rumble = record_rumble;
    *id = SDL_AttachVirtualJoystick(&desc);
    return *id != 0 ? SDL_OpenJoystick(*id) : NULL;
}

/* CMake runs this with RECOMP_KEYMAP=a=20,bogus,start=74,x=53,up=79,down=7a and
   RECOMP_TEST_FOCUSED=1:
   Space is A, F5 is Start and S is X. The keys those had press nothing, and Y, which had S,
   has no key. "bogus" has no key and is skipped without losing the entries after it; F10 and
   F11 cannot be bound.
   RECOMP_PADMAP=b=RT,start=Nope gives B the right trigger, which leaves R Trigger with
   none; Start keeps its own button, as Nope names none. */
static int keymap(void)
{
    RecompInputGamepad pad, keys;
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_A, false) == 0x20 &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_Y, false) == -1);
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_B, true) == pad_source("RT") &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_RTRIG, true) == -1 &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_START, true) == pad_source("Start"));
    CHECK(strcmp(recomp_input_host_source_name(false, 0x74), "F5") == 0 &&
          strcmp(recomp_input_host_source_name(false, 'Q'), "Q") == 0 &&
          recomp_input_host_source_name(false, 0x79) == NULL); /* F10 */
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_UP, false) == 0x26 &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_DOWN, false) == 0x28);
    /* A key binding swaps with the key that had it; its default gives it back. */
    recomp_input_host_bind(RECOMP_INPUT_BIND_B, false, 'Q');
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_B, false) == 'Q' &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_WHITE, false) == 'X');
    recomp_input_keyboard_reset();
    recomp_input_keyboard_event('Q', true);
    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0xffu);
    CHECK(recomp_input_host_take_press(false) == 'Q' && recomp_input_host_take_press(false) == -1);
    recomp_input_host_bind(RECOMP_INPUT_BIND_B, false, -1);
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_B, false) == 'X' &&
          recomp_input_host_bound(RECOMP_INPUT_BIND_WHITE, false) == 'Q');
    recomp_input_keyboard_reset();
    recomp_input_keyboard_event('Z', true);
    recomp_input_keyboard_event(0x0du, true);
    recomp_input_keyboard_event('S', true);
    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0u && (pad.buttons & 0x0010u) == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_X] == 0xffu && pad.analog_buttons[RECOMP_INPUT_ANALOG_Y] == 0u);
    recomp_input_keyboard_event(0x20u, true);
    recomp_input_keyboard_event(0x74u, true);
    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0xffu && (pad.buttons & 0x0010u) != 0u);
    /* What the free camera leaves out while it flies. */
    recomp_input_host_keyboard_buttons(&keys);
    CHECK(keys.buttons == 0x0010u && keys.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0xffu);
    CHECK(keys.analog_buttons[RECOMP_INPUT_ANALOG_X] == 0xffu && keys.analog_buttons[RECOMP_INPUT_ANALOG_Y] == 0u);
    CHECK(keys.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0u);
    puts("input host keymap: ok");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) return keymap();
    uint8_t keys[RECOMP_INPUT_KEY_COUNT];
    recomp_input_keyboard_reset();
    recomp_input_keyboard_event('Z', true);
    recomp_input_keyboard_event('Z', false);
    recomp_input_keyboard_sample(keys);
    CHECK(keys['Z'] == 1u); /* A tap completed between guest polls. */
    recomp_input_keyboard_sample(keys);
    CHECK(keys['Z'] == 0u);
    recomp_input_keyboard_event('A', true);
    recomp_input_keyboard_event('S', true);
    recomp_input_keyboard_sample(keys);
    CHECK(keys['A'] && keys['S']);
    recomp_input_keyboard_sample(keys);
    CHECK(keys['A'] && keys['S']);
    recomp_input_keyboard_event('A', true); /* Typematic is not another press. */
    recomp_input_keyboard_event('A', false);
    recomp_input_keyboard_sample(keys);
    CHECK(!keys['A'] && keys['S']);
    recomp_input_keyboard_event('Z', true);
    recomp_input_keyboard_event('Z', false);
    recomp_input_keyboard_reset(); /* Focus loss cancels held and pending keys. */
    recomp_input_keyboard_event(RECOMP_INPUT_KEY_COUNT, true);
    recomp_input_keyboard_sample(keys);
    for (uint32_t i = 0; i < RECOMP_INPUT_KEY_COUNT; ++i) CHECK(!keys[i]);

    /* Only the virtual gamepads below may be visible. */
    const char *drivers[] = {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT,
        SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_JOYSTICK_GAMEINPUT};
    for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; ++i) SDL_SetHint(drivers[i], "0");
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));

    SDL_JoystickID first_id, second_id;
    SDL_Joystick *first = attach(0u, &first_id);
    CHECK(first != NULL);

    RecompInputGamepad pad;
    /* SDL discards the first jump of an axis that starts at an extreme (triggers). */
    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(!recomp_input_host_sample(1u, &pad));
    CHECK(recomp_input_host_pad_connected(0u) && !recomp_input_host_pad_connected(1u));
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_NORTH, true);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualButton(first, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    SDL_SetJoystickVirtualAxis(first, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_SetJoystickVirtualAxis(first, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(first, SDL_GAMEPAD_AXIS_RIGHTX, 1000);

    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(pad.buttons == 0x0014u); /* START | DPAD_LEFT */
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_Y] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_X] == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_WHITE] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_BLACK] == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_RTRIG] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_LTRIG] == 0u);
    CHECK(pad.thumb_ly == 32767); /* stick up is positive, as in XInput */
    CHECK(pad.thumb_rx == 1000);
    CHECK(pad.thumb_ry == 0);

    /* A second pad takes port 1 and gets its own input and rumble. It covers
       the mappings the first pad leaves unpressed. */
    SDL_Joystick *second = attach(1u, &second_id);
    CHECK(second != NULL);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_BACK, true);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    SDL_SetJoystickVirtualAxis(second, SDL_GAMEPAD_AXIS_LEFTX, -1000);
    SDL_SetJoystickVirtualAxis(second, SDL_GAMEPAD_AXIS_RIGHTY, 2000);
    CHECK(recomp_input_host_sample(1u, &pad));
    SDL_SetJoystickVirtualAxis(second, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(pad.buttons == 0x0060u); /* BACK | LEFT_THUMB */
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_X] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_BLACK] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_WHITE] == 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_LTRIG] == 0xffu);
    CHECK(pad.thumb_lx == -1000 && pad.thumb_ry == -2000);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(pad.buttons == 0x00a0u); /* BACK | RIGHT_THUMB */
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0xffu);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_X] == 0u);

    /* The Rebind screen hears a fresh press on any pad, once. */
    (void)recomp_input_host_take_press(true); /* what is already held */
    CHECK(recomp_input_host_take_press(true) == -1);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(recomp_input_host_take_press(true) == -1);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(recomp_input_host_take_press(true) == pad_source("RB"));
    CHECK(recomp_input_host_take_press(true) == -1);
    /* B takes RB, and Black, which had RB, takes B's old East. */
    recomp_input_host_bind(RECOMP_INPUT_BIND_B, true, pad_source("RB"));
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_BLACK, true) == pad_source("B"));
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0xffu && pad.analog_buttons[RECOMP_INPUT_ANALOG_BLACK] == 0xffu);
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0xffu && pad.analog_buttons[RECOMP_INPUT_ANALOG_BLACK] == 0u);
    recomp_input_host_bind(RECOMP_INPUT_BIND_B, true, -1); /* the default again */
    CHECK(recomp_input_host_sample(1u, &pad));
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_B] == 0u && pad.analog_buttons[RECOMP_INPUT_ANALOG_BLACK] == 0xffu);
    /* A trigger pulled past halfway presses a digital button. */
    recomp_input_host_bind(RECOMP_INPUT_BIND_START, true, pad_source("LT"));
    CHECK(recomp_input_host_sample(1u, &pad) && (pad.buttons & 0x0010u) != 0u);
    CHECK(pad.analog_buttons[RECOMP_INPUT_ANALOG_LTRIG] == 0u);
    recomp_input_host_bind(RECOMP_INPUT_BIND_START, true, -1);
    CHECK(recomp_input_host_bound(RECOMP_INPUT_BIND_LTRIG, true) == pad_source("LT"));
    SDL_SetJoystickVirtualButton(second, SDL_GAMEPAD_BUTTON_EAST, true);
    CHECK(recomp_input_host_sample(1u, &pad) && pad.buttons == 0x00a0u);

    recomp_input_host_set_vibration(1u, 0x1234u, 0x5678u);
    CHECK(rumbled[1] == 0x12345678u && rumbled[0] == 0u);
    recomp_input_host_stop_vibration();
    CHECK(rumbled[1] == 0u);

    /* Removing port 0's pad leaves port 1 in place. Port 0 still answers, neutral,
       so the keyboard keeps working and a netplay session keeps sending a pad. */
    SDL_CloseJoystick(first);
    CHECK(SDL_DetachVirtualJoystick(first_id));
    CHECK(recomp_input_host_sample(0u, &pad));
    CHECK(pad.buttons == 0u && pad.analog_buttons[RECOMP_INPUT_ANALOG_A] == 0u);
    CHECK(pad.thumb_ly == 0 && pad.analog_buttons[RECOMP_INPUT_ANALOG_RTRIG] == 0u);
    CHECK(!recomp_input_host_pad_connected(0u) && recomp_input_host_pad_connected(1u));
    CHECK(recomp_input_host_sample(1u, &pad) && pad.buttons == 0x00a0u);

    SDL_CloseJoystick(second);
    CHECK(SDL_DetachVirtualJoystick(second_id));
    CHECK(!recomp_input_host_sample(1u, &pad));
    puts("input host SDL mapping: ok");
    return 0;
}
