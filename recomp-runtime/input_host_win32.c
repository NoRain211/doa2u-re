#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "input_keyboard_win32.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <SDL3/SDL.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
    XBOX_LEFT_THUMB = 0x0040u,
    XBOX_RIGHT_THUMB = 0x0080u,
    /* The game resends motors every frame; a stalled game stops rumbling. */
    RUMBLE_DURATION_MS = 1000u,
};

static SDL_Gamepad *pads[RECOMP_INPUT_PORT_COUNT];
static uint8_t keyboard[RECOMP_INPUT_KEY_COUNT];

#ifdef _WIN32
static bool pressed(int key)
{
    return keyboard[key] || (GetAsyncKeyState(key) & 0x8000) != 0;
}

static bool focused(void)
{
    DWORD foreground_process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
    return foreground_process == GetCurrentProcessId();
}

static unsigned long long tick_ms(void)
{
    return GetTickCount64();
}
#else
/* Virtual-key codes the keyboard model is indexed by; the SDL_GPU presenter
   forwards SDL key events under these codes. */
enum {
    VK_BACK = 0x08, VK_RETURN = 0x0d, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F1 = 0x70, VK_F3 = 0x72, VK_F12 = 0x7b,
    VK_OEM_PERIOD = 0xbe, VK_OEM_3 = 0xc0, VK_OEM_5 = 0xdc,
};

static bool pressed(int key)
{
    return keyboard[key] != 0u;
}

static bool focused(void)
{
    return SDL_GetKeyboardFocus() != NULL;
}

static unsigned long long tick_ms(void)
{
    return SDL_GetTicks();
}
#endif

static bool trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_INPUT_TRACE") != NULL;
    return enabled != 0;
}

/* Host pad buttons a binding can take, named as private/controller.json
   spells them (run_game.py's PAD_NAMES). SDL names face buttons by position,
   so Cross/Circle/Square/Triangle are A/B/X/Y. */
enum {
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_START, PAD_LB, PAD_RB, PAD_LT, PAD_RT, PAD_LS, PAD_RS,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_MISC, PAD_P1, PAD_P2, PAD_P3, PAD_P4, PAD_TOUCHPAD, PAD_SOURCES
};
static const struct { const char *name; int button, axis; } pad_sources[PAD_SOURCES] = {
    [PAD_A] = {"A", SDL_GAMEPAD_BUTTON_SOUTH, -1}, [PAD_B] = {"B", SDL_GAMEPAD_BUTTON_EAST, -1},
    [PAD_X] = {"X", SDL_GAMEPAD_BUTTON_WEST, -1}, [PAD_Y] = {"Y", SDL_GAMEPAD_BUTTON_NORTH, -1},
    [PAD_BACK] = {"Back", SDL_GAMEPAD_BUTTON_BACK, -1}, [PAD_START] = {"Start", SDL_GAMEPAD_BUTTON_START, -1},
    [PAD_LB] = {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1}, [PAD_RB] = {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1},
    [PAD_LT] = {"LT", -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, [PAD_RT] = {"RT", -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    [PAD_LS] = {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK, -1}, [PAD_RS] = {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1},
    [PAD_UP] = {"Up", SDL_GAMEPAD_BUTTON_DPAD_UP, -1}, [PAD_DOWN] = {"Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1},
    [PAD_LEFT] = {"Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1}, [PAD_RIGHT] = {"Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1},
    [PAD_MISC] = {"Misc", SDL_GAMEPAD_BUTTON_MISC1, -1}, [PAD_P1] = {"P1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, -1},
    [PAD_P2] = {"P2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, -1}, [PAD_P3] = {"P3", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, -1},
    [PAD_P4] = {"P4", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, -1}, [PAD_TOUCHPAD] = {"Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD, -1},
};

/* Keys a binding can take, named as private/keyboard.json spells them
   (run_game.py's KEY_CODES). F10 is a Windows system key and F11 toggles
   fullscreen, so neither is here. */
static const struct { uint8_t key; const char *name; } named_keys[] = {
    {0x08, "Backspace"}, {0x09, "Tab"}, {0x0d, "Enter"}, {0x10, "Shift"}, {0x11, "Ctrl"}, {0x1b, "Esc"},
    {0x20, "Space"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"},
    {0x70, "F1"}, {0x71, "F2"}, {0x72, "F3"}, {0x73, "F4"}, {0x74, "F5"}, {0x75, "F6"}, {0x76, "F7"},
    {0x77, "F8"}, {0x78, "F9"}, {0x7b, "F12"},
};
static const char alnum_names[36][2] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C",
    "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z"};

/* Each pad button's key for port 0 and host pad button for every port:
   source[0] the key, source[1] the pad button, -1 for none. */
static struct {
    const char *name;
    uint16_t button;
    int analog;
    int defaults[2], source[2];
} bindings[RECOMP_INPUT_BIND_COUNT] = {
    {"up", XBOX_DPAD_UP, -1, {VK_UP, PAD_UP}}, {"down", XBOX_DPAD_DOWN, -1, {VK_DOWN, PAD_DOWN}},
    {"left", XBOX_DPAD_LEFT, -1, {VK_LEFT, PAD_LEFT}}, {"right", XBOX_DPAD_RIGHT, -1, {VK_RIGHT, PAD_RIGHT}},
    {"start", XBOX_START, -1, {VK_RETURN, PAD_START}}, {"back", XBOX_BACK, -1, {VK_BACK, PAD_BACK}},
    {"a", 0, RECOMP_INPUT_ANALOG_A, {'Z', PAD_A}}, {"b", 0, RECOMP_INPUT_ANALOG_B, {'X', PAD_B}},
    {"x", 0, RECOMP_INPUT_ANALOG_X, {'A', PAD_X}}, {"y", 0, RECOMP_INPUT_ANALOG_Y, {'S', PAD_Y}},
    /* White sits left of Black on the Xbox pad, as Q is left of W. */
    {"white", 0, RECOMP_INPUT_ANALOG_WHITE, {'Q', PAD_LB}}, {"black", 0, RECOMP_INPUT_ANALOG_BLACK, {'W', PAD_RB}},
    {"ltrig", 0, RECOMP_INPUT_ANALOG_LTRIG, {'E', PAD_LT}}, {"rtrig", 0, RECOMP_INPUT_ANALOG_RTRIG, {'R', PAD_RT}},
};

const char *recomp_input_host_source_name(bool pad, int source)
{
    if (pad) return source >= 0 && source < PAD_SOURCES ? pad_sources[source].name : NULL;
    if (source >= 'A' && source <= 'Z') return alnum_names[source - 'A' + 10];
    if (source >= '0' && source <= '9') return alnum_names[source - '0'];
    for (size_t i = 0; i < sizeof named_keys / sizeof named_keys[0]; ++i)
        if (named_keys[i].key == source) return named_keys[i].name;
    return NULL;
}

/* A RECOMP_KEYMAP (hex keys) or RECOMP_PADMAP (pad button names) value. */
static int parse_source(bool pad, const char *value, size_t length)
{
    if (!pad) {
        const unsigned long key = strtoul(value, NULL, 16);
        return key > 0ul && key < RECOMP_INPUT_KEY_COUNT &&
            recomp_input_host_source_name(false, (int)key) != NULL ? (int)key : -1;
    }
    for (int i = 0; i < PAD_SOURCES; ++i)
        if (strlen(pad_sources[i].name) == length && strncmp(value, pad_sources[i].name, length) == 0) return i;
    return -1;
}

static void load_map(bool pad, const char *at)
{
    bool bound[RECOMP_INPUT_BIND_COUNT] = {false};
    while (at != NULL && *at != '\0') {
        const char *next = strchr(at, ',');
        const char *equals = strchr(at, '=');
        if (equals != NULL && (next == NULL || equals < next)) { /* an entry without = is skipped */
            const char *value = equals + 1;
            const int source = parse_source(pad, value, next != NULL ? (size_t)(next - value) : strlen(value));
            for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i) {
                if (strlen(bindings[i].name) == (size_t)(equals - at) &&
                    strncmp(at, bindings[i].name, equals - at) == 0 && source >= 0) {
                    bindings[i].source[pad] = source;
                    bound[i] = true;
                }
            }
        }
        at = next != NULL ? next + 1 : NULL;
    }
    /* A source given to one button leaves the button that had it by default. */
    for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i)
        for (size_t j = 0; j < RECOMP_INPUT_BIND_COUNT; ++j)
            if (!bound[i] && bound[j] && bindings[i].source[pad] == bindings[j].source[pad]) bindings[i].source[pad] = -1;
}

static void load_bindings(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;
    for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i) memcpy(bindings[i].source, bindings[i].defaults, sizeof bindings[i].source);
    load_map(false, getenv("RECOMP_KEYMAP"));
    load_map(true, getenv("RECOMP_PADMAP"));
}

const char *recomp_input_host_binding_name(unsigned binding)
{
    return binding < RECOMP_INPUT_BIND_COUNT ? bindings[binding].name : NULL;
}

int recomp_input_host_bound(unsigned binding, bool pad)
{
    load_bindings();
    return binding < RECOMP_INPUT_BIND_COUNT ? bindings[binding].source[pad] : -1;
}

void recomp_input_host_bind(unsigned binding, bool pad, int source)
{
    load_bindings();
    if (binding >= RECOMP_INPUT_BIND_COUNT) return;
    if (source < 0) source = bindings[binding].defaults[pad];
    if (recomp_input_host_source_name(pad, source) == NULL) return;
    for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i)
        if (bindings[i].source[pad] == source) bindings[i].source[pad] = bindings[binding].source[pad];
    bindings[binding].source[pad] = source;
}

/* recomp_input_host_test has no window; only its target defines RECOMP_INPUT_TEST_FOCUSED,
   so RECOMP_TEST_FOCUSED never takes the focus check away in the game. */
static bool test_focused(void)
{
#ifdef RECOMP_INPUT_TEST_FOCUSED
    static int set = -1;
    if (set < 0) set = getenv("RECOMP_TEST_FOCUSED") != NULL;
    return set != 0;
#else
    return false;
#endif
}

static RecompInputGamepad keyboard_buttons; /* what the keymap pressed in port 0's last sample */

void recomp_input_host_keyboard_buttons(RecompInputGamepad *pressed_buttons)
{
    *pressed_buttons = keyboard_buttons;
}

static const char *test_unplug_file(void)
{
    static const char *path;
    static bool read;
    if (!read) {
        read = true;
        path = getenv("RECOMP_TEST_UNPLUG");
        if (path != NULL && *path == '\0') path = NULL;
    }
    return path;
}

static bool test_unplugged(void)
{
    const char *path = test_unplug_file();
    FILE *file = path != NULL ? fopen(path, "rb") : NULL;
    if (file == NULL) return false;
    fclose(file);
    return true;
}

/* SDL starts lazily on the guest thread that polls it. There is no SDL event
   loop; SDL_UpdateGamepads pumps it. A new gamepad takes the lowest free port
   and keeps it until it disconnects. */
static void update_pads(void)
{
    static bool initialized;

    if (!initialized) {
        initialized = true;
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "recomp input: SDL gamepad init failed: %s\n", SDL_GetError());
        }
        SDL_SetJoystickEventsEnabled(false);
        SDL_SetGamepadEventsEnabled(false);
    }
    SDL_UpdateGamepads();
    for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        if (pads[port] != NULL && !SDL_GamepadConnected(pads[port])) {
            SDL_CloseGamepad(pads[port]);
            pads[port] = NULL;
        }
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count; ++i) {
        uint32_t free_port = RECOMP_INPUT_PORT_COUNT;
        bool open = false;
        for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
            if (pads[port] == NULL) {
                if (free_port == RECOMP_INPUT_PORT_COUNT) free_port = port;
            } else if (SDL_GetGamepadID(pads[port]) == ids[i]) {
                open = true;
            }
        }
        if (!open && free_port < RECOMP_INPUT_PORT_COUNT) {
            pads[free_port] = SDL_OpenGamepad(ids[i]);
            if (pads[free_port] != NULL) {
                fprintf(stderr, "recomp input: gamepad '%s' on port=%u\n",
                    SDL_GetGamepadName(pads[free_port]), (unsigned)free_port);
            }
        }
    }
    SDL_free(ids);
}

void recomp_input_host_set_vibration(
    uint32_t port,
    uint16_t left_motor,
    uint16_t right_motor)
{
    static uint32_t traced[RECOMP_INPUT_PORT_COUNT];
    uint32_t motors = (uint32_t)left_motor << 16 | right_motor;

    if (port >= RECOMP_INPUT_PORT_COUNT || pads[port] == NULL) {
        return;
    }
    /* SDL sends only changed values and keeps a held value alive. */
    bool sent = SDL_RumbleGamepad(pads[port], left_motor, right_motor, RUMBLE_DURATION_MS);
    if (trace_enabled() && traced[port] != motors) {
        traced[port] = motors;
        fprintf(stderr, "recomp input: vibration port=%u left=%u right=%u sent=%u\n",
            (unsigned)port, (unsigned)left_motor, (unsigned)right_motor, (unsigned)sent);
    }
}

void recomp_input_host_stop_vibration(void)
{
    for (uint32_t port = 0u; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        recomp_input_host_set_vibration(port, 0u, 0u);
    }
}

static void trace_sample(
    const RecompInputGamepad *pad, SDL_Gamepad *host, bool is_focused)
{
    static bool seen;
    static unsigned lines;
    static RecompInputGamepad previous;
    static SDL_JoystickID previous_id;
    static bool previous_focused;
    if (!trace_enabled() || lines >= 4096u) return;
    SDL_JoystickID id = host != NULL ? SDL_GetGamepadID(host) : 0;
    if (seen && memcmp(&previous, pad, sizeof previous) == 0 &&
        previous_id == id && previous_focused == is_focused) return;
    const char *host_name = host != NULL ? SDL_GetGamepadName(host) : NULL;
    fprintf(stderr,
        "[DEBUG-r501-input] tick_ms=%llu pad='%s' focused=%u"
        " digital=%04x analog=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x"
        " axes=%d,%d,%d,%d\n",
        tick_ms(), host_name != NULL ? host_name : "none",
        is_focused ? 1u : 0u,
        (unsigned)pad->buttons, pad->analog_buttons[0], pad->analog_buttons[1],
        pad->analog_buttons[2], pad->analog_buttons[3], pad->analog_buttons[4],
        pad->analog_buttons[5], pad->analog_buttons[6], pad->analog_buttons[7],
        pad->thumb_lx, pad->thumb_ly, pad->thumb_rx, pad->thumb_ry);
    previous = *pad;
    previous_id = id;
    previous_focused = is_focused;
    seen = true;
    ++lines;
}

/* How far a host pad button is pressed: 0 or 0xff, or 0..0xff for a trigger. */
static uint8_t source_value(SDL_Gamepad *gamepad, int source)
{
    if (source < 0 || source >= PAD_SOURCES) return 0u;
    /* SDL triggers are 0..32767; the guest expects 0..255. */
    if (pad_sources[source].axis >= 0)
        return (uint8_t)(SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)pad_sources[source].axis) >> 7);
    return SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)pad_sources[source].button) ? 0xffu : 0u;
}

/* SDL Y axes point down; XInput Y axes point up. */
static int16_t up_axis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
    int16_t down = SDL_GetGamepadAxis(gamepad, axis);
    return down == INT16_MIN ? INT16_MAX : (int16_t)-down;
}

static void read_pad(SDL_Gamepad *host, RecompInputGamepad *gamepad)
{
    load_bindings();
    if (SDL_GetGamepadButton(host, SDL_GAMEPAD_BUTTON_LEFT_STICK)) gamepad->buttons |= XBOX_LEFT_THUMB;
    if (SDL_GetGamepadButton(host, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) gamepad->buttons |= XBOX_RIGHT_THUMB;
    for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i) {
        const uint8_t value = source_value(host, bindings[i].source[1]);
        if (bindings[i].button != 0u) {
            if (value > RECOMP_INPUT_ANALOG_DOWN) gamepad->buttons |= bindings[i].button;
        } else if (value > gamepad->analog_buttons[bindings[i].analog]) {
            gamepad->analog_buttons[bindings[i].analog] = value;
        }
    }
    gamepad->thumb_lx = SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_LEFTX);
    gamepad->thumb_ly = up_axis(host, SDL_GAMEPAD_AXIS_LEFTY);
    gamepad->thumb_rx = SDL_GetGamepadAxis(host, SDL_GAMEPAD_AXIS_RIGHTX);
    gamepad->thumb_ry = up_axis(host, SDL_GAMEPAD_AXIS_RIGHTY);
}

static RecompInputCaptureHook capture_hook;

void recomp_input_host_set_capture_hook(RecompInputCaptureHook hook)
{
    capture_hook = hook;
}

static bool sample(uint32_t port, RecompInputGamepad *gamepad);

bool recomp_input_host_sample(uint32_t port, RecompInputGamepad *gamepad)
{
    const bool present = sample(port, gamepad);
    if (present && capture_hook != NULL) {
        const uint16_t stick_clicks = gamepad->buttons & (XBOX_LEFT_THUMB | XBOX_RIGHT_THUMB);
        /* Port 0's right stick click is the free camera's own. */
        if (port == 0u) gamepad->buttons &= (uint16_t)~XBOX_RIGHT_THUMB;
        capture_hook(port, gamepad, stick_clicks);
    }
    return present;
}

static bool sample(uint32_t port, RecompInputGamepad *gamepad)
{
    if (gamepad == NULL || port >= RECOMP_INPUT_PORT_COUNT) {
        return false;
    }
    memset(gamepad, 0, sizeof *gamepad);
    update_pads();
    SDL_Gamepad *host = port == 0u && test_unplugged() ? NULL : pads[port];
    if (host != NULL) {
        read_pad(host, gamepad);
    }
    if (port != 0u) {
        /* The keyboard drives port 0 only. */
        return host != NULL;
    }
    memset(&keyboard_buttons, 0, sizeof keyboard_buttons);
    if (!focused() && !test_focused()) {
        recomp_input_keyboard_reset();
        trace_sample(gamepad, host, false);
        return true;
    }

    recomp_input_keyboard_sample(keyboard);
    load_bindings();
    for (size_t i = 0; i < RECOMP_INPUT_BIND_COUNT; ++i) {
        if (bindings[i].source[0] < 0 || !pressed(bindings[i].source[0])) continue;
        if (bindings[i].button != 0u) keyboard_buttons.buttons |= bindings[i].button;
        else keyboard_buttons.analog_buttons[bindings[i].analog] = 0xffu;
    }
    gamepad->buttons |= keyboard_buttons.buttons;
    for (unsigned i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i)
        gamepad->analog_buttons[i] |= keyboard_buttons.analog_buttons[i];
    trace_sample(gamepad, host, true);
    return true;
}

bool recomp_input_host_pad_connected(uint32_t port)
{
    if (port >= RECOMP_INPUT_PORT_COUNT) return false;
    if (port == 0u && test_unplug_file() != NULL) return !test_unplugged();
    return pads[port] != NULL;
}

int recomp_input_host_take_press(bool pad)
{
    static bool was[2][RECOMP_INPUT_KEY_COUNT];
    int found = -1;
    const int count = pad ? PAD_SOURCES : RECOMP_INPUT_KEY_COUNT;
    const bool keys = focused() || test_focused();
    for (int source = 0; source < count; ++source) {
        bool down = false;
        if (!pad) down = keys && recomp_input_host_source_name(false, source) != NULL && pressed(source);
        for (uint32_t port = 0u; pad && port < RECOMP_INPUT_PORT_COUNT; ++port)
            down = down || (pads[port] != NULL && source_value(pads[port], source) > RECOMP_INPUT_ANALOG_DOWN);
        if (down && !was[pad][source] && found < 0) found = source;
        was[pad][source] = down;
    }
    return found;
}

bool recomp_input_host_key(RecompInputHostKey key)
{
    if (!focused()) return false;
    switch (key) {
    case RECOMP_INPUT_HOST_KEY_F1: return pressed(VK_F1);
    case RECOMP_INPUT_HOST_KEY_GRAVE: return pressed(VK_OEM_3);
    case RECOMP_INPUT_HOST_KEY_F12: return pressed(VK_F12);
    case RECOMP_INPUT_HOST_KEY_W: return pressed('W');
    case RECOMP_INPUT_HOST_KEY_A: return pressed('A');
    case RECOMP_INPUT_HOST_KEY_S: return pressed('S');
    case RECOMP_INPUT_HOST_KEY_D: return pressed('D');
    case RECOMP_INPUT_HOST_KEY_Q: return pressed('Q');
    case RECOMP_INPUT_HOST_KEY_E: return pressed('E');
    case RECOMP_INPUT_HOST_KEY_R: return pressed('R');
    case RECOMP_INPUT_HOST_KEY_Z: return pressed('Z');
    case RECOMP_INPUT_HOST_KEY_X: return pressed('X');
    case RECOMP_INPUT_HOST_KEY_C: return pressed('C');
    case RECOMP_INPUT_HOST_KEY_P: return pressed('P');
    case RECOMP_INPUT_HOST_KEY_H: return pressed('H');
    case RECOMP_INPUT_HOST_KEY_PERIOD: return pressed(VK_OEM_PERIOD);
    case RECOMP_INPUT_HOST_KEY_BACKSLASH: return pressed(VK_OEM_5);
    case RECOMP_INPUT_HOST_KEY_UP: return pressed(VK_UP);
    case RECOMP_INPUT_HOST_KEY_DOWN: return pressed(VK_DOWN);
    case RECOMP_INPUT_HOST_KEY_LEFT: return pressed(VK_LEFT);
    case RECOMP_INPUT_HOST_KEY_RIGHT: return pressed(VK_RIGHT);
    case RECOMP_INPUT_HOST_KEY_RETURN: return pressed(VK_RETURN);
    case RECOMP_INPUT_HOST_KEY_BACKSPACE: return pressed(VK_BACK);
    case RECOMP_INPUT_HOST_KEY_SHIFT: return pressed(VK_SHIFT);
    case RECOMP_INPUT_HOST_KEY_CONTROL: return pressed(VK_CONTROL);
    case RECOMP_INPUT_HOST_KEY_F3: return pressed(VK_F3);
    case RECOMP_INPUT_HOST_KEY_COUNT: break;
    }
    return false;
}

bool recomp_input_host_mouse_drag(float *dx, float *dy)
{
    static bool dragging;
    static float last_x, last_y;
    float x = 0.0f, y = 0.0f;
    bool held = false;

    *dx = *dy = 0.0f;
    if (focused()) {
#ifdef _WIN32
        /* GetAsyncKeyState reads physical buttons; swapped buttons put the
           logical right one on the left. */
        const int right = GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON;
        POINT point = {0, 0};
        held = (GetAsyncKeyState(right) & 0x8000) != 0 && GetCursorPos(&point);
        if (held && !dragging) {
            /* A drag starts only over the game's client area. */
            HWND window = GetForegroundWindow();
            POINT client = point;
            RECT area;
            held = ScreenToClient(window, &client) && GetClientRect(window, &area) &&
                PtInRect(&area, client);
        }
        x = (float)point.x;
        y = (float)point.y;
#else
        held = (SDL_GetMouseState(&x, &y) & SDL_BUTTON_RMASK) != 0u;
#endif
    }
    if (held && dragging) {
        *dx = x - last_x;
        *dy = y - last_y;
    }
    dragging = held;
    last_x = x;
    last_y = y;
    return held;
}
