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

#ifndef SV_SURROUNDVIEW_JPU_H
#define SV_SURROUNDVIEW_JPU_H

/*******************************************************************************
 * Custom buffer metadata
 ******************************************************************************/

/* ...metadata structure */
typedef struct jpu_meta
{
    GstMeta             meta;

    /* ...user-specific private data */
    void               *priv;

    /* ...buffer dimensions */
    int                 width, height;

}   jpu_meta_t;

/* ...metadata API type accessor */
extern GType jpu_meta_api_get_type(void);
#define JPU_META_API_TYPE               (jpu_meta_api_get_type())

/* ...metadata information handle accessor */
extern const GstMetaInfo *jpu_meta_get_info(void);
#define JPU_META_INFO                   (jpu_meta_get_info())

/* ...get access to the buffer metadata */
#define gst_buffer_get_jpu_meta(b)      \
    ((jpu_meta_t *)gst_buffer_get_meta((b), JPU_META_API_TYPE))

/* ...attach metadata to the buffer */
#define gst_buffer_add_jpu_meta(b)    \
    ((jpu_meta_t *)gst_buffer_add_meta((b), JPU_META_INFO, NULL))

/*******************************************************************************
 * Types definitions
 ******************************************************************************/

/* ...JPU decoder type declarations */
typedef struct jpu_data     jpu_data_t;

/* ...JPU buffer definition */
typedef struct jpu_buffer
{
    /* ...private data associated with buffer */
    void               *priv;

    /* ...buffer mapping */
    int                 map;

    /* ...buffer memory descriptor */
    union
    {
        /* ...DMA file-descriptors (for output YUV buffers) */
        struct
        {
            /* ...memory offsets of the planes */
            u32                 mem_offset[2];

            /* ...DMA descriptors of the planes buffers (exported) */
            int                 dmafd[2];

            /* ...pointers to the planes buffers */
            void               *planebuf[2];
        };

        /* ...memory-mapped address (for input JPEG buffers) */
        struct
        {
            /* ...plane offset to pass to V4L2 */
            u32                 offset;

            /* ...memory mapped pointer */
            void               *data;

            /* ...length of buffer */
            u32                 length;
        };
    }   m;

}   jpu_buffer_t;

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...module initialization */
extern jpu_data_t * jpu_init(const char *devname);
extern void jpu_destroy(jpu_data_t *jpu);

/* ...pollable file descriptor retrieval */
extern int jpu_capture_fd(jpu_data_t *jpu);

/* ...JPU format specification */
extern int jpu_set_formats(jpu_data_t *jpu,
                           int width,
                           int height,
                           int max_in_size);

/* ...allocate input/output buffers pool */
extern int jpu_allocate_buffers(jpu_data_t *jpu,
                                int capture,
                                jpu_buffer_t *pool,
                                u8 num);

/* ...destroy input/output buffers pool */
extern int jpu_destroy_buffers(jpu_data_t *jpu,
                               int capture,
                               jpu_buffer_t *pool,
                               u8 num);

/* ...input/output buffers processing */
extern int jpu_input_buffer_queue(jpu_data_t *jpu,
                                  int i,
                                  jpu_buffer_t *pool);

extern int jpu_input_buffer_dequeue(jpu_data_t *jpu);

extern int jpu_output_buffer_queue(jpu_data_t *jpu,
                                   int i,
                                   jpu_buffer_t *pool);

extern int jpu_output_buffer_dequeue(jpu_data_t *jpu);

#endif  /* SV_SURROUNDVIEW_JPU_H */
