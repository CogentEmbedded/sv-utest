/*******************************************************************************
 *
 * MJPEG camera decoder implementation
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
#include "vsink.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);
TRACE_TAG(BUFFER, 0);

/*******************************************************************************
 * Local constants definitions
 ******************************************************************************/

/* ...maximal length of encoded JPEG frame */
#define MJPEG_MAX_FRAME_LENGTH          ((256 << 10) * 2)

/* ...number of input buffers per each camera */
#define MJPEG_INPUT_POOL_SIZE           4

/* ...total number of input buffers for decoder */
#define MJPEG_INPUT_BUFFERS_NUM         (MJPEG_INPUT_POOL_SIZE * CAMERAS_NUMBER)

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/* ...pool buffer definition */
typedef struct pool_buffer
{
    /* ...private data associated with buffer */
    void               *priv;

    /* ...memory mapped pointer */
    void               *data;

}   pool_buffer_t;

/* ...camera data */
typedef struct mjpeg_stream
{
    /* ...application callbacks */
    const camera_callback_t    *cb;

    /* ...application data */
    void                       *cdata;

    /* ...camera identifier */
    int                         id;

}   mjpeg_stream_t;

typedef struct mjpeg_decoder
{
    /* ...GStreamer bin element for pipeline handling */
    GstElement                 *bin;

    /* ...input buffer pool */
    pool_buffer_t              input_pool[MJPEG_INPUT_BUFFERS_NUM];

    /* ...individual cameras (need to keep them for offline processing) */
    camera_data_t              *camera[CAMERAS_NUMBER];

    /* ...available input buffers queues */
    GQueue                      input[CAMERAS_NUMBER];

    /* ...individual stream data */
    mjpeg_stream_t              stream[CAMERAS_NUMBER];

    /* ...number of output buffers queued */
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

/* ...metadata structure */
typedef struct mjpeg_meta
{
    GstMeta             meta;

    /* ...user-specific private data */
    void               *priv;

}   mjpeg_meta_t;

/* ...metadata API type accessor */
extern GType mjpeg_meta_api_get_type(void);
#define MJPEG_META_API_TYPE               (mjpeg_meta_api_get_type())

/* ...metadata information handle accessor */
extern const GstMetaInfo *mjpeg_meta_get_info(void);
#define MJPEG_META_INFO                   (mjpeg_meta_get_info())

/* ...get access to the buffer metadata */
#define gst_buffer_get_mjpeg_meta(b)      \
    ((mjpeg_meta_t *)gst_buffer_get_meta((b), MJPEG_META_API_TYPE))

/* ...attach metadata to the buffer */
#define gst_buffer_add_mjpeg_meta(b)    \
    ((mjpeg_meta_t *)gst_buffer_add_meta((b), MJPEG_META_INFO, NULL))

/* ...metadata type registration */
GType mjpeg_meta_api_get_type(void)
{
    static volatile GType type;
    static const gchar *tags[] = { GST_META_TAG_VIDEO_STR, GST_META_TAG_MEMORY_STR, NULL };

    if (g_once_init_enter(&type))
    {
        GType _type = gst_meta_api_type_register("MJpegDecMetaAPI", tags);
        g_once_init_leave(&type, _type);
    }

    return type;
}

/* ...low-level interface */
static gboolean mjpeg_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
    mjpeg_meta_t     *_meta = (mjpeg_meta_t *) meta;

    /* ...reset fields */
    memset(&_meta->meta + 1, 0, sizeof(mjpeg_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata transformation */
static gboolean mjpeg_meta_transform(GstBuffer *transbuf, GstMeta *meta,
        GstBuffer *buffer, GQuark type, gpointer data)
{
    mjpeg_meta_t     *_meta = (mjpeg_meta_t *) meta, *_tmeta;

    /* ...add MJpeg metadata for a transformed buffer */
    _tmeta = gst_buffer_add_mjpeg_meta(transbuf);

    /* ...just copy data regardless of transform type? */
    memcpy(&_tmeta->meta + 1, &_meta->meta + 1, sizeof(mjpeg_meta_t) - sizeof(GstMeta));

    return TRUE;
}

/* ...metadata release */
static void mjpeg_meta_free(GstMeta *meta, GstBuffer *buffer)
{
    mjpeg_meta_t     *_meta = (mjpeg_meta_t *) meta;

    /* ...anything to destroy? - tbd */
    TRACE(DEBUG, _b("free metadata %p"), _meta);
}

/* ...register metadata implementation */
const GstMetaInfo * mjpeg_meta_get_info(void)
{
    static const GstMetaInfo *meta_info = NULL;

    if (g_once_init_enter(&meta_info))
    {
        const GstMetaInfo *mi = gst_meta_register(
            MJPEG_META_API_TYPE,
            "MJpegDecMeta",
            sizeof(mjpeg_meta_t),
            mjpeg_meta_init,
            mjpeg_meta_free,
            mjpeg_meta_transform);

        g_once_init_leave (&meta_info, mi);
    }

    return meta_info;
}

/*******************************************************************************
 * Internal functions
 ******************************************************************************/

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

/* ...state change notification */
static inline void camera_state_changed(GstElement *element, GstState oldstate, GstState newstate, GstState pending)
{
    mjpeg_decoder_t    *dec = &__dec;
    int                 i;

    /* ...bail out if inactive - ugly hack */
    if (!dec->active)
    {
        return;
    }

    TRACE(DEBUG, _b("element-%p: old=%u, new=%u, pending=%u"), element, oldstate, newstate, pending);

    /* ...if element enters NULL state, signal termination to all reading threads */
    if (newstate == GST_STATE_NULL)
    {
        /* ...acquire data protection lock */
        pthread_mutex_lock(&dec->lock);

        /* ...notify cameras on state change */
        for (i = 0; i < CAMERAS_NUMBER; i++)    pthread_cond_signal(&dec->wait_input[i]);

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
 * Runtime initialization
 ******************************************************************************/

/* ...input buffer dispose function (called with lock held) */
static gboolean __input_buffer_dispose(GstMiniObject *obj)
{
    GstBuffer          *buffer = GST_BUFFER(obj);
    mjpeg_decoder_t    *dec = (mjpeg_decoder_t *)buffer->pool;
    mjpeg_meta_t         *meta = gst_buffer_get_mjpeg_meta(buffer);
    pool_buffer_t      *buf = meta->priv;
    int                 j = (int)(buf - dec->input_pool);
    int                 i = j / MJPEG_INPUT_POOL_SIZE;
    gboolean            destroy;

    /* ...lock access to internal data */
    pthread_mutex_lock(&dec->lock);

    /* ...buffer shall be kept if it is referenced in output pool */
    if (dec->active)
    {
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

static void __release_buffer(gpointer data)
{
    /* ...unmap V4L2 buffer memory */
    TRACE(BUFFER, _b("buffer %p released"), data);
}

/* ...runtime initialization */
static inline int mjpeg_runtime_init(mjpeg_decoder_t *dec, int width, int height)
{
    int     i, j;

    /* ...create input buffers pool for cameras */
    memset(&dec->input, 0, sizeof(dec->input));

    /* ...create input buffer pool */
    for (j = 0; j < MJPEG_INPUT_BUFFERS_NUM; j++)
    {
        pool_buffer_t  *buf = &dec->input_pool[j];
        GstBuffer      *buffer = NULL;
        mjpeg_meta_t     *jmeta;

        buf->data = malloc(MJPEG_MAX_FRAME_LENGTH);
        CHK_ERR(buf->data != NULL, -errno);

         /* ...determine camera index */
        i = j / MJPEG_INPUT_POOL_SIZE;

        /* ...allocate gst-buffer wrapping the memory allocated by malloc */
        buffer = gst_buffer_new_wrapped_full(0, buf->data, MJPEG_MAX_FRAME_LENGTH, 0, MJPEG_MAX_FRAME_LENGTH, buf->data, __release_buffer);

        /* ...associate pool-buffer and gst-buffer */
        CHK_ERR(buf->priv = buffer, -ENOMEM);

        /* ...create buffer metadata */
        CHK_ERR(jmeta = gst_buffer_add_mjpeg_meta(buffer), -ENOMEM);
        jmeta->priv = buf;
        GST_META_FLAG_SET(jmeta, GST_META_FLAG_POOLED);

        /* ...modify buffer release callback */
        GST_MINI_OBJECT_CAST(buffer)->dispose = __input_buffer_dispose;

        /* ...use "pool" pointer as a custom data */
        buffer->pool = (void *)dec;

        /* ...add buffer to particular pool */
        g_queue_push_tail(&dec->input[i], GINT_TO_POINTER(j));
    }

    /* ...set decoder activity flag */
    dec->active = 1;

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
    int                 i;

    /* ...it is not permitted to terminate active decoder */
    BUG(dec->active != 0 || dec->output_busy > 0, _x("invalid transaction: active=%d, busy=%d"), dec->active, dec->output_busy);

    /* ...deallocate the pool */
    for (i = 0; i < MJPEG_INPUT_BUFFERS_NUM; i++)
    {
        GstBuffer  *buffer;

        /* ...drop input buffer if needed */
        ((buffer = dec->input_pool[i].priv) ? gst_buffer_unref(buffer) : 0);

        free(dec->input_pool[i].data);
    }

    /* ...destroy mutex */
    pthread_mutex_destroy(&dec->lock);

    /* ...mark decoder structure is destroyed */
    dec->bin = NULL;

    TRACE(INIT, _b("mjpeg-camera-bin destroyed"));
}

/*******************************************************************************
 * Video sink callbacks
 ******************************************************************************/

/* ...buffer allocation callback */
static int __video_mjpeg_buffer_allocate(GstBuffer *buffer, void *data)
{
    mjpeg_stream_t  *stream = data;

    TRACE(BUFFER, _b("buffer allocated (%p)"), buffer);

    /* ...pass allocation request to the application */
    return CHK_API(stream->cb->allocate(stream->cdata, buffer));
}

/* ...data availability callback */
static int __video_mjpeg_buffer_process(GstBuffer *buffer, void *data)
{
    mjpeg_stream_t  *stream = data;
    int              id = stream->id;
    vsink_meta_t    *meta = gst_buffer_get_vsink_meta(buffer);

    /* ...make sure we have a valid metadata */
    CHK_ERR(meta, -EPIPE);

    /* ...pass buffer to decoder */
    CHK_API(stream->cb->process(stream->cdata, id, buffer));

    return 0;
}

/* ...video-sink callbacks */
static const vsink_callback_t   vsink_mjpeg_cb =
{
    .allocate = __video_mjpeg_buffer_allocate,
    .process = __video_mjpeg_buffer_process,
};

/*******************************************************************************
 * Camera bin (JPEG decoder) initialization
 ******************************************************************************/

/* ...create camera bin interface */
GstElement * camera_mjpeg_create(const camera_callback_t *cb,
                                 void *cdata,
                                 int n,
                                 int width,
                                 int height)
{
    mjpeg_decoder_t    *dec = &__dec;
    GstElement         *bin;
    int                 i;

    /* ...make sure decoder is not created already */
    CHK_ERR(dec->bin == NULL, (errno = EBUSY, NULL));

    /* ...create a bin that will host all cameras */
    if ((dec->bin = bin = gst_bin_new("mjpeg-camera::bin")) == NULL)
    {
        TRACE(ERROR, _x("failed to create a bin"));
        errno = ENOMEM;
        goto error;
    }

    /* ...initialize cameras interface */
    for (i = 0; i < n; i++)
    {
        GstElement     *camera;
        GstElement     *sink;
        GstElement     *decoder;
        GstCaps        *caps;
        GstPad         *dpad;

        /* ...create individual camera (keep them for offline playback) */
        if ((dec->camera[i] = mjpeg_camera_create(i, __camera_input_get, dec)) == NULL)
        {
            TRACE(ERROR, _x("failed to create camera-%d: %m"), i);
            goto error_bin;
        }
        else
        {
            camera = mjpeg_camera_gst_element(dec->camera[i]);
        }

        pthread_cond_init(&dec->wait_input[i], NULL);

        /* ...add camera to the bin */
        gst_bin_add(GST_BIN(bin), camera);

        /* ...initialize individual stream structures */
        dec->stream[i].cb = cb;
        dec->stream[i].cdata = cdata;
        dec->stream[i].id = i;

        /* ... create and insert jpeg decoder into pipeline */
        decoder = gst_element_factory_make("jpegdec", NULL);
        g_assert(decoder);
        gst_object_ref(decoder);
        gst_bin_add_many(GST_BIN(bin), decoder, NULL);

        /* ...link with camera element */
        gst_element_link(camera, decoder);
        gst_element_sync_state_with_parent (decoder);

        dpad = gst_element_get_static_pad(decoder, "src");
        caps = gst_pad_query_caps(dpad, NULL);

        /* ...create video sink */
        sink = video_sink_create(caps, &vsink_mjpeg_cb, &dec->stream[i]);

        /* ...add sink to a stream bin */
        gst_bin_add(GST_BIN(bin), sink);
        gst_element_link(decoder, sink);
        gst_element_sync_state_with_parent (sink);

    }

    /* ...clear number of queued/busy output buffers */
    dec->output_count = dec->output_busy = 0;

    /* ...initialize internal queue access lock */
    pthread_mutex_init(&dec->lock, NULL);

    /* ...initialize decoding thread conditional variable */
    pthread_cond_init(&dec->wait, NULL);

    /* ...initialize conditional variable for flushing */
    pthread_cond_init(&dec->flush_wait, NULL);

    /* ...initialize decoder runtime (image size hardcoded for now - tbd) */
    if ((errno = -mjpeg_runtime_init(dec, width, height)) != 0)
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

error:
    /* ...destroy data object */
    return NULL;
}

/* ...pass packet to a camera */
void camera_mjpeg_packet_receive(int id, u8 *pdu, u16 len, u64 ts)
{
    camera_packet_receive(__dec.camera[id], pdu, len, ts);
}
