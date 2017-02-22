/*******************************************************************************
 *
 * Gstreamer video sink for rendering via EGL
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

#define MODULE_TAG                      VSINK

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <sys/mman.h>

#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>
#include <gst/allocators/gstdmabuf.h>
#include <sys/mman.h>

#include "main.h"
#include "common.h"
#include "display.h"
#include "vsink.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);
TRACE_TAG(BUFFER, 1);

/*******************************************************************************
 * Forward declarations - tbd
 ******************************************************************************/

/* ...contiguous memory allocator */
extern GstAllocator * vpool_allocator_new(void);

/* ...custom video buffers pool creation */
extern GstBufferPool * gst_vsink_buffer_pool_new(
            void (*alloc)(GstBuffer *buf, void *cdata),
            void *cdata);

G_DEFINE_QUARK(GstVsinkMetaQuark, gst_buffer_vsink);

/* test mutex */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/* ...custom video sink node */
struct video_sink
{
    /* ...application sink node */
    GstAppSink                 *appsink;

    /* ...buffer pool */
    GstBufferPool              *pool;

    /* ...buffer allocator */
    GstAllocator               *allocator;

    /* ...user-provided callbacks */
    const vsink_callback_t     *cb;

    /* ...processing function custom data */
    void                       *cdata;
};

/*******************************************************************************
 * Custom buffer metadata implementation
 ******************************************************************************/

/* ...metadata type registration */
GType vsink_meta_api_get_type(void)
{
    static volatile GType type;
    static const gchar *tags[] =
        {
            GST_META_TAG_VIDEO_STR,
            GST_META_TAG_MEMORY_STR,
            NULL
        };

    if (g_once_init_enter(&type))
    {
        GType _type = gst_meta_api_type_register("VideoSinkMetaAPI", tags);
        g_once_init_leave(&type, _type);
    }

    return type;
}

/* ...low-level interface */
static gboolean vsink_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
    vsink_meta_t   *vmeta = (vsink_meta_t *) meta;

    /* ...reset fields */
    memset(meta + 1, 0, sizeof(*vmeta) - sizeof(*meta));

    return TRUE;
}

/* ...metadata transformation */
static gboolean vsink_meta_transform(GstBuffer *transbuf,
                                     GstMeta *meta,
                                     GstBuffer *buffer,
                                     GQuark type,
                                     gpointer data)
{
    /* ...just copy data regardless of transform type? */
    (void)gst_buffer_add_vsink_meta(transbuf);

    return TRUE;
}

/* ...metadata release */
static void vsink_meta_free(GstMeta *meta, GstBuffer *buffer)
{
    vsink_meta_t   *vmeta = (vsink_meta_t *) meta;

    /* ...notify sink about buffer destruction? */
    TRACE(DEBUG, _b("free metadata %p, buffer: %p, refcount: %d"),
          vmeta, buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
}

/* ...register metadata implementation */
const GstMetaInfo * vsink_meta_get_info(void)
{
    static const GstMetaInfo *meta_info = NULL;

    if (g_once_init_enter(&meta_info))
    {
        const GstMetaInfo *mi = gst_meta_register(
            VSINK_META_API_TYPE,
            "VideoSinkMeta",
            sizeof(vsink_meta_t),
            vsink_meta_init,
            vsink_meta_free,
            vsink_meta_transform);

        g_once_init_leave (&meta_info, mi);
    }

    return meta_info;
}

/*******************************************************************************
 * GPU memory allocator
 ******************************************************************************/


/*******************************************************************************
 * Buffer allocation
 ******************************************************************************/

/* ...allocate video-buffer (custom query from OMX component) */
static GstBuffer * vsink_buffer_create(video_sink_t *sink,
                                       gint *dmabuf,
                                       GstAllocator *allocator,
                                       gint width,
                                       gint height,
                                       gint *stride,
                                       gpointer *planebuf,
                                       GstVideoFormat format,
                                       int n_planes)
{
    GstBuffer          *buffer = gst_buffer_new();
    vsink_meta_t       *meta = gst_buffer_add_vsink_meta(buffer);
    GstMemory          *m;
    int                 i;

    TRACE(BUFFER, _b("new buffer: %p, refcount: %d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...save meta-format */
    meta->width = width;
    meta->height = height;
    meta->format = format;
    meta->sink = sink;

    /* ...avoid detaching of metadata when buffer is returned to a pool */
    GST_META_FLAG_SET(meta, GST_META_FLAG_POOLED);

    /* ...buffer meta-data creation - tbd */
    switch (format)
    {
    case GST_VIDEO_FORMAT_NV12:
        /* ...make sure we have two planes */
        if (n_planes != 2)
        {
            goto error_planes;
        }

        TRACE(BUFFER, _b("allocate NV12 %u*%u texture (buffer=%p, meta=%p, refcount=%d)"),
              width, height, buffer, meta, GST_MINI_OBJECT_REFCOUNT(buffer));

        /* ...add buffer metadata */
        meta->plane[0] = planebuf[0];
        meta->plane[1] = planebuf[1];
        meta->dmafd[0] = dmabuf[0];
        meta->dmafd[1] = dmabuf[1];

        /* ...invoke user-supplied allocation callback */
        if (sink->cb->allocate(buffer, sink->cdata))
        {
            goto error_user;
        }

        TRACE(BUFFER, _b("allocated %u*%u NV12 buffer: %p (dmafd=%d,%d), refcount=%d"),
              width, height, buffer, dmabuf[0], dmabuf[1], GST_MINI_OBJECT_REFCOUNT(buffer));

        break;

    default:
        /* ...unrecognized format; give up */
        TRACE(ERROR, _b("unsupported buffer format: %s"),
                gst_video_format_to_string(format));
        goto error;
    }

    /* ...add fake memory to the buffer */
    for (i = 0; i < n_planes; i++)
    {
        /* ...the memory allocated in that way cannot be used by GPU */
        m = gst_dmabuf_allocator_alloc(allocator, dmabuf[i], 0);

        /* ...but we still need to put some memory to the buffer to mark buffer is allocated */
        gst_buffer_append_memory(buffer, m);
    }

    return buffer;

error_user:
    TRACE(ERROR, _b("buffer creation rejected by user"));
    goto error;

error_planes:
    TRACE(ERROR, _b("invalid number of planes for format '%s': %d"),
            gst_video_format_to_string(format), n_planes);
    goto error;

error:
    /* ...release buffer */
    gst_object_unref(buffer);
    TRACE(BUFFER, _b("error exit: buffer %p, refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
    return NULL;
}

static void __vsink_alloc(GstBuffer *buffer, void *cdata)
{
    video_sink_t   *sink = cdata;
    vsink_meta_t   *vmeta = gst_buffer_get_vsink_meta(buffer);

    TRACE(BUFFER, _b("vsink alloc buffer: %p, refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...pass notification further to user-provided callback */
    vmeta->sink = sink;

    sink->cb->allocate(buffer, sink->cdata);
}

/* ...buffer probing callback */
static GstPadProbeReturn vsink_probe(GstPad *pad,
                                     GstPadProbeInfo *info,
                                     gpointer user_data)
{
    video_sink_t   *sink = user_data;

    TRACE(0, _b("video-sink[%p]: probe <%X, %lu, %p, %zX, %u>"),
            sink, info->type, info->id, info->data, info->offset, info->size);

    if (info->type & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM)
    {
        GstQuery        *query = GST_PAD_PROBE_INFO_QUERY(info);

        if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION)
        {
            GstBufferPool          *pool = sink->pool;
            GstAllocator           *allocator = sink->allocator;
            GstCaps                *caps;
            GstAllocationParams     params;
            guint                   size;
            guint                   min = 4;
            guint                   max = 4;
            GstVideoInfo            vinfo;
            GstStructure           *config;
            gboolean                need_pool;
            gchar                  *str;

            /* ...parse allocation parameters from the query */
            gst_allocation_params_init(&params);
            gst_query_parse_allocation(query, &caps, &need_pool);
            gst_video_info_from_caps(&vinfo, caps);

            str = gst_caps_to_string(caps);
            TRACE(DEBUG, _b("caps={%s}; need-pool=%d, writable:%d"),
                    str, need_pool, gst_query_is_writable(query));
            free(str);

            /* ...check if we already have a pool allocated */
            if (pool)
            {
                GstCaps    *_caps;

                config = gst_buffer_pool_get_config(pool);
                gst_buffer_pool_config_get_params(config, &_caps, &size, NULL, NULL);

                if (!gst_caps_is_equal(caps, _caps))
                {
                    TRACE(INFO, _b("caps are different; destroy pool"));
                    gst_object_unref(pool);
                    sink->pool = pool = NULL;
                    gst_object_unref(allocator);
                    sink->allocator = allocator = NULL;
                }
                else
                {
                    TRACE(DEBUG, _b("caps are same"));
                }

                gst_structure_free(config);
            }

            /* ...allocate pool if needed */
            if (pool == NULL && need_pool)
            {
                /* ...create new buffer pool */
                sink->pool = pool = gst_vsink_buffer_pool_new(__vsink_alloc, sink);
                size = vinfo.size;

                TRACE(DEBUG, _b("pool allocated: %u/%u/%u"), size, min, max);

                /* ...create DMA allocator */
                sink->allocator = allocator = vpool_allocator_new();

                /* ...configure buffer pool */
                config = gst_buffer_pool_get_config(pool);
                gst_buffer_pool_config_set_params(config, caps, size, min, max);
                gst_structure_set(config, "videosink_buffer_creation_request_supported",
                        G_TYPE_BOOLEAN, TRUE, NULL);
                gst_buffer_pool_config_set_allocator(config, allocator, &params);
                gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
                gst_buffer_pool_set_config(pool, config);
            }

            /* ...add allocation pool description */
            if (pool)
            {
                /* ...add allocation pool */
                gst_query_add_allocation_pool(query, pool, size, min, max);

                /* ...put create DMA allocator as well */
                gst_query_add_allocation_param(query, allocator, &params);

                /* ...should we unref allocator or we just pass ownership? - tbd */

                TRACE(DEBUG, _b("query: %p added pool %p, allocator: %p"),
                      query, pool, allocator);
            }

            /* ...output query parameters */
            TRACE(DEBUG, _b("query[%p]: alloc: %d, pools: %d"),
                    query,
                    gst_query_get_n_allocation_params(query),
                    gst_query_get_n_allocation_pools(query));

            /* ...do not pass allocation request to the component */
            return GST_PAD_PROBE_HANDLED;
        }
        else if (GST_QUERY_TYPE(query) == GST_QUERY_CUSTOM)
        {
            const GstStructure  *structure = gst_query_get_structure(query);
            GstStructure    *str_writable = gst_query_writable_structure(query);
            gint dmabuf[GST_VIDEO_MAX_PLANES] = { 0 };
            GstAllocator *allocator;
            gint width, height;
            gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
            gpointer planebuf[GST_VIDEO_MAX_PLANES] = { 0 };
            const gchar *str;
            const GValue *p_val;
            GValue val = { 0, };
            GstVideoFormat format;
            GstBuffer *buffer;
            GArray *dmabuf_array;
            GArray *stride_array;
            GArray *planebuf_array;
            gint n_planes;
            gint i;

            /* ...chek for a buffer creation request */
            if (!structure ||
                !gst_structure_has_name (structure,
                                         "videosink_buffer_creation_request")
               )
            {
                TRACE(DEBUG, _b("unknown query"));
                return GST_PAD_PROBE_DROP;
            }

            /* ...retrieve allocation parameters from the structure */
            gst_structure_get (structure,
                               "width", G_TYPE_INT, &width,
                               "height", G_TYPE_INT, &height,
                               "stride", G_TYPE_ARRAY, &stride_array,
                               "dmabuf", G_TYPE_ARRAY, &dmabuf_array,
                               "planebuf", G_TYPE_ARRAY, &planebuf_array,
                               "n_planes", G_TYPE_INT, &n_planes,
                               "allocator", G_TYPE_POINTER, &p_val,
                               "format", G_TYPE_STRING, &str, NULL);

            allocator = (GstAllocator *) g_value_get_pointer(p_val);
            g_assert(allocator);

            format = gst_video_format_from_string(str);
            g_assert(format != GST_VIDEO_FORMAT_UNKNOWN);

            for (i = 0; i < n_planes; i++)
            {
                dmabuf[i] = g_array_index (dmabuf_array, gint, i);
                stride[i] = g_array_index (stride_array, gint, i);
                planebuf[i] = g_array_index (planebuf_array, gpointer, i);
                TRACE(DEBUG, _b("plane-%d: dmabuf=%d, stride=%d, planebuf=%p"),
                        i,
                        dmabuf[i],
                        stride[i],
                        planebuf[i]);
            }

            buffer = vsink_buffer_create(sink, dmabuf, allocator, width,
                    height, stride, planebuf, format, n_planes);

            TRACE(BUFFER, _b("buffer created: %p, refcount=%d"),
                  buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

            g_value_init(&val, GST_TYPE_BUFFER);
            gst_value_set_buffer(&val, buffer);
            gst_buffer_unref(buffer);

            TRACE(BUFFER, _b("return: buffer: %p, refcount=%d"),
                  buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

            gst_structure_set_value (str_writable, "buffer", &val);

            TRACE(DEBUG, _b("return ok"));

            /* ...do not pass allocation request to the component */
            return GST_PAD_PROBE_HANDLED;
        }
        else if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS)
        {
            GstCaps    *caps = gst_app_sink_get_caps(sink->appsink);

            /* ...pass current capabilities */
            gst_query_set_caps_result(query, caps);
            gst_caps_unref(caps);
        }
        else if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS)
        {
            GstCaps    *caps;
            gchar      *str;

            /* ...parse capabilities provided by upstream */
            gst_query_parse_accept_caps(query, &caps);
            str = gst_caps_to_string(caps);
            TRACE(DEBUG, _b("accepted caps={%s}"), str);
            free(str);

            /* ...mark it's all good */
            gst_query_set_accept_caps_result(query, TRUE);
        }
        else
        {
            TRACE(DEBUG, _b("unknown query type: %X"), GST_QUERY_TYPE(query));
        }
    }
    else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
    {
        TRACE(0, _b("event-dowstream"));
    }

    /* ...everything else is passed to the sink? */
    return GST_PAD_PROBE_OK;
}

static int vsink_buffer_check(video_sink_t *sink, GstBuffer *buffer)
{
    vsink_meta_t   *meta = gst_buffer_get_vsink_meta(buffer);
    GstVideoMeta   *vmeta;
    GstMemory      *mem;
    unsigned        i;
    gboolean        is_dma;

    TRACE(BUFFER, _b("buffer check: buffer: %p, refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...check if we have metadata already */
    if (meta)
    {
        return 0;
    }

    /* ...get video meta */
    vmeta = gst_buffer_get_video_meta(buffer);
    if (!vmeta)
    {
        return 0;
    }

    /* ...create new meta-data */
    meta = gst_buffer_add_vsink_meta(buffer);
    if (meta == NULL)
    {
        TRACE(ERROR, _x("failed to allocate meta"));
        return -ENOMEM;
    }

    TRACE(BUFFER, _b("meta %p buffer %p, refcount=%d"),
          meta, buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...save meta-format */
    meta->width = vmeta->width;
    meta->height = vmeta->height;
    meta->format = vmeta->format;

    is_dma = gst_is_dmabuf_memory(gst_buffer_peek_memory(buffer, 0));
    if (is_dma)
    {
        meta->n_dma = meta->n_planes = vmeta->n_planes;
        TRACE(BUFFER, _b("buffer %p getting DMA fds, refcount=%d"),
              buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
    }
    else
    {
        meta->n_dma = 0;
    }

    /* ...avoid detaching of metadata when buffer is returned to a pool */
    GST_META_FLAG_SET(meta, GST_META_FLAG_POOLED);

    /* ...map memory */
    for (i = 0; i < vmeta->n_planes; i++)
    {
        mem = gst_buffer_peek_memory(buffer, i);
        if (is_dma)
        {
            meta->dmafd[i] = gst_dmabuf_memory_get_fd(mem);
            meta->offsets[i] = mem->offset;
            meta->plane[i] = mmap(NULL,
                                  mem->size,
                                  PROT_READ,
                                  MAP_SHARED,
                                  meta->dmafd[i],
                                  mem->offset);
            if (meta->plane[i] == MAP_FAILED)
            {
                TRACE(ERROR, _x("failed to map memory"));
                return -ENOMEM;
            }
        }
        else
        {
            GstMapInfo map = {0};
            /* ...map buffer into CPU space */
            if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
            {
                TRACE(ERROR, _x("Could not map buffer."));
                return -1;
            }
            meta->plane[0] = map.data;
            meta->dmafd[i] = -1;
            /* FIXME: how to handle properly buffer mapping in this case?
               Now just free and hope memory will be valid for some time */
            gst_buffer_unmap(buffer, &map);
        }
        TRACE(BUFFER, _b("plane[%d]: dmafd=%d, addr=%p, size=%zX, offset=%zX, errno: %d, buffer: %p, refcount=%d"),
              i, meta->dmafd[i], meta->plane[i], mem->size, mem->offset, errno, buffer, GST_MINI_OBJECT_REFCOUNT(buffer));
    }

    meta->is_dma = is_dma;

    TRACE(INFO, _b("Invoke user applied callback"));

    /* ...invoke user-supplied allocation callback */
    CHK_API(sink->cb->allocate(buffer, sink->cdata));

    TRACE(INFO, _b("buffer %p acquired (refcount=%d)"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    return 0;
}


/*******************************************************************************
 * Video sink implementation
 ******************************************************************************/

/* ...end-of-stream submission */
static void vsink_eos(GstAppSink *appsink, gpointer user_data)
{
    video_sink_t   *sink = user_data;

    TRACE(DEBUG, _b("video-sink[%p]::eos called"), sink);
}

/* ...new preroll sample is available */
static GstFlowReturn vsink_new_preroll(GstAppSink *appsink, gpointer user_data)
{
    video_sink_t   *sink = user_data;
    GstSample      *sample;
    GstBuffer      *buffer;
    int             r = 0;

    TRACE(DEBUG, _b("video-sink[%p]::new-preroll called"), sink);

    /* ...retrieve new sample from the pipe */
    sample = gst_app_sink_pull_preroll(sink->appsink);
    buffer = gst_sample_get_buffer(sample);

    TRACE(BUFFER, _b("preroll: buffer: %p, refcount=%d"),
          buffer, GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...process frame; invoke user-provided callback if given */
    if (sink->cb->preroll != NULL)
    {
        /* ...make sure buffer is good */
        CHK_ERR(vsink_buffer_check(sink, buffer) == 0, GST_FLOW_ERROR);

        r = sink->cb->preroll(buffer, sink->cdata);
    }

    /* ...release the sample (and buffer automatically unless user adds a reference) */
    gst_sample_unref(sample);

    return (r < 0 ? GST_FLOW_ERROR : GST_FLOW_OK);
}

/* ...new sample is available */
static GstFlowReturn vsink_new_sample(GstAppSink *appsink, gpointer user_data)
{
    video_sink_t   *sink = user_data;
    GstSample      *sample;
    GstBuffer      *buffer;
    int             r;

    TRACE(DEBUG, _b("video-sink[%p]::new-samples called"), sink);

    /* ...retrieve new sample from the pipe */
    sample = gst_app_sink_pull_sample(sink->appsink);
    buffer = gst_sample_get_buffer(sample);

    TRACE(BUFFER, _b("buffer: %p, timestamp: %zu, refcount=%d"),
          buffer, GST_BUFFER_PTS(buffer), GST_MINI_OBJECT_REFCOUNT(buffer));

    /* ...make sure buffer is good */
    CHK_ERR(vsink_buffer_check(sink, buffer) == 0, GST_FLOW_ERROR);

    /* ...process frame; invoke user-provided callback */
    r = sink->cb->process(buffer, sink->cdata);

    /* ...release the sample (and buffer automatically unless user adds a reference) */
    gst_sample_unref(sample);

    return (r < 0 ? GST_FLOW_ERROR : GST_FLOW_OK);
}

/* ...element destruction notification */
static void vsink_destroy(gpointer data)
{
    video_sink_t   *sink = data;

    TRACE(INIT, _b("video-sink[%p] destroy notification"), sink);

    /* ...destroy buffer pool if allocated (doesn't look great - memleaks - tbd) */
    if (sink->pool != NULL)
    {
        gst_object_unref(sink->pool);
    }

    TRACE(INIT, _b("video-sink[%p] deallocate"), sink);

    free(sink);

    TRACE(INIT, _b("video-sink[%p] destroyed"), sink);
}

/* ...sink node callbacks */
static GstAppSinkCallbacks  vsink_callbacks =
{
    .eos = vsink_eos,
    .new_preroll = vsink_new_preroll,
    .new_sample = vsink_new_sample,
};

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...create custom video sink node */
GstElement * video_sink_create(GstCaps *caps,
                               const vsink_callback_t *cb,
                               void *cdata)
{
    video_sink_t   *sink;
    GstPad         *pad;

    /* ...allocate data */
    sink = malloc(sizeof(*sink));
    if (sink == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory"));
        return NULL;
    }

    /* ...create application source item */
    sink->appsink = (GstAppSink *)gst_element_factory_make("appsink", NULL);
    if (sink->appsink == NULL)
    {
        TRACE(ERROR, _x("element creation failed"));
        goto error;
    }

    /* ...by default, consume buffers as fast as they arrive */
    g_object_set(G_OBJECT(sink->appsink), "sync", FALSE, NULL);

    /* ...reset pool/allocator handles */
    sink->pool = NULL;
    sink->allocator = NULL;

    /* ...set processing function */
    sink->cb = cb;
    sink->cdata = cdata;

    /* ...bind control interface */
    gst_app_sink_set_callbacks(sink->appsink,
                               &vsink_callbacks,
                               sink,
                               vsink_destroy);

    /* ...set stream capabilities (caps are not fixed yet) */
    gst_app_sink_set_caps(sink->appsink, caps);

    /* ...set probing function for a sink pad to intercept allocations */
    pad = gst_element_get_static_pad(GST_ELEMENT(sink->appsink), "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_ALL_BOTH, vsink_probe, sink, NULL);
    gst_object_unref(pad);

    TRACE(INIT, _b("video-sink[%p] created"), sink);

    return GST_ELEMENT(sink->appsink);

error:
    /* ...destroy data allocated */
    free(sink);
    return NULL;
}
