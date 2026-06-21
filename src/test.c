#include <stdio.h>

#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdlib.h>
#include <string.h>

#include "shimpl.h"

static EGLDisplay egl_display;
static EGLConfig egl_config;
static EGLSurface egl_surface;
static EGLContext egl_context;
static struct wl_egl_window* egl_window;
static int32_t width = 640, height = 480;

void init_egl(struct wl_display* display, struct wl_surface* surface);

char text[1024*10] = {0};
uint32_t text_len = 0;

bool input_mode = false;

void err_cb(uint8_t sev, const char* msg, const char* file, uint32_t line) {
    const char* sev_str[] = { "WARNING", "ERROR", "FATAL"};
    fprintf(stderr, "[%s]: %s@%d: %s\n", sev_str[sev], file, line, msg);
}

int main(int argc, char* argv[]) {
    printf("State Size  = %zu bytes\n", sizeof(PLState));
    printf("Window Size = %zu bytes\n", sizeof(PLWindow));
    printf("Event Size  = %zu bytes\n", sizeof(PLEvent));
    PLState state;
    pl_init(&state, err_cb);

    uint32_t win = pl_open_window(&state);
    state.windows[win].title = "ShimPL Test";
    pl_update(&state);

    init_egl(state.wl_display, state.windows[win].wl_surface);

    eglSwapInterval(egl_display, 1);
    while (1) {
        pl_update(&state);

        for (int i = 0; i < state.events_len; i++) {
            if (state.events[i].type == PL_EV_MOUSE_MOTION) {
                continue;
            }
            //printf("EV: %d, win %d\n", state.events[i].type, state.events[i].window);
            if (state.events[i].type == PL_EV_TEXT_INPUT && input_mode) {
                if (state.events[i].text.text[0] == '\b') {
                    text[text_len] = '\0';
                    if (text_len > 0) {
                        text_len--;
                    }
                } else if (state.events[i].text.text[0] == '\n') {
                    printf("Exiting input mode...\n");
                } else {
                    memcpy(text + text_len, state.events[i].text.text, state.events[i].text.text_len);
                    text_len += state.events[i].text.text_len;
                    //printf("char: %d\n", state.events[i].text.text[0]);
                }
                printf("Text Buffer: %.*s\n", text_len, text);
            }
        }

        if (state.windows[win].quit) {
            break;
        }

        if (state.keyboard.keys[PL_KEY_ENTER].just_pressed) {
            if (!input_mode) {
                input_mode = true;
                memset(text, 0, sizeof(text));
                text_len = 0;
                printf("Entering input mode...\n");
            } else {
                input_mode = false;
            }
        }

        //if (state.keyboard.keys[PL_KEY_W].is_down) {
        //    printf("FWD\n");
        //}
        //if (state.keyboard.keys[PL_KEY_LEFTSHIFT].just_pressed) {
        //    printf("SPRINT\n");
        //}
        if (state.mouse.buttons[PL_MB_LEFT].just_released) {
            printf("RECOIL!\n");
        }
        if (state.mouse.buttons[PL_MB_BACK].just_pressed) {
            printf("PAGE BACK!\n");
        }
        if (state.mouse.buttons[PL_MB_FORWARD].just_pressed) {
            printf("PAGE FORWARD!\n");
        }
        if (state.mouse.buttons[PL_MB_WHEELUP].just_pressed) {
            printf("NEXT!\n");
        }
        if (state.mouse.buttons[PL_MB_WHEELDOWN].just_released) {
            printf("PREV!\n");
        }

        if (width != state.windows[win].width
                || height != state.windows[win].height) {
            width = state.windows[win].width;
            height = state.windows[win].height;
            wl_egl_window_resize(egl_window, width, height, 0, 0);
        }

        glClearColor(0.0, 0.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();

        if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
            printf("Failed to swap buffers\n");
        }
    }

    pl_deinit(&state);
    return 0;
}

void init_egl(struct wl_display* display, struct wl_surface* surface) {
    if (display == NULL) {
        printf("display NULL\n");
    }
    if (surface == NULL) {
        printf("surface NULL\n");
    }
    egl_display = eglGetPlatformDisplay(
            EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        printf("Failed to get EGL display %d\n", eglGetError());
        exit(1);
    }

    EGLint major, minor;
    if (eglInitialize(egl_display, &major, &minor) != EGL_TRUE) {
        printf("Failed to init EGL\n");
        exit(1);
    }
    printf("Initialized EGL %d.%d\n", major, minor);

    EGLint count;
    eglGetConfigs(egl_display, NULL, 0, &count);
    printf("Got %d egl configs\n", count);

    EGLConfig* configs;
    configs = calloc(count, sizeof(*configs));
    EGLint n;
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE,
    };
    eglChooseConfig(egl_display, config_attribs, configs, count, &n);
    EGLint size;
    for (int i = 0; i < n; i++) {
        eglGetConfigAttrib(egl_display, configs[i],
                EGL_BUFFER_SIZE, &size);
        eglGetConfigAttrib(egl_display, configs[i], EGL_RED_SIZE, &size);
    }
    egl_config = configs[0];
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, egl_config,
            EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        printf("failed to create context\n");
        exit(1);
    }

    egl_window = wl_egl_window_create(surface, width, height);
    if (egl_window == EGL_NO_SURFACE) {
        printf("failed to create egl window\n");
        exit(1);
    }

    egl_surface = eglCreateWindowSurface(egl_display,
            egl_config, egl_window, NULL);

    if (eglMakeCurrent(egl_display, egl_surface,
                egl_surface, egl_context) != EGL_TRUE) {
        printf("Failed to make EGL context current\n");
        exit(1);
    }

    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();

    if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
        printf("Failed to swap buffers\n");
        exit(1);
    }
}
