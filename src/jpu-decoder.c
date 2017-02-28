/*******************************************************************************
 * MJPEG AVB-camera (Technica) backend
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

#define MODULE_TAG                      MJPEG

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <sys/poll.h>

#include "main.h"
#include "common.h"
#include "camera.h"
#include "camera-mjpeg.h"
#include "jpu.h"
#include "vsink.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);

/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

/* ...maximal length of encoded JPEG frame */
#define MJPEG_MAX_FRAME_LENGTH          (512 << 10)

/* ...number of input buffers per each camera */
#define MJPEG_INPUT_POOL_SIZE           4

/* ...total number of input buffers for JPU decoder */
#define MJPEG_INPUT_BUFFERS_NUM         (MJPEG_INPUT_POOL_SIZE * CAMERAS_NUMBER)

/* ...total number of output buffers for JPU decoder
 * (should be same as for input - tbd) */
#define MJPEG_OUTPUT_BUFFERS_NUM        MJPEG_INPUT_BUFFERS_NUM

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

typedef struct mjpeg_decoder
{
    /* ...GStreamer bin element for pipeline handling */
    GstElement                 *bin;

    /* ...JPU module handle */
    jpu_data_t                 *jpu;

    /* ...JPU input buffer pools */
    jpu_buffer_t                input_pool[MJPEG_INPUT_BUFFERS_NUM];

    /* ...JPU output buffers pool */
    jpu_buffer_t                output_pool[MJPEG_OUTPUT_BUFFERS_NUM];

    /* ...individual cameras (need to keep them for offline processing) */
    camera_data_t              *camera[CAMERAS_NUMBER];

    /* ...available input buffers queues */
    GQueue                      input[CAMERAS_NUMBER];

    /* ...number of output buffers queued to JPU */
    int                         output_count;

    /* ...number of output buffers submitted to the client */
    int                         output_busy;

    /* ...queue access lock */
    pthread_mutex_t             lock;

    /* ...decoding thread - tbd - make it a data source for GMainLoop? */
    pthread_t                   thread;

    /* ...decoder activity state */
    int                         active;

    /* ...decoding thread conditional variable */
    pthread_cond_t              wait;

    /* ...input buffer waiting conditional */
    pthread_cond_t              wait_input[CAMERAS_NUMBER];

    /* ...output buffers flushing conditional */
    pthread_cond_t              flush_wait;

    /* ...application-provided callback */
    const camera_callback_t    *cb;

    /* ...application callback data */
    void                       *cdata;

}   mjpeg_decoder_t;

/*******************************************************************************
 * Static decoder data
 ******************************************************************************/

/* ...static decoder data */
static mjpeg_decoder_t    __dec;

/*******************************************************************************
 * Custom buffer metadata implementation
 ******************************************************************************/

/* ...metadata type registration */
GType jpu_meta_api_get_type(void)
{
    static volatile GType type;
    static const gchar *tags[] = { GST_META_TAG_VIDEO_STR,
                                   GST_META_TAG_MEMORY_STR,
                                   NULL };

    if (g_once_init_enter(&type))
    {
        GType _type = gst_meta_api_type_register("JpuDecMetaAPI", tags);
        g_once_init_leave(&type, _type);
    }

    return type;
}

/* ...low-level interface */
static gboolean jpu_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
    jpu_meta_t     *_meta = (jpu_meta_t *) meta;

    /* ...reset fields */
    memset(&_meta->meta + 1, 0, sizeof(jpu_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata transformation */
static gboolean jpu_meta_transform(GstBuffer *transbuf, GstMeta *meta,
        GstBuffer *buffer, GQuark type, gpointer data)
{
    jpu_meta_t     *_meta = (jpu_meta_t *) meta, *_tmeta;

    /* ...add JPU metadata for a transformed buffer */
    _tmeta = gst_buffer_add_jpu_meta(transbuf);

    /* ...just copy data regardless of transform type? */
    memcpy(&_tmeta->meta + 1,
           &_meta->meta + 1,
           sizeof(jpu_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata release */
static void jpu_meta_free(GstMeta *meta, GstBuffer *buffer)
{
    jpu_meta_t     *_meta = (jpu_meta_t *) meta;

    /* ...anything to destroy? - tbd */
    TRACE(DEBUG, _b("free metadata %p"), _meta);
}

/* ...register metadata implementation */
const GstMetaInfo * jpu_meta_get_info(void)
{
    static const GstMetaInfo *meta_info = NULL;

    if (g_once_init_enter(&meta_info))
    {
        const GstMetaInfo *mi = gst_meta_register(
            JPU_META_API_TYPE,
            "JpuDecMeta",
            sizeof(jpu_meta_t),
            jpu_meta_init,
            jpu_meta_free,
            jpu_meta_transform);

        g_once_init_leave (&meta_info, mi);
    }

    return meta_info;
}

/*******************************************************************************
 * Internal functions
 ******************************************************************************/

/* ...submit buffers for decoding (runs with decoder access lock held) */
static int __submit_buffers(mjpeg_decoder_t *dec, int j)
{
    /* ...submit buffer pair to the decoder */
    CHK_API(jpu_input_buffer_queue(dec->jpu, j, dec->input_pool));
    CHK_API(jpu_output_buffer_queue(dec->jpu, j, dec->output_pool));

    TRACE(DEBUG, _b("camera-%d: submit buffer pair %d (queued: %d)"),
          j / CAMERAS_NUMBER, j, dec->output_count);

    /* ...notify decoding thread as appropriate (tbd - move to jpu?) */
    (dec->output_count++ == 0 ? pthread_cond_signal(&dec->wait) : 0);

    return 0;
}

/* ...process new input buffer submitted from camera */
static inline int __camera_input_put(void *data, int i, GstBuffer *buffer)
{
    mjpeg_decoder_t    *dec = data;
    jpu_meta_t         *meta = gst_buffer_get_jpu_meta(buffer);
    jpu_buffer_t       *buf = meta->priv;
    int                 j = (int)(buf - dec->input_pool);
    int                 r;

    /* ...make sure buffer is valid */
    CHK_ERR(j >= 0 && j < MJPEG_INPUT_BUFFERS_NUM, -EINVAL);
    CHK_ERR(buf->priv == buffer, -EINVAL);

    /* ...prevent returning of the buffer to pool */
    gst_buffer_ref(buffer);

    /* ...save buffer length */
    buf->m.length = gst_buffer_get_size(buffer);

    TRACE(DEBUG, _b("camera-%d: input buffer #%d received (%u bytes, ts=%lu)"),
          i, j, buf->m.length, GST_BUFFER_DTS(buffer));

    /* ...get queue access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...mark input buffer contains data (is mapped) */
    dec->input_pool[j].map = 1;

    /* ...check if associated output buffer is available for writing */
    r = (dec->output_pool[j].map == 0 ? __submit_buffers(dec, j) : 0);

    /* ...release queue access lock */
    pthread_mutex_unlock(&dec->lock);

    return r;
}

/* ...retrieve new input buffer (interface exposed to a camera) */
static GstBuffer * __camera_input_get(void *data, int i)
{
    mjpeg_decoder_t    *dec = data;
    GstBuffer          *buffer;
    int                 j;

    /* ...get queue access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...check if we have available input buffer for particular queue */
    while (g_queue_is_empty(&dec->input[i]))
    {
#if !DROP_BUFFERS
        /* ...wait until we get an input buffer */
        while (dec->active && g_queue_is_empty(&dec->input[i]))
        {
            TRACE(DEBUG, _b("camera-%d: wait for input buffer"), i);

            pthread_cond_wait(&dec->wait_input[i], &dec->lock);
        }

        /* ...if component is active, go get buffer from the queue */
        if (dec->active)
        {
            break;
        }
#endif
        /* ...no buffers available */
        TRACE(DEBUG, _b("camera-%d: buffer queue is empty"), i);

        buffer = NULL;

        goto out;
    }

    /* ...take head of the queue */
    j = GPOINTER_TO_INT(g_queue_pop_head(&dec->input[i]));

    TRACE(DEBUG, _b("camera-%d: got input buffer #%d"), i, j);

    buffer = dec->input_pool[j].priv;

    /* ...reset buffer size */
    gst_buffer_set_size(buffer, MJPEG_MAX_FRAME_LENGTH);

out:
    /* ...release data access lock */
    pthread_mutex_unlock(&dec->lock);

    return buffer;
}

/* ...buffer probing callback */
static GstPadProbeReturn camera_buffer_probe(GstPad *pad,
                                             GstPadProbeInfo *info,
                                             gpointer user_data)
{
    mjpeg_decoder_t    *dec = user_data;
    int                 i = GPOINTER_TO_INT(gst_pad_get_element_private(pad));
    GstBuffer          *buffer;
    int                 r;

    /* ...get a buffer handle */
    CHK_ERR(buffer = gst_pad_probe_info_get_buffer(info), GST_PAD_PROBE_DROP);

    /* ...verify we have metadata */
    CHK_ERR(gst_buffer_get_jpu_meta(buffer) != NULL, GST_PAD_PROBE_DROP);

    /* ...pass buffer to a pool */
    r = __camera_input_put(dec, i, buffer);

    /* ...don't pass buffer further */
    return (r >= 0 ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_REMOVE);
}

/* ...state change notification */
static inline void camera_state_changed(GstElement *element,
                                        GstState oldstate,
                                        GstState newstate,
                                        GstState pending)
{
    mjpeg_decoder_t    *dec = &__dec;
    int                 i;

    /* ...bail out if inactive - ugly hack */
    if (!dec->active)       return;

    TRACE(DEBUG, _b("element-%p: old=%u, new=%u, pending=%u"),
          element, oldstate, newstate, pending);

    /* ...if element enters NULL state, signal termination to all reading threads */
    if (newstate == GST_STATE_NULL)
    {
        /* ...acquire data protection lock */
        pthread_mutex_lock(&dec->lock);

        /* ...notify cameras on state change */
        for (i = 0; i < CAMERAS_NUMBER; i++)
        {
            pthread_cond_signal(&dec->wait_input[i]);
        }

        /* ...clear activity flag */
        dec->active = 0;

        /* ...notify decoding thread as required */
        pthread_cond_signal(&dec->wait);

        /* ...wait here until all buffers are returned */
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
 * Processing thread
 ******************************************************************************/

/* ...data processing notification - from decoder loop */
static inline int __decoder_process(mjpeg_decoder_t *dec)
{
    int             i, j, k;
    GstBuffer      *ibuffer, *obuffer;

    /* ...get queue access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...get ready output buffer */
    CHK_API(k = jpu_output_buffer_dequeue(dec->jpu));

    /* ...associated input buffer must be available too */
    CHK_API(j = jpu_input_buffer_dequeue(dec->jpu));

    /* ...get camera index */
    i = j / CAMERAS_NUMBER;

    /* ...decrement number of queued outputs */
    dec->output_count--;

    TRACE(DEBUG, _b("camera-%d: dequeued buffer pair: %d:%d (queued: %d)"),
          i, j, k, dec->output_count);

    /* ...validate association between buffers */
    CHK_ERR(k == j, -EBADFD);

    /* ...get input/output buffers */
    ibuffer = dec->input_pool[j].priv;
    obuffer = dec->output_pool[j].priv;

    /* ...check if decoder is active still */
    if (dec->active)
    {
        /* ...mark the output buffer contains valid data */
        dec->output_pool[k].map = 1;

        /* ...advance number of busy (submitted) buffers */
        dec->output_busy++;

        TRACE(DEBUG, _b("camera-%d: submit buffer #%d (busy=%d)"),
              i, k, dec->output_busy);

        /* ...release queue lock */
        pthread_mutex_unlock(&dec->lock);

        /* ...copy decoding/presentation timestamps */
        GST_BUFFER_DTS(obuffer) = GST_BUFFER_DTS(ibuffer);

        /* ...presentation time is not needed, actually, but let it be */
        GST_BUFFER_PTS(obuffer) = GST_BUFFER_PTS(ibuffer);

        /* ...pass output buffer to application */
        CHK_API(dec->cb->process(dec->cdata, i, obuffer));
    }
    else
    {
        TRACE(DEBUG, _b("camera-%d: drop buffer #%d (busy=%d)"),
              i, k, dec->output_busy);

        /* ...release queue lock */
        pthread_mutex_unlock(&dec->lock);
    }

    /* ...drop the reference to input buffer */
    gst_buffer_unref(ibuffer);

    /* ...drop the reference to the ouutput buffer
     * (it is now owned by application) */
    gst_buffer_unref(obuffer);

    return 0;
}

/* ...data processing notification - from decoder loop */
static void * decode_thread(void *arg)
{
    mjpeg_decoder_t    *dec = arg;
    struct pollfd       pfd;

    /* ...initialize JPU poll descriptor */
    CHK_ERR((pfd.fd = jpu_capture_fd(dec->jpu)) >= 0, NULL);
    pfd.events = POLLIN;

    /* ...start processing loop */
    while (1)
    {
        int     r;

        /* ...check if we have output buffers queued */
        pthread_mutex_lock(&dec->lock);

        /* ...wait until we get any buffer queued */
        while (dec->active && !dec->output_count)
        {
            pthread_cond_wait(&dec->wait, &dec->lock);
        }

        /* ...we cannot safely leave while we have queued buffers */
        if (!dec->output_count)
        {
            pthread_mutex_unlock(&dec->lock);
            break;
        }

        /* ...release lock */
        pthread_mutex_unlock(&dec->lock);

        /* ...wait for a JPU decoding completion */
        if ((r = poll(&pfd, 1, -1)) < 0)
        {
            /* ...ignore soft interruption (e.g. from gdb) */
            if (errno == EINTR) continue;
            TRACE(ERROR, _x("poll failed: %m"));
            break;
        }

        TRACE(0, _b("waiting complete: %d"), r);

        /* ...verify we have a buffer ready (it is probably a fatal error if no) */
        if ((pfd.revents & POLLIN) == 0)
        {
            TRACE(ERROR, _x("output is not ready: %X"), pfd.revents);
            break;
        }

        /* ...invoke processing */
        if (__decoder_process(dec) < 0)
        {
            TRACE(ERROR, _x("processing failed: %m"));
            break;
        }
    }

    TRACE(INIT, _b("decoding thread exits: %m"));

    return (void *)(intptr_t)-errno;
}

/* ...start module operation */
static inline int mjpeg_decoding_start(mjpeg_decoder_t *dec)
{
    pthread_attr_t  attr;
    int             r;

    /* ...set decoder activity flag */
    dec->active = 1;

    /* ...initialize thread attributes (joinable, 128KB stack) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 128 << 10);

    /* ...create JPU decoding thread to asynchronously process JPG frames decoding */
    r = pthread_create(&dec->thread, &attr, decode_thread, dec);
    pthread_attr_destroy(&attr);

    return CHK_API(r);
}

/*******************************************************************************
 * Runtime initialization
 ******************************************************************************/

/* ...input buffer dispose function (called with lock held) */
static gboolean __jpu_input_buffer_dispose(GstMiniObject *obj)
{
    GstBuffer          *buffer = GST_BUFFER(obj);
    mjpeg_decoder_t    *dec = (mjpeg_decoder_t *)buffer->pool;
    jpu_meta_t         *meta = gst_buffer_get_jpu_meta(buffer);
    jpu_buffer_t       *buf = meta->priv;
    int                 j = (int)(buf - dec->input_pool);
    int                 i = j / CAMERAS_NUMBER;
    gboolean            destroy;

    /* ...lock access to internal data */
    pthread_mutex_lock(&dec->lock);

    /* ...buffer shall be kept if it is referenced in output pool */
    if (dec->active)
    {
        /* ...mark the buffer doesn't contain any data */
        dec->input_pool[j].map = 0;

        /* ...put buffer back to the pending input queue */
        g_queue_push_tail(&dec->input[i], GINT_TO_POINTER(j));

        /* ...notify input path about buffer availability */
        pthread_cond_signal(&dec->wait_input[i]);

        /* ...increment buffer reference */
        gst_buffer_ref(buffer);

        TRACE(DEBUG, _b("camera-%d: input buffer #%d processed"), i, j);

        /* ...indicate the miniobject should not be freed */
        destroy = FALSE;
    }
    else
    {
        /* ...decoder is not active; drop the buffer */
        TRACE(DEBUG, _b("camera-%d: input buffer #%d freed"), i, j);

        /* ...reset buffer pointer */
        dec->input_pool[j].priv = NULL;

        /* ...mark buffer is to be destroyed */
        destroy = TRUE;
    }

    /* ...release internal access lock */
    pthread_mutex_unlock(&dec->lock);

    return destroy;
}

/* ...output buffer dispose function (called in response to "gst_buffer_unref") */
static gboolean __jpu_output_buffer_dispose(GstMiniObject *obj)
{
    GstBuffer          *buffer = GST_BUFFER(obj);
    mjpeg_decoder_t    *dec = (mjpeg_decoder_t *)buffer->pool;
    jpu_meta_t         *meta = gst_buffer_get_jpu_meta(buffer);
    jpu_buffer_t       *buf = meta->priv;
    int                 k = (int)(buf - dec->output_pool);
    gboolean            destroy;

    /* ...verify buffer validity */
    BUG((unsigned)k >= MJPEG_OUTPUT_BUFFERS_NUM, _x("invalid buffer: %p, k=%d"),
        buffer, k);

    /* ...acquire decoder access lock */
    pthread_mutex_lock(&dec->lock);

    /* ...decrement amount of outstanding buffers */
    dec->output_busy--;

    TRACE(DEBUG, _b("output buffer #%d (%p) returned to pool (busy: %d)"),
          k, buffer, dec->output_busy);

    /* ...check if buffer needs to be requeued into the pool */
    if (dec->active)
    {
        /* ...mark the buffer is now free */
        dec->output_pool[k].map = 0;

        /* ...increment buffer reference */
        gst_buffer_ref(buffer);

        /* ...if associated input buffer is "mapped", force processing */
        (void)(dec->input_pool[k].map ? __submit_buffers(dec, k) : 0);

        /* ...indicate the miniobject should not be freed */
        destroy = FALSE;
    }
    else
    {
        TRACE(DEBUG, _b("buffer #%d (%p) is freed"), k, buffer);

        /* ...signal flushing completion operation */
        (dec->output_busy == 0 ? pthread_cond_signal(&dec->flush_wait) : 0);

        /* ...reset buffer pointer */
        dec->output_pool[k].priv = NULL;

        /* ...force destruction of the buffer miniobject */
        destroy = TRUE;
    }

    /* ...release decoder access lock */
    pthread_mutex_unlock(&dec->lock);

    return destroy;
}

static void __release_buffer(gpointer data)
{
    /* ...unmap V4L2 buffer memory */
    TRACE(DEBUG, _b("buffer %p released"), data);
}

/* ...runtime initialization */
static inline int mjpeg_runtime_init(mjpeg_decoder_t *dec,
                                     int width,
                                     int height)
{
    int     i, j, k;

    /* ...set JPU format (image parameters are hardcoded - tbd) */
    CHK_API(jpu_set_formats(dec->jpu,
                            width, height,
                            MJPEG_MAX_FRAME_LENGTH));

    /* ...allocate input buffers */
    CHK_API(jpu_allocate_buffers(dec->jpu,
                                 0,
                                 dec->input_pool,
                                 MJPEG_INPUT_BUFFERS_NUM));

    /* ...allocate output buffers */
    CHK_API(jpu_allocate_buffers(dec->jpu,
                                 1,
                                 dec->output_pool,
                                 MJPEG_OUTPUT_BUFFERS_NUM));

    /* ...create input buffers pool for cameras */
    memset(&dec->input, 0, sizeof(dec->input));

    /* ...create JPU input buffer pool */
    for (j = 0; j < MJPEG_INPUT_BUFFERS_NUM; j++)
    {
        jpu_buffer_t   *buf = &dec->input_pool[j];
        GstBuffer      *buffer;
        jpu_meta_t     *jmeta;

        /* ...determine camera index */
        i = j / CAMERAS_NUMBER;

        /* ...allocate gst-buffer wrapping the memory allocated by JPU */
        buffer = gst_buffer_new_wrapped_full(0,
                                             buf->m.data,
                                             MJPEG_MAX_FRAME_LENGTH,
                                             0,
                                             MJPEG_MAX_FRAME_LENGTH,
                                             buf->m.data,
                                             __release_buffer);

        /* ...associate jpu-buffer and gst-buffer */
        CHK_ERR(buf->priv = buffer, -ENOMEM);

        /* ...clear buffer mapping flag */
        buf->map = 0;

        /* ...create buffer metadata */
        CHK_ERR(jmeta = gst_buffer_add_jpu_meta(buffer), -ENOMEM);
        jmeta->priv = buf;
        jmeta->width = width;
        jmeta->height = height;
        GST_META_FLAG_SET(jmeta, GST_META_FLAG_POOLED);

        /* ...modify buffer release callback */
        GST_MINI_OBJECT_CAST(buffer)->dispose = __jpu_input_buffer_dispose;

        /* ...use "pool" pointer as a custom data */
        buffer->pool = (void *)dec;

        /* ...add buffer to particular pool */
        g_queue_push_tail(&dec->input[i], GINT_TO_POINTER(j));
    }

    /* ...create GStreamer buffers for output pool */
    for (k = 0; k < MJPEG_OUTPUT_BUFFERS_NUM; k++)
    {
        jpu_buffer_t   *buf = &dec->output_pool[k];
        GstBuffer      *buffer;
        jpu_meta_t     *jmeta;
        vsink_meta_t   *vmeta;

        /* ...allocate empty GStreamer buffer */
        CHK_ERR(buf->priv = buffer = gst_buffer_new(), -ENOMEM);

        /* ...clear buffer mapped flag */
        buf->map = 0;

        /* ...add JPU metadata for decoding purposes */
        CHK_ERR(jmeta = gst_buffer_add_jpu_meta(buffer), -ENOMEM);
        jmeta->priv = buf;
        jmeta->width = width;
        jmeta->height = height;
        GST_META_FLAG_SET(jmeta, GST_META_FLAG_POOLED);

        /* ...add vsink metadata */
        CHK_ERR(vmeta = gst_buffer_add_vsink_meta(buffer), -ENOMEM);
        vmeta->width = width;
        vmeta->height = height;
        vmeta->format = GST_VIDEO_FORMAT_NV12;
        vmeta->dmafd[0] = buf->m.dmafd[0];
        vmeta->dmafd[1] = buf->m.dmafd[1];
        vmeta->plane[0] = buf->m.planebuf[0];
        vmeta->plane[1] = buf->m.planebuf[1];
        GST_META_FLAG_SET(vmeta, GST_META_FLAG_POOLED);

        /* ...modify buffer release callback */
        GST_MINI_OBJECT(buffer)->dispose = __jpu_output_buffer_dispose;

        /* ...use "pool" pointer as a custom data */
        buffer->pool = (void *)dec;

        /* ...notify application on output buffer allocation */
        CHK_API(dec->cb->allocate(dec->cdata, buffer));
    }

    /* ...start decoding thread */
    CHK_API(mjpeg_decoding_start(dec));

    TRACE(INIT, _b("mjpeg camera-bin runtime initialized"));

    return 0;
}

/*******************************************************************************
 * Camera bin destructor
 ******************************************************************************/

/* ...destructor function (to-be-verified; looks it will not work) */
static void mjpeg_decoder_destroy(gpointer data, GObject *obj)
{
    mjpeg_decoder_t    *dec = data;
    int                 j;

    /* ...it is not permitted to terminate active decoder */
    BUG(dec->active != 0 || dec->output_busy > 0,
        _x("invalid transaction: active=%d, busy=%d"),
        dec->active, dec->output_busy);

    TRACE(DEBUG, _b("joining decoder thread"));

    /* ...wait for a thread completion */
    pthread_join(dec->thread, NULL);

    TRACE(INIT, _b("decoder thread joined"));

    /* ...drop all queued input/output buffers  */
    for (j = 0; j < MJPEG_OUTPUT_BUFFERS_NUM; j++)
    {
        GstBuffer  *buffer;

        /* ...drop input buffer if needed */
        ((buffer = dec->input_pool[j].priv) ? gst_buffer_unref(buffer) : 0);

        /* ...drop output buffer if needed */
        ((buffer = dec->output_pool[j].priv) ? gst_buffer_unref(buffer) : 0);
    }

    /* ...deallocate both pools (free JPU memory) */
    jpu_destroy_buffers(dec->jpu, 0, dec->input_pool, MJPEG_INPUT_BUFFERS_NUM);
    jpu_destroy_buffers(dec->jpu, 1, dec->output_pool, MJPEG_OUTPUT_BUFFERS_NUM);

    /* ...close JPU decoder interface */
    jpu_destroy(dec->jpu);

    /* ...destroy mutex */
    pthread_mutex_destroy(&dec->lock);

    /* ...mark decoder structure is destroyed */
    dec->bin = NULL;

    TRACE(INIT, _b("mjpeg-camera-bin destroyed"));
}

/*******************************************************************************
 * Camera bin (JPEG decoder) initialization
 ******************************************************************************/

/* ...create camera bin interface */
GstElement * camera_mjpeg_create(const camera_callback_t *cb, void *cdata, int n, int width, int height)
{
    mjpeg_decoder_t    *dec = &__dec;
    GstElement         *bin;
    int                 i;

    /* ...make sure decoder is not created already */
    CHK_ERR(dec->bin == NULL, (errno = EBUSY, NULL));

    /* ...initialize JPU decoder module */
    if ((dec->jpu = jpu_init(jpu_dev_name)) == NULL)
    {
        TRACE(ERROR, _x("failed to initialize JPU module: %m"));
        goto error;
    }

    /* ...create a bin that will host all cameras */
    if ((dec->bin = bin = gst_bin_new("mjpeg-camera::bin")) == NULL)
    {
        TRACE(ERROR, _x("failed to create a bin"));
        errno = ENOMEM;
        goto error_jpu;
    }

    /* ...initialize cameras interface */
    for (i = 0; i < n; i++)
    {
        GstElement     *camera;
        GstPad         *pad, *gpad;
        char            name[16];

        /* ...create individual camera (keep them for offline playback) */
        dec->camera[i] = mjpeg_camera_create(i, __camera_input_get, dec);
        if (dec->camera[i] == NULL)
        {
            TRACE(ERROR, _x("failed to create camera-%d: %m"), i);
            goto error_bin;
        }

        camera = mjpeg_camera_gst_element(dec->camera[i]);

        pthread_cond_init(&dec->wait_input[i], NULL);

        /* ...add camera to the bin */
        gst_bin_add(GST_BIN(bin), camera);

        /* ...add probing function to the camera source pad */
        sprintf(name, "sview::src_%u", i);
        pad = gst_element_get_static_pad(camera, "src");
        gpad = gst_ghost_pad_new(name, pad);
        gst_object_unref(pad);
        gst_pad_set_element_private(gpad, GINT_TO_POINTER(i));
        gst_element_add_pad(bin, gpad);

        /* ...set probing function for output pad? */
        gst_pad_add_probe(gpad,
                          GST_PAD_PROBE_TYPE_BUFFER,
                          camera_buffer_probe,
                          dec,
                          NULL);
    }

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
    if ((errno = -mjpeg_runtime_init(dec, 1280, 800)) != 0)
    {
        TRACE(ERROR, _x("failed to initialize decoder runtime: %m"));
        goto error_bin;
    }

    /* ...set custom state change notification hook */
    GST_ELEMENT_GET_CLASS(bin)->state_changed = camera_state_changed;

    /* ...set custom object destructor */
    g_object_weak_ref(G_OBJECT(bin), mjpeg_decoder_destroy, dec);

    TRACE(INIT, _b("MJPEG camera bin interface created"));

    /* ...return generic camera bin interface */
    return bin;

error_bin:
    /* ...destroy bin object (remove all cameras as well) */
    gst_object_unref(bin);

error_jpu:
    /* ...destroy JPU module */
    jpu_destroy(dec->jpu);

error:
    /* ...destroy data object */
    return NULL;
}

/* ...pass packet to a camera */
void camera_mjpeg_packet_receive(int id, u8 *pdu, u16 len, u64 ts)
{
    camera_packet_receive(__dec.camera[id], pdu, len, ts);
}
