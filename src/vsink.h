/*******************************************************************************
 *
 * Gstreamer video sink definitions
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

#ifndef SV_SURROUNDVIEW_VSINK_H
#define SV_SURROUNDVIEW_VSINK_H

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>

/*******************************************************************************
 * Opaque type declaration
 ******************************************************************************/

typedef struct video_sink   video_sink_t;

/*******************************************************************************
 * Custom buffer metadata
 ******************************************************************************/

/* ...metadata structure */
typedef struct vsink_meta
{
    GstMeta             meta;

    /* ...user-specific private data */
    void               *priv;

    /* ...buffer dimensions */
    int                 width, height;

    /* ...planes number... */
    int                 n_planes;

    /* ...video format type */
    GstVideoFormat      format;

    /* ...plane buffers (data pointers) */
    void               *plane[GST_VIDEO_MAX_PLANES];

    /* is DMA? */
    int is_dma;
    /* ...Number of DMA fds... */
    int n_dma;
    /* ...plane buffers DMA file-descriptors */
    int                 dmafd[GST_VIDEO_MAX_PLANES];

    /* ...plane offsets for  DMA file-descriptors */
    int                 offsets[GST_VIDEO_MAX_PLANES];

    /* ...sink pointer */
    video_sink_t       *sink;

}   vsink_meta_t;

/* ...metadata API type accessor */
extern GType vsink_meta_api_get_type(void);
#define VSINK_META_API_TYPE             (vsink_meta_api_get_type())

/* ...metadata information handle accessor */
extern const GstMetaInfo *vsink_meta_get_info(void);
#define VSINK_META_INFO                 (vsink_meta_get_info())

extern GQuark gst_buffer_vsink_quark(void);

/* ...get access to the buffer metadata */
#define gst_buffer_get_vsink_meta(b)                                 \
    ((vsink_meta_t *)gst_mini_object_get_qdata(GST_MINI_OBJECT((b)), \
                                               gst_buffer_vsink_quark()))

/* ...attach metadata to the buffer */
#define gst_buffer_add_vsink_meta(b)                        \
    ({                                                      \
        vsink_meta_t *__m = calloc(1, sizeof(*__m));        \
        gst_mini_object_set_qdata(GST_MINI_OBJECT((b)),     \
                                  gst_buffer_vsink_quark(), \
                                  __m,                      \
                                  NULL);                    \
        __m;                                                \
    })


/*******************************************************************************
 * Video sink node for OMX/GL interface
 ******************************************************************************/

/* ...callbacks associated with video-sink */
typedef struct vsink_callback
{
    /* ...buffer allocation callback */
    int         (*allocate)(GstBuffer *buffer, void *data);

    /* ...buffer processing callback */
    int         (*process)(GstBuffer *buffer, void *data);

    /* ...preroll buffer processing */
    int         (*preroll)(GstBuffer *buffer, void *data);

    /* ...buffer destruction callback */
    void        (*destroy)(GstBuffer *buffer, void *data);

}   vsink_callback_t;

/* ...custom video sink node creation */
extern GstElement * video_sink_create(GstCaps *caps,
                                      const vsink_callback_t *cb,
                                      void *cdata);

#endif  /* SV_SURROUNDVIEW_VSINK_H */
