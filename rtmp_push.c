#include <stdio.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

#include "rtmp_push.h"


/*
 * ============================
 * 全局 FFmpeg 结构
 * ============================
 */

static AVFormatContext *g_fmt_ctx = NULL;

static AVStream *g_video_stream = NULL;

static AVCodecContext *g_codec_ctx = NULL;

static AVFrame *g_frame = NULL;

static struct SwsContext *g_sws_ctx = NULL;


/*
 * 当前帧编号
 */
static int64_t g_frame_index = 0;


/*
 * 视频参数
 */
static int g_width = 0;
static int g_height = 0;
static int g_fps = 0;


/*
 * 打印 FFmpeg 错误信息
 */
static void print_ffmpeg_error(
        const char *msg,
        int err)
{
    char errbuf[256];

    av_strerror(
        err,
        errbuf,
        sizeof(errbuf)
    );

    fprintf(
        stderr,
        "%s: %s\n",
        msg,
        errbuf
    );
}


/*
 * ============================
 * RTMP 初始化
 * ============================
 */

int rtmp_init(
        const char *url,
        int width,
        int height,
        int fps)
{
    int ret;

    AVCodec *codec = NULL;

    AVDictionary *codec_options = NULL;


    g_width = width;
    g_height = height;
    g_fps = fps;
    g_frame_index = 0;


    /*
     * YUV420P 要求宽高最好为偶数
     */
    if ((width & 1) ||
        (height & 1))
    {
        fprintf(
            stderr,
            "width and height must be even\n"
        );

        return -1;
    }


    /*
     * FFmpeg 3.0：
     * 注册 muxer/demuxer/codec 等
     */
    av_register_all();


    /*
     * 初始化网络功能
     *
     * RTMP 属于网络协议
     */
    ret = avformat_network_init();

    if (ret < 0)
    {
        print_ffmpeg_error(
            "avformat_network_init failed",
            ret
        );

        return -1;
    }


    /*
     * ============================
     * 创建 FLV 输出
     * ============================
     *
     * RTMP 通常承载 FLV
     */

    ret = avformat_alloc_output_context2(
        &g_fmt_ctx,
        NULL,
        "flv",
        url
    );

    if (ret < 0 ||
        g_fmt_ctx == NULL)
    {
        fprintf(
            stderr,
            "Could not create FLV output context\n"
        );

        return -1;
    }


    /*
     * ============================
     * 找 libx264 编码器
     * ============================
     */

    codec =
        avcodec_find_encoder_by_name(
            "libx264"
        );

    if (codec == NULL)
    {
        fprintf(
            stderr,
            "libx264 encoder not found\n"
        );

        return -1;
    }


    /*
     * ============================
     * 创建视频流
     * ============================
     */

    g_video_stream =
        avformat_new_stream(
            g_fmt_ctx,
            codec
        );

    if (g_video_stream == NULL)
    {
        fprintf(
            stderr,
            "Could not create video stream\n"
        );

        return -1;
    }


    /*
     * FFmpeg 3.0 中
     *
     * AVStream 里面仍然有 codec
     */
    g_codec_ctx =
        g_video_stream->codec;


    /*
     * ============================
     * 配置 H.264 编码器
     * ============================
     */

    g_codec_ctx->codec_id =
        AV_CODEC_ID_H264;

    g_codec_ctx->codec_type =
        AVMEDIA_TYPE_VIDEO;


    /*
     * 分辨率
     */
    g_codec_ctx->width =
        width;

    g_codec_ctx->height =
        height;


    /*
     * 比特率
     *
     * i.MX6ULL 第一版不要太高
     */
    g_codec_ctx->bit_rate =
        500000;


    /*
     * H.264 输入格式
     */
    g_codec_ctx->pix_fmt =
        AV_PIX_FMT_YUV420P;


    /*
     * time_base = 1 / fps
     *
     * 如果 fps = 10：
     *
     * time_base = 1/10 秒
     */
    g_codec_ctx->time_base.num =
        1;

    g_codec_ctx->time_base.den =
        fps;


    /*
     * stream 使用同一 time_base
     */
    g_video_stream->time_base =
        g_codec_ctx->time_base;


    /*
     * 每 fps 帧一个关键帧
     *
     * fps=10
     * → 大约1秒一个I帧
     */
    g_codec_ctx->gop_size =
        fps;


    /*
     * 禁止 B 帧
     *
     * 有利于降低直播延迟
     */
    g_codec_ctx->max_b_frames =
        0;


    /*
     * i.MX6ULL 单核 Cortex-A7
     */
    g_codec_ctx->thread_count =
        1;


    /*
     * 某些封装要求 codec header
     * 单独保存
     */
    if (g_fmt_ctx->oformat->flags &
        AVFMT_GLOBALHEADER)
    {
        g_codec_ctx->flags |=
            AV_CODEC_FLAG_GLOBAL_HEADER;
    }


    /*
     * ============================
     * libx264 参数
     * ============================
     *
     * ultrafast：
     *   降低 CPU 压力
     *
     * zerolatency：
     *   降低推流延迟
     */

    av_dict_set(
        &codec_options,
        "preset",
        "ultrafast",
        0
    );

    av_dict_set(
        &codec_options,
        "tune",
        "zerolatency",
        0
    );


    /*
     * 打开 H.264 编码器
     */

    ret = avcodec_open2(
        g_codec_ctx,
        codec,
        &codec_options
    );

    av_dict_free(
        &codec_options
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "avcodec_open2 failed",
            ret
        );

        return -1;
    }


    /*
     * ============================
     * 创建 YUV420P AVFrame
     * ============================
     */

    g_frame =
        av_frame_alloc();

    if (g_frame == NULL)
    {
        fprintf(
            stderr,
            "av_frame_alloc failed\n"
        );

        return -1;
    }


    g_frame->format =
        AV_PIX_FMT_YUV420P;

    g_frame->width =
        width;

    g_frame->height =
        height;


    /*
     * FFmpeg 为 YUV420P
     * 分配真正的图像内存
     */
    ret = av_frame_get_buffer(
        g_frame,
        32
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "av_frame_get_buffer failed",
            ret
        );

        return -1;
    }


    /*
     * ============================
     * RGB565 → YUV420P
     * ============================
     *
     * 这里假设 V4L2 给你的数据：
     *
     * RGB565 little endian
     */

    g_sws_ctx =
        sws_getContext(
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

    if (g_sws_ctx == NULL)
    {
        fprintf(
            stderr,
            "sws_getContext failed\n"
        );

        return -1;
    }


    /*
     * ============================
     * 打开 RTMP URL
     * ============================
     */

    if (!(g_fmt_ctx->oformat->flags &
          AVFMT_NOFILE))
    {
        ret = avio_open(
            &g_fmt_ctx->pb,
            url,
            AVIO_FLAG_WRITE
        );

        if (ret < 0)
        {
            print_ffmpeg_error(
                "Could not open RTMP URL",
                ret
            );

            return -1;
        }
    }


    /*
     * ============================
     * 写 FLV header
     * ============================
     */

    ret = avformat_write_header(
        g_fmt_ctx,
        NULL
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "avformat_write_header failed",
            ret
        );

        return -1;
    }


    /*
     * 打印输出信息
     */
    av_dump_format(
        g_fmt_ctx,
        0,
        url,
        1
    );


    printf(
        "RTMP initialized successfully\n"
    );

    printf(
        "URL: %s\n",
        url
    );

    printf(
        "Video: %dx%d @ %d fps\n",
        width,
        height,
        fps
    );


    return 0;
}


/*
 * ============================
 * 推送一帧
 * ============================
 */

int rtmp_push_frame(
        const uint8_t *data,
        int stride)
{
    int ret;

    int got_packet = 0;

    AVPacket pkt;


    if (data == NULL ||
        g_codec_ctx == NULL ||
        g_fmt_ctx == NULL)
    {
        return -1;
    }


    /*
     * encoder 有可能持有之前 frame
     *
     * 写之前确保 buffer 可以修改
     */
    ret = av_frame_make_writable(
        g_frame
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "av_frame_make_writable failed",
            ret
        );

        return -1;
    }


    /*
     * ============================
     * RGB565 → YUV420P
     * ============================
     */

    {
        const uint8_t *src_data[4] =
        {
            data,
            NULL,
            NULL,
            NULL
        };


        int src_linesize[4] =
        {
            stride,
            0,
            0,
            0
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
        {
            fprintf(
                stderr,
                "sws_scale failed\n"
            );

            return -1;
        }
    }


    /*
     * ============================
     * 时间戳
     * ============================
     *
     * 0
     * 1
     * 2
     * 3
     * ...
     */

    g_frame->pts =
        g_frame_index++;


    /*
     * ============================
     * 初始化 AVPacket
     * ============================
     */

    av_init_packet(
        &pkt
    );

    pkt.data = NULL;
    pkt.size = 0;


    /*
     * ============================
     * YUV420P → H.264
     * ============================
     *
     * FFmpeg 3.0 老 API
     */

    ret = avcodec_encode_video2(
        g_codec_ctx,
        &pkt,
        g_frame,
        &got_packet
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "H264 encode failed",
            ret
        );

        return -1;
    }


    /*
     * 编码器不一定每次立刻输出 packet
     */
    if (!got_packet)
    {
        return 0;
    }


    /*
     * ============================
     * 转换时间戳
     * ============================
     *
     * codec time_base
     *
     *        ↓
     *
     * stream time_base
     */

    av_packet_rescale_ts(
        &pkt,
        g_codec_ctx->time_base,
        g_video_stream->time_base
    );


    /*
     * 这是第几个 stream
     */
    pkt.stream_index =
        g_video_stream->index;


    /*
     * ============================
     * H264
     * ↓
     * FLV
     * ↓
     * RTMP
     * ============================
     */

    ret = av_interleaved_write_frame(
        g_fmt_ctx,
        &pkt
    );

    if (ret < 0)
    {
        print_ffmpeg_error(
            "RTMP write failed",
            ret
        );

        return -1;
    }


    return 0;
}


/*
 * ============================
 * 结束推流
 * ============================
 */

void rtmp_close(void)
{
    int ret;
    int got_packet;


    /*
     * ============================
     * flush H264 encoder
     * ============================
     */

    if (g_codec_ctx &&
        g_fmt_ctx &&
        g_video_stream)
    {
        do
        {
            AVPacket pkt;

            got_packet = 0;

            av_init_packet(
                &pkt
            );

            pkt.data = NULL;
            pkt.size = 0;


            /*
             * frame=NULL
             *
             * 表示让 encoder
             * 把内部残留 packet 吐出来
             */
            ret =
                avcodec_encode_video2(
                    g_codec_ctx,
                    &pkt,
                    NULL,
                    &got_packet
                );


            if (ret < 0)
            {
                break;
            }


            if (got_packet)
            {
                av_packet_rescale_ts(
                    &pkt,
                    g_codec_ctx->time_base,
                    g_video_stream->time_base
                );


                pkt.stream_index =
                    g_video_stream->index;


                av_interleaved_write_frame(
                    g_fmt_ctx,
                    &pkt
                );
            }

        } while (got_packet);
    }


    /*
     * 写 FLV trailer
     */
    if (g_fmt_ctx)
    {
        av_write_trailer(
            g_fmt_ctx
        );
    }


    /*
     * 释放 swscale
     */
    if (g_sws_ctx)
    {
        sws_freeContext(
            g_sws_ctx
        );

        g_sws_ctx = NULL;
    }


    /*
     * 释放 AVFrame
     */
    if (g_frame)
    {
        av_frame_free(
            &g_frame
        );
    }


    /*
     * 关闭 encoder
     */
    if (g_codec_ctx)
    {
        avcodec_close(
            g_codec_ctx
        );

        g_codec_ctx = NULL;
    }


    /*
     * 关闭 RTMP 网络连接
     */
    if (g_fmt_ctx &&
        g_fmt_ctx->pb)
    {
        avio_close(
            g_fmt_ctx->pb
        );

        g_fmt_ctx->pb =
            NULL;
    }


    /*
     * 释放 AVFormatContext
     */
    if (g_fmt_ctx)
    {
        avformat_free_context(
            g_fmt_ctx
        );

        g_fmt_ctx = NULL;
    }


    avformat_network_deinit();


    printf(
        "RTMP closed\n"
    );
}