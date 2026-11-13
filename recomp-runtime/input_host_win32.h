#ifndef DOAXBV_RECOMP_INPUT_HOST_WIN32_H
#define DOAXBV_RECOMP_INPUT_HOST_WIN32_H

#include "input_model.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad);
/* True while a host gamepad holds the port, as of the last sample. For tests,
   RECOMP_TEST_UNPLUG=<file> stands in a pad on port 0 that is unplugged while
   the file exists. */
bool recomp_input_host_pad_connected(uint32_t port);
/* Sees every finished sample (pad and keyboard) and may change it: the free
   camera reads its controls here and hands the game a still pad. While it is
   set the right stick's click never reaches the game; stick_clicks holds it
   (and the left one) apart from the sample. */
typedef void (*RecompInputCaptureHook)(uint32_t port, RecompInputGamepad *gamepad, uint16_t stick_clicks);
void recomp_input_host_set_capture_hook(RecompInputCaptureHook hook);

/* The pad buttons the keyboard pressed in port 0's last sample: the default
   keys' (A S Z X Q W E R, the arrows, Return, Backspace) or RECOMP_KEYMAP's.
   The free camera leaves them out while it flies. */
void recomp_input_host_keyboard_buttons(RecompInputGamepad *pressed_buttons);

/* The pad buttons the keyboard and host pads press, in the Rebind screen's
   order. RECOMP_KEYMAP="a=20,..." (hex virtual keys) and RECOMP_PADMAP="a=LB,..."
   (host pad button names), built by run_game.py from private/keyboard.json and
   private/controller.json, replace the defaults; a key or button given to one
   leaves the one that had it by default. The stick clicks stay fixed. */
typedef enum RecompInputBinding {
    RECOMP_INPUT_BIND_UP, RECOMP_INPUT_BIND_DOWN, RECOMP_INPUT_BIND_LEFT, RECOMP_INPUT_BIND_RIGHT,
    RECOMP_INPUT_BIND_START, RECOMP_INPUT_BIND_BACK, RECOMP_INPUT_BIND_A, RECOMP_INPUT_BIND_B,
    RECOMP_INPUT_BIND_X, RECOMP_INPUT_BIND_Y, RECOMP_INPUT_BIND_WHITE, RECOMP_INPUT_BIND_BLACK,
    RECOMP_INPUT_BIND_LTRIG, RECOMP_INPUT_BIND_RTRIG, RECOMP_INPUT_BIND_COUNT
} RecompInputBinding;
/* Its name in the JSON files: "up", "a", "ltrig"... */
const char *recomp_input_host_binding_name(unsigned binding);
/* What presses a binding: a virtual-key code, or a host pad button (an index
   for recomp_input_host_source_name) when pad; -1 for nothing. */
int recomp_input_host_bound(unsigned binding, bool pad);
/* Gives source (-1: the binding's default) to the binding; one that had
   source takes the binding's old one, so no button loses its key. Call where
   pads are sampled. */
void recomp_input_host_bind(unsigned binding, bool pad, int source);
/* A source's name as the JSON files spell it; NULL when it cannot be bound. */
const char *recomp_input_host_source_name(bool pad, int source);
/* The first bindable key, or host pad button on any pad, pressed since the
   last call; -1 for none. Call where pads are sampled. */
int recomp_input_host_take_press(bool pad);

/* Keys the runtime reads for itself. */
typedef enum RecompInputHostKey {
    RECOMP_INPUT_HOST_KEY_F1,
    RECOMP_INPUT_HOST_KEY_GRAVE,
    RECOMP_INPUT_HOST_KEY_F12,
    RECOMP_INPUT_HOST_KEY_W,
    RECOMP_INPUT_HOST_KEY_A,
    RECOMP_INPUT_HOST_KEY_S,
    RECOMP_INPUT_HOST_KEY_D,
    RECOMP_INPUT_HOST_KEY_Q,
    RECOMP_INPUT_HOST_KEY_E,
    RECOMP_INPUT_HOST_KEY_R,
    RECOMP_INPUT_HOST_KEY_Z,
    RECOMP_INPUT_HOST_KEY_X,
    RECOMP_INPUT_HOST_KEY_C,
    RECOMP_INPUT_HOST_KEY_P,
    RECOMP_INPUT_HOST_KEY_H,
    RECOMP_INPUT_HOST_KEY_PERIOD,
    RECOMP_INPUT_HOST_KEY_BACKSLASH,
    RECOMP_INPUT_HOST_KEY_UP,
    RECOMP_INPUT_HOST_KEY_DOWN,
    RECOMP_INPUT_HOST_KEY_LEFT,
    RECOMP_INPUT_HOST_KEY_RIGHT,
    RECOMP_INPUT_HOST_KEY_RETURN,
    RECOMP_INPUT_HOST_KEY_BACKSPACE,
    RECOMP_INPUT_HOST_KEY_SHIFT,
    RECOMP_INPUT_HOST_KEY_CONTROL,
    RECOMP_INPUT_HOST_KEY_F3,
    RECOMP_INPUT_HOST_KEY_COUNT
} RecompInputHostKey;

/* As of port 0's last sample; false while the game window does not have the
   keyboard. */
bool recomp_input_host_key(RecompInputHostKey key);
/* True while a right-button drag that started over the game is held (it goes
   on outside the window), with the pointer's motion in pixels since the last
   call; zero on the call that starts a drag. */
bool recomp_input_host_mouse_drag(float *dx, float *dy);

void recomp_input_host_set_vibration(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor);
void recomp_input_host_stop_vibration(void);

#ifdef __cplusplus
}
#endif

#endif
