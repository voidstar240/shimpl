#ifndef SHIMPL_BACKEND_LINUX_H
#define SHIMPL_BACKEND_LINUX_H

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <wayland-client.h>
#include <cursor-shape.h>
#include <pointer-constraints.h>
#include <xdg-shell.h>
#include <xdg-decoration.h>

typedef uint32_t PLWinID;
typedef struct PLBackendState PLBackendState;
typedef struct PLBackendWindow PLBackendWindow;

struct PLBackendWindow {
    PLBackendState* bstate;
    struct wl_surface* surface;
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* toplevel;
    struct zxdg_toplevel_decoration_v1* decor;
    PLWinID window_id;
};

#define SCROLL_AXIS_COUNT 2
typedef struct {
    double scroll[SCROLL_AXIS_COUNT];
    int32_t scroll120[SCROLL_AXIS_COUNT];
    uint32_t source;
    uint8_t stop[SCROLL_AXIS_COUNT];
    uint8_t scroll_dir[SCROLL_AXIS_COUNT];
} WlPointerEventGroup;

struct PLBackendState {
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

    struct PLState* last_state; // last state sent to user
    struct PLState* curr_state; // current state containing user changes
};

#endif /* SHIMPL_BACKEND_LINUX */
