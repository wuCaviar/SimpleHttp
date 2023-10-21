#include <stdio.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#include "Server.h"

int initListenFd(unsigned short port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }
    struct sockaddr_in addr;            // IPv4地址结构体
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_port = htons(port);        // 端口号
    addr.sin_addr.s_addr = INADDR_ANY;  // IP地址
    ret = bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        perror("bind");
        return -1;
    }
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }
    return lfd;
}

int epollRun(int lfd)
{
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }
    struct epoll_event ev;  // epoll事件结构体
    ev.data.fd = lfd;       // 监听的fd
    ev.events = EPOLLIN;    // 监听读事件
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    struct epoll_event evs[1024];                         // 用于回传代处理事件的数组
    int size = sizeof(evs) / sizeof(struct epoll_event);  // 数组大小
    while (1) {
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; ++i) {
            int fd = evs[i].data.fd;  // 事件对应的fd
            if (fd == lfd) {
                accepClient(lfd, epfd);
            } else {
                recvHttpRequset(fd, epfd);
            }
        }
    }
    return 0;
}

int accepClient(int lfd, int epfd)
{
    int cfd = accept(lfd, NULL, NULL);
    if (cfd == -1) {
        perror("accept");
        return -1;
    }
    int flag = fcntl(cfd, F_GETFL);  // 获取文件状态标志
    flag |= O_NONBLOCK;              // 设置为非阻塞
    fcntl(cfd, F_SETFL, flag);       // 设置文件状态标志
    struct epoll_event ev;          // epoll事件结构体
    ev.data.fd = cfd;               // 监听的fd
    ev.events = EPOLLIN | EPOLLET;  // 监听读事件
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
}

int recvHttpRequset(int cfd, int epfd)
{
    printf("recvHttpRequset\n");
    char buf[4096] = {0};    // 接收缓冲区
    char tmp[1024] = {0};    // 临时缓冲区
    int len = 0, total = 0;  // 读取的长度、总长度
    while ((len = recv(cfd, tmp, sizeof(tmp), 0)) > 0) {
        if (total + len < sizeof(buf)) {
            memcpy(buf + total, tmp, len);  // 拷贝数据
        }
        total += len;
    }
    if (len == -1 && errno == EAGAIN) {
        char *ptr = strstr(buf, "\r\n");  // 查找\r\n
        int reqLen = ptr - buf;  // 请求行的长度
        buf[reqLen] = '\0';      // 添加\0
        parseRequestLine(buf, cfd);  // 解析请求行
    } else if (len == 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);  // 从epoll树上删除cfd
        close(cfd);                                 // 关闭cfd
    } else {
        perror("recv");
        return -1;
    }
    return 0;
}

int parseRequestLine(const char *line, int cfd)
{
    char method[12] = {0};  // 方法
    char path[1024] = {0};  // 路径
    sscanf(line, "%[^ ] %[^ ]", method, path);
    if (strcasecmp(method, "GET") != 0) {
        return -1;
    }
    decodeMeg(path, path);
    char *file = NULL;
    if (strcmp(path, "/") == 0) {
        file = "./";
    } else {
        file = path + 1;  // 去掉path前面的/ file = "xxx/1.jpg"
    }
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1) {
        senfHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);  // 发送响应头
        sendFile("./404.html", cfd);                                   // 发送404页面
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        senfHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);  // 发送响应头
        sendDir(file, cfd);                                     // 发送目录
    } else {
        senfHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);  // 发送响应头
        sendFile(file, cfd);                                         // 发送文件
    }
    return 0;
}

int sendFile(const char *fileName, int cfd)
{
    int fd = open(fileName, O_RDONLY);
    assert(fd > 0);  // 断言
#if 0
    while (1) {
        char buf[1024] = {0};                  // 读取缓冲区
        int len = read(fd, buf, sizeof(buf));  // 读取文件
        if (len > 0) {
            // 发送读取的数据
            send(cfd, buf, len, 0);
            usleep(10);  // 休眠10微秒 *非常重要*
        } else if (len == 0) {
            break;
        } else {
            perror("read");
        }
    }
#else
    off_t offset = 0;  // 偏移量
    int size = lseek(fd, 0, SEEK_END);  // 获取文件大小
    lseek(fd, 0, SEEK_SET);             // 设置偏移量为文件开头
    while (offset < size) {
        int ret = sendfile(cfd, fd, &offset, size - offset);
        printf("ret = %d\n", ret);
        if (ret == -1 && errno == EAGAIN) {
            printf("没有数据...\n");
            return -1;
        }
    }
#endif
    close(fd);  // 关闭文件
    return 0;
}

int senfHeadMsg(int cfd, int status, const char *descr, const char *type, int length)
{
    char buf[4096] = {0};
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, descr);
    sprintf(buf + strlen(buf), "Content-Type:%s\r\n", type);
    sprintf(buf + strlen(buf), "Content-Length:%d\r\n\r\n", length);
    send(cfd, buf, strlen(buf), 0);  // 发送
    return 0;
}

const char *getFileType(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain;charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html;charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";
    return "text/plain;charset=utf-8";
}

int sendDir(const char *dirName, int cfd)
{
    char buf[4096] = {0};
    sprintf(buf, "<html><head><title>目录名: %s</title></head><body><table>", dirName);
    struct dirent **namelist;  // 目录项结构体数组
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; i++) {
        char *name = namelist[i]->d_name;  // 取出文件名
        struct stat st;
        char subPath[1024] = {0};
        sprintf(subPath, "%s/%s", dirName, name);  // 拼接子路径
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode)) {
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        } else {
            sprintf(buf + strlen(buf), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        send(cfd, buf, strlen(buf), 0);  // 发送
        memset(buf, 0, sizeof(buf));     // 清空buf
        free(namelist[i]);               // 释放namelist[i]
    }
    sprintf(buf, "</table></body></html>");  // 拼接
    send(cfd, buf, strlen(buf), 0);          // 发送
    free(namelist);                          // 释放namelist
    return 0;
}

int hexToDec(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

void decodeMeg(char *to, char *from)
{
    for (; *from != '\0'; ++to, ++from) {
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);
            from += 2;
        } else {
            *to = *from;
        }
    }
    *to = '\0';
}
