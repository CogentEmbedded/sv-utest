/*******************************************************************************
 *
 * Gstreamer video pool
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
#define MODULE_TAG                      VPOOL

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <glob.h>
#include <mmngr_user_public.h>
#include <gst/video/gstvideopool.h>
#include <sys/syscall.h>

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
 * Memory allocator
 ******************************************************************************/

/* ...memory descriptor */
typedef struct vpool_mem
{
    /* ...base memory descriptor */
    GstMemory           mem;

    /* ...identifier of the memory buffer */
    MMNGR_ID            id;

    /* ...physical address */
    unsigned long       phy_addr;

    /* ...user-accessible pointer */
    unsigned long       user_virt_addr;

    /* ...size of a chunk */
    unsigned long       size;

}   vpool_mem_t;

/* ...types definition */
typedef struct GstVPoolAllocator
{
    /* ...generic allocator structure */
    GstAllocator        parent;

}   GstVPoolAllocator;

typedef struct GstVPoolAllocatorClass
{
    GstAllocatorClass   parent_class;

}   GstVPoolAllocatorClass;

GType vpool_mem_allocator_get_type(void);
G_DEFINE_TYPE(GstVPoolAllocator, vpool_mem_allocator, GST_TYPE_ALLOCATOR);

#define GST_TYPE_VPOOL_ALLOCATOR    (vpool_mem_allocator_get_type())
#define GST_IS_VPOOL_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VPOOL_ALLOCATOR))

/* ...memory allocation hook */
static GstMemory *__vpool_alloc(GstAllocator *allocator, gsize size, GstAllocationParams *params)
{
    vpool_mem_t        *mem;
    int                 err;
    unsigned long       pha;

    /* ...allocate descriptor */
    CHK_ERR(mem = g_slice_new0(vpool_mem_t), (errno = ENOMEM, NULL));

    /* ...initialize memory structure */
    gst_memory_init(GST_MEMORY_CAST(mem), 0, allocator, NULL, size, 0, 0, size);

    /* ...save size */
    mem->size = size;

    /* ...allocate physically contiguous memory */
    err = mmngr_alloc_in_user(&mem->id,
                              size,
                              &mem->phy_addr,
                              &pha,
                              &mem->user_virt_addr,
                              MMNGR_VA_SUPPORT);
    switch (err)
    {
    case R_MM_OK:
        /* ...memory allocated successfully */
        TRACE(DEBUG, _b("allocated %p[%zu] block[%X] (pa=%08lx)"),
              (void *)(uintptr_t)mem->user_virt_addr, size, mem->id, mem->phy_addr);
        return (GstMemory *) mem;

    case R_MM_NOMEM:
        /* ...insufficient memory */
        TRACE(ERROR, _x("failed to allocated contiguous memory block (%zu bytes)"), size);
        errno = ENOMEM;
        return NULL;

    default:
        /* ...internal allocation error */
        TRACE(ERROR, _x("memory allocation error (%zu bytes), err=%d"), size, err);
        errno = EBADF;
        return NULL;
    }
}

/* ...free memory block */
static void __vpool_free(GstAllocator *allocator, GstMemory *gmem)
{
    vpool_mem_t    *mem = (vpool_mem_t *)gmem;

    /* ...free allocated memory */
    mmngr_free_in_user(mem->id);
    TRACE(DEBUG, _b("destroyed block #%X (va=%p)"),
          mem->id, (void *)(uintptr_t)mem->user_virt_addr);

    /* ...destroy memory descriptor */
    g_slice_free(vpool_mem_t, mem);
}

/* ...memory mapping function */
static gpointer __vpool_mem_map(GstMemory *_mem, gsize maxsize, GstMapFlags flags)
{
    vpool_mem_t    *mem = (vpool_mem_t *)_mem;

    TRACE(DEBUG, _b("block #%X [%p] mapped"),
          mem->id, (void *)(uintptr_t)mem->user_virt_addr);
    return (gpointer)(uintptr_t)mem->user_virt_addr;
}

/* ...memory unmapping function */
static void __vpool_mem_unmap(GstMemory *_mem)
{
    vpool_mem_t    *mem = (vpool_mem_t *)_mem;

    TRACE(DEBUG, _b("block #%X [%p] unmapped"), mem->id, (void *)(uintptr_t)mem->user_virt_addr);
}

/* ...class initialization function */
static void vpool_mem_allocator_class_init(GstVPoolAllocatorClass *klass)
{
    GstAllocatorClass   *allocator_class = (GstAllocatorClass *) klass;

    allocator_class->alloc = __vpool_alloc;
    allocator_class->free = __vpool_free;
}

/* ...allocator initialization function */
static void vpool_mem_allocator_init(GstVPoolAllocator *allocator)
{
    GstAllocator   *alloc = GST_ALLOCATOR_CAST(allocator);

    alloc->mem_type = "vpoolbuf";
    alloc->mem_map = __vpool_mem_map;
    alloc->mem_unmap = __vpool_mem_unmap;

    /* ...set custom allocator marker */
    GST_OBJECT_FLAG_SET(allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...create new allocator */
GstAllocator * vpool_allocator_new(void)
{
    return g_object_new(GST_TYPE_VPOOL_ALLOCATOR, NULL);
}

/*******************************************************************************
 * Buffer pool support
 ******************************************************************************/

typedef struct GstVsinkBufferPool
{
    /* ...parent class */
    GstVideoBufferPool      parent;

    /* ...notification callback */
    void                  (*alloc)(GstBuffer *buffer, void *cdata);

    /* ...client data */
    void                   *cdata;

    /* ...video info */
    GstVideoInfo            info;

}   GstVsinkBufferPool;

/* ...buffer class definition */
typedef struct GstVsinkBufferPoolClass
{
    GstVideoBufferPoolClass     parent_class;

}   GstVsinkBufferPoolClass;

GType gst_vsink_buffer_pool_get_type(void);

#define GST_TYPE_VSINK_BUFFER_POOL          (gst_vsink_buffer_pool_get_type())

#define GST_IS_VSINK_BUFFER_POOL(obj)       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
        GST_TYPE_VSINK_BUFFER_POOL))

#define GST_VSINK_BUFFER_POOL(obj)          (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
        GST_TYPE_VSINK_BUFFER_POOL, GstVsinkBufferPool))

#define GST_VSINK_BUFFER_POOL_CAST(obj)     ((GstVsinkBufferPool*)(obj))

#define gst_vsink_buffer_pool_parent_class  parent_class
G_DEFINE_TYPE(GstVsinkBufferPool, gst_vsink_buffer_pool, GST_TYPE_VIDEO_BUFFER_POOL);

/* ...buffer allocation function */
static GstFlowReturn gst_vsink_buffer_pool_alloc_buffer(GstBufferPool *_pool,
                GstBuffer **buffer, GstBufferPoolAcquireParams *params)
{
    GstVsinkBufferPool *pool = GST_VSINK_BUFFER_POOL_CAST(_pool);
    GstBuffer          *buf;
    GstVideoMeta       *meta;
    vsink_meta_t       *vmeta;
    GstMemory          *mem;
    GstMapInfo          info;
    GstFlowReturn       r;

    TRACE(BUFFER, _b("buffer allocation"));

    /* ...pass to parent class first */
    r = GST_BUFFER_POOL_CLASS(parent_class)->alloc_buffer(_pool, buffer, params);
    if (r != GST_FLOW_OK || !(buf = *buffer))
    {
        TRACE(ERROR, _x("failed to allocate buffer"));
        return r;
    }

    /* ...get video meta */
    if ((meta = gst_buffer_get_video_meta(buf)) == NULL)
    {
        TRACE(ERROR, _x("no video meta provided"));
        return GST_FLOW_OK;
    }
    else if ((mem = gst_buffer_get_memory(buf, 0)) == NULL)
    {
        TRACE(ERROR, _x("failed to get memory"));
        return GST_FLOW_OK;
    }
    else if (!gst_memory_map(mem, &info, GST_MAP_READ))
    {
        TRACE(ERROR, _x("failed to map user memory"));
        return GST_FLOW_OK;
    }

    /* ...add custom buffer metadata */
    vmeta = gst_buffer_add_vsink_meta(buf);
    vmeta->width = meta->width;
    vmeta->height = meta->height;
    vmeta->format = meta->format;
    vmeta->plane[0] = info.data;

    /* ...avoid detaching of metadata when buffer is returned to a pool */
    GST_META_FLAG_SET(vmeta, GST_META_FLAG_POOLED);

    /* ...notify caller */
    (pool->alloc ? pool->alloc(buf, pool->cdata) : 0);

    /* ...unmap memory afterwards */
    gst_memory_unmap(mem, &info);
    gst_memory_unref(mem);

    TRACE(BUFFER, _b("buffer allocated [%p], refcount=%d"), buf, GST_MINI_OBJECT_REFCOUNT(buf));

    return GST_FLOW_OK;
}

/* ...buffer acquisition callback */
static inline GstFlowReturn gst_vsink_buffer_pool_acquire_buffer(GstBufferPool *_pool,
                GstBuffer **buffer, GstBufferPoolAcquireParams *params)
{
    GstVsinkBufferPool *pool = GST_VSINK_BUFFER_POOL_CAST(_pool);
    GstFlowReturn       ret;

    /* ...call parent class first */
    ret = GST_BUFFER_POOL_CLASS(pool)->acquire_buffer(_pool, buffer, params);
    if (ret != GST_FLOW_OK || !*buffer)
    {
        TRACE(ERROR, _x("failed to acquire buffer: %d"), ret);
        return ret;
    }

    return ret;
}

/* ...buffer class destructor (need that? - tbd) */
static inline void gst_vsink_buffer_pool_finalize(GObject * object)
{
    GstVsinkBufferPool     *pool = GST_VSINK_BUFFER_POOL_CAST(object);

    /* ...call parent class destructor */
    G_OBJECT_CLASS(pool)->finalize(object);
}

/* ...buffer class initializer */
static void gst_vsink_buffer_pool_class_init(GstVsinkBufferPoolClass * klass)
{
    GstBufferPoolClass  *pool_class = (GstBufferPoolClass *) klass;

    pool_class->alloc_buffer = gst_vsink_buffer_pool_alloc_buffer;
}

/* ...buffer pool initialization function */
static void gst_vsink_buffer_pool_init(GstVsinkBufferPool *pool)
{
    /* ...nothing? - tbd */
}

/* ...entry point */
GstBufferPool * gst_vsink_buffer_pool_new(void (*alloc)(GstBuffer *buf, void *cdata), void *cdata)
{
    GstVsinkBufferPool     *pool;

    /* ...instantiate object */
    CHK_ERR(pool = g_object_new(GST_TYPE_VSINK_BUFFER_POOL, NULL), (errno = ENOMEM, NULL));

    /* ...set user-provided callback */
    pool->alloc = alloc, pool->cdata = cdata;

    TRACE(BUFFER, _b("buffer pool created: %p"), pool);

    return GST_BUFFER_POOL_CAST(pool);
}
