/*******************************************************************************
 *
 * Network interface support implementation
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

#define MODULE_TAG                      NET

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/version.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include "main.h"
#include "common.h"
#include "netif.h"

/* ...linux-headers fixups */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
#define PACKET_TX_HAS_OFF               19
#define TP_STATUS_TS_RAW_HARDWARE       (1 << 31)

#define ETHTOOL_GET_TS_INFO             0x00000041

/* ...copy of kernel definition for 3.10 */
struct ethtool_ts_info {
    __u32    cmd;
    __u32    so_timestamping;
    __s32    phc_index;
    __u32    tx_types;
    __u32    tx_reserved[3];
    __u32    rx_filters;
    __u32    rx_reserved[3];
};
#endif

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 0);
TRACE_TAG(RX, 0);
TRACE_TAG(TX, 0);
TRACE_TAG(DUMP, 1);

/*******************************************************************************
 * Local data definitions
 ******************************************************************************/

typedef struct netif_stream
{
    /* ...socket descriptor */
    int                     sfd;

    /* ...rx-ring reading index */
    u16                     rx_read_idx, rx_write_idx;

    /* ...tx-ring read/write indices */
    u16                     tx_write_idx, tx_read_idx;

    /* ...ring-buffer length mask (power-of-two - 1) */
    u16                     rx_ring_mask, tx_ring_mask;

    /* ...size of internal packet buffer */
    u32                     bufsize;

    /* ...stream statistics */
    struct tpacket_stats    stats;

    /* ...network buffers (for tx/rx paths) */
    netif_buffer_t         *nbuf[];

}   netif_stream_t;

/* ...size of network stream structure */
#define NETIF_STREAM_SIZE(rx_nr, tx_nr)     \
    (sizeof(netif_stream_t) + ((rx_nr) + (tx_nr)) * sizeof(netif_buffer_t *))

/* ...RX frame accessor */
static inline struct tpacket2_hdr * __nbuf_rx(netif_stream_t *stream, u16 idx)
{
    return stream->nbuf[idx];
}

/* ...TX frame accessor */
static inline struct tpacket2_hdr * __nbuf_tx(netif_stream_t *stream, u16 idx)
{
    return stream->nbuf[(u16)(stream->rx_ring_mask + 1) + idx];
}

/* ...next power-of-two calculation */
#define avb_next_power_of_two(v)    ({ u32 __v = (v); __avb_power_of_two_1(__v - 1); })
#define __avb_power_of_two_1(v)     __avb_power_of_two_2((v) | ((v) >> 1))
#define __avb_power_of_two_2(v)     __avb_power_of_two_3((v) | ((v) >> 2))
#define __avb_power_of_two_3(v)     __avb_power_of_two_4((v) | ((v) >> 4))
#define __avb_power_of_two_4(v)     __avb_power_of_two_5((v) | ((v) >> 8))
#define __avb_power_of_two_5(v)     __avb_power_of_two_6((v) | ((v) >> 16))
#define __avb_power_of_two_6(v)     ((v) + 1)

/* ...check if non-zero value is a power-of-two */
#define avb_is_power_of_two(v)      (((v) & ((v) - 1)) == 0)

/*******************************************************************************
 * Local data definitions
 ******************************************************************************/

/* ...length of the table */
#define __filter_size(filter)       (sizeof((filter)) / sizeof((filter)[0]))

/*******************************************************************************
 * Network filter setting
 ******************************************************************************/

/* ...add socket to a multicast group */
static int netif_add_multicast(int index, int sfd, const u8 *addr)
{
    struct packet_mreq  mreq;

    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = index;
    mreq.mr_type = PACKET_MR_MULTICAST;
    mreq.mr_alen = 6;
    memcpy(mreq.mr_address, addr, 6);

    if (setsockopt(sfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        TRACE(ERROR, _x("failed to add to multicast group: %m"));
    }

    return 0;
}

/* ...stream filter setup */
static inline int netif_filter_setup(int index, int sfd, u8 *da, u8 *sa, u16 proto, u16 vlan)
{
    /* ...tcpdump -dd 'ether dst AA:BB:CC:DD:EE:FF and ether src 00:11:22:33:44:55 and (ether proto 0x6677 or vlan 4095)' */
    struct sock_filter  filter[18], *f = filter;
    struct sock_fprog   fprog;
    u8                  i, length;

    /* ...add filtering by destination address if specified */
    if (da)
    {
        f->code = 0x20, f->jt = 0, f->jf = 0, f->k = 2, f++;
        f->code = 0x15, f->jt = 0, f->jf = -1, f->k = netif_get_u32(da + 2), f++;
        f->code = 0x28, f->jt = 0, f->jf = 0, f->k = 0, f++;
        f->code = 0x15, f->jt = 0, f->jf = -1, f->k = netif_get_u16(da), f++;
    }

    /* ...add filtering by source address if specified */
    if (sa)
    {
        f->code = 0x20, f->jt = 0, f->jf = 0, f->k = 8, f++;
        f->code = 0x15, f->jt = 0, f->jf = -1, f->k = netif_get_u32(sa + 2), f++;
        f->code = 0x28, f->jt = 0, f->jf = 0, f->k = 6, f++;
        f->code = 0x15, f->jt = 0, f->jf = -1, f->k = netif_get_u16(sa), f++;
    }

    /* ...add filtering by proto/vlan */
    if (vlan || proto)
    {
        f->code = 0x28, f->jt = 0, f->jf = 0, f->k = 12, f++;

        if (proto)
        {
            f->code = 0x15, f->jt = (vlan ? 6 : 3), f->jf = 0, f->k = proto, f++;
        }

        f->code = 0x15, f->jt = 0, f->jf = -1, f->k = 0x8100, f++;

        if (vlan)
        {
            f->code = 0x28, f->jt = 0, f->jf = 0, f->k = 14, f++;
            f->code = 0x54, f->jt = 0, f->jf = 0, f->k = 0xFFF, f++;
            f->code = 0x15, f->jt = 0, f->jf = -1, f->k = vlan, f++;
        }

        if (proto)
        {
            f->code = 0x28, f->jt = 0, f->jf = 0, f->k = 16, f++;
            f->code = 0x15, f->jt = 0, f->jf = -1, f->k = proto, f++;
        }
    }

    /* ...return points */
    f->code = 0x06, f->jt = 0, f->jf = 0, f->k = 0xFFFF, f++;
    f->code = 0x06, f->jt = 0, f->jf = 0, f->k = 0, f++;

    /* ...set filter length */
    fprog.len = length = (u8)(f - filter);
    fprog.filter = filter;

    /* ...adjust reject index reference */
    for (f = filter, i = length - 2; i > 0; f++, i--)
    {
        (f->jt == (u8)-1 ? f->jt = i : 0);
        (f->jf == (u8)-1 ? f->jf = i : 0);
        TRACE(0, _b("prog: %04X, %02X, %02X, %08X"), f->code, f->jt, f->jf, f->k);
    }

    /* ...apply filter */
    if (setsockopt(sfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog)) < 0)
    {
        TRACE(ERROR, _x("setsockopt SO_ATTACH_FILTER failed: %m"));
        return -errno;
    }

    /* ...add network interface to multicast group if needed */
    if (da && (da[0] & 1))
    {
        netif_add_multicast(index, sfd, da);
    }

    return 0;
}

/*******************************************************************************
 * Public accessors
 ******************************************************************************/

/* ...pointer to MAC header data */
static inline u8 * __nbuf_ethhdr(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return (u8 *)frame + frame->tp_mac;
}

u8 * nbuf_ethhdr(netif_buffer_t *nbuf)
{
    return __nbuf_ethhdr(nbuf);
}

/* ...pointer to network data (payload) */
static inline u8 * __nbuf_pdu(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return (u8 *)frame + frame->tp_net;
}

u8 * nbuf_pdu(netif_buffer_t *nbuf)
{
    return __nbuf_pdu(nbuf);
}

/* ...total packet length (including MAC header) */
static inline u16 __nbuf_len(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return frame->tp_len;
}

u16 nbuf_len(netif_buffer_t *nbuf)
{
    return __nbuf_len(nbuf);
}

/* ...length of MAC header */
static inline u16 __nbuf_ethhdrlen(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return (u16)(frame->tp_net - frame->tp_mac);
}

u16 nbuf_ethhdrlen(netif_buffer_t *nbuf)
{
    return __nbuf_ethhdrlen(nbuf);
}

/* ...length of MAC header */
static inline void __nbuf_ethhdrlen_set(netif_buffer_t *nbuf, u16 len)
{
    struct tpacket2_hdr    *frame = nbuf;

    frame->tp_net = frame->tp_mac + len;
}

void nbuf_ethhdrlen_set(netif_buffer_t *nbuf, u16 len)
{
    return __nbuf_ethhdrlen_set(nbuf, len);
}

/* ...packet PDU length (excluding MAC header) */
static inline u16 __nbuf_datalen(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return frame->tp_len - (frame->tp_net - frame->tp_mac);
}

u16 nbuf_datalen(netif_buffer_t *nbuf)
{
    return __nbuf_datalen(nbuf);
}

/* ...packet timestamp */
static inline u64 __nbuf_tstamp(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    return frame->tp_sec * 1000000000ULL + frame->tp_nsec;
}

u64 nbuf_tstamp(netif_buffer_t *nbuf)
{
    return __nbuf_tstamp(nbuf);
}

/* ...ethernet header processing */
static inline u16 __nbuf_eth_translate(netif_buffer_t *nbuf, u16 *length)
{
    struct tpacket2_hdr    *frame = nbuf;
    u8                     *pkt = (u8 *)frame + frame->tp_mac;
    u16                     ethtype;

    /* ...check whether the packet is a 802.1Q frame */
    if ((ethtype = nbuf_ethtype(pkt)) == 0x8100)
    {
        ethtype = nbuf_8021q_ethtype(pkt);
        frame->tp_net = frame->tp_mac + 18;
        *length = frame->tp_len - 18;
    }
    else
    {
        frame->tp_net = frame->tp_mac + 14;
        *length = frame->tp_len - 14;
    }

    return ethtype;
}

u16 nbuf_eth_translate(netif_buffer_t *nbuf, u16 *length)
{
    return __nbuf_eth_translate(nbuf, length);
}

/* ...mark buffer is available for subsequent transmission */
static inline void __nbuf_rx_done(netif_buffer_t *nbuf)
{
    struct tpacket2_hdr    *frame = nbuf;

    /* ...mark the frame is available for receiving */
    frame->tp_status = TP_STATUS_KERNEL;
}

/* ...dump network packet (tbd - VLAN-tagged frames? )*/
void netif_nbuf_dump(netif_buffer_t *nbuf, u16 length, const char *tag)
{
    u8                     *pkt;
    char                   *p;
    u16                     i = 0;
    static char             s[32];

    /* ...get pointer to frame data */
    pkt = __nbuf_ethhdr(nbuf);

    /* ...increment length by header size */
    length += __nbuf_ethhdrlen(nbuf);

    /* ...dump all packets */
    while (length > 0)
    {
        u8      k = (u8)(length >= 8 ? 8 : length);

        p = s + sprintf(s, "[%04u]:", i);

        /* ...decrement packet length */
        length -= k, i += k;

        /* ...output string */
        while (k--) p += sprintf(p, "%02x:", *pkt++);

        /* ...put terminator */
        p[-1] = 0;

        TRACE(DUMP, _b("%s-%s"), tag, s);
    }
}

/*******************************************************************************
 * Stream support via PACKET-MMAP
 ******************************************************************************/

/* ...initialize network stream buffers */
static inline int netif_stream_setup(netif_stream_t *stream, u16 rx_nr, u16 tx_nr, u16 f_size)
{
    int                     sfd = stream->sfd;
    u32                     bufsize = 0;
    int                     v;
    struct tpacket_req      req;
    void                   *mm;
    u16                     i;

    /* ...select version-2 of packet socket */
    v = TPACKET_V2;
    SV_CHK_ERR(setsockopt(sfd, SOL_PACKET, PACKET_VERSION, &v, sizeof(v)) == 0, -errno);

    /* ...use offset (tp_mac) for transmitted frames */
    v = 1;
    SV_CHK_ERR(setsockopt(sfd, SOL_PACKET, PACKET_TX_HAS_OFF, &v, sizeof(v)) == 0, -errno);

    /* ...add frame overheads (we do not reserve anything) and scale to next power-of-2 */
    f_size = avb_next_power_of_two(TPACKET_ALIGN(f_size + TPACKET2_HDRLEN));

    /* ...setup transmission path if specified */
    if (tx_nr)
    {
        /* ...we are okay to use a single block */
        req.tp_block_nr = 1;
        req.tp_frame_nr = tx_nr;
        req.tp_frame_size = f_size;
        bufsize += (req.tp_block_size = tx_nr * f_size);

        TRACE(INIT,
              _b("setup tx-buffer: {b:%u, f:%u, fs:%u, bs:%u}"),
              req.tp_block_nr, req.tp_frame_nr, req.tp_frame_size, req.tp_block_size);

        /* ...set socket transmit ring-buffer */
        SV_CHK_ERR(setsockopt(sfd, SOL_PACKET, PACKET_TX_RING, &req, sizeof(req)) == 0, -errno);
    }

    stream->tx_ring_mask = (u16)(tx_nr - 1);

    /* ...setup receive path if specified */
    if (rx_nr)
    {
        /* ...we are okay to use a single block */
        req.tp_block_nr = 1;
        req.tp_frame_nr = rx_nr;
        req.tp_frame_size = f_size;
        bufsize += (req.tp_block_size = rx_nr * f_size);

        TRACE(INIT,
              _b("setup rx-buffer: {b:%u, f:%u, fs:%u, bs:%u}"),
              req.tp_block_nr, req.tp_frame_nr, req.tp_frame_size, req.tp_block_size);

        /* ...set socket receive ring-buffer */
        SV_CHK_ERR(setsockopt(sfd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) == 0, -errno);
    }

    stream->rx_ring_mask = (u16)(rx_nr - 1);

    /* ...make sure buffer size is non-zero */
    SV_CHK_ERR((stream->bufsize = bufsize) != 0, -EINVAL);

    /* ...map buffers */
    SV_CHK_ERR((mm = mmap(0, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, stream->sfd, 0)) != MAP_FAILED, -errno);

    /* ...setup frame headers; RX path first */
    for (i = 0; i < rx_nr; i++, mm += f_size)
    {
        stream->nbuf[i] = mm;
    }

    /* ...for TX frames, specify a custom offset */
    for (; i < rx_nr + tx_nr; i++, mm += f_size)
    {
        struct tpacket2_hdr    *frame = mm;

        /* ...set MAC offset to same value that is used by RX-buffers */
        frame->tp_mac = TPACKET_ALIGN(TPACKET2_HDRLEN);

        /* ...set buffer pointer */
        stream->nbuf[i] = frame;
    }

    TRACE(INIT, _b("net-stream buffers allocated: [%p:%p): tx:%u, rx:%u, size:%u"), stream->nbuf[0], mm, tx_nr, rx_nr, f_size);

    return 0;
}

/* ...create network stream */
netif_stream_t * netif_stream_create(int sfd, u16 rx_nr, u16 tx_nr, u16 f_size)
{
    netif_stream_t     *stream;

    /* ...sanity check - make sure frames number is a power-of-two */
    if (!avb_is_power_of_two(rx_nr) || !avb_is_power_of_two(tx_nr))
    {
        TRACE(ERROR, _x("invalid ring sizes: %u/%u"), rx_nr, tx_nr);
        errno = ERANGE;
        return NULL;
    }

    /* ...allocate memory structure */
    if ((stream = malloc(NETIF_STREAM_SIZE(rx_nr, tx_nr))) == NULL)
    {
        TRACE(ERROR, _x("memory allocation failed"));
        errno = ENOMEM;
        return NULL;
    }

    /* ...reset stream data structure */
    memset(stream, 0, sizeof(*stream));

    /* ...save socket handle */
    stream->sfd = sfd;

    /* ...setup ring-buffer */
    if ((errno = -netif_stream_setup(stream, rx_nr, tx_nr, f_size)) != 0)
    {
        TRACE(ERROR, _x("stream buffer setup failed: %m"));
        goto error;
    }

    /* ...stream allocated successfully */
    return stream;

error:
    /* ...destroy stream data */
    netif_stream_destroy(stream);

    return NULL;
}

/* ...destroy network stream data */
void netif_stream_destroy(netif_stream_t *stream)
{
    /* ...unmap packet rings (doesn't get destroyed upon simple socket closing) */
    munmap(stream->nbuf[0], stream->bufsize);

    /* ...close stream socket descriptor (destroys mmap automatically) */
    close(stream->sfd);

    /* ...deallocate stream memory */
    free(stream);
}

/* ...test if the stream is ready for receiving */
int netif_stream_rx_ready(netif_stream_t *stream)
{
    u16                     read_idx = stream->rx_read_idx;
    struct tpacket2_hdr    *frame = __nbuf_rx(stream, read_idx);

    /* ...report status */
    return (frame->tp_status & TP_STATUS_USER) != 0;
}

/* ...calculate amount of the pending packets available in receive queue */
u16 netif_stream_rx_pending(netif_stream_t *stream)
{
    u16         read_idx = stream->rx_read_idx;
    u16         mask = stream->rx_ring_mask;
    u16         count;

    for (count = 0; count <= mask; count++)
    {
        /* ...check if the packet is available for reading */
        if ((__nbuf_rx(stream, read_idx)->tp_status & TP_STATUS_USER) == 0)
        {
            break;
        }

        read_idx = (read_idx + 1) & mask;
    }

    return count;
}

/* ...wait for new frame reception */
int netif_stream_wait_rx(netif_stream_t *stream)
{
    u16                     read_idx = stream->rx_read_idx;
    struct tpacket2_hdr    *frame = __nbuf_rx(stream, read_idx);

    /* ...wait for a new frame if needed */
    while (frame->tp_status == TP_STATUS_KERNEL)
    {
        struct pollfd   pfd;

        /* ...wait for a packet reception */
        pfd.fd = stream->sfd;
        pfd.revents = 0;
        pfd.events = POLLIN;
        SV_CHK_ERR(poll(&pfd, 1, -1) == 1, -errno);

        /* ...process errors as needed - tbd */
        SV_CHK_ERR((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0, -EPIPE);
    }

    /* ...frame is available for reading */
    return 0;
}

/* ...read next frame */
netif_buffer_t * netif_stream_read(netif_stream_t *stream)
{
    u16                     read_idx = stream->rx_read_idx;
    struct tpacket2_hdr    *frame = __nbuf_rx(stream, read_idx);

    if ((frame->tp_status & TP_STATUS_USER) != 0)
    {
        /* ...check extended statistics if needed */
        if (frame->tp_status & TP_STATUS_COPY)
        {
            /* ...drop such long frame */
            TRACE(WARNING, _x("truncated frame (length=%u)"), frame->tp_len);
        }

        /* ...check for any lost frames at the ring wrap-around point - needed? - tbd */
        if (read_idx == 0 && (frame->tp_status & TP_STATUS_LOSING))
        {
            struct tpacket_stats    stats;
            socklen_t               optlen = sizeof(stats);

            if (getsockopt(stream->sfd, SOL_PACKET, PACKET_STATISTICS, &stats, &optlen) == 0)
            {
                TRACE(WARNING, _x("packets: %u (dropped: %u)"), stats.tp_packets, stats.tp_drops);
            }
        }

        /* ...increment reading index*/
        stream->rx_read_idx = (read_idx + 1) & stream->rx_ring_mask;

        /* ...return frame received */
        return frame;
    }

    /* ...frame is not available yet */
    return NULL;
}

/* ...purge receiving stream queue */
void netif_stream_rx_purge(netif_stream_t *stream)
{
    u16                     read_idx = stream->rx_read_idx;
    u16                     mask = stream->rx_ring_mask;
    struct tpacket2_hdr    *frame;
    struct tpacket_stats    stats;
    socklen_t               optlen = sizeof(stats);
    int                     repeat;

    /* ...two cycles should be enough to reliably clear packet ring */
    for (repeat = 0; repeat < 2; repeat++)
    {
        /* ...clear packets statistics */
        (void)getsockopt(stream->sfd, SOL_PACKET, PACKET_STATISTICS, &stats, &optlen);

        /* ...drop all frames collected thus far */
        while ((frame = __nbuf_rx(stream, read_idx))->tp_status & TP_STATUS_USER)
        {
            /* ...pass buffer ownership to the kernel */
            __nbuf_rx_done(frame);

            /* ...advance frame index */
            read_idx = (read_idx + 1) & mask;
        }
    }

    stream->rx_read_idx = read_idx;
}

/* ...release RX-frame (return to kernel) */
void netif_stream_rx_done(netif_stream_t *stream, netif_buffer_t *nbuf)
{
    __nbuf_rx_done(nbuf);
}

/* ...get next transmission buffer */
netif_buffer_t * netif_stream_get_tx_buffer(netif_stream_t *stream, int wait)
{
    u16                     write_idx = stream->tx_write_idx;
    struct tpacket2_hdr    *frame = __nbuf_tx(stream, write_idx);

    /* ...wait for a new frame if needed */
    while (frame->tp_status != TP_STATUS_AVAILABLE)
    {
        struct pollfd   pfd;

        /* ...if waiting is disabled, bail out */
        if (!wait)
        {
            return NULL;
        }

        /* ...wait for a packet transmission */
        pfd.fd = stream->sfd;
        pfd.revents = 0;
        pfd.events = POLLOUT;
        SV_CHK_ERR(poll(&pfd, 1, -1) == 1, NULL);

        /* ...check for transmission errors as needed */
        SV_CHK_ERR((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0, NULL);
    }

    /* ...advance buffer pointer */
    stream->tx_write_idx = (write_idx + 1) & stream->tx_ring_mask;

    /* ...return buffer pointer (any special setup? - tbd) */
    return (netif_buffer_t *)frame;
}

/* ...write next frame to the stream socket */
int netif_stream_write(netif_stream_t *stream, netif_buffer_t *nbuf, u16 length, int commit)
{
    struct tpacket2_hdr    *frame = nbuf;

    /* ...add MAC header length */
    length += __nbuf_ethhdrlen(nbuf);

    /* ...save total length of data to transmit */
    frame->tp_len = length;

    /* ...pass ownership to kernel */
    frame->tp_status = TP_STATUS_SEND_REQUEST;

    /* ...commit transmission if requested */
    (commit ? SV_CHK_ERR(send(stream->sfd, NULL, 0, MSG_DONTWAIT) >= 0, -errno) : 0);

    return 0;
}

/* ...check if packet is transmitted */
static inline netif_buffer_t * netif_stream_written(netif_stream_t *stream)
{
    u16                     read_idx = stream->tx_read_idx;
    struct tpacket2_hdr    *frame = __nbuf_tx(stream, read_idx);

    if (frame->tp_status & TP_STATUS_TS_RAW_HARDWARE)
    {
        /* ...increment reading index */
        stream->tx_read_idx = (read_idx + 1) & stream->tx_ring_mask;

        /* ...and return a ransmitted frame (should I do ethtype tranlsation here? - tbd) */
        return frame;
    }

    /* ...frame is not available yet */
    return NULL;
}

/* ...get a network buffer with specified index */
netif_buffer_t * netif_stream_tx_buffer(netif_stream_t *stream, u16 idx)
{
    return (netif_buffer_t *)__nbuf_tx(stream, idx);
}

/*******************************************************************************
 * Internal helpers
 ******************************************************************************/

static inline int netif_stream_bind(int index, int sfd)
{
    struct sockaddr_ll      addr;

    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = index;

    /* ...bind socket */
    SV_CHK_ERR(bind(sfd, (struct sockaddr *) &addr, sizeof(addr)) == 0, -errno);

    return 0;
}

/* ...return associated file descriptor suitable for poll/select */
int netif_stream_fd(netif_stream_t *stream)
{
    return stream->sfd;
}

/*******************************************************************************
 * Data stream interface
 ******************************************************************************/

/* ...open streaming network interface */
netif_stream_t * netif_data_stream_create(netif_data_t *netif, netif_filter_t *filter, u16 rx_nr, u16 tx_nr, u16 f_size)
{
    int index = netif->index;
    int                 sfd;
    netif_stream_t     *stream;

    /* ...open raw socket */
    if ((sfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0)
    {
        TRACE(ERROR, _x("socket creation failed: %m"));
        return NULL;
    }

    /* ...set socket filter to retrieve only stream-related AVTP frames */
    if (filter && netif_filter_setup(index, sfd, filter->da, filter->sa, filter->proto, filter->vlan) < 0)
    {
        TRACE(ERROR, _x("failed to setup filter: %m"));
        goto error_sfd;
    }

    /* ...create network stream data */
    if ((stream = netif_stream_create(sfd, rx_nr, tx_nr, f_size)) == NULL)
    {
        TRACE(ERROR, _x("stream creation failed: %m"));
        goto error_sfd;
    }

    /* ...bind socket to selected network interface */
    if ((errno = -netif_stream_bind(index, sfd)) != 0)
    {
        TRACE(ERROR, _x("stream binding failed: %m"));
        goto error_stream;
    }

    TRACE(INIT, _b("data-stream [%p] created"), stream);

    return stream;

error_stream:
    /* ...destroy stream */
    netif_stream_destroy(stream);

    return NULL;

error_sfd:
    /* ...close socket handle */
    close(sfd);

    return NULL;
}

/*******************************************************************************
 * Network interface module initialization
 ******************************************************************************/

/* ...open specified network interface */
int netif_init(netif_data_t *netif, const char *name)
{
    int                     index;
    int                     sfd;
    struct ifreq            ifreq;

    /* ...open raw socket */
    if ((sfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0)
    {
        TRACE(ERROR, _x("socket creation failed: %m"));
        return -errno;
    }

    /* ...retrieve network interface index */
    memset(&ifreq, 0, sizeof(ifreq));
    strncpy(ifreq.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sfd, SIOCGIFINDEX, &ifreq) < 0)
    {
        TRACE(ERROR, _x("ioctl SIOCGIFINDEX failed: %m"));
        goto error_sfd;
    }
    else
    {
        netif->index = index = ifreq.ifr_ifindex;
    }

    /* ...get the mac address of the network interface */
    memset(&ifreq, 0, sizeof(ifreq));
    strncpy(ifreq.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sfd, SIOCGIFHWADDR, &ifreq) < 0)
    {
        TRACE(ERROR, _x("ioctl SIOCGIFHWADDR failed: %m"));
        goto error_sfd;
    }
    else
    {
        memcpy(netif->mac, (u8 *)ifreq.ifr_hwaddr.sa_data, 6);
    }

    /* ...close socket handle */
    close(sfd);

    TRACE(INIT, _b("Network interface '%s' successfully opened"), name);

    return 0;

error_sfd:
    /* ...close socket handle */
    close(sfd);

    return -errno;
}

/*******************************************************************************
 * Network source
 ******************************************************************************/

/* ...data-source handle */
typedef struct netif_source
{
    /* ...generic source handle */
    GSource             source;

    /* ...network stream data */
    netif_stream_t     *stream;

    /* ...polling object tag */
    gpointer            tag;

}   netif_source_t;

/* ...prepare handle */
static gboolean netif_source_prepare(GSource *source, gint *timeout)
{
    netif_source_t     *nsrc = (netif_source_t *)source;

    if (nsrc->tag && netif_stream_rx_ready(nsrc->stream))
    {
        TRACE(0, _b("camera source: %p - prepare - ready"), source);

        /* ...there is a buffer available for reading */
        return TRUE;
    }
    else
    {
        TRACE(0, _b("camera source: %p - prepare - nothing"), source);

        /* ...no buffer available; wait indefinitely */
        *timeout = -1;
        return FALSE;
    }
}

/* ...check function called after polling returns */
static gboolean netif_source_check(GSource *source)
{
    netif_source_t     *nsrc = (netif_source_t *) source;

    TRACE(0, _b("camera source: %p - check"), source);

    /* ...check if there is input data already */
    return (nsrc->tag && netif_stream_rx_ready(nsrc->stream));
}

/* ...dispatch function */
static gboolean netif_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    netif_source_t     *nsrc = (netif_source_t *) source;

    TRACE(0, _b("camera source: %p - dispatch"), source);

    /* ...call dispatch function (if source has been removed, still return TRUE) */
    return (nsrc->tag ? callback(user_data) : TRUE);
}

/* ...finalization function */
static void netif_source_finalize(GSource *source)
{
    TRACE(DEBUG, _b("network source destroyed"));
}

/* ...source callbacks */
static GSourceFuncs netif_source_funcs = {
    .prepare = netif_source_prepare,
    .check = netif_source_check,
    .dispatch = netif_source_dispatch,
    .finalize = netif_source_finalize,
};

/*******************************************************************************
 * Public API
 ******************************************************************************/

/* ...create network stream source */
netif_source_t * netif_source_create(netif_stream_t *stream, gint prio, GSourceFunc func, gpointer user_data, GDestroyNotify notify)
{
    netif_source_t     *nsrc;
    GSource            *source;

    /* ...allocate source handle */
    SV_CHK_ERR(source = g_source_new(&netif_source_funcs, sizeof(*nsrc)), NULL);

    /* ...set network stream pointer */
    (nsrc = (netif_source_t *)source)->stream = stream;

    /* ...add stream file handle - here? - no, postpone until explicit resume command */
    nsrc->tag = NULL;

    /* ...set priority */
    g_source_set_priority(source, prio);

    /* ...set callback function */
    g_source_set_callback(source, func, user_data, notify);

    /* ...attach source to the default thread context */
    g_source_attach(source, g_main_context_get_thread_default());

    /* ...pass ownership to the loop */
    g_source_unref(source);

    return nsrc;
}

/* ...suspend network source */
void netif_source_suspend(netif_source_t *nsrc)
{
    GSource    *source = (GSource *)nsrc;

    if (nsrc->tag)
    {
        g_source_remove_unix_fd(source, nsrc->tag);
        nsrc->tag = NULL;
        TRACE(DEBUG, _b("net-source [%p] suspended"), nsrc);
    }
}

/* ...resume network source */
void netif_source_resume(netif_source_t *nsrc, int purge)
{
    GSource    *source = (GSource *)nsrc;

    if (!nsrc->tag)
    {
        /* ...purge stream content if needed */
        (purge ? netif_stream_rx_purge(nsrc->stream) : 0);
        nsrc->tag = g_source_add_unix_fd(source, netif_stream_fd(nsrc->stream), G_IO_IN | G_IO_ERR);
        TRACE(DEBUG, _b("net-source[%p] resumed (fd=%d)"), nsrc, netif_stream_fd(nsrc->stream));
    }
}

/* ...check if data source is active */
int netif_source_is_active(netif_source_t *nsrc)
{
    return (nsrc->tag != NULL);
}

/* ...destroy network source */
void netif_source_destroy(netif_source_t *nsrc)
{
    /* ...remove source from default thread context */
    g_source_destroy((GSource *)nsrc);
}

