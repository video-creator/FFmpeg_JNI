/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include "config_components.h"
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/tx.h"
#include "libswresample/swresample.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include "cmdutils.h"
#include "ffplay_renderer.h"
#include "opt_common.h"
#include "ffplay_jni.h"
const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;
typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;
typedef enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } ShowMode;
enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

/* Forward declarations needed by the multi-instance registry below */
typedef struct VideoState VideoState;
typedef struct FFPlayGlobalParams FFPlayGlobalParams;

/* ============================================================
 * Multi-instance registry
 * Maps SDL windowID → VideoState so events can be dispatched
 * to the correct player instance without any globals.
 * All operations are protected by a single SDL mutex.
 * ============================================================ */
#define FFPLAY_MAX_INSTANCES 64

typedef struct FFPlayInstanceEntry {
    Uint32        window_id;   /* SDL_GetWindowID(gp->window) */
    VideoState   *is;
} FFPlayInstanceEntry;

static FFPlayInstanceEntry g_instances[FFPLAY_MAX_INSTANCES];
static int                 g_instance_count = 0;
static SDL_mutex          *g_instance_mutex = NULL;   /* created on first SDL_Init */

/* Called once after SDL_Init() to create the registry mutex */
static void ffplay_registry_init(void)
{
    if (!g_instance_mutex)
        g_instance_mutex = SDL_CreateMutex();
}

/* Called once when the last instance exits, to destroy the mutex */
static void ffplay_registry_destroy(void)
{
    if (g_instance_mutex) {
        SDL_DestroyMutex(g_instance_mutex);
        g_instance_mutex = NULL;
    }
}

static void ffplay_register_instance(Uint32 window_id, VideoState *is)
{
    SDL_LockMutex(g_instance_mutex);
    if (g_instance_count < FFPLAY_MAX_INSTANCES) {
        g_instances[g_instance_count].window_id = window_id;
        g_instances[g_instance_count].is        = is;
        g_instance_count++;
    }
    SDL_UnlockMutex(g_instance_mutex);
}

static void ffplay_unregister_instance(VideoState *is)
{
    SDL_LockMutex(g_instance_mutex);
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].is == is) {
            g_instances[i] = g_instances[--g_instance_count];
            break;
        }
    }
    SDL_UnlockMutex(g_instance_mutex);
}

/* Returns NULL if not found */
static VideoState *ffplay_find_instance_by_window(Uint32 window_id)
{
    VideoState *result = NULL;
    SDL_LockMutex(g_instance_mutex);
    for (int i = 0; i < g_instance_count; i++) {
        if (g_instances[i].window_id == window_id) {
            result = g_instances[i].is;
            break;
        }
    }
    SDL_UnlockMutex(g_instance_mutex);
    return result;
}

/* Returns the number of currently active instances */
static int ffplay_instance_count(void)
{
    int n;
    SDL_LockMutex(g_instance_mutex);
    n = g_instance_count;
    SDL_UnlockMutex(g_instance_mutex);
    return n;
}

/* ============================================================
 * Deferred destruction mechanism
 * 
 * To avoid race conditions where an instance is destroyed during
 * event handling but the pointer is still used in video_refresh,
 * we use a deferred destruction mechanism:
 * 1. During event handling, mark instances for destruction
 * 2. After video_refresh, actually destroy the marked instances
 * ============================================================ */
static VideoState *g_pending_destroy[FFPLAY_MAX_INSTANCES];
static int g_pending_destroy_count = 0;

/* Mark an instance for deferred destruction. Safe to call during event handling. */
static void ffplay_mark_for_destroy(VideoState *is)
{
    if (!is) return;
    SDL_LockMutex(g_instance_mutex);
    /* Check if already marked */
    for (int i = 0; i < g_pending_destroy_count; i++) {
        if (g_pending_destroy[i] == is) {
            SDL_UnlockMutex(g_instance_mutex);
            return;
        }
    }
    /* Add to pending list */
    if (g_pending_destroy_count < FFPLAY_MAX_INSTANCES) {
        g_pending_destroy[g_pending_destroy_count++] = is;
    }
    SDL_UnlockMutex(g_instance_mutex);
}

/* Check if an instance is marked for destruction */
static int ffplay_is_marked_for_destroy(VideoState *is)
{
    int result = 0;
    if (!is) return 1;
    SDL_LockMutex(g_instance_mutex);
    for (int i = 0; i < g_pending_destroy_count; i++) {
        if (g_pending_destroy[i] == is) {
            result = 1;
            break;
        }
    }
    SDL_UnlockMutex(g_instance_mutex);
    return result;
}

/* Forward declaration: used by ffplay_process_pending_destroys() */
static void do_exit(VideoState *is);

/* Actually destroy all pending instances. Must be called from main thread. */
static void ffplay_process_pending_destroys(void)
{
    SDL_LockMutex(g_instance_mutex);
    /* Take a snapshot of pending instances */
    int count = g_pending_destroy_count;
    VideoState *to_destroy[FFPLAY_MAX_INSTANCES];
    for (int i = 0; i < count; i++)
        to_destroy[i] = g_pending_destroy[i];
    g_pending_destroy_count = 0;
    SDL_UnlockMutex(g_instance_mutex);
    
    /* Destroy each pending instance */
    for (int i = 0; i < count; i++) {
        VideoState *is = to_destroy[i];
        if (!is) continue;
        ffplay_unregister_instance(is);
        do_exit(is);
    }
}

/* Forward declaration: used by sdl_event_thread() and ffplay_player_run_event_loop() */
static int dispatch_sdl_event(SDL_Event *event, VideoState *driver);

/* Helper to trigger loading callback */
/* Forward declaration - implemented after FFPlayer struct is defined */
static void trigger_loading_callback(VideoState *is, double progress, int buffer_kb);

/* Global SDL initialisation reference-count */
static int g_sdl_init_count = 0;
static SDL_mutex *g_sdl_init_mutex = NULL;   /* needs to be created before SDL_Init */

/* ============================================================ */
struct FFPlayGlobalParams {
    /* options specified by the user */
    AVInputFormat* file_iformat;
    char* input_filename;
    char* window_title;
    int default_width;
    int default_height;
    int screen_width;
    int screen_height;
    int screen_left;
    int screen_top;
    int audio_disable;
    int video_disable;
    int subtitle_disable;
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB];
    int seek_by_bytes;
    float seek_interval;
    int display_disable;
    int borderless;
    int alwaysontop;
    int startup_volume;
    int show_status;
    int av_sync_type;
    int64_t start_time;
    int64_t duration;
    int fast;
    int genpts;
    int lowres;
    int decoder_reorder_pts;
    int autoexit;
    int exit_on_keydown;
    int exit_on_mousedown;
    int loop;
    int framedrop;
    int infinite_buffer;
    ShowMode show_mode;
    const char* audio_codec_name;
    const char* subtitle_codec_name;
    const char* video_codec_name;
    double rdftspeed;
    int64_t cursor_last_shown;
    int cursor_hidden;
    const char** vfilters_list;
    int nb_vfilters;
    char* afilters;
    int autorotate;
    int find_stream_info;
    int filter_nbthreads;
    int enable_vulkan;
    char* vulkan_params;
    char* video_background;
    const char* hwaccel;

    /* current context */
    int is_full_screen;
    int64_t audio_callback_time;

    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_RendererInfo renderer_info;
    SDL_AudioDeviceID audio_dev;

    VkRenderer* vk_renderer;
    int hide_banner;
    AVDictionary *sws_dict;
    AVDictionary *swr_opts;
    AVDictionary *format_opts, *codec_opts;
    int dummy;
};

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

struct VideoState {
    SDL_Thread *read_tid;
    const AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;
    int64_t show_status_last_time;  /* moved from static local in video_refresh */

    /* --- loading/buffering state (network bad / stalled) --- */
    int loading;                   /* 1 when buffering/loading, 0 when playing */
    int64_t loading_start_time_us;  /* first time we entered loading */
    int64_t last_packet_time_us;    /* last time av_read_frame() succeeded */
    int64_t last_loading_cb_us;     /* throttle loading callbacks */

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
    struct AudioParams audio_filter_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    ShowMode show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;
    int xpos;
    double last_vis_time;
    RenderParams render_params;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    int width, height, xleft, ytop;
    int step;

    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
    FFPlayGlobalParams *global_params;
    FFPlayer *player;  /* back pointer to FFPlayer handle */
};

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
};

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *params = (FFPlayGlobalParams *)optctx;
    int ret = GROW_ARRAY(params->vfilters_list, params->nb_vfilters);
    if (ret < 0)
        return ret;

    params->vfilters_list[params->nb_vfilters - 1] = av_strdup(arg);
    if (!params->vfilters_list[params->nb_vfilters - 1])
        return AVERROR(ENOMEM);

    return 0;
}

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request)
       return -1;


    pkt1.pkt = pkt;
    pkt1.serial = q->serial;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    q->serial++;
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            if (serial)
                *serial = pkt1.serial;
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

static int decoder_decode_frame(FFPlayGlobalParams *global_params, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (global_params->decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!global_params->decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !d->pkt->data) {
                    d->packet_pending = 1;
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        } else {
            if (d->pkt->buf && !d->pkt->opaque_ref) {
                FrameData *fd;

                d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!d->pkt->opaque_ref)
                    return AVERROR(ENOMEM);
                fd = (FrameData*)d->pkt->opaque_ref->data;
                fd->pkt_pos = d->pkt->pos;
            }

            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

static inline void fill_rectangle(FFPlayGlobalParams *global_params, int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(global_params->renderer, &rect);
}

static int realloc_texture(FFPlayGlobalParams *global_params, SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(global_params->renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map); i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(FFPlayGlobalParams *global_params, SDL_Texture **tex, AVFrame *frame)
{
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(global_params, tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

static enum AVAlphaMode sdl_supported_alpha_modes[] = {
    AVALPHA_MODE_UNSPECIFIED,
    AVALPHA_MODE_STRAIGHT,
};

static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode); /* FIXME: no support for linear transfer */
#endif
}

static void draw_video_background(VideoState *is)
{
    const int tile_size = VIDEO_BACKGROUND_TILE_SIZE;
    SDL_Rect *rect = &is->render_params.target_rect;
    SDL_BlendMode blendMode;

    if (!SDL_GetTextureBlendMode(is->vid_texture, &blendMode) && blendMode == SDL_BLENDMODE_BLEND) {
        switch (is->render_params.video_background_type) {
        case VIDEO_BACKGROUND_TILES:
            SDL_SetRenderDrawColor(is->global_params->renderer, 237, 237, 237, 255);
            fill_rectangle(is->global_params,rect->x, rect->y, rect->w, rect->h);
            SDL_SetRenderDrawColor(is->global_params->renderer, 222, 222, 222, 255);
            for (int x = 0; x < rect->w; x += tile_size * 2)
                fill_rectangle(is->global_params,rect->x + x, rect->y, FFMIN(tile_size, rect->w - x), rect->h);
            for (int y = 0; y < rect->h; y += tile_size * 2)
                fill_rectangle(is->global_params,rect->x, rect->y + y, rect->w, FFMIN(tile_size, rect->h - y));
            SDL_SetRenderDrawColor(is->global_params->renderer, 237, 237, 237, 255);
            for (int y = 0; y < rect->h; y += tile_size * 2) {
                int h = FFMIN(tile_size, rect->h - y);
                for (int x = 0; x < rect->w; x += tile_size * 2)
                    fill_rectangle(is->global_params,x + rect->x, y + rect->y, FFMIN(tile_size, rect->w - x), h);
            }
            break;
        case VIDEO_BACKGROUND_COLOR: {
            const uint8_t *c = is->render_params.video_background_color;
            SDL_SetRenderDrawColor(is->global_params->renderer, c[0], c[1], c[2], c[3]);
            fill_rectangle(is->global_params,rect->x, rect->y, rect->w, rect->h);
            break;
        }
        case VIDEO_BACKGROUND_NONE:
            SDL_SetTextureBlendMode(is->vid_texture, SDL_BLENDMODE_NONE);
            break;
        }
    }
}

static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect *rect = &is->render_params.target_rect;

    vp = frame_queue_peek_last(&is->pictq);
    calculate_display_rect(rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);
    if (is->global_params->vk_renderer) {
        vk_renderer_display(is->global_params->vk_renderer, vp->frame, &is->render_params);
        return;
    }

    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(is->global_params, &is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                            0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }

    set_sdl_yuv_conversion_mode(vp->frame);

    if (!vp->uploaded) {
        if (upload_texture(is->global_params, &is->vid_texture, vp->frame) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    draw_video_background(is);
    SDL_RenderCopyEx(is->global_params->renderer, is->vid_texture, NULL, rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
    set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(is->global_params->renderer, is->sub_texture, NULL, rect);
#else
        int i;
        double xratio = (double)rect->w / (double)sp->width;
        double yratio = (double)rect->h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                               .y = rect.y + sub_rect->y * yratio,
                               .w = sub_rect->w * xratio,
                               .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

static void video_audio_display(VideoState *s)
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = s->audio_tgt.ch_layout.nb_channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used= s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation */
        if (s->global_params->audio_callback_time) {
            time_diff = av_gettime_relative() -s->global_params->audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start= x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    if (s->show_mode == SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(s->global_params->renderer, 255, 255, 255, 255);

        /* total height for one channel */
        h = s->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(s->global_params, s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(s->global_params->renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(s->global_params,s->xleft, y, s->width, 1);
        }
    } else {
        int err = 0;
        if (realloc_texture(s->global_params, &s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        if (s->xpos >= s->width)
            s->xpos = 0;
        nb_display_channels= FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            const float rdft_scale = 1.0;
            av_tx_uninit(&s->rdft);
            av_freep(&s->real_data);
            av_freep(&s->rdft_data);
            s->rdft_bits = rdft_bits;
            s->real_data = av_malloc_array(nb_freq, 4 *sizeof(*s->real_data));
            s->rdft_data = av_malloc_array(nb_freq + 1, 2 *sizeof(*s->rdft_data));
            err = av_tx_init(&s->rdft, &s->rdft_fn, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !s->rdft_data) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            float *data_in[2];
            AVComplexFloat *data[2];
            SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data_in[ch] = s->real_data + 2 * nb_freq * ch;
                data[ch] = s->rdft_data + nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data_in[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                s->rdft_fn(s->rdft, data[ch], data_in[ch], sizeof(float));
                data[ch][0].im = data[ch][nb_freq].re;
                data[ch][nb_freq].re = 0;
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            if (!SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
                    int b = (nb_display_channels == 2 ) ? sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                                        : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            SDL_RenderCopy(s->global_params->renderer, s->vis_texture, NULL, NULL);
        }
        if (!s->paused)
            s->xpos++;
    }
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudioDevice(is->global_params->audio_dev);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;

    /* Abort all packet queues to signal decoder threads */
    packet_queue_abort(&is->audioq);
    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->subtitleq);

    /* Signal all condition variables to wake up waiting threads immediately.
     * Without these signals, threads may be stuck in SDL_CondWaitTimeout()
     * and won't notice abort_request until timeout expires. */
    if (is->continue_read_thread)
        SDL_CondSignal(is->continue_read_thread);
    frame_queue_signal(&is->pictq);
    frame_queue_signal(&is->sampq);
    frame_queue_signal(&is->subpq);

    /* Wait for the read thread to finish.
     * Use SDL_WaitThread only if the thread is still valid.
     * If the thread already exited, SDL_WaitThread may cause double-free on macOS.
     */
    if (is->read_tid) {
        SDL_WaitThread(is->read_tid, NULL);
        is->read_tid = NULL;
    }

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);
    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    av_free(is);
}

static void do_exit(VideoState *is)
{
    /* Mark the instance for exit, but don't free any resources.
     * All resources are freed in ffplay_player_destroy() -> stream_close().
     * This function is called from event handlers (user pressed 'q', etc.)
     * and should only signal that the instance should stop.
     */
    if (is) {
        is->abort_request = 1;
    }
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static void set_default_window_size(FFPlayGlobalParams *gp, int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = gp->screen_width  ? gp->screen_width  : INT_MAX;
    int max_height = gp->screen_height ? gp->screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    gp->default_width  = rect.w;
    gp->default_height = rect.h;
}

static int video_open(VideoState *is)
{
    int w,h;

    w = is->global_params->screen_width ? is->global_params->screen_width : is->global_params->default_width;
    h = is->global_params->screen_height ? is->global_params->screen_height : is->global_params->default_height;

    if (!is->global_params->window_title)
        is->global_params->window_title = is->global_params->input_filename;
    SDL_SetWindowTitle(is->global_params->window, is->global_params->window_title);

    SDL_SetWindowSize(is->global_params->window, w, h);
    SDL_SetWindowPosition(is->global_params->window, is->global_params->screen_left, is->global_params->screen_top);
    if (is->global_params->is_full_screen)
        SDL_SetWindowFullscreen(is->global_params->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(is->global_params->window);

    is->width  = w;
    is->height = h;

    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is)
{
    if (!is->width)
        video_open(is);

    SDL_SetRenderDrawColor(is->global_params->renderer, 0, 0, 0, 255);
    SDL_RenderClear(is->global_params->renderer);
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
        video_audio_display(is);
    else if (is->video_st)
        video_image_display(is);
    SDL_RenderPresent(is->global_params->renderer);
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
   if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
       is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
       set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
       set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed;
       if (speed != 1.0)
           set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

static void update_volume(VideoState *is, int sign, double step)
{
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (!is->global_params->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + is->global_params->rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + is->global_params->rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && (is->global_params->framedrop>0 || (is->global_params->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial
                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
display:
        /* display picture */
        if (!is->global_params->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display(is);
    }
    is->force_refresh = 0;
    if (is->global_params->show_status) {
        AVBPrint buf;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!is->show_status_last_time || (cur_time - is->show_status_last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                      "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
                      get_master_clock(is),
                      (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                      av_diff,
                      is->frame_drops_early + is->frame_drops_late,
                      aqsize / 1024,
                      vqsize / 1024,
                      sqsize);

            if (is->global_params->show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(NULL, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, NULL);

            is->show_status_last_time = cur_time;
        }
    }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(is->global_params, vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(is->global_params, &is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (is->global_params->framedrop>0 || (is->global_params->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    const AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    for (i = 0; i < is->global_params->renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if (is->global_params->renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }

    while ((e = av_dict_iterate(is->global_params->sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);


    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = frame->format;
    par->time_base           = is->video_st->time_base;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
    par->alpha_mode          = frame->alpha_mode;
    par->frame_rate          = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;

    ret = avfilter_init_dict(filt_src, NULL);
    if (ret < 0)
        goto fail;

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                           "ffplay_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
        goto fail;
    if (!is->global_params->vk_renderer &&
        (ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_color_spaces),
                                AV_OPT_TYPE_INT, sdl_supported_color_spaces)) < 0)
        goto fail;

    if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_alpha_modes),
                                AV_OPT_TYPE_INT, sdl_supported_alpha_modes)) < 0)
        goto fail;

    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (is->global_params->autorotate) {
        double theta = 0.0;
        int32_t *displaymatrix = NULL;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd)
                displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0)
                INSERT_FILT("hflip", NULL);
            if (displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);
    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = is->global_params->filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(is->global_params->swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0)
        goto end;

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int audio_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(is->global_params, &is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                                   frame->format, frame->ch_layout.nb_channels)    ||
                    av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                    av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                    if (ret < 0)
                        goto the_end;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(is, is->global_params->afilters, 1)) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int video_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = is->global_params->filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, is->global_params->vfilters_list ? is->global_params->vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            FrameData *fd;

            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
 the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(is->global_params, &is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                            &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                            0, NULL);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;

    is->global_params->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, is->global_params->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    VideoState *is = opaque;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (!(is->global_params->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
        return -1;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

static int create_hwaccel(FFPlayGlobalParams *gp, AVBufferRef **device_ctx)
{
    enum AVHWDeviceType type;
    int ret;
    AVBufferRef *vk_dev;

    *device_ctx = NULL;

    if (!gp->hwaccel)
        return 0;

    type = av_hwdevice_find_type_by_name(gp->hwaccel);
    if (type == AV_HWDEVICE_TYPE_NONE)
        return AVERROR(ENOTSUP);

    if (!gp->vk_renderer) {
        av_log(NULL, AV_LOG_ERROR, "Vulkan renderer is not available\n");
        return AVERROR(ENOTSUP);
    }

    ret = vk_renderer_get_hw_dev(gp->vk_renderer, &vk_dev);
    if (ret < 0)
        return ret;

    ret = av_hwdevice_ctx_create_derived(device_ctx, type, vk_dev, 0);
    if (!ret)
        return 0;

    if (ret != AVERROR(ENOSYS))
        return ret;

    av_log(NULL, AV_LOG_WARNING, "Derive %s from vulkan not supported.\n", gp->hwaccel);
    ret = av_hwdevice_ctx_create(device_ctx, type, NULL, NULL, 0);
    return ret;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    int ret = 0;
    int stream_lowres = is->global_params->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    is->global_params->audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = is->global_params->subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    is->global_params->video_codec_name; break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (is->global_params->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = filter_codec_opts(is->global_params->codec_opts, avctx->codec_id, ic,
                            ic->streams[stream_index], codec, &opts, NULL);
    if (ret < 0)
        goto fail;

    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(is->global_params, &avctx->hw_device_ctx);
        if (ret < 0)
            goto fail;
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    ret = check_avoptions(opts);
    if (ret < 0)
        goto fail;

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
            if (ret < 0)
                goto fail;
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, is->global_params->afilters, 0)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            ret = av_buffersink_get_ch_layout(sink, &ch_layout);
            if (ret < 0)
                goto fail;
        }

        /* prepare audio output */
        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
            goto fail;
        if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
            goto out;
        SDL_PauseAudioDevice(is->global_params->audio_dev, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                 || !strncmp(s->url, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    int64_t stream_start_time;
    char metadata_description[96];
    int pkt_in_play_range = 0;
    const AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(is->global_params->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&is->global_params->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &is->global_params->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = err;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&is->global_params->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    remove_avoptions(&is->global_params->format_opts, is->global_params->codec_opts);

    ret = check_avoptions(is->global_params->format_opts);
    if (ret < 0)
        goto fail;
    is->ic = ic;

    if (is->global_params->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    if (is->global_params->find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic->nb_streams;

        err = setup_find_stream_info_opts(ic, is->global_params->codec_opts, &opts);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error setting up avformat_find_stream_info() options\n");
            ret = err;
            goto fail;
        }

        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (is->global_params->seek_by_bytes < 0)
        is->global_params->seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!is->global_params->window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        is->global_params->window_title = av_asprintf("%s - %s", t->value, is->global_params->input_filename);

    /* if seeking requested, we execute it */
    if (is->global_params->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = is->global_params->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (is->global_params->show_status) {
        fprintf(stderr, "\x1b[2K\r");
        av_dump_format(ic, 0, is->filename, 0);
    }

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && is->global_params->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, is->global_params->wanted_stream_spec[type]) > 0)
                st_index[type] = i;
        // Clear all pre-existing metadata update flags to avoid printing
        // initial metadata as update.
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (is->global_params->wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", is->global_params->wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }

    if (!is->global_params->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!is->global_params->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!is->global_params->video_disable && !is->global_params->subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = is->global_params->show_mode;

    /* init loading state */
    is->loading = 0;
    is->loading_start_time_us = 0;
    is->last_packet_time_us = av_gettime_relative();
    is->last_loading_cb_us = 0;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(is->global_params,codecpar->width, codecpar->height, sar);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (is->global_params->infinite_buffer < 0 && is->realtime)
        is->global_params->infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(is->global_params->input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audioq);
                if (is->subtitle_stream >= 0)
                    packet_queue_flush(&is->subtitleq);
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queues are full, no need to read more */
        if (is->global_params->infinite_buffer < 1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* This is "buffer-full" (demux is faster than decoders). Not a network-stall.
             * Do NOT report loading here, otherwise it will be very noisy.
             */

            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (is->global_params->loop != 1 && (!is->global_params->loop || --is->global_params->loop)) {
                stream_seek(is, is->global_params->start_time != AV_NOPTS_VALUE ? is->global_params->start_time : 0, 0, 0);
            } else if (is->global_params->autoexit) {
                /* Auto-exit at end: request instance stop.
                 * Note: we MUST NOT call do_exit() directly here because UI resources and
                 * instance registry must be cleaned up from the main thread.
                 */
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                break;
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            /* Network stall detection (debounced):
             * Enter loading only when:
             *   - not EOF
             *   - no packet has been received for LOADING_START_THRESHOLD_MS
             */
            if (ret != AVERROR_EOF && ic->pb && !avio_feof(ic->pb)) {
                const int64_t now_us = av_gettime_relative();
                const int64_t LOADING_START_THRESHOLD_US = 300000; /* 300ms */

                if (!is->loading && now_us - is->last_packet_time_us >= LOADING_START_THRESHOLD_US) {
                    is->loading = 1;
                    is->loading_start_time_us = now_us;
                    is->last_loading_cb_us = now_us;
                    trigger_loading_callback(is, 0.0,
                                             (is->audioq.size + is->videoq.size + is->subtitleq.size) / 1024);
                }
            }

            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                if (is->global_params->autoexit)
                    goto fail;
                else
                    break;
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            /* Successfully received a packet */
            const int64_t now_us = av_gettime_relative();
            is->last_packet_time_us = now_us;

            /* Exit loading only after we have buffered enough data to avoid immediate re-buffering.
             * Here we use packet counts as a lightweight heuristic.
             */
            if (is->loading) {
                const int NEED_AUDIO_PKTS = 8;
                const int NEED_VIDEO_PKTS = 8;
                int audio_ok = (is->audio_stream < 0) || (is->audioq.nb_packets >= NEED_AUDIO_PKTS);
                int video_ok = (is->video_stream < 0) || (is->videoq.nb_packets >= NEED_VIDEO_PKTS);

                if (audio_ok && video_ok) {
                    is->loading = 0;
                    is->last_loading_cb_us = now_us;
                    trigger_loading_callback(is, 1.0,
                                             (is->audioq.size + is->videoq.size + is->subtitleq.size) / 1024);
                }
            }

            is->eof = 0;
        }

        if (is->global_params->show_status && ic->streams[pkt->stream_index]->event_flags &
            AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
            fprintf(stderr, "\x1b[2K\r");
            snprintf(metadata_description,
                     sizeof(metadata_description),
                     "\r  New metadata for stream %d",
                     pkt->stream_index);
            dump_dictionary(NULL, ic->streams[pkt->stream_index]->metadata,
                               metadata_description, "    ", AV_LOG_INFO);
        }
        ic->streams[pkt->stream_index]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = is->global_params->duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(is->global_params->start_time != AV_NOPTS_VALUE ? is->global_params->start_time : 0) / 1000000
                <= ((double)is->global_params->duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    if (ret != 0) {
        /* Startup failed (e.g. avformat_open_input/avformat_find_stream_info failed).
         * This VideoState is not registered in the instance registry yet.
         * Posting SDL events here is unsafe because the main thread may concurrently
         * destroy the FFPlayer on start() error, leading to double-free.
         *
         * IMPORTANT: do_exit() also destroys global_params (gp) which is owned by FFPlayer.
         * In the FFPlayer API, global_params lifetime is managed by ffplay_player_destroy().
         * Therefore on startup failure we must ONLY cleanup VideoState-side allocations here.
         */
        /* Startup failed: only signal the read thread to exit.
         * Do NOT free resources here because ffplay_player_start() still holds `player->is`.
         * Cleanup is handled by ffplay_player_destroy() on the main thread.
         */
        is->abort_request = 1;
        SDL_DestroyMutex(wait_mutex);
        return 0;
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

static VideoState *stream_open(FFPlayGlobalParams *global_params, FFPlayer *player, const char *filename,
                               const AVInputFormat *iformat)
{
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    is->global_params = global_params;
    is->player = player;  /* Store the FFPlayer reference */
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    if (is->global_params->startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", is->global_params->startup_volume);
    if (is->global_params->startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", is->global_params->startup_volume);
    if (is->global_params->video_background) {
        if (!strcmp(is->global_params->video_background, "none")) {
            is->render_params.video_background_type = VIDEO_BACKGROUND_NONE;
        } else if (strcmp(is->global_params->video_background, "tiles")) {
            if (av_parse_color(is->render_params.video_background_color, is->global_params->video_background, -1, NULL) >= 0)
                is->render_params.video_background_type = VIDEO_BACKGROUND_COLOR;
            else
                goto fail;
        }
    }
    is->global_params->startup_volume = av_clip(is->global_params->startup_volume, 0, 100);
    is->global_params->startup_volume = av_clip(SDL_MIX_MAXVOLUME * is->global_params->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = is->global_params->startup_volume;
    is->muted = 0;
    is->av_sync_type = is->global_params->av_sync_type;
    is->read_tid     = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        /* Don't free anything here - let ffplay_player_destroy handle cleanup.
         * Set abort_request so stream_close knows to skip waiting for read_thread. */
        is->abort_request = 1;
        return is;  /* Return is so ffplay_player_destroy can clean up properly */
    }
    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
 the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}


static void toggle_full_screen(VideoState *is)
{
    is->global_params->is_full_screen = !is->global_params->is_full_screen;
    SDL_SetWindowFullscreen(is->global_params->window, is->global_params->is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *is)
{
    int next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = next;
    }
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!is->global_params->cursor_hidden && av_gettime_relative() - is->global_params->cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            is->global_params->cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        /* refresh all registered instances, not just `is` */
        SDL_LockMutex(g_instance_mutex);
        int cnt = g_instance_count;
        VideoState *snaps[FFPLAY_MAX_INSTANCES];
        for (int i = 0; i < cnt; i++)
            snaps[i] = g_instances[i].is;
        SDL_UnlockMutex(g_instance_mutex);
        for (int i = 0; i < cnt; i++) {
            if (snaps[i]->show_mode != SHOW_MODE_NONE &&
                (!snaps[i]->paused || snaps[i]->force_refresh))
                video_refresh(snaps[i], &remaining_time);
        }
        SDL_PumpEvents();
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;
    /* Resolve the VideoState for this particular event.
     * For window events we look up by windowID so that multi-instance
     * setups dispatch to the correct player.  Key / mouse events that
     * carry a windowID are handled the same way.
     * Events with no window association fall back to cur_stream. */
    VideoState *is;

    for (;;) {
        double x;
        refresh_loop_wait_event(cur_stream, &event);

        /* ---- resolve the target VideoState from the event windowID ---- */
        switch (event.type) {
        case SDL_WINDOWEVENT:
            is = ffplay_find_instance_by_window(event.window.windowID);
            if (!is) is = cur_stream;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            is = ffplay_find_instance_by_window(event.key.windowID);
            if (!is) is = cur_stream;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            is = ffplay_find_instance_by_window(event.button.windowID);
            if (!is) is = cur_stream;
            break;
        case SDL_MOUSEMOTION:
            is = ffplay_find_instance_by_window(event.motion.windowID);
            if (!is) is = cur_stream;
            break;
        case FF_QUIT_EVENT:
            /* data1 contains the VideoState that requested quit */
            is = (VideoState *)event.user.data1;
            if (!is) is = cur_stream;
            break;
        default:
            is = cur_stream;
            break;
        }

        switch (event.type) {
        case SDL_KEYDOWN:
            if (is->global_params->exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                /* Save whether this is the current stream before destroying */
                int was_cur_stream = (is == cur_stream);
                ffplay_unregister_instance(is);
                do_exit(is);
                /* is is now invalid - do not access it */
                if (ffplay_instance_count() == 0)
                    return;
                /* The stopped stream was cur_stream – pick any remaining one */
                if (was_cur_stream) {
                    SDL_LockMutex(g_instance_mutex);
                    cur_stream = (g_instance_count > 0) ? g_instances[0].is : NULL;
                    SDL_UnlockMutex(g_instance_mutex);
                    if (!cur_stream)
                        return;
                }
                break;
            }
            // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
            if (!is->width)
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen(is);
                is->force_refresh = 1;
                break;
            case SDLK_p:
            case SDLK_SPACE:
                toggle_pause(is);
                break;
            case SDLK_m:
                toggle_mute(is);
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                update_volume(is, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(is, -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                step_to_next_frame(is);
                break;
            case SDLK_a:
                stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (is->show_mode == SHOW_MODE_VIDEO && is->vfilter_idx < is->global_params->nb_vfilters - 1) {
                    if (++is->vfilter_idx >= is->global_params->nb_vfilters)
                        is->vfilter_idx = 0;
                } else {
                    is->vfilter_idx = 0;
                    toggle_audio_display(is);
                }
                break;
            case SDLK_PAGEUP:
                if (is->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(is, 1);
                break;
            case SDLK_PAGEDOWN:
                if (is->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(is, -1);
                break;
            case SDLK_LEFT:
                incr = is->global_params->seek_interval ? -is->global_params->seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = is->global_params->seek_interval ? is->global_params->seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                    if (is->global_params->seek_by_bytes) {
                        pos = -1;
                        if (pos < 0 && is->video_stream >= 0)
                            pos = frame_queue_last_pos(&is->pictq);
                        if (pos < 0 && is->audio_stream >= 0)
                            pos = frame_queue_last_pos(&is->sampq);
                        if (pos < 0)
                            pos = avio_tell(is->ic->pb);
                        if (is->ic->bit_rate)
                            incr *= is->ic->bit_rate / 8.0;
                        else
                            incr *= 180000.0;
                        pos += incr;
                        stream_seek(is, pos, incr, 1);
                    } else {
                        pos = get_master_clock(is);
                        if (isnan(pos))
                            pos = (double)is->seek_pos / AV_TIME_BASE;
                        pos += incr;
                        if (is->ic->start_time != AV_NOPTS_VALUE && pos < is->ic->start_time / (double)AV_TIME_BASE)
                            pos = is->ic->start_time / (double)AV_TIME_BASE;
                        stream_seek(is, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                    }
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (is->global_params->exit_on_mousedown) {
                ffplay_unregister_instance(is);
                do_exit(is);
                if (ffplay_instance_count() == 0)
                    return;
                if (is == cur_stream) {
                    SDL_LockMutex(g_instance_mutex);
                    cur_stream = (g_instance_count > 0) ? g_instances[0].is : NULL;
                    SDL_UnlockMutex(g_instance_mutex);
                    if (!cur_stream)
                        return;
                }
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    toggle_full_screen(is);
                    is->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:
            if (is->global_params->cursor_hidden) {
                SDL_ShowCursor(1);
                is->global_params->cursor_hidden = 0;
            }
            is->global_params->cursor_last_shown = av_gettime_relative();
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))
                    break;
                x = event.motion.x;
            }
                if (is->global_params->seek_by_bytes || is->ic->duration <= 0) {
                    uint64_t size =  avio_size(is->ic->pb);
                    stream_seek(is, size*x/is->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns  = is->ic->duration / 1000000LL;
                    thh  = tns / 3600;
                    tmm  = (tns % 3600) / 60;
                    tss  = (tns % 60);
                    frac = x / is->width;
                    ns   = frac * tns;
                    hh   = ns / 3600;
                    mm   = (ns % 3600) / 60;
                    ss   = (ns % 60);
                    av_log(NULL, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                            hh, mm, ss, thh, tmm, tss);
                    ts = frac * is->ic->duration;
                    if (is->ic->start_time != AV_NOPTS_VALUE)
                        ts += is->ic->start_time;
                    stream_seek(is, ts, 0, 0);
                }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    is->global_params->screen_width  = is->width  = event.window.data1;
                    is->global_params->screen_height = is->height = event.window.data2;
                    if (is->vis_texture) {
                        SDL_DestroyTexture(is->vis_texture);
                        is->vis_texture = NULL;
                    }
                    if (is->global_params->vk_renderer)
                        vk_renderer_resize(is->global_params->vk_renderer, is->global_params->screen_width, is->global_params->screen_height);
                case SDL_WINDOWEVENT_EXPOSED:
                    is->force_refresh = 1;
            }
            break;
        case SDL_QUIT:
            /* SDL_QUIT closes all instances */
            {
                int count;
                SDL_LockMutex(g_instance_mutex);
                count = g_instance_count;
                SDL_UnlockMutex(g_instance_mutex);
                /* Shut down all registered instances */
                while (ffplay_instance_count() > 0) {
                    VideoState *victim;
                    SDL_LockMutex(g_instance_mutex);
                    victim = (g_instance_count > 0) ? g_instances[0].is : NULL;
                    SDL_UnlockMutex(g_instance_mutex);
                    if (!victim) break;
                    ffplay_unregister_instance(victim);
                    do_exit(victim);
                }
                return;
            }
        case FF_QUIT_EVENT:
            {
                /* Save whether this is the current stream before destroying */
                int was_cur_stream = (is == cur_stream);
                ffplay_unregister_instance(is);
                do_exit(is);
                /* is is now invalid - do not access it */
                (void)was_cur_stream;  /* suppress unused warning if needed */
                if (ffplay_instance_count() == 0)
                    return;
                /* If the closed stream was the one we are refreshing, switch to another */
                if (was_cur_stream) {
                    SDL_LockMutex(g_instance_mutex);
                    cur_stream = (g_instance_count > 0) ? g_instances[0].is : NULL;
                    SDL_UnlockMutex(g_instance_mutex);
                    if (!cur_stream)
                        return;
                }
            }
            break;
        default:
            break;
        }
    }
}

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;
    global_params->screen_width = num;
    global_params->screen_width = num;
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    global_params->screen_height = num;
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
    global_params->file_iformat = av_find_input_format(arg);
    if (!global_params->file_iformat) {
        av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_sync(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
    if (!strcmp(arg, "audio"))
        global_params->av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        global_params->av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        global_params->av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
    global_params->show_mode = !strcmp(arg, "video") ? SHOW_MODE_VIDEO :
                !strcmp(arg, "waves") ? SHOW_MODE_WAVES :
                !strcmp(arg, "rdft" ) ? SHOW_MODE_RDFT  : SHOW_MODE_NONE;

    if (global_params->show_mode == SHOW_MODE_NONE) {
        double num;
        int ret = parse_number(opt, arg, OPT_TYPE_INT, 0, SHOW_MODE_NB-1, &num);
        if (ret < 0)
            return ret;
        global_params->show_mode = num;
    }
    return 0;
}

static int opt_input_file(void *optctx, const char *filename)
{
    FFPlayGlobalParams *global_params = optctx;
    if (global_params->input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
                filename, global_params->input_filename);
        return AVERROR(EINVAL);
    }
    if (!strcmp(filename, "-"))
        filename = "fd:";
    global_params->input_filename = av_strdup(filename);
    if (!global_params->input_filename)
        return AVERROR(ENOMEM);

    return 0;
}

static int opt_codec(void *optctx, const char *opt, const char *arg)
{
    FFPlayGlobalParams *global_params = optctx;
   const char *spec = strchr(opt, ':');
   const char **name;
   if (!spec) {
       av_log(NULL, AV_LOG_ERROR,
              "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
       return AVERROR(EINVAL);
   }
   spec++;

   switch (spec[0]) {
   case 'a' : name = &global_params->audio_codec_name;    break;
   case 's' : name = &global_params->subtitle_codec_name; break;
   case 'v' : name = &global_params->video_codec_name;    break;
   default:
       av_log(NULL, AV_LOG_ERROR,
              "Invalid media specifier '%s' in option '%s'\n", spec, opt);
       return AVERROR(EINVAL);
   }

   av_freep(name);
   *name = av_strdup(arg);
   return *name ? 0 : AVERROR(ENOMEM);
}


#define OFFSET(x) offsetof(FFPlayGlobalParams, x)
static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "x",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
    { "y",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
    { "fs",                 OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(is_full_screen) }, "force full screen" },
    { "an",                 OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(audio_disable) }, "disable audio" },
    { "vn",                 OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(video_disable) }, "disable video" },
    { "sn",                 OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET ,{ .off       = OFFSET(subtitle_disable) }, "disable subtitling" },
    { "ast",                OPT_TYPE_STRING, OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(wanted_stream_spec[AVMEDIA_TYPE_AUDIO]) }, "select desired audio stream", "stream_specifier" },
    { "vst",                OPT_TYPE_STRING, OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(wanted_stream_spec[AVMEDIA_TYPE_VIDEO]) }, "select desired video stream", "stream_specifier" },
    { "sst",                OPT_TYPE_STRING, OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]) }, "select desired subtitle stream", "stream_specifier" },
    { "ss",                 OPT_TYPE_TIME,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(start_time) }, "seek to a given position in seconds", "pos" },
    { "t",                  OPT_TYPE_TIME,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(duration) }, "play  \"duration\" seconds of audio/video", "duration" },
    { "bytes",              OPT_TYPE_INT,             OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(seek_by_bytes) }, "seek by bytes 0=off 1=on -1=auto", "val" },
    { "seek_interval",      OPT_TYPE_FLOAT,           OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(seek_interval) }, "set seek interval for left/right keys, in seconds", "seconds" },
    { "nodisp",             OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(display_disable) }, "disable graphical display" },
    { "noborder",           OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(borderless) }, "borderless window" },
    { "alwaysontop",        OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(alwaysontop) }, "window always on top" },
    { "volume",             OPT_TYPE_INT,             OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(startup_volume)}, "set startup volume 0=min 100=max", "volume" },
    { "f",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_format }, "force format", "fmt" },
    { "stats",              OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(show_status) }, "show status", "" },
    { "fast",               OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(fast) }, "non spec compliant optimizations", "" },
    { "genpts",             OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(genpts) }, "generate pts", "" },
    { "drp",                OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(decoder_reorder_pts) }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres",             OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(lowres) }, "", "" },
    { "sync",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit",           OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(autoexit) }, "exit at the end", "" },
    { "exitonkeydown",      OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(exit_on_keydown) }, "exit on key down", "" },
    { "exitonmousedown",    OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(exit_on_mousedown) }, "exit on mouse down", "" },
    { "loop",               OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(loop) }, "set number of times the playback shall be looped", "loop count" },
    { "framedrop",          OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(framedrop) }, "drop frames when cpu is too slow", "" },
    { "infbuf",             OPT_TYPE_BOOL,   OPT_EXPERT, { .off       = OFFSET(infinite_buffer) }, "don't limit the input buffer size (useful with realtime streams)", "" },
    { "window_title",       OPT_TYPE_STRING,          OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(window_title) }, "set window title", "window title" },
    { "left",               OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(screen_left) }, "set the x position for the left of the window", "x pos" },
    { "top",                OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(screen_top) }, "set the y position for the top of the window", "y pos" },
    { "vf",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af",                 OPT_TYPE_STRING,          OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(afilters) }, "set audio filters", "filter_graph" },
    { "rdftspeed",          OPT_TYPE_INT, OPT_AUDIO | OPT_EXPERT, { .off       = OFFSET(rdftspeed) }, "rdft speed", "msecs" },
    { "showmode",           OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "i",                  OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(dummy)}, "read specified file", "input_file"},
    { "codec",              OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
    { "acodec",             OPT_TYPE_STRING, OPT_EXPERT, {    .off       = OFFSET(audio_codec_name) }, "force audio decoder",    "decoder_name" },
    { "scodec",             OPT_TYPE_STRING, OPT_EXPERT, { .off       = OFFSET(subtitle_codec_name) }, "force subtitle decoder", "decoder_name" },
    { "vcodec",             OPT_TYPE_STRING, OPT_EXPERT, {    .off       = OFFSET(video_codec_name) }, "force video decoder",    "decoder_name" },
    { "autorotate",         OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(autorotate) }, "automatically rotate video", "" },
    { "find_stream_info",   OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT, { .off       = OFFSET(find_stream_info) },
        "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads",     OPT_TYPE_INT,    OPT_EXPERT, { .off       = OFFSET(filter_nbthreads) }, "number of filter threads per graph" },
    { "enable_vulkan",      OPT_TYPE_BOOL,            OPT_EXPERT | OPT_FLAG_OFFSET, { .off       = OFFSET(enable_vulkan) }, "enable vulkan renderer" },
    { "vulkan_params",      OPT_TYPE_STRING, OPT_EXPERT, { .off       = OFFSET(vulkan_params) }, "vulkan configuration using a list of key=value pairs separated by ':'" },
    { "video_bg",           OPT_TYPE_STRING, OPT_EXPERT, { .off       = OFFSET(video_background) }, "set video background for transparent videos" },
    { "hwaccel",            OPT_TYPE_STRING, OPT_EXPERT, { .off       = OFFSET(hwaccel) }, "use HW accelerated decoding" },
    { NULL, },
};

static void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
           );
}

/* Called from the main */
int ffplay(int argc, const char **argv)
{
    int flags, ret;
    VideoState *is;

    init_dynload();
    FFPlayGlobalParams *global_params = av_mallocz(sizeof(FFPlayGlobalParams));
    if (!global_params) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate global params\n");
        return AVERROR(ENOMEM);
    }
    global_params->default_width = 640;
    global_params->default_height = 480;
    global_params->screen_left = SDL_WINDOWPOS_CENTERED;
    global_params->screen_top = SDL_WINDOWPOS_CENTERED;
    global_params->seek_by_bytes = -1;
    global_params->seek_interval = 10;
    global_params->startup_volume = 100;
    global_params->show_status = -1;
    global_params->av_sync_type = AV_SYNC_AUDIO_MASTER;
    global_params->start_time = AV_NOPTS_VALUE;
    global_params->duration = AV_NOPTS_VALUE;
    global_params->decoder_reorder_pts = -1;
    global_params->loop = 1;
    global_params->framedrop = -1;
    global_params->infinite_buffer = -1;
    global_params->show_mode = SHOW_MODE_NONE;
    global_params->autorotate = 1;
    global_params->find_stream_info = 1;
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options, &global_params);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
   

    show_banner(argc, argv, options, global_params);
    
    ret = parse_options(global_params, argc, argv, options, opt_input_file);
    if (ret < 0) {
        av_freep(&global_params);
        return ret == AVERROR_EXIT ? 0 : ret;
    }

    if (!global_params->input_filename) {
        show_usage();
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        av_freep(&global_params);
        return AVERROR(EINVAL);
    }

    if (global_params->display_disable) {
        global_params->video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (global_params->audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (global_params->display_disable)
        flags &= ~SDL_INIT_VIDEO;

    /* ----------------------------------------------------------------
     * SDL reference-counting: only call SDL_Init() for the first
     * instance; subsequent instances reuse the existing SDL context.
     * ---------------------------------------------------------------- */
    if (!g_sdl_init_mutex)
        g_sdl_init_mutex = SDL_CreateMutex();   /* created before any SDL call */

    SDL_LockMutex(g_sdl_init_mutex);
    if (g_sdl_init_count == 0) {
        if (SDL_Init(flags)) {
            SDL_UnlockMutex(g_sdl_init_mutex);
            av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
            av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
            av_freep(&global_params);
            return AVERROR_UNKNOWN;
        }
        SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
        SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
        ffplay_registry_init();   /* create registry mutex after SDL is up */
    }
    g_sdl_init_count++;
    SDL_UnlockMutex(g_sdl_init_mutex);

    if (!global_params->display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (global_params->alwaysontop)
#if SDL_VERSION_ATLEAST(2,0,5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (global_params->borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
        if (global_params->hwaccel && !global_params->enable_vulkan) {
            av_log(NULL, AV_LOG_INFO, "Enable vulkan renderer to support hwaccel %s\n", global_params->hwaccel);
            global_params->enable_vulkan = 1;
        }
        if (global_params->enable_vulkan) {
            global_params->vk_renderer = vk_get_renderer();
            if (global_params->vk_renderer) {
#if SDL_VERSION_ATLEAST(2, 0, 6)
                flags |= SDL_WINDOW_VULKAN;
#endif
            } else {
                av_log(NULL, AV_LOG_WARNING, "Doesn't support vulkan renderer, fallback to SDL renderer\n");
                global_params->enable_vulkan = 0;
            }
        }
        global_params->window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, global_params->default_width, global_params->default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (!global_params->window) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
            do_exit(NULL);
            SDL_LockMutex(g_sdl_init_mutex);
            if (--g_sdl_init_count == 0) {
                SDL_UnlockMutex(g_sdl_init_mutex);
                ffplay_registry_destroy();
                SDL_Quit();
            } else {
                SDL_UnlockMutex(g_sdl_init_mutex);
            }
            av_freep(&global_params);
            return AVERROR_UNKNOWN;
        }

        if (global_params->vk_renderer) {
            AVDictionary *dict = NULL;

            if (global_params->vulkan_params) {
                int ret = av_dict_parse_string(&dict, global_params->vulkan_params, "=", ":", 0);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Failed to parse, %s\n", global_params->vulkan_params);
                    do_exit(NULL);
                    av_freep(&global_params);
                    return ret;
                }
            }
            ret = vk_renderer_create(global_params->vk_renderer, global_params->window, dict);
            av_dict_free(&dict);
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL, "Failed to create vulkan renderer, %s\n", av_err2str(ret));
                do_exit(NULL);
                av_freep(&global_params);
                return ret;
            }
        } else {
            global_params->renderer = SDL_CreateRenderer(global_params->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!global_params->renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                global_params->renderer = SDL_CreateRenderer(global_params->window, -1, 0);
            }
            if (global_params->renderer) {
                if (!SDL_GetRendererInfo(global_params->renderer, &global_params->renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", global_params->renderer_info.name);
            }
            if (!global_params->renderer || !global_params->renderer_info.num_texture_formats) {
                av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
                do_exit(NULL);
                av_freep(&global_params);
                return AVERROR_UNKNOWN;
            }
        }
    }

    is = stream_open(global_params, NULL, global_params->input_filename, global_params->file_iformat);
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
        av_freep(&global_params);

        SDL_LockMutex(g_sdl_init_mutex);
        if (--g_sdl_init_count == 0) {
            SDL_UnlockMutex(g_sdl_init_mutex);
            ffplay_registry_destroy();
            SDL_Quit();
        } else {
            SDL_UnlockMutex(g_sdl_init_mutex);
        }
        return AVERROR_UNKNOWN;
    }

    /* Register this instance so the shared event_loop can route events to it */
    if (global_params->window)
        ffplay_register_instance(SDL_GetWindowID(global_params->window), is);

    event_loop(is);

    /* event_loop returns when this (or all) instance(s) quit.
     * Release the SDL reference held by this call. */
    SDL_LockMutex(g_sdl_init_mutex);
    if (--g_sdl_init_count == 0) {
        SDL_UnlockMutex(g_sdl_init_mutex);
        avformat_network_deinit();
        ffplay_registry_destroy();
        SDL_Quit();
    } else {
        SDL_UnlockMutex(g_sdl_init_mutex);
    }

    return 0;
}

/* ============================================================
 *  FFPlayer — non-blocking multi-instance player API
 *
 *  Design:
 *    • ffplay_player_create()  allocates + initialises SDL (ref-counted)
 *                               and creates the SDL window/renderer.
 *                               Returns an opaque FFPlayer*.
 *    • ffplay_player_start()   opens the stream, starts decode threads
 *                               and the shared SDL event thread.
 *                               Returns immediately (non-blocking).
 *    • ffplay_player_stop()    signals the stream to stop gracefully.
 *    • ffplay_player_pause()   toggles pause.
 *    • ffplay_player_seek()    seeks to position_s (seconds from start).
 *    • ffplay_player_destroy() waits for threads, frees all resources,
 *                               decrements SDL ref-count.
 *
 *  Thread model:
 *    A single global SDL event thread (g_event_thread) handles ALL
 *    registered player instances.  It is created with the first
 *    ffplay_player_start() call and stays alive until the last
 *    ffplay_player_destroy() that leaves no more instances.
 *    
 *  macOS threading constraints:
 *    On macOS, all Cocoa UI operations (including SDL_PumpEvents,
 *    SDL_ShowCursor, window creation/destruction) must occur on the
 *    main thread.  We therefore use a two-level approach:
 *      1. On platforms requiring main-thread UI, we require the caller
 *         to run the SDL event loop via ffplay_player_run_event_loop().
 *      2. On other platforms, we automatically start an SDL event thread.
 * ============================================================ */

#define FF_PLAYER_START_EVENT   (SDL_USEREVENT + 3)
#define FF_PLAYER_STOP_EVENT    (SDL_USEREVENT + 4)
#define FF_PLAYER_RUN_EVENT_LOOP_EVENT (SDL_USEREVENT + 5)

/* Flag indicating whether we should run SDL event loop on main thread */
#ifdef __APPLE__
#define REQUIRE_MAIN_THREAD_UI 1
#else
#define REQUIRE_MAIN_THREAD_UI 0
#endif

/* Internal state for the global SDL event thread */
static SDL_Thread  *g_event_thread      = NULL;
static SDL_mutex   *g_event_thread_lock = NULL;  /* serialise create/destroy */
static int          g_event_thread_quit = 0;     /* set to 1 to stop thread */
static SDL_cond    *g_event_thread_cond = NULL;  /* used to wait for stop */

/* Main thread ID for platforms requiring main-thread UI */
static SDL_threadID g_main_thread_id = 0;

/* Helper function to check if we're on the main thread (macOS only) */
static int is_on_main_thread(void)
{
#if REQUIRE_MAIN_THREAD_UI
    return g_main_thread_id != 0 && SDL_ThreadID() == g_main_thread_id;
#else
    return 1; /* On other platforms, any thread is fine */
#endif
}

/* The global event thread – handles events for every registered instance.
 * Used only on non-macOS platforms; on macOS the caller must drive the loop
 * via ffplay_player_run_event_loop() on the main thread instead. */
static int sdl_event_thread(void *arg)
{
    (void)arg;

#if REQUIRE_MAIN_THREAD_UI
    /* Should never be reached on macOS – ensure_event_thread_locked won't start us */
    av_log(NULL, AV_LOG_WARNING,
           "sdl_event_thread started on macOS – this should not happen. "
           "Use ffplay_player_run_event_loop() on the main thread.\n");
    return 0;
#endif

    SDL_Event event;

    while (1) {
        /* Check quit flag */
        SDL_LockMutex(g_event_thread_lock);
        int quit = g_event_thread_quit;
        SDL_UnlockMutex(g_event_thread_lock);
        if (quit) break;

        /* Pick an arbitrary registered instance to drive the wait/refresh cycle.
         * If no instances are registered we briefly sleep to avoid busy-spin. */
        SDL_LockMutex(g_instance_mutex);
        VideoState *driver = (g_instance_count > 0) ? g_instances[0].is : NULL;
        SDL_UnlockMutex(g_instance_mutex);

        if (!driver) {
            av_usleep(10000);
            continue;
        }

        /* Wait for next event (or next frame deadline) */
        refresh_loop_wait_event(driver, &event);

        /* Dispatch event to the correct instance */
        if (dispatch_sdl_event(&event, driver) != 0) {
            /* SDL_QUIT – signal the thread to exit */
            SDL_LockMutex(g_event_thread_lock);
            g_event_thread_quit = 1;
            SDL_CondBroadcast(g_event_thread_cond);
            SDL_UnlockMutex(g_event_thread_lock);
            break;
        }
    }

    /* Signal anyone waiting that the thread has exited */
    SDL_LockMutex(g_event_thread_lock);
    g_event_thread = NULL;
    SDL_CondBroadcast(g_event_thread_cond);
    SDL_UnlockMutex(g_event_thread_lock);
    return 0;
}

/* Start the global event thread if it isn't running yet.
 * Must be called with g_event_thread_lock held.
 * On macOS, we don't start a separate event thread; instead,
 * the caller must run the event loop on the main thread. */
static int ensure_event_thread_locked(void)
{
    if (g_event_thread)
        return 0;   /* already running */
    
#if REQUIRE_MAIN_THREAD_UI
    /* On macOS, we don't create a separate event thread.
     * The caller must run the event loop via ffplay_player_run_event_loop(). */
    av_log(NULL, AV_LOG_INFO, "macOS detected: SDL event loop must run on main thread\n");
    return 0;
#else
    g_event_thread_quit = 0;
    g_event_thread = SDL_CreateThread(sdl_event_thread, "ffplay_event", NULL);
    if (!g_event_thread) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed to create SDL event thread: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
#endif
}

/* ============================================================
 *  Public FFPlayer opaque handle + API
 * ============================================================ */

struct FFPlayer {
    FFPlayGlobalParams  *params;      /* per-instance configuration     */
    VideoState          *is;          /* per-instance playback state     */
    int                  started;     /* ffplay_player_start() called?   */
    int                  destroyed;   /* set to 1 after destroy, prevent double-free */
    ffplay_loading_callback loading_callback; /* callback for network loading */
    void                *loading_callback_userdata; /* user data for callback */
};

/* Implementation of trigger_loading_callback - must be after FFPlayer struct definition */
static void trigger_loading_callback(VideoState *is, double progress, int buffer_kb)
{
    if (!is || !is->player || !is->player->loading_callback)
        return;
    
    /* Call the user's callback */
    is->player->loading_callback(is->player, progress, buffer_kb, 
                                is->player->loading_callback_userdata);
}

/* Shared once-init of SDL + support structures.
 * Safe to call from multiple threads; uses g_sdl_init_mutex. */
static int player_sdl_init(int audio_disable, int video_disable)
{
    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)  flags &= ~SDL_INIT_AUDIO;
    if (video_disable)  flags &= ~SDL_INIT_VIDEO;

    if (!g_sdl_init_mutex) {
        /* Bootstrap: create the raw mutex before any SDL call.
         * On the very first call this is single-threaded so it's safe. */
        g_sdl_init_mutex = SDL_CreateMutex();
        if (!g_sdl_init_mutex) return AVERROR(ENOMEM);
    }
    if (!g_event_thread_lock) {
        g_event_thread_lock = SDL_CreateMutex();
        if (!g_event_thread_lock) return AVERROR(ENOMEM);
    }
    if (!g_event_thread_cond) {
        g_event_thread_cond = SDL_CreateCond();
        if (!g_event_thread_cond) return AVERROR(ENOMEM);
    }

    SDL_LockMutex(g_sdl_init_mutex);
    if (g_sdl_init_count == 0) {
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);

        if (SDL_Init(flags)) {
            SDL_UnlockMutex(g_sdl_init_mutex);
            av_log(NULL, AV_LOG_FATAL,
                   "Could not initialize SDL - %s\n(Did you set DISPLAY?)\n",
                   SDL_GetError());
            return AVERROR_UNKNOWN;
        }
        SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
        SDL_EventState(SDL_USEREVENT,  SDL_IGNORE);
        ffplay_registry_init();
        
        /* Record the main thread ID on first SDL initialization */
        if (REQUIRE_MAIN_THREAD_UI && g_main_thread_id == 0) {
            g_main_thread_id = SDL_ThreadID();
            av_log(NULL, AV_LOG_INFO, "SDL initialized on thread %lu (main thread)\n",
                   (unsigned long)g_main_thread_id);
        }
    }
    g_sdl_init_count++;
    SDL_UnlockMutex(g_sdl_init_mutex);
    return 0;
}

/* Decrement SDL ref-count; tear down SDL when last user leaves. */
static void player_sdl_unref(void)
{
    SDL_LockMutex(g_sdl_init_mutex);
    int remaining = --g_sdl_init_count;
    SDL_UnlockMutex(g_sdl_init_mutex);

    if (remaining == 0) {
        avformat_network_deinit();
        ffplay_registry_destroy();

        if (g_event_thread_lock) {
            SDL_LockMutex(g_event_thread_lock);
            g_event_thread_quit = 1;
            SDL_CondBroadcast(g_event_thread_cond);
            /* Wait for the event thread to finish */
            while (g_event_thread)
                SDL_CondWait(g_event_thread_cond, g_event_thread_lock);
            SDL_UnlockMutex(g_event_thread_lock);

            SDL_DestroyMutex(g_event_thread_lock);
            g_event_thread_lock = NULL;
            SDL_DestroyCond(g_event_thread_cond);
            g_event_thread_cond = NULL;
        }

        SDL_Quit();

        if (g_sdl_init_mutex) {
            SDL_DestroyMutex(g_sdl_init_mutex);
            g_sdl_init_mutex = NULL;
        }
    }
}

/* Helper: allocate and populate FFPlayGlobalParams with defaults */
static FFPlayGlobalParams *alloc_default_params(void)
{
    FFPlayGlobalParams *p = av_mallocz(sizeof(FFPlayGlobalParams));
    if (!p) return NULL;
    p->default_width       = 640;
    p->default_height      = 480;
    p->screen_left         = SDL_WINDOWPOS_CENTERED;
    p->screen_top          = SDL_WINDOWPOS_CENTERED;
    p->seek_by_bytes       = -1;
    p->seek_interval       = 10;
    p->startup_volume      = 100;
    p->show_status         = -1;
    p->av_sync_type        = AV_SYNC_AUDIO_MASTER;
    p->start_time          = AV_NOPTS_VALUE;
    p->duration            = AV_NOPTS_VALUE;
    p->decoder_reorder_pts = -1;
    p->loop                = 1;
    p->framedrop           = -1;
    p->infinite_buffer     = -1;
    p->show_mode           = SHOW_MODE_NONE;
    p->autorotate          = 1;
    p->find_stream_info    = 1;
    p->rdftspeed           = 0.02;
    return p;
}

/* Helper: create SDL window + renderer for the given params */
static int player_create_window(FFPlayGlobalParams *p)
{
    if (p->display_disable)
        return 0;   /* headless mode – no window needed */

    int win_flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
    if (p->alwaysontop) {
#if SDL_VERSION_ATLEAST(2,0,5)
        win_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#endif
    }
    if (p->borderless)
        win_flags = (win_flags & ~SDL_WINDOW_RESIZABLE) | SDL_WINDOW_BORDERLESS;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif

    if (p->hwaccel && !p->enable_vulkan) {
        av_log(NULL, AV_LOG_INFO,
               "Enable vulkan renderer to support hwaccel %s\n", p->hwaccel);
        p->enable_vulkan = 1;
    }
    if (p->enable_vulkan) {
        p->vk_renderer = vk_get_renderer();
        if (p->vk_renderer) {
#if SDL_VERSION_ATLEAST(2,0,6)
            win_flags |= SDL_WINDOW_VULKAN;
#endif
        } else {
            av_log(NULL, AV_LOG_WARNING,
                   "Vulkan not available, falling back to SDL renderer\n");
            p->enable_vulkan = 0;
        }
    }

    const char *title = p->window_title ? p->window_title :
                        (p->input_filename ? p->input_filename : program_name);
    p->window = SDL_CreateWindow(title,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 p->default_width, p->default_height,
                                 win_flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (!p->window) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window: %s\n", SDL_GetError());
        return AVERROR_UNKNOWN;
    }

    if (p->vk_renderer) {
        AVDictionary *dict = NULL;
        if (p->vulkan_params)
            av_dict_parse_string(&dict, p->vulkan_params, "=", ":", 0);
        int ret = vk_renderer_create(p->vk_renderer, p->window, dict);
        av_dict_free(&dict);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create vulkan renderer: %s\n",
                   av_err2str(ret));
            return ret;
        }
    } else {
        p->renderer = SDL_CreateRenderer(p->window, -1,
                                         SDL_RENDERER_ACCELERATED |
                                         SDL_RENDERER_PRESENTVSYNC);
        if (!p->renderer) {
            av_log(NULL, AV_LOG_WARNING,
                   "Hardware accelerated renderer failed (%s), trying software\n",
                   SDL_GetError());
            p->renderer = SDL_CreateRenderer(p->window, -1, 0);
        }
        if (p->renderer)
            SDL_GetRendererInfo(p->renderer, &p->renderer_info);

        if (!p->renderer || !p->renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL,
                   "Failed to create renderer: %s\n", SDL_GetError());
            return AVERROR_UNKNOWN;
        }
    }
    return 0;
}

/* ---- Public API ---- */

/**
 * Create a player instance with default settings.
 * Caller may adjust params via ffplay_player_set_*() before calling start().
 * @return opaque handle, or NULL on allocation failure.
 */
FFPlayer *ffplay_player_create(void)
{
    init_dynload();

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    FFPlayer *player = av_mallocz(sizeof(FFPlayer));
    if (!player) return NULL;

    player->params = alloc_default_params();
    if (!player->params) {
        av_freep(&player);
        return NULL;
    }
    
    player->started = 0;
    player->loading_callback = NULL;
    player->loading_callback_userdata = NULL;
    return player;
}

/**
 * Set the media URL/path to play.
 * Must be called before ffplay_player_start().
 */
void ffplay_player_set_url(FFPlayer *player, const char *url)
{
    if (!player || !url) return;
    av_freep(&player->params->input_filename);
    player->params->input_filename = av_strdup(url);
}

/**
 * Set window title (optional; defaults to URL basename).
 */
void ffplay_player_set_title(FFPlayer *player, const char *title)
{
    if (!player || !title) return;
    /* window_title points into argv, only free if we own it */
    player->params->window_title = av_strdup(title);
}

/**
 * Set preferred window size. 0 means "use video native size".
 */
void ffplay_player_set_size(FFPlayer *player, int width, int height)
{
    if (!player) return;
    if (width  > 0) { player->params->screen_width  = width;  player->params->default_width  = width;  }
    if (height > 0) { player->params->screen_height = height; player->params->default_height = height; }
}

/**
 * Enable/disable audio (must be called before start).
 */
void ffplay_player_set_audio_disabled(FFPlayer *player, int disabled)
{
    if (!player) return;
    player->params->audio_disable = disabled;
}

/**
 * Enable/disable video (must be called before start).
 */
void ffplay_player_set_video_disabled(FFPlayer *player, int disabled)
{
    if (!player) return;
    player->params->video_disable = disabled;
    if (disabled) player->params->display_disable = 1;
}

/**
 * Set startup volume [0-100].
 */
void ffplay_player_set_volume(FFPlayer *player, int volume)
{
    if (!player) return;
    player->params->startup_volume = av_clip(volume, 0, 100);
}

/**
 * Set loop count (0 = infinite, 1 = no loop).
 */
void ffplay_player_set_loop(FFPlayer *player, int loop)
{
    if (!player) return;
    player->params->loop = loop;
}

/**
 * Set loading callback for network streams.
 */
void ffplay_player_set_loading_callback(FFPlayer *player, ffplay_loading_callback callback, void *user_data)
{
    if (!player) return;
    player->loading_callback = callback;
    player->loading_callback_userdata = user_data;
}

/**
 * Start playback.
 *
 * Initialises SDL (ref-counted), creates the window/renderer, opens the
 * media stream and launches decode threads.  A single shared SDL event
 * thread is started or reused.  This function returns immediately.
 *
 * @return 0 on success, negative AVERROR on failure.
 */
int ffplay_player_start(FFPlayer *player)
{
    if (!player) return AVERROR(EINVAL);
    if (player->started) return AVERROR(EINVAL);  /* already started */

    FFPlayGlobalParams *p = player->params;
    if (!p->input_filename) {
        av_log(NULL, AV_LOG_ERROR, "ffplay_player_start: no URL set\n");
        return AVERROR(EINVAL);
    }

    if (p->display_disable)
        p->video_disable = 1;

    /* Initialise SDL (shared, ref-counted) */
    int ret = player_sdl_init(p->audio_disable, p->video_disable);
    if (ret < 0) return ret;

    /* Create the SDL window / renderer for this instance */
    ret = player_create_window(p);
    if (ret < 0) {
        player_sdl_unref();
        return ret;
    }

    /* Open the media stream */
    player->is = stream_open(p, player, p->input_filename, p->file_iformat);
    if (!player->is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open '%s'\n", p->input_filename);

        /* stream_open failed before creating VideoState, safe to cleanup window/renderer */
        if (p->renderer) { SDL_DestroyRenderer(p->renderer); p->renderer = NULL; }
        if (p->vk_renderer) { vk_renderer_destroy(p->vk_renderer); p->vk_renderer = NULL; }
        if (p->window)   { SDL_DestroyWindow(p->window);   p->window   = NULL; }

        player_sdl_unref();
        return AVERROR_UNKNOWN;
    }

    /* If read_thread failed during startup (e.g. avformat_open_input returned 404),
     * it sets abort_request=1 and exits without freeing resources. Treat this as a start failure.
     */
    /* NOTE: read_thread() reports startup failure by setting abort_request=1.
     * At this point, read_thread may still be running, so do NOT call stream_close()/do_exit()
     * here (they would wait/free and can race with partially initialised queues).
     * Instead, let the caller destroy the player (ffplay_player_destroy) which will perform
     * a consistent cleanup.
     */
    if (player->is->abort_request && !player->is->ic) {
        av_log(NULL, AV_LOG_ERROR, "ffplay_player_start: startup failed\n");
        player->started = 1; /* so ffplay_player_destroy() will take the started-path */
        return AVERROR_UNKNOWN;
    }

    /* Register in the multi-instance table */
    if (p->window)
        ffplay_register_instance(SDL_GetWindowID(p->window), player->is);

    /* Start (or reuse) the global SDL event thread */
    SDL_LockMutex(g_event_thread_lock);
    ret = ensure_event_thread_locked();
    SDL_UnlockMutex(g_event_thread_lock);
    if (ret < 0) {
        ffplay_unregister_instance(player->is);
        /* Don't free anything here - let ffplay_player_destroy handle cleanup */
        player->started = 1;  /* So ffplay_player_destroy will clean up is */
        player_sdl_unref();
        return ret;
    }

    player->started = 1;
    return 0;
}

/**
 * Pause or resume playback.
 * @param paused  1 = pause, 0 = resume, -1 = toggle.
 */
void ffplay_player_pause(FFPlayer *player, int paused)
{
    if (!player || !player->is) return;
    VideoState *is = player->is;
    if (paused == -1) {
        toggle_pause(is);
    } else if (paused && !is->paused) {
        toggle_pause(is);
    } else if (!paused && is->paused) {
        toggle_pause(is);
    }
}

/**
 * Seek to position.
 * @param position_s  Position in seconds from the beginning.
 */
void ffplay_player_seek(FFPlayer *player, double position_s)
{
    if (!player || !player->is) return;
    VideoState *is = player->is;
    int64_t target = (int64_t)(position_s * AV_TIME_BASE);
    if (is->ic && is->ic->start_time != AV_NOPTS_VALUE)
        target += is->ic->start_time;
    stream_seek(is, target, 0, 0);
}

/**
 * Seek relative to current position.
 * @param delta_s  Delta in seconds (positive = forward, negative = backward).
 */
void ffplay_player_seek_relative(FFPlayer *player, double delta_s)
{
    if (!player || !player->is) return;
    VideoState *is = player->is;
    double pos = get_master_clock(is);
    if (isnan(pos))
        pos = (double)is->seek_pos / AV_TIME_BASE;
    pos += delta_s;
    if (is->ic && is->ic->start_time != AV_NOPTS_VALUE &&
        pos < is->ic->start_time / (double)AV_TIME_BASE)
        pos = is->ic->start_time / (double)AV_TIME_BASE;
    stream_seek(is, (int64_t)(pos * AV_TIME_BASE),
                    (int64_t)(delta_s * AV_TIME_BASE), 0);
}

/**
 * Set playback volume [0-100] while playing.
 */
void ffplay_player_set_volume_live(FFPlayer *player, int volume)
{
    if (!player || !player->is) return;
    player->is->audio_volume =
        av_clip(SDL_MIX_MAXVOLUME * volume / 100, 0, SDL_MIX_MAXVOLUME);
}

/**
 * Mute or unmute.
 */
void ffplay_player_set_mute(FFPlayer *player, int muted)
{
    if (!player || !player->is) return;
    player->is->muted = muted ? 1 : 0;
}

/**
 * Toggle fullscreen.
 */
void ffplay_player_toggle_fullscreen(FFPlayer *player)
{
    if (!player || !player->is) return;
    toggle_full_screen(player->is);
    player->is->force_refresh = 1;
}

/**
 * Get current playback position in seconds. Returns NAN if unknown.
 */
double ffplay_player_get_position(FFPlayer *player)
{
    if (!player || !player->is) return NAN;
    double pos = get_master_clock(player->is);
    if (isnan(pos) && player->is->ic)
        pos = (double)player->is->seek_pos / AV_TIME_BASE;
    return pos;
}

/**
 * Get total duration in seconds. Returns NAN if unknown.
 */
double ffplay_player_get_duration(FFPlayer *player)
{
    if (!player || !player->is || !player->is->ic) return NAN;
    int64_t dur = player->is->ic->duration;
    if (dur == AV_NOPTS_VALUE) return NAN;
    return (double)dur / AV_TIME_BASE;
}

/**
 * Check whether playback has reached end-of-file.
 */
int ffplay_player_is_eof(FFPlayer *player)
{
    if (!player || !player->is) return 1;
    VideoState *is = player->is;
    return is->eof &&
           (!is->audio_st || (is->auddec.finished == is->audioq.serial &&
                               frame_queue_nb_remaining(&is->sampq) == 0)) &&
           (!is->video_st || (is->viddec.finished == is->videoq.serial &&
                               frame_queue_nb_remaining(&is->pictq) == 0));
}

/**
 * Stop playback and release all resources.
 *
 * Signals the stream to stop, waits for threads to finish, destroys the
 * window/renderer, and decrements the SDL ref-count.  Safe to call from
 * any thread.  After this call player is invalid and must not be used.
 */
void ffplay_player_destroy(FFPlayer *player)
{
    if (!player) return;
    if (player->destroyed) return;   /* already destroyed */
    player->destroyed = 1;

    if (player->started) {
        VideoState *is = player->is;
        if (is) {
            /* Unregister instance from global registry */
            ffplay_unregister_instance(is);   /* safe even if not registered */
            
            /* Detach read thread to avoid SDL_WaitThread issues on macOS */
            if (is->read_tid) {
                SDL_DetachThread(is->read_tid);
                is->read_tid = NULL;
            }
            
            /* Call stream_close once - this frees is and all its resources */
            stream_close(is);
            player->is = NULL;  /* Mark as destroyed */
        }
        
        /* Free player->params (global_params) */
        if (player->params) {
            FFPlayGlobalParams *p = player->params;
            if (p->renderer) { SDL_DestroyRenderer(p->renderer); p->renderer = NULL; }
            if (p->vk_renderer) { vk_renderer_destroy(p->vk_renderer); p->vk_renderer = NULL; }
            if (p->window)   { SDL_DestroyWindow(p->window);   p->window   = NULL; }
            uninit_opts(p);
            for (int i = 0; i < p->nb_vfilters; i++)
                av_freep(&p->vfilters_list[i]);
            av_freep(&p->vfilters_list);
            av_freep(&p->video_codec_name);
            av_freep(&p->audio_codec_name);
            av_freep(&p->subtitle_codec_name);
            av_freep(&p->input_filename);
            av_freep(&player->params);
        }
        
        player_sdl_unref();
    } else if (player->params) {
        /* Never started – only free params */
        FFPlayGlobalParams *p = player->params;
        uninit_opts(p);
        for (int i = 0; i < p->nb_vfilters; i++)
            av_freep(&p->vfilters_list[i]);
        av_freep(&p->vfilters_list);
        av_freep(&p->input_filename);
        av_freep(&p->video_codec_name);
        av_freep(&p->audio_codec_name);
        av_freep(&p->subtitle_codec_name);
        av_freep(&player->params);
    }

    av_freep(&player);
}

/**
 * Dispatch a single SDL event to the correct VideoState.
 * Shared by both the background sdl_event_thread and the main-thread
 * ffplay_player_run_event_loop so that key/mouse/window handling is
 * identical regardless of which code path is active.
 *
 * @param event   The SDL event to handle.
 * @param driver  Fallback VideoState when the event carries no windowID.
 * @return  0 normally; 1 if a global SDL_QUIT was received (caller should stop).
 */
static int dispatch_sdl_event(SDL_Event *event, VideoState *driver)
{
    VideoState *is = NULL;
    double incr, pos, frac, x;

    /* ---- resolve target instance from windowID ---- */
    switch (event->type) {
    case SDL_WINDOWEVENT:
        is = ffplay_find_instance_by_window(event->window.windowID);
        if (!is) is = driver;
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        is = ffplay_find_instance_by_window(event->key.windowID);
        if (!is) is = driver;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        is = ffplay_find_instance_by_window(event->button.windowID);
        if (!is) is = driver;
        break;
    case SDL_MOUSEMOTION:
        is = ffplay_find_instance_by_window(event->motion.windowID);
        if (!is) is = driver;
        break;
    case FF_QUIT_EVENT:
    case FF_PLAYER_STOP_EVENT:
        is = (VideoState *)event->user.data1;
        if (!is) is = driver;
        break;
    default:
        is = driver;
        break;
    }

    /* ---- handle event ---- */
    switch (event->type) {
    case SDL_KEYDOWN:
        if (!is) break;
        if (is->global_params->exit_on_keydown ||
            event->key.keysym.sym == SDLK_ESCAPE ||
            event->key.keysym.sym == SDLK_q) {
            ffplay_unregister_instance(is);
            do_exit(is);
            break;
        }
        if (!is->width)
            break;
        switch (event->key.keysym.sym) {
        case SDLK_f:
            toggle_full_screen(is);
            is->force_refresh = 1;
            break;
        case SDLK_p:
        case SDLK_SPACE:
            toggle_pause(is);
            break;
        case SDLK_m:
            toggle_mute(is);
            break;
        case SDLK_KP_MULTIPLY:
        case SDLK_0:
            update_volume(is, 1, SDL_VOLUME_STEP);
            break;
        case SDLK_KP_DIVIDE:
        case SDLK_9:
            update_volume(is, -1, SDL_VOLUME_STEP);
            break;
        case SDLK_s:
            step_to_next_frame(is);
            break;
        case SDLK_a:
            stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
            break;
        case SDLK_v:
            stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
            break;
        case SDLK_c:
            stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
            stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
            stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
            break;
        case SDLK_t:
            stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
            break;
        case SDLK_w:
            if (is->show_mode == SHOW_MODE_VIDEO &&
                is->vfilter_idx < is->global_params->nb_vfilters - 1) {
                if (++is->vfilter_idx >= is->global_params->nb_vfilters)
                    is->vfilter_idx = 0;
            } else {
                is->vfilter_idx = 0;
                toggle_audio_display(is);
            }
            break;
        case SDLK_PAGEUP:
            if (is->ic->nb_chapters <= 1) { incr = 600.0; goto ev_seek; }
            seek_chapter(is, 1); break;
        case SDLK_PAGEDOWN:
            if (is->ic->nb_chapters <= 1) { incr = -600.0; goto ev_seek; }
            seek_chapter(is, -1); break;
        case SDLK_LEFT:
            incr = is->global_params->seek_interval ? -is->global_params->seek_interval : -10.0;
            goto ev_seek;
        case SDLK_RIGHT:
            incr = is->global_params->seek_interval ?  is->global_params->seek_interval :  10.0;
            goto ev_seek;
        case SDLK_UP:   incr =  60.0; goto ev_seek;
        case SDLK_DOWN: incr = -60.0;
        ev_seek:
            if (is->global_params->seek_by_bytes) {
                pos = -1;
                if (pos < 0 && is->video_stream >= 0)
                    pos = frame_queue_last_pos(&is->pictq);
                if (pos < 0 && is->audio_stream >= 0)
                    pos = frame_queue_last_pos(&is->sampq);
                if (pos < 0)
                    pos = avio_tell(is->ic->pb);
                if (is->ic->bit_rate) incr *= is->ic->bit_rate / 8.0;
                else                  incr *= 180000.0;
                pos += incr;
                stream_seek(is, pos, incr, 1);
            } else {
                pos = get_master_clock(is);
                if (isnan(pos)) pos = (double)is->seek_pos / AV_TIME_BASE;
                pos += incr;
                if (is->ic->start_time != AV_NOPTS_VALUE &&
                    pos < is->ic->start_time / (double)AV_TIME_BASE)
                    pos = is->ic->start_time / (double)AV_TIME_BASE;
                stream_seek(is, (int64_t)(pos * AV_TIME_BASE),
                                (int64_t)(incr * AV_TIME_BASE), 0);
            }
            break;
        default:
            break;
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (!is) break;
        if (is->global_params->exit_on_mousedown) {
            ffplay_unregister_instance(is);
            do_exit(is);
            break;
        }
        if (event->button.button == SDL_BUTTON_LEFT) {
            static int64_t last_mouse_left_click = 0;
            if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                toggle_full_screen(is);
                is->force_refresh = 1;
                last_mouse_left_click = 0;
            } else {
                last_mouse_left_click = av_gettime_relative();
            }
        }
        /* fall-through */
    case SDL_MOUSEMOTION:
        if (!is) break;
        if (is->global_params->cursor_hidden) {
            SDL_ShowCursor(1);
            is->global_params->cursor_hidden = 0;
        }
        is->global_params->cursor_last_shown = av_gettime_relative();
        if (event->type == SDL_MOUSEBUTTONDOWN) {
            if (event->button.button != SDL_BUTTON_RIGHT) break;
            x = event->button.x;
        } else {
            if (!(event->motion.state & SDL_BUTTON_RMASK)) break;
            x = event->motion.x;
        }
        if (is->global_params->seek_by_bytes || is->ic->duration <= 0) {
            uint64_t size = avio_size(is->ic->pb);
            stream_seek(is, size * x / is->width, 0, 1);
        } else {
            int64_t ts;
            int ns, hh, mm, ss;
            int tns, thh, tmm, tss;
            tns  = is->ic->duration / 1000000LL;
            thh  = tns / 3600; tmm = (tns % 3600) / 60; tss = tns % 60;
            frac = x / is->width;
            ns   = frac * tns;
            hh   = ns / 3600; mm = (ns % 3600) / 60; ss = ns % 60;
            av_log(NULL, AV_LOG_INFO,
                   "Seek to %2.0f%% (%2d:%02d:%02d) of total duration "
                   "(%2d:%02d:%02d)       \n",
                   frac * 100, hh, mm, ss, thh, tmm, tss);
            ts = frac * is->ic->duration;
            if (is->ic->start_time != AV_NOPTS_VALUE)
                ts += is->ic->start_time;
            stream_seek(is, ts, 0, 0);
        }
        break;

    case SDL_WINDOWEVENT:
        if (!is) break;
        switch (event->window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            is->global_params->screen_width  = is->width  = event->window.data1;
            is->global_params->screen_height = is->height = event->window.data2;
            if (is->vis_texture) {
                SDL_DestroyTexture(is->vis_texture);
                is->vis_texture = NULL;
            }
            if (is->global_params->vk_renderer)
                vk_renderer_resize(is->global_params->vk_renderer,
                                   is->global_params->screen_width,
                                   is->global_params->screen_height);
            /* fall-through */
        case SDL_WINDOWEVENT_EXPOSED:
            is->force_refresh = 1;
            break;
        }
        break;

    case SDL_QUIT:
        /* Shut down every registered instance */
        while (ffplay_instance_count() > 0) {
            VideoState *victim;
            SDL_LockMutex(g_instance_mutex);
            victim = (g_instance_count > 0) ? g_instances[0].is : NULL;
            SDL_UnlockMutex(g_instance_mutex);
            if (!victim) break;
            ffplay_unregister_instance(victim);
            do_exit(victim);
        }
        return 1;   /* signal caller to stop */

    case FF_QUIT_EVENT:
    case FF_PLAYER_STOP_EVENT:
        if (is) {
            ffplay_unregister_instance(is);
            do_exit(is);
        }
        break;

    default:
        break;
    }
    return 0;
}

/**
 * Run SDL event loop for a short period (non-blocking).
 *
 * On macOS, SDL requires all UI operations to occur on the main thread.
 * This function MUST therefore be called from the main thread on macOS.
 * On other platforms it may be called from any thread (the background
 * event thread calls it automatically).
 *
 * Multiple player instances are fully supported: a single call to this
 * function processes events for ALL currently registered instances and
 * refreshes every active video surface.  Simply create and start as many
 * FFPlayer handles as needed; they all share this one event pump.
 *
 * @param timeout_ms  How long to sleep after processing events (milliseconds).
 *                    Pass 0 for fully non-blocking, or a small value (e.g. 10)
 *                    to cap CPU use while keeping latency low.
 * @return  1 if there are still active player instances (keep calling),
 *          0 if all instances have been destroyed / closed.
 */
int ffplay_player_run_event_loop(int timeout_ms)
{
    /* Nothing to do if no instances are registered */
    if (ffplay_instance_count() == 0)
        return 0;

    /* On macOS, ensure we're on the main thread */
#if REQUIRE_MAIN_THREAD_UI
    if (!is_on_main_thread()) {
        av_log(NULL, AV_LOG_ERROR,
               "ffplay_player_run_event_loop: must be called from the main thread on macOS\n");
        return 0;
    }
#endif

    /* ---- pick a driver instance for event dispatch fallback ---- */
    SDL_LockMutex(g_instance_mutex);
    VideoState *driver = (g_instance_count > 0) ? g_instances[0].is : NULL;
    SDL_UnlockMutex(g_instance_mutex);

    if (!driver)
        return 0;

    SDL_Event event;

    /* Pump OS events into the SDL queue (MUST be main-thread on macOS) */
    SDL_PumpEvents();

    /* Drain all queued events (non-blocking) */
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
        if (dispatch_sdl_event(&event, driver) != 0)
            return 0; /* global quit */

        /* After handling the event, re-acquire driver (it might have been destroyed) */
        SDL_LockMutex(g_instance_mutex);
        driver = (g_instance_count > 0) ? g_instances[0].is : NULL;
        SDL_UnlockMutex(g_instance_mutex);
        if (!driver)
            return 0;
    }

    /* Refresh all registered instances once (non-blocking) */
    double remaining_time = 0.0;
    SDL_LockMutex(g_instance_mutex);
    int cnt = g_instance_count;
    VideoState *snaps[FFPLAY_MAX_INSTANCES];
    for (int i = 0; i < cnt; i++)
        snaps[i] = g_instances[i].is;
    SDL_UnlockMutex(g_instance_mutex);

    for (int i = 0; i < cnt; i++) {
        if (snaps[i] &&
            snaps[i]->show_mode != SHOW_MODE_NONE &&
            (!snaps[i]->paused || snaps[i]->force_refresh))
            video_refresh(snaps[i], &remaining_time);
    }

    /* IMPORTANT (macOS):
     * This API is designed as a UI-thread "tick". Never block here waiting for events
     * (do NOT call refresh_loop_wait_event()). If you want to cap CPU, pass a small
     * timeout_ms (e.g. 1~10) and call this from a timer.
     */
    if (timeout_ms > 0)
        av_usleep((int64_t)timeout_ms * 1000LL);

    return ffplay_instance_count() > 0;
}

