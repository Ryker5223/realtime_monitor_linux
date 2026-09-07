#include "http.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>


/*
 * 保证所有数据发送完成
 */
static int send_all(int sockfd, const char *buf, size_t len)
{
    size_t total = 0;

    while (total < len)
    {
        ssize_t ret = send(sockfd, buf + total, len - total, 0);

        if (ret <= 0)
        {
            perror("send");
            return -1;
        }

        total += ret;
    }

    return 0;
}


int send_status_http(const char *server_ip, int server_port, const char *path, const char *json)
{
    int sockfd;

    struct sockaddr_in server_addr;

    /* =========================
     * 1. 创建 TCP socket
     * ========================= */

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }

    /* =========================
     * 2. 设置服务器地址
     * ========================= */

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;

    server_addr.sin_port = htons(server_port);


    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1)
    {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }


    /* =========================
     * 3. 连接公网服务器
     * ========================= */

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return -1;
    }

    /* =========================
     * 4. 构造 HTTP 请求头
     * ========================= */
    char header[1024];

    int header_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",

        path,
        server_ip,
        strlen(json)
    );

    if (header_len < 0 || header_len >= (int)sizeof(header))
    {
        printf("HTTP header too long\n");
        close(sockfd);
        return -1;
    }


    /* =========================
     * 5. 发送 HTTP Header
     * ========================= */

    if (send_all(sockfd, header, header_len) < 0)
    {
        close(sockfd);
        return -1;
    }

    /* =========================
     * 6. 发送 JSON body
     * ========================= */
    if (send_all(sockfd, json, strlen(json)) < 0)
    {
        close(sockfd);
        return -1;
    }

    /* =========================
     * 7. 接收服务器响应
     * ========================= */
    char response[2048];

    ssize_t ret = recv(sockfd, response, sizeof(response) - 1,0);

    if (ret > 0)
    {
        response[ret] = '\0';

        printf("HTTP response:\n%s\n", response);
    }

    close(sockfd);
    return 0;
}