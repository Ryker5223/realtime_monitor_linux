#include <stdio.h>
#include <stdint.h>
#include <alsa/asoundlib.h>

#define SAMPLE_RATE 48000
#define CHANNELS    2
#define RECORD_TIME 10
#define FRAMES  1024

int main()
{
    snd_pcm_t *pcm = NULL;
    int ret;

    int16_t buffer[FRAMES * CHANNELS];


    /* open device */
    ret = snd_pcm_open(&pcm, "plughw:0,0", SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0){
        printf("open failed: %s\n", snd_strerror(ret));
        return -1;
    }

    /* config */
    ret = snd_pcm_set_params(
        pcm,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        CHANNELS,
        SAMPLE_RATE,
        1,  // 这里改成 1，允许 ALSA 在需要时进行软件重采样
        500000
    );
    if (ret < 0){
        printf("config failed: %s\n", snd_strerror(ret));
        snd_pcm_close(pcm);
        return -1;
    }

    /* create file */
    FILE *fp = fopen("test.pcm", "wb");
    if (!fp){
        perror("fopen");
        snd_pcm_close(pcm);
        return -1;
    }

    printf("Start recording......");

    int total_frames = 0;
    int target_frames = SAMPLE_RATE * RECORD_TIME;
    while (total_frames < target_frames){
        snd_pcm_uframes_t request = FRAMES;
        
        if (request > target_frames - total_frames)
            request = target_frames - total_frames;
        
        ret = snd_pcm_readi(
            pcm,
            buffer,
            request
        );

        if (ret == -EINTR)
            continue;
        
        if (ret < 0){
            ret = snd_pcm_recover(pcm, ret, 1);

            if (ret < 0){
                printf("record failed:%s\n", snd_strerror(ret));
                break;
            }

            continue;
        }

        if (ret == 0)
            continue;

        /* save pcm data */
        size_t written = fwrite(buffer, CHANNELS * sizeof(int16_t), ret, fp);
        if (written != (size_t)ret){
            printf("write failed\n");
            break;
        }
        total_frames += ret;
    }

    printf("record done");
    fclose(fp);

    /* clean */
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    return 0;
}
