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

struct FdInfo {
    int fd;
    int epfd;
    pthread_t tid;
};

int initListenFd(unsigned short port)
{
    // 创建监听的fd
    /*
     * int socket(int domain, int type, int protocol);
     * domain: 协议域，AF_INET(IPv4)、AF_INET6(IPv6)、AF_LOCAL(本地通信)、AF_ROUTE(路由套接字)、AF_KEY(秘钥套接字)
     * type:
     * 套接字类型，SOCK_STREAM(字节流套接字)、SOCK_DGRAM(数据报套接字)、SOCK_PACKET(数据包套接字)、SOCK_SEQPACKET(有序分组套接字)
     * protocol: 指定协议，一般为0
     */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        return -1;
    }

    // 设置端口复用
    int opt = 1;
    /*
     * int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
     * sockfd: 套接字描述符
     * level:
     * 选项定义的层次，SOL_SOCKET(通用套接字选项)、IPPROTO_IP(IP选项)、IPPROTO_TCP(TCP选项)、IPPROTO_IPV6(IPv6选项)
     * optname: 需要访问的选项名
     * optval: 对于getsockopt()，指向返回选项值的缓冲；对于setsockopt()，指向包含新选项值的缓冲
     * optlen:
     * 对于getsockopt()，作为入口参数时，选项值的最大长度；作为出口参数时，选项值的实际长度；对于setsockopt()，选项值的长度
     */
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }

    // 绑定地址
    struct sockaddr_in addr;            // IPv4地址结构体
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_port = htons(port);        // 端口号
    addr.sin_addr.s_addr = INADDR_ANY;  // IP地址

    /*
     * int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
     * sockfd: 套接字描述符
     * addr: 指向要绑定给sockfd的协议地址
     * addrlen: addr的长度
     */
    ret = bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        perror("bind");
        return -1;
    }

    // 监听
    /*
     * int listen(int sockfd, int backlog);
     * sockfd: 套接字描述符
     * backlog: 未完成连接队列的最大长度
     */
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }

    // 返回监听的fd
    return lfd;
}

int epollRun(int lfd)
{
    // 创建epoll实例
    /*
     * int epoll_create(int size);
     * size: epoll实例的大小
     */
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }

    // lfd 上树
    struct epoll_event ev;  // epoll事件结构体
    ev.data.fd = lfd;       // 监听的fd
    ev.events = EPOLLIN;    // 监听读事件
    /*
     * int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
     * epfd: epoll实例的fd
     * op:
     * 操作类型，EPOLL_CTL_ADD(注册新的fd到epfd)、EPOLL_CTL_MOD(修改已经注册的fd的监听事件)、EPOLL_CTL_DEL(从epfd删除一个fd)
     * fd: 需要监听的fd
     * event: 监听的事件
     */
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return -1;
    }

    // 检测
    struct epoll_event evs[1024];                         // 用于回传代处理事件的数组
    int size = sizeof(evs) / sizeof(struct epoll_event);  // 数组大小
    while (1) {
        /*
         * int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
         * epfd: epoll实例的fd
         * events: 用于回传代处理事件的数组
         * maxevents: 每次能处理的事件数
         * timeout: 等待I/O事件发生的超时值，-1表示永远等待，0表示立即返回
         */
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; ++i) {
            struct FdInfo *info = (struct FdInfo *)malloc(sizeof(struct FdInfo));  // 申请内存
            int fd = evs[i].data.fd;                                               // 事件对应的fd
            info->epfd = epfd;
            info->fd = fd;
            if (fd == lfd) {
                // 建立连接 accept
                // accepClient(lfd, epfd);
                pthread_create(&info->tid, NULL, accepClient, info);
            } else {
                // 读数据 read
                // 主要是接受对端的数据
                // recvHttpRequset(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequset, info);
            }
        }
    }

    return 0;
}

// int accepClient(int lfd, int epfd)
void *accepClient(void *arg)
{
    struct FdInfo *info = (struct FdInfo *)arg;
    // 建立连接
    /*
     * int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
     * sockfd: 套接字描述符
     * addr: 指向结构体sockaddr的指针，用于存放对方（客户端）的协议地址
     * addrlen: addr的长度
     */
    int cfd = accept(info->fd, NULL, NULL);
    if (cfd == -1) {
        perror("accept");
        return NULL;
    }

    // 设置cfd为非阻塞
    int flag = fcntl(cfd, F_GETFL);  // 获取文件状态标志
    flag |= O_NONBLOCK;              // 设置为非阻塞
    fcntl(cfd, F_SETFL, flag);       // 设置文件状态标志

    // cfd添加到epoll树上
    struct epoll_event ev;          // epoll事件结构体
    ev.data.fd = cfd;               // 监听的fd
    ev.events = EPOLLIN | EPOLLET;  // 监听读事件
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptClient threadID = %ld\n", info->tid);
    free(info);  // 释放内存
    return 0;
}

// int recvHttpRequset(int cfd, int epfd)
void *recvHttpRequset(void *arg)
{
    struct FdInfo *info = (struct FdInfo *)arg;
    printf("recvHttpRequset\n");
    char buf[4096] = {0};    // 接收缓冲区
    char tmp[1024] = {0};    // 临时缓冲区
    int len = 0, total = 0;  // 读取的长度、总长度
    // 接收http请求
    while ((len = recv(info->fd, tmp, sizeof(tmp), 0)) > 0) {
        if (total + len < sizeof(buf)) {
            memcpy(buf + total, tmp, len);  // 拷贝数据
        }
        total += len;
    }
    // 判断是否读取完毕
    if (len == -1 && errno == EAGAIN) {
        // 解析请求行
        char *ptr = strstr(buf, "\r\n");  // 查找\r\n
        // ptr = "\r\nxxxx"
        int reqLen = ptr - buf;  // 请求行的长度
        buf[reqLen] = '\0';      // 添加\0
        // buf = "GET /xxx/1.jpg HTTP/1.1"
        parseRequestLine(buf, info->fd);  // 解析请求行
    } else if (len == 0) {
        // 客户端断开了连接
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);  // 从epoll树上删除cfd
        close(info->fd);                                       // 关闭cfd
    } else {
        perror("recv");
        return NULL;
    }
    printf("recvMsg threadID = %ld\n", info->tid);
    free(info);       // 释放内存
    close(info->fd);  // 关闭cfd
    return 0;
}

int parseRequestLine(const char *line, int cfd)
{
    char method[12] = {0};  // 方法
    char path[1024] = {0};  // 路径
    // line = "GET /xxx/1.jpg HTTP/1.1"
    sscanf(line, "%[^ ] %[^ ]", method, path);
    /*
        method = "GET"
        path = "/xxx/1.jpg"
    */

    printf("method = %s, path = %s\n", method, path);

    // strcasecmp()忽略大小写比较字符串
    if (strcasecmp(method, "GET") != 0) {
        return -1;
    }
    decodeMeg(path, path);
    // 处理客户端请求的静态资源(目录或文件)
    char *file = NULL;
    // 判断path是否为根目录
    if (strcmp(path, "/") == 0) {
        file = "./";
    } else {
        file = path + 1;  // 去掉path前面的/ file = "xxx/1.jpg"
    }

    // 获取文件属性
    struct stat st;
    /*
     * int stat(const char *pathname, struct stat *statbuf);
     * pathname: 文件路径
     * statbuf: 文件属性结构体
     */
    int ret = stat(file, &st);
    if (ret == -1) {
        // 文件不存在
        // 响应404
        senfHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);  // 发送响应头
        sendFile("./404.html", cfd);                                   // 发送404页面
        return 0;
    }
    // 判断文件类型
    // st_mode: 文件类型和权限
    if (S_ISDIR(st.st_mode)) {
        // 目录
        // 把这个目录中的内容发送给客户端
        senfHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);  // 发送响应头
        sendDir(file, cfd);                                     // 发送目录
    } else {
        // 文件
        // 把这个文件发送给客户端
        senfHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);  // 发送响应头
        sendFile(file, cfd);                                         // 发送文件
    }

    return 0;
}

int sendFile(const char *fileName, int cfd)
{
    // 打开文件
    /*
     * int open(const char *pathname, int flags, mode_t mode);
     * pathname: 文件路径
     * flags: 文件打开方式 O_RDONLY(只读方式打开)、O_WRONLY(只写方式打开)、O_RDWR(读写方式打开)
     * mode: 文件权限
     */
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
    /*
     * off_t lseek(int fd, off_t offset, int whence);
     * fd: 文件描述符
     * offset: 偏移量
     * whence: SEEK_SET(文件开头)、SEEK_CUR(当前位置)、SEEK_END(文件结尾)
     */
    int size = lseek(fd, 0, SEEK_END);  // 获取文件大小
    lseek(fd, 0, SEEK_SET);             // 设置偏移量为文件开头

    // 偏移量小于文件大小
    while (offset < size) {
        /*
         * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
         * out_fd: 目标文件描述符
         * in_fd: 源文件描述符
         * offset: 偏移量
         * count: 发送字节数
         */
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
    // 状态行
    char buf[4096] = {0};
    // "HTTP/1.1 200 OK\r\n"
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, descr);

    // 响应头
    sprintf(buf + strlen(buf), "Content-Type:%s\r\n", type);
    sprintf(buf + strlen(buf), "Content-Length:%d\r\n\r\n", length);

    send(cfd, buf, strlen(buf), 0);  // 发送

    return 0;
}

const char *getFileType(const char *name)
{
    // a.jpg a.mp4 a.html
    // 自右向左查找.的位置，如不存在返回NULL
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

/*
<html>
    <head>
        <title>test</title>
    </head>
    <bady>
        <table>
            <tr>
                <td></td>
                <td></td>
            </tr>
            <tr>
                <td></td>
                <td></td>
            </tr>
        </table>
    </bady>
</html>
*/
int sendDir(const char *dirName, int cfd)
{
    char buf[4096] = {0};
    /*
    <html>
        <head>
            <title>test</title>
        </head>
        <bady>
            <table>
    */
    sprintf(buf, "<html><head><title>目录名: %s</title></head><body><table>", dirName);

    struct dirent **namelist;  // 目录项结构体数组
    /*
     * int scandir(const char *dir, struct dirent ***namelist, int (*filter)(const struct dirent *),
     *             int (*compar)(const struct dirent **, const struct dirent **));
     * dir: 目录名
     * namelist: 用于存放目录项的结构体数组
     * filter: 过滤器
     * compar: 比较器
     */
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; i++) {
        // 取出文件名 namelist 指向的是一个指针数组 struct dirent **tmp[]
        char *name = namelist[i]->d_name;  // 取出文件名
        struct stat st;
        char subPath[1024] = {0};
        sprintf(subPath, "%s/%s", dirName, name);  // 拼接子路径
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode)) {
            /*
            <tr>
                <td><a href="xxx/">xxx/</a></td>
                <td>xxx</td>
            </tr>
             */
            // a标签 <a href="">name</a> 超链接
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

// 将字符转换为整形数
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

// 解码
// to 存储解码后的数据，传出参数 from 存储待解码的数据，传入参数
void decodeMeg(char *to, char *from)
{
    for (; *from != '\0'; ++to, ++from) {
        // isxdigit() 判断字符是否为十六进制数字,取值范围0-9 A-F
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            // 将16进制数转换为10进制数
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);
            from += 2;
        } else {
            // 拷贝
            *to = *from;
        }
    }
    *to = '\0';
}
