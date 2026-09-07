#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <poll.h>
#include <termios.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <pthread.h>

#include "rtmp_push.h"
#include "status.h"
#include "http.h"

#define FB_DEV "/dev/fb0" // LCD设备节点
#define FRAMEBUFFER_COUNT 3 // 帧缓存数量
#define MAX_FORMAT_COUNT 10      // 最多保存 10 种摄像头像素格式
#define SAVE_PATH "/mnt/frame.rgb565"
#define PORT 8888  // 本机 tcp 服务端端口

static int v4l2_fd = -1;
static int fb_fd = -1;
static double fps;

/* 摄像头像素格式及其描述信息 */
typedef struct camera_format
{
    unsigned char description[32]; // 字符串描述字符
    unsigned int pixelformat; // 像素格式
} cam_fmt;

/* 描述一个摄像头帧缓冲的信息 */
typedef struct cam_buf_info
{
    unsigned short *start;
    unsigned long length;
} cam_buf_info;

/* 最新帧结构体 */
typedef struct
{
    unsigned char *data;
    size_t size;
    unsigned long frame_id;
    int stop;               // 通知线程是否退出，因为多线程程序里，一个线程可能一直运行：while(1)
    pthread_mutex_t mutex;  // mutex 和 cond 用于互斥锁
    pthread_cond_t cond;

} latest_frame_t;


static latest_frame_t latest_frame;

/* LCD 信息 */
static int width;                    // LCD 宽度
static int height;                   // LCD 高度
static int fb_line_length;           // LCD 每行占用的字节数

static unsigned short *screen_base;  // LCD 显存基地址, 指向两个字节的short类型的数据
static unsigned long screen_size;    // LCD 显存大小

/* 摄像头实际视频帧大小 */
static int frm_width;
static int frm_height;

/* 摄像头帧缓冲 */
static cam_buf_info buf_infos[FRAMEBUFFER_COUNT];
static unsigned int buf_count = 0;

/* 摄像头支持的格式 */
static cam_fmt cam_fmts[MAX_FORMAT_COUNT];
static int cam_fmt_count = 0;

/* 初始化 last frame */
static int latest_frame_init(void)
{
    size_t frame_size;

    frame_size = frm_width * frm_height * 2;

    memset(&latest_frame, 0, sizeof(latest_frame));

    latest_frame.data = malloc(frame_size);

    if (latest_frame.data == NULL)
    {
        perror("malloc latest_frame");
        return -1;
    }

    latest_frame.size = frame_size;
    latest_frame.frame_id = 0;
    latest_frame.stop = 0;


    if (pthread_mutex_init(&latest_frame.mutex, NULL) != 0)
    {
        perror("pthread_mutex_init");
        free(latest_frame.data);
        latest_frame.data = NULL;
        return -1;
    }


    /*
    条件变量初始化函数，
    初始化一个 pthread_cond_t 条件变量，让线程之后可以用它来等待某个条件发生，
    或者被其他线程唤醒。
    发送线程没有新帧时，让这个线程“睡眠等待”
    */
    if (pthread_cond_init(&latest_frame.cond, NULL) != 0)
    {
        perror("pthread_cond_init");
        pthread_mutex_destroy(&latest_frame.mutex);
        free(latest_frame.data);
        latest_frame.data = NULL;
        return -1;
    }
    return 0;
}


/* 停止函数 两个线程都可能遇到错误，所以统一 */
static void request_stop(void)
{
    pthread_mutex_lock(&latest_frame.mutex);

    // 设置全局停止标志
    latest_frame.stop = 1;

    // 唤醒所有正在等待 latest_frame.cond 条件变量的线程。
    pthread_cond_broadcast(&latest_frame.cond);

    pthread_mutex_unlock(&latest_frame.mutex);
}



/* 初始化摄像头设备 */
static int v4l2_dev_init(const char *device)
{
    struct v4l2_capability cap = {0};
    unsigned int capabilities;  // capabilities 有32位数据，所以用四个字节int 保存

    // 打开摄像头
    v4l2_fd = open(device, O_RDWR);
    if (v4l2_fd < 0)
    {
        fprintf(stderr, "v4l2 open %s failed:%s\n", device, strerror(errno));
        return -1;
    }

    // 查询摄像头能力
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) <0)
    {
        perror("query cap failed:");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }
    // 某些较新的驱动使用 device_caps 保存真正的设备能力
    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        capabilities = cap.device_caps;
    else
        capabilities = cap.capabilities;

    printf("driver:%s\n", cap.driver);
    printf("card:%s\n", cap.card);
    printf("bus_info:%s\n", cap.bus_info);

    // 判断是否是视频采集设备
    if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "NOT capture video device\n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;
    }

    // mmap 模式要求设备支持 Streaming
    if (!(capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "NOT support streaming \n");
        close(v4l2_fd);
        v4l2_fd = -1;
        return -1;        
    }

    return 0;
}

/* 初始化LCD framebuffer */
static int fb_dev_init(void)
{
    struct fb_var_screeninfo fb_var = {0};
    struct fb_fix_screeninfo fb_fix = {0};

    fb_fd = open(FB_DEV, O_RDWR);
    if (fb_fd < 0)
    {
        fprintf(stderr, "open fb error: %s", strerror(errno));
        return -1;
    }

    // 获取 framebuffer 可变参数
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, & fb_var) < 0)
    {
        perror("ioctl FBIOGET_VSCREENINFO");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    // 获取 framebuffer 固定参数
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, & fb_fix) < 0)
    {
        perror("ioctl FBIOGET_FSCREENINFO");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    width = fb_var.xres;
    height = fb_var.yres;
    fb_line_length = fb_fix.line_length;

    screen_size = fb_fix.line_length * fb_var.yres;

    printf("LCD resolution: %d x %d\n", width, height);
    printf("bits_per_pixel: %d\n", fb_var.bits_per_pixel);
    printf("line_length: %d bytes\n", fb_line_length);

    /*
     * 下面代码直接把 RGB565 摄像头数据复制给 LCD，
     * 因此要求 framebuffer 是 16bit。
     */
     if (fb_var.bits_per_pixel != 16)
     {
        fprintf(stderr, "error: framebuffer is not 16-bit RGB565!\n");
        close(fb_fd);
        fb_fd = -1;
        return -1;
     }

     // 将 LCD 显存映射到用户空间
     screen_base = mmap(NULL, screen_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);

     if (screen_base == MAP_FAILED)
     {
        perror("mmap framebuffer:");
        close(fb_fd);
        fb_fd = -1;
        return -1;
     }

     // LCD 背景刷白
     memset(screen_base, 0xFF, screen_size);

     return 0;

}


/* 枚举摄像头支持的像素格式 */
static void v4l2_enum_formats(void)
{
    struct v4l2_fmtdesc fmtdesc = {0};

    cam_fmt_count = 0;

    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (fmtdesc.index = 0; fmtdesc.index < MAX_FORMAT_COUNT; fmtdesc.index++)
    {
        if (ioctl(v4l2_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0)
        {
            break;
        }
        cam_fmts[cam_fmt_count].pixelformat = fmtdesc.pixelformat;

        // 数组不能使用“=”来赋值，必须以字节复制, 这里还把 unsigned char* 转换为 char*
        snprintf(
            (char*)cam_fmts[cam_fmt_count].description, 
            sizeof(cam_fmts[cam_fmt_count].description),
            "%s",
            fmtdesc.description);

        cam_fmt_count++;
    }
}


/*
 * 打印摄像头支持的格式、分辨率和帧率
 */
static void v4l2_print_formats(void)
{
    int i;
    for (i = 0; i < cam_fmt_count; i++)
    {
        struct v4l2_frmsizeenum frmsize = {0};
        printf("------------------------------------\n");

        printf("format<%c%c%c%c>, description<%s>\n",
                cam_fmts[i].pixelformat & 0xFF,
                (cam_fmts[i].pixelformat >> 8) & 0xFF,
                (cam_fmts[i].pixelformat >> 16) & 0xFF,
                (cam_fmts[i].pixelformat >> 24) & 0xFF,
                cam_fmts[i].description);
        
        // 枚举像素格式支持的所以分辨率
        frmsize.pixel_format = cam_fmts[i].pixelformat;

        for (frmsize.index = 0; ; frmsize.index++)
        {
            if (ioctl(v4l2_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0)
            {
                break;
            }

            /*
             * 普通摄像头通常返回离散分辨率
                这里的 frmsize.type 不应该由你设置，它是 VIDIOC_ENUM_FRAMESIZES 调用成功后由驱动返回给你的
             */
             if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
             {
                struct v4l2_frmivalenum frmival = {0};

                printf("size<%u*%u", frmsize.discrete.width, frmsize.discrete.height);

                // 枚举当前分辨率支持的帧率
                frmival.pixel_format = cam_fmts[i].pixelformat;
                frmival.width = frmsize.discrete.width;
                frmival.height = frmsize.discrete.height;

                for (frmival.index = 0; ; frmival.index++)
                {
                    if (ioctl(v4l2_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) < 0)
                    {
                        break;
                    }
                    if (frmival.discrete.numerator != 0)
                    {
                        fps = (double)frmival.discrete.denominator/frmival.discrete.numerator;
                        printf("<%.2ffps>", fps);
                    }
                }
                printf("\n");
             }
        }

    }
}


/*
 * 设置摄像头视频格式, 设置 type
 */
 static int v4l2_set_format(void)
 {
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm streamparm = {0};

    /*
     * 设置视频帧格式
     *
     * 这里直接使用 LCD 的分辨率。
    */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;

    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        fprintf(stderr, "ioctl error: VIDIOC_S_FMT:%s", strerror(errno));
        return -1;
    }

    /* 判断驱动最后真正设置的格式是不是 RGB565。
     *
     * VIDIOC_S_FMT 有可能自动修改我们请求的格式。
    */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565)
    {
        fprintf(stderr, "Error: device does not support RGB565 format.");
        printf("Actual format: %c%c%c%c\n",
            fmt.fmt.pix.pixelformat & 0xFF,
            (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 24) & 0xFF);
        return -1;
    }
    /*
     * 获取驱动最终设置成功的实际分辨率
    */
    frm_width = fmt.fmt.pix.width;
    frm_height = fmt.fmt.pix.height;

    printf("width        = %u\n", fmt.fmt.pix.width);
    printf("height       = %u\n", fmt.fmt.pix.height);
    printf("bytesperline = %u\n", fmt.fmt.pix.bytesperline);
    printf("sizeimage    = %u\n", fmt.fmt.pix.sizeimage);
    printf("expected     = %u\n",
        fmt.fmt.pix.width *
        fmt.fmt.pix.height *
        2);

    printf("视频帧大小<%d * %d>\n", frm_width, frm_height);


    /*
    * 获取摄像头视频流参数, 需要设置 type
    */
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v4l2_fd, VIDIOC_G_PARM, &streamparm) < 0)
    {
        perror("ioctl VIOIOC_G_PARM");
        return -1;
    }

    /*
     * 判断摄像头是否支持设置帧率
    */
    if (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
    {
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = 30;

        if (ioctl(v4l2_fd, VIDIOC_S_PARM, &streamparm) < 0)
        {
            fprintf(stderr, "ioctl error: VIDIOC_S_PARM %s\n", strerror(errno));
            return -1;
        }
    }

    printf("<fps%d>\n", 
        streamparm.parm.capture.timeperframe.denominator / streamparm.parm.capture.timeperframe.numerator);
    
    return 0;
 }


 /*
 * 初始化摄像头帧缓冲
 */
static int v4l2_init_buffer(void)
{
    struct v4l2_requestbuffers reqbuf = {0};
    struct v4l2_buffer buf = {0};
    unsigned int i;

    /*
     * 向驱动申请帧缓冲,FRAMEBUFFER_COUNT 帧缓冲数量，
     */
     reqbuf.count = FRAMEBUFFER_COUNT;
     reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
     reqbuf.memory = V4L2_MEMORY_MMAP;

     if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
     {
        fprintf(stderr, "ioctl error: VIDIOC_REQBUFS: %s", strerror(errno));
        return -1;
     }

     /*
     * 驱动实际给我们的 buffer 数量
     * 不一定等于 FRAMEBUFFER_COUNT。
     */
    buf_count = reqbuf.count;

    if (buf_count == 0)
    {
        fprintf(stderr, "Error: driver allocated 0 buffers.");
        return -1;
    }

    /*
     * 我们数组最多只能存 FRAMEBUFFER_COUNT 个。
     */
    if (buf_count > FRAMEBUFFER_COUNT)
        buf_count = FRAMEBUFFER_COUNT;

    printf("实际申请到 %u 个帧缓冲\n", buf_count);

    /*
     * 查询每一个 buffer，并建立 mmap 映射
     */
    for (i = 0; i < buf_count; i++)
    {
        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        // 查询第 i 个 buffer
        if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("ioctl VIDIOC_QUERYBU");
            return -1;
        }

        buf_infos[i].length = buf.length; // 每个缓冲区的大小 length：通常由驱动根据你设置的视频格式自动决定

        /*
         * 将内核中的摄像头帧缓冲
         * 映射到用户空间。
         */
        buf_infos[i].start = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset);
        if (buf_infos[i].start == MAP_FAILED)
        {
            perror("mmap camera buffer");
            return -1;
        }

        printf("buffer[%u]: addr=%p, length=%lu\n", i, (void*)buf_infos[i].start, buf_infos[i].length);
    }

    /*
    * 将所有 buffer 放入采集队列
    */
    for (i = 0; i < buf_count; i++)
    {
        memset(&buf, 0, sizeof(buf));
        
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0)
        {
            fprintf(stderr, "ioctl error: VIDIOC_QBUF");
            return -1;
        }
    }
    return 0;

}

/*
 * 开启摄像头视频流
 */
static int v4l2_stream_on(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0)
    {
        fprintf(stderr, "ioctl error: VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}


// 关闭摄像头视频流
static int v4l2_stream_off(void)
{
    enum v4l2_buf_type type =
        V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (v4l2_fd >= 0)
    {
        if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0)
        {
            perror("VIDIOC_STREAMOFF");
            return -1;
        }
    }

    return 0;
}



/*
 * 线程1 循环读取摄像头数据并显示到 LCD
 */
static void *capture_lcd_thread(void *arg)
{
    struct v4l2_buffer buf;
    struct pollfd fds[1];

    int min_w;
    int min_h;
    int lcd_stride;

    (void)arg;

    // ========================================================
    // LCD显示范围
    // ========================================================
    if (width > frm_width)
        min_w = frm_width;
    else
        min_w = width;
    if (height > frm_height)
        min_h = frm_height;
    else
        min_h = height;
    lcd_stride = fb_line_length / 2;

    // ========================================================
    // poll
    // ========================================================

    memset(fds, 0, sizeof(fds));  // 里面的内容是未初始化的随机值，所以先清零更安全

    fds[0].fd = v4l2_fd;
    fds[0].events = POLLIN;


    // ========================================================
    // 采集循环
    // ========================================================
    while (1)
    {
        int ret;
        /*
         * 检查是否要求退出
         */
        pthread_mutex_lock(&latest_frame.mutex);

        int stop = latest_frame.stop;

        pthread_mutex_unlock(&latest_frame.mutex);

        if (stop)
            break;

        ret = poll(fds, 1, 1000);

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;

            perror("poll");

            break;
        }


        if (ret == 0)
        {
            continue;
        }

        // ====================================================
        // 摄像头错误
        // ====================================================

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "camera poll error\n");

            break;
        }


        if (!(fds[0].revents & POLLIN))
        {
            continue;
        }


        // ====================================================
        // DQBUF
        // ====================================================

        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0)
        {
            if (errno == EINTR)
                continue;

            perror("VIDIOC_DQBUF");

            break;
        }


        if (buf.index >= buf_count)
        {
            fprintf(stderr, "Invalid buffer index\n");

            ioctl(v4l2_fd, VIDIOC_QBUF, &buf);

            break;
        }


        unsigned short *start = buf_infos[buf.index].start;

        size_t frame_size = frm_width * frm_height * 2;

        // ====================================================
        // 复制到“最新帧”
        //
        // 另一线程 以后只使用这块内存
        // ====================================================

        pthread_mutex_lock(&latest_frame.mutex);

        memcpy(latest_frame.data, start, frame_size);

        latest_frame.frame_id++;

        /*
         * 通知 线程：
         * 有新帧。
         */

         // 唤醒至少一个正在等待 latest_frame.cond 条件变量的线程。
        pthread_cond_signal(&latest_frame.cond);

        pthread_mutex_unlock(&latest_frame.mutex);


        // ====================================================
        // 马上 QBUF
        //
        // 到这里之后我们不再访问 starts
        // ====================================================

        if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("VIDIOC_QBUF");

            break;
        }

        // ====================================================
        // LCD显示最新帧
        //
        // 此处 latest_frame.data 只会由当前 capture 线程写。
        // 另一线程只读。
        // 下一次覆盖发生在下一轮采集，因此当前显示安全。
        // ====================================================

        unsigned short *base = screen_base;

        unsigned short *lcd_src = (unsigned short *)latest_frame.data;

        int j;

        for (j = 0; j < min_h; j++)
        {
            memcpy(base, lcd_src, min_w * 2);

            base += lcd_stride;

            lcd_src += frm_width;
        }

    }


    request_stop();

    return NULL;
}


/* 线程2 ffmpeg */
static void *ffmpeg_thread(void *arg)
{
    unsigned long last_frame_id = 0;

    size_t frame_size;

    unsigned char *frame_copy;

    (void)arg;

    frame_size = frm_height * frm_width * 2;

     /*
     * 给推流线程自己申请一块缓存。
     *
     * 注意：
     * 不能直接长期使用 latest_frame.data。
     *
     * 因为采集线程下一帧还会覆盖它。
     */
    frame_copy = malloc(frame_size);
    if (frame_copy == NULL)
    {
        perror("malloc frame_copu failed\n");
        request_stop();
        return NULL;
    }

    
    /*
    * 初始化 FFmpeg + RTMP
    */
    if (rtmp_init("rtmp://106.14.2.164/live/cam1", frm_width, frm_height, 10) < 0)
    {
        fprintf(stderr, "rtmp_init failed\n");
        free(frame_copy);
        request_stop();
        return NULL;
    }


    while (1)
    {
        /*等待新帧*/
        pthread_mutex_lock(&latest_frame.mutex);

        /*
        * 两种情况需要等待，1.程序没有退出。2.还是上一帧
        */
        while (!latest_frame.stop && latest_frame.frame_id == last_frame_id)
        {
            // 先释放互斥锁，然后让当前线程睡眠；被唤醒后，再重新获得互斥锁，最后函数才返回
            pthread_cond_wait(&latest_frame.cond, &latest_frame.mutex);
        }

        if (latest_frame.stop == 1)
        {
            pthread_mutex_unlock(&latest_frame.mutex);
            break;
        }

        /* 拷贝最新帧， 还必须持有 mutex, 防止采集线程修改 */
        memcpy(frame_copy, latest_frame.data, frame_size);

        last_frame_id = latest_frame.frame_id;

        // 解锁
        pthread_mutex_unlock(&latest_frame.mutex);

        /*
        * ffmpeg 编码 + RTMP， 使用frame_copy的数据，不再访问latest_frame
        */
        if (rtmp_push_frame(frame_copy, frm_width * 2) < 0)
        {
            fprintf(stderr, "rtmp_push frame failed\n");
            request_stop();
            break;
        }
    }

    free(frame_copy);
    frame_copy = NULL;
    return NULL;

}


/* 线程3 status */
static void *status_thread(void *arg)
{
    device_status_t status;
    char json[512] = {0};

    while(1)
    {
        // 读取stop加锁
        pthread_mutex_lock(&latest_frame.mutex);
        int stop = latest_frame.stop;
        pthread_mutex_unlock(&latest_frame.mutex);

        if (stop == 1)
        {
            break;
        }

        float now_fps = fps;

        if (get_device_status(&status, now_fps, v4l2_fd) < 0)
        {
            fprintf(stderr,
            "get device status failed\n");
            request_stop();
            break;
        }

        if (status_to_json(&status, json, sizeof(json)) < 0)
        {
            fprintf(stderr,
            "status_to_json failed\n");
            request_stop();
            break;
        }

        // HTTP 路径应该以 / 开头：
        static char path[24] = {"/api/device/status/"};
        if (send_status_http("106.14.2.164", 80, path, json) < 0)
        {
            printf("send http failed\n");
            sleep(2);
            continue;
        }
    }
    return NULL;
}


/*
 * 释放资源
 */
 static void cleanup(void)
 {
    unsigned int i;

    v4l2_stream_off();

    /*
     * 解除摄像头 buffer mmap
     */
    for (i = 0; i < buf_count; i++)
    {
        if (buf_infos[i].start != NULL && buf_infos[i].start != MAP_FAILED)
        {
            munmap(buf_infos[i].start, buf_infos[i].length);
            buf_infos[i].start = NULL;
        }
    }

    /*
     * 解除 LCD mmap
     */
    if (screen_base != NULL && screen_base != MAP_FAILED)
    {
        munmap(screen_base, screen_size);

        screen_base = NULL;
    }

    if (v4l2_fd >= 0)
    {
        close(v4l2_fd);
        v4l2_fd = -1;
    }

    if (fb_fd >= 0)
    {
        close(fb_fd);
        fb_fd = -1;
    }
 }


int main(int argc, char *argv[])
{

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <video_dev>\n", argv[0]);

        exit(EXIT_FAILURE);
    }

    /*
     * 初始化 LCD
     */
    if (fb_dev_init() < 0)
    {
        cleanup();
        exit(EXIT_FAILURE);
    }

    /*
     * 初始化摄像头
     *
     * 例如：
     * ./camera /dev/video1
     */
    if (v4l2_dev_init(argv[1]) < 0)
    {
        cleanup();
        exit(EXIT_FAILURE);
    }

    /*
     * 枚举所有格式
     */
    v4l2_enum_formats();

    /*
     * 打印摄像头支持的：
     *
     * 像素格式
     * 分辨率
     * 帧率
     */
    v4l2_print_formats();

    /*
     * 设置视频格式：
     *
     * RGB565
     * LCD 分辨率
     * 30 FPS
     */
    if (v4l2_set_format() < 0)
    {
        cleanup();
        exit(EXIT_FAILURE);
    }

    /*
     * 初始化帧缓冲：
     *
     * 1. VIDIOC_REQBUFS
     * 2. VIDIOC_QUERYBUF
     * 3. mmap
     * 4. VIDIOC_QBUF
     */
    if (v4l2_init_buffer() < 0)
    {
        cleanup();
        exit(EXIT_FAILURE);
    }  

    /*
     * 初始化
     */
    if (latest_frame_init() < 0)
    {
    cleanup();
    exit(EXIT_FAILURE);
    }

    pthread_t capture_tid;
    pthread_t ffmpeg_tid;
    pthread_t status_tid;


    /*
     * 开启视频采集
     */
    if (v4l2_stream_on() < 0)
    {
        cleanup();
        exit(EXIT_FAILURE);
    }

    /*
    * Thread 1:
    *
    * 摄像头 + LCD
    */
    if (pthread_create(&capture_tid, NULL, capture_lcd_thread, NULL) != 0)
    {
        perror("pthread_create capture");

        request_stop();

        cleanup();

        exit(EXIT_FAILURE);
    }


    /*
    * Thread 2:
    *
    * 推流ffmpeg
    */
    if (pthread_create(&ffmpeg_tid, NULL, ffmpeg_thread, NULL) != 0)
    {
        perror("pthread_create ffmpeg");

        request_stop();

        pthread_join(capture_tid, NULL);

        cleanup();

        exit(EXIT_FAILURE);
    }

    /*
    * Thread 3:
    *
    * 发送设备状态信息
    */
    if (pthread_create(&status_tid, NULL, status_thread, NULL) != 0)
    {
        perror("pthread_create status");

        request_stop();

        // 三个线程的pthread_join不同，
        // 因为pthread_join() 只能等待已经成功创建出来的线程
        pthread_join(capture_tid, NULL);
        pthread_join(ffmpeg_tid, NULL);


        cleanup();
        
        exit(EXIT_FAILURE);
    }



    /*
    * 等待三个线程
    */
    pthread_join(capture_tid, NULL);
    pthread_join(ffmpeg_tid, NULL);
    pthread_join(status_tid, NULL);

    pthread_mutex_destroy(&latest_frame.mutex);
    pthread_cond_destroy(&latest_frame.cond);

    free(latest_frame.data);

    latest_frame.data = NULL;

    cleanup();

    return EXIT_SUCCESS;
}