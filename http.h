#ifndef HTTP_H
#define HTTP_H

int send_status_http(
    const char *server_ip,
    int server_port,
    const char *path,
    const char *json
);

#endif