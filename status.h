#ifndef STATUS_H
#define STATUS_H


typedef struct
{
    float cpu_usage;
    float cpu_temp;
    float ram_usage;
    float fps;

    int camera_online;
    int streaming;

    unsigned long uptime;

} device_status_t;


/*
 * 获取 CPU 使用率
 */
float get_cpu_usage(void);


/*
 * 获取 CPU 温度
 */
float get_cpu_temp(void);


/*
 * 获取 RAM 使用率
 */
float get_ram_usage(void);


/*
 * 获取系统运行时间
 */
unsigned long get_uptime(void);


/*
 * 检查摄像头设备是否存在
 */
int camera_is_online(const char *device);


/*
 * 一次性采集设备状态
 */
int get_device_status(
    device_status_t *status,
    float fps,
    int streaming
);


/*
 * 将状态转换成 JSON
 */
int status_to_json(
    const device_status_t *status,
    char *buf,
    unsigned int buf_size
);


#endif