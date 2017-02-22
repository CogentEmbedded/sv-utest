/*******************************************************************************
 *
 * Network interface support header
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

#ifndef SV_SURROUNDVIEW_NETIF_H
#define SV_SURROUNDVIEW_NETIF_H

#include <string.h>

/*******************************************************************************
 * Global constants definitions
 ******************************************************************************/

#define ETH_TYPE_AVTP           0x22F0
#define ETH_TYPE_MSRP           0x22EA
#define ETH_TYPE_MVRP           0x88F5
#define ETH_TYPE_MMRP           0x88F6
#define ETH_TYPE_GPTP           0x88F7
#define ETH_TYPE_AVBTP          0x88B5

/*******************************************************************************
 * Typedefs
 ******************************************************************************/

typedef struct netif_data netif_data_t;
typedef struct netif_stream netif_stream_t;

/* ...network interface data */
struct netif_data
{
    /* ...network stream */
    netif_stream_t *stream;

    /* ...network interface index */
    int index;

    /* ...local interface MAC address */
    u8 mac[6];
};

/* ...network filter */
typedef struct netif_filter
{
    u8 *sa;
    u8 *da;
    u16 proto;
    u16 vlan;

} netif_filter_t;

typedef struct netif_source     netif_source_t;
typedef struct netif_stream     netif_stream_t;

/* ...network source creation */
netif_source_t * netif_source_create(netif_stream_t *stream, gint prio, GSourceFunc func, gpointer user_data, GDestroyNotify notify);
void netif_source_suspend(netif_source_t *nsrc);
void netif_source_resume(netif_source_t *nsrc, int purge);
int netif_source_is_active(netif_source_t *nsrc);
void netif_source_destroy(netif_source_t *nsrc);


/*******************************************************************************
 * Network buffer accessors
 ******************************************************************************/

static inline u8 netif_get_u8(u8 *pdu)
{
    return pdu[0];
}

static inline void netif_set_u8(u8 *pdu, u8 v)
{
    pdu[0] = v;
}

static inline u16 netif_get_u16(u8 *pdu)
{
    return (u16) ((pdu[0] << 8) | pdu[1]);
}

static inline void netif_set_u16(u8 *pdu, u16 v)
{
    pdu[0] = (u8) (v >> 8), pdu[1] = (u8) (v & 0xFF);
}

static inline u32 netif_get_u24(u8 *pdu)
{
    return (u32) ((pdu[0] << 16) | (pdu[1] << 8) | pdu[2]);
}

static inline void netif_set_u24(u8 *pdu, u32 v)
{
    pdu[0] = (u8) (v >> 16), pdu[1] = (u8) (v >> 8), pdu[2] = (u8) v;
}

static inline u32 netif_get_u32(u8 *pdu)
{
    return (u32) ((pdu[0] << 24) | (pdu[1] << 16) | (pdu[2] << 8) | pdu[3]);
}

static inline void netif_set_u32(u8 *pdu, u32 v)
{
    pdu[0] = (u8) (v >> 24), pdu[1] = (u8) (v >> 16), pdu[2] = (u8) (v >> 8), pdu[3] = (u8) v;
}

static inline u64 netif_get_u48(u8 *pdu)
{
    return ((u64) netif_get_u32(pdu) << 16) | netif_get_u16(pdu + 4);
}

static inline void netif_set_u48(u8 *pdu, u64 v)
{
    netif_set_u32(pdu, v >> 16), netif_set_u16(pdu + 4, v & 0xFFFF);
}

static inline u64 netif_get_u64(u8 *pdu)
{
    return ((u64) netif_get_u32(pdu) << 32) | netif_get_u32(pdu + 4);
}

static inline void netif_set_u64(u8 *pdu, u64 v)
{
    netif_set_u32(pdu, v >> 32), netif_set_u32(pdu + 4, v & 0xFFFFFFFF);
}

static inline void netif_set_id(u8 *pdu, u8 *id)
{
    if (id)
        memcpy(pdu, id, 8);
    else
        memset(pdu, 0, 8);
}

static inline void netif_set_mac(u8 *pdu, u8 *mac)
{
    if (mac)
        memcpy(pdu, mac, 6);
    else
        memset(pdu, 0, 6);
}

/*******************************************************************************
 * MAC header accessors
 ******************************************************************************/

static inline u8 * nbuf_sa(u8 *pkt)
{
    return pkt;
}

static inline u8 * nbuf_da(u8 *pkt)
{
    return pkt + 6;
}

static inline u16 nbuf_ethtype(u8 *pkt)
{
    return netif_get_u16(pkt + 12);
}

static inline int nbuf_is_8021q(u8 *pkt)
{
    return nbuf_ethtype(pkt) == 0x8100;
}

static inline u16 nbuf_8021q_vid(u8 *pkt)
{
    return netif_get_u16(pkt + 14);
}

static inline u16 nbuf_8021q_ethtype(u8 *pkt)
{
    return netif_get_u16(pkt + 16);
}

/*******************************************************************************
 * Network buffer API
 ******************************************************************************/

/* ...network buffer opaque data */
typedef void netif_buffer_t;

/* ...pointer to MAC header */
extern u8 * nbuf_ethhdr(netif_buffer_t *nbuf);

/* ...pointer to packet payload */
extern u8 * nbuf_pdu(netif_buffer_t *nbuf);

extern u16 nbuf_len(netif_buffer_t *nbuf);

/* ...timestamp accessor */
extern u64 nbuf_tstamp(netif_buffer_t *nbuf);

/* ...buffer translation (ethertype determination) */
extern u16 nbuf_eth_translate(netif_buffer_t *nbuf, u16 *length);

/*******************************************************************************
 * Network stream data
 ******************************************************************************/

extern int netif_init(netif_data_t *netif, const char *name);

/* ...create network stream */
extern netif_stream_t * netif_stream_create(int sfd, u16 rx_nr, u16 tx_nr, u16 f_size);

extern netif_stream_t * netif_data_stream_create(netif_data_t *netif,
        netif_filter_t *filter, u16 rx_nr, u16 tx_nr, u16 f_size);

/* ...destroy network stream */
extern void netif_stream_destroy(netif_stream_t *stream);

/* ...get stream handle suitable for select/poll */
extern int netif_stream_fd(netif_stream_t *stream);

/* ...test if the stream is ready for receiving */
extern int netif_stream_rx_ready(netif_stream_t *stream);

/* ...wait for new frame / free place */
extern int netif_stream_wait_rx(netif_stream_t *stream);

/* ...calculate amount of the pending packets available in receive queue */
extern u16 netif_stream_rx_pending(netif_stream_t *stream);

/* ...read next frame */
extern netif_buffer_t * netif_stream_read(netif_stream_t *stream);

/* ...purge receiving stream queue */
extern void netif_stream_rx_purge(netif_stream_t *stream);

/* ...release frame (return to kernel) */
extern void netif_stream_rx_done(netif_stream_t *stream, netif_buffer_t *nbuf);

/* ...write next frame to the stream socket */
extern int netif_stream_write(netif_stream_t *stream, netif_buffer_t *nbuf, u16 length, int commit);

/* ...get next available transmission buffer */
extern netif_buffer_t * netif_stream_get_tx_buffer(netif_stream_t *stream, int wait);

/* ...get a network buffer with specified index */
extern netif_buffer_t * netif_stream_tx_buffer(netif_stream_t *stream, u16 idx);

/*******************************************************************************
 * Auxiliary tracing macros
 ******************************************************************************/

#define __tf_mac                        "%02X%02X%02X%02X%02X%02X"
#define __tp_mac(m)                     (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

#define __tf_id                         "%02X%02X%02X%02X%02X%02X%02X%02X"
#define __tp_id(i)                      (i)[0], (i)[1], (i)[2], (i)[3], (i)[4], (i)[5], (i)[6], (i)[7]

#define NBUF_TRACE(tag, nbuf, fmt, ...)     \
    ({ u8 * __h = nbuf_ethhdr(nbuf); __NBUF_TRACE(tag, __nbuf##fmt, __nbuf_param, ##__VA_ARGS__); })

#define __NBUF_TRACE(tag, fmt, ...)         \
    TRACE(tag, fmt, ##__VA_ARGS__)

#define __nbuf_fmt(fmt)                     \
    "[sa:" __tf_mac ", da:" __tf_mac ", type: %04X, vid: %X] " fmt

#define __nbuf_param                                                    \
    __tp_mac(nbuf_sa(__h)), __tp_mac(nbuf_da(__h)),                     \
    (nbuf_is_8021q(__h) ? nbuf_8021q_ethtype(__h) : nbuf_ethtype(__h)), \
    (nbuf_is_8021q(__h) ? nbuf_8021q_vid(__h) : 0)

#define __nbuf_b(fmt)                   _b(__nbuf_fmt(fmt))
#define __nbuf_x(fmt)                   _x(__nbuf_fmt(fmt))


/* ...dump network packet */
void netif_nbuf_dump(netif_buffer_t *nbuf, u16 length, const char *tag);

#define NETIF_HEADER_LENGTH             24

/* ...maximal MTU size */
#define NETIF_MTU_SIZE                   1544

static inline u8 pdu_get_subtype(u8 *pdu)
{
    return pdu[0] & 0x7F;
}

static inline u8 pdu_get_sequence_number(u8 *pdu)
{
    return pdu[2];
}

static inline u32 pdu_get_timestamp(u8 *pdu)
{
    return netif_get_u32(pdu + 12);
}

static inline u16 pdu_get_stream_data_length(u8 *pdu)
{
    return netif_get_u16(pdu + 20);
}

static inline u16 pdu_get_protocol_header(u8 *pdu)
{
    return netif_get_u16(pdu + 22);
}

static inline u8 * get_pdu(u8 *pdu)
{
    return pdu + NETIF_HEADER_LENGTH;
}

#include <linux/can.h>
/* .. data packet containing full SocketCAN frame */
typedef
struct can2udp_packet
{
    /* .. version of the data packet structure */
    uint8_t version;

    /* .. miscellaneous flags */
    uint8_t flags;

    /* .. id of the can interface the host */
    uint16_t interface_id;

} __attribute__ ((packed)) can2udp_packet_t;

#endif  /* SV_SURROUNDVIEW_NETIF_H */
