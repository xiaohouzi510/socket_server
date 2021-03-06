#include <stdio.h>
#include <pthread.h>  
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netdb.h>
#include "socket_server.h"

using namespace std;

//连接 socket 管理器
socket_data *g_connect = NULL;
//epoll 管理器
cepoll_data *g_epoll;

void *readline_stdin(void *arg) 
{
	char tmp[1024];
	while (!feof(stdin)) {
		if (fgets(tmp,sizeof(tmp),stdin) == NULL) 
		{
			// read stdin failed
			LOG_DBG("stdin error");
			break;
		}
		int n  = strlen(tmp) -1;
		tmp[n] = 0;
		char *result = pack_data(tmp,n);
		if(strcmp(tmp,"close") == 0)
		{
			printf("rst close %s\n",tmp);
			linger so_linger;
			so_linger.l_onoff = 1;
			so_linger.l_linger = 0;
			int r = setsockopt(g_connect->m_fd,SOL_SOCKET,SO_LINGER,&so_linger,sizeof(so_linger));
			if(r != 0)
			{
				printf("set sockopt linger fail=%s\n",strerror(errno));
			}
			close(g_connect->m_fd);
			continue;
		}
		send_data(g_epoll,g_connect->m_id,result,n+2);
	}
	return NULL;
}

//收到数据回调
void recv_data_cb(cepoll_data *data,socket_data* sock_data,char *buff,int n)
{
	netpack *pack = NULL;
	filter_data(&sock_data->m_read_queue,buff,n);
	while((pack=pop_queue(&sock_data->m_read_queue)))
	{
		LOG_DBG("recv server id=%d data=[%s]",sock_data->m_id,pack->m_buffer);
	}
}

//et epoll
int main(int argc,char *argv[])
{
	if(argc != 3)
	{
		LOG_DBG("param one ip,tow port");
		return 0;
	}
	const char *ip = argv[1]; 
	int port = atoi(argv[2]);
	g_epoll = create_epoll();
	g_epoll->re_cb = recv_data_cb;
	g_connect = connect_sock(g_epoll,ip,port);
	epoll_event evs[EPOLL_EVENT_MAX];
	pthread_t pid;
	pthread_create(&pid,NULL,readline_stdin,NULL);
	while(true)
	{
		server_loop(g_epoll,evs,EPOLL_EVENT_MAX);
	}
	release_epoll(g_epoll);
	return 0;	
}