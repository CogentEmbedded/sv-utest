/*******************************************************************************
 *
 * Camera interface for surround view application
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

#ifndef SV_SURROUNDVIEW_CAMERA_H
#define SV_SURROUNDVIEW_CAMERA_H

#include <gst/app/gstappsrc.h>

#include "netif.h"

/* ...opaque camera data handle */
typedef struct camera_data  camera_data_t;

/*******************************************************************************
 * Cameras mapping
 ******************************************************************************/

#define CAMERA_RIGHT                    0
#define CAMERA_LEFT                     1
#define CAMERA_FRONT                    2
#define CAMERA_REAR                     3

/*******************************************************************************
 * Camera interface
 ******************************************************************************/

typedef struct camera_callback
{
    /* ...buffer allocation hook */
    int       (*allocate)(void *data, GstBuffer *buffer);

    /* ...buffer processing hook */
    int       (*process)(void *data, int id, GstBuffer *buffer);

}   camera_callback_t;

/* ...camera data source callback structure */
typedef struct camera_source_callback
{
    /* ...end-of-stream signalization */
    void      (*eos)(void *data);

    /* ...packet processing hook (ethernet frame) */
    void      (*pdu)(void *data, int id, u8 *pdu, u16 len, u64 ts);

}   camera_source_callback_t;

/* ...camera set initialization function */
typedef GstElement * (*camera_init_func_t)(const camera_callback_t *cb,
                                           void *cdata,
                                           int n,
                                           int width,
                                           int height);


/*******************************************************************************
 * Entry points
 ******************************************************************************/

/* ...camera back-ends */
extern GstElement * video_stream_create(const camera_callback_t *cb,
                                        void *cdata,
                                        int n,
                                        int width,
                                        int height);

extern GstElement * camera_vin_create(const camera_callback_t *cb,
                                      void *cdata,
                                      int *vfd,
                                      int n,
                                      int width,
                                      int height);

extern camera_data_t * mjpeg_camera_create(int id,
                GstBuffer * (*get_buffer)(void *, int),
                void *cdata);

extern GstElement * mjpeg_camera_gst_element(camera_data_t *camera);

const char * video_stream_filename(void);

const char * video_stream_get_file(int i);

/* ...ethernet frame processing callback - tbd */
extern void camera_mjpeg_packet_receive(int id, u8 *pdu, u16 len, u64 ts);

#endif  /* SV_SURROUNDVIEW_CAMERA_H */
