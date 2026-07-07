#include "shimpl.h"

#define WAYLAND_PROTOCOLS_IMPLEMENTATION
#include "shimpl-backend-linux.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <linux/input-event-codes.h>
#include <locale.h>

// REMOVE IF ADDING TABLET INTERFACE
const struct wl_interface zwp_tablet_tool_v2_interface = {0};

// TODO typedef window handle/id type
// TODO Should NULL_WIN_ID be -1 to avoid wasted space in windows array? but
// this breaks ZII
#define FIRST_WIN_ID 1

static PLErrorCallback g_error_callback = NULL;
static char g_error_msg_buf[1024] = {0};

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

static void resize_events(PLState* state, uint32_t new_cap) {
    PLEvent* new_events = NULL;
    if (new_cap == 0) {
        if (state->events) {
            free(state->events);
            state->events_cap = 0;
        }
        return;
    }
    new_events = calloc(new_cap, sizeof(*new_events));
    if (!new_events) {
        LOG_FATAL("Failed to allocate events array");
        return;
    }
    memset(new_events, 0, sizeof(*state->events) * new_cap);
    if (state->events) {
        memcpy(new_events, state->events, sizeof(*state->events) * state->events_len);
        free(state->events);
    }
    state->events = new_events;
    state->events_cap = new_cap;
}

static void push_event(PLBackendState* bstate, PLEvent ev) {
    if (bstate->curr_state->events_len >= bstate->curr_state->events_cap) {
        resize_events(bstate->curr_state, bstate->curr_state->events_cap * 2);
    }
    bstate->curr_state->events[bstate->curr_state->events_len++] = ev;
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
    PLBackendState* bstate = data;
    assert(bstate->registry == registry);
    struct RegistryGlobalData g = {
        .registry = bstate->registry,
        .name = name,
        .interface = interface,
        .version = version,
    };

    (void)( // avoid unused value warning
        try_bind_global(&g, &wl_compositor_interface,
            (void**)&bstate->compositor)
        || try_bind_global(&g, &xdg_wm_base_interface,
            (void**)&bstate->xdg_wm_base)
        || try_bind_global(&g, &zxdg_decoration_manager_v1_interface,
                (void**)&bstate->decor_manager)
        || try_bind_global(&g, &wl_seat_interface,
            (void**)&bstate->seat)
    );
}

static void wl_registry_global_remove(void* data, struct wl_registry* registry,
        uint32_t name) {
    // TODO remove or smth
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
    PLBackendState* bstate = data;
    assert(bstate->xdg_wm_base == xdg_wm_base);
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
    PLBackendState* bstate = data;
    bstate->pointer_serial = serial;
    bstate->curr_state->mouse.pos_x = wl_fixed_to_double(surface_x);
    bstate->curr_state->mouse.pos_y = wl_fixed_to_double(surface_y);
    // TODO update cursor image

    PLBackendWindow* bwin = wl_proxy_get_user_data((void*)surface);
    if (bwin) {
        bstate->curr_state->windows[bwin->window_id].has_mouse_focus = true;
        bstate->curr_state->mouse.focus_window = bwin->window_id;
        PLEvent ev = { 0 };
        ev.type = PL_EV_MOUSE_ENTER;
        ev.window = bwin->window_id;
        push_event(bstate, ev);
    }
}

void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, struct wl_surface *surface) {
    PLBackendState* bstate = data;
    bstate->pointer_serial = serial;

    PLBackendWindow* bwin = wl_proxy_get_user_data((void*)surface);
    if (bwin) {
        bstate->curr_state->windows[bwin->window_id].has_mouse_focus = false;
        bstate->curr_state->mouse.focus_window = 0;
        PLEvent ev = { 0 };
        ev.type = PL_EV_MOUSE_LEAVE;
        ev.window = bwin->window_id;
        push_event(bstate, ev);
    }
}

void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        wl_fixed_t surface_x, wl_fixed_t surface_y) {
    PLBackendState* bstate = data;
    bstate->curr_state->mouse.pos_x = wl_fixed_to_double(surface_x);
    bstate->curr_state->mouse.pos_y = wl_fixed_to_double(surface_y);

    // TODO compute relative pointer motion if extension isn't available

    PLEvent ev = { 0 };
    ev.type = PL_EV_MOUSE_MOTION;
    ev.window = bstate->curr_state->mouse.focus_window;
    ev.motion.x = wl_fixed_to_double(surface_x);
    ev.motion.y = wl_fixed_to_double(surface_y);
    push_event(bstate, ev);
}

void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t but_state) {
    PLBackendState* bstate = data;
    bstate->pointer_serial = serial;

    PLMouseButton mb = evcode2mb(button);
    if (mb == PL_MB_COUNT) {
        LOG_WARNING("got invalid wl_pointer button code: %d", button);
        return;
    }

    PLEvent ev = { 0 };
    ev.window = bstate->curr_state->mouse.focus_window;
    ev.mouse_button.button = mb;
    switch (but_state) {
        case WL_POINTER_BUTTON_STATE_RELEASED:
            ev.type = PL_EV_BUTTON_RELEASE;
            release_button(&bstate->curr_state->mouse.buttons[mb]);
            break;
        case WL_POINTER_BUTTON_STATE_PRESSED:
            ev.type = PL_EV_BUTTON_PRESS;
            press_button(&bstate->curr_state->mouse.buttons[mb]);
            break;
        default:
            LOG_WARNING("got invalid wl_pointer button state: %d", but_state);
            return;
    }
    push_event(bstate, ev);
}

// snap scroll vector to nearest axis within 'a' degrees
static void scroll_dir_snap(double* v, double* h, double tan_a) {
    *v = (*v > 0.0) ? *v : -(*v);
    *h = (*h > 0.0) ? *h : -(*h);
    *v = (tan_a * (*h) > *v) ? 0 : *v;
    *h = (tan_a * (*v) > *h) ? 0 : *h;
}

static void send_scroll_button(PLBackendState* bstate, PLMouseButton button) {
    PLEvent ev = { 0 };
    ev.type = PL_EV_BUTTON_PRESS;
    ev.window = bstate->curr_state->mouse.focus_window;
    ev.mouse_button.button = button;
    push_event(bstate, ev);
    press_button(&bstate->curr_state->mouse.buttons[button]);
    release_button(&bstate->curr_state->mouse.buttons[button]);
}

static void send_pointer_events(PLBackendState* bstate) {
    uint32_t v = WL_POINTER_AXIS_VERTICAL_SCROLL;
    uint32_t h = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    PLMouseButton scroll_pos[] = { PL_MB_WHEELDOWN, PL_MB_WHEELRIGHT };
    PLMouseButton scroll_neg[] = { PL_MB_WHEELUP, PL_MB_WHEELLEFT };

    WlPointerEventGroup* events = &bstate->pointer_ev_group;

    bstate->curr_state->mouse.scroll_delta_v = events->scroll[v];
    bstate->curr_state->mouse.scroll_delta_h = events->scroll[h];
    bstate->curr_state->mouse.scroll_inverted_v = events->scroll_dir[v];
    bstate->curr_state->mouse.scroll_inverted_h = events->scroll_dir[h];

    if ((events->scroll[v] != 0) || (events->scroll[h] != 0)) {
        PLEvent ev = { 0 };
        ev.type = PL_EV_SCROLL;
        ev.window = bstate->curr_state->mouse.focus_window;
        ev.scroll.v = events->scroll[v];
        ev.scroll.h = events->scroll[h];
        ev.scroll.inverted_v = events->scroll_dir[v];
        ev.scroll.inverted_h = events->scroll_dir[h];
        push_event(bstate, ev);
    }

    // snap scroll vector to prevent discrete scroll events from slowly
    // accumulating in directions ~adjacent to the intended scroll direction
    double tan_15 = 0.267949192431;
    scroll_dir_snap(&events->scroll[v], &events->scroll[h], tan_15);

    // send discrete scroll events
    for (uint8_t axis = 0; axis < SCROLL_AXIS_COUNT; axis++) {
        if (events->scroll120[axis] != 0.0) {
            bstate->scroll120_acc[axis] += events->scroll120[axis];
            while (bstate->scroll120_acc[axis] >= 120) {
                bstate->scroll120_acc[axis] -= 120;
                send_scroll_button(bstate, scroll_pos[axis]);
            }
            while (bstate->scroll120_acc[axis] <= -120) {
                bstate->scroll120_acc[axis] += 120;
                send_scroll_button(bstate, scroll_neg[axis]);
            }
            // we got discrete scroll event. no need to produce one manually
            bstate->scroll_acc[axis] = 0;
        } else if (events->scroll[axis] != 0.0) {
            // got no discrete scroll event. accumulate axis events and push
            // discrete scroll manually
            bstate->scroll_acc[axis] += events->scroll[axis] *
                bstate->scroll_click_scale;
            while (bstate->scroll_acc[axis] >= 1.0) {
                bstate->scroll_acc[axis] -= 1.0;
                if (bstate->scroll_acc[axis] < 0.0) {
                    bstate->scroll_acc[axis] = 0.0;
                }
                send_scroll_button(bstate, scroll_pos[axis]);
            }
            while (bstate->scroll_acc[axis] <= -1.0) {
                bstate->scroll_acc[axis] += 1.0;
                if (bstate->scroll_acc[axis] > 0.0) {
                    bstate->scroll_acc[axis] = 0.0;
                }
                send_scroll_button(bstate, scroll_neg[axis]);
            }
        }
    }
    // TODO handle source, stop and direction events
    // separate scroll events by source
    memset(events, 0, sizeof(*events));
}

void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.scroll[axis] += wl_fixed_to_double(value);
    if (wl_proxy_get_version((void*)wl_pointer) < 5) {
        // <v5 no frame event. send events manually
        send_pointer_events(bstate);
    }
}

// since 5
void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    PLBackendState* bstate = data;
    send_pointer_events(bstate);
}

// since 5
void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis_source) {
    // what "device" generated the axis event (wheel, finger, etc.)
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.source = axis_source;
}

// since 5
void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, uint32_t axis) {
    // called when user lifts finger stopping a scroll event
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.stop[axis] = 1;
}

// since 5 deprecated since 8
void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t discrete) {
    // called every "click" of the scroll wheel
    PLBackendState* bstate = data;
    if (wl_proxy_get_version((void*)wl_pointer) >= 8) { return; }
    bstate->pointer_ev_group.scroll120[axis] += 120 * discrete;
}

// since 8
void wl_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t value120) {
    // replaces axis_discrete. every 120 is one click of wheel
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.scroll120[axis] += value120;
}

// since 9
void wl_pointer_axis_relative_direction(void *data,
        struct wl_pointer *wl_pointer, uint32_t axis, uint32_t direction) {
    // reports whether the users action (scroll, swipe, etc.) is in the same
    // direction as the scroll (reverse/natural scrolling)
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.scroll_dir[axis] = direction;
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
    PLBackendState* bstate = data;

    if (bstate->xkb_compose_state) {
        xkb_compose_state_unref(bstate->xkb_compose_state);
        bstate->xkb_compose_state = NULL;
    }
    if (bstate->xkb_state) {
        xkb_state_unref(bstate->xkb_state);
        bstate->xkb_state = NULL;
    }
    if (bstate->xkb_keymap) {
        xkb_keymap_unref(bstate->xkb_keymap);
        bstate->xkb_keymap = NULL;
    }
    if (bstate->keymap_raw) {
        munmap(bstate->keymap_raw, bstate->keymap_raw_size);
    }

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        LOG_ERROR("keymap not in XKB V1 format. wl_keymap_format: %d", format);
        return;
    }

    bstate->keymap_raw_size = size;
    bstate->keymap_raw = mmap(NULL, bstate->keymap_raw_size,
            PROT_READ, MAP_PRIVATE, fd, 0);
    if (!bstate->keymap_raw) {
        LOG_ERROR("failed to memory map keymap");
        return;
    }
    bstate->xkb_keymap = xkb_keymap_new_from_string(bstate->xkb_ctx,
            bstate->keymap_raw, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!bstate->xkb_keymap) {
        LOG_ERROR("failed to create xkb_keymap");
        return;
    }

    bstate->xkb_state = xkb_state_new(bstate->xkb_keymap);
    if (!bstate->xkb_state) {
        LOG_ERROR("failed to create xkb_state");
        return;
    }

    if (bstate->xkb_compose_table) {
        bstate->xkb_compose_state = xkb_compose_state_new(bstate->xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        if (!bstate->xkb_compose_state) {
            LOG_WARNING("failed to create xkb_compose_state");
            return;
        }
    }
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    PLBackendState* bstate = data;
    PLBackendWindow* bwin = wl_proxy_get_user_data((void*)surface);
    PLWindow* window = &bstate->curr_state->windows[bwin->window_id];
    if (bwin) {
        window->has_keyboard_focus = true;
        bstate->curr_state->keyboard.focus_window = bwin->window_id;
        PLEvent ev = { 0 };
        ev.type = PL_EV_KEYBOARD_ENTER;
        ev.window = bwin->window_id;
        push_event(bstate, ev);
    }

    uint32_t* key = NULL;
    wl_array_for_each(key, keys) {
        PLKey pl_key = evcode2key(*key);
        bstate->curr_state->keyboard.keys[pl_key].is_down = true;
        // "do not emulate key press events" -wl_keyboard::enter desc.
    }
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface) {
    PLBackendState* bstate = data;
    PLBackendWindow* bwin = wl_proxy_get_user_data((void*)surface);
    PLWindow* window = &bstate->curr_state->windows[bwin->window_id];
    if (bwin) {
        window->has_keyboard_focus = true;
        bstate->curr_state->keyboard.focus_window = 0;
        PLEvent ev = { 0 };
        ev.type = PL_EV_KEYBOARD_LEAVE;
        ev.window = bwin->window_id;
        push_event(bstate, ev);
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
    PLBackendState* bstate = data;
    PLKey pl_key = evcode2key(key);
    PLEvent ev = { 0 };
    ev.window = bstate->curr_state->keyboard.focus_window;
    switch (but_state) {
        case WL_KEYBOARD_KEY_STATE_RELEASED:
            release_button(&bstate->curr_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_RELEASE;
            push_event(bstate, ev);
            return;
        case WL_KEYBOARD_KEY_STATE_PRESSED:
            press_button(&bstate->curr_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_PRESS;
            // TODO manually produce key repeat events if wl_keyboard <v10
            break;
        case WL_KEYBOARD_KEY_STATE_REPEATED: // since 10
            repeat_button(&bstate->curr_state->keyboard.keys[pl_key]);
            ev.type = PL_EV_KEY_REPEAT;
            break;
    }
    push_event(bstate, ev);

    const xkb_keysym_t* syms;
    int syms_len = xkb_state_key_get_syms(bstate->xkb_state, key + 8, &syms);
    uint32_t utf8_len = 0;
    char* utf8_buf = NULL;
    if (bstate->xkb_compose_state) {
        for (int i = 0; i < syms_len; i++) {
            xkb_compose_state_feed(bstate->xkb_compose_state, syms[i]);
        }
        switch (xkb_compose_state_get_status(bstate->xkb_compose_state)) {
            case XKB_COMPOSE_NOTHING:
                utf8_len = xkb_state_key_get_utf8(bstate->xkb_state, key + 8, NULL, 0) + 1;
                utf8_buf = malloc(utf8_len);
                xkb_state_key_get_utf8(bstate->xkb_state, key + 8, utf8_buf, utf8_len);
                break;
            case XKB_COMPOSE_COMPOSED:
                utf8_len = xkb_compose_state_get_utf8(bstate->xkb_compose_state, NULL, 0) + 1;
                utf8_buf =  malloc(utf8_len);
                utf8_len = xkb_compose_state_get_utf8(bstate->xkb_compose_state, utf8_buf, utf8_len);
                break;
            case XKB_COMPOSE_COMPOSING:
            case XKB_COMPOSE_CANCELLED:
                break;
        }
    } else {
        utf8_len = xkb_state_key_get_utf8(bstate->xkb_state, key + 8, NULL, 0) + 1;
        utf8_buf = malloc(utf8_len);
        xkb_state_key_get_utf8(bstate->xkb_state, key + 8, utf8_buf, utf8_len);
    }
    utf8_len = clean_utf8_string(utf8_buf, utf8_len);
    if (utf8_len > 0) {
        PLEvent text_ev = { 0 };
        text_ev.type = PL_EV_TEXT_INPUT;
        text_ev.window = bstate->curr_state->keyboard.focus_window;
        text_ev.text.text = utf8_buf;
        text_ev.text.text_len = utf8_len;
        push_event(bstate, text_ev);
    }
    // TODO utf8_buf gets leaked. make temp alloc or something
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group) {
    PLBackendState* bstate = data;
    if (bstate->xkb_state) {
        xkb_state_update_mask(bstate->xkb_state, mods_depressed, mods_latched,
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
    PLBackendState* bstate = data;

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        if (!bstate->pointer) {
            bstate->pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(bstate->pointer, &pointer_listener, bstate);
        }
    } else {
        if (bstate->pointer) {
            if (wl_proxy_get_version((void*)bstate->pointer) >= 3) {
                wl_pointer_release(bstate->pointer);
                bstate->pointer = NULL;
            }
        }
    }

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!bstate->keyboard) {
            bstate->keyboard = wl_seat_get_keyboard(wl_seat);
            wl_keyboard_add_listener(bstate->keyboard,
                    &keyboard_listener, bstate);
        }
    } else {
        if (bstate->keyboard) {
            if (wl_proxy_get_version((void*)bstate->keyboard) >= 3) {
                wl_keyboard_release(bstate->keyboard);
                bstate->keyboard = NULL;
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
    PLBackendWindow* bwin = data;
    assert(bwin->xdg_surface == xdg_surface);
    xdg_surface_ack_configure(xdg_surface, serial);
}

static struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};
// END XDG_SURFACE LISTENER

// BEGIN XDG_TOPLEVEL LISTENER
static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
        int32_t width, int32_t height, struct wl_array* states) {
    PLBackendWindow* bwin = data;
    assert(bwin->toplevel == toplevel);
    PLWindow* win = &bwin->bstate->curr_state->windows[bwin->window_id];
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
    PLBackendWindow* bwin = data;
    assert(bwin->toplevel == toplevel);
    PLBackendState* bstate = bwin->bstate;
    PLState* state = bstate->curr_state;
    PLWindow* window = &state->windows[bwin->window_id];
    window->quit = true;
    PLEvent ev = { 0 };
    ev.type = PL_EV_CLOSE;
    ev.window = bwin->window_id;
    push_event(bstate, ev);
}

static void xdg_toplevel_configure_bounds(void* data,
        struct xdg_toplevel* toplevel, int32_t width, int32_t height) {
    PLBackendWindow* bwin = data;
    assert(bwin->toplevel == toplevel);
    // STUB
}

static void xdg_toplevel_wm_capabilities(void* data,
        struct xdg_toplevel* toplevel, struct wl_array* capabilities) {
    PLBackendWindow* bwin = data;
    assert(bwin->toplevel == toplevel);
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
    PLBackendWindow* bwin = data;
    assert(bwin->decor == decor);
    PLBackendState* bstate = bwin->bstate;
    PLState* state = bstate->curr_state;
    PLWindow* window = &state->windows[bwin->window_id];
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

static void deep_copy_state(PLState* dest, PLState* src) {
    if (dest->windows && src->windows) {
        for (PLWinID i = 0; i < dest->windows_cap; i++) {
            char* title = dest->windows[i].title;
            memcpy(&dest->windows[i], &src->windows[i], sizeof(*src->windows));
            dest->windows[i].title = title;
        }
    }
    PLWindow* windows = dest->windows;
    memcpy(dest, src, sizeof(*dest));
    dest->windows = windows;
}

static void resize_windows(PLBackendState* bstate, uint32_t new_cap) {
    PLWindow* curr_wins = bstate->curr_state->windows;
    PLWindow* last_wins = bstate->last_state->windows;
    PLWindow* new_curr_wins = NULL;
    PLWindow* new_last_wins = NULL;
    uint32_t curr_cap = bstate->curr_state->windows_cap;
    uint32_t curr_bytes = sizeof(*curr_wins) * curr_cap;
    if (new_cap > 0) {
        new_curr_wins = calloc(new_cap * 2, sizeof(*curr_wins));
        if (!new_curr_wins) {
            LOG_FATAL("Failed to allocate windows array");
            return;
        }
        new_last_wins = new_curr_wins + new_cap;
        if (last_wins) {
            memcpy(new_last_wins, last_wins, curr_bytes);
        }
        if (curr_wins) {
            memcpy(new_curr_wins, curr_wins, curr_bytes);
        }
    }
    if (curr_wins) {
        free(curr_wins);
    }
    bstate->curr_state->windows = new_curr_wins;
    bstate->last_state->windows = new_last_wins;
    bstate->curr_state->windows_cap = new_cap;
    bstate->last_state->windows_cap = new_cap;
}

static void init_backend_state(PLBackendState* bstate, PLState* state) {
    if (!bstate) { return; }
    memset(bstate, 0, sizeof(*bstate));

    bstate->scroll_click_scale = 1.0 / 15.0;

    bstate->display = wl_display_connect(NULL);
    if (!bstate->display) {
        LOG_FATAL("failed to connect to wayland display");
        return;
    }
    bstate->registry = wl_display_get_registry(bstate->display);
    if (!bstate->display) {
        LOG_FATAL("failed to get wl_registry");
        return;
    }
    wl_registry_add_listener(bstate->registry, &wl_registry_listener, bstate);
    wl_display_roundtrip(bstate->display);

    if (!bstate->compositor) {
        LOG_FATAL("failed to get wl_compositor");
        return;
    }
    if (!bstate->xdg_wm_base) {
        LOG_FATAL("failed to get xdg_wm_base");
        return;
    }
    xdg_wm_base_add_listener(bstate->xdg_wm_base, &xdg_wm_base_listener, bstate);

    if (bstate->seat) {
        wl_seat_add_listener(bstate->seat, &wl_seat_listener, bstate);
    }

    bstate->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if (bstate->xkb_ctx) {
        bstate->xkb_compose_table = xkb_compose_table_new_from_locale(bstate->xkb_ctx, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (bstate->xkb_compose_table == NULL) {
            LOG_WARNING("Failed to get xkb_compose_table");
        }
    } else {
        LOG_ERROR("Failed to create xkb_context");
    }

    bstate->curr_state = state;
    // TODO last_state (and all child data) can be temp allocated since it is
    // only read after the deep copy each update not written to
    bstate->last_state = calloc(1, sizeof(*bstate->last_state));
    if (!bstate->last_state) {
        LOG_FATAL("failed to allocate last_state");
        return;
    }
    resize_windows(bstate, 16);
}

void pl_init(PLState* state, PLErrorCallback error_callback) {
    g_error_callback = error_callback;
    if (!state) { return; }
    memset(state, 0, sizeof(*state));
    state->_backend = calloc(1, sizeof(*state->_backend));
    if (!state->_backend) {
        LOG_FATAL("Failed to allocate backend state");
        return;
    }
    init_backend_state(state->_backend, state);
    resize_events(state, 128);
    deep_copy_state(state->_backend->last_state, state);
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

static PLBackendWindow* alloc_backend_window(PLBackendState* bstate) {
    PLBackendWindow* bwin = calloc(1, sizeof(*bwin));
    if (!bwin) {
        LOG_FATAL("failed to allocate PLBackendWindow");
        return NULL;
    }
    return bwin;
}

static void free_backend_window(PLBackendWindow* bwin) {
    if (bwin == NULL) { return; }
    free(bwin);
}

static void destroy_backend_window(PLBackendWindow* bwin) {
    if (!bwin) { return; }
    if (bwin->decor) {
        zxdg_toplevel_decoration_v1_destroy(bwin->decor);
    }
    if (bwin->toplevel) {
        xdg_toplevel_destroy(bwin->toplevel);
    }
    if (bwin->xdg_surface) {
        xdg_surface_destroy(bwin->xdg_surface);
    }
    if (bwin->surface) {
        wl_surface_destroy(bwin->surface);
    }
    free_backend_window(bwin);
}

static void close_window(PLBackendState* bstate, PLWinID win) {
    if (win < 0) { return; }
    PLWindow* curr_win = &bstate->curr_state->windows[win];
    PLWindow* last_win = &bstate->last_state->windows[win];
    destroy_backend_window(bstate->curr_state->windows[win]._backend);
    if (last_win->title) {
        free(last_win->title);
    }
    memset(curr_win, 0, sizeof(*curr_win));
    memset(last_win, 0, sizeof(*last_win));
}

void deinit_backend_state(PLBackendState* bstate) {
    if (!bstate) { return; }

    for (PLWinID id = 0; id < bstate->curr_state->windows_cap; id++) {
        close_window(bstate, id);
    }

    if (bstate->xkb_state) {
        xkb_state_unref(bstate->xkb_state);
    }
    if (bstate->xkb_keymap) {
        xkb_keymap_unref(bstate->xkb_keymap);
    }
    if (bstate->xkb_ctx) {
        xkb_context_unref(bstate->xkb_ctx);
    }

    if (bstate->keyboard) {
        if (wl_proxy_get_version((void*)bstate->keyboard) >= 3) {
            wl_keyboard_release(bstate->keyboard);
        }
    }
    if (bstate->pointer) {
        if (wl_proxy_get_version((void*)bstate->pointer) >= 3) {
            wl_pointer_release(bstate->pointer);
        }
    }
    if (bstate->seat) {
        if (wl_proxy_get_version((void*)bstate->seat) >= 5) {
            wl_seat_release(bstate->seat);
        }
    }
    if (bstate->decor_manager) {
        zxdg_decoration_manager_v1_destroy(bstate->decor_manager);
    }
    if (bstate->xdg_wm_base) {
        xdg_wm_base_destroy(bstate->xdg_wm_base);
    }
    if (bstate->compositor) {
        wl_compositor_destroy(bstate->compositor);
    }
    if (bstate->registry) {
        wl_registry_destroy(bstate->registry);
    }
    if (bstate->display) {
        wl_display_disconnect(bstate->display);
    }
    if (bstate->last_state) {
        free(bstate->last_state);
    }
    resize_windows(bstate, 0);
    free(bstate);
}

void pl_deinit(PLState* state) {
    if (!state) { return; }
    deinit_backend_state(state->_backend);
    resize_events(state, 0);
    memset(state, 0, sizeof(*state));
}

static PLBackendWindow* create_window(PLBackendState* bstate) {
    PLBackendWindow* bwin = alloc_backend_window(bstate);
    if (!bwin) {
        LOG_ERROR("failed to allocate new window state");
        goto error;
    }
    bwin->bstate = bstate;

    bwin->surface = wl_compositor_create_surface(bstate->compositor);
    if (!bwin->surface) {
        LOG_ERROR("failed to create wl_surface");
        goto error;
    }
    wl_surface_add_listener(bwin->surface, &wl_surface_listener, bwin);

    bwin->xdg_surface = xdg_wm_base_get_xdg_surface(bstate->xdg_wm_base, bwin->surface);
    if (!bwin->xdg_surface) {
        LOG_ERROR("failed to create xdg_surface");
        goto error;
    }
    xdg_surface_add_listener(bwin->xdg_surface, &xdg_surface_listener, bwin);

    bwin->toplevel = xdg_surface_get_toplevel(bwin->xdg_surface);
    if (!bwin->toplevel) {
        LOG_ERROR("failed to get xdg_toplevel");
        goto error;
    }
    xdg_toplevel_add_listener(bwin->toplevel, &xdg_toplevel_listener, bwin);

    if (bstate->decor_manager) {
        bwin->decor = zxdg_decoration_manager_v1_get_toplevel_decoration(
                bstate->decor_manager, bwin->toplevel);
        if (!bwin->decor) {
            LOG_ERROR("failed to get xdg_toplevel_decoration");
            goto error;
        }
        zxdg_toplevel_decoration_v1_add_listener(bwin->decor,
                &xdg_toplevel_decoration_listener, bwin);
        zxdg_toplevel_decoration_v1_set_mode(bwin->decor,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(bwin->surface);
    return bwin;

error:
    destroy_backend_window(bwin);
    return NULL;
}

static char* null_to_empty(char* str) {
    if (str == NULL) {
        return "";
    } else {
        return str;
    }
}

void update_window(PLBackendState* bstate, PLWinID id) {
    PLWindow* last = &bstate->last_state->windows[id];
    PLWindow* curr = &bstate->curr_state->windows[id];
    PLBackendWindow* bwin = curr->_backend;
    if (!curr->valid) {
        if (last->valid) {
            close_window(bstate, id);
        }
        return;
    }
    if (!last->valid) {
        bwin = create_window(bstate);
        if (!bwin) {
            memset(curr, 0, sizeof(*curr));
            return;
        }
        bwin->window_id = id;
        curr->_backend = bwin;
    }
    char* last_title = null_to_empty(last->title);
    char* new_title = null_to_empty(curr->title);
    if (strcmp(last_title, new_title) != 0) {
        if (bwin->toplevel) {
            xdg_toplevel_set_title(bwin->toplevel, new_title);
        }
        if (last->title) {
            free(last->title);
            last->title = NULL;
        }
        if (curr->title) {
            size_t len = strlen(curr->title) + 1;
            last->title = calloc(len, 1);
            memcpy(last->title, curr->title, len);
        }
    }
}

void pl_update(PLState* state) {
    PLBackendState* bstate = state->_backend;
    bstate->curr_state = state;

    // enact changes based on last -> curr state delta
    uint32_t max_cap = state->windows_cap;
    if (bstate->last_state->windows_cap > max_cap) {
        max_cap = bstate->last_state->windows_cap;
    }
    for (PLWinID id = FIRST_WIN_ID; id < max_cap; id++) {
        update_window(bstate, id);
    }

    // TODO separate this or smth
    // clear transients
    bstate->curr_state->events_len = 0;
    for (uint32_t i = 0; i < PL_MB_COUNT; i++) {
        PLButtonState* button = &bstate->curr_state->mouse.buttons[i];
        button->just_pressed = false;
        button->just_released = false;
        button->repeat = false;
        button->extra_presses = 0;
    }
    for (uint32_t i = 0; i < PL_KEY_COUNT; i++) {
        PLButtonState* button = &bstate->curr_state->keyboard.keys[i];
        button->just_pressed = false;
        button->just_released = false;
        button->repeat = false;
        button->extra_presses = 0;
    }

    // read events & update curr_state (in callbacks)
    poll_wayland_events(bstate->display);

    // set last state to current state
    deep_copy_state(bstate->last_state, bstate->curr_state);
}

static PLWinID next_free_window_id(PLState* state) {
    for (PLWinID i = FIRST_WIN_ID; i < state->windows_cap; i++) {
        // _backend must be null to avoid returning a window that was closed
        // (valid = false) but not yet freed in the next pl_update
        if (!state->windows[i].valid && !state->windows[i]._backend) {
            PLWindow* win = &state->windows[i];
            memset(win, 0, sizeof(*win));
            return i;
        }
    }
    PLWinID old_cap = state->windows_cap;
    resize_windows(state->_backend, state->windows_cap * 2);
    return old_cap;
}

PLWinID pl_open_window(PLState* state) {
    PLWinID id = next_free_window_id(state);
    PLWindow* win = &state->windows[id];
    win->valid = true;
    return id;
}
