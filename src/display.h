/*******************************************************************************
 *
 * Display support
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

#ifndef SV_SURROUNDVIEW_DISPLAY_H
#define SV_SURROUNDVIEW_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <sys/types.h>
#include <cairo.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "event.h"

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/

typedef struct display_data         display_data_t;
typedef struct window_info          window_info_t;
typedef struct window_data          window_data_t;
typedef struct window_data_base     window_data_base_t;
typedef struct widget_info          widget_info_t;
typedef struct widget_data          widget_data_t;
typedef struct texture_data         texture_data_t;
typedef struct texture_platform     texture_platform_t;


/*******************************************************************************
 * EGL functions binding (make them global; create EGL adaptation layer - tbd)
 ******************************************************************************/

/* ...EGL configuration data */
typedef struct egl_data
{
    /* ...EGL display handle associated with current wayland display */
    EGLDisplay              dpy;

    /* ...shared EGL context */
    EGLContext              ctx;

    /* ...current EGL configuration */
    EGLConfig               conf;

}   egl_data_t;

/*******************************************************************************
 * Generic widgets support
 ******************************************************************************/

/* ...widget descriptor data */
typedef struct widget_info
{
    /* ...coordinates within parent window/widget */
    int                 left, top, width, height;

    /* ...initialization function */
    int               (*init)(widget_data_t *widget, void *cdata);

    /* ...redraw hook */
    void              (*draw)(widget_data_t *widget, void *cdata, cairo_t *cr);

    /* ...input event processing */
    widget_data_t *   (*event)(widget_data_t *widget, void *cdata, widget_event_t *event);

    /* ...deinitialization function? - need that? */
    void              (*destroy)(widget_data_t *widget, void *cdata);

}   widget_info_t;

/* ...widget data structure */
struct widget_data
{
    /* ...reference to owning window */
    window_data_t              *window;

    /* ...reference to parent widget */
    widget_data_t              *parent;

    /* ...pointer to the user-provided widget info */
    widget_info_t              *info;

    /* ...widget client data */
    void                       *cdata;

    /* ...cairo surface associated with this widget */
    cairo_surface_t            *cs;

    /* ...actual widget dimensions */
    int                         left, top, width, height;

    /* ...surface update request */
    int                         dirty;
};


/*******************************************************************************
 * Window processing flags
 ******************************************************************************/

/* ...redraw command pending */
#define WINDOW_FLAG_REDRAW              (1 << 0)

/* ...termination command pending */
#define WINDOW_FLAG_TERMINATE           (1 << 1)

#define WINDOW_BV_REINIT                (1 << 2)

/*******************************************************************************
 * Window base data structure
 ******************************************************************************/

/* ...window configuration data */
struct window_info
{
    /* ...window title */
    const char         *title;

    /* ...fullscreen mode */
    int                fullscreen;

    /* ...dimensions */
    uint32_t            width, height;

    /* ...output device id */
    uint32_t            output;

    /* ...window transformation */
    uint32_t            transform;

    /* ...context initialization function */
    int               (*init)(display_data_t *, window_data_t *, void *);

    /* ...resize hook */
    void              (*resize)(display_data_t *, void *);

    /* ...drawing completion callback */
    void              (*redraw)(display_data_t *, void *);

    void              (*init_bv)(display_data_t *, void *);

    /* ...custom context destructor */
    void              (*destroy)(window_data_t *, void *);
};

/* ...output window data */
struct window_data_base {
    /* ...root widget data (must be first) */
    widget_data_t widget;

    /* ...reference to a display data */
    display_data_t *display;

    /* ...cairo device associated with current window context */
    cairo_device_t *cairo;

    /* ...current cairo transformation matrix (screen rotation) */
    cairo_matrix_t cmatrix;

    /* ...saved cairo program */
    int cprog;

    /* ...window information */
    const window_info_t *info;

    /* ...client data for a callback */
    void *cdata;

    /* ...internal data access lock */
    pthread_mutex_t lock;

    /* ...conditional variable for rendering thread */
    pthread_cond_t wait;

    /* ...window rendering thread */
    pthread_t thread;

    /* ...processing flags */
    u32 flags;

    /* ...frame-rate calculation */
    u32 fps_ts;
    u32 fps_acc;
};

/*******************************************************************************
 * External textures support
 ******************************************************************************/

/* ...external texture data */
struct texture_data
{
    /* ...drawable EGL pixmap */
    texture_platform_t *pdata;

    /* ...GL texture index (in shared display EGL context) */
    uint32_t            tex;

    /* ...buffer data pointer (per-plane; up to 3 planes) */
    void               *data[3];

    /* ...buffer plane size */
    uint32_t            size[3];

    /* ...texture format */
    int                 format;

    /* ...texture width */
    int                 width;

    /* ...texture height */
    int                 height;
};

/* ...connect to a display */
extern display_data_t * display_create(void);

/* ...cairo device accessor */
extern cairo_device_t  * __display_cairo_device(display_data_t *display);
/* ...window creation/destruction */
extern window_data_t * window_create(display_data_t *display,
        window_info_t *info, widget_info_t *info2, void *data);

extern void window_destroy(window_data_t *window);

/* ...schedule window redrawal */
extern void window_schedule_redraw(window_data_t *window);
extern void window_draw(window_data_t *window);
extern void window_clear(window_data_t *window);


/* ...bv reinitialize */
void window_reinit_bv(window_data_t *window);

/* ...window getters... */
extern int window_get_width(window_data_t *window);
extern int window_get_height(window_data_t *window);
extern widget_data_t *window_get_widget(window_data_t *window);
extern const window_info_t *window_get_info(window_data_t *window);
extern cairo_t * window_get_cairo(window_data_t *window);
extern void window_put_cairo(window_data_t *window, cairo_t *cr);
extern cairo_device_t * window_get_cairo_device(window_data_t *window);
extern cairo_matrix_t * window_get_cmatrix(window_data_t *window);

/* ...auxiliary helpers */
extern void window_frame_rate_reset(window_data_t *window);
extern float window_frame_rate_update(window_data_t *window);

extern int __check_surface(cairo_surface_t *cs);

/* ...external textures handling */
extern texture_data_t * texture_create(void *data);
extern int texture_update(texture_data_t *texture);

extern void texture_destroy(texture_data_t *texture);

/*******************************************************************************
 * Generic widgets support
 ******************************************************************************/

/* ...widget creation/destruction */
extern widget_data_t * widget_create(window_data_t *window, widget_info_t *info, void *cdata);
extern void widget_destroy(widget_data_t *widget);

/* Platform-dependent declaration */
int __widget_init(widget_data_t *widget,
        window_data_t *window, int W, int H, widget_info_t *info, void *cdata);

/* ...widget rendering */
extern void widget_render(widget_data_t *widget, cairo_t *cr, float alpha);
extern void widget_update(widget_data_t *widget);
extern void widget_schedule_redraw(widget_data_t *widget);
extern cairo_device_t * widget_get_cairo_device(widget_data_t *widget);

/* ...input event processing */
extern widget_data_t * widget_input_event(widget_data_t *widget, widget_event_t *event);
extern widget_data_t * widget_get_parent(widget_data_t *widget);

/* ...helpers */
extern int widget_get_left(widget_data_t *widget);
extern int widget_get_top(widget_data_t *widget);
extern int widget_get_width(widget_data_t *widget);
extern int widget_get_height(widget_data_t *widget);
extern void window_get_viewport(window_data_t *window, int *w, int *h);
extern void window_translate_coordinates(window_data_t *window, int x, int y, int *X, int *Y);

#ifdef __cplusplus
}
#endif

#endif /* SV_SURROUNDVIEW_DISPLAY_H */

