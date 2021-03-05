#ifndef _EPOLL_H_
#define _EPOLL_H_

#include "netpack.h"
#include <sys/epoll.h>
#include <sys/time.h>
#include <time.h>

inline const char * TimeFormat()
{
    struct timeval stCurTimeVal;
    gettimeofday(&stCurTimeVal, NULL);

    static char szBuf[128];
    time_t tv_sec = stCurTimeVal.tv_sec;
    time_t tv_usec = stCurTimeVal.tv_usec;
    struct tm * pTm = localtime(&tv_sec);

    int sz = strftime(szBuf, sizeof(szBuf), "%Y-%m-%d %H:%M:%S", pTm);
    snprintf(szBuf + sz, sizeof(szBuf) - sz, ".%u", tv_usec);
    return szBuf;
}

#define LOG_DBG(format,...) printf("[%s][%s:%d] ", TimeFormat(), __FILE__, __LINE__); printf(format, ##__VA_ARGS__); printf("\n"); fflush(stdout)

//打包
char* pack_data(const char *buffer,unsigned short len);

//释放所有 epoll
void release_epoll(cepoll_data *data);

//创建 epoll
cepoll_data* create_epoll();
//监听端口
socket_data* listen_sock(cepoll_data *data,const char *ip,int port,int opaque);

//连接 socket
socket_data* connect_sock(cepoll_data *data,const char *ip,int port);

//发送数据
void send_data(cepoll_data *data,socket_data* sock_data,char *buffer,int len);

//循环
void server_loop(cepoll_data *data,epoll_event *evs,int max_count);

#endif