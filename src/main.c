/*******************************************************************************
 *
 * Surround View Application main module
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

#define MODULE_TAG                      MAIN

/*******************************************************************************
 * Includes
 ******************************************************************************/

#include <getopt.h>

#include "main.h"
#include "common.h"
#include "app.h"
#include "camera-mjpeg.h"
#include "netif.h"

/*******************************************************************************
 * Tracing configuration
 ******************************************************************************/

TRACE_TAG(INIT, 1);
TRACE_TAG(INFO, 1);
TRACE_TAG(DEBUG, 0);

/*******************************************************************************
 * Global variables definitions
 ******************************************************************************/

/* ...output devices for main / auxiliary windows */
int                 __output_main = 0;
int                 __output_transform = 0;

/* ...pointer to effective MJPEG cameras MAC addresses */
u8                (*camera_mac_address)[6];

/* ...log level (looks ugly) */
int                 LOG_LEVEL = 1;

/*******************************************************************************
 * Local variables
 ******************************************************************************/

/* ...live VIN cameras capturing flag */
static int              vin;

/* ...network interface (live capturing over EthAVB) */
static netif_data_t    netif;

/* ...network interface for live capturing */
static char            *iface = NULL;

/* ...live source processing */
static int              __live_source;

/* ...global configuration data */
static sview_cfg_t      __sv_cfg =
{
        .pixformat = GST_VIDEO_FORMAT_NV12,
        .config_path = "config.xml"
};

#if defined (JPU_SUPPORT)
/* ...jpeg decoder device name  */
char                   *jpu_dev_name = "/dev/video1";
#endif

/* ...default joystick device name  */
char                   *joystick_dev_name = "/dev/input/js0";

/* ...application flags */
static int flags;

/*******************************************************************************
 * Tracks parsing
 ******************************************************************************/

/* ...track list head */
static track_list_t     __sv_tracks;
/* ...current track position */
static track_list_t    *__sv_current;
/* ...live track */
static track_desc_t    *__sv_live;

/*******************************************************************************
 * Track reading interface
 ******************************************************************************/

/* ...switch to next track */
static inline track_list_t * track_next(track_list_t *head, track_list_t *track)
{
    BUG(head->next == head, _x("list is empty"));

    if (track)
    {
        /* ..switch to the next track in the list */
        return ((track = track->next) == head ? head->next : track);
    }
    else
    {
        /* ...no list position defined yet */
        return head->next;
    }
}

/* ...switch to previous track */
static inline track_list_t * track_prev(track_list_t *head, track_list_t *track)
{
    BUG(head->prev == head, _x("list is empty"));

    /* ..switch to the previous track in the list */
    if (track)
    {
        return ((track = track->prev) == head ? head->prev : track);
    }
    else
    {
        return head->prev;
    }
}

/* ...create new track */
static inline track_desc_t * track_create(track_list_t *head, int type)
{
    track_desc_t    *track;

    /* ...allocate a structure */
    CHK_ERR(track = calloc(1, sizeof(*track)), (errno = ENOMEM, NULL));

    if (head)
    {
        /* ...insert track into the global list */
        track->list.next = head, track->list.prev = head->prev;

        /* ...adjust sentinel node */
        head->prev->next = &track->list, head->prev = &track->list;
    }
    else
    {
        track->list.next = track->list.prev = &track->list;
    }

    /* ...set track type */
    track->type = type;

    return track;
}

static void destroy_track(track_desc_t *track)
{
    int i;

    /* At this point there is no way to find proper destructor for track private data */
    BUG(track->priv, _x("track private data must be freed before this call"));

    free(track->info);
    free(track->file);

    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        free(track->camera_names[i]);
    }

    free(track);
}

static void destroy_live_track()
{
    int i;

    if (__sv_live == NULL)
    {
        return;
    }

    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        free(__sv_live->camera_names[i]);
    }

    free(__sv_live);
}

static void destroy_tracks(track_list_t *head)
{
    track_desc_t    *track;

    destroy_live_track();

    if (!head)
    {
        return;
    }

    while (head->next != head)
    {
        track = (track_desc_t *)head->next;
        head = track->list.next;
        track->list.prev->next = track->list.next;
        track->list.next->prev = track->list.prev;

        destroy_track(track);
    }
}

/* ...return next surround-view track */
track_desc_t * sview_track_next(void)
{
    return (track_desc_t *)(__sv_current = track_next(&__sv_tracks, __sv_current));
}

/* ...return previous surround-view track */
track_desc_t * sview_track_prev(void)
{
    return (track_desc_t *)(__sv_current = track_prev(&__sv_tracks, __sv_current));
}

/* ...return current surround-view track */
track_desc_t * sview_track_current(void)
{
    return (track_desc_t *)__sv_current;
}

/* ...return live surround-view track */
track_desc_t * sview_track_live(void)
{
    return __sv_live;
}

/* ...camera initialization function */
camera_data_t * mjpeg_camera_create(int id, GstBuffer * (*get_buffer)(void *, int), void *cdata)
{
    extern camera_data_t * __camera_mjpeg_create(netif_data_t *netif, int id, u8 *da, u8 *sa, u16 vlan, GstBuffer * (*get_buffer)(void *, int), void *cdata);

    /* ...validate camera id */
    CHK_ERR((unsigned)id < CAMERAS_NUMBER, (errno = ENOENT, NULL));

    /* ...create camera object */
    return __camera_mjpeg_create((__live_source ? &netif : NULL), id, NULL, camera_mac_address[id], (u16)0x56, get_buffer, cdata);
}

/* ...default cameras MAC addresses */
static u8               default_mac_addresses[CAMERAS_NUMBER][6];

/*******************************************************************************
 * Offline network interface callback structure
 ******************************************************************************/

static void camera_source_eos(void *cdata)
{
    app_data_t   *app = cdata;

    TRACE(INFO, _b("end-of-stream signalled"));

    /* ...pass end-of-stream to the application */
    app_eos(app);
}

/* ...send PDU to the camera */
static void camera_source_pdu(void *cdata, int id, u8 *pdu, u16 len, u64 ts)
{
    app_data_t   *app = cdata;

    /* ...pass packet to the camera bin */
    app_packet_receive(app, id, pdu, len, ts);
}

/* ...camera source callback structure */
static camera_source_callback_t camera_source_cb =
{
    .eos = camera_source_eos,
    .pdu = camera_source_pdu,

};

/*******************************************************************************
 * Live capturing from VIN cameras
 ******************************************************************************/

/* ...default V4L2 device names */
char * vin_devices[CAMERAS_NUMBER] =
{
    "/dev/video0",
    "/dev/video1",
    "/dev/video2",
    "/dev/video3",
};


/* ...VIN camera set creation for a object-detection */
static GstElement * __camera_vin_create(const camera_callback_t *cb,
                                        void *cdata,
                                        int n,
                                        int width,
                                        int height)
{
    return camera_vin_create(cb, cdata, vin_devices, n, width, height);
}

/*******************************************************************************
 * Parameters parsing
 ******************************************************************************/

static inline void vin_addresses_to_name(char* str[CAMERAS_NUMBER],
                                         char *vin[CAMERAS_NUMBER])
{
    int i, j;
    for(i = 0; i < CAMERAS_NUMBER; i++)
    {
        if(!str[i])
        {
            str[i] = malloc(40);
            memset(str[i], 0, 40);
        }

        memcpy(str[i], vin[i], strlen(vin[i]));

        for(j = 0; j < (int)strlen(vin[i]); j++)
        {
            if(    str[i][j]=='/')
            {
                str[i][j] = '_';
            }
        }
    }
}

/* ...parse VIN device names */
static inline int parse_vin_devices(char *str, char **name, int n)
{
    char   *s;

    for (s = strtok(str, ","); n > 0 && s; n--, s = strtok(NULL, ","))
    {
        /* ...copy a string */
        *name++ = strdup(s);
    }

    /* ...make sure we have parsed all addresses */
    CHK_ERR(n == 0, -EINVAL);

    return 0;
}

/* ...parse video stream file names */
static inline int parse_video_file_names(char *str, char **name, int n)
{
    char   *s;

    for (s = strtok(str, ","); n > 0 && s; n--, s = strtok(NULL, ","))
    {
        /* ...copy a string */
        *name++ = strdup(s);
    }

    /* ...make sure we have parsed all addresses */
    CHK_ERR(n == 0, -EINVAL);

    return 0;
}

static inline void mac_addresses_to_name(char* str[4], u8 addr[4][6])
{
    int i;
    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        if (!str[i])
        {
            str[i] = malloc(40);
            memset(str[i], 0, 40);
        }

        sprintf(str[i], "%02x-%02x-%02x-%02x-%02x-%02x", addr[i][0], addr[i][1], addr[i][2],
                addr[i][3], addr[i][4], addr[i][5]);
    }
}

/* ...MAC address parsing */
static inline int parse_mac_addresses(char *str, u8(*addr)[6], int n)
{
    char *s;

    for (s = strtok(str, ","); n > 0 && s; n--, s = strtok(NULL, ","))
    {
        u8 *b = *addr++;

        /* ...parse MAC address from the string */
        CHK_ERR(sscanf(s, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX", b, b + 1, b + 2, b + 3, b + 4, b + 5) == 6, -EINVAL);
    }

    /* ...make sure we have parsed all addresses */
    CHK_ERR(n == 0, -EINVAL);

    return 0;
}

/* ...parse camera format */
static inline int parse_format(char *str)
{
    if (strcasecmp(str, "uyvy") == 0)
    {
        return GST_VIDEO_FORMAT_UYVY;
    }
    else if (strcasecmp(str, "i420") == 0)
    {
        return GST_VIDEO_FORMAT_I420;
    }
    else if (strcasecmp(str, "nv12") == 0)
    {
        return GST_VIDEO_FORMAT_NV12;
    }
    else
    {
        return 0;
    }
}

/* ...configuration file parsing */
static int parse_cfg_file(char *name)
{
    track_desc_t   *track = NULL;
    int             num = 0;
    FILE           *f;

    /* ...open file handle */
    CHK_ERR(f = fopen(name, "rt"), -errno);

    /* ...parse tracks */
    while (1)
    {
        char    buf[4096], *s;

        /* ...get next string from the file */
        if (fgets(buf, sizeof(buf) - 1, f) == NULL)     break;

        /* ...strip trailing newline */
        ((s = strchr(buf, '\n')) != NULL ? *s = '\0' : 0);

        /* ...parse string */
        if (!strcmp(buf, "[sv-track]"))
        {
            /* ...start new surround-view track */
            CHK_ERR(track = track_create(&__sv_tracks, 0), -errno);
            track->pixformat = __sv_cfg.pixformat;
            /* ...advance tracks number */
            num++;

            /* ...mark we have a surround-view track */
            flags |= APP_FLAG_SVIEW;
            flags |= APP_FLAG_FILE;

            continue;
        }
        else if (!track)
        {
            /* ...no current track defined; skip line */
            continue;
        }

        /* ...parse track section */
        if (!strncmp(buf, "file=", 5))
        {
            CHK_ERR(track->file = strdup(buf + 5), -(errno = ENOMEM));
        }
        else if (!strncmp(buf, "info=", 5))
        {
            CHK_ERR(track->info = strdup(buf + 5), -(errno = ENOMEM));
        }
        else if (!strncmp(buf, "mac=", 4))
        {
            /* ...parse MAC addresses */
            CHK_API(parse_mac_addresses(buf + 4, track->mac, CAMERAS_NUMBER));
           mac_addresses_to_name(track->camera_names, track->mac);
        }
    }

    TRACE(INIT, _b("configuration file parsed (%u tracks)"), num);

    return num;
}

static
inline int parse_camera_intrinsic_frames(char *str,
    char *names[CAMERAS_NUMBER],
    int n)
{
    char *s;

    for (s = strtok(str, ","); n > 0 && s; n--, s = strtok(NULL, ","))
    {
        /* ...copy a string */
        names[CAMERAS_NUMBER - n] = strdup(s);
    }

    return 0;
}

/* internal IDs for command line options */
enum surround_view_options
{
    OPT_DEBUG     = 'd',
    OPT_IFACE     = 'i',
    OPT_MAC       = 'm',
    OPT_VIN       = 'v',
    OPT_FORMAT    = 'f',
    OPT_CONFIG    = 'c',
    OPT_OUTPUT    = 'o',
    OPT_TRANSFORM = 't',
    OPT_JPU       = 'j',
    OPT_JOYSTICK  = 'w',
    OPT_HELP      = 'h',
    OPT_VERSION   = 'V',

    // Start unnamed IDs after 'z'
    OPT_VIEW = 'z' + 1,
    OPT_NON_FISHEYE,
    OPT_SAVE,

    OPT_RESOLUTION,
    OPT_CAMERA_RESOLUTION,
    OPT_CAN_DUMP,
    OPT_INTRINSIC_FRAMES,
    OPT_EXTRINSIC_FRAMES,
    OPT_INTRINSIC_OUTPUT,
    OPT_EXTRINSIC_OUTPUT,
    OPT_INTRINSICS_CELL_WIDTH,
    OPT_INTRINSICS_CELL_HEIGHT,
    OPT_INTRINSICS_BOARD_WIDTH,
    OPT_INTRINSICS_BOARD_HEIGHT,
    OPT_INTRINSICS_GRAB_INTERVAL,
    OPT_INTRINSICS_NUM_FRAMES,
    OPT_EXTRINSICS_NUM_CIRCLES,
    OPT_EXTRINSICS_CIRCLES_PARAM    
};

/* ...command-line options */
static const struct option    options[] = {
    {   "debug",    required_argument,  NULL,   OPT_DEBUG },
    {   "iface",    required_argument,  NULL,   OPT_IFACE },
    {   "mac",      required_argument,  NULL,   OPT_MAC },
    {   "vin",      required_argument,  NULL,   OPT_VIN },
    {   "format",   required_argument,  NULL,   OPT_FORMAT },    
    {   "cfg",      required_argument,  NULL,   OPT_CONFIG },
    {   "output",   required_argument,  NULL,   OPT_OUTPUT },
    {   "transform",required_argument,  NULL,   OPT_TRANSFORM },
#if defined (JPU_SUPPORT)
    {   "jpu",      required_argument,  NULL,   OPT_JPU },
#endif
    {   "js",       required_argument,  NULL,   OPT_JOYSTICK },
    {   "help",     no_argument,        NULL,   OPT_HELP },
    {   "version",  no_argument,        NULL,   OPT_VERSION },

    /* ...svlib configuration options */
    {   "view",             required_argument,  NULL, OPT_VIEW },
    {   "nonFisheyeCam",    no_argument,        NULL, OPT_NON_FISHEYE },
    {   "save",             no_argument,        NULL, OPT_SAVE },

    {   "resolution",       required_argument,  NULL, OPT_RESOLUTION },
    {   "camres",           required_argument,  NULL, OPT_CAMERA_RESOLUTION },

    {   "intrinsicframes",  required_argument,  NULL, OPT_INTRINSIC_FRAMES },
    {   "extrinsicframes",  required_argument,  NULL, OPT_EXTRINSIC_FRAMES },
    {   "intrinsicoutput",  required_argument,  NULL, OPT_INTRINSIC_OUTPUT },
    {   "extrinsicoutput",  required_argument,  NULL, OPT_EXTRINSIC_OUTPUT },
    /* Calibration parameters */

    /* Intrinsics parameters */
    {   "intrinsics-cell-width",  required_argument,  NULL, OPT_INTRINSICS_CELL_WIDTH },
    {   "intrinsics-cell-height",  required_argument,  NULL, OPT_INTRINSICS_CELL_HEIGHT },
    {   "intrinsics-board-width",  required_argument,  NULL, OPT_INTRINSICS_BOARD_WIDTH },
    {   "intrinsics-board-height",  required_argument,  NULL, OPT_INTRINSICS_BOARD_HEIGHT },
    {   "intrinsics-grab-interval",  required_argument,  NULL, OPT_INTRINSICS_GRAB_INTERVAL },
    {   "intrinsics-num-frames",  required_argument,  NULL, OPT_INTRINSICS_NUM_FRAMES },

    /* Extrinsics parameters */
    {   "extrinsics-num-circles",  required_argument,  NULL, OPT_EXTRINSICS_NUM_CIRCLES },    
    {   "extrinsics-circles-param",  required_argument,  NULL, OPT_EXTRINSICS_CIRCLES_PARAM },

    {   NULL,               0,                  NULL, 0 },
};

static void print_usage()
{
    printf( "Usage: sv-utest [options]\n"
            "\nAvailable options:\n"
            "\t-d|--debug\t- set debug level 0-6\n"
	    "\t-f|--format\t- video format, must be first in command line (available options: uyvy, nv12,i420)\n"
            "\t-i|--iface\t- for MJPEG cameras only, network interface\n"
            "\t-m|--mac\t- for MJPEG cameras only, cameras MAC list: mac1,mac2,mac3,mac4\n"
            "\t        \t  where mac is in form AA:BB:CC:DD:EE:FF\n"
            "\t-v|--vin\t- V4L2 camera devices list: cam1,cam2,cam3,cam4\n"
            "\t        \t  where cam is in form /dev/videoX\n"
            "\t-c|--cfg\t- playback tracks configuration to load\n"
            "\t-o|--output\t- desired Weston display output number 0, 1,.., N\n"
            "\t-w|--js\t\t- joystick device name\n"
            "\t-h|--help\t- this help\n"
            "\t-V|--version\t- print version\n"
            "\t--view\t\t- orientation of window 0 - portrait, 1 - landscape\n"
            "\t--resolution\t- window size as WidthxHeight\n"
            "\t--camres\t- camera output size as WIDTHxHEIGHT\n"
            "\nAuxiliary calibration options:\n"
            "\t--intrinsicframes <mask1>,<mask2>,<mask3>,<mask4> - specify comma-separated\n"
            "\t         list of file masks which can be loaded in calibration UI\n"
            "\t         in place of grabbed frames\n"
            "\t--extrinsicframes <mask>,<mask2>,<mask3>,<mask4> - specify file masks\n"
            "\t         which can be loaded in extrinsic calibration UI\n"
            "\t         in place of grabbed frames\n"
            "\t--intrinsicoutput <directory> - specify directory where grabbed\n"
            "\t         intrinsic calibration frames are stored\n"
            "\t         with camera%%d_frame%%d.png file names\n"
            "\t--extrinsicoutput <directory> - specify directory where grabbed\n"
            "\t         extrinsic calibration frames are stored\n"
            "\t         with extrinsic_frame%%d.png file names"
	    "\nCalibration options:\n"
	    "\t--intrinsics-cell-width <value> - width of the cell on the chess pattern board\n"
	    "\t         in mm, default 50\n"
	    "\t--intrinsics-cell-height <value> - height of the cell on the chess pattern board\n"
	    "\t         in mm, default 50\n"
	    "\t--intrinsics-board-width <value> - width of the chess pattern board\n"
	    "\t         in terms of cross between cells, default 9\n"
	    "\t--intrinsics-board-height <value> - height of the chess pattern board\n"
	    "\t         in terms of cross between cells, default 6\n"
	    "\t--intrinsics-grab-interval <value> - time interval between frame capture attempts\n"
	    "\t         in timer mode in seconds, default 10 seconds\n"
	    "\t--intrinsics-num-frames <value> - number of frames to grab for intrinsics calculation,\n"
	    "\t         default 15 frames\n"
	    "\t--extrinsics-num-circles <value> - number of circles on pattern (2 or 3)\n"
	    "\t--extrinsics-circles-param <value> - circles pattern parameter: radius for 2-circles pattern,\n"
	    "\t         length between circles centers for 3-circles pattern\n"
            "\n"
    );
}

static void print_version()
{
    printf("Surround View Application, version %s\n"
           "Copyright (C) 2016-2017 Cogent Embedded Inc.\n"
           "All Rights Reserved\n",
            SV_VERSION_STRING);
}

/* ...option parsing */
static int parse_cmdline(int argc, char **argv)
{
    sview_cfg_t    *cfg = &__sv_cfg;
    int             index = 0;
    int             opt;
    int             channel;

    memset(cfg, 0 , sizeof(sview_cfg_t));
    cfg->pixformat = GST_VIDEO_FORMAT_UYVY;
    cfg->config_path = "config.xml";
    cfg->view_type = -1;
    for (channel = 0; channel < CAMERAS_NUMBER; ++channel)
    {
        cfg->vfd[channel] = -1;
    }

    /* ...process command-line parameters */
    while ((opt = getopt_long(argc, argv, "d:i:m:v:c:o:t:j:w:f:l:hV", options, &index)) >= 0)
    {
        switch (opt)
        {
        case OPT_HELP:
            print_usage();
            exit(0);
            break;

        case OPT_VERSION:
            print_version();
            exit(0);
            break;

        case OPT_DEBUG:
            /* ...debug level */
            TRACE(INIT, _b("debug level: '%s'"), optarg);
            LOG_LEVEL = atoi(optarg);

            /* Enable debug overlay */
            if (LOG_LEVEL >= LOG_INFO)
            {
                flags |= APP_FLAG_DEBUG;
            }
            break;

        case OPT_IFACE:
            /* ...short option - network interface */
            TRACE(INIT, _b("net interface: '%s'"), optarg);
            iface = optarg;
            break;

        case OPT_MAC:
            /* ...MAC address of network camera */
            TRACE(INIT, _b("MAC address: '%s'"), optarg);
            CHK_API(parse_mac_addresses(optarg, default_mac_addresses, CAMERAS_NUMBER));
            mac_addresses_to_name(cfg->cam_names, default_mac_addresses);
            break;
        case OPT_VIN:
            /* ...VIN device name (for live capturing from frontal camera) */
            TRACE(INIT, _b("VIN devices: '%s'"), optarg);
            CHK_API(parse_vin_devices(optarg, vin_devices, CAMERAS_NUMBER));
            vin_addresses_to_name(cfg->cam_names, vin_devices);
            vin = 1;
            break;
	case OPT_FORMAT:
	    /* ...VIN device name (for live capturing from frontal camera) */
	    TRACE(INIT, _b("Video format: '%s'"), optarg);
	    cfg->pixformat = parse_format(optarg);
            break;
        case OPT_CONFIG:
            /* ...parse offline track configuration */
            TRACE(INIT, _b("read tracks from configuration file '%s'"), optarg);
            CHK_API(parse_cfg_file(optarg));
            break;

        case OPT_OUTPUT:
            /* ...set global display output for a main window (surround-view scene) */
            __output_main = atoi(optarg);
            TRACE(INIT, _b("output for main window: %d"), __output_main);
            break;

#if defined (JPU_SUPPORT)
        case OPT_JPU:
            /* ...set default JPU decoder V4L2 device name */
            TRACE(INIT, _b("jpec decoder dev name : '%s'"), optarg);
            jpu_dev_name = optarg;
            break;
#endif

        case OPT_JOYSTICK:
            /* ...set default joystick device name */
            TRACE(INIT, _b("joystick device: '%s'"), optarg);
            joystick_dev_name = optarg;
            break;

        /*... beginning surround view cmd line parameters*/
        case OPT_VIEW:
            TRACE (INIT, _b ("view: '%s'"), optarg);
            cfg->start_view = atoi (optarg);
            break;

        case OPT_NON_FISHEYE:
            TRACE (INIT, _b ("nonFisheyeCam ON"));
            cfg->non_fisheye_camera = 1;
            break;

        case OPT_SAVE:
            TRACE (INIT, _b ("save ON"));
            cfg->saveFrames = 1;
            break;

        case OPT_RESOLUTION:
            TRACE (INIT, _b ("resolution: %s"), optarg);
            if (sscanf(optarg, "%dx%d", &cfg->width, &cfg->height)  != 2)
            {
                TRACE(ERROR, _x("Wrong resolution format. Example:  --resolution 320x240"));
                cfg->width = cfg->height = 0;
            }
            break;

        case OPT_CAMERA_RESOLUTION:
            TRACE (INIT, _b ("camera output resolution: %s"), optarg);
            if (sscanf(optarg, "%dx%d", &cfg->cam_width, &cfg->cam_height) != 2)
            {
                TRACE(ERROR, _x("Wrong resolution format. Example:  --camres 1280x800"));
                cfg->cam_width = cfg->cam_height = 0;
                break;
            }
            break;

        case OPT_INTRINSIC_FRAMES:
            TRACE (INIT, _b ("Intrinsic camera frames: %s"), optarg);
            parse_camera_intrinsic_frames(optarg,
                cfg->intrinsic_frames_mask,
                CAMERAS_NUMBER);
            break;

        case OPT_EXTRINSIC_FRAMES:
            TRACE (INIT, _b ("Extrinsic camera frames: %s"), optarg);
            cfg->extrinsic_frames_mask = malloc(strlen(optarg) + 1);
            strcpy(cfg->extrinsic_frames_mask, optarg);
            break;

        case OPT_INTRINSIC_OUTPUT:
            TRACE (INIT, _b ("Intrinsic output directory: %s"), optarg);
            cfg->intrinsic_output_directory = malloc(strlen(optarg) + 1);
            strcpy(cfg->intrinsic_output_directory, optarg);
            break;

        case OPT_EXTRINSIC_OUTPUT:
            TRACE (INIT, _b ("Extrinsic output directory: %s"), optarg);
            cfg->extrinsic_output_directory = malloc(strlen(optarg) + 1);
            strcpy(cfg->extrinsic_output_directory, optarg);
            break;
        case OPT_INTRINSICS_CELL_WIDTH:
            cfg->calib_cell_w = strtof(optarg, NULL);
            TRACE(INIT, _b("Intrinsics calibration: pattern board cell width: %f"), cfg->calib_cell_w);
            break;
	case OPT_INTRINSICS_CELL_HEIGHT:
            cfg->calib_cell_h = strtof(optarg, NULL);
	    TRACE(INIT, _b("Intrinsics calibration: pattern board cell height: %f"), cfg->calib_cell_h);
	    break;
	case OPT_INTRINSICS_BOARD_WIDTH:
	    cfg->calib_board_w = atoi(optarg);
	    TRACE(INIT, _b("Intrinsics calibration: pattern board width: %d"), cfg->calib_board_w);
	    break;	    
	case OPT_INTRINSICS_BOARD_HEIGHT:
            cfg->calib_board_h = atoi(optarg);
	    TRACE(INIT, _b("Intrinsics calibration: pattern board height: %d"), cfg->calib_board_h);
	    break;
	case OPT_INTRINSICS_GRAB_INTERVAL:
	    cfg->calib_grab_interval = atoi(optarg);
	    TRACE(INIT, _b("Intrinsics calibration: calibration grab interval: %d"), cfg->calib_grab_interval);
	    break;
	case OPT_INTRINSICS_NUM_FRAMES:
	    cfg->calib_boards_required = atoi(optarg);
	    TRACE(INIT, _b("Intrinsics calibration: required boards number: %d"), cfg->calib_boards_required);
	    break;	    
	case OPT_EXTRINSICS_CIRCLES_PARAM:
            cfg->pattern_radius = strtof(optarg, NULL);
	    TRACE(INIT, _b("Extrinsics calibration: pattern radius: %f"), cfg->pattern_radius);
	    break;
	case OPT_EXTRINSICS_NUM_CIRCLES:
            cfg->pattern_num_circles = atoi(optarg);
	    TRACE(INIT, _b("Extrinsics calibration: number of circles: %d"), cfg->calib_boards_required);
	    break;	    
        default:
        return -EINVAL;
        }
    }
    /* ...check we have found both live tracks */
    if (iface)
    {
        /* ...create live track descriptor */
        CHK_ERR(__sv_live = track_create(NULL, 0), -(errno = ENOMEM));

        /* ...set parameters */
        memcpy(__sv_live->mac, default_mac_addresses, sizeof(default_mac_addresses));
        __sv_live->camera_cfg = __sv_cfg.config_path;
        __sv_live->pixformat = cfg->pixformat;
        mac_addresses_to_name(__sv_live->camera_names, default_mac_addresses);
        __sv_live->camera_type = TRACK_CAMERA_TYPE_MJPEG;
        flags |= APP_FLAG_SVIEW;
        flags |= APP_FLAG_LIVE;
    }
    if (vin)
    {
        /* ...create live track descriptor */
        CHK_ERR(__sv_live = track_create(NULL, 0), -(errno = ENOMEM));
        __sv_live->camera_cfg = __sv_cfg.config_path;
        vin_addresses_to_name(__sv_live->camera_names, vin_devices);
        __sv_live->pixformat = GST_VIDEO_FORMAT_UYVY;
        __sv_live->camera_type = TRACK_CAMERA_TYPE_VIN;
        flags |= APP_FLAG_SVIEW;
        flags |= APP_FLAG_LIVE;
    }
    return 0;
}

/*******************************************************************************
 * Offline replay thread
 ******************************************************************************/

/* ...play data captured in PCAP file */
static inline int playback_pcap(app_data_t *app, track_desc_t *track, int start)
{
    /* ...PCAP is allowed only for surround-view track */
    CHK_ERR(track->type == 0, -EINVAL);
    camera_mac_address = track->mac;
    if (start)
    {
        /* ...initialize camera bin */
        CHK_API(sview_camera_init(app, camera_mjpeg_create));

        /* ...start PCAP replay thread (we should get a control handle, I guess) */
        CHK_ERR(track->priv = pcap_replay(track->file, &camera_source_cb, app, 0), -errno);
    }
    else
    {
        /* ...stop playback */
        pcap_stop(track->priv);
        track->priv = NULL;
    }

    return 0;
}

/* ...play data captured in BLF file */
static inline int playback_blf(app_data_t *app, track_desc_t *track, int start)
{
    /* ...BLF is allowed only for surround-view track */
    CHK_ERR(track->type == 0, -EINVAL);

    if (start)
    {
        /* ...initialize camera bin */
        CHK_API(sview_camera_init(app, camera_mjpeg_create));

        /* ...start BLF replay thread */
        CHK_ERR(track->priv = blf_replay(track->file, &camera_source_cb, app), -errno);
    }
    else
    {
        /* ...stop BLF thread */
        blf_stop(track->priv);
        track->priv = NULL;
    }

    return 0;
}

/* File names for mp4 replay */
static char * file_names[CAMERAS_NUMBER] = {NULL, NULL, NULL, NULL};

const char * video_stream_get_file(int i)
{
    if (i > CAMERAS_NUMBER - 1)
    {
        return NULL;
    }
    else
    {
        return file_names[i];
    }
}

/* ...play data from a movie file */
static inline int playback_video(app_data_t *app, track_desc_t *track, int start)
{
    if (start)
    {
        CHK_ERR(track->type == 0, -EINVAL);
        parse_video_file_names(track->file, file_names, CAMERAS_NUMBER);
        CHK_API(sview_camera_init(app, video_stream_create));
    }
    else
    {
        /* ...no special stop command; just emit eos */
    }

    return 0;
}

/*******************************************************************************
 * Track preparation - public API
 ******************************************************************************/
/* ...VIN capturing control */
static inline int app_vin_capturing(app_data_t *app, track_desc_t *track, int start)
{
    TRACE(INIT, _b("%s live capturing from VIN cameras"), (start ? "start" : "stop"));

    /* ...set live source flag */
    __live_source = 1;

    if (start)
    {
            /* ...make sure it is a live sview track */
        CHK_ERR(track == __sv_live, -EINVAL);
        CHK_API(sview_camera_init(app, __camera_vin_create));
    }
    else
    {
        /* ...anything special? - tbd */
    }

    return 0;
}
/* ...live network capturing control */
static inline int app_net_capturing(app_data_t *app, track_desc_t *track, int start)
{
    /* ...it must be a live surround-view track */
    CHK_ERR(track == __sv_live, -EINVAL);

    /* ...set live source flag */
    __live_source = 1;

    TRACE(INIT, _b("%s live capturing from '%s'"), (start ? "start" : "stop"), iface);

    if (start)
    {
        /* ...add surround-view camera set */
        CHK_API(sview_camera_init(app, camera_mjpeg_create));
    }
    else
    {
        /* ...anything special? - tbd */
    }

    return 0;
}

/* ...offline playback control */
static inline int app_offline_playback(app_data_t *app, track_desc_t *track, int start)
{
    char   *filename = track->file;

    TRACE(INIT, _b("%s offline playback: file='%s'"), (start ? "start" : "stop"), filename);

    /* ...clear live interface flag */
    __live_source = 0;

    char   *ext;
    /* ...get file extension */
    if ((ext = strrchr(filename, '.')) != NULL)
    {
        ext++;

        if (!strcasecmp(ext, "pcap"))
        {
            /* ...file is a TCPDUMP output */
            return CHK_API(playback_pcap(app, track, start));
        }
        else if (!strcasecmp(ext, "blf"))
        {
            /* ...file is a Vector BLF format */
            return CHK_API(playback_blf(app, track, start));
        }
    }
    /* ...unrecognized extension; treat file as a movie clip */
    return CHK_API(playback_video(app, track, start));
}


/* ...start track */
int app_track_start(app_data_t *app, track_desc_t *track, int start)
{
    /* ...initialize active cameras set (not always required) */

    /* ...current played file? - tbd */
    if (track == __sv_live)
    {
        TRACE(DEBUG, _b("track start"));

        if (track->camera_type == TRACK_CAMERA_TYPE_MJPEG)
        {
            camera_mac_address = track->mac;
            return app_net_capturing(app, track, start);
        }
        if (track->camera_type == TRACK_CAMERA_TYPE_VIN)
        {
            return app_vin_capturing(app, track, start);
        }
    }
    else if (track->file)
    {
        return app_offline_playback(app, track, start);
    }

    return CHK_API(- EINVAL);
}

/*******************************************************************************
 * Entry point
 ******************************************************************************/

int main(int argc, char **argv)
{
    int i;

    display_data_t  *display;
    app_data_t      *app;

    /* ...initialize tracer facility */
    TRACE_INIT("Surround View Application: " SV_VERSION_STRING);

    /* ...initialize GStreamer */
    gst_init(&argc, &argv);

    /* ...initialize global tracks list */
    __sv_tracks.next = __sv_tracks.prev = &__sv_tracks;

    /* ...parse application specific parameters */
    CHK_API(parse_cmdline(argc, argv));
    if (!__sv_cfg.cam_width)
    {
        __sv_cfg.cam_width = CAMERA_IMAGE_WIDTH;
    }
    if (!__sv_cfg.cam_height)
    {
        __sv_cfg.cam_height = CAMERA_IMAGE_HEIGHT;
    }

    /* ...initialize display subsystem */
    CHK_ERR(display = display_create(), -errno);
    /* ...initialize surround-view application */
    CHK_ERR(app = app_init(display, &__sv_cfg, flags), -errno);

    /* ...initialize network interface for a live capturing case */
    if(iface)
        CHK_API(netif_init(&netif, iface));

    /* ...execute mainloop thread */
    app_thread(app);

    destroy_tracks(&__sv_tracks);
    for (i = 0; i < CAMERAS_NUMBER; i++)
    {
        free(file_names[i]);
    }

    TRACE_INIT("application terminated");

    return 0;
}
