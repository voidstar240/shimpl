#ifndef SHIMPL_H
#define SHIMPL_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t PLWinID;
#define NULL_WINDOW ((PLWinID)0)

typedef enum PLMouseButton {
    PL_MB_LEFT,
    PL_MB_RIGHT,
    PL_MB_MIDDLE,
    PL_MB_BACK,
    PL_MB_FORWARD,
    PL_MB_EXTRA1,
    PL_MB_EXTRA2,
    PL_MB_EXTRA3,
    PL_MB_WHEELUP,
    PL_MB_WHEELDOWN,
    PL_MB_WHEELLEFT,
    PL_MB_WHEELRIGHT,
    PL_MB_COUNT,
} PLMouseButton;

typedef enum PLKey {
    // loosely based on USB keyboard scancodes (page 0x07)
    // https://usb.org/sites/default/files/hut1_7.pdf
    // Keyboard buttons
    PL_KEY_A,
    PL_KEY_B,
    PL_KEY_C,
    PL_KEY_D,
    PL_KEY_E,
    PL_KEY_F,
    PL_KEY_G,
    PL_KEY_H,
    PL_KEY_I,
    PL_KEY_J,
    PL_KEY_K,
    PL_KEY_L,
    PL_KEY_M,
    PL_KEY_N,
    PL_KEY_O,
    PL_KEY_P,
    PL_KEY_Q,
    PL_KEY_R,
    PL_KEY_S,
    PL_KEY_T,
    PL_KEY_U,
    PL_KEY_V,
    PL_KEY_W,
    PL_KEY_X,
    PL_KEY_Y,
    PL_KEY_Z,

    PL_KEY_1,
    PL_KEY_2,
    PL_KEY_3,
    PL_KEY_4,
    PL_KEY_5,
    PL_KEY_6,
    PL_KEY_7,
    PL_KEY_8,
    PL_KEY_9,
    PL_KEY_0,

    PL_KEY_ENTER,
    PL_KEY_ESCAPE,
    PL_KEY_BACKSPACE,
    PL_KEY_TAB,
    PL_KEY_SPACE,
    PL_KEY_MINUS,
    PL_KEY_EQUALS,
    PL_KEY_LEFTBRACKET,
    PL_KEY_RIGHTBRACKET,
    PL_KEY_BACKSLASH,
    PL_KEY_SEMICOLON,
    PL_KEY_APOSTROPHE,
    PL_KEY_GRAVE,
    PL_KEY_COMMA,
    PL_KEY_DOT,
    PL_KEY_SLASH,
    PL_KEY_CAPSLOCK,

    PL_KEY_F1,
    PL_KEY_F2,
    PL_KEY_F3,
    PL_KEY_F4,
    PL_KEY_F5,
    PL_KEY_F6,
    PL_KEY_F7,
    PL_KEY_F8,
    PL_KEY_F9,
    PL_KEY_F10,
    PL_KEY_F11,
    PL_KEY_F12,

    PL_KEY_PRINTSCREEN,
    PL_KEY_SCROLLLOCK,
    PL_KEY_PAUSE,

    PL_KEY_INSERT,
    PL_KEY_HOME,
    PL_KEY_PAGEUP,
    PL_KEY_DELETE,
    PL_KEY_END,
    PL_KEY_PAGEDOWN,

    PL_KEY_RIGHT,
    PL_KEY_LEFT,
    PL_KEY_DOWN,
    PL_KEY_UP,

    PL_KEY_NUMLOCK,

    PL_KEY_KP_SLASH,
    PL_KEY_KP_ASTERISK,
    PL_KEY_KP_MINUS,
    PL_KEY_KP_PLUS,
    PL_KEY_KP_ENTER,
    PL_KEY_KP_1,
    PL_KEY_KP_2,
    PL_KEY_KP_3,
    PL_KEY_KP_4,
    PL_KEY_KP_5,
    PL_KEY_KP_6,
    PL_KEY_KP_7,
    PL_KEY_KP_8,
    PL_KEY_KP_9,
    PL_KEY_KP_0,
    PL_KEY_KP_DOT,

    PL_KEY_BACKSLASH2,

    PL_KEY_APPLICATION,
    PL_KEY_POWER,

    PL_KEY_KP_EQUALS,

    PL_KEY_F13,
    PL_KEY_F14,
    PL_KEY_F15,
    PL_KEY_F16,
    PL_KEY_F17,
    PL_KEY_F18,
    PL_KEY_F19,
    PL_KEY_F20,
    PL_KEY_F21,
    PL_KEY_F22,
    PL_KEY_F23,
    PL_KEY_F24,

    PL_KEY_EXECUTE,
    PL_KEY_HELP,
    PL_KEY_MENU,
    PL_KEY_SELECT,
    PL_KEY_STOP,
    PL_KEY_AGAIN,
    PL_KEY_UNDO,
    PL_KEY_CUT,
    PL_KEY_COPY,
    PL_KEY_PASTE,
    PL_KEY_FIND,
    PL_KEY_MUTE,
    PL_KEY_VOLUMEUP,
    PL_KEY_VOLUMEDOWN,

    PL_KEY_KPCOMMA,

    PL_KEY_RO,
    PL_KEY_KATAKANAHIRAGANA,
    PL_KEY_YEN,
    PL_KEY_HENKAN,
    PL_KEY_MUHENKAN,
    PL_KEY_KPJPCOMMA,

    PL_KEY_HANGUL,
    PL_KEY_HANJA,
    PL_KEY_KATAKANA,
    PL_KEY_HIRAGANA,
    PL_KEY_HANKAKU,

    PL_KEY_KP_LEFTPAREN,
    PL_KEY_KP_RIGHTPAREN,

    PL_KEY_LEFTCTRL,
    PL_KEY_LEFTSHIFT,
    PL_KEY_LEFTALT,
    PL_KEY_LEFTMETA,
    PL_KEY_RIGHTCTRL,
    PL_KEY_RIGHTSHIFT,
    PL_KEY_RIGHTALT,
    PL_KEY_RIGHTMETA,
    PL_KEY_COUNT,
} PLKey;

typedef enum PLEventType {
    PL_EV_NONE = 0,
    PL_EV_CLOSE,
    PL_EV_MOUSE_ENTER,
    PL_EV_MOUSE_LEAVE,
    PL_EV_MOUSE_MOTION,
    PL_EV_SCROLL,
    PL_EV_BUTTON_PRESS,
    PL_EV_BUTTON_RELEASE,
    PL_EV_KEYBOARD_ENTER,
    PL_EV_KEYBOARD_LEAVE,
    PL_EV_KEY_PRESS,
    PL_EV_KEY_RELEASE,
    PL_EV_KEY_REPEAT,
    PL_EV_TEXT_INPUT,
} PLEventType;

typedef struct PLEventMotion {
    double x;
    double y;
} PLEventMotion;

typedef struct PLEventScroll {
    double h;
    double v;
    bool inverted_v : 1;
    bool inverted_h : 1;
} PLEventScroll;

typedef struct PLEventMouseButton {
    PLMouseButton button;
} PLEventMouseButton;

typedef struct PLEventKey {
    PLKey key;
} PLEventKey;

typedef struct PLEventTextInput {
    char* text;
    uint32_t text_len;
} PLEventTextInput;

typedef struct PLEvent {
    PLEventType type;
    PLWinID window;
    struct PLEvent* next;
    union {
        PLEventMotion motion;
        PLEventScroll scroll;
        PLEventMouseButton mouse_button;
        PLEventKey key;
        PLEventTextInput text;
    }
#ifdef PL_NAMED_EVENT_DATA
    data
#endif
    ;
} PLEvent;

typedef struct PLButtonState {
    bool is_down : 1;
    bool just_pressed : 1;
    bool just_released : 1;
    bool repeat : 1;
    uint8_t extra_presses : 4;
} PLButtonState;

typedef struct PLWindow {
    bool valid : 1;
    bool quit : 1; // TODO this feels bad. should_quit? transient value?
    bool maximized : 1;
    bool fullscreen : 1;
    bool has_mouse_focus : 1;
    bool has_keyboard_focus : 1;
    bool no_decorations : 1;
    char* title;
    int32_t width;
    int32_t height;

    struct PLBackendWindow* _backend;
} PLWindow;

typedef struct PLMouse {
    double pos_x;
    double pos_y;
    float scroll_delta_v;
    float scroll_delta_h;
    bool scroll_inverted_v : 1;
    bool scroll_inverted_h : 1;
    PLButtonState buttons[PL_MB_COUNT];
    PLWinID focus_window;
} PLMouse;

typedef struct PLKeyboard {
    PLButtonState keys[PL_KEY_COUNT];
    PLWinID focus_window;
} PLKeyboard;

typedef struct PLState {
    PLMouse mouse;
    PLKeyboard keyboard;
    PLWindow* windows;
    uint32_t windows_cap;

    PLEvent* event_list;

    // TODO multiple input devices?

    struct PLBackendState* _backend;
} PLState;

typedef enum PLLogLevel {
    PL_FATAL,
    PL_ERROR,
    PL_WARN,
    PL_INFO,
} PLLogLevel;

typedef void (*PLLogCallback)(PLLogLevel level, const char* message, const char* file, uint32_t line);

typedef enum PLError {
    PL_ERR_NONE = 0,
    PL_ERR_OOM,
    PL_ERR_WINDOW_SYS,
    PL_ERR_INPUT_SYS,
    PL_ERR_AUDIO_SYS,
} PLError;

uint32_t pl_last_error();
PLLogCallback pl_set_log_callback(PLLogCallback callback);
int32_t pl_init(PLState* state);
int32_t pl_deinit(PLState* state);

int32_t pl_read_events(PLState* state);
int32_t pl_update(PLState* state);

PLWinID pl_open_window(PLState* state);

#endif /* SHIMPL_H */
