/*******************************************************************************
 *
 * Surroundview streaming
 *
 * Copyright: Cogent Embedded Inc. 2017
 * All Rights Reserved
 *
 * This file is part of Surround View Application
 *
 * It is subject to the license terms in the LICENSE file found in the top-level
 * directory of this distribution or by request via http://cogentembedded.com
 *
 ******************************************************************************/

/* 
   TODO:
   1) Code review +
   2) libmediactl +
   3) Arguments +
   4) 5th camera
   5) Frame count and send state
   6) Proper stop
   7) Color correction back channel
   8) Rework for any cameras number
 */
#define MODULE_TAG                      STREAM


/*******************************************************************************
 * Includes
 ******************************************************************************/

#define _GNU_SOURCE

#include "main.h"
#include "common.h"
#include "vsink.h"
#include "app.h"

#include <glib.h>
#include <gst/gst.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <string.h>
#include <gstreamer-1.0/gst/app/gstappsrc.h>
#include <mqueue.h>

#include <linux/media.h>
#include "libmediactl-v4l2/mediactl.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 1);
TRACE_TAG(BUFFER, 1);

#define VSP_DEVICE_NUMBER CAMERAS_NUMBER

char* media_devices[VSP_DEVICE_NUMBER] =
{
    "/dev/media0",
    "/dev/media1",
    "/dev/media2",
    "/dev/media3",
#if defined STREAM_5TH_CAM
    "/dev/media4",
#endif
};

char* vsp_devices[2 * VSP_DEVICE_NUMBER];

#define STREAM_CONTROL_MQ "/svcontrolmq"
#define RECORDING_FILENAME "test.mkv"

#define STREAMING_PIPELINE "appsrc name=stream_src_%d ! " \
    "video/x-raw,width=%d,format=UYVY,framerate=30/1,height=%d ! " \
    "vspfilter devfile-input=%s devfile-output=%s input-io-mode=userptr ! queue ! " \
    "video/x-raw,format=NV12 !" \
    "omxh264enc use-dmabuf=true num-p-frames=29 control-rate=2 " \
    "target-bitrate=4000000 ! ieee1722pay ! udpsink host=%s port=%d sync=false "

#define RECORDING_PIPELINE "appsrc name=stream_src_%d ! " \
    "video/x-raw,width=%d,format=UYVY,framerate=30/1,height=%d ! " \
    "vspfilter devfile-input=%s devfile-output=%s input-io-mode=userptr ! queue ! " \
    "video/x-raw,format=NV12 ! " \
    "omxh264enc use-dmabuf=true num-p-frames=29 control-rate=2 " \
    "target-bitrate=4000000 ! video/x-h264,profile=high ! h264parse ! " \
    "mux.video_%d "

#define PIPELINE_FILESINK " matroskamux name=mux ! filesink location=%s "

#define COMBINED_PIPELINE "appsrc name=stream_src_%d ! " \
    "video/x-raw,width=%d,format=UYVY,framerate=30/1,height=%d ! " \
    "vspfilter devfile-input=%s devfile-output=%s input-io-mode=userptr ! queue ! " \
    "video/x-raw,format=NV12 ! " \
    "omxh264enc use-dmabuf=true num-p-frames=29 control-rate=2 " \
    "target-bitrate=4000000 ! video/x-h264,profile=high ! tee name=t_%d " \
    "t_%d.src_0 ! queue ! video/x-h264,profile=high ! h264parse ! mux.video_%d "\
    "t_%d.src_1 ! queue ! video/x-h264,profile=high ! ieee1722pay ! " \
    "udpsink host=%s port=%d sync=false "

/* ...create streaming instance */
GstPipeline* stream_pipeline_start(app_data_t *app, int state)
{
    char* str;
    GstPipeline* pipeline;
    int ret = 0;
    int i;

    switch (state)
    {
    case STREAMING:
        ret = asprintf(&str,
                       STREAMING_PIPELINE STREAMING_PIPELINE
                       STREAMING_PIPELINE STREAMING_PIPELINE
#if defined STREAM_5TH_CAM
                       STREAMING_PIPELINE
#endif
                       ,
                       0, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[0], vsp_devices[1], app->stream_ip, app->stream_base_port,
                       1, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[2], vsp_devices[3], app->stream_ip, app->stream_base_port + 1,
                       2, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[4], vsp_devices[5], app->stream_ip, app->stream_base_port + 2,
                       3, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[6], vsp_devices[7], app->stream_ip, app->stream_base_port + 3
#if defined STREAM_5TH_CAM
                       , 4, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[8], vsp_devices[9], app->stream_ip, app->stream_base_port + 4
#endif
            );
        break;
    case RECORDING:
        ret = asprintf(&str,
                       RECORDING_PIPELINE RECORDING_PIPELINE
                       RECORDING_PIPELINE RECORDING_PIPELINE
#if defined STREAM_5TH_CAM
                       RECORDING_PIPELINE
#endif
                       PIPELINE_FILESINK,
                       0, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[0], vsp_devices[1], 0,
                       1, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[2], vsp_devices[3], 1,
                       2, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[4], vsp_devices[5], 2,
                       3, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[6], vsp_devices[7], 3,
#if defined STREAM_5TH_CAM
                       4, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[8], vsp_devices[9], 4,
#endif
                       app->stream_file);
        break;
    case COMBINED:
        ret = asprintf(&str, COMBINED_PIPELINE COMBINED_PIPELINE
                       COMBINED_PIPELINE COMBINED_PIPELINE
#if defined STREAM_5TH_CAM
                       COMBINED_PIPELINE
#endif
                       PIPELINE_FILESINK,
                       0, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[0], vsp_devices[1], 0, 0, 0, 0, app->stream_ip,
                       app->stream_base_port,
                       1, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[2], vsp_devices[3], 1, 1, 1, 1, app->stream_ip,
                       app->stream_base_port + 1,
                       2, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[4], vsp_devices[5], 2, 2, 2, 2, app->stream_ip,
                       app->stream_base_port + 2 ,
                       3, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[6], vsp_devices[7], 3, 3, 3, 3, app->stream_ip,
                       app->stream_base_port + 3,
#if defined STREAM_5TH_CAM
                       4, app->sv_cfg->cam_width, app->sv_cfg->cam_height,
                       vsp_devices[8], vsp_devices[9], 4, 4, 4, 4, app->stream_ip,
                       app->stream_base_port + 4,
#endif
                       app->stream_file);        
        break;
    default:
        TRACE(ERROR, _b("stream: unknown state %d"), app->stream_state);
        return NULL;
    }

    if ((ret <= 0) || (!str))
    {
        TRACE(ERROR, _b("stream: failed to allocate pipeline string %d"),
              app->stream_state);
        return NULL;
    }

    pipeline = GST_PIPELINE(gst_parse_launch(str, NULL));

    TRACE(INFO, _b("stream: pipeline \"%s\""), str);

    free(str);

    app->stream_appsrc[0] = GST_APP_SRC(gst_bin_get_by_name(GST_BIN (pipeline), "stream_src_0"));
    app->stream_appsrc[1] = GST_APP_SRC(gst_bin_get_by_name(GST_BIN (pipeline), "stream_src_1"));
    app->stream_appsrc[2] = GST_APP_SRC(gst_bin_get_by_name(GST_BIN (pipeline), "stream_src_2"));
    app->stream_appsrc[3] = GST_APP_SRC(gst_bin_get_by_name(GST_BIN (pipeline), "stream_src_3"));

    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        g_object_set(app->stream_appsrc[i],
                     "stream-type", 0,
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "num-buffers", app->stream_frame_count == 0 ? -1 : app->stream_frame_count,
                     "max-bytes", 0,
                     "block", FALSE,
                     NULL);
    }

    app->stream_pipeline = pipeline;

    app->stream_state = state;

    gst_element_set_state(GST_ELEMENT(app->stream_pipeline), GST_STATE_PAUSED);
    gst_element_set_state(GST_ELEMENT(app->stream_pipeline), GST_STATE_PLAYING);

    return pipeline;
}


/* ...Stop network stream (set to READY) */
int stream_pipeline_stop(app_data_t *app)
{
    int i;

    gst_element_set_state (GST_ELEMENT(app->stream_pipeline), GST_STATE_NULL);
    g_object_unref(app->stream_pipeline);

    app->stream_pipeline = NULL;

    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        g_object_unref(app->stream_appsrc[i]);

        app->stream_appsrc[i] = NULL;
    }

    app->stream_state = DISABLED;

    return 0;
}

/* ...create streaming instance */
int stream_pipeline_push_buffer(app_data_t *app, int i, GstPipeline* pipeline, GstBuffer* buffer)
{
    int ret;
    
    ret = gst_app_src_push_buffer(app->stream_appsrc[i], buffer);

    if (ret != GST_FLOW_OK)
    {
        TRACE(ERROR, _b("camera-%d: failed to push  buffer in streamer"), i);
        return -1;
    }

    TRACE(BUFFER, _b("camera-%d: pushed  buffer in streamer"), i);

    return 0;
}


/* ...decoding thread */
static void * stream_control_thread(void *arg)
{
    app_data_t      *app = arg;
    struct mq_attr attr;
    mqd_t qd;

    app->stream_state = DISABLED;
    app->stream_frame_count = -1; /* infinite */

    if (!app->stream_file)
    {
        app->stream_file = RECORDING_FILENAME;
    }

    /* initialize the queue attributes */
    attr.mq_flags = 0;
    attr.mq_maxmsg = 1;
    attr.mq_msgsize = sizeof(u8);
    attr.mq_curmsgs = 0;

    qd = mq_open(STREAM_CONTROL_MQ, O_RDONLY  | O_CREAT , 0644, &attr);
    if (qd == -1)
    {
        BUG(1, _b("Failed to open control channel"));
    }

    /* Open posix mq */
    while (1)
    {
        ssize_t len;
        u8 cmd;
        
        len = mq_receive(qd, (char*)&cmd, sizeof(u8), NULL);

        if (len > 0)
        {
            pthread_mutex_lock(&app->lock);

            if (app->stream_state == cmd)
            {
                pthread_mutex_unlock(&app->lock);
                continue;
            }

            switch (cmd)
            {
            case DISABLED:
                /* Disable streaming and recording */
                TRACE(INFO, _b("Disable all"));
                stream_pipeline_stop(app);
                break;
            case STREAMING:
            case RECORDING:
            case COMBINED:

                TRACE(INFO, _b("Destroy pipeline"));

                if (app->stream_state != DISABLED)
                {
                    stream_pipeline_stop(app);
                }

                TRACE(INFO, _b("Start streaming: command %d"), cmd);

                if (stream_pipeline_start(app, cmd) == NULL)
                {
                    TRACE(ERROR, _b("stream failed"));
                }

                break;
            }
            pthread_mutex_unlock(&app->lock);
        }
        else if (!len)
        {
            TRACE(ERROR, _b("EOF in control channel"));
        }
        else if (errno == EINTR || errno == EAGAIN)
        {
            /* todo: rework */
            usleep(1*1000);
        }
        else
        {
            TRACE(ERROR, _b("Unknown error in stream control channel"));
        }
    }

    return (void *)(intptr_t)-errno;
}

static gchar*
find_v4l2_for_media_device(gchar* media_name, gchar* entity_name_regex)
{
    gchar* devname = NULL;
    int ret;
    struct media_device *media = NULL;

    GRegex *regex = g_regex_new(entity_name_regex, G_REGEX_CASELESS, G_REGEX_MATCH_NOTEMPTY, NULL);
    if (!regex)
    {
        g_warning("Cannot parse the regular expression '%s'", entity_name_regex);
        goto out;
    }

    media = media_device_new(media_name);
    if (!media) {
        g_warning("Failed to create media device %s.\n", media_name);
        goto out;
    }

    if ((ret = media_device_enumerate(media))< 0)
    {
        g_warning("Failed to enumerate %s (%d).\n", media_name, ret);
        goto out;
    }

    for (int i = 0; i < media_get_entities_count(media); i++)
    {
        struct media_entity *entity = media_get_entity(media, i);
        const struct media_entity_desc *desc = media_entity_get_info(entity);
        if (!desc)
        {
            g_warning("Ignored an entity with NULL descriptor.");
            continue;
        }

        if (g_regex_match(regex, desc->name, G_REGEX_MATCH_NOTEMPTY, 0))
        {
            devname = g_strdup(media_entity_get_devname(entity));
            break;
        }
    }

    if (!devname) {
        g_warning("Entity '%s' not found\n", entity_name_regex);
        goto out;
    }

out:
    g_clear_pointer(&media,  media_device_unref);
    g_clear_pointer(&regex, g_regex_unref);

    return devname;
}

/* ...Start network stream control */
int stream_pipeline_control_start(app_data_t *app)
{
    pthread_attr_t  attr;
    int             r;
    int i;

    app->stream_ip = __stream_ip;
    app->stream_base_port = __stream_base_port;
    app->stream_file = __stream_file;

    if (!app->stream_ip || app->stream_base_port == 0)
    {
        TRACE(ERROR, _b("No stream ip or port are set"));
        return -1;
    }

    for (i = 0; i < VSP_DEVICE_NUMBER; i++)
    {
        vsp_devices[2 * i] = find_v4l2_for_media_device(media_devices[i], "rpf.0");
        vsp_devices[2 * i + 1] = find_v4l2_for_media_device(media_devices[i], "wpf.0");

        TRACE(INIT, _b("media device %s: VSP input %s VSP output %s"),
              media_devices[i],
              vsp_devices[2 * i],
              vsp_devices[2 * i + 1]);
    }

    /* ...initialize thread attributes (joinable, 128KB stack) */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setstacksize(&attr, 128 << 10);

    r = pthread_create(&app->stream_control, &attr, stream_control_thread, app);
    pthread_attr_destroy(&attr);

    return CHK_API(r);
}

/* ...create streaming instance */
int stream_pipeline_destroy(app_data_t *app)
{
    if (app->stream_state != DISABLED)
    {
        stream_pipeline_stop(app);
    }

    return 0;
}
