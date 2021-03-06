#define inline __inline

#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <assert.h>
#include "play.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#   include "libavutil/avstring.h"
#   include "libavutil/eval.h"
#   include "libavutil/mathematics.h"
#   include "libavutil/pixdesc.h"
#   include "libavutil/imgutils.h"
#   include "libavutil/dict.h"
#   include "libavutil/parseutils.h"
#   include "libavutil/samplefmt.h"
#   include "libavutil/avassert.h"
#   include "libavutil/time.h"
#   include "libavformat/avformat.h"
#   include "libavdevice/avdevice.h"
#   include "libswscale/swscale.h"
#   include "libavutil/opt.h"
#   include "libavcodec/avfft.h"
#   include "libswresample/swresample.h"

#ifdef __cplusplus
}
#endif // __cplusplus

#include <SDL.h>
#include <SDL_thread.h>
// #define SDL_main main


#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define P_SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define P_SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define P_SDL_VOLUME_STEP (0.75) /** 声音大小控制步进值*/

#define P_SDL_CURSOR_HIDE_DELAY 1000000 /**播放时, 鼠标静止后自动隐藏时长*/

#define P_SDL_USE_ONEPASS_SUBTITLE_RENDER 1

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

// AVPacket 链表
typedef struct PktListNode {
    AVPacket pkt;
    struct PktListNode *next;
    int serial;
} PktList;

// Packet 队列
typedef struct {
    PktList *first_pkt, *last_pkt;
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
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */ //! pts_drift = pts - time
    double last_updated;                                                            //! last_updated = time
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

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

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

typedef struct Decoder {
    AVPacket pkt;
    AVPacket pkt_temp;
    PacketQueue *queue; // 绑定is->videoq、is->audioq、is->subtitleq
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

typedef struct VideoState {
    SDL_Thread *read_tid; //! 媒体包读取线程 read_thread, Demux解复用线程, 读视频文件stream线程, 得到AVPacket, 并对packet入栈
    AVInputFormat *iformat;
    int abort_request; //! 媒体包读取结束标志 stream_close、read_thread
    int force_refresh; //! 刷新标志
    int paused; //! 暂停、播放标志
    int last_paused; //! read_thread线程 暂停、播放标志
    int queue_attachments_req;

    int seek_req; //! 播放定位请求
    int seek_flags; //! seek 是否使用 AVSEEK_FLAG_BYTE
    int64_t seek_pos;
    int64_t seek_rel;

    int read_pause_return;
    AVFormatContext *ic;
    int realtime; // 是rtp、sdp实时流

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

    int av_sync_type; //! 视频同步音频还是音频同步视频av_sync_type, 默认AV_SYNC_AUDIO_MASTER(视频同步音频)
    //! 因为音频是采样数据, 有固定的采用周期并且依赖于主系统时钟, 要调整音频的延时播放较难控制. 所以实际场合中视频同步音频相比音频同步视频实现起来更容易

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf; // 音频缓冲区
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */ // 音频缓冲区偏移
    int audio_write_buf_size;
    int audio_volume;
    int muted; // 缄默(是否静音)
    struct AudioParams audio_src;

    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    enum ShowMode show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer; //! frame_time是delay的累加值
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st; // 视频流
    PacketQueue videoq;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    int width, height, xleft, ytop;
    int step; // 逐帧播放

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread; // read_thread 条件变量, SDL_CondWait: 等待条件变量, SDL_CondSignal: 激活信号
} VideoState;


// 播放窗口相关
static SDL_Window *window; // 播放器窗口
static SDL_Renderer *renderer; // 播放渲染器
static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int borderless; // 播放器是否有边框
static int is_full_screen; // 是否全屏播放
static int64_t cursor_last_shown; // 控制鼠标隐藏
static int cursor_shown = 1; // 鼠标是否显示, 鼠标移动显示, 停留一定时间(CURSOR_HIDE_DELAY)隐藏

static const char *audio_codec_name; // 强制使用音频解码器的名称, 如aac
static const char *subtitle_codec_name;
static const char *video_codec_name; // 如h264、hevc

static int show_status = 1; // 打印信息

static enum ShowMode show_mode = SHOW_MODE_NONE; //SHOW_MODE_RDFT

static int audio_enable = 1;    // 音频播放使能
static int video_enable = 1;    // 视频播放使能
static int subtitle_enable = 1; // 字幕播放使能

AVDictionary *format_opts, *codec_opts, *resample_opts;





/* options specified by the user */

static int64_t start_time = AV_NOPTS_VALUE; // 文件开始播放时的时间(用于播放调整)
static int64_t duration = AV_NOPTS_VALUE; // 持续时间(不晓得啥用处, 在判断pakcet.pts pkt_in_play_range 时用到)

static AVPacket flush_pkt;

double rdftspeed = 0.02; // Rdft速度, 参考SHOW_MODE_RDFT
static int64_t audio_callback_time; //! 记录音频回调的时间

static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
static int seek_by_bytes = -1;

static int fast = 0; //! Allow non spec compliant speedup tricks.
static int genpts = 0; //! Generate missing pts even if it requires parsing future frames.
static int lowres = 0; //! lowres value supported by the decoder
static int decoder_reorder_pts = -1;
static int framedrop = -1; //! drop frames when cpu is too slow
static int infinite_buffer = -1; // 是否限制packet队列的大小(实时流[rtp、sdp]不限制)




static int read_thread(void *arg);
static int audio_thread(void *arg);
static int video_thread(void *arg);
static int subtitle_thread(void *arg);

static void stream_close(VideoState *is);

// paly ctrl
static void do_close(VideoState *is);
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes);
static void stream_toggle_pause(VideoState *is);
//     void toggle_pause(VideoState *is);
//     void toggle_mute(VideoState *is);
//     void update_volume(VideoState *is, int sign, double step);
//     void step_to_next_frame(VideoState *is);
//     void toggle_full_screen(VideoState *is);
//     void toggle_next_show_mode(VideoState *is);
static void seek_chapter(VideoState *is, int incr);


// audio video sync
static int    get_master_sync_type(VideoState *is);
static double get_master_clock(VideoState *is);
static int    synchronize_audio(VideoState *is, int nb_samples);
static void   check_external_clock_speed(VideoState *is);
static double compute_target_delay(double delay, VideoState *is);
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
static void   update_video_pts(VideoState *is, double pts, int64_t pos, int serial);

// clock相关
static double get_clock(Clock *c);
static void   set_clock_at(Clock *c, double pts, int serial, double time);
static void   set_clock(Clock *c, double pts, int serial);
static void   set_clock_speed(Clock *c, double speed);
static void   init_clock(Clock *c, int *queue_serial);
static void   sync_clock_to_slave(Clock *c, Clock *slave);


// frame_queue
static void    frame_queue_unref_item(Frame *vp);
static int     frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);
static void    frame_queue_destory(FrameQueue *f);
static void    frame_queue_signal(FrameQueue *f);
static Frame * frame_queue_peek(FrameQueue *f);
static Frame * frame_queue_peek_next(FrameQueue *f);
static Frame * frame_queue_peek_last(FrameQueue *f);
static Frame * frame_queue_peek_writable(FrameQueue *f);
static Frame * frame_queue_peek_readable(FrameQueue *f);
static void    frame_queue_push(FrameQueue *f);
static void    frame_queue_next(FrameQueue *f);
static int     frame_queue_nb_remaining(FrameQueue *f);
static int64_t frame_queue_last_pos(FrameQueue *f);

// packet_queue
static int  packet_queue_put_private(PacketQueue *q, AVPacket *pkt);
static int  packet_queue_put(PacketQueue *q, AVPacket *pkt);
static int  packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
static int  packet_queue_init(PacketQueue *q);
static void packet_queue_flush(PacketQueue *q);
static void packet_queue_destroy(PacketQueue *q);
static void packet_queue_abort(PacketQueue *q);
static void packet_queue_start(PacketQueue *q);
static int  packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

// 解码
static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);
static int  decoder_start(Decoder *d, int(*fn)(void *), void *arg);
static int  decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
static void decoder_destroy(Decoder *d);
static void decoder_abort(Decoder *d, FrameQueue *fq);

void show_media_info(VideoState* is);

// texture相关
static inline void fill_rectangle(int x, int y, int w, int h);
static void        set_default_window_size(int width, int height, AVRational sar);
static int         realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture);
static void        calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, int pic_width, int pic_height, AVRational pic_sar);
static int         upload_texture(SDL_Texture *tex, AVFrame *frame, struct SwsContext **img_convert_ctx);

// AVDictionary
AVDictionary *  filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id, AVFormatContext *s, AVStream *st, AVCodec *codec);
AVDictionary ** setup_find_stream_info_opts(AVFormatContext *s, AVDictionary *codec_opts);


// other
static int lockmgr(void **mtx, enum AVLockOp op);







static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    vp = frame_queue_peek_last(&is->pictq);
    if (is->subtitle_st)
    {
        if (frame_queue_nb_remaining(&is->subpq) > 0)
        {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000))
            {
                if (!sp->uploaded)
                {
                    uint8_t* pixels[4];
                    int pitch[4];
                    unsigned int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }

                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0) {
                        return;
                    }

                    for (i = 0; i < sp->sub.num_rects; i++)
                    {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(
                            is->sub_convert_ctx,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                            sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                            0, NULL, NULL, NULL);
                        if (NULL == is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }

                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) 
                        {
                            sws_scale(is->sub_convert_ctx,
                                (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                0,
                                sub_rect->h,
                                pixels,
                                pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            }
            else {
                sp = NULL;
            }
        }
    }

    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

    if (!vp->uploaded)
    {
        int sdl_pix_fmt = vp->frame->format == AV_PIX_FMT_YUV420P ? SDL_PIXELFORMAT_YV12 : SDL_PIXELFORMAT_ARGB8888;
        if (realloc_texture(&is->vid_texture, sdl_pix_fmt, vp->frame->width, vp->frame->height, SDL_BLENDMODE_NONE, 0) < 0) {
            return;
        }
        if (upload_texture(is->vid_texture, vp->frame, &is->img_convert_ctx) < 0) {
            return;
        }

        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    if (sp)
    {
#if P_SDL_USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
                .y = rect.y + sub_rect->y * yratio,
                .w = sub_rect->w * xratio,
                .h = sub_rect->h * yratio };
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static void video_audio_display(VideoState *is)
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * is->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = is->audio_tgt.channels;
    nb_display_channels = channels;
    if (!is->paused)
    {
        int data_used = is->show_mode == SHOW_MODE_WAVES ? is->width : (2 * nb_freq);
        n = 2 * channels;
        delay = is->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
        the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * is->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used) {
            delay = data_used;
        }

#define compute_mod(a, b) ( a < 0 ? a%b + b : a%b )
        i_start = x = compute_mod(is->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (is->show_mode == SHOW_MODE_WAVES)
        {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels)
            {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = is->sample_array[idx];
                int b = is->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = is->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = is->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;

                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        is->last_i_start = i_start;
    }
    else {
        i_start = is->last_i_start;
    }

    if (is->show_mode == SHOW_MODE_WAVES)
    {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        /* total height for one channel */
        h = is->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++)
        {
            i = i_start + ch;
            y1 = is->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < is->width; x++)
            {
                y = (is->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                }
                else {
                    ys = y1;
                }

                fill_rectangle(is->xleft + x, ys, 1, y);

                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE) {
                    i -= SAMPLE_ARRAY_SIZE;
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = is->ytop + ch * h;
            fill_rectangle(is->xleft, y, is->width, 1);
        }
    }
    else
    {
        if (realloc_texture(&is->vis_texture, SDL_PIXELFORMAT_ARGB8888, is->width, is->height, SDL_BLENDMODE_NONE, 1) < 0) {
            return;
        }

        nb_display_channels = FFMIN(nb_display_channels, 2);
        if (rdft_bits != is->rdft_bits)
        {
            av_rdft_end(is->rdft);
            av_free(is->rdft_data);
            is->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            is->rdft_bits = rdft_bits;
            is->rdft_data = (FFTSample*)av_malloc_array(nb_freq, 4 * sizeof(*is->rdft_data));
        }

        if (!is->rdft || !is->rdft_data)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            is->show_mode = SHOW_MODE_WAVES;
        }
        else
        {
            FFTSample *data[2];
            SDL_Rect rect = { is->xpos, 0, 1, is->height };
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++)
            {
                data[ch] = is->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++)
                {
                    double w = (x - nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = is->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE) {
                        i -= SAMPLE_ARRAY_SIZE;
                    }
                }
                av_rdft_calc(is->rdft, data[ch]);
            }

            /* Least efficient way to do this, we should of course
            * directly access it but it is more than fast enough. */
            if (!SDL_LockTexture(is->vis_texture, &rect, (void **)&pixels, &pitch))
            {
                pitch >>= 2;
                pixels += pitch * is->height;
                for (y = 0; y < is->height; y++)
                {
                    double w = 1 / sqrt((double)nb_freq);
                    int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
                    int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                        : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                }

                SDL_UnlockTexture(is->vis_texture);
            }
            SDL_RenderCopy(renderer, is->vis_texture, NULL, NULL);
        }
        if (!is->paused) {
            is->xpos++;
        }
        if (is->xpos >= is->width) {
            is->xpos = is->xleft;
        }
    }
}


static int video_open(VideoState *is)
{
    int w, h;

    if (screen_width) {
        w = screen_width;
        h = screen_height;
    }
    else {
        w = default_width;
        h = default_height;
    }

    if (NULL == window)
    {
        int flags = SDL_WINDOW_SHOWN;
        if (is_full_screen) { flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; }
        if (borderless) { flags |= SDL_WINDOW_BORDERLESS; }
        else { flags |= SDL_WINDOW_RESIZABLE; }

        if (!window_title) { window_title = "sdl demo"; }

        window = SDL_CreateWindow(window_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (window)
        {
            SDL_RendererInfo info;
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
            }
        }
    }
    else {
        SDL_SetWindowSize(window, w, h);
    }

    if (!window || !renderer) {
        av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
        toggle_close(is);
    }

    is->width = w;
    is->height = h;

    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is)
{
    if (!window) {
        video_open(is);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO) {
        video_audio_display(is);
    }
    else if (is->video_st) {
        video_image_display(is);
    }

    SDL_RenderPresent(renderer);
}


/* called to display each frame */
static void video_refresh(void *arg, double *remaining_time)
{
    VideoState *is = (VideoState*)arg;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime) { /*如果用外部时钟同步的话*/
        check_external_clock_speed(is);
    }

    if (video_enable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) // 不显示视频(有音频), 作一下处理
    {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) /** 强制刷新视频*/
        {
            video_display(is);
            is->last_vis_time = time; /** 记录本次的时间*/
        }

        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st)
    {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0)
        {
            // nothing to do, no picture to display in the queue
        }
        else
        {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial) {
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            if (is->paused) {
                goto display;
            }

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp); //! 将当前帧vp的pts减去上一帧lastvp的pts, 得到中间时间差, 并检查差值是否在合理范围
            delay = compute_target_delay(last_duration, is); //! 以视频或音频为参考标准, 控制延时来保证音视频的同步
                                                             //! 通过上一帧的情况来预测本次的情况,这样可以得到下一帧的到来时间

            time = av_gettime_relative() / 1000000.0; //! 获取当前时间
            if (time < is->frame_timer + delay) {  //! 假如当前时间小于frame_timer + delay, 也就是这帧改显示的时间超前, 还没到, 跳转处理
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay; //! 根据音频时钟, 只要需要延时, 即delay大于0, 就需要更新累加到frame_timer当中
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
                is->frame_timer = time; //! 更新frame_timer, frame_time是delay的累加值
            }

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts)) {
                update_video_pts(is, vp->pts, vp->pos, vp->serial);  //! 更新is当中当前帧的pts
            }
            SDL_UnlockMutex(is->pictq.mutex);

            /*如果缓冲中帧数比较多的时候,例如下一帧也已经到了*/
            if (frame_queue_nb_remaining(&is->pictq) > 1)
            {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp); /*这个时候,应该用已经在缓存中的下一帧pts-当前pts来真实计算当前持续显示时间*/
                if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration)
                {
                    /*如果延迟时间超过一帧了,就采取丢掉当前帧*/
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq); /*采取丢帧策略,丢弃迟来的帧,取下一帧*/
                    goto retry;
                }
            }

            if (is->subtitle_st)
            {
                while (frame_queue_nb_remaining(&is->subpq) > 0)
                {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1) {
                        sp2 = frame_queue_peek_next(&is->subpq);
                    }
                    else {
                        sp2 = NULL;
                    }

                    if (sp->serial != is->subtitleq.serial
                        || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded)
                        {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++)
                            {
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
                    }
                    else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused) {
                stream_toggle_pause(is);
            }
        }

display:
        /* display picture */
        if (video_enable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown) {
            video_display(is);
        }
    }

    is->force_refresh = 0;

    // 显示状态信息
    if (show_status)
    {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
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
            if (is->audio_st && is->video_st) {
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            }
            else if (is->video_st) {
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            }
            else if (is->audio_st) {
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            }

            av_log(NULL, AV_LOG_INFO,
                "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%lld/%lld    \r",
                get_master_clock(is),
                (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                av_diff,
                is->frame_drops_early + is->frame_drops_late,
                aqsize / 1024,
                vqsize / 1024,
                sqsize,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

            fflush(stdout);
            last_time = cur_time;
        }
    }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n", av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    vp = frame_queue_peek_writable(&is->pictq);
    if (NULL == vp) {
        return -1;
    }

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture = decoder_decode_frame(&is->viddec, frame, NULL);
    if (got_picture < 0) {
        return -1;
    }

    if (got_picture)
    {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE) {
            dpts = av_q2d(is->video_st->time_base) * frame->pts;
        }

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
        {
            if (frame->pts != AV_NOPTS_VALUE)
            {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) &&
                    fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets > 0)
                {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}


/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size) { len = size; }
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE) {
            is->sample_array_index = 0;
        }

        size -= len;
    }
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
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused) {
        return -1;
    }

    do
    {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0)
        {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2) {
                return -1;
            }
            av_usleep(1000);
        }
#endif
        af = frame_queue_peek_readable(&is->sampq);
        if (NULL == af) { return -1; }

        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame), af->frame->nb_samples, (enum AVSampleFormat)af->frame->format, 1);

    // ~~~~~
    dec_channel_layout =
        (af->frame->channel_layout && av_frame_get_channels(af->frame) == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(af->frame));
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt ||
        dec_channel_layout != is->audio_src.channel_layout ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx))
    {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
            is->audio_tgt.channel_layout,
            is->audio_tgt.fmt,
            is->audio_tgt.freq,
            dec_channel_layout,
            (enum AVSampleFormat)af->frame->format,
            af->frame->sample_rate,
            0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n", af->frame->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)af->frame->format), av_frame_get_channels(af->frame), is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }

        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels = av_frame_get_channels(af->frame);
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = (enum AVSampleFormat)af->frame->format;
    }

    if (is->swr_ctx)
    {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }

        if (wanted_nb_samples != af->frame->nb_samples)
        {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }

        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) {
            return AVERROR(ENOMEM);
        }

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
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    }
    else
    {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts)) {
        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    }
    else {
        is->audio_clock = NAN;
    }

    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n", is->audio_clock - last_clock, is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *arg, Uint8 *stream, int len)
{
    VideoState *is = (VideoState*)arg;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size)
        {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0)
            {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = P_SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            }
            else 
            {
                if (is->show_mode != SHOW_MODE_VIDEO)
                    update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
                is->audio_buf_size = audio_size;
            }

            is->audio_buf_index = 0;
        }

        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME) {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }
        else
        {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf) {
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
            }
        }

        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }

    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock))
    {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
    static const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }

    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
        next_sample_rate_idx--;
    }

    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(P_SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / P_SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0)
    {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());

        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels)
        {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
                return -1;
            }
        }

        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }

    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }

    if (spec.channels != wanted_spec.channels)
    {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    return spec.size;
}







static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return;
    }

    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudio();
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_rdft_end(is->rdft);
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

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams) {
        return -1;
    }

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type){
    case AVMEDIA_TYPE_AUDIO: is->last_audio_stream = stream_index; forced_codec_name = audio_codec_name; break;
    case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
    case AVMEDIA_TYPE_VIDEO: is->last_video_stream = stream_index; forced_codec_name = video_codec_name; break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec)
    {
        if (forced_codec_name) {
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
        } else {
            av_log(NULL, AV_LOG_WARNING, "No codec could be found with id %d\n", avctx->codec_id);
        }

        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if (stream_lowres) { avctx->flags |= CODEC_FLAG_EMU_EDGE; }
#endif
    if (fast) { avctx->flags2 |= AV_CODEC_FLAG2_FAST; }
#if FF_API_EMU_EDGE
    if (codec->capabilities & AV_CODEC_CAP_DR1) { avctx->flags |= CODEC_FLAG_EMU_EDGE; }
#endif

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0)) {
        av_dict_set(&opts, "threads", "auto", 0);
    }

    if (stream_lowres) {
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }

    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        sample_rate = avctx->sample_rate;
        nb_channels = avctx->channels;
        channel_layout = avctx->channel_layout;

        /* prepare audio output */
        if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
        we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, is)) < 0)
            goto out;
        SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        if ((ret = decoder_start(&is->viddec, video_thread, is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}


static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);

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
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
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

static VideoState *stream_open(const char *filename)
{
    VideoState* is = (VideoState*)av_mallocz(sizeof(VideoState));
    if (!is) { return NULL; }

    is->filename = av_strdup(filename);
    if (!is->filename) { goto fail; }

    is->ytop = 0;
    is->xleft = 0;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) { goto fail; }
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0) { goto fail; }
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0) { goto fail; }

    if (packet_queue_init(&is->videoq) < 0 || packet_queue_init(&is->audioq) < 0 || packet_queue_init(&is->subtitleq) < 0) { goto fail; }

    is->continue_read_thread = SDL_CreateCond();
    if (NULL == is->continue_read_thread) { goto fail; }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);

    is->audio_clock_serial = -1;

    int startup_volume = 100;
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    is->muted = 0;
    is->av_sync_type = AV_SYNC_AUDIO_MASTER; // 视频同步于音频

    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is); // 创建读取线程
    if (0 == is->read_tid) { goto fail; }

    return is;

fail:
    av_log(NULL, AV_LOG_FATAL, "stream_open fail\n");
    stream_close(is);
    return NULL;
}




static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = (VideoState*)ctx;
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
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->filename, "rtp:", 4) || !strncmp(s->filename, "udp:", 4))) {
        return 1;
    }
    return 0;
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
    }
    else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    }
    else {
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
                    st->codecpar->channels != 0)
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
        av_get_media_type_string((enum AVMediaType)codec_type),
        old_index,
        stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    int rc = -1;
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!(rc = SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))) {
        if (cursor_shown && av_gettime_relative() - cursor_last_shown > P_SDL_CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_shown = 0;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
            video_refresh(is, &remaining_time);
        SDL_PumpEvents();
    }

    if (-1 == rc) {
        av_log(NULL, AV_LOG_INFO, "SDL_PeepEvents error: %s\n", SDL_GetError());
    }
}





void key_event_handler(VideoState* is, SDL_Event event)
{
    double incr, pos, frac;
    double x;

    switch (event.key.keysym.sym)
    {
    case SDLK_ESCAPE:
    case SDLK_q:
        toggle_close(is); // 退出
        break;
    case SDLK_f:
        toggle_full_screen(is); // 全屏
        break;
    case SDLK_p:
    case SDLK_SPACE:
        toggle_pause(is); // 播放/暂停
        break;
    case SDLK_m:
        toggle_mute(is); // 是否静音
        break;
    case SDLK_KP_MULTIPLY:
    case SDLK_0:
        update_volume(is, 1, P_SDL_VOLUME_STEP); // 声音增大
        break;
    case SDLK_KP_DIVIDE:
    case SDLK_9:
        update_volume(is, -1, P_SDL_VOLUME_STEP); // 声音减小
        break;
    case SDLK_s: // S: Step to next frame
        step_to_next_frame(is); // 下一帧
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
        toggle_next_show_mode(is);
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
        incr = -10.0;
        goto do_seek;
    case SDLK_RIGHT:
        incr = 10.0;
        goto do_seek;
    case SDLK_UP:
        incr = 60.0;
        goto do_seek;
    case SDLK_DOWN:
        incr = -60.0;
    do_seek:
        if (seek_by_bytes)
        {
            pos = -1;
            if (pos < 0 && is->video_stream >= 0) {
                pos = frame_queue_last_pos(&is->pictq);
            }
            if (pos < 0 && is->audio_stream >= 0) {
                pos = frame_queue_last_pos(&is->sampq);
            }

            if (pos < 0) {
                pos = avio_tell(is->ic->pb);
            }

            if (is->ic->bit_rate) {
                incr *= is->ic->bit_rate / 8.0;
            }
            else {
                incr *= 180000.0;
            }

            pos += incr;
            stream_seek(is, pos, incr, 1);
        }
        else
        {
            pos = get_master_clock(is);
            if (isnan(pos)) { pos = (double)is->seek_pos / AV_TIME_BASE; }

            pos += incr;
            if (is->ic->start_time != AV_NOPTS_VALUE && pos < is->ic->start_time / (double)AV_TIME_BASE) {
                pos = is->ic->start_time / (double)AV_TIME_BASE;
            }

            stream_seek(is, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
        }
        break;
    default:
        break;
    }

}


/* handle an event sent by the GUI */
unsigned int event_loop(VideoState *is)
{
    SDL_Event event;
    double incr, pos, frac;
    double x;
    int rc = 0;

    memset(&event, 0, sizeof(SDL_Event));

    refresh_loop_wait_event(is, &event);

    switch (event.type)
    {
    case SDL_QUIT:
    case FF_SDL_STREAM_CLOSE_EVENT:
        do_close(is);
        break;
    case SDL_KEYDOWN:
        key_event_handler(is, event);
        break;
    case SDL_WINDOWEVENT:
        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_RESIZED:
            screen_width = is->width = event.window.data1;
            screen_height = is->height = event.window.data2;
            if (is->vis_texture) {
                SDL_DestroyTexture(is->vis_texture);
                is->vis_texture = NULL;
            }
        case SDL_WINDOWEVENT_EXPOSED:
            is->force_refresh = 1;
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            // 双击全屏
            static int64_t last_mouse_left_click = 0;
            if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                toggle_full_screen(is);
                is->force_refresh = 1;
                last_mouse_left_click = 0;
            } else {
                last_mouse_left_click = av_gettime_relative();
            }
        }
        // 此处无 break;
    case SDL_MOUSEMOTION:
        if (0 == cursor_shown) {
            SDL_ShowCursor(1);
            cursor_shown = 1;
        }

        cursor_last_shown = av_gettime_relative();
        if (event.type == SDL_MOUSEBUTTONDOWN) {
            if (event.button.button != SDL_BUTTON_RIGHT) {
                break;
            }
            x = event.button.x;
        } else {
            if (!(event.motion.state & SDL_BUTTON_RMASK)) {
                break;
            }
            x = event.motion.x;
        }


        // 鼠标右键按下, seek
        if (seek_by_bytes || is->ic->duration <= 0)
        {
            uint64_t size = avio_size(is->ic->pb);
            stream_seek(is, size*x / is->width, 0, 1);
        }
        else
        {
            int64_t ts;
            int ns, hh, mm, ss;
            int tns, thh, tmm, tss;
            tns = is->ic->duration / 1000000LL;
            thh = tns / 3600;
            tmm = (tns % 3600) / 60;
            tss = (tns % 60);
            frac = x / is->width;
            ns = frac * tns;
            hh = ns / 3600;
            mm = (ns % 3600) / 60;
            ss = (ns % 60);
            av_log(NULL, AV_LOG_INFO, "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100, hh, mm, ss, thh, tmm, tss);
            ts = frac * is->ic->duration;
            if (is->ic->start_time != AV_NOPTS_VALUE) {
                ts += is->ic->start_time;
            }
            stream_seek(is, ts, 0, 0);
        }
        break;
    default:
        break;
    } // end of switch (event.type)

    return (0 != event.type) ? event.type : FF_SDL_STREAM_CLOSE_EVENT;
}


/* Called from the main */
int play(VideoState** video, const char* filename)
{
    VideoState *is;

    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    /* register all codecs, demux and protocols */
    av_register_all(); //! 注册所有编码器和解码器
    avformat_network_init();

    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (!video_enable) { flags &= ~SDL_INIT_VIDEO; }
    if (!audio_enable) { flags &= ~SDL_INIT_AUDIO; }
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
        * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (NULL == SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE")) { SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1); }
    }

    SDL_Init(flags);

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    av_lockmgr_register(lockmgr); // 解决多线程下的问题

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    is = stream_open(filename); //! 打开输入媒体
    if (NULL == is) {
        return -1;
    }

    *video = is;

    return 0;
}



static int lockmgr(void** pmtx, enum AVLockOp op)
{
    SDL_mutex* mtx = *((SDL_mutex**)pmtx);
    switch (op)
    {
    case AV_LOCK_CREATE:
        mtx = SDL_CreateMutex();
        if (NULL == mtx) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return 1;
        }
        *pmtx = mtx;
        return 0;
    case AV_LOCK_OBTAIN:
        return !!SDL_LockMutex(mtx);
    case AV_LOCK_RELEASE:
        return !!SDL_UnlockMutex(mtx);
    case AV_LOCK_DESTROY:
        SDL_DestroyMutex(mtx);
        return 0;
    }
    return 1;
}

void log_callback_help(void *ptr, int level, const char *fmt, va_list vl) {
    vfprintf(stdout, fmt, vl);
}
void show_paly_help()
{
    av_log_set_callback(log_callback_help);

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
        "left/right          seek backward/forward 10 seconds\n"
        "down/up             seek backward/forward 1 minute\n"
        "page down/page up   seek backward/forward 10 minutes\n"
        "right mouse click   seek to percentage in file corresponding to fraction of width\n"
        "left double-click   toggle full screen\n"
        );
}

void show_media_info(VideoState* is)
{
    AVFormatContext *pFormatCtx = is->ic;

    //转换成hh:mm:ss形式
    int tns, thh, tmm, tss;
    tns = (pFormatCtx->duration) / 1000000; //duration是以微秒为单位
    thh = tns / 3600;
    tmm = (tns % 3600) / 60;
    tss = (tns % 60);

    av_log(NULL, AV_LOG_INFO, "\n封装格式参数:\n");
    av_log(NULL, AV_LOG_INFO, "\t封装格式: %s\n", pFormatCtx->iformat->long_name);
    av_log(NULL, AV_LOG_INFO, "\t比特率: %5.2fkbps\n", pFormatCtx->bit_rate / 1000.0);
    av_log(NULL, AV_LOG_INFO, "\t视频时长: %02d:%02d:%02d\n", thh, tmm, tss);
  

    for (int i = 0; i < pFormatCtx->nb_streams; i++)
    {
        AVStream* pStream = pFormatCtx->streams[i];
        AVCodecContext *pCodecCtx = pStream->codec;
        if (pCodecCtx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            av_log(NULL, AV_LOG_INFO, "\n视频参数:\n");
            av_log(NULL, AV_LOG_INFO, "\t分辨率: %d x %d\n", pCodecCtx->width, pCodecCtx->height);

            char* pix_fmt = NULL;
            switch (pCodecCtx->pix_fmt) {
            case AV_PIX_FMT_YUV420P:
                pix_fmt = "YUV420P"; break;
            case AV_PIX_FMT_YUYV422:
                pix_fmt = "YUYV422"; break;
            case AV_PIX_FMT_RGB24:
                pix_fmt = "RGB24"; break;
            case AV_PIX_FMT_BGR24:
                pix_fmt = "BGR24"; break;
            case AV_PIX_FMT_YUVJ420P:
                pix_fmt = "PIX_FMT_YUVJ420P"; break;
            default:
                pix_fmt = "UNKNOWN";
            }
            av_log(NULL, AV_LOG_INFO, "\t输出像素格式: %s\n", pix_fmt);

            //帧率显示还有问题
            av_log(NULL, AV_LOG_INFO, "\t帧率: %5.2ffps\n", 1.0 * pStream->r_frame_rate.num / pStream->r_frame_rate.den);
            av_log(NULL, AV_LOG_INFO, "\t编码格式: %s\n", avcodec_find_encoder(pCodecCtx->codec_id)->long_name);
        }
        else if (pCodecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            av_log(NULL, AV_LOG_INFO, "\n音频参数:\n\n");

            av_log(NULL, AV_LOG_INFO, "\t编码格式: %s\n", avcodec_find_encoder(pCodecCtx->codec_id)->long_name);
            av_log(NULL, AV_LOG_INFO, "\t采样率: %d\n", pCodecCtx->sample_rate);
            av_log(NULL, AV_LOG_INFO, "\t声道数: %d\n", pCodecCtx->channels);
        }
    }

}

#pragma region frame_queue

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

static void frame_queue_destory(FrameQueue *f)
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
    while (f->size >= f->max_size && !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        return NULL;
    }

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request) {
        return NULL;
    }

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size) {
        f->windex = 0;
    }

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
    if (++f->rindex == f->max_size) {
        f->rindex = 0;
    }
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

#pragma endregion frame_queue


#pragma region packet_queue

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    PktList *pkt_node;

    if (q->abort_request) {
        return -1;
    }

    pkt_node = (PktList*)av_malloc(sizeof(PktList));
    if (!pkt_node) {
        return -1;
    }

    pkt_node->pkt = *pkt;
    pkt_node->next = NULL;
    if (pkt == &flush_pkt) {
        q->serial++;
    }
    pkt_node->serial = q->serial;

    if (!q->last_pkt) {
        q->first_pkt = pkt_node;
    }
    else {
        q->last_pkt->next = pkt_node;
    }

    q->last_pkt = pkt_node;
    q->nb_packets++;
    q->size += pkt_node->pkt.size + sizeof(*pkt_node);
    q->duration += pkt_node->pkt.duration;

    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0) {
        av_packet_unref(pkt);
    }

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
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
    PktList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }

    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
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
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    PktList *pkt_node;
    int ret;

    SDL_LockMutex(q->mutex);

    while (1)
    {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt_node = q->first_pkt;
        if (pkt_node)
        {
            q->first_pkt = pkt_node->next;
            if (NULL == q->first_pkt) {
                q->last_pkt = NULL;
            }

            q->nb_packets--;
            q->size -= pkt_node->pkt.size + sizeof(*pkt_node);
            q->duration -= pkt_node->pkt.duration;
            *pkt = pkt_node->pkt;

            if (serial) {
                *serial = pkt_node->serial;
            }

            av_free(pkt_node);
            ret = 1;
            break;
        }
        else if (!block) {
            ret = 0;
            break;
        }
        else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);
    return ret;
}


#pragma endregion packet_queue


#pragma region decoder

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
}

static int decoder_start(Decoder *d, int(*fn)(void *), void *arg)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
    int got_frame = 0;

    do {
        int ret = -1;

        if (d->queue->abort_request) {
            return -1;
        }

        if (!d->packet_pending || d->queue->serial != d->pkt_serial)
        {
            AVPacket pkt;

            do {
                if (d->queue->nb_packets == 0) {
                    SDL_CondSignal(d->empty_queue_cond);
                }

                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0) {
                    return -1;
                }

                if (pkt.data == flush_pkt.data)
                {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            } while (pkt.data == flush_pkt.data || d->queue->serial != d->pkt_serial);

            av_packet_unref(&d->pkt);
            d->pkt_temp = d->pkt = pkt;
            d->packet_pending = 1;
        }

        switch (d->avctx->codec_type)
        {
        case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_decode_video2(d->avctx, frame, &got_frame, &d->pkt_temp);
            if (got_frame) {
                if (-1 == decoder_reorder_pts) {
                    frame->pts = av_frame_get_best_effort_timestamp(frame);
                }
                else if (0 == decoder_reorder_pts) {
                    frame->pts = frame->pkt_dts;
                }
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_decode_audio4(d->avctx, frame, &got_frame, &d->pkt_temp);
            if (got_frame) {
                AVRational tb = { 1, frame->sample_rate };
                if (frame->pts != AV_NOPTS_VALUE) {
                    frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                } else if (d->next_pts != AV_NOPTS_VALUE) {
                    frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                }

                if (frame->pts != AV_NOPTS_VALUE) {
                    d->next_pts = frame->pts + frame->nb_samples;
                    d->next_pts_tb = tb;
                }
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &d->pkt_temp);
            break;
        }

        if (ret < 0) {
            d->packet_pending = 0;
        }
        else {
            d->pkt_temp.dts =
                d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                if (d->avctx->codec_type != AVMEDIA_TYPE_AUDIO)
                    ret = d->pkt_temp.size;
                d->pkt_temp.data += ret;
                d->pkt_temp.size -= ret;
                if (d->pkt_temp.size <= 0)
                    d->packet_pending = 0;
            }
            else {
                if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                }
            }
        }
    } while (!got_frame && !d->finished);

    return got_frame;
}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

#pragma endregion decoder


#pragma region texture

static inline void fill_rectangle(int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
    default_width = rect.w;
    default_height = rect.h;
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
    }
    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
    int scr_xleft, int scr_ytop, int scr_width, int scr_height,
    int pic_width, int pic_height, AVRational pic_sar)
{
    float aspect_ratio;
    int width, height, x, y;

    if (pic_sar.num == 0) {
        aspect_ratio = 0;
    }
    else {
        aspect_ratio = av_q2d(pic_sar);
    }

    if (aspect_ratio <= 0.0) {
        aspect_ratio = 1.0;
    }

    aspect_ratio *= (float)pic_width / (float)pic_height;

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = lrint(height * aspect_ratio) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = lrint(width / aspect_ratio) & ~1;
    }

    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX(width, 1);
    rect->h = FFMAX(height, 1);
}

static int upload_texture(SDL_Texture *tex, AVFrame *frame, struct SwsContext **img_convert_ctx)
{
    int ret = 0;
    switch (frame->format)
    {
    case AV_PIX_FMT_YUV420P:
        if (frame->linesize[0] < 0 || frame->linesize[1] < 0 || frame->linesize[2] < 0) {
            av_log(NULL, AV_LOG_ERROR, "Negative linesize is not supported for YUV.\n");
            return -1;
        }
        ret = SDL_UpdateYUVTexture(tex, NULL, frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);
        break;
    case AV_PIX_FMT_BGRA:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
        }
        else {
            ret = SDL_UpdateTexture(tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    default:
        /* This should only happen if we are not using avfilter... */
        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx, frame->width, frame->height, (enum AVPixelFormat)frame->format, frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
        if (*img_convert_ctx != NULL)
        {
            uint8_t *pixels[4];
            int pitch[4];
            if (!SDL_LockTexture(tex, NULL, (void **)pixels, pitch))
            {
                sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, pixels, pitch);
                SDL_UnlockTexture(tex);
            }
        }
        else {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            ret = -1;
        }
        break;
    }
    return ret;
}

#pragma endregion texture


#pragma region clock

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial) {
        return NAN;
    }

    if (c->paused) {
        return c->pts;
    }
    else {
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


#pragma endregion clock


#pragma region thread

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    int orig_nb_streams;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }

    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        ret = -1;
        goto fail;
    }

    if (scan_all_pmts_set) {
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    }

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->ic = ic;

    if (genpts) {
        ic->flags |= AVFMT_FLAG_GENPTS;
    }

    av_format_inject_global_side_data(ic);

    opts = setup_find_stream_info_opts(ic, codec_opts);
    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, opts);

    for (i = 0; i < orig_nb_streams; i++) {
        av_dict_free(&opts[i]);
    }
    av_freep(&opts);

    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
            "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (ic->pb) {
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    }

    if (seek_by_bytes < 0) {
        seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
    }

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s", t->value);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE) {
            timestamp += ic->start_time;
        }

        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (show_status) {
        av_dump_format(ic, 0, is->filename, 0);
    }

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1) {
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0) {
                st_index[type] = i;
            }
        }
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string((enum AVMediaType)i));

            st_index[i] = INT_MAX;
        }
    }

    if (video_enable) {
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    }
    if (audio_enable) {
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                st_index[AVMEDIA_TYPE_AUDIO],
                st_index[AVMEDIA_TYPE_VIDEO],
                NULL, 0);
    }
    if (video_enable && subtitle_enable) {
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                st_index[AVMEDIA_TYPE_SUBTITLE],
               (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                st_index[AVMEDIA_TYPE_AUDIO] :
                st_index[AVMEDIA_TYPE_VIDEO]),
                NULL, 0);
    }

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width) {
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE) {
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime) {
        infinite_buffer = 1;
    }

    show_media_info(is);

    while (1)
    {
        if (is->abort_request) {
            break;
        }

        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) {
                is->read_pause_return = av_read_pause(ic);
            } else {
                av_read_play(ic);
            }
        }

#pragma region seek
        if (is->seek_req)
        {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                    "%s: error while seeking\n", is->ic->filename);
            }
            else
            {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                }
                else {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused) {
                step_to_next_frame(is);
            }
        }
#pragma endregion seek


        if (is->queue_attachments_req)
        {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0) {
                    goto fail;
                }

                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }

            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        /** 队列数据满, 不读取数据 */ 
        if (infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE ||
            (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
            stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
            stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq))))
        {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);

            continue;
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0)
        {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
            {
                if (is->video_stream >= 0) {
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                }
                if (is->audio_stream >= 0) {
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                }

                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                break;
            }

            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);

            continue;
        }
        else {
            is->eof = 0;
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = (pkt->pts == AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;

        {
            int64_t tmp_stream_start_time = (stream_start_time != AV_NOPTS_VALUE) ? stream_start_time : 0;
            int64_t tmp_start_time = (start_time != AV_NOPTS_VALUE) ? start_time : 0;
            double pkt_duration_time = (pkt_ts - tmp_stream_start_time)* av_q2d(ic->streams[pkt->stream_index]->time_base) - (double)(tmp_start_time) / 1000000;

            if ( duration == AV_NOPTS_VALUE ||
                 pkt_duration_time <= duration / 1000000.0 )
            {
                pkt_in_play_range = 1;
            }
            else {
                pkt_in_play_range = 0;
            }
        }

        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        }
        else if (pkt->stream_index == is->video_stream && pkt_in_play_range && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        }
        else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        }
        else {
            av_packet_unref(pkt);
        }
    } // end while (1)

    ret = 0;


fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
    }

    if (ret != 0) {
        toggle_close(is);
    }

    SDL_DestroyMutex(wait_mutex);
    return 0;
}


static int audio_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int got_frame = 0;
    AVRational tb;

    if (NULL == frame) { return AVERROR(ENOMEM); }

    while(1)
    {
        got_frame = decoder_decode_frame(&is->auddec, frame, NULL);
        if (got_frame < 0) {
            goto the_end;
        }

        if (got_frame)
        {
            tb.num = 1;
            tb.den = frame->sample_rate;

            af = frame_queue_peek_writable(&is->sampq);
            if (NULL == af) {
                goto the_end;
            }

            af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            af->pos = av_frame_get_pkt_pos(frame);
            af->serial = is->auddec.pkt_serial;

            AVRational a = { frame->nb_samples, frame->sample_rate };
            af->duration = av_q2d(a);

            av_frame_move_ref(af->frame, frame);
            frame_queue_push(&is->sampq);
        }

    }

the_end:
    av_frame_free(&frame);
    return 0;
}

static int video_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    if (!frame) {
        return AVERROR(ENOMEM);
    }

    while (1)
    {
        ret = get_video_frame(is, frame);
        if (ret < 0) {
            goto the_end;
        }
        if (0 == ret) {
            continue;
        }

        AVRational a = { frame_rate.den, frame_rate.num };

        duration = (frame_rate.num && frame_rate.den ? av_q2d(a) : 0);
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        ret = queue_picture(is, frame, pts, duration, av_frame_get_pkt_pos(frame), is->viddec.pkt_serial);
        av_frame_unref(frame);

        if (ret < 0) {
            goto the_end;
        }
    }

the_end:
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    while (1)
    {
        if (!(sp = frame_queue_peek_writable(&is->subpq))) {
            return 0;
        }

        got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub);
        if (got_subtitle < 0) {
            break;
        }

        pts = 0;

        if (got_subtitle && sp->sub.format == 0)
        {
            if (sp->sub.pts != AV_NOPTS_VALUE) {
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            }
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        }
        else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }

    return 0;
}

#pragma endregion thread




#pragma region audio_video_sync


static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    }
    else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    }
    else {
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

/* return the wanted number of samples to get better sync if sync_type is video
* or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER)
    {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD)
        {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            }
            else
            {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold)
                {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }

                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff, avg_diff, wanted_nb_samples - nb_samples, is->audio_clock, is->audio_diff_threshold);
            }
        }
        else {
            /* too big difference : may be initial PTS errors, so
            reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}


static void check_external_clock_speed(VideoState *is) {
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    }
    else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
        (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    }
    else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    //! 因为音频是采样数据, 有固定的采用周期并且依赖于主系统时钟, 要调整音频的延时播放较难控制. 所以实际场合中视频同步音频相比音频同步视频实现起来更容易
    /* update delay to follow master synchronisation source */ 
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
        duplicating or deleting a frame */ /*我们通过复制和删除一帧来纠正大的延时*/
        //! 获取当前视频帧播放的时间, 与系统主时钟时间相减得到差值
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
        delay to compute the threshold. I still don't know
        if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay)); //! sync_threshold > AV_SYNC_THRESHOLD_MIN > 0
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration)
        {
            if (diff <= -sync_threshold) { /*当前视频帧落后于主时钟源*/
                delay = FFMAX(0, delay + diff); //! diff < sync_threshold < 0, 将 delay置为0 (当前帧的播放时间，也就是pts，滞后于主时钟)
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) { /*视频帧超前,但If a frame duration is longer than this, it will not be duplicated to compensate AV sync*/
                /*大概意思是:
                    本来当视频帧超前的时候,
                    我们应该要选择重复该帧或者下面的2倍延时(即加重延时的策略),
                    但因为该帧的显示时间大于显示更新门槛,
                    所以这个时候不应该以该帧做同步*/
                delay = delay + diff; //! 假如当前帧的播放时间(pts), 超前于主时钟, 并且delay还不小
            }
            else if (diff >= sync_threshold) {
                /*采取加倍延时*/
                delay = 2 * delay; //! 假如当前帧的播放时间(pts)，超前于主时钟，那就需要加大延时
            }
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    }
    else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

#pragma endregion audio_video_sync




#pragma region paly_ctrl


static void do_close(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);

    av_lockmgr_register(NULL);
    avformat_network_deinit();

    if (show_status)
        printf("\n");

    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes) {
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        }
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

void stream_seek_percent(VideoState *is, double percent)
{
    if (NULL == is || NULL == is->ic) { return; }

    if (seek_by_bytes || is->ic->duration <= 0)
    {
        uint64_t size = avio_size(is->ic->pb);
        int64_t ts = percent * size;

        stream_seek(is, ts, 0, 1);
    }
    else
    {
        int64_t ts = percent * is->ic->duration;
        if (is->ic->start_time != AV_NOPTS_VALUE) {
            ts += is->ic->start_time; // 偏移开始时间
        }

        stream_seek(is, ts, 0, 0);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused)
    {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

void update_volume(VideoState *is, int sign, double step)
{
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10.0)) : -1000.0;

    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));

    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused) {
        stream_toggle_pause(is);
    }
    is->step = 1;
}

void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

    is->force_refresh = 1;
}

void toggle_close(VideoState* is)
{
    SDL_Event event;

    event.type = FF_SDL_STREAM_CLOSE_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
}

void toggle_next_show_mode(VideoState *is)
{
    int next = is->show_mode;

    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));

    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = (enum ShowMode)next;
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters) {
        return;
    }

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];

        AVRational tb_a = { 1, AV_TIME_BASE };
        if (av_compare_ts(pos, tb_a, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters) {
        return;
    }

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);

    AVRational cq = { 1, AV_TIME_BASE };
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base, cq), 0, 0);
}

#pragma endregion paly_ctrl



#pragma region AVDictionary

AVDictionary *filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id, AVFormatContext *s, AVStream *st, AVCodec *codec)
{
    AVDictionary    *ret = NULL;
    AVDictionaryEntry *t = NULL;
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
        : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;
    const AVClass    *cc = avcodec_get_class();

    if (!codec)
        codec = s->oformat ? avcodec_find_encoder(codec_id)
        : avcodec_find_decoder(codec_id);

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix = 'v';
        flags |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix = 'a';
        flags |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix = 's';
        flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    }

    while (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) {
        char *p = strchr(t->key, ':');

        /* check stream specification in opt name */
        if (p)
            switch (avformat_match_stream_specifier(s, st, p + 1)) {
            case  1: *p = 0; break;
            case  0:         continue;
            default:;
        }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            (codec->priv_class &&
            av_opt_find(&codec->priv_class, t->key, NULL, flags,
            AV_OPT_SEARCH_FAKE_OBJ)))
            av_dict_set(&ret, t->key, t->value, 0);
        else if (t->key[0] == prefix &&
            av_opt_find(&cc, t->key + 1, NULL, flags,
            AV_OPT_SEARCH_FAKE_OBJ))
            av_dict_set(&ret, t->key + 1, t->value, 0);

        if (p)
            *p = ':';
    }
    return ret;
}

AVDictionary **setup_find_stream_info_opts(AVFormatContext *s, AVDictionary *codec_opts)
{
    int i;
    AVDictionary **opts;

    if (!s->nb_streams) {
        return NULL;
    }

    opts = (AVDictionary**)av_mallocz_array(s->nb_streams, sizeof(*opts));
    if (!opts) {
        av_log(NULL, AV_LOG_ERROR, "Could not alloc memory for stream options.\n");
        return NULL;
    }

    for (i = 0; i < s->nb_streams; i++) {
        opts[i] = filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id, s, s->streams[i], NULL);
    }

    return opts;
}

#pragma endregion AVDictionary