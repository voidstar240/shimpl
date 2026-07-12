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

#define FIRST_WIN_ID ((PLWinID)1)

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

typedef struct ArenaPageHeader {
    struct ArenaPageHeader* next_page;
    size_t page_size;
} ArenaPageHeader;

static void* arena_next_page(PLArenaAlloc* arena, size_t min_size) {
    ArenaPageHeader* pre_new_page = arena->curr_page;
    ArenaPageHeader* new_page = NULL;
    if (pre_new_page) {
        new_page = pre_new_page->next_page;
    }

    // find large enough unused page
    while (new_page) {
        if (new_page->page_size - sizeof(ArenaPageHeader) >= min_size) {
            break;
        }
        pre_new_page = new_page;
        new_page = new_page->next_page;
    }

    // alloc new page if one couldn't be found
    if (!new_page) {
        size_t page_size = min_size + sizeof(ArenaPageHeader);
        page_size += arena->default_page_size - 1;
        page_size /= arena->default_page_size;
        page_size *= arena->default_page_size;
        new_page = calloc(1, page_size);
        if (!new_page) {
            return NULL;
        }
        if (pre_new_page) {
            pre_new_page->next_page = new_page;
        } else {
            arena->first_page = new_page;
        }
        arena->page_count++;
    }

    // insert new_page after curr_page
    if (pre_new_page && (pre_new_page != arena->curr_page)) {
        ArenaPageHeader* curr_next_page = arena->curr_page->next_page;
        ArenaPageHeader* new_next_page = new_page->next_page;
        arena->curr_page->next_page = new_page;
        new_page->next_page = curr_next_page;
        pre_new_page->next_page = new_next_page;
    }

    arena->curr_page = new_page;
    arena->page_usage = sizeof(ArenaPageHeader);
    return new_page;
}

static void arena_init(PLArenaAlloc* arena, size_t n_pages, size_t page_size) {
    arena->first_page = NULL;
    arena->curr_page = NULL;
    arena->default_page_size = page_size;
    arena->page_usage = 0;
    arena->align = sizeof(void*);
    arena->page_count = 0;
    arena_next_page(arena, n_pages * arena->default_page_size);
}

static void* arena_alloc(PLArenaAlloc* arena, size_t size) {
    size_t rem_cap = 0;
    if (arena->curr_page) {
        rem_cap = arena->curr_page->page_size - arena->page_usage;
    }
    if (size > rem_cap) {
        if (!arena_next_page(arena, size)) {
            return NULL;
        }
    }
    // round up to next alignment
    arena->page_usage += arena->align - 1;
    arena->page_usage /= arena->align;
    arena->page_usage *= arena->align;
    void* out_ptr = (uint8_t*)arena->curr_page + arena->page_usage;
    arena->page_usage += size;
    return out_ptr;
}

static void arena_reset(PLArenaAlloc* arena) {
    arena->curr_page = arena->first_page;
    arena->page_usage = sizeof(ArenaPageHeader);
}

static void arena_free_pages(PLArenaAlloc* arena) {
    while (arena->first_page) {
        void* next = *(void**)arena->first_page;
        free(arena->first_page);
        arena->first_page = next;
    }
    arena->curr_page = NULL;
    arena->page_usage = 0;
    arena->page_count = 0;
};

static void arena_defrag_pages(PLArenaAlloc* arena) {
    if (arena->page_count <= 1) {
        return;
    }
    ArenaPageHeader* page = arena->first_page;
    size_t total_size = 0;
    for (; page; page = page->next_page) {
        total_size += page->page_size;
    }
    arena_free_pages(arena);
    // round up to page size
    total_size += arena->default_page_size - 1;
    total_size /= arena->default_page_size;
    total_size *= arena->default_page_size;
    arena->first_page = calloc(1, total_size);
    arena->curr_page = arena->first_page;
    arena->page_usage = sizeof(ArenaPageHeader);
}

static void push_event(PLBackendState* bstate, PLEvent ev) {
    static PLEvent* last_event;
    PLEvent* ev_alloc = arena_alloc(&bstate->frame_alloc, sizeof(ev));
    if (!ev_alloc) {
        LOG_FATAL("Failed to allocate event");
        return;
    }
    *ev_alloc = ev;
    if (!bstate->curr_state->first_event) {
        bstate->curr_state->first_event = ev_alloc;
    } else {
        last_event->next = ev_alloc;
    }
    last_event = ev_alloc;
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
    LOG_WARNING("wl_registry removed global object %d\n", name);
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
static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
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

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
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

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
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

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
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

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value) {
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.scroll[axis] += wl_fixed_to_double(value);
    if (wl_proxy_get_version((void*)wl_pointer) < 5) {
        // <v5 no frame event. send events manually
        send_pointer_events(bstate);
    }
}

// since 5
static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    PLBackendState* bstate = data;
    send_pointer_events(bstate);
}

// since 5
static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis_source) {
    // what "device" generated the axis event (wheel, finger, etc.)
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.source = axis_source;
}

// since 5
static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, uint32_t axis) {
    // called when user lifts finger stopping a scroll event
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.stop[axis] = 1;
}

// since 5 deprecated since 8
static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t discrete) {
    // called every "click" of the scroll wheel
    PLBackendState* bstate = data;
    if (wl_proxy_get_version((void*)wl_pointer) >= 8) { return; }
    bstate->pointer_ev_group.scroll120[axis] += 120 * discrete;
}

// since 8
static void wl_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t value120) {
    // replaces axis_discrete. every 120 is one click of wheel
    PLBackendState* bstate = data;
    bstate->pointer_ev_group.scroll120[axis] += value120;
}

// since 9
static void wl_pointer_axis_relative_direction(void *data,
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

    xkb_keycode_t xkb_key = key + 8;
    const xkb_keysym_t* syms;
    int syms_len = xkb_state_key_get_syms(bstate->xkb_state, xkb_key, &syms);
    uint32_t utf8_len = 0;
    char* utf8_buf = NULL;
    enum xkb_compose_status status = XKB_COMPOSE_NOTHING;
    if (bstate->xkb_compose_state) {
        xkb_keysym_t sym = (syms_len > 1) ? XKB_KEY_NoSymbol : syms[0];
        xkb_compose_state_feed(bstate->xkb_compose_state, sym);
        status = xkb_compose_state_get_status(bstate->xkb_compose_state);
    }
    switch (status) {
        case XKB_COMPOSE_NOTHING:
            utf8_len = xkb_state_key_get_utf8(bstate->xkb_state, xkb_key, NULL, 0);
            utf8_buf = arena_alloc(&bstate->frame_alloc, utf8_len + 1);
            xkb_state_key_get_utf8(bstate->xkb_state, xkb_key, utf8_buf, utf8_len + 1);
            break;
        case XKB_COMPOSE_COMPOSED:
            utf8_len = xkb_compose_state_get_utf8(bstate->xkb_compose_state, NULL, 0);
            utf8_buf =  arena_alloc(&bstate->frame_alloc, utf8_len + 1);
            xkb_compose_state_get_utf8(bstate->xkb_compose_state, utf8_buf, utf8_len + 1);
            break;
        case XKB_COMPOSE_COMPOSING:
        case XKB_COMPOSE_CANCELLED:
            break;
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


static PLState* cache_state(PLState* state) {
    PLArenaAlloc* alloc = &state->_backend->frame_alloc;
    PLState* cstate = arena_alloc(alloc, sizeof(*state));
    memcpy(cstate, state, sizeof(*state));
    size_t wins_bytes = sizeof(*state->windows) * state->windows_cap;
    cstate->windows = arena_alloc(alloc, wins_bytes);
    memcpy(cstate->windows, state->windows, wins_bytes);
    for (PLWinID i = 0; i < state->windows_cap; i++) {
        if (!state->windows[i].valid) { continue; }
        if (!state->windows[i].title) { continue; }
        size_t title_len = strlen(state->windows[i].title) + 1;
        cstate->windows[i].title = arena_alloc(alloc, title_len);
        memcpy(cstate->windows[i].title, state->windows[i].title, title_len);
    }
    state->_backend->last_state = cstate;
    return cstate;
}

static void resize_windows(PLState* state, uint32_t new_cap) {
    if (new_cap == state->windows_cap) { return; }
    PLWindow* new_wins = NULL;
    if (new_cap > 0) {
        new_wins = realloc(state->windows, new_cap * sizeof(*state->windows));
        if (!new_wins) {
            LOG_FATAL("failed to reallocate windows array");
            return;
        }
        if (new_cap > state->windows_cap) {
            PLWindow* uninit_wins = new_wins + state->windows_cap;
            size_t uninit_cap = new_cap - state->windows_cap;
            memset(uninit_wins, 0, uninit_cap * sizeof(*state->windows));
        }
    } else {
        free(state->windows);
    }
    state->windows = new_wins;
    state->windows_cap = new_cap;
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

    arena_init(&bstate->frame_alloc, 1, 4096);
    arena_init(&bstate->bwin_alloc, 1, 4096);

    bstate->curr_state = state;
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
    resize_windows(state, 16);
    cache_state(state);
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
    PLBackendWindow* bwin = bstate->bwin_free_list;
    if (bwin) {
        bstate->bwin_free_list = (PLBackendWindow*)bwin->bstate;
    } else {
        bwin = arena_alloc(&bstate->bwin_alloc, sizeof(*bwin));
        if (!bwin) {
            LOG_FATAL("failed to allocate PLBackendWindow");
            return NULL;
        }
    }
    memset(bwin, 0, sizeof(*bwin));
    return bwin;
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
    PLBackendState* bstate = bwin->bstate;
    memset(bwin, 0, sizeof(*bwin));
    bwin->bstate = (PLBackendState*)bstate->bwin_free_list;
    bstate->bwin_free_list = bwin;
}

static void deinit_backend_state(PLBackendState* bstate) {
    if (!bstate) { return; }

    if (bstate->last_state) {
        for (PLWinID id = 0; id < bstate->last_state->windows_cap; id++) {
            PLWindow* win = &bstate->last_state->windows[id];
            if (win->valid) {
                destroy_backend_window(win->_backend);
            }
        }
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
    arena_free_pages(&bstate->frame_alloc);
    arena_free_pages(&bstate->bwin_alloc);
    free(bstate);
}

void pl_deinit(PLState* state) {
    if (!state) { return; }
    deinit_backend_state(state->_backend);
    resize_windows(state, 0);
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

static void update_window(PLBackendState* bstate, PLWinID id) {
    PLWindow* last = &bstate->last_state->windows[NULL_WINDOW];
    PLWindow* curr = &bstate->curr_state->windows[NULL_WINDOW];
    if (id < bstate->last_state->windows_cap) {
        last = &bstate->last_state->windows[id];
    }
    if (id < bstate->curr_state->windows_cap) {
        curr = &bstate->curr_state->windows[id];
    }
    PLBackendWindow* bwin = curr->_backend;
    if (!curr->valid) {
        if (last->valid) {
            destroy_backend_window(last->_backend);
            memset(curr, 0, sizeof(*curr));
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
    }
}

void pl_update(PLState* state) {
    PLBackendState* bstate = state->_backend;
    bstate->curr_state = state;

    // windows[0] must be 0 filled
    memset(&state->windows[NULL_WINDOW], 0, sizeof(*state->windows));

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
    bstate->curr_state->first_event = NULL;
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
    arena_reset(&bstate->frame_alloc);
    arena_defrag_pages(&bstate->frame_alloc);

    // read events & update curr_state (in callbacks)
    poll_wayland_events(bstate->display);

    // cache states to compare deltas next update
    cache_state(state);
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
    resize_windows(state, state->windows_cap * 2);
    return old_cap;
}

PLWinID pl_open_window(PLState* state) {
    PLWinID id = next_free_window_id(state);
    PLWindow* win = &state->windows[id];
    win->valid = true;
    return id;
}
