#include "status.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>


/*
 * CPU 原始统计结构
 *
 * 这个结构体只在 status.c 内部使用，
 * 所以不需要放到 status.h    long long 占8字节
 */
typedef struct
{
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;

} cpu_stat_t;


/*
 * 这个函数也只供 status.c 内部使用。
 * 所以使用 static
 */
static int read_cpu_stat(cpu_stat_t *stat)
{
    FILE *fp;

    fp = fopen("/proc/stat", "r");

    if (fp == NULL)
    {
        perror("fopen /proc/stat");
        return -1;
    }

    int ret = fscanf(
        fp,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &stat->user,
        &stat->nice,
        &stat->system,
        &stat->idle,
        &stat->iowait,
        &stat->irq,
        &stat->softirq,
        &stat->steal
    );

    fclose(fp);

    if (ret < 4)
        return -1;

    return 0;
}


/*
 * 获取 CPU 使用率，这一小段时间里“CPU 忙碌的时间” ÷ “CPU 总时间” × 100%
 */
float get_cpu_usage(void)
{
    cpu_stat_t s1;
    cpu_stat_t s2;

    if (read_cpu_stat(&s1) < 0)
        return -1.0f;

    usleep(200000);

    if (read_cpu_stat(&s2) < 0)
        return -1.0f;

    // 空闲时间 = idle + iowait
    unsigned long long idle1 = s1.idle + s1.iowait;

    unsigned long long idle2 = s2.idle + s2.iowait;

    unsigned long long non_idle1 =
        s1.user +
        s1.nice +
        s1.system +
        s1.irq +
        s1.softirq +
        s1.steal;

    unsigned long long non_idle2 =
        s2.user +
        s2.nice +
        s2.system +
        s2.irq +
        s2.softirq +
        s2.steal;

    unsigned long long total1 = idle1 + non_idle1;

    unsigned long long total2 = idle2 + non_idle2;

    unsigned long long total_delta = total2 - total1;

    unsigned long long idle_delta = idle2 - idle1;

    if (total_delta == 0)
        return 0.0f;

    return 100.0f *
           (float)(total_delta - idle_delta) /
           (float)total_delta;
}

/*
 * 获取 CPU 温度
 */
float get_cpu_temp(void)
{
    FILE *fp;

    fp = fopen(
        "/sys/class/thermal/thermal_zone0/temp",
        "r"
    );


    if (fp == NULL)
    {
        perror("open cpu temp");
        return -1.0f;
    }

    int temp;

    if (fscanf(fp, "%d", &temp) != 1)
    {
        fclose(fp);
        return -1.0f;
    }

    fclose(fp);


    return temp / 1000.0f;
}


/*
 * 获取 RAM 使用率
 */
float get_ram_usage(void)
{
    FILE *fp;

    fp = fopen("/proc/meminfo", "r");

    if (fp == NULL)
    {
        perror("fopen /proc/meminfo");
        return -1.0f;
    }

    char line[256];

    unsigned long mem_total = 0;
    unsigned long mem_available = 0;
    unsigned long mem_free = 0;
    unsigned long buffers = 0;
    unsigned long cached = 0;

    // 从文件 fp 中读取一整行字符串，保存到 line 数组里
    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1)
        {
            continue;
        }


        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1)
        {
            continue;
        }

        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1)
        {
            continue;
        }

        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1)
        {
            continue;
        }

        if (sscanf(line, "Cached: %lu kB", &cached) == 1)
        {
            continue;
        }
    }
    fclose(fp);

    if (mem_total == 0)
        return -1.0f;

    /*
     * 新内核
     */
    if (mem_available > 0)
    {
        return 100.0f *
               (float)(mem_total - mem_available) /
               (float)mem_total;
    }

    /*
     * 老内核兼容
     */
    unsigned long used =mem_total - mem_free - buffers - cached;

    return 100.0f *
           (float)used /
           (float)mem_total;
}

/*
 * 获取 uptime
 */
unsigned long get_uptime(void)
{
    FILE *fp;

    fp = fopen("/proc/uptime", "r");

    if (fp == NULL)
        return 0;

    double uptime;

    if (fscanf(fp, "%lf", &uptime) != 1)
    {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    return (unsigned long)uptime;
}


/*
 * 检查摄像头设备是否存在
 */
int camera_is_online(const char *device, int v4l2_fd)
{
    if (v4l2_fd > 0)
        return 1;

    return 0;
}

/*
 * 统一采集
 */
int get_device_status(
    device_status_t *status,
    float fps,
    int v4l2_fd
)

{
    if (status == NULL)
        return -1;

    status->cpu_usage = get_cpu_usage();

    status->cpu_temp = get_cpu_temp();

    status->ram_usage = get_ram_usage();

    status->fps = fps;

    status->camera_online = camera_is_online("/dev/video1", v4l2_fd);

    status->uptime = get_uptime();

    return 0;
}


/*
 * 转换成 JSON
 */
int status_to_json(
    const device_status_t *status,
    char *buf,
    unsigned int buf_size
)
{
    if (status == NULL ||
        buf == NULL ||
        buf_size == 0)
    {
        return -1;
    }


    int ret = snprintf(
        buf,
        buf_size,
        "{"
        "\"device_id\":\"imx6ull_001\","
        "\"online\":true,"
        "\"cpu_usage\":%.1f,"
        "\"cpu_temp\":%.1f,"
        "\"ram_usage\":%.1f,"
        "\"fps\":%.1f,"
        "\"camera\":\"%s\","
        "\"uptime\":%lu"
        "}",
        status->cpu_usage,
        status->cpu_temp,
        status->ram_usage,
        status->fps,
        status->camera_online
            ? "online"
            : "offline",
        status->uptime
    );


    if (ret < 0 ||
        (unsigned int)ret >= buf_size)
    {
        return -1;
    }

    return 0;
}