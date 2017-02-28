/*******************************************************************************
 *
 * Common application definitions
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

#ifndef SV_SURROUNDVIEW_APP_H
#define SV_SURROUNDVIEW_APP_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include "camera.h"
#include "common.h"
#include "display.h"
#include "main.h"
#include "vsink.h"

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/

typedef struct app_data     app_data_t;
typedef struct track_desc   track_desc_t;
typedef struct track_list   track_list_t;

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/*******************************************************************************
 * Types definitions
 ******************************************************************************/

/* ...surround-view application data */
struct app_data
{
    /* ...main window */
    window_data_t      *window;

    /* ...main execution loop */
    GMainLoop          *loop;

    /* ...GStreamer pipeline */
    GstElement         *pipe;

    /* ...camera-set container */
    GstElement         *sv_camera, *fr_camera;

    /* ...frontal camera stream dimensions */
    int                 f_width, f_height;

    /* ...cairo transformation matrix */
    cairo_matrix_t      matrix;

    /* ...surround-view engine configuration data */
    sview_cfg_t        *sv_cfg;

    /* ...miscellaneous control flags */
    u32                 flags;

    /* ...pending output buffers (surround-view and frontal camera) */
    GQueue              render[CAMERAS_NUMBER + 1];

    /* ...mask of available frames (for surround view) */
    u32                 frames;

    /* ...surround-view library handle */
    sview_t            *sv;

    /* ...queues access lock */
    pthread_mutex_t     lock;

    /* ...surround-view / object-detection engine access lock */
    pthread_mutex_t     access;

    /* ...synchronous operation completion variable */
    pthread_cond_t      wait;

    /* ...frame number */
    u32                 frame_num;

    /* ...GUI widget handle */
    widget_data_t      *gui;

    u32 configuration;

    track_desc_t       *track_sv_live;

    track_list_t       *track_list;

};

/* ...double-linked list item */
struct track_list
{
    /* ...double-linked list pointers */
    struct track_list      *next, *prev;
};

#define TRACK_TYPE_SVIEW 0

#define TRACK_CAMERA_TYPE_VIN 0
#define TRACK_CAMERA_TYPE_MJPEG 1

/* ...track descriptor */
struct track_desc
{
    /* ...track list item */
    track_list_t        list;

    /* ...track type (0 - surround-view track, 1 - front-camera track) */
    int                 type;

    int                 camera_type;

    /* ...private track-type-specfic data */
    void               *priv;

    /* ...textual description of the track */
    char               *info;

    /* ...filename for offline playback - video/CAN data */
    char               *file;

    /* ...set of camera addresses */
    u8                  mac[CAMERAS_NUMBER][6];

    /* ...camera configuration */
    const char         *camera_cfg;

    char               *camera_names[CAMERAS_NUMBER];

    int                 pixformat;
};

/* ...mapping of cameras into texture indices (the order if left/right/front/rear) */
static inline int camera_id(int i)
{
    return (i < 2 ? i ^ 1 : i);
}

static inline int camera_idx(int id)
{
    return (id < 2 ? id ^ 1 : id);
}

/*******************************************************************************
 * Track setup
 ******************************************************************************/

/* ...switching to next/previous surround-view tracks */
extern track_desc_t * sview_track_live(void);
extern track_desc_t * sview_track_next(void);
extern track_desc_t * sview_track_prev(void);
extern track_desc_t * sview_track_current(void);

/* ...switching to next/previous object-detection tracks */
extern track_desc_t * objdet_track_live(void);
extern track_desc_t * objdet_track_next(void);
extern track_desc_t * objdet_track_prev(void);
extern track_desc_t * objdet_track_current(void);

/* ...prepare a runtime to start track playing */
extern int app_track_start(app_data_t *app, track_desc_t *track, int start);

/*******************************************************************************
 * Global configuration options
 ******************************************************************************/

/* ...output devices */
extern int __output_main;
extern int __output_transform;

/*******************************************************************************
 * Public module API
 ******************************************************************************/

/* ...surround view application data initialization */
extern app_data_t * app_init(display_data_t *display, sview_cfg_t *sv_cfg, int configuration);

/* ...camera interface attachment for surround-view mode */
extern int sview_camera_init(app_data_t *app, camera_init_func_t camera_init);

/* ...camera interface attachment for object-detection mode */
extern int objdet_camera_init(app_data_t *app, camera_init_func_t camera_init);

/* ...main application thread */
extern void * app_thread(void *arg);

/* ...end-of-stream indication */
extern void app_eos(app_data_t *app);

/* ...ethernet packet reception callback */
extern void app_packet_receive(app_data_t *app, int id, u8 *pdu, u16 len, u64 ts);

/*******************************************************************************
 * GUI commands processing
 ******************************************************************************/

/* ...enable spherical projection */
extern void sview_sphere_enable(app_data_t *app, int enable);

/* ...select live capturing mode */
extern void app_live_enable(app_data_t *app, int enable);

/* ...surround view scene preset */
extern void sview_set_view(app_data_t *app, int view);

/* ...enable undistort view */
extern void sview_set_undistort(app_data_t *app, int enable);

/* ...surround escape */
extern void sview_escape(app_data_t *app);

/* ...switch to next track */
extern void app_next_track(app_data_t *app);

/* ...switch to previous track */
extern void app_prev_track(app_data_t *app);

/* ...restart current track */
extern void app_restart_track(app_data_t *app);

/* ...enable surround-view scene showing */
extern void sview_scene_enable(app_data_t *app, int enable);

/* ...enable debugging output */
extern void app_debug_enable(app_data_t *app, int enable);

/* ...enable debugging output */
extern int app_debug_enabled(app_data_t *app);

/* ...close application */
extern void app_exit(app_data_t *app);

/*******************************************************************************
 * Graphical user interface
 ******************************************************************************/

/* ...GUI initialization function */
extern widget_data_t * gui_create(window_data_t *window, app_data_t *app);

/* ...draw GUI layer */
extern void gui_redraw(widget_data_t *widget, cairo_t *cr);

/* ...update GUI configuration */
extern void gui_config_update(widget_data_t *widget);

/*******************************************************************************
 * Operation control flags
 ******************************************************************************/

extern int app_has_multiple_sources(app_data_t *app);

/* ...surround-view scene show flag */
#define APP_FLAG_SVIEW                  (1 << 0)

/* ...output debugging info */
#define APP_FLAG_DEBUG                  (1 << 1)

/* ...live capturing mode */
#define APP_FLAG_LIVE                   (1 << 2)

/* ...switching to next track */
#define APP_FLAG_NEXT                   (1 << 3)

/* ...switching to previous track */
#define APP_FLAG_PREV                   (1 << 4)

/* ...end-of-stream processing */
#define APP_FLAG_EOS                    (1 << 5)

/* ...application termination request */
#define APP_FLAG_EXIT                   (1 << 6)

/* ...application has tracks file*/
#define APP_FLAG_FILE                   (1 << 7)

#endif  /* SV_SURROUNDVIEW_APP_H */
