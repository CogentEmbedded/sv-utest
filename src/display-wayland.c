/*******************************************************************************
 *
 * Display support (Wayland-client)
 *
 *
 * Copyright (c) 2017 Cogent Embedded Inc. ALL RIGHTS RESERVED.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *******************************************************************************/

#define MODULE_TAG                      DISPLAY_WL

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/epoll.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libdrm/drm_fourcc.h>

#include <cairo-gl.h>
#include <math.h>

#include "main.h"
#include "app.h"
#include "common.h"
#include "display.h"
#include "display-wayland.h"
#include "egl_renesas.h"
#include "event.h"
#include "vsink.h"


/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(EVENT, 1);
TRACE_TAG(DEBUG, 1);

/*******************************************************************************
 * Local typedefs
 ******************************************************************************/

typedef void* texture_platform;

/* ...output device data */
typedef struct output_data
{
    /* ...list node */
    struct wl_list link;

    /* ...Wayland output device handle */
    struct wl_output *output;

    /* ...current output device width / height */
    u32 width, height;

    /* ...rotation value */
    u32 transform;

} output_data_t;

/* ...input device data */
typedef struct input_data
{
    /* ...list node */
    struct wl_list link;

    /* ...Wayland seat handle */
    struct wl_seat *seat;

    /* ...seat capabilities */
    u32 caps;

    /* ...pointer device interface */
    struct wl_pointer *pointer;

    /* ...current focus for pointer device (should I make them different?) */
    widget_data_t *pointer_focus;

    /* ...latched pointer position */
    int pointer_x, pointer_y;

    /* ...keyboard device interface */
    struct wl_keyboard *keyboard;

    /* ...current focus for keyboard device (should I make them different?) */
    widget_data_t *keyboard_focus;

    /* ...touch device interface */
    struct wl_touch *touch;

    /* ...current focus widgets for touchscreen events */
    widget_data_t *touch_focus;

} input_data_t;

/* ...dispatch loop source */
typedef struct display_source_cb
{
    /* ...processing function */
    int (*hook)(display_data_t *, struct display_source_cb *, u32 events);

} display_source_cb_t;

/* ...display data */
struct display_data
{
    /* ...Wayland display handle */
    struct wl_display *display;

    /* ...Wayland registry handle */
    struct wl_registry *registry;

    /* ...screen compositor */
    struct wl_compositor *compositor;

    /* ...subcompositor interface handle (not used) */
    struct wl_subcompositor *subcompositor;

    /* ...shell interface handle */
    struct wl_shell *shell;

    /* ...kms-buffers interface (not used?) */
    struct wl_kms *kms;

    /* ...shared memory interface handle (not used?) */
    struct wl_shm *shm;

    /* ...input/output device handles */
    struct wl_list outputs, inputs;

    /* ...set of registered */
    struct wl_list windows;

    /* ...EGL configuration data */
    egl_data_t egl;

    /* ...cairo device associated with EGL display */
    cairo_device_t *cairo;

    /* ...dispatch loop epoll descriptor */
    int efd;

    /* ...pending display event status */
    int pending;

    /* ...dispatch thread handle */
    pthread_t thread;

    /* ...display lock (need that really? - tbd) */
    pthread_mutex_t lock;
};

/* ...output window data */
struct window_data
{
    /* ...root window data (must be first) */
    window_data_base_t base;

    /* ...list node in display windows list */
    struct wl_list link;

    /* ...wayland surface */
    struct wl_surface *surface;

    /* ...shell surface */
    struct wl_shell_surface *shell;

    /* ...native EGL window */
    struct wl_egl_window *native;

    /* ...window EGL context (used by native / cairo renderers) */
    EGLContext user_egl_ctx;

    /* ...EGL surface */
    EGLSurface egl;
};

/*******************************************************************************
 * Local variables
 ******************************************************************************/

/* ...this should be singleton for now - tbd */
static display_data_t __display;

/*******************************************************************************
 * EGL functions binding (make them global; create EGL adaptation layer - tbd)
 ******************************************************************************/

/* ...EGL/GLES functions */
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC eglSwapBuffersWithDamageEXT;
PFNGLMAPBUFFEROESPROC glMapBufferOES;
PFNGLUNMAPBUFFEROESPROC glUnmapBufferOES;
PFNGLBINDVERTEXARRAYOESPROC glBindVertexArrayOES;
PFNGLDELETEVERTEXARRAYSOESPROC glDeleteVertexArraysOES;
PFNGLGENVERTEXARRAYSOESPROC glGenVertexArraysOES;
PFNGLISVERTEXARRAYOESPROC glIsVertexArrayOES;

PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;


/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

#if defined (EGL_HAS_IMG_EXTERNAL_EXT)
#    define TEXTURE_TARGET GL_TEXTURE_EXTERNAL_OES
#else
#    define TEXTURE_TARGET GL_TEXTURE_2D
#endif

#define MAX_ATTRIBUTES_COUNT 30

/*******************************************************************************
 * Internal helpers
 ******************************************************************************/

static inline window_data_t * __window_lookup(struct wl_surface *surface)
{
    window_data_t *window;

    if (!surface)
    {
        return NULL;
    }

    /* ...get user data */
    window = wl_surface_get_user_data(surface);

    /* ...check output value*/
    if (!window || window->surface != surface)
    {
        return NULL;
    }

    return window;
}

/*******************************************************************************
 * Display dispatch thread
 ******************************************************************************/

/* ...number of events expected */
#define DISPLAY_EVENTS_NUM      4

/* ...add handle to a display polling structure */
static inline int display_add_poll_source(display_data_t *display,
        int fd, display_source_cb_t *cb)
{
    struct epoll_event event;

    event.events = EPOLLIN;
    event.data.ptr = cb;
    return epoll_ctl(display->efd, EPOLL_CTL_ADD, fd, &event);
}

/* ...remove handle from a display polling structure */
static inline int display_remove_poll_source(display_data_t *display, int fd)
{
    return epoll_ctl(display->efd, EPOLL_CTL_DEL, fd, NULL);
}

/* ...display dispatch thread */
static void * dispatch_thread(void *arg)
{
    display_data_t *display = arg;
    struct epoll_event event[DISPLAY_EVENTS_NUM];

    /* ...add display file descriptor */
    CHK_ERR(display_add_poll_source(display, wl_display_get_fd(display->display), NULL) == 0, NULL);

    /* ...start waiting loop */
    while (1) {
        int disp = 0;
        int i, r;

        /* ...as we are preparing to poll Wayland display, add polling prologue */
        while (wl_display_prepare_read(display->display) != 0)
        {
            /* ...dispatch all pending events and repeat attempt */
            wl_display_dispatch_pending(display->display);
        }

        /* ...flush all outstanding commands to a display */
        if (wl_display_flush(display->display) < 0)
        {
            TRACE(ERROR, _x("display flush failed: %m"));
            goto error;
        }

        /* ...wait for an event */
        if ((r = epoll_wait(display->efd, event, DISPLAY_EVENTS_NUM, -1)) < 0)
        {
            if(errno == EINTR)
            {
                wl_display_cancel_read(display->display);
                continue;
            }
            TRACE(ERROR, _x("epoll failed: %m"));
            goto error;
        }

        /* ...process all signalled events */
        for (i = 0; i < r; i++)
        {
            display_source_cb_t *dispatch = event[i].data.ptr;

            /* ...invoke event-processing function (ignore result code) */
            if (dispatch)
            {
                dispatch->hook(display, dispatch, event[i].events);
            } else if (event[i].events & EPOLLIN)
            {
                disp = 1;
            }
        }

        /* ...process display event separately */
        if (disp)
        {
            /* ...read display events */
            if (wl_display_read_events(display->display) < 0 && errno != EAGAIN)
            {
                TRACE(ERROR, _x("failed to read display events: %m"));
                goto error;
            }

            /* ...process pending display events (if any) */
            if (wl_display_dispatch_pending(display->display) < 0)
            {
                TRACE(ERROR, _x("failed to dispatch display events: %m"));
                goto error;
            }
        } else
        {
            /* ...if nothing was read from display, cancel initiated reading */
            wl_display_cancel_read(display->display);
        }
    }

    TRACE(INIT, _b("display dispatch thread terminated"));
    return NULL;

error:
    return (void *) (intptr_t) - errno;
}

/*******************************************************************************
 * Output device handling
 ******************************************************************************/

/* ...geometry change notification */
static void output_handle_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y,
        int32_t physical_width, int32_t physical_height,
        int32_t subpixel,
        const char *make, const char *model,
        int32_t output_transform)
{
    output_data_t *output = data;

    /* ...save screen rotation mode */
    output->transform = output_transform;

    /* ...nothing but printing? */
    TRACE(INFO, _b("output[%p:%p]: %s:%s: x=%d, y=%d, transform=%d"), output, wl_output, make, model, x, y, output_transform);
}

/* ...output device mode reporting processing */
static void output_handle_mode(void *data, struct wl_output *wl_output,
        uint32_t flags, int32_t width, int32_t height,
        int32_t refresh)
{
    output_data_t *output = data;

    /* ...check if the mode is current */
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;

    /* ...set current output device size */
    output->width = width, output->height = height;

    TRACE(INFO, _b("output[%p:%p] - %d*%d"), output, wl_output, width, height);
}

static const struct wl_output_listener output_listener =
{
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
};

/* ...add output device */
static inline void display_add_output(display_data_t *display, struct wl_registry *registry, uint32_t id)
{
    output_data_t *output = calloc(1, sizeof (*output));

    BUG(!output, _x("failed to allocate memory"));

    output->output = wl_registry_bind(registry, id, &wl_output_interface, 1);
    wl_output_add_listener(output->output, &output_listener, output);
    wl_list_insert(display->outputs.prev, &output->link);

    /* ...force another round of display initialization */
    display->pending = 1;
}

/* ...get output device by number */
static output_data_t *display_get_output(display_data_t *display, int n)
{
    output_data_t *output;

    /* ...traverse available outputs list */
    wl_list_for_each(output, &display->outputs, link)
    if (n-- == 0)
        return output;

    /* ...not found */
    return NULL;
}

/*******************************************************************************
 * Input device handling
 ******************************************************************************/

/* ...pointer entrance notification */
static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface,
        wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    input_data_t *input = data;
    int sx = wl_fixed_to_int(sx_w);
    int sy = wl_fixed_to_int(sy_w);
    window_data_t *window;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]-enter: surface: %p, serial: %u, sx: %d, sy: %d"), input, surface, serial, sx, sy);

    /* ...check the surface is valid */
    if (!(window = __window_lookup(surface))) return;

    /* ...latch pointer position */
    input->pointer_x = sx, input->pointer_y = sy;

    /* ...set current focus */
    focus = &window->base.widget;

    /* ...drop event if no processing is associated */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to the root widget */
    event.type = WIDGET_EVENT_MOUSE_ENTER;
    event.mouse.x = sx, event.mouse.y = sy;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...pointer leave notification */
static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface)
{
    input_data_t *input = data;
    window_data_t *window;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]-leave: surface: %p, serial: %u"), input, surface, serial);

    /* ...check the surface is valid */
    if (!(window = __window_lookup(surface)))
    {
        return;
    }

    /* ...drop event if no focus is defined */
    if (!(focus = input->pointer_focus))
    {
        return;
    }

    /* ...clear pointer-device focus */
    input->pointer_focus = NULL;

    /* ...drop event if no processing is associated */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to the current widget */
    event.type = WIDGET_EVENT_MOUSE_LEAVE;

    /* ...pass event to active widget */
    input->pointer_focus = info->event(focus, focus->cdata, &event);

    (focus != input->pointer_focus ? TRACE(DEBUG, _b("focus updated: %p"), input->pointer_focus) : 0);
}

/* ...handle pointer motion */
static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
        uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    input_data_t *input = data;
    int sx = wl_fixed_to_int(sx_w);
    int sy = wl_fixed_to_int(sy_w);
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]: motion: sx=%d, sy=%d"), input, sx, sy);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))
    {
        return;
    }

    /* ...latch input position */
    input->pointer_x = sx, input->pointer_y = sy;

    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_MOVE;
    event.mouse.x = sx;
    event.mouse.y = sy;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...button press/release processing */
static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state)
{
    input_data_t *input = data;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]: serial=%u, button=%u, state=%u"), input, serial, button, state);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))
    {
        return;
    }

    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_BUTTON;
    event.mouse.x = input->pointer_x;
    event.mouse.y = input->pointer_y;
    event.mouse.button = button;
    event.mouse.state = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

/* ...button wheel (?) processing */
static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value)
{
    input_data_t *input = data;
    int v = wl_fixed_to_int(value);
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _x("input[%p]: axis=%u, value=%d"), input, axis, v);

    /* ...drop event if no current focus set */
    if (!(focus = input->pointer_focus))
    {
        return;
    }

    /* ...drop event if no processing hook set */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_MOUSE_AXIS;
    event.mouse.x = input->pointer_x;
    event.mouse.y = input->pointer_y;
    event.mouse.axis = axis;
    event.mouse.value = v;
    input->pointer_focus = info->event(focus, focus->cdata, &event);
}

static const struct wl_pointer_listener pointer_listener =
{
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

/*******************************************************************************
 * Touchscreen support
 ******************************************************************************/

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
        uint32_t serial, uint32_t time, struct wl_surface *surface,
        int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    input_data_t *input = data;
    int sx = wl_fixed_to_int(x_w);
    int sy = wl_fixed_to_int(y_w);
    window_data_t *window;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]-touch-down: surface=%p, id=%u, sx=%d, sy=%d"), input, surface, id, sx, sy);

    /* ...get window associated with a surface */
    if (!(window = __window_lookup(surface)))
    {
        return;
    }

    /* ...get touch focus if needed */
    focus = (input->touch_focus ? : &window->base.widget);

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to root widget */
    event.type = WIDGET_EVENT_TOUCH_DOWN;
    event.touch.x = sx;
    event.touch.y = sy;
    event.touch.id = id;

    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)
    {
        TRACE(DEBUG, _x("touch focus lost!"));
    }
}

/* ...touch removal event notification */
static void touch_handle_up(void *data, struct wl_touch *wl_touch,
        uint32_t serial, uint32_t time, int32_t id)
{
    input_data_t *input = data;
    widget_data_t *focus = input->touch_focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]-touch-up: serial=%u, id=%u"), input, serial, id);

    /* ...drop event if no focus defined */
    if (!(focus = input->touch_focus))
    {
        return;
    }

    /* ...reset touch focus pointer */
    input->touch_focus = NULL;

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_TOUCH_UP;
    event.touch.id = id;
    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)
    {
        TRACE(DEBUG, _x("touch focus lost!"));
    }
}

/* ...touch sliding event processing */
static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
        uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    input_data_t *input = data;
    int sx = wl_fixed_to_int(x_w);
    int sy = wl_fixed_to_int(y_w);
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(0, _b("input[%p]-move: id=%u, sx=%d, sy=%d (focus: %p)"), input, id, sx, sy, input->touch_focus);

    /* ...ignore event if no touch focus exists */
    if (!(focus = input->touch_focus))
    {
        return;
    }

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_TOUCH_MOVE;
    event.touch.x = sx;
    event.touch.y = sy;
    event.touch.id = id;
    input->touch_focus = info->event(focus, focus->cdata, &event);

    if (!input->touch_focus)
    {
        TRACE(DEBUG, _x("touch focus lost!"));
    }
}

/* ...end of touch frame (gestures recognition?) */
static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
    input_data_t *input = data;

    TRACE(DEBUG, _b("input[%p]-touch-frame"), input);
}

/* ...touch-frame cancellation (gestures recognition?) */
static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
    input_data_t *input = data;

    TRACE(DEBUG, _b("input[%p]-frame-cancel"), input);
}

/* ...wayland touch device listener callbacks */
static const struct wl_touch_listener touch_listener =
{
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
};

/*******************************************************************************
 * Keyboard events processing
 ******************************************************************************/

/* ...keymap handling */
static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
        uint32_t format, int fd, uint32_t size)
{
    input_data_t *input = data;

    /* ...here we can remap keycodes - tbd */
    TRACE(DEBUG, _b("input[%p]: keymap format: %X, fd=%d, size=%u"), input, format, fd, size);
}

/* ...keyboard focus receive notification */
static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface,
        struct wl_array *keys)
{
    input_data_t *input = data;
    window_data_t *window;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(DEBUG, _b("input[%p]: key-enter: surface: %p"), input, surface);

    /* ...get window associated with a surface */
    if (!(window = __window_lookup(surface)))
    {
        return;
    }

    /* ...set focus to root widget (? - tbd) */
    input->keyboard_focus = focus = &window->base.widget;

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_KEY_ENTER;
    input->keyboard_focus = info->event(focus, focus->cdata, &event);

    /* ...process all pressed keys? modifiers? */
}

/* ...keyboard focus leave notification */
static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    input_data_t *input = data;
    window_data_t *window;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(DEBUG, _b("input[%p]: key-leave: surface: %p"), input, surface);

    /* ...find a target widget */
    if (!(window = __window_lookup(surface)))
    {
        return;
    }

    /* ...select active widget (root widget if nothing) */
    focus = (input->keyboard_focus ? : &window->base.widget);

    /* ...reset keyboard focus */
    input->keyboard_focus = NULL;

    /* ...drop message if no processing is defined */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass event to current widget */
    event.type = WIDGET_EVENT_KEY_LEAVE;
    input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...key pressing event */
static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    input_data_t *input = data;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(DEBUG, _b("input[%p]: key-press: key=%u, state=%u"), input, key, state);

    /* ...ignore event if no focus defined */
    if (!(focus = input->keyboard_focus))
    {
        return;
    }

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass notification to the widget */
    event.type = WIDGET_EVENT_KEY_PRESS;
    event.key.code = key;
    event.key.state = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...modifiers state change */
static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t mods_depressed,
        uint32_t mods_latched, uint32_t mods_locked,
        uint32_t group) {
    input_data_t *input = data;
    widget_data_t *focus;
    widget_info_t *info;
    widget_event_t event;

    TRACE(DEBUG, _b("input[%p]: mods-press: press=%X, latched=%X, locked=%X, group=%X"), input, mods_depressed, mods_latched, mods_locked, group);

    /* ...ignore event if no focus defined */
    if (!(focus = input->keyboard_focus))
    {
        return;
    }

    /* ...drop event if no processing is registered */
    if (!(info = focus->info) || !info->event)
    {
        return;
    }

    /* ...pass notification to the widget */
    event.type = WIDGET_EVENT_KEY_MODS;
    event.key.mods_on = mods_latched;
    event.key.mods_off = mods_depressed;
    event.key.mods_locked = mods_locked;
    input->keyboard_focus = info->event(focus, focus->cdata, &event);
}

/* ...keyboard listener callback */
static const struct wl_keyboard_listener keyboard_listener =
{
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    NULL,                       /* ... repeat_info = NULL */
};

/*******************************************************************************
 * Input device registration
 ******************************************************************************/

/* ...input device capabilities registering */
static void seat_handle_capabilities(void *data, struct wl_seat *seat, enum wl_seat_capability caps)
{
    input_data_t *input = data;

    /* ...process pointer device addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer)
    {
        input->pointer = wl_seat_get_pointer(seat);
        wl_pointer_set_user_data(input->pointer, input);
        wl_pointer_add_listener(input->pointer, &pointer_listener, input);
        TRACE(INFO, _b("pointer-device %p added"), input->pointer);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer)
    {
        TRACE(INFO, _b("pointer-device %p removed"), input->pointer);
        wl_pointer_destroy(input->pointer);
        input->pointer = NULL;
    }

    /* ...process keyboard addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard)
    {
        input->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_set_user_data(input->keyboard, input);
        wl_keyboard_add_listener(input->keyboard, &keyboard_listener, input);
        TRACE(INFO, _b("keyboard-device %p added"), input->keyboard);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard)
    {
        TRACE(INFO, _b("keyboard-device %p removed"), input->keyboard);
        wl_keyboard_destroy(input->keyboard);
        input->keyboard = NULL;
    }

    /* ...process touch device addition/removal */
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch)
    {
        input->touch = wl_seat_get_touch(seat);
        wl_touch_set_user_data(input->touch, input);
        wl_touch_add_listener(input->touch, &touch_listener, input);
        TRACE(INFO, _b("touch-device %p added"), input->touch);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch)
    {
        TRACE(INFO, _b("touch-device %p removed"), input->touch);
        wl_touch_destroy(input->touch);
        input->touch = NULL;
    }
}

/* ...input device name (probably, for a mapping to particular output? - tbd) */
static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
    input_data_t *input = data;

    /* ...just output a name */
    TRACE(INFO, _b("input[%p]: device '%s' registered"), input, name);
}

/* ...input device wayland callback */
static const struct wl_seat_listener seat_listener =
{
    seat_handle_capabilities,
    seat_handle_name
};

/* ...register input device */
static inline void display_add_input(display_data_t *display, struct wl_registry *registry, uint32_t id, uint32_t version)
{
    input_data_t *input = calloc(1, sizeof (*input));

    BUG(!input, _x("failed to allocate memory"));

    /* ...bind seat interface */
    input->seat = wl_registry_bind(registry, id, &wl_seat_interface, MIN(version, 3));
    wl_seat_add_listener(input->seat, &seat_listener, input);
    wl_list_insert(display->inputs.prev, &input->link);

    /* ...force another round of display initialization */
    display->pending = 1;
}

#ifdef SPACENAV_ENABLED
/*******************************************************************************
 * Spacenav 3D-joystick support
 ******************************************************************************/

/* ...spacenav input event processing */
static int input_spacenav_event(display_data_t *display, display_source_cb_t *cb, u32 events)
{
    widget_event_t event;
    spnav_event e;
    window_data_t *window;

    /* ...drop event if no reading flag set */
    if ((events & EPOLLIN) == 0)
    {
        return 0;
    }

    /* ...retrieve poll event */
    if (CHK_API(spnav_poll_event(&e)) == 0)
    {
        return 0;
    }

    /* ...preare widget event */
    event.type = WIDGET_EVENT_SPNAV;
    event.spnav.e = &e;

    /* ...pass to all windows */
    wl_list_for_each(window, &display->windows, link) {
        widget_data_t *widget = &window->base.widget;
        widget_info_t *info = widget->info;

        /* ...ignore window if no input event is registered */
        if (!info || !info->event)
        {
            continue;
        }

        /* ...pass event to root widget (only one consumer?) */
        if (info->event(widget, window->base.cdata, &event) != NULL)
        {
            break;
        }
    }

    return 0;
}

static display_source_cb_t spacenav_source =
{
    .hook = input_spacenav_event,
};

/* ...spacenav event initializer */
static inline int input_spacenav_init(display_data_t *display)
{
    int fd;

    /* ...open spacenav device (do not die if not found) */
    if (spnav_open() < 0) {
        TRACE(INIT, _b("spacenavd daemon is not running"));
        return 0;
    }

    if ((fd = spnav_fd()) < 0) {
        TRACE(ERROR, _x("failed to open spacenv connection: %m"));
        goto error;
    }

    /* ...add file-descriptor as display poll source */
    if (display_add_poll_source(display, fd, &spacenav_source) < 0)
    {
        TRACE(ERROR, _x("failed to add poll source: %m"));
        goto error;
    }

    TRACE(INIT, _b("spacenav input added"));

    return 0;

error:
    /* ...destroy connection to a server */
    spnav_close();

    return -errno;
}

/*******************************************************************************
 * Joystick support
 ******************************************************************************/

typedef struct joystick_data {
    /* ...generic display source handle */
    display_source_cb_t source;

    /* ...file descriptor */
    int fd;

    /* ...any axis? - button maps? - tbd - need to keep latched values */

} joystick_data_t;

/* ...joystick input event processing */
static int input_joystick_event(display_data_t *display, display_source_cb_t *cb, u32 events)
{
    joystick_data_t *js = (joystick_data_t *) cb;
    widget_event_t event;
    struct js_event e;
    window_data_t *window;

    /* ...drop event if no reading flag set */
    if ((events & EPOLLIN) == 0)
    {
        return 0;
    }

    /* ...retrieve poll event */
    CHK_ERR(read(js->fd, &e, sizeof (e)) == sizeof (e), -errno);

    /* ...preare widget event */
    event.type = WIDGET_EVENT_JOYSTICK;
    event.js.e = &e;

    TRACE(DEBUG, _b("joystick event: type=%x, value=%x, number=%x"), e.type & ~JS_EVENT_INIT, e.value, e.number);

    /* ...pass to all windows */
    wl_list_for_each(window, &display->windows, link)
    {
        widget_data_t *widget = &window->base.widget;
        widget_info_t *info = widget->info;

        /* ...ignore window if no input event is registered */
        if (!info || !info->event)
        {
            continue;
        }

        /* ...pass event to root widget (only one consumer?) */
        if (info->event(widget, window->base.cdata, &event) != NULL)
        {
            break;
        }
    }

    return 0;
}

static joystick_data_t joystick_source =
{
    .source =
    {
        .hook = input_joystick_event,
    },
};

/* ...spacenav event initializer */
static inline int input_joystick_init(display_data_t *display, const char *devname)
{
    int fd;
    int version = 0x800;
    int axes = 2, buttons = 2;
    char name[128] = {'\0'};

    /* ...open joystick device */
    if ((joystick_source.fd = fd = open(devname, O_RDONLY)) < 0)
    {
        TRACE(INIT, _b("no joystick connected"));
        return 0;
    }

    ioctl(fd, JSIOCGVERSION, &version);
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    ioctl(fd, JSIOCGNAME(sizeof (name)), name);

    TRACE(INIT, _b("device: %s; version: %X, buttons: %d, axes: %d, name: %s"), devname, version, buttons, axes, name);

    /* ...put joystick into non-blocking mode */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    /* ...add file descriptor to display poll set */
    if (display_add_poll_source(display, fd, &joystick_source.source) < 0)
    {
        TRACE(ERROR, _x("failed to add joystick: %m"));
        goto error;
    }

    TRACE(INIT, _b("joystick device '%s' added"), devname);

    return 0;

error:
    /* close device descriptor */
    close(fd);
    return -errno;
}
#endif

/*******************************************************************************
 * Registry listener callbacks
 ******************************************************************************/

/* ...interface registrar */
static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
        const char *interface, uint32_t version)
{
    display_data_t *display = data;

    if (strcmp(interface, "wl_compositor") == 0)
    {
        display->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        display->subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "wl_shell") == 0)
    {
        display->shell = wl_registry_bind(registry, id, &wl_shell_interface, 1);
    }
    else if (strcmp(interface, "wl_output") == 0)
    {
        display_add_output(display, registry, id);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        display_add_input(display, registry, id, version);
    }
}

/* ...interface removal notification callback */
static void global_registry_remove(void *data, struct wl_registry *registry, uint32_t id)
{
    display_data_t *display = data;

    TRACE(INIT, _b("display[%p]: id removed: %u"), display, id);
}

/* ...registry listener callbacks */
static const struct wl_registry_listener registry_listener =
{
    global_registry_handler,
    global_registry_remove
};

/*******************************************************************************
 * Shell surface interface implementation
 ******************************************************************************/

/* ...shell surface heartbeat callback */
static void handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

/* ...shell surface reconfiguration callback */
static void handle_configure(void *data, struct wl_shell_surface *shell_surface,
        uint32_t edges, int32_t width, int32_t height)
{
    TRACE(INFO, _b("shell configuration changed: W=%d, H=%d, E=%u"), width, height, edges);
}

/* ...focus removal notification */
static void handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
    TRACE(INFO, _b("focus removed - hmm..."));
}

/* ...shell surface callbacks */
static const struct wl_shell_surface_listener shell_surface_listener =
{
    handle_ping,
    handle_configure,
    handle_popup_done
};

/*******************************************************************************
 * EGL helpers
 ******************************************************************************/

static const EGLint __egl_context_attribs[] =
{
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
};

/* ...destroy EGL context */
static void fini_egl(display_data_t *display)
{
    eglTerminate(display->egl.dpy);
    eglReleaseThread();
}

/* ...initialize EGL */
static int init_egl(display_data_t *display)
{
    /* ...EGL configuration attributes */
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BUFFER_SIZE, 24,
        EGL_DEPTH_SIZE, 1,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint major, minor, n, count, i, size;
    EGLConfig *configs;
    EGLDisplay dpy;
    const char *extensions;

    /* ...get Wayland EGL display */
    CHK_ERR(display->egl.dpy = dpy = eglGetDisplay((NativeDisplayType)display->display), -ENOENT);

    /* ...initialize EGL module? */
    if (!eglInitialize(dpy, &major, &minor))
    {
        TRACE(ERROR, _x("failed to initialize EGL: %m (%X)"), eglGetError());
        goto error;
    }
    else if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        TRACE(ERROR, _x("failed to bind API: %m (%X)"), eglGetError());
        goto error;
    }
    else
    {
        TRACE(INIT, _b("EGL display opened: %p, major:minor=%d:%d"), dpy, major, minor);
    }

    /* ...get total number of configurations */
    if (!eglGetConfigs(dpy, NULL, 0, &count))
    {
        TRACE(ERROR, _x("failed to get EGL configs number"));
        goto error;
    }
    else if (count == 0)
    {
        TRACE(ERROR, _x("no configurations found"));
        goto error;
    }

    /* ...retrieve available configurations */
    if ((configs = calloc(count, sizeof (*configs))) == NULL)
    {
        TRACE(ERROR, _x("failed to allocate %lu bytes"), sizeof (*configs));
        goto error;
    }
    else if (!eglChooseConfig(dpy, config_attribs, configs, count, &n))
    {
        TRACE(ERROR, _x("failed to get matching configuration"));
        goto error_cfg;
    }
    else if (n == 0)
    {
        TRACE(ERROR, _x("no matching configurations"));
        goto error_cfg;
    }

    /* ...select configuration? */
    for (i = 0; i < n; i++)
    {
        EGLint id = -1;

        eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);

        /* ...get buffer size of that configuration */
        eglGetConfigAttrib(dpy, configs[i], EGL_BUFFER_SIZE, &size);

        TRACE(INFO, _b("config[%u of %u]: id=%X, size=%X"), i, n, id, size);

        /* ...check if we have a 32-bit buffer size - tbd */
        if (size != 32)
        {
            continue;
        }

        /* ...found a suitable configuration - print it? */
        display->egl.conf = configs[i];

        goto found;
    }

    TRACE(ERROR, _x("did not find suitable configuration"));
    errno = -ENODEV;
    goto error_cfg;

found:
    /* ...bind extensions */
    eglCreateImageKHR = (void *) eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (void *) eglGetProcAddress("eglDestroyImageKHR");
    eglSwapBuffersWithDamageEXT = (void *) eglGetProcAddress("eglSwapBuffersWithDamageEXT");
    glEGLImageTargetTexture2DOES = (void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    glMapBufferOES = (void *) eglGetProcAddress("glMapBufferOES");
    glUnmapBufferOES = (void *) eglGetProcAddress("glUnmapBufferOES");
    glBindVertexArrayOES = (void *) eglGetProcAddress("glBindVertexArrayOES");
    glDeleteVertexArraysOES = (void *) eglGetProcAddress("glDeleteVertexArraysOES");
    glGenVertexArraysOES = (void *) eglGetProcAddress("glGenVertexArraysOES");
    glIsVertexArrayOES = (void *) eglGetProcAddress("glIsVertexArrayOES");

    eglCreateSyncKHR = (void *) eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR = (void *) eglGetProcAddress("eglDestroySyncKHR");
    eglClientWaitSyncKHR = (void *) eglGetProcAddress("eglClientWaitSyncKHR");

    /* ...make sure we have eglImageKHR extension */
    BUG(!(eglCreateImageKHR && eglDestroyImageKHR), _x("breakpoint"));

    /* ...check for specific EGL extensions */
    if ((extensions = eglQueryString(display->egl.dpy, EGL_EXTENSIONS)) != NULL)
    {
        TRACE(INIT, _b("EGL extensions: %s"), extensions);
    }

    /* ...create display (shared?) EGL context */
    if ((display->egl.ctx = eglCreateContext(dpy, display->egl.conf, EGL_NO_CONTEXT, __egl_context_attribs)) == NULL)
    {
        TRACE(ERROR, _x("failed to create EGL context: %m/%X"), eglGetError());
        goto error_cfg;
    }

    /* ...free configuration array (ha-ha display->egl.conf) */
    free(configs);

    TRACE(INIT, _b("EGL initialized"));

    return 0;

error_cfg:
    /* ...destroy configuration array */
    free(configs);

error:
    /* ...close a display */
    fini_egl(display);

    return -1;
}

/*******************************************************************************
 * Display dispatch thread
 ******************************************************************************/

/* ...return cairo device associated with a display */
cairo_device_t * __display_cairo_device(display_data_t *display)
{
    return display->cairo;
}

egl_data_t * display_egl_data(display_data_t *display)
{
    return &display->egl;
}

/* ...return EGL surface associated with window */
EGLSurface window_egl_surface(window_data_t *window)
{
    return window->egl;
}

EGLContext window_egl_context(window_data_t *window)
{
    return window->user_egl_ctx;
}

/* ...get exclusive access to shared EGL context */
static inline void display_egl_ctx_get(display_data_t *display)
{
    /* ...we should not call that function in user-window context */
    BUG(eglGetCurrentContext() != EGL_NO_CONTEXT, _x("invalid egl context"));

    /* ...get shared context lock */
    pthread_mutex_lock(&display->lock);

    /* ...display context is shared with all windows; context is surfaceless */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, display->egl.ctx);
}

/* ...release shared EGL context */
static inline void display_egl_ctx_put(display_data_t *display)
{
    /* ...display context is shared with all windows; context is surfaceless */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    /* ...release shared context lock */
    pthread_mutex_unlock(&display->lock);
}

/*******************************************************************************
 * Window support
 ******************************************************************************/

/* ...window rendering thread */
static void * window_thread(void *arg)
{
    window_data_t *window = arg;
    display_data_t *display = window->base.display;

    while (1)
    {
        /* ...serialize access to window state */
        pthread_mutex_lock(&window->base.lock);

        /* ...wait for a drawing command from an application */
        while (!(window->base.flags & (WINDOW_FLAG_REDRAW | WINDOW_FLAG_TERMINATE | WINDOW_BV_REINIT)))
        {
            TRACE(DEBUG, _b("window[%p] wait"), window);
            pthread_cond_wait(&window->base.wait, &window->base.lock);
        }

        TRACE(DEBUG, _b("window[%p] redraw (flags=%X)"), window, window->base.flags);

        /* ...break processing thread if requested to do that */
        if (window->base.flags & WINDOW_FLAG_TERMINATE)
        {
            pthread_mutex_unlock(&window->base.lock);
            break;
        }
        else if (window->base.flags & WINDOW_FLAG_REDRAW)
        {

            /* ...clear window drawing schedule flag */
            window->base.flags &= ~WINDOW_FLAG_REDRAW;

            /* ...release window access lock */
            pthread_mutex_unlock(&window->base.lock);

            /* ...re-acquire window GL context */
            eglMakeCurrent(display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

            /* ...invoke user-supplied hook */
            window->base.info->redraw(display, window->base.cdata);
        }
        else
        {
            /* Reinitialize bv in sv_engine */

            window->base.flags &= ~WINDOW_BV_REINIT;

            /* ...release window access lock */
            pthread_mutex_unlock(&window->base.lock);

            /* ...re-acquire window GL context */
            eglMakeCurrent(display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

            /* ...invoke user-supplied hook */
            window->base.info->init_bv(display, window->base.cdata);
        }
    }

    TRACE(INIT, _b("window[%p] thread terminated"), window);

    /* ...release context eventually */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    return NULL;
}

/*******************************************************************************
 * Internal helpers - getting messy - tbd
 ******************************************************************************/

/* ...check cairo device status */
static inline int __check_device(cairo_device_t *cairo)
{
    cairo_status_t status;

    status = cairo_device_status(cairo);

    switch (status)
    {
        case CAIRO_STATUS_SUCCESS:
            return 0;
        case CAIRO_STATUS_DEVICE_ERROR: errno = EINVAL;
            break;
        default: errno = ENOMEM;
            break;
    }

    TRACE(ERROR, _b("cairo device error: '%s'"), cairo_status_to_string(status));

    return -errno;
}

/*******************************************************************************
 * Basic widgets support
 ******************************************************************************/

/* ...internal widget initialization function */
int __widget_init(widget_data_t *widget, window_data_t *window, int W, int H, widget_info_t *info, void *cdata)
{
    cairo_device_t *cairo = window->base.cairo;
    int w, h;

    /* ...set user-supplied data */
    widget->info = info, widget->cdata = cdata;

    /* ...set pointer to the owning window */
    widget->window = window;

    /* ...if width/height are not specified, take them from window */
    widget->width = w = (info && info->width ? info->width : W);
    widget->height = h = (info && info->height ? info->height : H);
    widget->top = (info ? info->top : 0);
    widget->left = (info ? info->left : 0);

    /* ...create cairo surface for a graphical content */
    if (widget == &window->base.widget)
    {
        widget->cs = cairo_gl_surface_create_for_egl(cairo, window->egl, w, h);
    }
    else
    {
        widget->cs = cairo_gl_surface_create(cairo, CAIRO_CONTENT_COLOR_ALPHA, w, h);
    }

    /* Force context sanity after cairo calls */
    eglMakeCurrent(window->base.display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

    if (__check_surface(widget->cs) != 0)
    {
        TRACE(ERROR, _x("failed to create GL-surface [%u*%u]: %m"), w, h);
        return -errno;
    }
    /* ...initialize widget controls as needed */
    if (info && info->init)
    {
        if (info->init(widget, cdata) < 0)
        {
            TRACE(ERROR, _x("widget initialization failed: %m"));
            goto error_cs;
        }
        eglMakeCurrent(window->base.display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

        /* ...mark widget is dirty */
        widget->dirty = 1;
    }
    else
    {
        /* ...clear dirty flag */
        widget->dirty = 0;
    }

    BUG(eglGetCurrentContext() != window->user_egl_ctx, _x("invalid egl context"));
    BUG(eglGetCurrentSurface(EGL_READ) != window->egl, _x("invalid egl READ"));
    BUG(eglGetCurrentSurface(EGL_DRAW) != window->egl, _x("invalid egl DRAW"));

    TRACE(INIT, _b("widget [%p] initialized"), widget);

    return 0;

error_cs:
    /* ...destroy cairo surface */
    cairo_surface_destroy(widget->cs);

    return -errno;
}

/*******************************************************************************
 * Window API
 ******************************************************************************/

/* ...transformation matrix processing */
static inline void window_set_transform_matrix(window_data_t *window, int *width, int *height, int fullscreen, u32 transform)
{
    cairo_matrix_t *m = &window->base.cmatrix;
    int w = *width, h = *height;

    if (fullscreen && transform)
    {
        switch (transform)
        {
            case 90:
                m->xx = 0.0, m->xy = -1.0, m->x0 = w;
                m->yx = 1.0, m->yy = 0.0, m->y0 = 0;
                break;

            case 180:
                m->xx = -1.0, m->xy = 0.0, m->x0 = w;
                m->yx = 0.0, m->yy = -1.0, m->y0 = h;
                break;

            case 270:
                m->xx = 0.0, m->xy = 1.0, m->x0 = 0;
                m->yx = -1.0, m->yy = 0.0, m->y0 = h;
                break;

            default:
                BUG(1, _x("invalid transformation: %u"), transform);
        }
    }
    else
    {
        /* ...set identity transformation matrix */
        cairo_matrix_init_identity(m);
    }
}

/* ...create native window */
window_data_t * window_create(display_data_t *display, window_info_t *info, widget_info_t *info2, void *cdata)
{
    int width = info->width;
    int height = info->height;
    output_data_t *output;
    window_data_t *window;
    struct wl_region *region;
    pthread_attr_t attr;
    int r;

    /* ...make sure we have a valid output device */
    if ((output = display_get_output(display, info->output)) == NULL) {
        TRACE(ERROR, _b("invalid output device number: %u"), info->output);
        errno = EINVAL;
        return NULL;
    }

    /* ...allocate a window data */
    if ((window = malloc(sizeof (*window))) == NULL) {
        TRACE(ERROR, _x("failed to allocate memory"));
        errno = ENOMEM;
        return NULL;
    }

    /* ...if width/height are not specified, use output device dimensions */
    (!width ? width = output->width : 0), (!height ? height = output->height : 0);

    /* ...initialize window data access lock */
    pthread_mutex_init(&window->base.lock, NULL);

    /* ...initialize conditional variable for communication with rendering thread */
    pthread_cond_init(&window->base.wait, NULL);

    /* ...save display handle */
    window->base.display = display;

    /* ...save window info data */
    window->base.info = info, window->base.cdata = cdata;

    /* ...clear window flags */
    window->base.flags = 0;

    /* ...reset frame-rate calculator */
    window_frame_rate_reset(window);

    /* ...get wayland surface (subsurface maybe?) */
    window->surface = wl_compositor_create_surface(display->compositor);

    /* ...specify window has the only opaque region */
    region = wl_compositor_create_region(display->compositor);
    wl_region_add(region, 0, 0, width, height);
    wl_surface_set_opaque_region(window->surface, region);
    wl_region_destroy(region);

    /* ...get desktop shell surface handle */
    window->shell = wl_shell_get_shell_surface(display->shell, window->surface);
    wl_shell_surface_add_listener(window->shell, &shell_surface_listener, window);
    (info->title ? wl_shell_surface_set_title(window->shell, info->title) : 0);
    wl_shell_surface_set_toplevel(window->shell);
    (info->fullscreen ? wl_shell_surface_set_fullscreen(window->shell, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, output->output) : 0);

    /* ...set private data poitner */
    wl_surface_set_user_data(window->surface, window);

    /* ...create native window */
    window->native = wl_egl_window_create(window->surface, width, height);
    window->egl = eglCreateWindowSurface(display->egl.dpy, display->egl.conf, (EGLNativeWindowType)window->native, NULL);

    /* ...create window user EGL context (share textures with everything else?)*/
    window->user_egl_ctx = eglCreateContext(display->egl.dpy, display->egl.conf, display->egl.ctx, __egl_context_attribs);

    /* ...create cairo context */
    window->base.cairo = cairo_egl_device_create(display->egl.dpy, window->user_egl_ctx);
    if (__check_device(window->base.cairo) != 0)
    {
        TRACE(ERROR, _x("failed to create cairo device: %m"));
        goto error;
    }

    /* ...make it simple - we are handling thread context ourselves */
    cairo_gl_device_set_thread_aware(window->base.cairo, FALSE);

    /* ...reset cairo program */
    window->base.cprog = 0;

    /* ...set cairo transformation matrix */
    window_set_transform_matrix(window, &width, &height, info->fullscreen, info->transform);

    /* ...set window EGL context */
    eglMakeCurrent(display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

    /* ...initialize root widget data */
    if (__widget_init(&window->base.widget, window, width, height, info2, cdata) < 0)
    {
        TRACE(INIT, _b("widget initialization failed: %m"));
        goto error;
    }

    /* ...clear surface to flush all textures loading etc.. - looks a bit strange */
    cairo_t *cr = cairo_create(window->base.widget.cs);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    /* ...release window EGL context */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    /* ...initialize thread attributes (joinable, default stack size) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    /* ...create rendering thread */
    r = pthread_create(&window->base.thread, &attr, window_thread, window);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        TRACE(ERROR, _x("thread creation failed: %m"));
        goto error;
    }

    /* ...add window to global display list */
    wl_list_insert(display->windows.prev, &window->link);

    TRACE(INFO, _b("window created: %p:%p, %u * %u, output: %u"), window, window->egl, width, height, info->output);

    return window;

error:
    /* ...destroy window memory */
    free(window);
    return NULL;
}

static void __destroy_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    pthread_mutex_t *wait_lock = data;

    TRACE(DEBUG, _b("release wait lock"));

    /* ...release mutex */
    pthread_mutex_unlock(wait_lock);

    wl_callback_destroy(callback);
}

static const struct wl_callback_listener __destroy_listener =
{
    __destroy_callback,
};

/* ...destroy a window */
void window_destroy(window_data_t *window)
{
    display_data_t *display = window->base.display;
    EGLDisplay dpy = display->egl.dpy;
    const window_info_t *info = window->base.info;
    const widget_info_t *info2 = window->base.widget.info;
    struct wl_callback *callback;

    /* ...terminate window rendering thread */
    pthread_mutex_lock(&window->base.lock);
    window->base.flags |= WINDOW_FLAG_TERMINATE;
    pthread_cond_signal(&window->base.wait);
    pthread_mutex_unlock(&window->base.lock);

    /* ...wait until thread completes */
    pthread_join(window->base.thread, NULL);

    TRACE(DEBUG, _b("window[%p] thread joined"), window);

    /* ...remove window from global display list */
    wl_list_remove(&window->link);

    /* ...acquire window context before doing anything */
    eglMakeCurrent(dpy, window->egl, window->egl, window->user_egl_ctx);

    /* ...invoke custom widget destructor function as needed */
    (info2 && info2->destroy ? info2->destroy(&window->base.widget, window->base.cdata) : 0);

    /* ...destroy root widget cairo surface */
    cairo_surface_destroy(window->base.widget.cs);

    /* ...invoke custom window destructor function as needed */
    (info && info->destroy ? info->destroy(window, window->base.cdata) : 0);

    /* ...destroy cairo device */
    cairo_device_destroy(window->base.cairo);

    /* ...release EGL context before destruction */
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    /* ...destroy context */
    eglDestroyContext(dpy, window->user_egl_ctx);

    /* ...destroy EGL surface */
    eglDestroySurface(display->egl.dpy, window->egl);

    /* ...destroy native window */
    wl_egl_window_destroy(window->native);

    /* ...destroy shell surface */
    wl_shell_surface_destroy(window->shell);

    /* ....destroy wayland surface (shell surface gets destroyed automatically) */
    wl_surface_destroy(window->surface);

    /* ...make sure function is complete before we actually proceed */
    callback = wl_display_sync(display->display);
    if (callback != NULL)
    {
        pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;

        pthread_mutex_lock(&wait_lock);
        wl_callback_add_listener(callback, &__destroy_listener, &wait_lock);

        wl_display_flush(display->display);

        /* ...mutex will be released in callback function executed from display thread context */
        pthread_mutex_lock(&wait_lock);
    }

    /* ...destroy window lock */
    pthread_mutex_destroy(&window->base.lock);

    /* ...destroy rendering thread conditional variable */
    pthread_cond_destroy(&window->base.wait);

    /* ...destroy object */
    free(window);

    TRACE(INFO, _b("window[%p] destroyed"), window);
}

/* ...retrieve associated cairo surface */
cairo_t * window_get_cairo(window_data_t *window)
{
    cairo_t *cr;

    /* ...re-acquire window GL context */
    eglMakeCurrent(window->base.display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

    /* ...it is a bug if we lost a context */
    BUG(eglGetCurrentContext() != window->user_egl_ctx, _x("invalid GL context"));

    /* ...restore original cairo program */
    glUseProgram(((window_data_base_t*)window)->cprog);

    /* ...create new drawing context */
    cr = cairo_create(((window_data_base_t*)window)->widget.cs);

    /* ...set transformation matrix */
    cairo_set_matrix(cr, &((window_data_base_t*)window)->cmatrix);

    /* ...make it a bug for a moment */
    BUG(cairo_status(cr) != CAIRO_STATUS_SUCCESS, _x("invalid status: (%d) - %s"), cairo_status(cr), cairo_status_to_string(cairo_status(cr)));

    return cr;
}

/* ...release associated cairo surface */
void window_put_cairo(window_data_t *window, cairo_t *cr)
{
    /* ...destroy cairo drawing interface */
    cairo_destroy(cr);

    /* ...re-acquire window GL context */
    eglMakeCurrent(window->base.display->egl.dpy, window->egl, window->egl, window->user_egl_ctx);

    /* ...save cairo program */
    glGetIntegerv(GL_CURRENT_PROGRAM, &window->base.cprog);
}

/* ...submit window to a renderer */
void window_draw(window_data_t *window)
{
    u32 t0, t1;

    t0 = get_cpu_cycles();

    /* ...swap buffers (finalize any pending 2D-drawing) */
    cairo_gl_surface_swapbuffers(window->base.widget.cs);

    /* ...make sure everything is correct */
    BUG(cairo_surface_status(window->base.widget.cs) != CAIRO_STATUS_SUCCESS,
        _x("bad status: %s"),
        cairo_status_to_string(cairo_surface_status(window->base.widget.cs)));

    t1 = get_cpu_cycles();

    TRACE(DEBUG, _b("swap[%p]: %u (error=%X)"), window, t1 - t0, eglGetError());
}

void window_clear(window_data_t *window)
{
    /* ...re-acquire window GL context */
    eglMakeCurrent(window->base.display->egl.dpy,
                   window->egl,
                   window->egl,
                   window->user_egl_ctx);

    /* ...ready for rendering; clear surface content */
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

/*******************************************************************************
 * Display module initialization
 ******************************************************************************/

/* ...create display data */
display_data_t * display_create(void)
{
    display_data_t *display = &__display;
    pthread_attr_t attr;
    int r;

    /* ...reset display data */
    memset(display, 0, sizeof (*display));

    /* ...connect to Wayland display */
    if ((display->display = wl_display_connect(NULL)) == NULL)
    {
        TRACE(ERROR, _x("failed to connect to Wayland: %m"));
        errno = EBADFD;
        goto error;
    }
    else if ((display->registry = wl_display_get_registry(display->display)) == NULL)
    {
        TRACE(ERROR, _x("failed to get registry: %m"));
        errno = EBADFD;
        goto error_disp;
    }
    else
    {
        /* ...set global registry listener */
        wl_registry_add_listener(display->registry, &registry_listener, display);
    }

    /* ...initialize inputs/outputs lists */
    wl_list_init(&display->outputs);
    wl_list_init(&display->inputs);

    /* ...initialize windows list */
    wl_list_init(&display->windows);

    /* ...create a display command/response lock */
    pthread_mutex_init(&display->lock, NULL);

    /* ...create polling structure */
    if ((display->efd = epoll_create(DISPLAY_EVENTS_NUM)) < 0)
    {
        TRACE(ERROR, _x("failed to create epoll: %m"));
        goto error_disp;
    }

    /* ...pre-initialize global Wayland interfaces */
    do
    {
        display->pending = 0, wl_display_roundtrip(display->display);
    } while (display->pending);

    /* ...initialize EGL */
    if (init_egl(display) < 0)
    {
        TRACE(ERROR, _x("EGL initialization failed: %m"));
        goto error_disp;
    }

    /* ...make current display context */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, display->egl.ctx);

    /* ...dump available GL extensions */
    TRACE(INIT, _b("GL version: %s"), (char *) glGetString(GL_VERSION));
    TRACE(INIT, _b("GL extension: %s"), (char *) glGetString(GL_EXTENSIONS));

    /* ...release display EGL context */
    eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    /* ...initialize thread attributes (joinable, default stack size) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    /* ...create Wayland dispatch thread */
    r = pthread_create(&display->thread, &attr, dispatch_thread, display);
    pthread_attr_destroy(&attr);

    if (r != 0)
    {
        TRACE(ERROR, _x("thread creation failed: %m"));
        goto error_egl;
    }

    /* ...wait until display thread starts? */
    TRACE(INIT, _b("Wayland display interface initialized"));

#ifdef SPACENAV_ENABLED
    /* ...initialize extra input devices */
    input_spacenav_init(display);

    /* ...joystick device requires start-up events generation (should be window-specific) */
    input_joystick_init(display, joystick_dev_name);
#endif

    /* ...doesn't look good, actually - don't want to start thread right here */
    return display;

error_egl:
    /* ...destroy EGL context */
    fini_egl(display);

error_disp:
    /* ...disconnect display */
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

error:
    return NULL;
}

/*******************************************************************************
 * Textures handling
 ******************************************************************************/

#if defined (EGL_HAS_IMG_EXTERNAL_EXT)

/* ...translate V4L2 pixel-format into native EGL-format */
static inline EGLint pixfmt_gst_to_egl(int format) {
    EGLint ret = -1;

    switch (format) {
    case GST_VIDEO_FORMAT_NV12:
        ret = EGL_NATIVE_PIXFORMAT_NV12;
        break;

    case GST_VIDEO_FORMAT_UYVY:
        ret = EGL_NATIVE_PIXFORMAT_UYVY;
        break;

    case GST_VIDEO_FORMAT_NV16:
        ret = EGL_NATIVE_PIXFORMAT_NV16;
        break;

    case GST_VIDEO_FORMAT_I420:
        ret = EGL_NATIVE_PIXFORMAT_I420;
        break;

    default:
        break;
    }

    BUG(ret == -1, _x("unsupported video format: %d"), format);
    return ret;
}

/* ...translate V4L2 pixel-format into DRM fourcc */
static inline int pixfmt_gst_to_drm_v4l2(int format)
{
    int ret;

    switch (format)
    {
    case GST_VIDEO_FORMAT_NV16:
        ret = DRM_FORMAT_NV16;
        break;

    case GST_VIDEO_FORMAT_NV12:
        ret = DRM_FORMAT_NV12;
        break;

    case GST_VIDEO_FORMAT_UYVY:
        ret = DRM_FORMAT_UYVY;
        break;

    case GST_VIDEO_FORMAT_YUY2:
        ret = DRM_FORMAT_YUYV;
        break;

    case GST_VIDEO_FORMAT_I420:
        ret = DRM_FORMAT_YUV420;
        break;

    case GST_VIDEO_FORMAT_BGRx:
        ret = DRM_FORMAT_ARGB8888;
        break;

    default:
        return -1;
    }
    BUG(ret == -1, _x("unsupported video format: %d"), format);
    return ret;
}

/* ...texture creation (in shared display context) */
static texture_data_t * texture_create_dma(vsink_meta_t* meta)
{
    display_data_t         *display = &__display;
    EGLDisplay              dpy = display->egl.dpy;
    texture_data_t         *texture;
    EGLImageKHR             image;
    EGLint                  attribs[MAX_ATTRIBUTES_COUNT];
    EGLint                  error;
    int                     idx = 0;
    int                     egl_format = 0;

    /* ...allocate texture data */
    texture = malloc(sizeof(*texture));
    if (texture == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory"));
        return NULL;
    }

    /* ...map format to the internal value */
    egl_format = pixfmt_gst_to_drm_v4l2(meta->format);
    BUG(egl_format < 0, _x("failed to map pixel format to DRM type"));

    /* ...get shared display EGL context */
    display_egl_ctx_get(display);

    /* ...save planes buffers pointers */
    memcpy(texture->data, meta->plane, sizeof(texture->data));

    attribs[idx++] = EGL_WIDTH;
    attribs[idx++] = meta->width;
    attribs[idx++] = EGL_HEIGHT;
    attribs[idx++] = meta->height;
    attribs[idx++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[idx++] = egl_format;

    switch(meta->format)
    {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV16:
        ASSERT(meta->n_dma == 2);

        attribs[idx++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[idx++] = meta->dmafd[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[idx++] = meta->offsets[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[idx++] = meta->width;

        attribs[idx++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        attribs[idx++] = meta->dmafd[1];
        attribs[idx++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        attribs[idx++] = meta->offsets[1];
        attribs[idx++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        attribs[idx++] = meta->width;

        break;

    case GST_VIDEO_FORMAT_I420:
        ASSERT(meta->n_dma == 1);

        attribs[idx++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[idx++] = meta->dmafd[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[idx++] = meta->offsets[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[idx++] = meta->width;

        break;

    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_YUY2:
        ASSERT(meta->n_dma == 1);

        attribs[idx++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[idx++] = meta->dmafd[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[idx++] = meta->offsets[0];
        attribs[idx++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[idx++] = meta->width * 2;

        break;

    default:
        BUG(1, _x("unsupported video format: %d"), meta->format);
    }

    attribs[idx++] = EGL_NONE;

    texture->pdata = image = eglCreateImageKHR(dpy,
                                               NULL,
                                               EGL_LINUX_DMA_BUF_EXT,
                                               NULL,
                                               attribs);

    if (!image)
    {
        error = eglGetError();
        TRACE(ERROR, _x("eglCreateImageKHR failed: %d (%#x)"), error, error);
        goto error;
    }

    /* ...allocate texture */
    glGenTextures(1, &texture->tex);

    /* ...bind texture to the output device */
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture->tex);

    error = eglGetError();
    if (error != EGL_SUCCESS)
    {
        goto error_tex;
    }

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    error = eglGetError();
    if (error != EGL_SUCCESS)
    {
        goto error_tex;
    }

    TRACE(DEBUG,
          _b("plane #0: image=%p, tex=%u, data=%p, format = %d, n_dma=%d"),
          image, texture->tex, texture->data[0], meta->format, meta->n_dma);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    /* ...release shared display context */
    display_egl_ctx_put(display);

    return texture;

error_tex:
    glDeleteTextures(1, &texture->tex);
    TRACE(ERROR, _x("gl call failed: %d (%#x)"), error, error);

error:
    free(texture);
    return NULL;
}
#endif

#if defined(EGL_HAS_IMG_EXTERNAL_EXT)
static void texture_set(int w, int h, int format, texture_data_t *texture)
{
    GLenum target = TEXTURE_TARGET;
    display_data_t *display = &__display;
    EGLDisplay dpy = display->egl.dpy;
    EGLImageKHR image;
    DEFINE_PIXMAP(pixmap,
                  w,
                  h,
                  pixfmt_gst_to_egl(format),
                  texture->data[0]);

    texture->size[0] = __pixfmt_image_size(w, h, format);
    image = eglCreateImageKHR(dpy,
                              EGL_NO_CONTEXT,
                              EGL_NATIVE_PIXMAP_KHR,
                              (EGLClientBuffer)&pixmap,
                              NULL);

    ASSERT(image != EGL_NO_IMAGE_KHR);

    glEGLImageTargetTexture2DOES(target, image);

    texture->pdata = image;
}
#else
static void texture_set(int w, int h, int format, texture_data_t *texture)
{
    GLenum target = TEXTURE_TARGET;
    GLint internal_format;

    texture->size[0] = __pixfmt_image_size(w, h, format);

    texture->pdata = NULL;
    switch (format)
    {
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_NV12:
        internal_format = GL_ALPHA;
        texture->format = GL_ALPHA;
        texture->width = w;
        texture->height = (h * 3) / 2;
        break;

    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_YUY2:
        internal_format = GL_RG8_EXT;
        texture->format = GL_RG_EXT;
        texture->width = w;
        texture->height = h;
        break;

    default:
        BUG(1, _x("not supported format: %d"), format);
        return;
    }

    glTexImage2D(target,
                 0,
                 internal_format,
                 texture->width,
                 texture->height,
                 0,
                 texture->format,
                 GL_UNSIGNED_BYTE,
                 texture->data[0]);
}
#endif

/* ...texture creation (in shared display context) */
texture_data_t * texture_create_pixmap(vsink_meta_t* meta)
{
    display_data_t *display = &__display;
    texture_data_t *texture;
    GLenum target = TEXTURE_TARGET;

    /* ...allocate texture data */
    texture = malloc(sizeof (*texture));
    if (texture == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory"));
        return NULL;
    }

    EGLContext ctx = eglGetCurrentContext();
    /* ...get display shared context */
    if (ctx == EGL_NO_CONTEXT)
    {
        display_egl_ctx_get(display);
    }

    /* ...allocate texture */
    glGenTextures(1, &texture->tex);

    /* ...save planes buffers pointers */
    memcpy(texture->data, meta->plane, sizeof (texture->data));

    /* ...bind texture to the output device */
    glBindTexture(target, texture->tex);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    texture_set(meta->width, meta->height, meta->format, texture);

    TRACE(DEBUG,
          _b("plane #0: image=%p, tex=%u, data=%p"),
          texture->pdata, texture->tex, texture->data[0]);

    glBindTexture(target, 0);

    /* ...release shared display context */
    (ctx == EGL_NO_CONTEXT ? display_egl_ctx_put(display) : 0);

    return texture;
}

/* ...texture creation (in shared display context) */
texture_data_t * texture_create(void* data)
{
    vsink_meta_t* meta = (vsink_meta_t*) data;
    TRACE(DEBUG, _x("is dma: %d, width: %d, height: %d"),
          meta->is_dma, meta->width, meta->height);

#if defined (EGL_HAS_IMG_EXTERNAL_EXT)
    if (meta->is_dma)
    {
        return texture_create_dma(meta);
    }
#endif
    return texture_create_pixmap(meta);
}

int texture_update(texture_data_t *texture)
{
    display_data_t *display = &__display;
    EGLint ret;
    GLenum target = TEXTURE_TARGET;
    EGLContext ctx = eglGetCurrentContext();

    /* ...get display shared context */
    if (ctx == EGL_NO_CONTEXT)
    {
        display_egl_ctx_get(display);
    }

    glBindTexture(target, texture->tex);
    glTexSubImage2D(target,
                    0,
                    0,
                    0,
                    texture->width,
                    texture->height,
                    texture->format,
                    GL_UNSIGNED_BYTE,
                    texture->data[0]);

    ret = glGetError();
    TRACE(DEBUG, _b("texture update from: %p, err: %#x"), texture->data[0], ret);

    glBindTexture(target, 0);

    if (ctx == EGL_NO_CONTEXT)
    {
        display_egl_ctx_put(display);
    }

    return ret;
}

/* ...destroy texture data */
void texture_destroy(texture_data_t *texture)
{
    display_data_t *display = &__display;
    EGLContext ctx = eglGetCurrentContext();

    /* ...get display shared context */
    if (ctx == EGL_NO_CONTEXT)
    {
        display_egl_ctx_get(display);
    }

    /* ...destroy textures */
    glDeleteTextures(1, &texture->tex);

    /* ...destroy EGL images */
    if (texture->pdata)
    {
        eglDestroyImageKHR(display->egl.dpy, texture->pdata);
    }

    /* ...release shared display context */
    if (ctx == EGL_NO_CONTEXT)
    {
        display_egl_ctx_put(display);
    }

    /* ...destroy texture structure */
    free(texture);
}
