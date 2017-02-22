/*******************************************************************************
 *
 * VIN LVDS cameras backend
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

#define MODULE_TAG                      VIN

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <linux/videodev2.h>

#include <sv/svlib.h>

#include "main.h"
#include "common.h"
#include "camera.h"
#include "vsink.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);
TRACE_TAG(BUFFER, 1);

/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

/* ...individual camera buffer pool size */
#define VIN_BUFFER_POOL_SIZE            8

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/* ...buffer description */
typedef struct vin_buffer
{
    /* ...data pointer */
    void               *data;

    /* ...memory offset */
    u32                 offset;

    /* ...buffer length */
    u32                 length;

    /* ...associated GStreamer buffer */
    GstBuffer          *buffer;

}   vin_buffer_t;

/* ...particular VIN device data */
typedef struct vin_device
{
    /* ...file descriptor */
    int                 vfd;

    /* ...buffer pool */
    vin_buffer_t        pool[VIN_BUFFER_POOL_SIZE];

    /* ...input buffer waiting conditional */
    pthread_cond_t      wait;

}   vin_device_t;

/* ...decoder data structure */
typedef struct vin_decoder
{
    /* ...GStreamer bin element for pipeline handling */
    GstElement                 *bin;

    /* ...VIN devices */
    vin_device_t               *dev;

    /* ...total number of devices in bin */
    int                         number;

    /* ...number of output buffers queued to VIN */
    int                         output_count;

    /* ...number of output buffers submitted to the application */
    int                         output_busy;

    /* ...decoder activity state */
    int                         active;

    /* ...queue access lock */
    pthread_mutex_t             lock;

    /* ...decoding thread - tbd - make it a data source for GMainLoop? */
    pthread_t                   thread;

    /* ...decoding thread conditional variable */
    pthread_cond_t              wait;

    /* ...output buffers flushing conditional */
    pthread_cond_t              flush_wait;

    /* ...application-provided callback */
    const camera_callback_t    *cb;

    /* ...application callback data */
    void                       *cdata;

}   vin_decoder_t;

/*******************************************************************************
 * Custom buffer metadata implementation
 ******************************************************************************/

/* ...metadata structure */
typedef struct vin_meta
{
    GstMeta             meta;

    /* ...camera identifier */
    int                 camera_id;

    /* ...buffer index in the camera pool */
    int                 index;

}   vin_meta_t;

/* ...metadata API type accessor */
extern GType vin_meta_api_get_type(void);
#define VIN_META_API_TYPE               (vin_meta_api_get_type())

/* ...metadata information handle accessor */
extern const GstMetaInfo *vin_meta_get_info(void);
#define VIN_META_INFO                   (vin_meta_get_info())

/* ...get access to the buffer metadata */
#define gst_buffer_get_vin_meta(b)      \
    ((vin_meta_t *)gst_buffer_get_meta((b), VIN_META_API_TYPE))

/* ...attach metadata to the buffer */
#define gst_buffer_add_vin_meta(b)    \
    ((vin_meta_t *)gst_buffer_add_meta((b), VIN_META_INFO, NULL))

/* ...metadata type registration */
GType vin_meta_api_get_type(void)
{
    static volatile GType type;
    static const gchar *tags[] = { GST_META_TAG_VIDEO_STR, GST_META_TAG_MEMORY_STR, NULL };

    if (g_once_init_enter(&type))
    {
        GType _type = gst_meta_api_type_register("VinDecMetaAPI", tags);
        g_once_init_leave(&type, _type);
    }

    return type;
}

/* ...low-level interface */
static gboolean vin_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
    vin_meta_t     *_meta = (vin_meta_t *) meta;

    /* ...reset fields */
    memset(&_meta->meta + 1, 0, sizeof(vin_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata transformation */
static gboolean vin_meta_transform(GstBuffer *transbuf, GstMeta *meta,
        GstBuffer *buffer, GQuark type, gpointer data)
{
    vin_meta_t     *_meta = (vin_meta_t *) meta, *_tmeta;

    /* ...add metadata for a transformed buffer */
    _tmeta = gst_buffer_add_vin_meta(transbuf);

    /* ...just copy data regardless of transform type? */
    memcpy(&_tmeta->meta + 1, &_meta->meta + 1, sizeof(vin_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata release */
static void vin_meta_free(GstMeta *meta, GstBuffer *buffer)
{
    vin_meta_t     *_meta = (vin_meta_t *) meta;

    /* ...anything to destroy? - tbd */
    TRACE(DEBUG, _b("free metadata %p"), _meta);
}

/* ...register metadata implementation */
const GstMetaInfo * vin_meta_get_info(void)
{
    static const GstMetaInfo *meta_info = NULL;

    if (g_once_init_enter(&meta_info))
    {
        const GstMetaInfo *mi = gst_meta_register(
            VIN_META_API_TYPE,
            "VinDecMeta",
            sizeof(vin_meta_t),
            vin_meta_init,
            vin_meta_free,
            vin_meta_transform);

        g_once_init_leave (&meta_info, mi);
    }

    return meta_info;
}

/*******************************************************************************
 * V4L2 VIN interface helpers
 ******************************************************************************/

/* ...check video device capabilities */
static inline int __vin_check_caps(int vfd)
{
    struct v4l2_capability  cap;
    u32                     caps;

    /* ...query device capabilities */
    CHK_API(ioctl(vfd, VIDIOC_QUERYCAP, &cap));
    caps = cap.device_caps;

    /* ...check for a required capabilities */
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE))
    {
        TRACE(ERROR, _x("single-planar output expected: %X"), caps);
        return -1;
    }
    else if (!(caps & V4L2_CAP_STREAMING))
    {
        TRACE(ERROR, _x("streaming I/O is expected: %X"), caps);
        return -1;
    }

    /* ...all good */
    return 0;
}

/* ...prepare VIN module for operation */
static inline int vin_set_formats(int vfd, int width, int height, u32 format)
{
    struct v4l2_format  fmt;

    /* ...set output format (single-plane NV12/NV16/UYVY? - tbd) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = format;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    CHK_API(ioctl(vfd, VIDIOC_S_FMT, &fmt));

    return 0;
}

/* ...start/stop streaming on specific V4L2 device */
static inline int vin_streaming_enable(int vfd, int enable)
{
    int     type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    return CHK_API(ioctl(vfd, (enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF), &type));
}

/* ...allocate buffer pool */
static inline int vin_allocate_buffers(int vfd, vin_buffer_t *pool, int num)
{
    struct v4l2_requestbuffers  reqbuf;
    struct v4l2_buffer          buf;
    int                         j;

    /* ...all buffers are allocated by kernel */
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = num;
    CHK_API(ioctl(vfd, VIDIOC_REQBUFS, &reqbuf));
    CHK_ERR(reqbuf.count == (u32)num, -(errno = ENOMEM));

    /* ...prepare query data */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (j = 0; j < num; j++)
    {
        vin_buffer_t   *_buf = &pool[j];

        /* ...query buffer */
        buf.index = j;
        CHK_API(ioctl(vfd, VIDIOC_QUERYBUF, &buf));
        _buf->length = buf.length;
        _buf->offset = buf.m.offset;
        _buf->data = mmap(NULL, _buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, _buf->offset);
        CHK_ERR(_buf->data != MAP_FAILED, -errno);

        TRACE(DEBUG, _b("output-buffer-%d mapped: %p[%08X] (%u bytes)"),
                j, _buf->data, _buf->offset, _buf->length);
    }

    /* ...start streaming as soon as we allocated buffers */
    CHK_API(vin_streaming_enable(vfd, 1));

    TRACE(BUFFER, _b("buffer-pool allocated (%u buffers)"), num);

    return 0;
}

/* ...allocate output/capture buffer pool */
static inline int vin_destroy_buffers(int vfd, vin_buffer_t *pool, int num)
{
    struct v4l2_requestbuffers  reqbuf;
    int                         j;

    /* ...stop streaming before doing anything */
    CHK_API(vin_streaming_enable(vfd, 0));

    /* ...unmap all buffers */
    for (j = 0; j < num; j++)
    {
        munmap(pool[j].data, pool[j].length);
    }

    /* ...release kernel-allocated buffers */
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = 0;
    CHK_API(ioctl(vfd, VIDIOC_REQBUFS, &reqbuf));

    TRACE(BUFFER, _b("buffer-pool destroyed (%d buffers)"), num);

    return 0;
}

/* ...enqueue output buffer */
static inline int vin_output_buffer_enqueue(int vfd, int j)
{
    struct v4l2_buffer  buf;

    /* ...set buffer parameters */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = j;
    CHK_API(ioctl(vfd, VIDIOC_QBUF, &buf));

    TRACE(BUFFER, _b("output-buffer #%d queued"), j);
    return 0;
}

/* ...dequeue input buffer */
static inline int vin_output_buffer_dequeue(int vfd)
{
    struct v4l2_buffer  buf;

    /* ...set buffer parameters */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    CHK_API(ioctl(vfd, VIDIOC_DQBUF, &buf));

    TRACE(BUFFER, _b("output-buffer #%d dequeued"), buf.index);
    return buf.index;
}


/*******************************************************************************
 * V4L2 decoder thread
 ******************************************************************************/

/* ...submit buffer to the device (called with a decoder lock held) */
static inline int __submit_buffer(vin_decoder_t *dec, int i, int j)
{
    /* ...submit a buffer */
    CHK_API(vin_output_buffer_enqueue(dec->dev[i].vfd, j));

    TRACE(BUFFER, _b("camera-%d: enqueue buffer #%d"), i, j);

    /* ...notify decoder thread about buffer queueing */
    (dec->output_count++ == 0 ? pthread_cond_signal(&dec->wait) : 0);

    return 0;
}

/* ...buffer processing function */
static inline int __decoder_process(vin_decoder_t *dec, int i)
{
    vin_device_t   *dev = &dec->dev[i];
    GstBuffer      *buffer;
    vin_buffer_t   *buf;
    int             j;

    /* ...get internal data access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...get buffer from a device */
    CHK_API(j = vin_output_buffer_dequeue(dev->vfd));

    /* ...atomically decrement number of queued outputs */
    dec->output_count--;

    /* ...pass buffer to the application */
    buffer = (buf = &dev->pool[j])->buffer;

    TRACE(BUFFER,
          _b("camera-%d: dequeued buffer %p #%d (queued: %d), refcount=%d"),
          i, buffer, j, dec->output_count, GST_MINI_OBJECT_REFCOUNT(buffer));

    if (dec->active)
    {
        /* ...set decoding/presentation timestamp */
        GST_BUFFER_PTS(buffer) = gst_clock_get_time(GST_ELEMENT_CLOCK(dec->bin));
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);

        /* ...increment number of buffers submitted */
        dec->output_busy++;

        TRACE(BUFFER,
              _b("camera-%d: submit buffer %p #%d (busy=%d), refcount=%d"),
              i, buffer, j, dec->output_busy, GST_MINI_OBJECT_REFCOUNT(buffer));

        /* ...release queue access lock */
        pthread_mutex_unlock(&dec->lock);

        /* ...pass output buffer to application */
        CHK_API(dec->cb->process(dec->cdata, i, buffer));
    }
    else
    {
        TRACE(BUFFER,
              _b("camera-%d: drop buffer %p #%d (busy=%d), refcount=%d"),
              i, buffer, j, dec->output_busy, GST_MINI_OBJECT_REFCOUNT(buffer));

        /* ...release queue lock */
        pthread_mutex_unlock(&dec->lock);
    }

    /* ...drop the reference (buffer is now owned by application) */
    gst_buffer_unref(buffer);
    TRACE(BUFFER, _b("decoder process exit: buffer: %p, refcount=%d"), buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
    return 0;
}

/* ...decoding thread */
static void * vin_decode_thread(void *arg)
{
    vin_decoder_t      *dec = arg;
    int                 n = dec->number;
    struct pollfd      *pfd;
    int                 i;

    /* ...allocate poll descriptors */
    CHK_ERR(pfd = malloc(sizeof(*pfd) * n), (errno = ENOMEM, NULL));

    /* ...prepare polling descriptors */
    for (i = 0; i < n; i++)
    {
        pfd[i].fd = dec->dev[i].vfd;
        pfd[i].events = POLLIN;
    }

    /* ...start processing loop */
    while (1)
    {
        int     r;

        /* ...check if we have output buffers queued */
        pthread_mutex_lock(&dec->lock);

        /* ...wait until we have any buffers queued */
        while (dec->active && !dec->output_count)
        {
            pthread_cond_wait(&dec->wait, &dec->lock);
        }

        /* ...release lock */
        pthread_mutex_unlock(&dec->lock);

        /* ...check if thread needs to be terminated (VIN doesn't return all buffers???) */
        if (!dec->active)
        {
            break;
        }

        TRACE(0, _b("start waiting..."));

        /* ...wait for a decoding completion */
        if ((r = poll(pfd, n, -1)) < 0)
        {
            /* ...ignore soft interruption (e.g. from gdb) */
            if (errno == EINTR) continue;
            TRACE(ERROR, _x("poll failed: %m"));
            break;
        }

        TRACE(0, _b("waiting complete: %d"), r);

        for (i = 0; i < n; i++)
        {
            /* ...skip item if it's not signalled */
            if ((pfd[i].revents & POLLIN) == 0)    continue;

            /* ...retrieve a buffer from device */
            if (__decoder_process(dec, i) < 0)
            {
                TRACE(ERROR, _x("processing failed: %m"));
                break;
            }
        }
    }

    TRACE(INIT, _b("decoding thread exits: %m"));

    /* ...destroy poll structures */
    free(pfd);

    return (void *)(intptr_t)-errno;
}

/* ...start module operation */
static inline int vin_decoding_start(vin_decoder_t *dec)
{
    pthread_attr_t  attr;
    int             r;

    /* ...set decoder active flag */
    dec->active = 1;

    /* ...initialize thread attributes (joinable, 128KB stack) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 128 << 10);

    /* ...create decoding thread to asynchronously process JPG frames decoding */
    r = pthread_create(&dec->thread, &attr, vin_decode_thread, dec);
    pthread_attr_destroy(&attr);

    return CHK_API(r);
}

/*******************************************************************************
 * Buffer pool handling
 ******************************************************************************/

/* ...output buffer dispose function (called in response to "gst_buffer_unref") */
static gboolean __output_buffer_dispose(GstMiniObject *obj)
{
    GstBuffer          *buffer = GST_BUFFER(obj);
    vin_decoder_t      *dec = (vin_decoder_t *)buffer->pool;
    vin_meta_t         *meta = gst_buffer_get_vin_meta(buffer);
    int                 i = meta->camera_id;
    int                 j = meta->index;
    gboolean            destroy;

    /* ...acquire decoder access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...decrement amount of outstanding buffers */
    dec->output_busy--;

    TRACE(BUFFER,
          _b("camera-%d: buffer #%d (%p) returned to pool (busy: %d), refcount=%d"),
          i, j, buffer, dec->output_busy, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...check if buffer needs to be requeued into the pool */
    if (dec->active)
    {
        /* ...increment buffer reference */
        gst_buffer_ref(buffer);
        TRACE(BUFFER, _b("output buffer dispose: buf %p, refcount=%d"), buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
        /* ...requeue buffer instantly */
        __submit_buffer(dec, i, j);

        /* ...indicate the miniobject should not be freed */
        destroy = FALSE;
    }
    else
    {
        TRACE(BUFFER, _b("buffer %p is freed, refcount=%d"), buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

        /* ...signal flushing completion operation */
        (dec->output_busy == 0 ? pthread_cond_signal(&dec->flush_wait) : 0);

        /* ...reset buffer pointer */
        dec->dev[i].pool[j].buffer = NULL;

        /* ...force destruction of the buffer miniobject */
        destroy = TRUE;
    }

    /* ...release decoder access lock */
    pthread_mutex_unlock(&dec->lock);

    return destroy;
}

/* ...runtime initialization */
static inline int vin_runtime_init(vin_decoder_t *dec,
        char **devname, int n, int width, int height, u32 format)
{
    int     i, j;

    for (i = 0; i < n; i++)
    {
        vin_device_t   *dev = dec->dev + i;
        int             vfd;

        /* ...open associated VIN device */
        CHK_API(dev->vfd = vfd = open(devname[i], O_RDWR, O_NONBLOCK));

        /* ...set VIN format (image parameters are hardcoded - tbd) */
        CHK_API(vin_set_formats(vfd, width, height, format));

        /* ...allocate output buffers */
        CHK_API(vin_allocate_buffers(vfd, dev->pool, VIN_BUFFER_POOL_SIZE));

        /* ...create gstreamer buffers */
        for (j = 0; j < VIN_BUFFER_POOL_SIZE; j++)
        {
            vin_buffer_t   *buf = &dev->pool[j];
            GstBuffer      *buffer;
            vin_meta_t     *meta;
            vsink_meta_t   *vmeta;

            /* ...allocate empty GStreamer buffer */
            CHK_ERR(buf->buffer = buffer = gst_buffer_new(), -ENOMEM);
            TRACE(BUFFER, _b("new buffer: %p, refcount=%d, format=%s"), buffer, GST_MINI_OBJECT_REFCOUNT(buffer), gst_video_format_to_string (__pixfmt_v4l2_to_gst(format)));
            /* ...add VIN metadata for decoding purposes */
            CHK_ERR(meta = gst_buffer_add_vin_meta(buffer), -ENOMEM);
            meta->camera_id = i;
            meta->index = j;
            GST_META_FLAG_SET(meta, GST_META_FLAG_POOLED);

            /* ...add vsink metadata */
            CHK_ERR(vmeta = gst_buffer_add_vsink_meta(buffer), -ENOMEM);
            vmeta->width = width;
            vmeta->height = height;
            vmeta->format = __pixfmt_v4l2_to_gst(format);
            vmeta->dmafd[0] = -1;
            vmeta->dmafd[1] = -1;
            vmeta->plane[0] = buf->data;
            vmeta->plane[1] = NULL;
            GST_META_FLAG_SET(vmeta, GST_META_FLAG_POOLED);

            /* ...modify buffer release callback */
            GST_MINI_OBJECT(buffer)->dispose = __output_buffer_dispose;

            /* ...use "pool" pointer as a custom data */
            buffer->pool = (void *)dec;

            /* ...notify application on output buffer allocation */
            CHK_API(dec->cb->allocate(dec->cdata, buffer));

            /* ...submit a buffer into device */
            __submit_buffer(dec, i, j);
            TRACE(BUFFER, _b("gst buffer %p allocated, data pointer: %p, refcount=%d"), buffer, buf->data, GST_MINI_OBJECT_REFCOUNT(buffer));
        }
    }

    /* ...start decoding thread */
    CHK_API(vin_decoding_start(dec));

    TRACE(INIT, _b("VIN camera-bin runtime initialized"));

    return 0;
}

/*******************************************************************************
 * Component state change processing function
 ******************************************************************************/

/* ...state change notification */
static inline void vin_state_changed(GstElement *element,
        GstState oldstate, GstState newstate, GstState pending)
{
    vin_decoder_t      *dec = GST_OBJECT(element)->_gst_reserved;

    /* ...ugly stub... */
    if (!dec)
    {
        return;
    }

    TRACE(DEBUG, _b("element-%p: old=%u, new=%u, pending=%u"), element, oldstate, newstate, pending);

    /* ...if element enters NULL state, signal termination to all reading threads */
    if (newstate == GST_STATE_NULL)
    {
        /* ...acquire data protection lock */
        pthread_mutex_lock(&dec->lock);

        /* ...clear activity flag */
        dec->active = 0;

        /* ...notify decoding thread as required */
        pthread_cond_signal(&dec->wait);

        /* ...wait here until all buffers are returned back to pool */
        while (dec->output_busy > 0)
        {
            pthread_cond_wait(&dec->flush_wait, &dec->lock);
        }

        /* ...release data protection lock */
        pthread_mutex_unlock(&dec->lock);

        TRACE(INFO, _b("decoder enters NULL state"));
    }
}

/*******************************************************************************
 * Component destructor
 ******************************************************************************/

/* ...destructor function */
static void vin_decoder_destroy(gpointer data, GObject *obj)
{
    vin_decoder_t      *dec = data;
    int                 i, j;

    /* ...acquire decoder lock */
    pthread_mutex_lock(&dec->lock);

    /* ...clear activity flag */
    dec->active = 0;

    /* ...kick decoding thread as needed */
    pthread_cond_signal(&dec->wait);

    TRACE(BUFFER, _b("wait for output buffers: busy=%d"), dec->output_busy);

    /* ...wait for all output buffers getting back to a pool */
    while (dec->output_busy > 0)
    {
        pthread_cond_wait(&dec->flush_wait, &dec->lock);
    }

    TRACE(BUFFER, _b("output buffers are all collected"));

    /* ...release decoder access lock to allow thread to finish */
    pthread_mutex_unlock(&dec->lock);

    /* ...wait for a thread completion */
    pthread_join(dec->thread, NULL);

    TRACE(INIT, _b("decoder thread joined"));

    /* ...drop all queued output buffers (and inputs as well) */
    for (i = 0; i < dec->number; i++)
    {
        vin_device_t   *dev = &dec->dev[i];
        vin_buffer_t   *pool = dev->pool;

        for (j = 0; j < VIN_BUFFER_POOL_SIZE; j++)
        {
            GstBuffer  *buffer;

            /* ...unref buffer if it hasn't yet been freed */
            buffer = pool[j].buffer;
            if (buffer != NULL)
            {
                gst_buffer_unref(buffer);
                TRACE(BUFFER, _b("destroy: buffer %p refcount=%d"), buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
            }
        }

        /* ...deallocate buffers */
        vin_destroy_buffers(dev->vfd, pool, VIN_BUFFER_POOL_SIZE);

        /* ...close V4L2 device */
        close(dev->vfd);
    }

    /* ...destroy mutex */
    pthread_mutex_destroy(&dec->lock);

    /* ...destroy devices handles */
    free(dec->dev);

    /* ...destroy decoder structure */
    free(dec);

    TRACE(INIT, _b("vin-camera-bin destroyed"));
}

/*******************************************************************************
 * Camera bin (V4L2) initialization
 ******************************************************************************/

/* ...create camera bin interface */
GstElement * camera_vin_create(const camera_callback_t *cb,
                               void *cdata,
                               char **devname,
                               int n,
                               int width,
                               int height)
{
    vin_decoder_t      *dec;
    vin_device_t       *dev;
    GstElement         *bin;

    /* ...create decoder structure */
    CHK_ERR(dec = malloc(sizeof(*dec)), (errno = ENOMEM, NULL));

    /* ...create video-devices data */
    if ((dec->dev = dev = malloc(n * sizeof(*dev))) == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory for %u devices"), n);
        errno = ENOMEM;
        goto error;
    }

    /* ...create a bin that will host all cameras */
    if ((dec->bin = bin = gst_bin_new("vin-camera::bin")) == NULL)
    {
        TRACE(ERROR, _x("failed to create a bin"));
        errno = ENOMEM;
        goto error_dev;
    }

    /* ...save association between bin and decoder (tbd - looks like a hack) */
    GST_OBJECT(bin)->_gst_reserved = dec;

    /* ...save number of cameras in bin */
    dec->number = n;

    /* ...save application provided callback */
    dec->cb = cb, dec->cdata = cdata;

    /* ...clear number of queued/busy output buffers */
    dec->output_count = dec->output_busy = 0;

    /* ...initialize internal queue access lock */
    pthread_mutex_init(&dec->lock, NULL);

    /* ...initialize decoding thread conditional variable */
    pthread_cond_init(&dec->wait, NULL);

    /* ...initialize conditional variable for flushing */
    pthread_cond_init(&dec->flush_wait, NULL);

    /* ...initialize decoder runtime (image size hardcoded for now - tbd) */
    if ((errno = -vin_runtime_init(dec,
                                   devname,
                                   n,
                                   width,
                                   height,
                                   __gst_to_pixfmt_v4l2(GST_VIDEO_FORMAT_UYVY))) != 0)
    {
        TRACE(ERROR, _x("failed to initialize decoder runtime: %m"));
        goto error_bin;
    }

    /* ...set state-change notification function */
    GST_ELEMENT_GET_CLASS(bin)->state_changed = vin_state_changed;

    /* ...set custom object destructor */
    g_object_weak_ref(G_OBJECT(bin), vin_decoder_destroy, dec);

    TRACE(INIT, _b("VIN camera bin interface created"));

    /* ...return generic camera bin interface */
    return bin;

error_bin:
    /* ...destroy bin object */
    gst_object_unref(bin);

error_dev:
    /* ...destroy device data */
    free(dev);

error:
    /* ...destroy data object */
    free(dec);
    return NULL;
}

/* ...camera data */
typedef struct video_stream
{
    /* ...bin containing all internal nodes */
    GstElement                 *bin;

    /* ...application callbacks */
    const camera_callback_t    *cb;

    /* ...application data */
    void                       *cdata;

    /* ...camera identifier */
    int                         id;

}   video_stream_t;

/*******************************************************************************
 * Video sink callbacks
 ******************************************************************************/

/* ...buffer allocation callback */
static int __video_buffer_allocate(GstBuffer *buffer, void *data)
{
    video_stream_t  *stream = data;

    TRACE(BUFFER, _b("buffer allocated (%p), refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...pass allocation request to the application */
    return CHK_API(stream->cb->allocate(stream->cdata, buffer));
}

/* ...data availability callback */
static int __video_buffer_process(GstBuffer *buffer, void *data)
{
    video_stream_t  *stream = data;
    int              id = stream->id;

    TRACE(BUFFER, _b("process: buffer %p refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...pass buffer to decoder */
    CHK_API(stream->cb->process(stream->cdata, id, buffer));

    return 0;
}

/* ...video-sink callbacks */
static const vsink_callback_t   vsink_cb = {
    .allocate = __video_buffer_allocate,
    .process = __video_buffer_process,
};

/* ...custom destructor function */
static void __stream_destructor(gpointer data, GObject *obj)
{
    video_stream_t     *stream = data;

    /* ...deallocate stream data */
    free(stream);

    TRACE(INIT, _b("video-stream %p destroyed"), stream);
}
