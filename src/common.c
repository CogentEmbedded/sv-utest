/*******************************************************************************
 *
 * Common helpers for the application
 *
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

#define MODULE_TAG                      COMMON

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <fcntl.h>
#include <sys/timerfd.h>

#include "main.h"
#include "common.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(DEBUG, 0);

/*******************************************************************************
 * Timeout support
 ******************************************************************************/

/* ...data-source handle */
typedef struct timer_source
{
    /* ...generic source handle */
    GSource             source;

    /* ...timer file descriptor */
    int                 tfd;

    /* ...polling object tag */
    gpointer            tag;

}   timer_source_t;

/* ...prepare handle */
static gboolean timer_source_prepare(GSource *source, gint *timeout)
{
    /* ...we need to go to "poll" call anyway */
    *timeout = -1;
    return FALSE;
}

/* ...check function called after polling returns */
static gboolean timer_source_check(GSource *source)
{
    timer_source_t     *tsrc = (timer_source_t *) source;

    TRACE(DEBUG, _b("timer-fd: %p, poll: %X"),
            tsrc->tag,
            (tsrc->tag ? g_source_query_unix_fd(source, tsrc->tag) : ~0U));

    /* ...test if last poll returned data availability */
    if (tsrc->tag && (g_source_query_unix_fd(source, tsrc->tag) & G_IO_IN))
    {
        guint64     value;

        /* ...read timer value to clear polling flag */
        read(tsrc->tfd, &value, sizeof(value));
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/* ...dispatch function */
static gboolean timer_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    timer_source_t     *tsrc = (timer_source_t *) source;

    /* ...call dispatch function if still enabled */
    return (tsrc->tag ? callback(user_data) : TRUE);
}

/* ...finalization function */
static void timer_source_finalize(GSource *source)
{
    TRACE(DEBUG, _b("timer-source destroyed"));
}

/* ...source callbacks */
static GSourceFuncs timer_source_funcs = {
    .prepare = timer_source_prepare,
    .check = timer_source_check,
    .dispatch = timer_source_dispatch,
    .finalize = timer_source_finalize,
};

/* ...file source creation */
timer_source_t * timer_source_create(GSourceFunc func, gpointer user_data,
        GDestroyNotify notify, GMainContext *context)
{
    timer_source_t *tsrc;
    GSource        *source;

    /* ...allocate source handle */
    SV_CHK_ERR(source = g_source_new(&timer_source_funcs, sizeof(*tsrc)), NULL);

    /* ...create file descriptor */
    (tsrc = (timer_source_t *)source)->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    /* ...make sure descriptor is valid */
    SV_CHK_ERR(tsrc->tfd >= 0, NULL);

    /* ...do not enable source until explicit start command received */
    tsrc->tag = NULL;

    /* ...set priority */
    g_source_set_priority(source, G_PRIORITY_DEFAULT);

    /* ...set callback function */
    g_source_set_callback(source, func, user_data, notify);

    /* ...attach source to the default thread context */
    g_source_attach(source, context);

    /* ...pass ownership to the loop */
    g_source_unref(source);

    return tsrc;
}

/* ...retrive file descriptor */
int timer_source_get_fd(timer_source_t *tsrc)
{
    return tsrc->tfd;
}

/* ...start timeout operation */
void timer_source_start(timer_source_t *tsrc, u32 interval, u32 period)
{
    GSource            *source = (GSource *)tsrc;
    struct itimerspec   ts;

    /* ...(re)set timer parameters */
    ts.it_interval.tv_sec = period / 1000;
    ts.it_interval.tv_nsec = (period % 1000) * 1000000;
    ts.it_value.tv_sec = interval / 1000;
    ts.it_value.tv_nsec = (interval % 1000) * 1000000;
    timerfd_settime(tsrc->tfd, 0, &ts, NULL);

    /* ...add timer-source to the poll loop as needed */
    (!tsrc->tag ? tsrc->tag = g_source_add_unix_fd(source, tsrc->tfd, G_IO_IN | G_IO_ERR) : 0);

    TRACE(DEBUG, _b("timer-source[%p] activated (int=%u, period=%u)"), tsrc, interval, period);
}

/* ...suspend file source */
void timer_source_stop(timer_source_t *tsrc)
{
    GSource    *source = (GSource *)tsrc;

    if (tsrc->tag)
    {
        struct itimerspec   ts;

        /* ...remove timer from poll loop */
        g_source_remove_unix_fd(source, tsrc->tag);
        tsrc->tag = NULL;

        /* ...disable timer operation */
        memset(&ts, 0, sizeof(ts));
        timerfd_settime(tsrc->tfd, 0, &ts, NULL);

        TRACE(DEBUG, _b("timer-source [%p] suspended"), tsrc);
    }
}

/* ...check if data source is active */
int timer_source_is_active(timer_source_t *tsrc)
{
    return (tsrc->tag != NULL);
}

