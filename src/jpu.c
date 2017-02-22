/*******************************************************************************
 *
 * JPEG decoding using V4L2 JPU module
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

#define MODULE_TAG                      JPU

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "main.h"
#include "common.h"
#include "jpu.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 0);

/*******************************************************************************
 * Local types definition
 ******************************************************************************/

/* ...static module data */
struct jpu_data
{
    /* ...V4L2 file descriptor */
    int                 vfd;

    /* ...maximal input size */
    u32                 max_in_size;

    /* ...input buffer pool */
    jpu_buffer_t       *input_pool;

    /* ...output buffer pool */
    jpu_buffer_t       *output_pool;
};

/*******************************************************************************
 * Internal helpers functions
 ******************************************************************************/

/* ...check video device capabilities */
static inline int __jpu_check_caps(struct v4l2_capability *cap)
{
    u32     caps = cap->device_caps;

    if (!(caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE))
    {
        TRACE(ERROR, _x("multi-planar output expected: %X"), caps);
        return -1;
    }
    else if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
    {
        TRACE(ERROR, _x("multi-planar output expected: %X"), caps);
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

/* ...start streaming on specific V4L2 device */
static inline int jpu_streaming_enable(int fd, int capture, int enable)
{
    int     type = (capture ?
                    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
                    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    return CHK_API(ioctl(fd,
                         (enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF),
                         &type));
}

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...get decoder capture file-descriptor */
int jpu_capture_fd(jpu_data_t *jpu)
{
    return jpu->vfd;
}

/* ...prepare JPU module for operation */
int jpu_set_formats(jpu_data_t *jpu, int width, int height, int max_in_size)
{
    int                 vfd = jpu->vfd;
    struct v4l2_format  fmt;

    /* ...set input format (single-plane JPEG always) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix.sizeimage = jpu->max_in_size = max_in_size;
    CHK_API(ioctl(vfd, VIDIOC_S_FMT, &fmt));

    /* ...set output format (single-plane NV12 always - tbd) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.num_planes = 1;
    CHK_API(ioctl(vfd, VIDIOC_S_FMT, &fmt));

    return 0;
}

/* ...allocate output/capture buffer pool */
int jpu_allocate_buffers(jpu_data_t *jpu,
                         int capture,
                         jpu_buffer_t *pool,
                         u8 num)
{
    struct v4l2_requestbuffers  reqbuf;

    /* ...all buffers are allocated by kernel */
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.type = (capture ?
                   V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
                   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = num;
    CHK_API(ioctl(jpu->vfd, VIDIOC_REQBUFS, &reqbuf));
    CHK_ERR(reqbuf.count == num, -(errno = ENOMEM));

    /* ...process buffers allocated */
    if (capture)
    {
        struct v4l2_buffer  buf;
        struct v4l2_plane   planes[1];

        /* ...prepare query data */
        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        /* ...process individual capture buffers */
        for (buf.index = 0; buf.index < num; buf.index++)
        {
            jpu_buffer_t   *_buf = &pool[buf.index];

            /* ...query buffer */
            CHK_API(ioctl(jpu->vfd, VIDIOC_QUERYBUF, &buf));
            _buf->m.mem_offset[0] = planes[0].m.mem_offset;
            _buf->m.planebuf[0] = mmap(NULL,
                                       planes[0].length,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED,
                                       jpu->vfd,
                                       planes[0].m.mem_offset);
            CHK_ERR(_buf->m.planebuf[0] != MAP_FAILED, -errno);
            TRACE(DEBUG, _b("output-buffer-%d mapped: %p[%08X] (%u bytes)"),
                  buf.index,
                  _buf->m.planebuf[0],
                  _buf->m.mem_offset[0],
                  planes[0].length);
        }
    }
    else
    {
        struct v4l2_buffer  buf;
        struct v4l2_plane   planes[1];

        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        /* ...proces individual buffers */
        for (buf.index = 0; buf.index < num; buf.index++)
        {
            jpu_buffer_t   *_buf = &pool[buf.index];

            CHK_API(ioctl(jpu->vfd, VIDIOC_QUERYBUF, &buf));
            _buf->m.offset = planes[0].m.mem_offset;
            _buf->m.data = mmap(NULL,
                                planes[0].length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                jpu->vfd,
                                planes[0].m.mem_offset);

            CHK_ERR(_buf->m.data != MAP_FAILED, -ENOMEM);

            TRACE(DEBUG, _b("input-buffer-%d mapped: %p[%08X] (%u bytes)"),
                  buf.index,
                  _buf->m.data,
                  _buf->m.offset,
                  planes[0].length);
        }
    }

    /* ...should I store buffer pool somewhere? - tbd */

    /* ...start streaming as soon as we allocated buffers */
    CHK_API(jpu_streaming_enable(jpu->vfd, capture, 1));

    TRACE(INFO, _b("%s-pool allocated (%u buffers)"),
          (capture ? "output" : "input"), num);

    return 0;
}

/* ...allocate output/capture buffer pool */
int jpu_destroy_buffers(jpu_data_t *jpu,
                        int capture,
                        jpu_buffer_t *pool,
                        u8 num)
{
    struct v4l2_requestbuffers  reqbuf;
    struct v4l2_buffer          buf;
    struct v4l2_plane           planes[1];
    u8                          i;

    /* ...stop streaming before doing anything */
    CHK_API(jpu_streaming_enable(jpu->vfd, capture, 0));

    /* ...prepare query data */
    memset(&buf, 0, sizeof(buf));
    buf.type = (capture ?
                V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
                V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    CHK_API(ioctl(jpu->vfd, VIDIOC_QUERYBUF, &buf));

    TRACE(DEBUG, _b("destroy %s-pool: plane-length=%u"),
          (capture ? "output" : "input"), planes[0].length);

    /* ...check if we have capture or output buffer */
    if (capture)
    {
        for (i = 0; i < num; i++)
        {
            munmap(pool[i].m.planebuf[0], planes[0].length);
        }
    }
    else
    {
        for (i = 0; i < num; i++)
        {
            munmap(pool[i].m.data, planes[0].length);
        }
    }

    /* ...release kernel-allocated buffers */
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.type = (capture ?
                   V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
                   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = 0;
    CHK_API(ioctl(jpu->vfd, VIDIOC_REQBUFS, &reqbuf));

    TRACE(INFO, _b("%s-pool destroyed (%u buffers)"),
          (capture ? "output" : "input"), num);

    return 0;
}

/* ...enqueue input buffer */
int jpu_input_buffer_queue(jpu_data_t *jpu, int i, jpu_buffer_t *pool)
{
    struct v4l2_buffer  buf;
    struct v4l2_plane   planes[1];

    /* ...set buffer parameters */
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.m.planes = planes;
    buf.length = 1;
    planes[0].bytesused = pool[i].m.length;
    planes[0].length = jpu->max_in_size;
    planes[0].m.mem_offset = pool[i].m.offset;
    CHK_API(ioctl(jpu->vfd, VIDIOC_QBUF, &buf));

    TRACE(DEBUG, _b("input-buffer #%d queued"), i);
    return 0;
}

/* ...dequeue input buffer */
int jpu_input_buffer_dequeue(jpu_data_t *jpu)
{
    struct v4l2_buffer  buf;
    struct v4l2_plane   planes[1];

    /* ...set buffer parameters */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    CHK_API(ioctl(jpu->vfd, VIDIOC_DQBUF, &buf));

    TRACE(DEBUG, _b("input-buffer #%d dequeued"), buf.index);
    return buf.index;
}

/* ...enqueue output buffer */
int jpu_output_buffer_queue(jpu_data_t *jpu, int i, jpu_buffer_t *pool)
{
    struct v4l2_buffer  buf;
    struct v4l2_plane   planes[1];

    /* ...set buffer parameters (NV12 always - tbd) */
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.m.planes = planes;
    buf.length = 1;
    planes[0].m.mem_offset = pool[i].m.mem_offset[0];
    CHK_API(ioctl(jpu->vfd, VIDIOC_QBUF, &buf));

    TRACE(DEBUG, _b("output-buffer #%d queued"), i);
    return 0;
}

/* ...dequeue output buffer */
int jpu_output_buffer_dequeue(jpu_data_t *jpu)
{
    struct v4l2_buffer  buf;
    struct v4l2_plane   planes[1];

    /* ...set buffer parameters */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    CHK_API(ioctl(jpu->vfd, VIDIOC_DQBUF, &buf));

    TRACE(DEBUG, _b("output-buffer #%d dequeued"), buf.index);
    return buf.index;
}

/* ...module initialization */
jpu_data_t * jpu_init(const char *devname)
{
    jpu_data_t             *jpu;
    struct v4l2_capability  cap;

    /* ...allocate JPU data (it is a singleton in fact) */
    if ((jpu = malloc(sizeof(*jpu))) == NULL)
    {
        TRACE(ERROR, _x("failed to allocate memory"));
        errno = ENOMEM;
        return NULL;
    }

    /* ...open V4L2 decoder device */
    if ((jpu->vfd = open(devname, O_RDWR, O_NONBLOCK)) < 0)
    {
        TRACE(ERROR, _x("failed to open device '%s': %m"), devname);
        goto error;
    }
    else if (ioctl(jpu->vfd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        TRACE(ERROR, _x("failed to query device capabilities: %m"));
        goto error_fd;
    }

    TRACE(INFO, _b("V4L2 JPG decoder initialized (%s, fd=%d)"),
          devname, jpu->vfd);

    return jpu;

error_fd:
    /* ...close V4L2 device */
    close(jpu->vfd);

error:
    /* ...deallocate memory */
    free(jpu);
    return NULL;
}

/* ...module destructor */
void jpu_destroy(jpu_data_t *jpu)
{
    /* ...close device handle */
    close(jpu->vfd);

    /* ...deallocate memory */
    free(jpu);

    TRACE(INIT, _b("jpu module destroyed"));
}
