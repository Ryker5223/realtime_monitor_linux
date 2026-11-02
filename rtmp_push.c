#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <alsa/asoundlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "rtmp_push.h"

/* ================================
 * 配置
 * ================================ */

#define AUDIO_DEVICE       "plughw:0,0"
#define AUDIO_SAMPLE_RATE  48000
#define AUDIO_CHANNELS     1
#define AUDIO_BIT_RATE     64000

/* ================================
 * FFmpeg 全局变量
 * ================================ */

static AVFormatContext *g_fmt_ctx = NULL;

/* 视频 */
static AVStream *g_video_stream = NULL;
static AVCodecContext *g_codec_ctx = NULL;
static AVFrame *g_frame = NULL;
static struct SwsContext *g_sws_ctx = NULL;

/* 音频 */
static AVStream *g_audio_stream = NULL;
static AVCodecContext *g_audio_ctx = NULL;
static AVFrame *g_audio_frame = NULL;
static struct SwrContext *g_swr_ctx = NULL;

/* ALSA */
static snd_pcm_t *g_pcm = NULL;
static pthread_t g_audio_tid;
static int g_audio_thread_created = 0;
static volatile int g_audio_running = 0;

/* 视频参数 */
static int g_width = 0;
static int g_height = 0;
static int g_fps = 0;

/* 时间戳 */
static int64_t g_start_us = 0;  // 是整个音视频系统的公共起点
static int64_t g_last_video_pts = -1;
static int64_t g_audio_next_pts = 0;
static int g_audio_pts_initialized = 0;

/* 输出状态 */
static int g_header_written = 0;
static int g_output_failed = 0;

/*
 * 音频线程和视频线程都可能写入 FLV。
 * 因此必须保护 av_interleaved_write_frame()。
 */
static pthread_mutex_t g_mux_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/* ================================
 * 工具函数
 * ================================ */

static void print_ffmpeg_error(const char *msg, int err)
{
    char buf[256];

    av_strerror(err, buf, sizeof(buf));

    fprintf(stderr, "%s: %s\n", msg, buf);
}

/* 获取单调时钟，单位：微秒 */
static int64_t get_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000000LL +
           ts.tv_nsec / 1000;
}

/*
 * 写入一个已经编码完成的 AVPacket。
 *
 * 所有写入操作通过同一个互斥锁串行执行。
 */
static int write_packet(AVPacket *pkt,
                        AVCodecContext *codec_ctx,
                        AVStream *stream)
{
    int ret;

    if (!g_fmt_ctx || !stream)
        return -1;

    static int audio_debug_count = 0;

int64_t old_pts = pkt->pts;
int64_t old_dts = pkt->dts;

AVRational src_tb = codec_ctx->time_base;
AVRational dst_tb = stream->time_base;

/* 时间基转换之前 */
if (stream == g_audio_stream && audio_debug_count < 30)
{
    fprintf(stderr,
        "[AAC BEFORE] pts=%lld dts=%lld duration=%lld "
        "time_base=%d/%d\n",
        (long long)old_pts,
        (long long)old_dts,
        (long long)pkt->duration,
        src_tb.num,
        src_tb.den);
}

    av_packet_rescale_ts(
        pkt,
        codec_ctx->time_base,
        stream->time_base
    );

    /* 时间基转换之后 */
    if (stream == g_audio_stream && audio_debug_count < 30)
    {
        fprintf(stderr,
            "[AAC AFTER] pts=%lld dts=%lld duration=%lld "
            "time_base=%d/%d\n",
            (long long)pkt->pts,
            (long long)pkt->dts,
            (long long)pkt->duration,
            dst_tb.num,
            dst_tb.den);

        audio_debug_count++;
    }

    pkt->stream_index = stream->index;

    pthread_mutex_lock(&g_mux_mutex);

    if (g_output_failed || !g_header_written) {
        ret = -1;
    } else {
        ret = av_interleaved_write_frame(g_fmt_ctx, pkt);

        if (ret < 0)
            g_output_failed = 1;
    }

    pthread_mutex_unlock(&g_mux_mutex);

    if (ret < 0 && ret != -1)
        print_ffmpeg_error("Write packet failed", ret);

    return ret;
}


/* ================================
 * 初始化 AAC 编码器
 * ================================ */

static int init_audio_encoder(void)
{
    AVCodec *codec;
    int ret;
    int supported = 0;

    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

    if (!codec) {
        fprintf(stderr, "AAC encoder not found\n");
        return -1;
    }

    /* 确认编码器支持 FLTP */
    if (codec->sample_fmts) {
        const enum AVSampleFormat *p = codec->sample_fmts;

        while (*p != AV_SAMPLE_FMT_NONE) {
            if (*p == AV_SAMPLE_FMT_FLTP) {
                supported = 1;
                break;
            }
            p++;
        }
    }

    if (!supported) {
        fprintf(stderr, "AAC FLTP format unsupported\n");
        return -1;
    }

    /* FFmpeg 3.0 使用 AVStream->codec */
    g_audio_stream = avformat_new_stream(g_fmt_ctx, codec);

    if (!g_audio_stream)
        return -1;

    g_audio_ctx = g_audio_stream->codec;

    g_audio_ctx->codec_id = AV_CODEC_ID_AAC;
    g_audio_ctx->codec_type = AVMEDIA_TYPE_AUDIO;

    g_audio_ctx->sample_rate = AUDIO_SAMPLE_RATE;
    g_audio_ctx->channels = AUDIO_CHANNELS;
    g_audio_ctx->channel_layout = AV_CH_LAYOUT_MONO;

    g_audio_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    g_audio_ctx->bit_rate = AUDIO_BIT_RATE;

    g_audio_ctx->time_base =
        (AVRational){1, AUDIO_SAMPLE_RATE};

    g_audio_stream->time_base = g_audio_ctx->time_base;

    if (g_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        g_audio_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(g_audio_ctx, codec, NULL);

    if (ret < 0) {
        print_ffmpeg_error("Open AAC encoder failed", ret);
        return -1;
    }

    if (g_audio_ctx->frame_size <= 0) {
        fprintf(stderr, "Invalid AAC frame size\n");
        return -1;
    }

    /* 创建音频 AVFrame */
    g_audio_frame = av_frame_alloc();

    if (!g_audio_frame)
        return -1;

    g_audio_frame->format = g_audio_ctx->sample_fmt;
    g_audio_frame->sample_rate = g_audio_ctx->sample_rate;
    g_audio_frame->channel_layout =
        g_audio_ctx->channel_layout;
    g_audio_frame->nb_samples = g_audio_ctx->frame_size;

    ret = av_frame_get_buffer(g_audio_frame, 0);

    if (ret < 0)
        return ret;

    /* ALSA S16 -> AAC FLTP */
    g_swr_ctx = swr_alloc_set_opts(
        NULL,
        AV_CH_LAYOUT_MONO,
        AV_SAMPLE_FMT_FLTP,
        AUDIO_SAMPLE_RATE,
        AV_CH_LAYOUT_MONO,
        AV_SAMPLE_FMT_S16,
        AUDIO_SAMPLE_RATE,
        0,
        NULL
    );

    if (!g_swr_ctx)
        return -1;

    ret = swr_init(g_swr_ctx);

    if (ret < 0)
        return ret;

    printf("AAC initialized: %d Hz, %d channels, "
           "%d kbps, frame_size=%d\n",
           AUDIO_SAMPLE_RATE,
           AUDIO_CHANNELS,
           AUDIO_BIT_RATE / 1000,
           g_audio_ctx->frame_size);

    return 0;
}


/* ================================
 * 编码一帧 AAC
 * ================================ */

static int encode_audio_frame(AVFrame *frame)
{
    AVPacket pkt;
    int ret;
    int got_packet = 0;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    ret = avcodec_encode_audio2(
        g_audio_ctx,
        &pkt,
        frame,
        &got_packet
    );

    if (ret < 0) {
        print_ffmpeg_error("AAC encode failed", ret);
        av_packet_unref(&pkt);
        return ret;
    }

    if (got_packet) {
        ret = write_packet(
            &pkt,
            g_audio_ctx,
            g_audio_stream
        );
    }

    av_packet_unref(&pkt);

    return ret;
}


/* ================================
 * ALSA 音频采集线程
 * ================================ */

static void *audio_capture_thread(void *arg)
{
    int ret;
    int frame_size;
    int16_t *pcm_buffer = NULL;

    (void)arg;

    frame_size = g_audio_ctx->frame_size;

    pcm_buffer = av_malloc(
        frame_size * AUDIO_CHANNELS * sizeof(int16_t)
    );

    if (!pcm_buffer) {
        fprintf(stderr, "Allocate audio buffer failed\n");
        g_audio_running = 0;
        return NULL;
    }

    /*
     * 单声道/双声道的实际输入均由 ALSA 配置决定。
     * 当前固定使用双声道 S16_LE。
     */
    ret = snd_pcm_open(
        &g_pcm,
        AUDIO_DEVICE,
        SND_PCM_STREAM_CAPTURE,
        0
    );

    if (ret < 0) {
        fprintf(stderr, "ALSA open failed: %s\n",
                snd_strerror(ret));
        goto exit_thread;
    }

    ret = snd_pcm_set_params(
        g_pcm,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        AUDIO_CHANNELS,
        AUDIO_SAMPLE_RATE,
        1,       /* 允许软件重采样 */
        500000
    );

    if (ret < 0) {
        fprintf(stderr, "ALSA config failed: %s\n",
                snd_strerror(ret));
        goto exit_thread;
    }

    printf("ALSA audio capture started\n");

    while (g_audio_running) {
        int collected = 0;

        /* 收集一个完整 AAC 输入帧 */
        while (collected < frame_size && g_audio_running) {
            snd_pcm_sframes_t n;

            n = snd_pcm_readi(
                g_pcm,
                pcm_buffer + collected * AUDIO_CHANNELS,
                frame_size - collected
            );

            if (n == -EINTR)
                continue;

            if (n < 0) {
                int recover_ret = snd_pcm_recover(
                    g_pcm, (int)n, 1
                );

                if (recover_ret < 0) {
                    fprintf(stderr,
                            "ALSA read failed: %s\n",
                            snd_strerror(recover_ret));
                    goto exit_thread;
                }

                continue;
            }

            if (n == 0)
                continue;

            collected += (int)n;
        }


        /*test*/
        if (!g_audio_running)
            break;

            if (collected != frame_size)
                continue;

            /* 检查 PCM 音量 */
            int max_abs = 0;
            int samples = collected * AUDIO_CHANNELS;

            for (int i = 0; i < samples; i++)
            {
                int value = pcm_buffer[i];

                if (value < 0)
                    value = -value;

                if (value > max_abs)
                    max_abs = value;
            }

            static int count = 0;

            if (++count % 50 == 0)
            {
                printf("PCM amplitude: %d\n", max_abs);
            }



        if (!g_audio_running)
            break;

        if (collected != frame_size)
            continue;


        /* PCM 数据刚刚采集完成时记录时间 */
        int64_t read_done_us = get_monotonic_us();

        if (read_done_us < 0)
            goto exit_thread;

        ret = av_frame_make_writable(g_audio_frame);

        if (ret < 0)
            goto exit_thread;

        const uint8_t *input_data[1] = {
            (const uint8_t *)pcm_buffer
        };

        ret = swr_convert(
            g_swr_ctx,
            g_audio_frame->data,
            frame_size,
            input_data,
            frame_size
        );

        if (ret != frame_size) {
            fprintf(stderr,
                    "Audio format conversion failed: %d\n",
                    ret);
            goto exit_thread;
        }

        /*
         * 两路使用同一个 monotonic 起点。
         *
         * 对首个音频块，根据采集完成时刻估计
         * 该块的起始时间；之后按采样数递增。
         */
        if (!g_audio_pts_initialized) {
            int64_t now_us = get_monotonic_us();

            if (now_us < 0)
                goto exit_thread;

            int64_t elapsed_us = now_us - g_start_us;

            int64_t first_us =
                elapsed_us -
                (int64_t)frame_size * 1000000 /
                    AUDIO_SAMPLE_RATE;

            if (first_us < 0)
                first_us = 0;

            g_audio_next_pts = av_rescale_q(
                first_us,
                (AVRational){1, 1000000},
                g_audio_ctx->time_base
            );

            g_audio_pts_initialized = 1;
        }

        g_audio_frame->pts = g_audio_next_pts;
        g_audio_next_pts += frame_size;

        ret = encode_audio_frame(g_audio_frame);

        if (ret < 0)
            goto exit_thread;
    }

exit_thread:

    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }

    av_free(pcm_buffer);

    g_audio_running = 0;

    printf("ALSA audio thread exited\n");

    return NULL;
}


/* ================================
 * RTMP 初始化
 * ================================ */

int rtmp_init(const char *url, int width, int height, int fps)
{
    int ret;
    AVCodec *codec = NULL;
    AVDictionary *codec_options = NULL;

    if (!url || width <= 0 || height <= 0 || fps <= 0)
        return -1;

    if ((width & 1) || (height & 1)) {
        fprintf(stderr, "Video size must be even\n");
        return -1;
    }

    g_width = width;
    g_height = height;
    g_fps = fps;

    g_last_video_pts = -1;
    g_audio_next_pts = 0;
    g_audio_pts_initialized = 0;

    g_header_written = 0;
    g_output_failed = 0;

    av_register_all();

    ret = avformat_network_init();
    if (ret < 0)
        return -1;

    ret = avformat_alloc_output_context2(
        &g_fmt_ctx,
        NULL,
        "flv",
        url
    );

    if (ret < 0 || !g_fmt_ctx)
        goto fail;

    /* ================================
     * 初始化 H.264 编码器
     * ================================ */

    codec = avcodec_find_encoder_by_name("libx264");

    if (!codec) {
        fprintf(stderr, "libx264 not found\n");
        goto fail;
    }

    g_video_stream = avformat_new_stream(g_fmt_ctx, codec);

    if (!g_video_stream)
        goto fail;

    g_codec_ctx = g_video_stream->codec;

    g_codec_ctx->codec_id = AV_CODEC_ID_H264;
    g_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;

    g_codec_ctx->width = width;
    g_codec_ctx->height = height;

    g_codec_ctx->bit_rate = 500000;
    g_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    /*
     * 使用微秒时间基，便于表示真实采集间隔。
     * framerate 保留原来的目标帧率信息。
     */
    /* H.264 编码器时间基 */
    g_codec_ctx->time_base = (AVRational){1, fps};

    /* 实际编码帧率 */
    g_codec_ctx->framerate = (AVRational){fps, 1};

    /* 视频流时间基 */
    g_video_stream->time_base = g_codec_ctx->time_base;

    g_codec_ctx->gop_size = fps;
    g_codec_ctx->max_b_frames = 0;
    g_codec_ctx->thread_count = 1;

    if (g_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        g_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_dict_set(&codec_options, "preset", "ultrafast", 0);
    av_dict_set(&codec_options, "tune", "zerolatency", 0);

    ret = avcodec_open2(g_codec_ctx, codec, &codec_options);

    av_dict_free(&codec_options);

    if (ret < 0) {
        print_ffmpeg_error("Open H264 encoder failed", ret);
        goto fail;
    }

    /* 视频 AVFrame */
    g_frame = av_frame_alloc();

    if (!g_frame)
        goto fail;

    g_frame->format = AV_PIX_FMT_YUV420P;
    g_frame->width = width;
    g_frame->height = height;

    ret = av_frame_get_buffer(g_frame, 32);

    if (ret < 0)
        goto fail;

    /* RGB565 -> YUV420P */
    g_sws_ctx = sws_getContext(
        width,
        height,
        AV_PIX_FMT_RGB565LE,
        width,
        height,
        AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    if (!g_sws_ctx)
        goto fail;

    /* ================================
     * 初始化 AAC
     * 必须在写 FLV header 之前执行
     * ================================ */

    ret = init_audio_encoder();

    if (ret < 0)
        goto fail;

    /* ================================
     * 打开 RTMP
     * ================================ */

    if (!(g_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(
            &g_fmt_ctx->pb,
            url,
            AVIO_FLAG_WRITE
        );

        if (ret < 0) {
            print_ffmpeg_error("Open RTMP failed", ret);
            goto fail;
        }
    }

    ret = avformat_write_header(g_fmt_ctx, NULL);

    if (ret < 0) {
        print_ffmpeg_error("Write FLV header failed", ret);
        goto fail;
    }

    g_header_written = 1;

    av_dump_format(g_fmt_ctx, 0, url, 1);

    /* 所有输出流准备好后，建立公共起始时间 */
    g_start_us = get_monotonic_us();

    /* ================================
     * 启动 ALSA 采集线程
     * ================================ */

    g_audio_running = 1;

    ret = pthread_create(
        &g_audio_tid,
        NULL,
        audio_capture_thread,
        NULL
    );

    if (ret != 0) {
        fprintf(stderr,
                "Create audio thread failed: %s\n",
                strerror(ret));

        g_audio_running = 0;
        goto fail;
    }

    g_audio_thread_created = 1;

    printf("RTMP audio/video initialized\n");
    printf("URL: %s\n", url);
    printf("Video: %dx%d @ %d fps\n", width, height, fps);

    return 0;

fail:
    av_dict_free(&codec_options);
    rtmp_abort();
    return -1;
}


/* ================================
 * 视频编码和推流
 * 保留原来的调用接口
 * ================================ */

int rtmp_push_frame(
    const uint8_t *data,
    int stride,
    int64_t capture_us)
{
    int ret;
    int got_packet = 0;
    AVPacket pkt;

    if (!data || !g_codec_ctx ||
        !g_fmt_ctx || g_output_failed)
        return -1;

    ret = av_frame_make_writable(g_frame);

    if (ret < 0)
        return -1;

    /* RGB565 -> YUV420P */
    const uint8_t *src_data[4] = {
        data, NULL, NULL, NULL
    };

    int src_linesize[4] = {
        stride, 0, 0, 0
    };

    ret = sws_scale(
        g_sws_ctx,
        src_data,
        src_linesize,
        0,
        g_height,
        g_frame->data,
        g_frame->linesize
    );

    if (ret <= 0)
        return -1;

    /*
     * 视频 PTS 使用公共起点后的单调时钟。
     * 丢帧后仍能反映真实时间间隔。
     */
    /* 计算距离开始推流经过的时间，单位：微秒 */
    /*
    * 使用视频真实采集时间计算 PTS
    *
    * capture_us 和 g_start_us
    * 都使用 CLOCK_MONOTONIC 时间轴。
    */

    /* 丢弃 RTMP 初始化之前采集的旧帧 */
    static int debug_count = 0;

    if (debug_count < 20)
    {
        printf("[VIDEO] capture_us=%lld, start_us=%lld\n",
            (long long)capture_us,
            (long long)g_start_us);
    }

    if (capture_us < g_start_us)
    {
        printf("[VIDEO] Frame dropped: capture_us < start_us\n");
        debug_count++;
        return 0;
    }

    /* 计算相对于 RTMP 起点的时间 */
    int64_t elapsed_us = capture_us - g_start_us;

    /* 转换成 H.264 编码器时间基 */
    int64_t pts = av_rescale_q(
        elapsed_us,
        (AVRational){1, 1000000},
        g_codec_ctx->time_base
    );

    /*
    * 如果时间戳没有递增，丢弃当前帧。
    * 不人为修改 PTS，避免破坏真实时间关系。
    */
    if (pts <= g_last_video_pts)
    {
        printf("[VIDEO] PTS dropped: pts=%lld, last=%lld\n",
            (long long)pts,
            (long long)g_last_video_pts);

        return 0;
    }

    /* 设置视频 PTS */
    g_last_video_pts = pts;
    g_frame->pts = pts;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    ret = avcodec_encode_video2(
        g_codec_ctx,
        &pkt,
        g_frame,
        &got_packet
    );

    if (ret < 0) {
        print_ffmpeg_error("H264 encode failed", ret);
        av_packet_unref(&pkt);
        return ret;
    }

    if (got_packet)
    {
        static int video_count = 0;

        if (++video_count % 30 == 0)
        {
            printf("[VIDEO] H264 packet: size=%d, pts=%lld\n",
                pkt.size,
                (long long)pkt.pts);
        }

        /* H264 视频包写入视频流 */
        ret = write_packet(
            &pkt,
            g_codec_ctx,
            g_video_stream
        );

        if (ret < 0)
        {
            fprintf(stderr,
                    "H264 RTMP write failed: %d\n",
                    ret);
        }
    }


    av_packet_unref(&pkt);

    return ret;
}


/* ================================
 * 释放资源
 * ================================ */

static void release_resources(int write_trailer)
{
    int ret;

    /* 先停止音频采集线程 */
    g_audio_running = 0;

    if (g_audio_thread_created) {
        pthread_join(g_audio_tid, NULL);
        g_audio_thread_created = 0;
    }

    /*
     * 正常关闭时，刷新两个编码器。
     * 异常断线时直接释放。
     */
    if (write_trailer &&
        g_header_written &&
        !g_output_failed) {

        if (g_audio_ctx) {
            while (1) {
                AVPacket pkt;
                int got = 0;

                av_init_packet(&pkt);
                pkt.data = NULL;
                pkt.size = 0;

                ret = avcodec_encode_audio2(
                    g_audio_ctx,
                    &pkt,
                    NULL,
                    &got
                );

                if (ret < 0 || !got) {
                    av_packet_unref(&pkt);
                    break;
                }

                ret = write_packet(
                    &pkt,
                    g_audio_ctx,
                    g_audio_stream
                );

                av_packet_unref(&pkt);

                if (ret < 0)
                    break;
            }
        }

        if (g_codec_ctx && !g_output_failed) {
            while (1) {
                AVPacket pkt;
                int got = 0;

                av_init_packet(&pkt);
                pkt.data = NULL;
                pkt.size = 0;

                ret = avcodec_encode_video2(
                    g_codec_ctx,
                    &pkt,
                    NULL,
                    &got
                );

                if (ret < 0 || !got) {
                    av_packet_unref(&pkt);
                    break;
                }

                ret = write_packet(
                    &pkt,
                    g_codec_ctx,
                    g_video_stream
                );

                av_packet_unref(&pkt);

                if (ret < 0)
                    break;
            }
        }

        if (!g_output_failed && g_fmt_ctx)
            av_write_trailer(g_fmt_ctx);
    }

    if (g_swr_ctx) {
        swr_free(&g_swr_ctx);
    }

    if (g_sws_ctx) {
        sws_freeContext(g_sws_ctx);
        g_sws_ctx = NULL;
    }

    if (g_audio_frame) {
        av_frame_free(&g_audio_frame);
    }

    if (g_frame) {
        av_frame_free(&g_frame);
    }

    /*
     * codec context 来自 AVStream->codec。
     * 遵循 FFmpeg 3.0 的资源所有权。
     */
    if (g_audio_ctx)
        avcodec_close(g_audio_ctx);

    if (g_codec_ctx)
        avcodec_close(g_codec_ctx);

    if (g_fmt_ctx && g_fmt_ctx->pb)
        avio_closep(&g_fmt_ctx->pb);

    if (g_fmt_ctx)
        avformat_free_context(g_fmt_ctx);

    g_fmt_ctx = NULL;

    g_video_stream = NULL;
    g_audio_stream = NULL;

    g_codec_ctx = NULL;
    g_audio_ctx = NULL;

    g_pcm = NULL;

    g_last_video_pts = -1;
    g_audio_next_pts = 0;
    g_audio_pts_initialized = 0;

    g_header_written = 0;
    g_output_failed = 0;

    avformat_network_deinit();

    printf("RTMP resources released\n");
}


/* 正常关闭 */
void rtmp_close(void)
{
    release_resources(1);
}

/* 断线或异常时释放 */
void rtmp_abort(void)
{
    release_resources(0);
}