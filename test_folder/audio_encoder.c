#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <alsa/asoundlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

/* 音频参数 */
#define SAMPLE_RATE 48000
#define CHANNELS    2
#define BIT_RATE    64000
#define RECORD_TIME 10

#define AUDIO_DEVICE "plughw:0,0"
#define OUTPUT_FILE  "output.m4a"


/*
 * AAC 编码并写入 M4A
 *
 * frame != NULL：编码正常音频帧
 * frame == NULL：刷新编码器
 */
static int encode_audio(
    AVCodecContext *codec_ctx,
    AVFormatContext *fmt_ctx,
    AVStream *audio_st,
    AVFrame *frame,
    int *got_packet)
{
    AVPacket pkt;
    int ret;
    int got = 0;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    /* 编码 PCM -> AAC */
    ret = avcodec_encode_audio2(
        codec_ctx,
        &pkt,
        frame,
        &got
    );

    if (ret < 0) {
        fprintf(stderr, "AAC encode failed: %d\n", ret);
        av_packet_unref(&pkt);
        return ret;
    }

    if (got) {

        /* 转换音频时间戳 */
        av_packet_rescale_ts(
            &pkt,
            codec_ctx->time_base,
            audio_st->time_base
        );

        /* 设置音频流编号 */
        pkt.stream_index = audio_st->index;

        /* 写入 M4A 文件 */
        ret = av_interleaved_write_frame(
            fmt_ctx,
            &pkt
        );

        if (ret < 0) {
            fprintf(stderr, "Write packet failed: %d\n", ret);
        }
    }

    av_packet_unref(&pkt);

    if (got_packet)
        *got_packet = got;

    return ret;
}


int main(void)
{
    int ret;
    int result = -1;
    int frame_size = 0;

    int64_t total_samples = 0;
    int64_t target_samples =
        (int64_t)SAMPLE_RATE * RECORD_TIME;

    /* ALSA */
    snd_pcm_t *pcm = NULL;

    /* FFmpeg */
    AVCodec *codec = NULL;
    AVCodecContext *codec_ctx = NULL;

    AVFormatContext *fmt_ctx = NULL;
    AVStream *audio_st = NULL;

    AVFrame *frame = NULL;

    struct SwrContext *swr = NULL;

    /* PCM 数据缓冲区 */
    int16_t *pcm_buffer = NULL;

    int header_written = 0;


    /* ====================================
     * 1. 初始化 FFmpeg
     * ==================================== */

    av_register_all();


    /* ====================================
     * 2. 打开 ALSA 录音设备
     * ==================================== */

    ret = snd_pcm_open(
        &pcm,
        AUDIO_DEVICE,
        SND_PCM_STREAM_CAPTURE,
        0
    );

    if (ret < 0) {
        fprintf(stderr,
                "ALSA open failed: %s\n",
                snd_strerror(ret));
        goto end;
    }

    printf("ALSA device opened\n");


    /* ====================================
     * 3. 配置 ALSA
     * ==================================== */

    ret = snd_pcm_set_params(
        pcm,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        CHANNELS,
        SAMPLE_RATE,
        1,
        500000
    );

    if (ret < 0) {
        fprintf(stderr,
                "ALSA config failed: %s\n",
                snd_strerror(ret));
        goto end;
    }

    printf("ALSA configured successfully\n");


    /* ====================================
     * 4. 查找 AAC 编码器
     * ==================================== */

    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

    if (!codec) {
        fprintf(stderr, "AAC encoder not found\n");
        goto end;
    }

    /* 确认编码器支持 FLTP */
    int fltp_supported = 0;

    if (codec->sample_fmts) {
        const enum AVSampleFormat *fmt =
            codec->sample_fmts;

        while (*fmt != AV_SAMPLE_FMT_NONE) {
            if (*fmt == AV_SAMPLE_FMT_FLTP) {
                fltp_supported = 1;
                break;
            }

            fmt++;
        }
    }

    if (!fltp_supported) {
        fprintf(stderr,
                "AAC encoder does not support FLTP\n");
        goto end;
    }


    /* ====================================
     * 5. 创建编码器上下文
     * ==================================== */

    codec_ctx = avcodec_alloc_context3(codec);

    if (!codec_ctx) {
        fprintf(stderr, "Alloc codec context failed\n");
        goto end;
    }

    codec_ctx->sample_rate = SAMPLE_RATE;

    codec_ctx->channels = CHANNELS;

    codec_ctx->channel_layout =
        AV_CH_LAYOUT_STEREO;

    codec_ctx->sample_fmt =
        AV_SAMPLE_FMT_FLTP;

    codec_ctx->bit_rate = BIT_RATE;

    codec_ctx->time_base =
        (AVRational){1, SAMPLE_RATE};


    /* ====================================
     * 6. 创建 M4A 输出上下文
     * ==================================== */

    ret = avformat_alloc_output_context2(
        &fmt_ctx,
        NULL,
        NULL,
        OUTPUT_FILE
    );

    if (ret < 0 || !fmt_ctx) {
        fprintf(stderr, "Create output context failed\n");
        goto end;
    }

    /* 容器需要全局编码头 */
    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }


    /* ====================================
     * 7. 打开 AAC 编码器
     * ==================================== */

    ret = avcodec_open2(
        codec_ctx,
        codec,
        NULL
    );

    if (ret < 0) {
        fprintf(stderr, "Open AAC encoder failed: %d\n", ret);
        goto end;
    }

    frame_size = codec_ctx->frame_size;

    if (frame_size <= 0) {
        fprintf(stderr, "Invalid AAC frame size\n");
        goto end;
    }

    printf("AAC encoder opened\n");
    printf("AAC frame size: %d\n", frame_size);


    /* ====================================
     * 8. 创建音频流
     * ==================================== */

    audio_st = avformat_new_stream(
        fmt_ctx,
        NULL
    );

    if (!audio_st) {
        fprintf(stderr, "Create audio stream failed\n");
        goto end;
    }

    audio_st->time_base = codec_ctx->time_base;

    /*
     * FFmpeg 3.0 使用 AVStream->codec
     */
    ret = avcodec_copy_context(
        audio_st->codec,
        codec_ctx
    );

    if (ret < 0) {
        fprintf(stderr, "Copy codec context failed\n");
        goto end;
    }

    audio_st->codec->codec_tag = 0;


    /* ====================================
     * 9. 打开输出文件
     * ==================================== */

    ret = avio_open(
        &fmt_ctx->pb,
        OUTPUT_FILE,
        AVIO_FLAG_WRITE
    );

    if (ret < 0) {
        fprintf(stderr, "Open output file failed\n");
        goto end;
    }

    ret = avformat_write_header(
        fmt_ctx,
        NULL
    );

    if (ret < 0) {
        fprintf(stderr, "Write header failed\n");
        goto end;
    }

    header_written = 1;


    /* ====================================
     * 10. 初始化音频格式转换
     * ==================================== */

    /*
     * ALSA 输出：S16（交错）
     * AAC 输入：FLTP（平面）
     *
     * 采样率都是 48000Hz
     */

    swr = swr_alloc_set_opts(
        NULL,

        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_FLTP,
        SAMPLE_RATE,

        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_S16,
        SAMPLE_RATE,

        0,
        NULL
    );

    if (!swr) {
        fprintf(stderr, "Create swr failed\n");
        goto end;
    }

    ret = swr_init(swr);

    if (ret < 0) {
        fprintf(stderr, "Init swr failed\n");
        goto end;
    }


    /* ====================================
     * 11. 创建 AVFrame
     * ==================================== */

    frame = av_frame_alloc();

    if (!frame) {
        fprintf(stderr, "Alloc frame failed\n");
        goto end;
    }

    frame->nb_samples = frame_size;

    frame->format = codec_ctx->sample_fmt;

    frame->channel_layout =
        codec_ctx->channel_layout;

    frame->sample_rate =
        codec_ctx->sample_rate;

    ret = av_frame_get_buffer(frame, 0);

    if (ret < 0) {
        fprintf(stderr, "Alloc frame buffer failed\n");
        goto end;
    }


    /* ====================================
     * 12. 分配 PCM 缓冲区
     * ==================================== */

    pcm_buffer = av_malloc(
        frame_size *
        CHANNELS *
        sizeof(int16_t)
    );

    if (!pcm_buffer) {
        fprintf(stderr, "Alloc PCM buffer failed\n");
        goto end;
    }


    /* ====================================
     * 13. 实时录音 + AAC 编码
     * ==================================== */

    printf("Start recording and AAC encoding...\n");

    while (total_samples < target_samples) {

        /*
         * 每次准备一个 AAC 编码帧
         */
        int wanted = frame_size;

        if (target_samples - total_samples < wanted) {
            wanted = (int)(
                target_samples - total_samples
            );
        }

        int collected = 0;

        /*
         * 从 ALSA 采集 PCM
         */
        while (collected < wanted) {

            snd_pcm_sframes_t frames =
                snd_pcm_readi(
                    pcm,
                    pcm_buffer + collected * CHANNELS,
                    wanted - collected
                );

            if (frames == -EINTR)
                continue;

            if (frames < 0) {

                fprintf(stderr,
                        "ALSA read failed: %s\n",
                        snd_strerror((int)frames));

                ret = snd_pcm_recover(
                    pcm,
                    (int)frames,
                    1
                );

                if (ret < 0) {
                    fprintf(stderr,
                            "ALSA recover failed: %s\n",
                            snd_strerror(ret));
                    goto end;
                }

                continue;
            }

            if (frames == 0)
                continue;

            collected += (int)frames;
        }


        /*
         * 最后不足一个 AAC 帧
         * 使用静音补齐
         */
        if (collected < frame_size) {

            memset(
                pcm_buffer + collected * CHANNELS,
                0,
                (frame_size - collected) *
                    CHANNELS * sizeof(int16_t)
            );
        }


        /* 确保 AVFrame 可写 */
        ret = av_frame_make_writable(frame);

        if (ret < 0)
            goto end;


        /* ALSA S16 输入数据 */
        const uint8_t *input_data[1] = {
            (const uint8_t *)pcm_buffer
        };


        /*
         * S16 -> FLTP
         */
        ret = swr_convert(
            swr,
            frame->data,
            frame_size,
            input_data,
            frame_size
        );

        if (ret != frame_size) {
            fprintf(stderr,
                    "Audio convert failed: %d\n",
                    ret);
            goto end;
        }


        /* 设置音频时间戳 */
        frame->pts = total_samples;


        /* AAC 编码并写入 M4A */
        ret = encode_audio(
            codec_ctx,
            fmt_ctx,
            audio_st,
            frame,
            NULL
        );

        if (ret < 0)
            goto end;


        /* 累加实际录制的采样帧数 */
        total_samples += collected;
    }


    /* ====================================
     * 14. 刷新 AAC 编码器
     * ==================================== */

    while (1) {

        int got_packet = 0;

        ret = encode_audio(
            codec_ctx,
            fmt_ctx,
            audio_st,
            NULL,
            &got_packet
        );

        if (ret < 0)
            goto end;

        if (!got_packet)
            break;
    }


    /* ====================================
     * 15. 写入 M4A 文件尾
     * ==================================== */

    ret = av_write_trailer(fmt_ctx);

    if (ret < 0) {
        fprintf(stderr, "Write trailer failed\n");
        goto end;
    }

    printf("\nRecording completed!\n");
    printf("Output file: %s\n", OUTPUT_FILE);

    result = 0;


end:

    /* ====================================
     * 16. 释放资源
     * ==================================== */

    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }

    if (pcm_buffer)
        av_free(pcm_buffer);

    if (frame)
        av_frame_free(&frame);

    if (swr)
        swr_free(&swr);

    if (codec_ctx)
        avcodec_free_context(&codec_ctx);

    if (fmt_ctx) {

        if (fmt_ctx->pb)
            avio_closep(&fmt_ctx->pb);

        avformat_free_context(fmt_ctx);
    }

    if (result < 0) {
        fprintf(stderr,
                "Audio recording failed"
                " (header written: %d)\n",
                header_written);
    }

    return result == 0 ? 0 : 1;
}