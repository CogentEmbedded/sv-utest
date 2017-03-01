/*******************************************************************************
 *
 * Offline packet parser
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

#define MODULE_TAG                      PCAP

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <netinet/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include <arpa/inet.h>

#include <pcap/pcap.h>

#include "main.h"
#include "common.h"
#include "camera.h"
#include "netif.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);

/*******************************************************************************
 * Local types definitions
 ******************************************************************************/

typedef struct netif_pcap_data
{
    /* ...packet capture handle */
    pcap_t                     *pcap;

    /* ...packet processing callback */
    camera_source_callback_t   *cb;

    /* ...processing callback client data */
    void                       *cdata;

    /* ...reading thread */
    pthread_t                   thread;

    /* ...thread termination flag */
    int                         exit;

}   netif_pcap_data_t;

/*******************************************************************************
 * Packet parser
 ******************************************************************************/

/* ...get current timestamp value */
static inline u64 net_offline_wait(struct timeval *tv, s64 *diff)
{
    struct timespec     tp;
    u64                 t0, t1;

    /* ...get current time value */
    clock_gettime(CLOCK_MONOTONIC, &tp);

    /* ...get clock value in microseconds */
    t0 = (u64)tp.tv_sec * 1000000ULL + (tp.tv_nsec / 1000);

    /* ...translate timestamp read from capture file into microseconds */
    t1 = (u64)tv->tv_sec * 1000000ULL + tv->tv_usec;

    /* ...wait until timestamp becomes valid */
    if (*diff)
    {
        t1 = (u64)(t1 + *diff);

        /* ...suspend current thread execution until timestamp gets valid */
        if (t1 > t0)    usleep(t1 - t0);
    }
    else
    {
        *diff = (s64)(t0 - t1);
    }

    /* ...t0 + __ts_diff = t1 */
    return t1;
}

static void * pcap_replay_thread(void *arg)
{
    netif_pcap_data_t  *pcap = arg;
    s64                 diff = 0;
    int                 fd = -1;

    /* ...start packets decoding loop */
    while (!pcap->exit)
    {
        struct pcap_pkthdr      pkthdr;
        u8                     *pdata;
        u64                     ts;
        int                     i;

        /* ...read next packet from the file */
        if ((pdata = (u8 *)pcap_next(pcap->pcap, &pkthdr)) == NULL)
        {
            /* ...end of file; emit end-of-stream signal */
            pcap->cb->eos(pcap->cdata);

            break;
        }

        /* ...suspend execution with respect to timestamp value */
        ts = net_offline_wait(&pkthdr.ts, &diff);

        u16     proto = netif_get_u16(pdata + 12);

        TRACE(0, _b("packet: %p[%lu.%lu] proto: %x"), pdata, pkthdr.ts.tv_sec, pkthdr.ts.tv_sec, proto);

        if (proto == 0x8100 || proto == __proto)
        {
            /* ...compare source address */
            for (i = 0; i < CAMERAS_NUMBER; i++)
            {
                /* ...simple packet selection by MAC source-address */
                if (!memcmp(pdata + 6, camera_mac_address[i], 6))
                {
                    u8     *pdu = pdata + 14;
                    u16     len = pkthdr.len - 14;

                    /* ...verify packet is a valid frame */
                    (proto == 0x8100 ? proto = netif_get_u16(pdata + 16), pdu += 4, len -= 4 : 0);

                    /* ...check packet type is expected */
                    if (proto != __proto)
                    {
                        break;
                    }

                    TRACE(0, _b("packet-%d: %p[%u]"), i, pdu, len);

                    /* ...pass packet to receiver */
                    pcap->cb->pdu(pcap->cdata, i, pdu, len, ts);

                    break;
                }
            }
        }
        else if (proto == 0x0800)
        {
            //Get the IP Header part of this packet , excluding the ethernet header
            struct iphdr *iph = (struct iphdr*)(pdata + sizeof(struct ethhdr));

            if (iph->protocol == 17)
            {
                unsigned short iphdrlen = iph->ihl*4;
                struct udphdr *udph = (struct udphdr*)(pdata + iphdrlen  + sizeof(struct ethhdr));
                int header_size =  sizeof(struct ethhdr) + iphdrlen + sizeof(struct udphdr);

                ASSERT(ntohs(udph->len) <= pkthdr.len - header_size + sizeof(struct udphdr));

                struct can2udp_packet *can2udp = (struct can2udp_packet*)(pdata + header_size);

                if (fd > 0)
                {
                    write(fd, &ts, sizeof(ts));
                    write(fd, can2udp, sizeof(*can2udp));
                }
            }
        }
    }
    close(fd);
    /* ...close file finally */
    pcap_close(pcap->pcap);

    return NULL;
}

/* ...open capturing file for replay */
void * pcap_replay(const char *filename, void *cb, void *cdata, int c)
{
    pthread_attr_t      attr;
    int                 r;
    netif_pcap_data_t  *pcap;
    char                errbuf[PCAP_ERRBUF_SIZE];

    /* ...create a pcap data */
    CHK_ERR(pcap = malloc(sizeof(*pcap)), (errno = ENOMEM, NULL));

    /* ...open capture file */
    if ((pcap->pcap = pcap_open_offline(filename, errbuf)) == NULL)
    {
        TRACE(ERROR, _x("failed to open capture file: %s"), errbuf);
        errno = ENOENT;
        goto error;
    }
    else
    {
        /* ...save callback data */
        pcap->cb = cb, pcap->cdata = cdata;
    }

    /* ...mark thread is running */
    pcap->exit = 0;

    /* ...initialize thread attributes (joinable, 1MB stack) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 1 << 20);

    /* ...create playback thread to asynchronously process buffers consumption */
    r = pthread_create(&pcap->thread, &attr, pcap_replay_thread, pcap);
    pthread_attr_destroy(&attr);
    if (r < 0)
    {
        TRACE(ERROR, _x("failed to start a playback thread: %m"));
        goto error_pcap;
    }

    return pcap;

error_pcap:
    /* ...close pcap interface */
    pcap_close(pcap->pcap);

error:
    /* ...free handle */
    free(pcap);

    return NULL;
}

/* ...playback stop */
void pcap_stop(void *arg)
{
    netif_pcap_data_t  *pcap = arg;
    void               *retval;

    /* ...instruct thread to terminate */
    pcap->exit = 1;

    /* ...wait for a thread completion if not already */
    pthread_join(pcap->thread, &retval);

    /* ...destroy thread handle */
    free(pcap);

    TRACE(INIT, _b("pcap thread completed with result %d"), (int)(intptr_t)retval);
}
