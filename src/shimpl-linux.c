#include "shimpl.h"
#include <stdio.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <locale.h>

#define WAYLAND_PROTOCOLS_IMPLEMENTATION
#include <wayland-client.h>
#include <cursor-shape.h>
#include <pointer-constraints.h>
#include <xdg-shell.h>
#include <xdg-decoration.h>
// REMOVE IF ADDING TABLET INTERFACE
const struct wl_interface zwp_tablet_tool_v2_interface = {0};

#define FIRST_WIN_ID 1

static PLWindow windows[128];
static PLWindow last_windows[128];
static PLWindow next_windows[128];

static PLEvent events[1024];

static PLErrorCallback g_error_callback = NULL;
static char g_error_msg_buf[1024] = {0};

typedef struct StateInternal StateInternal;
typedef struct WindowState WindowState;

struct WindowState {
    StateInternal* state;

    struct wl_surface* surface;
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* toplevel;
    struct zxdg_toplevel_decoration_v1* decor;
    uint32_t window_id;
};

#define SCROLL_AXIS_COUNT 2

typedef struct {
    double scroll[SCROLL_AXIS_COUNT];
    int32_t scroll120[SCROLL_AXIS_COUNT];
    uint32_t source;
    uint8_t stop[SCROLL_AXIS_COUNT];
    uint8_t scroll_dir[SCROLL_AXIS_COUNT];
} WlPointerEventGroup;

struct StateInternal {
    bool initialized;
    // wayland state
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct xdg_wm_base* xdg_wm_base;
    struct zxdg_decoration_manager_v1* decor_manager;
    struct wl_seat* seat;
    struct wl_pointer* pointer;
    struct wl_keyboard* keyboard;

    uint32_t pointer_serial;
    WlPointerEventGroup pointer_ev_group;
    double scroll_acc[SCROLL_AXIS_COUNT];
    double scroll120_acc[SCROLL_AXIS_COUNT];
    double scroll_click_scale;

    // xkb state
    void* keymap_raw;
    uint32_t keymap_raw_size;
    struct xkb_context* xkb_ctx;
    struct xkb_keymap* xkb_keymap;
    struct xkb_state* xkb_state;
    struct xkb_compose_table* xkb_compose_table;
    struct xkb_compose_state* xkb_compose_state;

    // extra
    WindowState* window_states;
    WindowState* next_free_window; // free list
    uint32_t window_states_len;

    PLState* last_state; // last state sent to user
    PLState* curr_state; // current state containing user changes
    PLState* next_state; // state to send at the end of the frame

    PLState state_buf_a;
    PLState state_buf_b;
};

static WindowState _window_states[128];

static StateInternal _internal_state;

#define LOG_WARNING(...) do { \
    snprintf(g_error_msg_buf, sizeof(g_error_msg_buf), __VA_ARGS__); \
    log_error(PL_WARNING, __FILE__, __LINE__); \
} while (0)
#define LOG_ERROR(...) do { \
    snprintf(g_error_msg_buf, sizeof(g_error_msg_buf), __VA_ARGS__); \
    log_error(PL_ERROR, __FILE__, __LINE__); \
} while (0)
#define LOG_FATAL(...) do { \
    snprintf(g_error_msg_buf, sizeof(g_error_msg_buf), __VA_ARGS__); \
    log_error(PL_FATAL, __FILE__, __LINE__); \
} while (0)

static void log_error(uint8_t severity, const char* file, uint32_t line) {
    if (g_error_callback) {
        g_error_callback(severity, g_error_msg_buf, file, line);
    }
}

static void push_event(StateInternal* state, PLEvent ev) {
    uint32_t cap = sizeof(events) / sizeof(events[0]);
    assert(state->next_state->events_len < cap);
    state->next_state->events[state->next_state->events_len++] = ev;
}

static void press_button(PLButtonState* button) {
    button->is_down = true;
    if (button->just_pressed) {
        if (button->extra_presses < 15) {
            button->extra_presses++;
        }
    }
    button->just_pressed = true;
}

static void release_button(PLButtonState* button) {
    button->is_down = false;
    button->just_released = true;
}

static void repeat_button(PLButtonState* button) {
    if (button->repeat) {
        if (button->extra_presses < 15) {
            button->extra_presses++;
        }
    }
    button->repeat = true;
}

static PLKey evcode2key(uint32_t evcode) {
    switch (evcode) {
        case KEY_A: return PL_KEY_A;
        case KEY_B: return PL_KEY_B;
        case KEY_C: return PL_KEY_C;
        case KEY_D: return PL_KEY_D;
        case KEY_E: return PL_KEY_E;
        case KEY_F: return PL_KEY_F;
        case KEY_G: return PL_KEY_G;
        case KEY_H: return PL_KEY_H;
        case KEY_I: return PL_KEY_I;
        case KEY_J: return PL_KEY_J;
        case KEY_K: return PL_KEY_K;
        case KEY_L: return PL_KEY_L;
        case KEY_M: return PL_KEY_M;
        case KEY_N: return PL_KEY_N;
        case KEY_O: return PL_KEY_O;
        case KEY_P: return PL_KEY_P;
        case KEY_Q: return PL_KEY_Q;
        case KEY_R: return PL_KEY_R;
        case KEY_S: return PL_KEY_S;
        case KEY_T: return PL_KEY_T;
        case KEY_U: return PL_KEY_U;
        case KEY_V: return PL_KEY_V;
        case KEY_W: return PL_KEY_W;
        case KEY_X: return PL_KEY_X;
        case KEY_Y: return PL_KEY_Y;
        case KEY_Z: return PL_KEY_Z;
        case KEY_1: return PL_KEY_1;
        case KEY_2: return PL_KEY_2;
        case KEY_3: return PL_KEY_3;
        case KEY_4: return PL_KEY_4;
        case KEY_5: return PL_KEY_5;
        case KEY_6: return PL_KEY_6;
        case KEY_7: return PL_KEY_7;
        case KEY_8: return PL_KEY_8;
        case KEY_9: return PL_KEY_9;
        case KEY_0: return PL_KEY_0;
        case KEY_ENTER: return PL_KEY_ENTER;
        case KEY_ESC: return PL_KEY_ESCAPE;
        case KEY_BACKSPACE: return PL_KEY_BACKSPACE;
        case KEY_TAB: return PL_KEY_TAB;
        case KEY_SPACE: return PL_KEY_SPACE;
        case KEY_MINUS: return PL_KEY_MINUS;
        case KEY_EQUAL: return PL_KEY_EQUALS;
        case KEY_LEFTBRACE: return PL_KEY_LEFTBRACKET;
        case KEY_RIGHTBRACE: return PL_KEY_RIGHTBRACKET;
        case KEY_BACKSLASH: return PL_KEY_BACKSLASH;
        case KEY_SEMICOLON: return PL_KEY_SEMICOLON;
        case KEY_APOSTROPHE: return PL_KEY_APOSTROPHE;
        case KEY_GRAVE: return PL_KEY_GRAVE;
        case KEY_COMMA: return PL_KEY_COMMA;
        case KEY_DOT: return PL_KEY_DOT;
        case KEY_SLASH: return PL_KEY_SLASH;
        case KEY_CAPSLOCK: return PL_KEY_CAPSLOCK;
        case KEY_F1: return PL_KEY_F1;
        case KEY_F2: return PL_KEY_F2;
        case KEY_F3: return PL_KEY_F3;
        case KEY_F4: return PL_KEY_F4;
        case KEY_F5: return PL_KEY_F5;
        case KEY_F6: return PL_KEY_F6;
        case KEY_F7: return PL_KEY_F7;
        case KEY_F8: return PL_KEY_F8;
        case KEY_F9: return PL_KEY_F9;
        case KEY_F10: return PL_KEY_F10;
        case KEY_F11: return PL_KEY_F11;
        case KEY_F12: return PL_KEY_F12;
        case KEY_SYSRQ: return PL_KEY_PRINTSCREEN;
        case KEY_SCROLLLOCK: return PL_KEY_SCROLLLOCK;
        case KEY_PAUSE: return PL_KEY_PAUSE;
        case KEY_INSERT: return PL_KEY_INSERT;
        case KEY_HOME: return PL_KEY_HOME;
        case KEY_PAGEUP: return PL_KEY_PAGEUP;
        case KEY_DELETE: return PL_KEY_DELETE;
        case KEY_END: return PL_KEY_END;
        case KEY_PAGEDOWN: return PL_KEY_PAGEDOWN;
        case KEY_RIGHT: return PL_KEY_RIGHT;
        case KEY_LEFT: return PL_KEY_LEFT;
        case KEY_DOWN: return PL_KEY_DOWN;
        case KEY_UP: return PL_KEY_UP;
        case KEY_NUMLOCK: return PL_KEY_NUMLOCK;
        case KEY_KPSLASH: return PL_KEY_KP_SLASH;
        case KEY_KPASTERISK: return PL_KEY_KP_ASTERISK;
        case KEY_KPMINUS: return PL_KEY_KP_MINUS;
        case KEY_KPPLUS: return PL_KEY_KP_PLUS;
        case KEY_KPENTER: return PL_KEY_KP_ENTER;
        case KEY_KP1: return PL_KEY_KP_1;
        case KEY_KP2: return PL_KEY_KP_2;
        case KEY_KP3: return PL_KEY_KP_3;
        case KEY_KP4: return PL_KEY_KP_4;
        case KEY_KP5: return PL_KEY_KP_5;
        case KEY_KP6: return PL_KEY_KP_6;
        case KEY_KP7: return PL_KEY_KP_7;
        case KEY_KP8: return PL_KEY_KP_8;
        case KEY_KP9: return PL_KEY_KP_9;
        case KEY_KP0: return PL_KEY_KP_0;
        case KEY_KPDOT: return PL_KEY_KP_DOT;
        case KEY_102ND: return PL_KEY_BACKSLASH2;
        case KEY_COMPOSE: return PL_KEY_APPLICATION;
        case KEY_POWER: return PL_KEY_POWER;
        case KEY_KPEQUAL: return PL_KEY_KP_EQUALS;
        case KEY_F13: return PL_KEY_F13;
        case KEY_F14: return PL_KEY_F14;
        case KEY_F15: return PL_KEY_F15;
        case KEY_F16: return PL_KEY_F16;
        case KEY_F17: return PL_KEY_F17;
        case KEY_F18: return PL_KEY_F18;
        case KEY_F19: return PL_KEY_F19;
        case KEY_F20: return PL_KEY_F20;
        case KEY_F21: return PL_KEY_F21;
        case KEY_F22: return PL_KEY_F22;
        case KEY_F23: return PL_KEY_F23;
        case KEY_F24: return PL_KEY_F24;
        case KEY_OPEN: return PL_KEY_EXECUTE;
        case KEY_HELP: return PL_KEY_HELP;
        case KEY_PROPS: return PL_KEY_MENU;
        case KEY_FRONT: return PL_KEY_SELECT;
        case KEY_STOP: return PL_KEY_STOP;
        case KEY_AGAIN: return PL_KEY_AGAIN;
        case KEY_UNDO: return PL_KEY_UNDO;
        case KEY_CUT: return PL_KEY_CUT;
        case KEY_COPY: return PL_KEY_COPY;
        case KEY_PASTE: return PL_KEY_PASTE;
        case KEY_FIND: return PL_KEY_FIND;
        case KEY_MUTE: return PL_KEY_MUTE;
        case KEY_VOLUMEUP: return PL_KEY_VOLUMEUP;
        case KEY_VOLUMEDOWN: return PL_KEY_VOLUMEDOWN;
        case KEY_KPCOMMA: return PL_KEY_KPCOMMA;
        case KEY_RO: return PL_KEY_RO;
        case KEY_KATAKANAHIRAGANA: return PL_KEY_KATAKANAHIRAGANA;
        case KEY_YEN: return PL_KEY_YEN;
        case KEY_HENKAN: return PL_KEY_HENKAN;
        case KEY_MUHENKAN: return PL_KEY_MUHENKAN;
        case KEY_KPJPCOMMA: return PL_KEY_KPJPCOMMA;
        case KEY_HANGUEL: return PL_KEY_HANGUL;
        case KEY_HANJA: return PL_KEY_HANJA;
        case KEY_KATAKANA: return PL_KEY_KATAKANA;
        case KEY_HIRAGANA: return PL_KEY_HIRAGANA;
        case KEY_ZENKAKUHANKAKU: return PL_KEY_HANKAKU;
        case KEY_KPLEFTPAREN: return PL_KEY_KP_LEFTPAREN;
        case KEY_KPRIGHTPAREN: return PL_KEY_KP_RIGHTPAREN;
        case KEY_LEFTCTRL: return PL_KEY_LEFTCTRL;
        case KEY_LEFTSHIFT: return PL_KEY_LEFTSHIFT;
        case KEY_LEFTALT: return PL_KEY_LEFTALT;
        case KEY_LEFTMETA: return PL_KEY_LEFTMETA;
        case KEY_RIGHTCTRL: return PL_KEY_RIGHTCTRL;
        case KEY_RIGHTSHIFT: return PL_KEY_RIGHTSHIFT;
        case KEY_RIGHTALT: return PL_KEY_RIGHTALT;
        case KEY_RIGHTMETA: return PL_KEY_RIGHTMETA;
        default: return PL_KEY_COUNT;
    }
}

static PLMouseButton evcode2mb(uint32_t evcode) {
    switch (evcode) {
        case BTN_LEFT: return PL_MB_LEFT;
        case BTN_RIGHT: return PL_MB_RIGHT;
        case BTN_MIDDLE: return PL_MB_MIDDLE;
        case BTN_SIDE: return PL_MB_BACK;
        case BTN_EXTRA: return PL_MB_FORWARD;
        case BTN_FORWARD: return PL_MB_EXTRA1;
        case BTN_BACK: return PL_MB_EXTRA2;
        case BTN_TASK: return PL_MB_EXTRA3;
        default: return PL_MB_COUNT;
    }
}


// BEGIN WAYLAND LISTENERS

// BEGIN WL_REGISTRY LISTENER
struct RegistryGlobalData {
    struct wl_registry* registry;
    uint32_t name;
    const char* interface;
    uint32_t version;
};

static uint8_t try_bind_global(struct RegistryGlobalData* global,
        const struct wl_interface* interface, void** out) {
    if (strcmp(interface->name, global->interface) == 0) {
        if (*out) {
            fprintf(stderr, "Got second %s!\n", interface->name);
            return 1;
        }
        if (interface->version < global->version) {
            global->version = interface->version;
        }
        *out = wl_registry_bind(global->registry, global->name,
                interface, global->version);
        if (!(*out)) {
            fprintf(stderr, "Failed to bind to %s!\n", interface->name);
        }
        return 1;
    } else {
        return 0;
    }
}

static void wl_registry_global(void* data, struct wl_registry* registry,
        uint32_t name, const char* interface, uint32_t version) {
    StateInternal* state = data;
    assert(state->registry == registry);
    //printf("REGISTRY: got global %d: %s v%d\n", name, interface, version);
    struct RegistryGlobalData g = {
        .registry = state->registry,
        .name = name,
        .interface = interface,
        .version = version,
    };

    (void)( // avoid unused value warning
        try_bind_global(&g, &wl_compositor_interface,
            (void**)&state->compositor)
        || try_bind_global(&g, &xdg_wm_base_interface,
            (void**)&state->xdg_wm_base)
        || try_bind_global(&g, &zxdg_decoration_manager_v1_interface,
                (void**)&state->decor_manager)
        || try_bind_global(&g, &wl_seat_interface,
            (void**)&state->seat)
    );
}

static void wl_registry_global_remove(void* data, struct wl_registry* registry,
        uint32_t name) {
    printf("REGISTRY: removed global %d\n", name);
}

static struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_global,
    .global_remove = wl_registry_global_remove,
};
// END WL_REGISTRY LISTENER

// BEGIN XDG_WM_BASE LISTENER
static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base,
        uint32_t serial) {
    StateInternal* state = data;
    assert(state->xdg_wm_base == xdg_wm_base);
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};
// END XDG_WM_BASE LISTENER

// BEGIN WL_POINTER LISTENER
void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x,
        wl_fixed_t surface_y) {
    StateInternal* state = data;
    state->pointer_serial = serial;
    state->next_state->mouse.pos_x = wl_fixed_to_double(surface_x);
    state->next_state->mouse.pos_y = wl_fixed_to_double(surface_y);
    // TODO update cursor image

    WindowState* win_state = wl_proxy_get_user_data((void*)surface);
    if (win_state) {
        state->next_state->windows[win_state->window_id].has_mouse_focus = true;
        state->next_state->mouse.focus_window = win_state->window_id;
        PLEvent ev = { 0 };
        ev.type = PL_EV_MOUSE_ENTER;
        ev.window = win_state->window_id;
        push_event(state, ev);
    }
}

void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, struct wl_surface *surface) {
    StateInternal* state = data;
    state->pointer_serial = serial;

    WindowState* win_state = wl_proxy_get_user_data((void*)surface);
    if (win_state) {
        state->next_state->windows[win_state->window_id].has_mouse_focus = false;
        state->next_state->mouse.focus_window = 0;
        PLEvent ev = { 0 };
        ev.type = PL_EV_MOUSE_LEAVE;
        ev.window = win_state->window_id;
        push_event(state, ev);
    }
}

void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        wl_fixed_t surface_x, wl_fixed_t surface_y) {
    StateInternal* state = data;
    state->next_state->mouse.pos_x = wl_fixed_to_double(surface_x);
    state->next_state->mouse.pos_y = wl_fixed_to_double(surface_y);

    // TODO compute relative pointer motion if extension isn't available

    PLEvent ev = { 0 };
    ev.type = PL_EV_MOUSE_MOTION;
    ev.window = state->next_state->mouse.focus_window;
    ev.motion.x = wl_fixed_to_double(surface_x);
    ev.motion.y = wl_fixed_to_double(surface_y);
    push_event(state, ev);
}

void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t but_state) {
    StateInternal* state = data;
    state->pointer_serial = serial;

    PLMouseButton mb = evcode2mb(button);
    if (mb == PL_MB_COUNT) {
        LOG_WARNING("got invalid wl_pointer button code: %d", button);
        return;
    }

    PLEvent ev = { 0 };
    ev.window = state->next_state->mouse.focus_window;
    ev.mouse_button.button = mb;
    switch (but_state) {
        case WL_POINTER_BUTTON_STATE_RELEASED:
            ev.type = PL_EV_BUTTON_RELEASE;
            release_button(&state->next_state->mouse.buttons[mb]);
            break;
        case WL_POINTER_BUTTON_STATE_PRESSED:
            ev.type = PL_EV_BUTTON_PRESS;
            press_button(&state->next_state->mouse.buttons[mb]);
            break;
        default:
            LOG_WARNING("got invalid wl_pointer button state: %d", but_state);
            return;
    }
    push_event(state, ev);
}

// snap scroll vector to nearest axis within 'a' degrees
static void scroll_dir_snap(double* v, double* h, double tan_a) {
    *v = (*v > 0.0) ? *v : -(*v);
    *h = (*h > 0.0) ? *h : -(*h);
    *v = (tan_a * (*h) > *v) ? 0 : *v;
    *h = (tan_a * (*v) > *h) ? 0 : *h;
}

static void send_scroll_button(StateInternal* state, PLMouseButton button) {
    PLEvent ev = { 0 };
    ev.type = PL_EV_BUTTON_PRESS;
    ev.window = state->next_state->mouse.focus_window;
    ev.mouse_button.button = button;
    push_event(state, ev);
    press_button(&state->next_state->mouse.buttons[button]);
    release_button(&state->next_state->mouse.buttons[button]);
}

static void send_pointer_events(StateInternal* state) {
    uint32_t v = WL_POINTER_AXIS_VERTICAL_SCROLL;
    uint32_t h = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    PLMouseButton scroll_pos[] = { PL_MB_WHEELDOWN, PL_MB_WHEELRIGHT };
    PLMouseButton scroll_neg[] = { PL_MB_WHEELUP, PL_MB_WHEELLEFT };

    WlPointerEventGroup* events = &state->pointer_ev_group;

    state->next_state->mouse.scroll_delta_v = events->scroll[v];
    state->next_state->mouse.scroll_delta_h = events->scroll[h];
    state->next_state->mouse.scroll_inverted_v = events->scroll_dir[v];
    state->next_state->mouse.scroll_inverted_h = events->scroll_dir[h];

    if ((events->scroll[v] != 0) || (events->scroll[h] != 0)) {
        PLEvent ev = { 0 };
        ev.type = PL_EV_SCROLL;
        ev.window = state->next_state->mouse.focus_window;
        ev.scroll.v = events->scroll[v];
        ev.scroll.h = events->scroll[h];
        ev.scroll.inverted_v = events->scroll_dir[v];
        ev.scroll.inverted_h = events->scroll_dir[h];
        push_event(state, ev);
    }

    // snap scroll vector to prevent discrete scroll events from slowly
    // accumulating in directions ~adjacent to the intended scroll direction
    double tan_15 = 0.267949192431;
    scroll_dir_snap(&events->scroll[v], &events->scroll[h], tan_15);

    // send discrete scroll events
    for (uint8_t axis = 0; axis < SCROLL_AXIS_COUNT; axis++) {
        if (events->scroll120[axis] != 0.0) {
            state->scroll120_acc[axis] += events->scroll120[axis];
            while (state->scroll120_acc[axis] >= 120) {
                state->scroll120_acc[axis] -= 120;
                send_scroll_button(state, scroll_pos[axis]);
            }
            while (state->scroll120_acc[axis] <= -120) {
                state->scroll120_acc[axis] += 120;
                send_scroll_button(state, scroll_neg[axis]);
            }
            // we got discrete scroll event. no need to produce one manually
            state->scroll_acc[axis] = 0;
        } else if (events->scroll[axis] != 0.0) {
            // got no discrete scroll event. accumulate axis events and push
            // discrete scroll manually
            state->scroll_acc[axis] += events->scroll[axis] *
                state->scroll_click_scale;
            while (state->scroll_acc[axis] >= 1.0) {
                state->scroll_acc[axis] -= 1.0;
                if (state->scroll_acc[axis] < 0.0) {
                    state->scroll_acc[axis] = 0.0;
                }
                send_scroll_button(state, scroll_pos[axis]);
            }
            while (state->scroll_acc[axis] <= -1.0) {
                state->scroll_acc[axis] += 1.0;
                if (state->scroll_acc[axis] > 0.0) {
                    state->scroll_acc[axis] = 0.0;
                }
                send_scroll_button(state, scroll_neg[axis]);
            }
        }
    }
    // TODO handle source, stop and direction events
    // separate scroll events by source
    memset(events, 0, sizeof(*events));
}

void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    StateInternal* state = data;
    state->pointer_ev_group.scroll[axis] += wl_fixed_to_double(value);
    if (wl_proxy_get_version((void*)wl_pointer) < 5) {
        // <v5 no frame event. send events manually
        send_pointer_events(state);
    }
}

// since 5
void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    StateInternal* state = data;
    send_pointer_events(state);
}

// since 5
void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis_source) {
    // what "device" generated the axis event (wheel, finger, etc.)
    StateInternal* state = data;
    state->pointer_ev_group.source = axis_source;
}

// since 5
void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, uint32_t axis) {
    // called when user lifts finger stopping a scroll event
    StateInternal* state = data;
    state->pointer_ev_group.stop[axis] = 1;
}

// since 5 deprecated since 8
void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t discrete) {
    // called every "click" of the scroll wheel
    StateInternal* state = data;
    if (wl_proxy_get_version((void*)wl_pointer) >= 8) { return; }
    state->pointer_ev_group.scroll120[axis] += 120 * discrete;
}

// since 8
void wl_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t value120) {
    // replaces axis_discrete. every 120 is one click of wheel
    StateInternal* state = data;
    state->pointer_ev_group.scroll120[axis] += value120;
}

// since 9
void wl_pointer_axis_relative_direction(void *data,
        struct wl_pointer *wl_pointer, uint32_t axis, uint32_t direction) {
    // reports whether the users action (scroll, swipe, etc.) is in the same
    // direction as the scroll (reverse/natural scrolling)
    StateInternal* state = data;
    state->pointer_ev_group.scroll_dir[axis] = direction;
}

static struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
    .axis_value120 = wl_pointer_axis_value120,
    .axis_relative_direction = wl_pointer_axis_relative_direction,
};
// END WL_POINTER LISTENER

// BEGIN WL_KEYBOARD LISTENER
static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t format, int32_t fd, uint32_t size) {
    StateInternal* state = data;

    if (state->xkb_compose_state) {
        xkb_compose_state_unref(state->xkb_compose_state);
        state->xkb_compose_state = NULL;
    }
    if (state->xkb_state) {
        xkb_state_unref(state->xkb_state);
        state->xkb_state = NULL;
    }
    if (state->xkb_keymap) {
        xkb_keymap_unref(state->xkb_keymap);
        state->xkb_keymap = NULL;
    }
    if (state->keymap_raw) {
        munmap(state->keymap_raw, state->keymap_raw_size);
    }

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        LOG_ERROR("keymap not in XKB V1 format. wl_keymap_format: %d", format);
        return;
    }

    state->keymap_raw_size = size;
    state->keymap_raw = mmap(NULL, state->keymap_raw_size,
            PROT_READ, MAP_PRIVATE, fd, 0);
    if (!state->keymap_raw) {
        LOG_ERROR("failed to memory map keymap");
        return;
    }
    state->xkb_keymap = xkb_keymap_new_from_string(state->xkb_ctx,
            state->keymap_raw, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!state->xkb_keymap) {
        LOG_ERROR("failed to create xkb_keymap");
        return;
    }

    state->xkb_state = xkb_state_new(state->xkb_keymap);
    if (!state->xkb_state) {
        LOG_ERROR("failed to create xkb_state");
        return;
    }

    if (state->xkb_compose_table) {
        state->xkb_compose_state = xkb_compose_state_new(state->xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        if (!state->xkb_compose_state) {
            LOG_WARNING("failed to create xkb_compose_state");
            return;
        }
    }
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    StateInternal* state = data;
    WindowState* win_state = wl_proxy_get_user_data((void*)surface);
    PLWindow* window = &state->next_state->windows[win_state->window_id];
    if (win_state) {
        window->has_keyboard_focus = true;
        state->next_state->keyboard.focus_window = win_state->window_id;
        PLEvent ev = { 0 };
        ev.type = PL_EV_KEYBOARD_ENTER;
        ev.window = win_state->window_id;
        push_event(state, ev);
    }

    uint32_t* key = NULL;
    wl_array_for_each(key, keys) {
        PLKey pl_key = evcode2key(*key);
        state->next_state->keyboard.keys[pl_key].is_down = true;
        // "do not emulate key press events" -wl_keyboard::enter desc.
    }

}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface) {
    StateInternal* state = data;
    WindowState* win_state = wl_proxy_get_user_data((void*)surface);
    PLWindow* window = &state->next_state->windows[win_state->window_id];
    if (win_state) {
        window->has_keyboard_focus = true;
        state->next_state->keyboard.focus_window = 0;
        PLEvent ev = { 0 };
        ev.type = PL_EV_KEYBOARD_LEAVE;
        ev.window = win_state->window_id;
        push_event(state, ev);
    }
}

static size_t clean_utf8_string(char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c <= '\e' && !('\b' <= c && c <= '\r')) {
            // remove some ascii control characters
            // keep \b \t \n \v \f \r and separators
            len--;
            for (size_t j = i; j < len; j++) {
                str[j] = str[j + 1];
            }
            i--;
        }
        // enter/return produces \r on linux for some reason
        if (c == '\r') {
            str[i] = '\n';
        }
    }
    return len;
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t but_state) {
    StateInternal* state = data;
    PLKey pl_key = evcode2key(key);
    PLEvent ev = { 0 };
    ev.window = state->next_state->keyboard.focus_window;
    switch (but_state) {
        case WL_KEYBOARD_KEY_STATE_RELEASED:
            release_button(&state->next_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_RELEASE;
            push_event(state, ev);
            return;
        case WL_KEYBOARD_KEY_STATE_PRESSED:
            press_button(&state->next_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_PRESS;
            // TODO manually produce key repeat events if wl_keyboard <v10
            break;
        case WL_KEYBOARD_KEY_STATE_REPEATED: // since 10
            repeat_button(&state->next_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_REPEAT;
            break;
    }
    push_event(state, ev);

    const xkb_keysym_t* syms;
    int syms_len = xkb_state_key_get_syms(state->xkb_state, key + 8, &syms);
    uint32_t utf8_len = 0;
    char* utf8_buf = NULL;
    if (state->xkb_compose_state) {
        for (int i = 0; i < syms_len; i++) {
            xkb_compose_state_feed(state->xkb_compose_state, syms[i]);
        }
        switch (xkb_compose_state_get_status(state->xkb_compose_state)) {
            case XKB_COMPOSE_NOTHING:
                utf8_len = xkb_state_key_get_utf8(state->xkb_state, key + 8, NULL, 0) + 1;
                utf8_buf = malloc(utf8_len);
                xkb_state_key_get_utf8(state->xkb_state, key + 8, utf8_buf, utf8_len);
                break;
            case XKB_COMPOSE_COMPOSED:
                utf8_len = xkb_compose_state_get_utf8(state->xkb_compose_state, NULL, 0) + 1;
                utf8_buf =  malloc(utf8_len);
                utf8_len = xkb_compose_state_get_utf8(state->xkb_compose_state, utf8_buf, utf8_len);
                break;
            case XKB_COMPOSE_COMPOSING:
            case XKB_COMPOSE_CANCELLED:
                break;
        }
    } else {
        utf8_len = xkb_state_key_get_utf8(state->xkb_state, key + 8, NULL, 0) + 1;
        utf8_buf = malloc(utf8_len);
        xkb_state_key_get_utf8(state->xkb_state, key + 8, utf8_buf, utf8_len);
    }
    utf8_len = clean_utf8_string(utf8_buf, utf8_len);
    if (utf8_len > 0) {
        PLEvent text_ev = { 0 };
        text_ev.type = PL_EV_TEXT_INPUT;
        text_ev.window = state->next_state->keyboard.focus_window;
        text_ev.text.text = utf8_buf;
        text_ev.text.text_len = utf8_len;
        push_event(state, text_ev);
    }
    // TODO utf8_buf gets leaked. make temp alloc or something
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group) {
    StateInternal* state = data;
    if (state->xkb_state) {
        xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
                mods_locked, 0, 0, group);
    }
}

// since 4
static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
        int32_t rate, int32_t delay) {
    // this event will be resent if user changes repeat rate or delay
    // rate == 0 -> repeat disabled
    // TODO use rate and delay to manually produce key repeat events if
    // wl_keyboard <v10
}

static struct wl_keyboard_listener keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};
// END WL_KEYBOARD LISTENER

// BEGIN WL_SEAT LISTENER
static void wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
        uint32_t capabilities) {
    StateInternal* state = data;

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        if (!state->pointer) {
            state->pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(state->pointer, &pointer_listener, state);
        }
    } else {
        if (state->pointer) {
            if (wl_proxy_get_version((void*)state->pointer) >= 3) {
                wl_pointer_release(state->pointer);
                state->pointer = NULL;
            }
        }
    }

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!state->keyboard) {
            state->keyboard = wl_seat_get_keyboard(wl_seat);
            wl_keyboard_add_listener(state->keyboard,
                    &keyboard_listener, state);
        }
    } else {
        if (state->keyboard) {
            if (wl_proxy_get_version((void*)state->keyboard) >= 3) {
                wl_keyboard_release(state->keyboard);
                state->keyboard = NULL;
            }
        }
    }

    if (capabilities & WL_SEAT_CAPABILITY_TOUCH) {
        // TODO implement touch support?
    } else {
    }
}

// since 2
static void wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    // STUB
}

static struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};
// END WL_SEAT LISTENER

// BEGIN WL_SURFACE LISTENER
static void wl_surface_enter(void *data, struct wl_surface *wl_surface,
        struct wl_output *output) {
    // STUB
}

static void wl_surface_leave(void *data, struct wl_surface *wl_surface,
        struct wl_output *output) {
    // STUB
}

// since 6
static void wl_surface_preferred_buffer_scale(void *data,
        struct wl_surface *wl_surface, int32_t factor) {
    // STUB
}

// since 6
static void wl_surface_preferred_buffer_transform(void *data,
        struct wl_surface *wl_surface, uint32_t transform) {
    // STUB
}

static struct wl_surface_listener wl_surface_listener = {
    .enter = wl_surface_enter,
    .leave = wl_surface_leave,
    .preferred_buffer_scale = wl_surface_preferred_buffer_scale,
    .preferred_buffer_transform = wl_surface_preferred_buffer_transform,
};
// END WL_SURFACE LISTENER

// BEGIN XDG_SURFACE LISTENER
static void xdg_surface_configure(void* data, struct xdg_surface* xdg_surface,
        uint32_t serial) {
    WindowState* ws = data;
    assert(ws->xdg_surface == xdg_surface);
    xdg_surface_ack_configure(xdg_surface, serial);
}

static struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};
// END XDG_SURFACE LISTENER

// BEGIN XDG_TOPLEVEL LISTENER
static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
        int32_t width, int32_t height, struct wl_array* states) {
    WindowState* ws = data;
    assert(ws->toplevel == toplevel);
    PLWindow* win = &ws->state->next_state->windows[ws->window_id];
    win->width = width;
    win->height = height;
    win->maximized = false;
    win->fullscreen = false;
    enum xdg_toplevel_state* state = NULL;
    wl_array_for_each(state, states) {
        switch (*state) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:
                win->maximized = true;
                break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:
                win->fullscreen = true;
                break;
            default:
                break;
        }
    }
    // TODO window state events
}

static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel) {
    WindowState* ws = data;
    assert(ws->toplevel == toplevel);
    StateInternal* state_i = ws->state;
    PLState* state = state_i->next_state;
    PLWindow* window = &state->windows[ws->window_id];
    window->quit = true;
    PLEvent ev = { 0 };
    ev.type = PL_EV_CLOSE;
    ev.window = ws->window_id;
    push_event(state_i, ev);
}

static void xdg_toplevel_configure_bounds(void* data,
        struct xdg_toplevel* toplevel, int32_t width, int32_t height) {
    WindowState* ws = data;
    assert(ws->toplevel == toplevel);
    // STUB
}

static void xdg_toplevel_wm_capabilities(void* data,
        struct xdg_toplevel* toplevel, struct wl_array* capabilities) {
    WindowState* ws = data;
    assert(ws->toplevel == toplevel);
    // STUB
}

static struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};
// END XDG_TOPLEVEL LISTENER

// BEGIN XDG_TOPLEVEL_DECORATION LISTENER
static void xdg_toplevel_decoration_configure(void* data,
        struct zxdg_toplevel_decoration_v1* decor, uint32_t mode) {
    WindowState* ws = data;
    assert(ws->decor == decor);
    StateInternal* state_i = ws->state;
    PLState* state = state_i->next_state;
    PLWindow* window = &state->windows[ws->window_id];
    switch (mode) {
        case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
            window->no_decorations = true;
            break;
        case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
            window->no_decorations = false;
            break;
    }
    // TODO signal decor mode change event
    //PLEvent ev = { 0 };
    //ev.window = ws->window_id;
    //push_event(state_i, ev);
}

static struct zxdg_toplevel_decoration_v1_listener
xdg_toplevel_decoration_listener = {
    .configure = xdg_toplevel_decoration_configure,
};
// END XDG_TOPLEVEL_DECORATION LISTENER

// END WAYLAND LISTENERS

// this does not allocate. arrays must be set on dest before calling
static void deep_copy_state(PLState* dest, PLState* src) {
    // TODO pointers need to be deep copied (window->title)
    memcpy(dest->windows, src->windows, sizeof(windows));
    PLWindow* windows = dest->windows;
    memcpy(dest, src, sizeof(*dest));
    dest->windows = windows;
}

static void init_state_internal(StateInternal* s, PLState* state) {
    if (!s) { return; }
    if (s->initialized) { return; }
    memset(s, 0, sizeof(*s));
    memset(_window_states, 0, sizeof(_window_states));

    s->scroll_click_scale = 1.0 / 15.0;

    s->display = wl_display_connect(NULL);
    if (!s->display) {
        LOG_FATAL("failed to connect to wayland display");
        assert(0);
        return;
    }
    s->registry = wl_display_get_registry(s->display);
    if (!s->display) {
        LOG_FATAL("failed to get wl_registry");
        assert(0);
        return;
    }
    wl_registry_add_listener(s->registry, &wl_registry_listener, s);
    wl_display_roundtrip(s->display);

    if (!s->compositor) {
        LOG_FATAL("failed to get wl_compositor");
        assert(0);
        return;
    }
    if (!s->xdg_wm_base) {
        LOG_FATAL("failed to get xdg_wm_base");
        assert(0);
        return;
    }
    xdg_wm_base_add_listener(s->xdg_wm_base, &xdg_wm_base_listener, s);

    if (s->seat) {
        wl_seat_add_listener(s->seat, &wl_seat_listener, s);
    }

    s->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if (s->xkb_ctx) {
        s->xkb_compose_table = xkb_compose_table_new_from_locale(s->xkb_ctx, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (s->xkb_compose_table == NULL) {
            LOG_WARNING("Failed to get xkb_compose_table");
        }
    } else {
        LOG_ERROR("Failed to create xkb_context");
    }

    s->window_states = _window_states;

    s->curr_state = state;
    s->last_state = &s->state_buf_a;
    s->next_state = &s->state_buf_b;
    s->next_state->windows = next_windows;
    s->last_state->windows = last_windows;
    deep_copy_state(s->last_state, state);

    s->next_state->events = events;
    s->next_state->events_len = 0;
    s->initialized = true;
}

void pl_init(PLState* state, PLErrorCallback error_callback) {
    g_error_callback = error_callback;
    if (!state) { return; }
    memset(state, 0, sizeof(*state));
    state->windows = windows;

    state->events = events;
    state->events_len = 0;
    state->_internal = &_internal_state;
    init_state_internal((StateInternal*)state->_internal, state);
    state->wl_display = _internal_state.display;
}

static int poll_wayland_events(struct wl_display* display) {
    struct pollfd pfd[1];
    pfd[0].fd = wl_display_get_fd(display);
    int ret = 0;

    while (wl_display_prepare_read(display) == -1) {
        ret = wl_display_dispatch_pending(display);
        if (ret == -1) {
            return ret;
        }
    }

    ret = wl_display_flush(display);
    if (ret == -1) {
        if (errno == EAGAIN) {
            pfd[0].events = POLLOUT;
            do {
                ret = poll(pfd, 1, -1);
            } while (ret == -1 && errno == EINTR);
            if (ret == -1) {
                wl_display_cancel_read(display);
                return ret;
            }
        } else if (errno != EPIPE) { // pipe closed, error event should be read
            wl_display_cancel_read(display);
            return ret;
        }
    }

    while (true) {
        pfd[0].events = POLLIN;
        do {
            ret = poll(pfd, 1, 0);
        } while (ret == -1 && errno == EINTR);
        if (ret <= 0) {
            wl_display_cancel_read(display);
            return ret;
        }

        ret = wl_display_read_events(display);
        if (ret == -1) {
            return ret;
        }

        if (wl_display_prepare_read(display) == -1) {
            return wl_display_dispatch_pending(display);
        }
        // event queue doesn't have a full event. loop
        // again to see if more data is available.
    }
}

static WindowState* alloc_window_state(StateInternal* state) {
    uint32_t cap = sizeof(_window_states) / sizeof(_window_states[0]);
    WindowState* win;
    if (!state->next_free_window) {
        assert(state->window_states_len < cap);
        win = &state->window_states[state->window_states_len++];
        win->state = state;
        return win;
    }
    win = state->next_free_window;
    state->next_free_window = (WindowState*)win->state;
    win->state = state;
    return win;
}

static void free_window_state(WindowState* win_state) {
    if (win_state == NULL) { return; }
    StateInternal* state = win_state->state;
    win_state->state = (StateInternal*)state->next_free_window;
    state->next_free_window = win_state;
}

static void destroy_window(WindowState* win_state) {
    if (!win_state) { return; }
    if (win_state->decor) {
        zxdg_toplevel_decoration_v1_destroy(win_state->decor);
    }
    if (win_state->toplevel) {
        xdg_toplevel_destroy(win_state->toplevel);
    }
    if (win_state->xdg_surface) {
        xdg_surface_destroy(win_state->xdg_surface);
    }
    if (win_state->surface) {
        wl_surface_destroy(win_state->surface);
    }
    free_window_state(win_state);
}

void pl_deinit(PLState* state) {
    if (!state) { return; }
    StateInternal* s = state->_internal;

    uint32_t windows_len = sizeof(windows) / sizeof(windows[0]);
    for (uint32_t i = 0; i < windows_len; i++) {
        if (state->windows[i].valid && state->windows[i]._internal) {
            destroy_window(state->windows[i]._internal);
        }
    }

    if (s->xkb_state) {
        xkb_state_unref(s->xkb_state);
    }
    if (s->xkb_keymap) {
        xkb_keymap_unref(s->xkb_keymap);
    }
    if (s->xkb_ctx) {
        xkb_context_unref(s->xkb_ctx);
    }

    if (s->keyboard) {
        if (wl_proxy_get_version((void*)s->keyboard) >= 3) {
            wl_keyboard_release(s->keyboard);
        }
    }
    if (s->pointer) {
        if (wl_proxy_get_version((void*)s->pointer) >= 3) {
            wl_pointer_release(s->pointer);
        }
    }
    if (s->seat) {
        if (wl_proxy_get_version((void*)s->seat) >= 5) {
            wl_seat_release(s->seat);
        }
    }
    if (s->decor_manager) {
        zxdg_decoration_manager_v1_destroy(s->decor_manager);
    }
    if (s->xdg_wm_base) {
        xdg_wm_base_destroy(s->xdg_wm_base);
    }
    if (s->compositor) {
        wl_compositor_destroy(s->compositor);
    }
    if (s->registry) {
        wl_registry_destroy(s->registry);
    }
    if (s->display) {
        wl_display_disconnect(s->display);
    }
    return;
}

static WindowState* create_window(StateInternal* state_i) {
    WindowState* win_state = alloc_window_state(state_i);
    if (!win_state) {
        LOG_ERROR("failed to allocate new window state");
        goto error;
    }

    win_state->surface = wl_compositor_create_surface(state_i->compositor);
    if (!win_state->surface) {
        LOG_ERROR("failed to create wl_surface");
        goto error;
    }
    wl_surface_add_listener(win_state->surface, &wl_surface_listener, win_state);

    win_state->xdg_surface = xdg_wm_base_get_xdg_surface(state_i->xdg_wm_base, win_state->surface);
    if (!win_state->xdg_surface) {
        LOG_ERROR("failed to create xdg_surface");
        goto error;
    }
    xdg_surface_add_listener(win_state->xdg_surface, &xdg_surface_listener, win_state);

    win_state->toplevel = xdg_surface_get_toplevel(win_state->xdg_surface);
    if (!win_state->toplevel) {
        LOG_ERROR("failed to get xdg_toplevel");
        goto error;
    }
    xdg_toplevel_add_listener(win_state->toplevel, &xdg_toplevel_listener, win_state);

    if (state_i->decor_manager) {
        win_state->decor = zxdg_decoration_manager_v1_get_toplevel_decoration(
                state_i->decor_manager, win_state->toplevel);
        if (!win_state->decor) {
            LOG_ERROR("failed to get xdg_toplevel_decoration");
            goto error;
        }
        zxdg_toplevel_decoration_v1_add_listener(win_state->decor,
                &xdg_toplevel_decoration_listener, win_state);
        zxdg_toplevel_decoration_v1_set_mode(win_state->decor,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(win_state->surface);

    return win_state;

error:
    destroy_window(win_state);
    return NULL;
}

static char* null_to_empty(char* str) {
    if (str == NULL) {
        return "";
    } else {
        return str;
    }
}

void update_window(StateInternal* state, uint32_t id) {
    PLWindow* last = &state->last_state->windows[id];
    PLWindow* curr = &state->curr_state->windows[id];
    PLWindow* next = &state->next_state->windows[id];
    WindowState* win_state = next->_internal;
    if (!curr->valid) {
        if (last->valid) {
            destroy_window(curr->_internal);
            memset(next, 0, sizeof(*next));
        }
        return;
    }
    if (!last->valid) {
        win_state = create_window(state);
        win_state->window_id = id;
        next->_internal = win_state;
        next->wl_surface = win_state->surface;
        if (next->_internal == NULL) {
            memset(next, 0, sizeof(*next));
        }
    }
    if (win_state->toplevel) {
        char* last_title = null_to_empty(last->title);
        char* new_title = null_to_empty(curr->title);
        if (strcmp(last_title, new_title) != 0) {
            printf("differing titles\n");
            xdg_toplevel_set_title(win_state->toplevel, new_title);
        }
    }
}

void pl_update(PLState* state) {
    StateInternal* state_i = state->_internal;

    // cache changes to state
    state_i->curr_state = state;
    deep_copy_state(state_i->next_state, state);

    // clear transients
    state_i->next_state->events_len = 0;
    for (uint32_t i = 0; i < PL_MB_COUNT; i++) {
        PLButtonState* button = &state_i->next_state->mouse.buttons[i];
        button->just_pressed = false;
        button->just_released = false;
        button->repeat = false;
        button->extra_presses = 0;
    }
    for (uint32_t i = 0; i < PL_KEY_COUNT; i++) {
        PLButtonState* button = &state_i->next_state->keyboard.keys[i];
        button->just_pressed = false;
        button->just_released = false;
        button->repeat = false;
        button->extra_presses = 0;
    }

    // read events & update next_state (in callbacks)
    poll_wayland_events(state_i->display);

    // enact changes based on last -> curr state delta
    uint32_t len = sizeof(windows) / sizeof(windows[0]);
    for (uint32_t id = FIRST_WIN_ID; id < len; id++) {
        update_window(state_i, id);
    }

    // copy out new state
    deep_copy_state(state, state_i->next_state);

    // swap buffers
    PLState* last_state = state_i->last_state;
    state_i->last_state = state_i->next_state;
    state_i->next_state = last_state;
}

static uint32_t next_free_window_id(PLState* state) {
    uint32_t len = sizeof(windows) / sizeof(windows[0]);
    for (uint32_t i = FIRST_WIN_ID; i < len; i++) {
        if (!state->windows[i].valid) {
            PLWindow* win = &state->windows[i];
            memset(win, 0, sizeof(*win));
            return i;
        }
    }
    return 0;
}

uint32_t pl_open_window(PLState* state) {
    uint32_t id = next_free_window_id(state);
    PLWindow* win = &state->windows[id];
    win->valid = true;
    return id;
}
