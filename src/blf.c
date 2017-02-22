/*******************************************************************************
 *
 * Offline packet processing from Vector BLF file
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

#define MODULE_TAG                      BLF

/*******************************************************************************
 * Includes
 ******************************************************************************/

/* ...support large files */
#define _FILE_OFFSET_BITS 64

#include <zlib.h>

#include "main.h"
#include "common.h"
#include "camera.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(DEBUG, 0);

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/* ...file statistic data */
typedef struct blf_info
{
    /** ...signature 'LLOG' */
    u32             signature;

    /** ...size of statistics data */
    u32             statistics_size;

    /* ...application ID (CANoe) */
    u8              app_id;

    /* ...application major number */
    u8              app_major;

    /* ...application minor number */
    u8              app_minor;

    /* ...application build number */
    u8              app_build;

    /* ...API major number */
    u8              api_major;

    /* ...API minor number */
    u8              api_minor;

    /* ...API build number */
    u8              api_build;

    /* ...API patch number */
    u8              api_patch;

    /* ...file size in bytes */
    u64             file_size;

    /* ...uncompressed file size in bytes */
    u64             uncompressed_size;

    /* ...number of objects */
    u32             object_count;

    /* ...number of objects read */
    u32             objects_read;

    /* ...measurement start time */
    u16             measurement_start[8];

    /* ...last object time */
    u16             last_object_time[8];

    /* ...reserved - padding */
    u32             reserved[18];

}    __attribute__((packed)) blf_info_t;

typedef struct blf_hdr
{
    /* ...object signature */
    u32             signature;

    /* ...size of header in bytes */
    u16             header_size;

    /* ...object header version number */
    u16             header_version;

    /* ...object size in bytes */
    u32             object_size;

    /* ...object type */
    u32             object_type;

}   __attribute__((packed)) blf_hdr_t;

/* ...packet header version 1 */
typedef struct blf_hdr_v1
{
    /* ...base header file */
    blf_hdr_t       base;

    /* ...auxiliary object flags */
    u32             flags;

    /* ...padding data */
    u16             reserved;

    /* ...object version */
    u16             version;

    /* ...timestamp value */
    u64             timestamp;

}   __attribute__((packed)) blf_hdr_v1_t;

/* ...packet header version 2 */
typedef struct blf_hdr_v2
{
    /* ...base header file */
    blf_hdr_t       base;

    /* ...auxiliary object flags */
    u32             flags;

    /* ...timestamp status */
    u8              timestamp_status;

    /* ...padding data */
    u8              reserved;

    /* ...object version */
    u16             version;

    /* ...object timestamp value */
    u64             timestamp;

    /* ...original object timestamp value */
    u64             orig_timestamp;

}   __attribute__((packed)) blf_hdr_v2_t;

/* ...composite packet read from BLF */
typedef union blf_pkt_hdr
{
    blf_hdr_t       base;
    blf_hdr_v1_t    v1;
    blf_hdr_v2_t    v2;

}   blf_pkt_hdr_t;

/* ...conatiner object header */
typedef struct blf_container_hdr
{
    /* ...container object flag */
    u32             object_flags;

    /* ...reserved data */
    u16             reserved;

    /* ...object-specific version */
    u16             object_version;

    /* ...uncompressed size in bytes */
    u64             uncompressed_size;

}   __attribute__((packed)) blf_container_hdr_t;

/* ...binary log file handle */
typedef struct blf
{
    /* ...input file handle */
    FILE                   *f;

    /* ...binary log file info */
    blf_info_t              info;

    /* ...current data pointer (access position within interim buffer) */
    u8                     *data;

    /* ...number of bytes available in an interim buffer */
    u32                     count;

    /* ...internal data buffer for compressed data */
    u8                     *buffer;

    /* ...internal data buffer for decompressed data */
    u8                     *uncompressed;

    /* ...internal compressed buffer size */
    u32                     buffer_size;

    /* ...internal uncompressed data buffer size */
    u32                     uncompressed_size;

    /* ...parsed packet header */
    blf_pkt_hdr_t           pkthdr;

}   blf_t;

/* ...do I need that at all? - tbd */
typedef struct netif_blf_data
{
    /* ...packet capture handle */
    blf_t                      *blf;

    /* ...decoding thread handle */
    pthread_t                   thread;

    /* ...processing callback */
    camera_source_callback_t   *cb;

    /* ...callback client data */
    void                       *cdata;

    /* ...thread termination flag */
    int                         exit;

}   netif_blf_data_t;

/*******************************************************************************
 * Local functions definitions
 ******************************************************************************/

/* ...open binary log file handle */
blf_t * blf_open(const char *filename)
{
    blf_t      *blf;

    /* ...allocate data structure */
    CHK_ERR(blf = malloc(sizeof(*blf)), (errno = ENOMEM, NULL));

    /* ...open data file */
    if ((blf->f = fopen(filename, "rb")) == NULL)
    {
        TRACE(ERROR, _x("failed to open file '%s': %m"), filename);
        goto error;
    }

    /* ...parse header */
    if (fread(&blf->info, sizeof(blf->info), 1, blf->f) != 1)
    {
        TRACE(ERROR, _x("failed to read header: %m"));
        goto error_f;
    }
    else if (blf->info.signature != 0x47474F4C)
    {
        TRACE(ERROR, _x("unrecognized file signature: %08X"), blf->info.signature);
        goto error_f;
    }

    /* ...reset internal buffer access */
    blf->count = 0, blf->data = NULL;

    /* ...calculate buffer size */
    blf->buffer_size = (128 << 10);

    /* ...allocate internal buffer data */
    blf->buffer = malloc(blf->buffer_size);

    if (blf->buffer == NULL)
    {
        TRACE(ERROR, _x("failed to allocate data buffer"));
        errno = ENOMEM;
        goto error_f;
    }
    else if ((blf->uncompressed = malloc((blf->uncompressed_size = (128 + 4) << 10))) == NULL)
    {
        TRACE(ERROR, _x("failed to allocate data buffer"));
        errno = ENOMEM;
        goto error_buffer;
    }

    TRACE(INIT, _b("file '%s' opened"), filename);

    return blf;

error_buffer:
    /* ...destroy internal buffer */
    free(blf->buffer);

error_f:
    /* ...close file descriptor */
    fclose(blf->f);

error:
    /* ...destroy log file data */
    free(blf);
    return NULL;
}

/* ...close binary log */
void blf_close(blf_t *blf)
{
    /* ...close binary log file descriptor */
    fclose(blf->f);

    /* ...destroy internal buffers */
    free(blf->buffer), free(blf->uncompressed);

    /* ...destroy data structure */
    free(blf);
}

/* ...read next object */
void * blf_next(blf_t *blf, blf_pkt_hdr_t **hdr)
{
    u8             *buffer = blf->buffer;
    u8             *uncompressed = blf->uncompressed;
    blf_pkt_hdr_t  *h = &blf->pkthdr;
    u8             *data = blf->data;

packet:
    /* ...check if we can read data from the internal buffer */
    if (blf->count > sizeof(h->base))
    {
        u32     obj_size;

        /* ...get next object from data buffer */
        memcpy(h, data, sizeof(h->base));

        /* ...check header is sane */
        if (h->base.signature != 0x4A424F4C)
        {
            TRACE(ERROR, _b("invalid header signature: %X"), h->base.signature);
            return NULL;
        }

        /* ...check object is valid */
        if (h->base.object_type == 10)
        {
            TRACE(ERROR, _b("nested container log; error"));
            return NULL;
        }

        /* ...get "aligned" object size */
        obj_size = h->base.object_size, obj_size += obj_size & 3;

        /* ...check if we have sufficient data in the buffer */
        if (blf->count >= obj_size)
        {
            /* ...copy remainder of a header (v1/v2) */
            memcpy(&h->base + 1, data + sizeof(h->base), h->base.header_size - sizeof(h->base));

            /* ...we have sufficient data to read a packet */
            blf->data = data + obj_size, blf->count -= obj_size;

            /* ...return packet data pointer */
            return *hdr = h, data + h->base.header_size;
        }
    }

    /* ...move header to the beginning of the buffer */
    (blf->count ? memcpy(uncompressed, data, blf->count) : 0);

    /* ...read object header */
    if (fread(h, sizeof(h->base), 1, blf->f) != 1)
    {
        /* ...no more data; indicate completion */
        return NULL;
    }
    else if (h->base.signature != 0x4A424F4C)
    {
        TRACE(ERROR, _x("unrecognized signature: %X"), h->base.signature);
        return NULL;
    }

    /* ...print size of the packet */
    TRACE(DEBUG, _b("object size: %x bytes (compressed); pos=%lx"), h->base.object_size, ftell(blf->f));

    /* ...read data into static buffer */
    if (h->base.object_size > blf->buffer_size)
    {
        TRACE(ERROR, _b("object too large: %u bytes"), h->base.object_size);
        return NULL;
    }
    else if (fread(buffer, 1, h->base.object_size - sizeof(h->base), blf->f) !=
            h->base.object_size - sizeof(h->base))
    {
        TRACE(ERROR, _x("failed to read data: %m"));
        return NULL;
    }
    else
    {
        /* ...skip padding data */
        fseek(blf->f, h->base.object_size & 0x3, SEEK_CUR);

        TRACE(DEBUG, _b("header size: %u, version: %u, object type: %u"),
                h->base.header_size,
                h->base.header_version,
                h->base.object_type);
    }

    /* ...check if object is a container */
    if (h->base.object_type != 10)
    {
        TRACE(ERROR, _x("unexpected object: %u (log-container expected)"), h->base.object_type);
        return NULL;
    }
    else
    {
        blf_container_hdr_t    *c_hdr = (blf_container_hdr_t *)buffer;

        if (c_hdr->uncompressed_size > blf->uncompressed_size - blf->count)
        {
            TRACE(ERROR, _x("too large chunk: %lu"), c_hdr->uncompressed_size);
            return NULL;
        }
        else
        {
            uLongf      in_size = h->base.object_size - sizeof(h->base) - sizeof(*c_hdr);
            uLongf      out_size = c_hdr->uncompressed_size;
            int         r;

            /* ...adjust data buffer */
            data = uncompressed + blf->count;

            /* ...uncompress log data */
            if ((r = uncompress(data, &out_size, buffer + sizeof(*c_hdr), in_size)) != Z_OK)
            {
                TRACE(ERROR, _x("failed to decompress input data: %d"), r);
                BUG(1, _x("breakpoint"));
                return NULL;
            }
            else
            {
                TRACE(DEBUG, _b("decompressed %u bytes"), (u32)out_size);

                BUG(out_size != c_hdr->uncompressed_size,
                        _x("invalid data: %u != %u"),
                        (u32)out_size,
                        (u32)c_hdr->uncompressed_size);

                blf->count += out_size;
                data = uncompressed;
            }

            /* ...repeat reading of the uncompressed data */
            goto packet;
        }
    }
}

/* ...parse v1 packet header */
u64 blf_v1_hdr_timestamp(blf_hdr_v1_t *hdr)
{
    u64     ts = hdr->timestamp;

    return (hdr->flags == 0x1 ? ts * 10 : ts / 1000);
}

/*******************************************************************************
 * Packet parser
 ******************************************************************************/

/* ...get current timestamp value */
static inline u64 net_offline_wait(u64 ts, u64 *diff)
{
    struct timespec     tp;
    u64                 t0;

    /* ...get current time value */
    clock_gettime(CLOCK_MONOTONIC, &tp);

    /* ...get clock value in microseconds */
    t0 = (u64)tp.tv_sec * 1000000ULL + (tp.tv_nsec / 1000);

    if (0) return t0;

    /* ...wait until timestamp becomes valid */
    if (*diff)
    {
        ts = (u64)(ts + *diff);

        /* ...suspend current thread execution until timestamp gets valid */
        if (ts > t0)    usleep(ts - t0);
    }
    else
    {
        *diff = (s64)(t0 - ts);
    }

    /* ...t0 + *diff = ts */
    return t0;
}

/* ...process ethernet frame */
static inline void __netif_blf_ethernet(netif_blf_data_t *blf, blf_hdr_v1_t *hdr, u8 *data, u64 *diff)
{
    u64     ts = blf_v1_hdr_timestamp(hdr);
    int     i;

    /* ...ignore packet if it is not "receiving" */
    if (*(u16 *)(data + 14) != 0)   return;

    /* ...suspend execution with respect to timestamp value */
    net_offline_wait(ts, diff);

    /* ...disable cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    /* ...compare source address */
    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        TRACE(DEBUG, _b("SA: %02X:%02X:%02X:%02X:%02X:%02X"),
                data[0],
                data[1],
                data[2],
                data[3],
                data[4],
                data[5]);

        /* ...simple packet selection by MAC source-address */
        if (memcmp(data, camera_mac_address[i], 6))   continue;

        /* ...verify packet protocol */
        if (*(u16 *)(data + 16) != 0x88B5)  continue;

        TRACE(DEBUG, _b("packet-%d: %p[%u]"), i, data + 24, *(u16 *)(data + 22));

        /* ...pass packet to camera receiver */
        blf->cb->pdu(blf->cdata, i, data + 32, *(u16 *)(data + 22), ts * 1000);

        break;
    }

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
}

/* ...packets replaying thread */
static void * blf_replay_thread(void *arg)
{
    netif_blf_data_t   *blf = arg;
    u64                 diff;

    /* ...clear difference between current/captured timestamps */
    diff = 0;

    /* ...start packets decoding loop */
    while (!blf->exit)
    {
        blf_pkt_hdr_t  *pkthdr;
        u8             *pdata;

        /* ...read next packet from the file */
        if ((pdata = blf_next(blf->blf, &pkthdr)) == NULL)
        {
            /* ...end of file; break the loop */
            break;
        }

        /* ...process packet */
        switch(pkthdr->base.object_type)
        {
        case 71:
            /* ...ethernet frame received */
            __netif_blf_ethernet(blf, &pkthdr->v1, pdata, &diff);
            break;

        default:
            TRACE(DEBUG, _b("unrecognized packet type: %u"), pkthdr->base.object_type);
        }
    }

    TRACE(INIT, _b("thread terminated"));

    /* ...close file finally */
    blf_close(blf->blf);

    /* ...signal completion (tbd) */
    (!blf->exit ? blf->cb->eos(blf->cdata), blf->exit = 1 : 0);

    return NULL;
}

/* ...open capturing file for replay */
void * blf_replay(char *filename, void *cb, void *cdata)
{
    netif_blf_data_t   *blf;
    pthread_attr_t      attr;
    int                 r;

    /* ...allocate offline network interface data */
    CHK_ERR(blf = malloc(sizeof(*blf)), (errno = ENOMEM, NULL));

    /* ...open capture file */
    if ((blf->blf = blf_open(filename)) == NULL)
    {
        TRACE(ERROR, _x("failed to open capture file '%s': %m"), filename);
        errno = ENOENT;
        goto error;
    }

    /* ...save callback parameters */
    blf->cb = cb, blf->cdata = cdata;

    /* ...mark thread is running */
    blf->exit = 0;

    /* ...initialize thread attributes (detached, 1MB stack) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 1 << 20);

    /* ...create playback thread to asynchronously process buffers consumption */
    r = pthread_create(&blf->thread, &attr, blf_replay_thread, blf);
    pthread_attr_destroy(&attr);
    if (r < 0)
    {
        TRACE(ERROR, _x("failed to create thread: %d"), r);
        errno = ENOMEM;
        goto error_blf;
    }

    return blf;

error_blf:
    /* ...close capture file */
    blf_close(blf->blf);

error:
    /* ...destroy offline interface data structure */
    free(blf);

    return NULL;
}

/* ...playback stop */
void blf_stop(void *arg)
{
    netif_blf_data_t   *blf = arg;
    void               *retval;

    TRACE(1, _b("cancelling thread.."));

    /* ...instruct thread to terminate */
    (!blf->exit ? blf->exit = 1 : 0);

    TRACE(1, _b("joining thread.."));

    /* ...wait for a thread completion if not already */
    pthread_join(blf->thread, &retval);

    /* ...destroy thread handle */
    free(blf);

    TRACE(INIT, _b("blf thread completed with result %d"), (int)(intptr_t)retval);
}
