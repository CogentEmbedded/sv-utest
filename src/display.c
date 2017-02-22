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

#define MODULE_TAG                      DISPLAY

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <cairo-gl.h>
#include <math.h>

#include "main.h"
#include "common.h"
#include "display.h"
#include "event.h"


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

/*******************************************************************************
 * Basic widgets support
 ******************************************************************************/

/* ...create widget */
widget_data_t * widget_create(window_data_t *window, widget_info_t *info, void *cdata)
{
    int w = window_get_widget(window)->width;
    int h = window_get_widget(window)->height;

    widget_data_t *widget;

    /* ...allocate data handle */
    CHK_ERR(widget = malloc(sizeof (*widget)), (errno = ENOMEM, NULL));

    /* ...initialize widget data */
    if (__widget_init(widget, window, w, h, info, cdata) < 0)
    {
        TRACE(ERROR, _x("widget initialization error: %m"));
        goto error;
    }

    return widget;

error:
    /* ...destroy widget data */
    free(widget);

    return NULL;
}

/* ...widget destructor */
void widget_destroy(widget_data_t *widget)
{
    widget_info_t *info = widget->info;

    /* ...invoke custom destructor function as needed */
    (info && info->destroy ? info->destroy(widget, widget->cdata) : 0);

    /* ...destroy cairo surface */
    cairo_surface_destroy(widget->cs);

    /* ...release data handle */
    free(widget);

    TRACE(INIT, _b("widget[%p] destroyed"), widget);
}

/* ...render widget content into given target context */
void widget_render(widget_data_t *widget, cairo_t *cr, float alpha)
{
    widget_info_t *info = widget->info;

    /* ...update widget content as needed */
    widget_update(widget);

    /* ...output widget content in current drawing context */
    cairo_save(cr);
    cairo_set_source_surface(cr, widget->cs, info->left, info->top);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

/* ...update widget content */
void widget_update(widget_data_t *widget)
{
    cairo_t *cr;

    /* ...do nothing if update is not required */
    TRACE(DEBUG, _b("widget state %d"), widget->dirty);

    if (!widget->dirty)
    {
        return;
    }

    /* ...clear dirty flag in advance */
    widget->dirty = 0;

    /* ...get curface drawing context */
    cr = cairo_create(widget->cs);

    /* ...update widget content */
    widget->info->draw(widget, widget->cdata, cr);

    /* ...make sure context is sane */
    if (TRACE_CFG(DEBUG) && cairo_status(cr) != CAIRO_STATUS_SUCCESS)
    {
        TRACE(ERROR, _x("widget[%p]: bad context: '%s'"),
                widget, cairo_status_to_string(cairo_status(cr)));
    }

    /* ...destroy context */
    cairo_destroy(cr);
}

/* ...schedule widget redrawing */
void widget_schedule_redraw(widget_data_t *widget)
{
    /* ...mark widget is dirty */
    widget->dirty = 1;

    /* ...schedule redrawing of the parent window */
    window_schedule_redraw(widget->window);
}

/* ...input event processing */
widget_data_t * widget_input_event(widget_data_t *widget, widget_event_t *event)
{
    widget_info_t *info = widget->info;

    return (info && info->event ? info->event(widget, widget->cdata, event) : NULL);
}

/* ...return current widget width */
int widget_get_width(widget_data_t *widget)
{
    return widget->width;
}

/* ...return current widget height */
int widget_get_height(widget_data_t *widget)
{
    return widget->height;
}

/* ...return left point */
int widget_get_left(widget_data_t *widget)
{
    return widget->left;
}

/* ...return top point */
int widget_get_top(widget_data_t *widget)
{
    return widget->top;
}

/* ...get cairo device associated with widget */
cairo_device_t * widget_get_cairo_device(widget_data_t *widget)
{
    return window_get_cairo_device(widget->window);
}

/* ...get parent window root widget */
widget_data_t * widget_get_parent(widget_data_t *widget)
{
    return window_get_widget(widget->window);
}

/*******************************************************************************
 * Window API
 ******************************************************************************/

/* ...transformation matrix processing */
static inline void window_set_transform_matrix(window_data_t *window,
        int *width, int *height, int fullscreen, u32 transform)
{
    cairo_matrix_t     *m = window_get_cmatrix(window);
    int                 w = *width, h = *height;

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

/* ...get window viewport data */
void window_get_viewport(window_data_t *window, int *w, int *h)
{
    switch(window_get_info(window)->transform)
    {
    case 90:
    case 270:
        /* ...swap vertical and horizontal dimensions */
        *w = window_get_height(window), *h = window_get_width(window);
        break;

    case 0:
    case 180:
        *w = window_get_width(window), *h = window_get_height(window);
        break;

    default:
        BUG(1, _x("invalid transformation: %u"), window_get_info(window)->transform);
    }
}

/* ...coordinates tranlsation */
void window_translate_coordinates(window_data_t *window, int x, int y, int *X, int *Y)
{
    int     w = window_get_width(window);
    int     h = window_get_height(window);

    switch (window_get_info(window)->transform)
    {
    case 0:
        *X = x, *Y = y;
        break;

    case 90:
        *X = y, *Y = w - x;
        break;

    case 180:
        *X = w - x, *Y = h - y;
        break;

    default:
        *X = w - y, *Y = x;
    }
}

/*******************************************************************************
 * Auxiliary widget helper functions
 ******************************************************************************/

/* ...create GL surface from PNG */
cairo_surface_t * widget_create_png(cairo_device_t *cairo, const char *path, int w, int h)
{
    cairo_surface_t    *image;
    cairo_surface_t    *cs;
    cairo_t            *cr;
    int                 W, H;

    /* ...create PNG surface */
    image = cairo_image_surface_create_from_png(path);
    if (__check_surface(image) != 0)
    {
        TRACE(ERROR, _x("failed to create image: %m"));
        return NULL;
    }
    else
    {
        W = cairo_image_surface_get_width(image);
        H = cairo_image_surface_get_height(image);
    }

    /* ...set widget dimensions */
    (w == 0 ? w = W : 0), (h == 0 ? h = H : 0);

    /* ...create new GL surface of requested size */
    cs = cairo_gl_surface_create(cairo, CAIRO_CONTENT_COLOR_ALPHA, w, h);
    if (__check_surface(cs) != 0)
    {
        TRACE(ERROR, _x("failed to create %u*%u GL surface: %m"), w, h);
        cs = NULL;
        goto out;
    }

    /* ...fill GL-surface */
    cr = cairo_create(cs);
    cairo_scale(cr, (double)w / W, (double)h / H);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    TRACE(DEBUG, _b("created GL-surface [%d*%d] from '%s' [%d*%d]"), w, h, path, W, H);

out:
    /* ...release scratch image surface */
    cairo_surface_destroy(image);

    return cs;
}

/* ...check surface status */
int __check_surface(cairo_surface_t *cs)
{
    cairo_status_t status;

    switch (status = cairo_surface_status(cs))
    {
        case CAIRO_STATUS_SUCCESS:
            return 0;
        case CAIRO_STATUS_READ_ERROR: errno = EINVAL;
            break;
        case CAIRO_STATUS_FILE_NOT_FOUND: errno = ENOENT;
            break;
        default: errno = ENOMEM;
            break;
    }

    TRACE(ERROR, _b("cairo surface error: '%s'"), cairo_status_to_string(status));

    return -errno;
}

/* ...get surface width */
int widget_image_get_width(cairo_surface_t *cs)
{
    return cairo_gl_surface_get_width(cs);
}

/* ...get surface height */
int widget_image_get_height(cairo_surface_t *cs)
{
    return cairo_gl_surface_get_height(cs);
}



/*******************************************************************************
 * Window base API
 ******************************************************************************/

/* ...return current window width */
int window_get_width(window_data_t *window)
{
    return ((window_data_base_t*)window)->widget.width;
}

/* ...return current window height */
int window_get_height(window_data_t *window)
{
    return ((window_data_base_t*)window)->widget.height;
}

widget_data_t *window_get_widget(window_data_t *window)
{
    return &((window_data_base_t*)window)->widget;
}

const window_info_t *window_get_info(window_data_t *window)
{
    return ((window_data_base_t*)window)->info;
}

cairo_device_t * window_get_cairo_device(window_data_t *window)
{
    return ((window_data_base_t*)window)->cairo;
}

cairo_matrix_t * window_get_cmatrix(window_data_t *window)
{
    return &((window_data_base_t*)window)->cmatrix;
}

/*******************************************************************************
 * Auxiliary frame-rate calculation functions
 ******************************************************************************/

/* ...reset FPS calculator */
void window_frame_rate_reset(window_data_t *window)
{
    /* ...reset accumulator and timestamp */
    ((window_data_base_t*)window)->fps_acc = 0;
    ((window_data_base_t*)window)->fps_ts = 0;
}

/* ...update FPS calculator */
float window_frame_rate_update(window_data_t *window)
{
    u32 ts_0, ts_1, delta, acc;
    float fps;

    /* ...get current timestamp for a window frame-rate calculation */
    delta = (ts_1 = get_time_usec()) - (ts_0 = ((window_data_base_t*)window)->fps_ts);

    /* ...check if accumulator is initialized */
    if ((acc = ((window_data_base_t*)window)->fps_acc) == 0)
    {
        if (ts_0 != 0)
        {
            /* ...initialize accumulator */
            acc = delta << 4;
        }
    } else {
        /* ...accumulator is setup already; do exponential averaging */
        acc += delta - ((acc + 8) >> 4);
    }

    /* ...calculate current frame-rate */
    if ((fps = (acc ? 1e+06 / ((acc + 8) >> 4) : 0)) != 0)
    {
        TRACE(DEBUG, _b("delta: %u, acc: %u, fps: %f"), delta, acc, fps);
    }

    /* ...update timestamp and accumulator values */
    ((window_data_base_t*)window)->fps_acc = acc, ((window_data_base_t*)window)->fps_ts = ts_1;

    return fps;
}

/* ...schedule redrawal of the window */
void window_schedule_redraw(window_data_t *window)
{
    /* ...acquire window lock */
    pthread_mutex_lock(&((window_data_base_t*)window)->lock);

    /* ...check if we don't have a flag already */
    if ((((window_data_base_t*)window)->flags & WINDOW_FLAG_REDRAW) == 0)
    {
        /* ...set a flag */
        ((window_data_base_t*)window)->flags |= WINDOW_FLAG_REDRAW;

        /* ...and kick processing thread */
        pthread_cond_signal(&((window_data_base_t*)window)->wait);

        TRACE(DEBUG, _b("schedule window[%p] redraw"), window);
    }

    /* ...release window access lock */
    pthread_mutex_unlock(&((window_data_base_t*)window)->lock);
}

void window_reinit_bv(window_data_t *window)
{
    /* ...acquire window lock */

    TRACE(INIT, _b("window[%p]: surround view bv reinitialize"), window);
    pthread_mutex_lock(&((window_data_base_t*)window)->lock);

    /* ...check if we don't have a flag already */
    if ((((window_data_base_t*)window)->flags & WINDOW_BV_REINIT) == 0)
    {
        /* ...set a flag */
        ((window_data_base_t*)window)->flags |= WINDOW_BV_REINIT;

        /* ...and kick processing thread */
        pthread_cond_signal(&((window_data_base_t*)window)->wait);

        TRACE(DEBUG, _b("window[%p]: surround view bv reinitialize"), window);
    }

    /* ...release window access lock */
    pthread_mutex_unlock(&((window_data_base_t*)window)->lock);
}


