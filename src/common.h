/*******************************************************************************
 *
 * Common definitions header
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

#ifndef SV_SURROUNDVIEW_COMMON_H
#define SV_SURROUNDVIEW_COMMON_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <pthread.h>
#include <sched.h>
#include <gst/gst.h>
#include <gst/video/video-format.h>
#include <linux/videodev2.h>

/*******************************************************************************
 * Global constants definitions
 ******************************************************************************/

/* ...total number of cameras */
#define CAMERAS_NUMBER          4

enum {CAMERA_IMAGE_WIDTH = 1280};
enum {CAMERA_IMAGE_HEIGHT = 800};

/*******************************************************************************
 * Forward types declarations
 ******************************************************************************/

/* ...forward declaration */
typedef struct netif_source     netif_source_t;
typedef struct netif_stream     netif_stream_t;
typedef struct fd_source        fd_source_t;
typedef struct timer_source     timer_source_t;

/*******************************************************************************
 * External functions
 ******************************************************************************/

/* ...file source operations */
extern fd_source_t * fd_source_create(const char *filename,
        gint prio,
        GSourceFunc func,
        gpointer user_data,
        GDestroyNotify notify,
        GMainContext *context);

extern int fd_source_get_fd(fd_source_t *fsrc);
extern void fd_source_suspend(fd_source_t *fsrc);
extern void fd_source_resume(fd_source_t *fsrc);
extern int fd_source_is_active(fd_source_t *fsrc);

/* ...timer source operations */
extern timer_source_t * timer_source_create(GSourceFunc func,
        gpointer user_data, GDestroyNotify notify, GMainContext *context);

extern int timer_source_get_fd(timer_source_t *tsrc);
extern void timer_source_start(timer_source_t *tsrc, u32 interval, u32 period);
extern void timer_source_stop(timer_source_t *tsrc);
extern int timer_source_is_active(timer_source_t *tsrc);

/*******************************************************************************
 * Camera support
 ******************************************************************************/

/* ...opaque declaration */
typedef struct camera_data  camera_data_t;

/*******************************************************************************
 * Global variables
 ******************************************************************************/

/* ...cameras MAC addresses */
extern u8   (*camera_mac_address)[6];

#if defined (JPU_SUPPORT)
/* ...jpeg decoder device name  */
extern char  *jpu_dev_name;
#endif

/* ...joystick device name */
extern char  *joystick_dev_name;

/*******************************************************************************
 * Surround-view application API
 ******************************************************************************/

typedef struct display_data     display_data_t;
typedef struct window_data      window_data_t;
typedef struct texture_data     texture_data_t;

/*******************************************************************************
 * Image format helpers
 ******************************************************************************/

/* ...mapping between Gstreamer and V4L2 pixel-formats */
static inline int __pixfmt_v4l2_to_gst(u32 format)
{
    switch (format)
    {
    case V4L2_PIX_FMT_RGB565:           return GST_VIDEO_FORMAT_RGB16;
    case V4L2_PIX_FMT_NV12:             return GST_VIDEO_FORMAT_NV12;
    case V4L2_PIX_FMT_NV16:             return GST_VIDEO_FORMAT_NV16;
    case V4L2_PIX_FMT_UYVY:             return GST_VIDEO_FORMAT_UYVY;
    case V4L2_PIX_FMT_YUYV:             return GST_VIDEO_FORMAT_YUY2;
    case V4L2_PIX_FMT_YUV420:           return GST_VIDEO_FORMAT_I420;
    case V4L2_PIX_FMT_GREY:             return GST_VIDEO_FORMAT_GRAY8;
    default:                            return -1;
    }
}

/* ...mapping between Gstreamer and V4L2 pixel-formats */
static inline int __gst_to_pixfmt_v4l2(u32 format)
{
    switch (format)
    {
    case GST_VIDEO_FORMAT_RGB16:            return V4L2_PIX_FMT_RGB565;
    case GST_VIDEO_FORMAT_NV12:             return V4L2_PIX_FMT_NV12;
    case GST_VIDEO_FORMAT_NV16:             return V4L2_PIX_FMT_NV16;
    case GST_VIDEO_FORMAT_UYVY:             return V4L2_PIX_FMT_UYVY;
    case GST_VIDEO_FORMAT_YUY2:             return V4L2_PIX_FMT_YUYV;
    case GST_VIDEO_FORMAT_I420:             return V4L2_PIX_FMT_YUV420;
    case GST_VIDEO_FORMAT_GRAY8:            return V4L2_PIX_FMT_GREY;
    default:                                return -1;
    }
}

static inline u32 __pixfmt_image_size(u32 w, u32 h, GstVideoFormat format)
{
    switch (format)
    {
    case GST_VIDEO_FORMAT_RGB16:        return w * h * 2;
    case GST_VIDEO_FORMAT_NV16:         return w * h * 2;
    case GST_VIDEO_FORMAT_NV12:         return w * h * 3 / 2;
    case GST_VIDEO_FORMAT_I420:         return w * h * 3 / 2;
    case GST_VIDEO_FORMAT_UYVY:         return w * h * 2;
    case GST_VIDEO_FORMAT_YUY2:         return w * h * 2;
    case GST_VIDEO_FORMAT_GRAY8:        return w * h;
    case GST_VIDEO_FORMAT_GRAY16_BE:    return w * h * 2;
    default:                            return 0;
    }
}

#endif  /* SV_SURROUNDVIEW_COMMON_H */
