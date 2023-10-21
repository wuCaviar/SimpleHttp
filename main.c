#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "Server.h"

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("./a.out port path\n");
        return -1;
    }
    // 获取端口号
    unsigned short port = atoi(argv[1]);
    // 切换服务器的工作目录
    chdir(argv[2]);
    // 初始化监听的套接字
    int lfd = initListenFd(port);
    // 启动服务器程序
    epollRun(lfd);

    return 0;
}