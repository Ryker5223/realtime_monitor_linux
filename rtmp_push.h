#ifndef RTMP_PUSH_H
#define RTMP_PUSH_H

#include <stdint.h>

/*
 * 初始化 RTMP 推流
 *
 * url:
 *   rtmp://192.168.1.100/live/cam1
 *
 * width/height:
 *   摄像头分辨率
 *
 * fps:
 *   帧率
 */
int rtmp_init(const char *url,
              int width,
              int height,
              int fps);


/*
 * 推送一帧 RGB565 图像
 *
 * data:
 *   V4L2 mmap 得到的一帧地址
 *
 * stride:
 *   一行占多少字节
 *
 * RGB565 通常：
 *   stride = width * 2
 */
int rtmp_push_frame(const uint8_t *data, int stride);


/*
 * 结束推流并释放资源
 */
void rtmp_close(void);

#endif