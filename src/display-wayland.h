/*******************************************************************************
 *
 * Display support (Wayland-client)
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

#ifndef SV_SURROUNDVIEW_DISPLAY_WAYLAND_H
#define SV_SURROUNDVIEW_DISPLAY_WAYLAND_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cairo-gl.h>

#include "display.h"

/*******************************************************************************
 * EGL functions binding (make them global; create EGL adaptation layer - tbd)
 ******************************************************************************/

/* ...EGL extensions */
extern PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
extern PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC eglSwapBuffersWithDamageEXT;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
extern PFNGLMAPBUFFEROESPROC glMapBufferOES;
extern PFNGLUNMAPBUFFEROESPROC glUnmapBufferOES;

extern PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
extern PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
extern PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;

/*******************************************************************************
 * Types definitions
 ******************************************************************************/

/* ...return EGL surface associated with window */
extern EGLSurface window_egl_surface(window_data_t *window);
extern EGLContext window_egl_context(window_data_t *window);

/* ...associated cairo surface handling */
extern cairo_t * window_get_cairo(window_data_t *window);
extern void window_put_cairo(window_data_t *window, cairo_t *cr);
extern int window_set_invisible(window_data_t *window);

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...get current EGL configuration data */
extern egl_data_t  * display_egl_data(display_data_t *display);

/*******************************************************************************
 * Miscellaneous helpers for 2D-graphics
 ******************************************************************************/

/* ...PNG images handling */
extern cairo_surface_t * widget_create_png(cairo_device_t *cairo, const char *path, int w, int h);
extern int widget_image_get_width(cairo_surface_t *cs);
extern int widget_image_get_height(cairo_surface_t *cs);

/* ...simple text output - tbd */

#endif  /* SV_SURROUNDVIEW_DISPLAY_WAYLAND_H */
