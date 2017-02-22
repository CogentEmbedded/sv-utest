/*******************************************************************************
 *
 * MJPEG camera support header
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

#ifndef SV_SURROUNDVIEW_CAMERA_MJPEG_H
#define SV_SURROUNDVIEW_CAMERA_MJPEG_H

#include <gst/gst.h>

#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Camera initialization hook */
    GstElement * camera_mjpeg_create(const camera_callback_t *cb,
                                     void *cdata,
                                     int n,
                                     int width,
                                     int height);

    /* ...offline operation mode packet processing callback */
    void camera_packet_receive(camera_data_t *camera,
                               u8 *pdu,
                               u16 length,
                               u64 ts);

    /* ...open capturing file for replay */
    void * pcap_replay(const char *filename, void *cb, void *cdata, int );

    void pcap_stop(void *arg);

    /* ...open capturing file for replay */
    void * blf_replay(char *filename, void *cb, void *cdata);

    void blf_stop(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SV_SURROUNDVIEW_CAMERA_MJPEG_H */

