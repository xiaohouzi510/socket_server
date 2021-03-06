#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "socket_server.h"

using namespace std;

netpack* push_data(pack_queue *q,char *buffer,unsigned int len,bool clone);

const char *g_ip = "0.0.0.0";
const int g_port = 8888;
//epoll 管理器
cepoll_data *g_epoll;
//工作线程读队列
pack_queue g_read_queue;
//读队列锁
spinlock g_read_lock;

//收到数据回调
void recv_data_cb(cepoll_data *data,socket_data* sock_data,char *buff,int n)
{
	netpack *pack = NULL;
	filter_data(&sock_data->m_read_queue,buff,n);
	spinlock_lock(&g_read_lock);
	while((pack=pop_queue(&sock_data->m_read_queue)))
	{
		netpack* new_pack = push_data(&g_read_queue,pack->m_buffer,pack->m_len,false);
		new_pack->m_id = sock_data->m_id;
	}
	spinlock_unlock(&g_read_lock);
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

//工作线程
void *work_loop(void *arg)
{
	netpack *pack = NULL;
	while(true)
	{
		spinlock_lock(&g_read_lock);
		while((pack=pop_queue(&g_read_queue)))
		{
			LOG_DBG("recv client id=%d data=[%s]",pack->m_id,pack->m_buffer);
			char *send_buffer = pack_data(pack->m_buffer,pack->m_len);
			send_data(g_epoll,pack->m_id,send_buffer,pack->m_len + 2);
			delete [] pack->m_buffer;
		}
		spinlock_unlock(&g_read_lock);
		usleep(2500);
	}
	return NULL;
}

//et epoll
int main(int argc,char *argv[])
{
	g_epoll = create_epoll();
	g_epoll->re_cb = recv_data_cb;
	listen_sock(g_epoll,g_ip,g_port,1);

	//网络线程
	pthread_t socket_pid;
	pthread_create(&socket_pid,NULL,socket_loop,NULL);

	//工作线程
	pthread_t work_pid;
	pthread_create(&work_pid,NULL,work_loop,NULL);

	//主线程等待
	pthread_join(socket_pid, NULL); 
	pthread_join(work_pid, NULL); 
	return 0;
}