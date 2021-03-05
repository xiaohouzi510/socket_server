#ifndef _NET_PACK_H_
#define _NET_PACK_H_

#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

using namespace std;
struct socket_data;
struct cepoll_data;

typedef void (*revc_cb)(cepoll_data *data,socket_data* sock_data);
typedef void (*accept_cb)(cepoll_data *data,socket_data* sock_data);
typedef void (*connect_cb)(cepoll_data *data,socket_data* sock_data);

#define EPOLL_EVENT_MAX 64
#define MIN_READ_BUFFER 64

//socket 类型
enum esock_type
{
	esock_invalid = 0,
	esock_listen = 1,
	esock_connecting = 2,	
	esock_connected = 3,
};

//锁
struct spinlock 
{
	spinlock() : lock(0){}
	int lock;
};

//一个包数据
struct netpack
{
	netpack() : m_len(0),m_buffer(NULL){}
	unsigned short m_len;
	char *m_buffer;
};

//未完成的一个包
struct uncomplete
{
	uncomplete() : m_header(0),m_read(-2){}
	netpack m_pack;
	unsigned short m_header;
	short m_read;
};

//包队列
struct pack_queue
{
	pack_queue() : m_cap(64),m_head(0),m_tail(0),m_queue(NULL)
	{
		m_queue = new netpack[m_cap];
	}
	unsigned int m_cap;
	unsigned int m_head;
	unsigned int m_tail;
	uncomplete m_un;
	netpack *m_queue;
};

//写缓存
struct write_buffer 
{
	write_buffer():m_next(NULL),m_buffer(NULL),m_ptr(NULL),m_len(0){}
	struct write_buffer *m_next;
	char *m_buffer;
	char *m_ptr;
	unsigned short m_len;
};

//写链表
struct wb_list 
{
	wb_list() : m_head(NULL),m_tail(NULL){}
	write_buffer *m_head;
	write_buffer *m_tail;
	spinlock      m_lock;
};

//一个 socket 数据
struct socket_data
{
	socket_data()
	{
		m_fd = 0;
		m_id = 0;
		m_recv_size = MIN_READ_BUFFER;
		m_port = 0;
		m_type = esock_invalid;
		m_opaque = 0;
		m_data = NULL;
		memset(m_ip,0,sizeof(m_ip));
	}
	uint32_t m_id;
	int m_fd;
	wb_list m_write_list;
	int m_recv_size;
	char m_ip[64];
	int m_port;
	int m_type;
	int m_opaque;
	char *m_data;
};

//epoll 数据
struct cepoll_data
{
	cepoll_data() : m_epoll_fd(0),re_cb(NULL),acpt_cb(NULL),conct_cb(NULL),m_alloc_id(0){}
	int m_epoll_fd;
	revc_cb re_cb;
	accept_cb acpt_cb;
	connect_cb conct_cb;
	uint32_t m_alloc_id;
	map<int,socket_data*> m_fds;
};

//获得一个包
netpack* pop_queue(pack_queue *q);
//粘包
void filter_data(pack_queue *q,char *buffer,unsigned int len);

#endif