/*******************************************************************************
 *
 * MJPEG camera implementation
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

#define MODULE_TAG                      CAMERA

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <gst/app/gstappsrc.h>

#include <netinet/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <sv/svlib.h>
#include <arpa/inet.h>
#include "main.h"
#include "common.h"
#include "netif.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 0);
TRACE_TAG(PROCESS, 0);

/*******************************************************************************
 * Capturing configuration
 ******************************************************************************/

CAPTURE_TAG(PROC_0, u32, 1);
CAPTURE_TAG(PROC_1, u32, 1);
CAPTURE_TAG(PROC_2, u32, 1);
CAPTURE_TAG(PROC_3, u32, 1);

CAPTURE_TAG(AVAIL_0, u16, 1);
CAPTURE_TAG(AVAIL_1, u16, 1);
CAPTURE_TAG(AVAIL_2, u16, 1);
CAPTURE_TAG(AVAIL_3, u16, 1);

CAPTURE_TAG(PRODUCED_0, u16, 1);
CAPTURE_TAG(PRODUCED_1, u16, 1);
CAPTURE_TAG(PRODUCED_2, u16, 1);
CAPTURE_TAG(PRODUCED_3, u16, 1);

CAPTURE_TAG(BACKLOG_0, u32, 1);
CAPTURE_TAG(BACKLOG_1, u32, 1);
CAPTURE_TAG(BACKLOG_2, u32, 1);
CAPTURE_TAG(BACKLOG_3, u32, 1);

PM_TAG(RX_0, 1);
PM_TAG(RX_1, 1);
PM_TAG(RX_2, 1);
PM_TAG(RX_3, 1);

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

/* ...camera data */
typedef struct camera_data
{
    /* ...GStreamer appsrc node */
    GstAppSrc                  *appsrc;

    /* ...network stream */
    netif_stream_t             *net;

    /* ...GStreamer data source id */
    netif_source_t             *source_id;

    /* ...receiver sequence number */
    u8                          sequence_num;

    /* ...receiver flags */
    u8                          flags;

    /* ...camera id (for profiling purposes) */
    u8                          id;

    /* ...currently accessed buffer */
    GstBuffer                  *buffer;

    /* ...current buffer mapping */
    GstMapInfo                  map;

    /* ...current writing pointer in the frame buffer */
    void                       *input;

    /* ...number of remaining bytes in a buffer */
    u32                         remaining;

    /* ...buffer retrieval function */
    GstBuffer *               (*get_buffer)(void *, int);

    /* ...custom data for buffer retrieval */
    void                       *cdata;

}   camera_data_t;

/*******************************************************************************
 * Camera receiver flags
 ******************************************************************************/

/* ...camera receiver flags */
#define CAMERA_FLAG_INIT_DONE           (1 << 0)
#define CAMERA_FLAG_SYNC                (1 << 1)

/* ...camera receiver events */
#define CAMERA_EVENT_AVBTP_DISC         (1 << 4)
#define CAMERA_EVENT_MASK               (0xF << 4)

/*******************************************************************************
 * JPEG stream parsing (SOI/EOI only)
 ******************************************************************************/

/* ...parse PDU carrying MJPEG video */
static inline int camera_jpeg_parse(camera_data_t *camera, u16 ph, u64 ts, u8 *data, u16 length)
{
    u32         flags = camera->flags;
    GstBuffer  *buffer;
    u32         remaining;
    void       *input;

    /* ...frame must be over 2 bytes in size */
    CHK_ERR(length > 2, 0);

    /* ...check if camera is initialized */
    if (flags & CAMERA_FLAG_INIT_DONE)
    {
        /* ...check if we have a discontinuity event */
        if (flags & CAMERA_EVENT_AVBTP_DISC)
        {
            TRACE(PROCESS, _b("camera-%u: discontinuity detected"), camera->id);

            /* ...clear discontinuity flag and restart synchronization sequence */
            flags = (flags ^ CAMERA_EVENT_AVBTP_DISC) | CAMERA_FLAG_SYNC;
        }
    }
    else
    {
        /* ...mark initialization is done; start searching for a frame */
        flags = CAMERA_FLAG_INIT_DONE | CAMERA_FLAG_SYNC;
    }

    /* ...check for a SOI tag as needed */
    if (flags & CAMERA_FLAG_SYNC)
    {
        /* ...drop the frame if it doesn't start with a SOI tag */
        if (netif_get_u16(data) != 0xFFD8)
        {
            TRACE(DEBUG, _b("camera-%u: no SOI tag; drop frame"), camera->id);
            goto drop;
        }

        /* ...try to get a buffer from pool */
        if ((buffer = camera->buffer) == NULL)
        {
            if ((buffer = camera->get_buffer(camera->cdata, camera->id)) == NULL)
            {
                TRACE(PROCESS, _b("camera-%u: no buffer available; drop frame"), camera->id);
                goto drop;
            }
            else
            {
                /* ...prepare buffer for filling */
                gst_buffer_map(buffer, &camera->map, GST_MAP_WRITE);
                camera->buffer = buffer;

                /* ...put current packet timestamp */
                GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer) = ts;
            }
        }

        TRACE(DEBUG, _b("camera-%u: SOI tag found"), camera->id);

        /* ...buffer is available; start collecting a frame */
        flags ^= CAMERA_FLAG_SYNC;

        /* ...set remaining frame length */
        remaining = camera->map.size, input = camera->map.data;
    }
    else
    {
        /* ...parser is in a middle of frame collection; get remaining buffer length */
        remaining = camera->remaining, input = camera->input;
    }

    TRACE(DEBUG, _b("camera-%u: frame [ph=%X]: %u bytes (remaining = %u)"),
            camera->id, ph, length, remaining);

    /* ...copy PDU data into buffer if there is a place */
    if (length > remaining)
    {
        TRACE(PROCESS, _b("camera-%u: frame is too long (> %lu bytes)"),
                camera->id, camera->map.size);

        /* ...restart searching for a new frame */
        flags ^= CAMERA_FLAG_SYNC;

        /* ...do not discard buffer */
        goto drop;
    }
    else
    {
        /* ...copy chunk of data into buffer */
        memcpy(input, data, length);

        /* ...increase packet length */
        input += length, remaining -= length;
    }

    /* ...check for a EOI tag at the end of frame (may be misaligned) */
    if (netif_get_u16(input - 2) == 0xFFD9 || (netif_get_u16(input - 3) == 0xFFD9 ? --input, 1 : 0))
    {

        GstBuffer  *buffer = camera->buffer;
        u32         n = (u32)(input - (void *)camera->map.data);

        TRACE(PROCESS, _b("camera-%u: frame received (%u bytes)"), camera->id, n);

        /* ...EOI found; complete a frame (any metadata? - tbd) */
        camera->map.size = n;
        gst_buffer_unmap(buffer, &camera->map);
        gst_buffer_set_size(buffer, n);

        if (GST_ELEMENT_CLOCK(GST_ELEMENT(camera->appsrc)) == 0)
        {
            TRACE(ERROR, _x("no clock for a component given yet!!!"));
        }

        TRACE(0, _b("camera-%u: frame received (%u bytes)"), camera->id, n);

        /* ...pass buffer downstream (timestamp added automatically) */
        gst_app_src_push_buffer(camera->appsrc, buffer);

        /* ...request retrieval of next buffer */
        camera->buffer = NULL;

        /* ...reset flags */
        camera->flags = flags ^ CAMERA_FLAG_SYNC;

        /* ...return a marker indicating frame is collected */
        return 0;
    }
    else
    {
        /* ...save adjusted remaining length */
        camera->remaining = remaining;
        camera->input = input;
    }

drop:
    /* ...save actual flags */
    camera->flags = flags;

    /* ...return a marker indicating frame is not available */
    return 1;
}

/*******************************************************************************
 * Receiver function
 ******************************************************************************/

/* ...PDU processing */
static inline int camera_pdu_rx(camera_data_t *camera, u8 *pdu, u16 length, u64 tstamp)
{
    u8      sequence_num = pdu_get_sequence_number(pdu);
    u32     ts = pdu_get_timestamp(pdu);
    u16     datalen = pdu_get_stream_data_length(pdu);
    u16     ph = pdu_get_protocol_header(pdu);

    /* ...make sure packet is of proper format */
    CHK_ERR(pdu_get_subtype(pdu) == 0x2, -EPROTO);

    /* ...check data-length is sane */
    CHK_ERR(length >= datalen + NETIF_HEADER_LENGTH, -EPROTO);

    /* ...check packet discontinuity */
    if (sequence_num != camera->sequence_num)
    {
        if (camera->flags & CAMERA_FLAG_INIT_DONE)
        {
            TRACE(PROCESS, _b("camera[%p]: disc: %02X != %02X"),
                    camera, sequence_num, camera->sequence_num);

            camera->flags |= CAMERA_EVENT_AVBTP_DISC;
        }
    }

    /* ...prepare timestamp basing on local/remote values - tbd */
    C_UNUSED(ts);

    /* ...pass payload to receiver */
    CHK_API(camera_jpeg_parse(camera, ph, tstamp, get_pdu(pdu), datalen));

    /* ...advance sequence number */
    camera->sequence_num = (u8)(sequence_num + 1);

    return 0;
}

/*******************************************************************************
 * AppSrc interface implementation
 ******************************************************************************/

/* ...packets reading callback (main loop source handler) */
static gboolean camera_appsrc_read_data(void *arg)
{
    camera_data_t      *camera = arg;
    netif_stream_t     *stream = camera->net;
    netif_buffer_t     *nbuf;
    u32                num_in = netif_stream_rx_pending(stream);
    u32                num_done = 0;
    u32                cycles = get_cpu_cycles();
    u32                backlog = gst_app_src_get_current_level_bytes(camera->appsrc);

    /* ...add profiling data */
    switch (camera->id)
    {
    case 0:
        CAPTURE(PROC_0, cycles);
        CAPTURE(AVAIL_0, num_in);
        CAPTURE(BACKLOG_0, backlog);
        PM(RX_0, 0);
        break;
    case 1:
        CAPTURE(PROC_1, cycles);
        CAPTURE(AVAIL_1, num_in);
        CAPTURE(BACKLOG_1, backlog);
        PM(RX_1, 0);
        break;
    case 2:
        CAPTURE(PROC_2, cycles);
        CAPTURE(AVAIL_2, num_in);
        CAPTURE(BACKLOG_2, backlog);PM(RX_2, 0);
        break;
    default:
        CAPTURE(PROC_3, cycles);
        CAPTURE(AVAIL_3, num_in);
        CAPTURE(BACKLOG_3, backlog);
        PM(RX_3, 0);
    }

    TRACE(0, _b("process input data - %p"), camera);

    /* ...read all available packets */
    while ((nbuf = netif_stream_read(stream)) != NULL)
    {
        u16     proto, length;

        /* ...get protocol id */
        proto = nbuf_eth_translate(nbuf, &length);

        if (proto != 0x88B5)
        {
            TRACE(ERROR, _x("unrecognized proto: %04X"), proto);
            goto release;
        }

        /* ...basic packet sanity check */
        if (length < NETIF_HEADER_LENGTH)
        {
            TRACE(ERROR, _x("invalid packet length: %u"), length);
            goto release;
        }

        /* ...pass control to AVBTP receiver (don't die on errors?) */
        (void) camera_pdu_rx(camera, nbuf_pdu(nbuf), length, nbuf_tstamp(nbuf));

    release:
        /* ...release buffer */
        netif_stream_rx_done(stream, nbuf);

        /* ...increase number of frames processed */
        if (SV_CAPTURE) num_done++;

        /* ...if application doesn't need data, move out */
        if (!netif_source_is_active(camera->source_id))     break;
    }

    TRACE(0, _b("done reading... - %p, packets: %u"), camera, num_done);

    /* ...add profiling data */
    switch (camera->id)
    {
    case 0:     CAPTURE(PRODUCED_0, num_done); PM(RX_0, 1);  break;
    case 1:     CAPTURE(PRODUCED_1, num_done); PM(RX_1, 1);  break;
    case 2:     CAPTURE(PRODUCED_2, num_done); PM(RX_2, 1);  break;
    default:    CAPTURE(PRODUCED_3, num_done); PM(RX_3, 1);
    }

    return TRUE;
}

/* ...offline operation mode packet processing callback */
void camera_packet_receive(camera_data_t *camera, u8 *pdu, u16 length, u64 ts)
{
    /* ...pass control to AVBTP receiver (don't die on errors?) */
    (void) camera_pdu_rx(camera, pdu, length, ts);
}

/* ...submit another buffer to the gst pipeline */
static void camera_appsrc_need_data(GstAppSrc *src, guint length, gpointer user_data)
{
    camera_data_t  *camera = user_data;

    /* ...resume network stream as needed */
    if (camera->source_id && !netif_source_is_active(camera->source_id))
    {
        TRACE(DEBUG, _b("application requests more data (%u bytes)"), length);
        netif_source_resume(camera->source_id, 0);
    }

    TRACE(0, _b("application requests more data (%u bytes)"), length);
}

/* ...pipeline buffer fullness indication */
static void camera_appsrc_enough_data(GstAppSrc *src, gpointer user_data)
{
    camera_data_t  *camera = user_data;

    /* ...suspend network stream as needed */
    if (camera->source_id && netif_source_is_active(camera->source_id))
    {
        TRACE(DEBUG, _b("application %p doesn't want to receive data"), camera);
        netif_source_suspend(camera->source_id);
    }

    TRACE(0, _b("application %p doesn't want to receive data"), camera);
}

/* ...app-src callbacks for live capturing */
static GstAppSrcCallbacks camera_appsrc_callbacks =
{
    .need_data = camera_appsrc_need_data,
    .enough_data = camera_appsrc_enough_data,
};

/* ...camera destructor */
static void camera_destroy(gpointer data, GObject *obj)
{
    camera_data_t  *camera = data;
    u8              id = camera->id;

    /* ...close network stream - tbd */
    (camera->net ? netif_stream_destroy(camera->net) : 0);

    /* ...destroy network source */
    (camera->source_id ? netif_source_destroy(camera->source_id) : 0);

    /* ...destroy camera handle */
    free(camera);

    TRACE(INIT, _b("camera-%u destroyed"), id);
}

/*******************************************************************************
 * External API
 ******************************************************************************/

/* ...retrieve gst-element representing camera */
GstElement * mjpeg_camera_gst_element(camera_data_t *camera)
{
    return (GstElement *)camera->appsrc;
}

/* ...start/stop streaming process */
int camera_streaming_enable(camera_data_t *camera, int enable)
{
    if (enable)
    {
        /* ...make sure streaming is not started */
        CHK_ERR(netif_source_is_active(camera->source_id), -EPERM);

        /* ...enable data source (purge content of the stream) */
        netif_source_resume(camera->source_id, 1);

        TRACE(INFO, _b("camera-%u: streaming started"), camera->id);
    }
    else
    {
        /* ...make sure streaming is active */
        CHK_ERR(!netif_source_is_active(camera->source_id), -EPERM);

        /* ...stop streaming source */
        netif_source_suspend(camera->source_id);

        TRACE(INFO, _b("camera-%u: streaming stopped"), camera->id);
    }

    return 0;
}

/* ...MJPEG camera initialization function */
camera_data_t * __camera_mjpeg_create(netif_data_t *netif,
        int id, u8 *da, u8 *sa, u16 vlan,
        GstBuffer * (*get_buffer)(void *, int), void *cdata)
{
    netif_filter_t  filter = { .da = da, .sa = sa, .proto = 0x88b5, .vlan = vlan };
    camera_data_t  *camera;
    char            name[32];

    /* ...allocate camera handle */
    CHK_ERR(camera = malloc(sizeof(*camera)), (errno = ENOMEM, NULL));

    /* ...set node name */
    sprintf(name, "camera-" __tf_mac, __tp_mac(sa));

    /* ...save camera identifier */
    camera->id = id;

    /* ...create application source item */
    if ((camera->appsrc = (GstAppSrc *)gst_element_factory_make("appsrc", name)) == NULL)
    {
        TRACE(ERROR, _x("element creation failed"));
        errno = ENOMEM;
        goto error;
    }
    else
    {
        GstCaps    *caps;

        /* ...set fixed node capabilities */
        caps = gst_caps_new_simple("image/jpeg", NULL, NULL);
        gst_app_src_set_caps(camera->appsrc, caps);
        gst_caps_unref(caps);

        /* ...specify stream has undefined size (no seeking is supported) */
        gst_app_src_set_size(camera->appsrc, -1);
        gst_app_src_set_stream_type(camera->appsrc, GST_APP_STREAM_TYPE_STREAM);

        /* ...do not limit maximal number of bytes in a queue */
        gst_app_src_set_max_bytes(camera->appsrc, 0);

        /* ...add timestamps to the output buffers */
        g_object_set(G_OBJECT(camera->appsrc),
                "format", GST_FORMAT_TIME, "do-timestamp", FALSE, NULL);
    }

    /* ...bind control interface */
    gst_app_src_set_callbacks(camera->appsrc, &camera_appsrc_callbacks, camera, NULL);

    /* ...set custom destructor */
    g_object_weak_ref(G_OBJECT(camera->appsrc), camera_destroy, camera);

    /* ...reset application flags */
    camera->flags = 0;

    /* ...mark we have no buffer yet */
    camera->buffer = NULL;

    /* ...set buffer accessor callback */
    camera->get_buffer = get_buffer, camera->cdata = cdata;

    /* ...open network interface in case of live-capturing mode */
    if (netif != NULL)
    {
        /* ...setup network stream for receiving (we expect unicast streams) */
        camera->net = netif_data_stream_create(netif, &filter, 64, 0, NETIF_MTU_SIZE);

        if (camera->net == NULL)
        {
            TRACE(ERROR, _x("failed to create network stream: %m"));
            goto error_appsrc;
        }

        /* ...initialize data source */
        camera->source_id = netif_source_create(camera->net,
                G_PRIORITY_HIGH, camera_appsrc_read_data, camera, NULL);

        if (camera->source_id == NULL)
        {
            TRACE(ERROR, _x("failed to create data source: %m"));
            goto error_appsrc;
        }
    }
    else
    {
        /* ...clear camera network interface */
        camera->net = NULL, camera->source_id = NULL;
    }

    TRACE(INIT, _b("camera sa:" __tf_mac " interface initialized"), __tp_mac(sa));

    return camera;

error_appsrc:
    /* ...destroy appsrc node */
    gst_object_unref(camera->appsrc);
    return NULL;

error:
    /* ...destroy camera handle */
    free(camera);
    return NULL;
}
