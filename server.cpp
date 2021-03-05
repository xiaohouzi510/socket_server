#include <stdio.h>
#include <pthread.h>
#include "socket_server.h"

using namespace std;

const char *g_ip = "0.0.0.0";
const int g_port = 8888;
cepoll_data *g_epoll;
map<uint32_t,pack_queue*> g_read_queue;

//收到数据回调
void recv_data_cb(cepoll_data *data,socket_data* sock_data)
{
	netpack *pack = NULL;
	while((pack=pop_queue(&sock_data->m_read_queue)))
	{
		LOG_DBG("recv ip=%s port=%d data=[%s]",sock_data->m_ip,sock_data->m_port,pack->m_buffer);
		char *send_buffer = pack_data(pack->m_buffer,pack->m_len);
		send_data(data,sock_data,send_buffer,pack->m_len + 2);
		delete [] pack->m_buffer;
		pack->m_buffer = NULL;
	}
}

//网络线程
void *socket_loop(void *arg) 
{
	epoll_event evs[EPOLL_EVENT_MAX];
	while(true)
	{
		server_loop(g_epoll,evs,EPOLL_EVENT_MAX);
	}
	release_epoll(g_epoll);
	return NULL;
}

//et epoll
int main(int argc,char *argv[])
{
	g_epoll = create_epoll();
	g_epoll->re_cb = recv_data_cb;
	listen_sock(g_epoll,g_ip,g_port,1);
	pthread_t pid;
	pthread_create(&pid,NULL,socket_loop,NULL);
	pthread_join(pid, NULL); 
	return 0;
}