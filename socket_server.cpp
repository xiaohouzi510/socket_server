#include <stdio.h>
#include <string.h>
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

#define MAX_BUFF_LEN 65535*4

static void spinlock_lock(spinlock *lock) 
{
	while (__sync_lock_test_and_set(&lock->lock,1)){}
}

static void spinlock_unlock(spinlock *lock) 
{
	__sync_lock_release(&lock->lock);
}

//打包
char* pack_data(const char *buffer,unsigned short len)
{
	static char temp[512];
	memcpy(temp,&len,sizeof(len));
	memcpy(temp+sizeof(len),buffer,len);
	return temp;
}

//释放写缓存
static void release_write_buffer(wb_list *list)
{
	while(list->m_head != NULL)
	{
		write_buffer *temp = list->m_head;
		list->m_head = temp->m_next;
		delete temp;
	}
}

//释放读缓存
static void release_read_buffer(pack_queue *q)
{
	netpack *t_pack = NULL;
	while((t_pack=pop_queue(q)))
	{
		delete [] t_pack->m_buffer;
	}
	delete [] q->m_queue;
}

//生成 id
static uint32_t reserve_id(cepoll_data *data)
{
	++data->m_alloc_id;
	if(data->m_alloc_id == 0)
	{
		data->m_alloc_id = 1;
	}
	return data->m_alloc_id;
}

//释放所有 epoll
void release_epoll(cepoll_data *data)
{
	map<int,socket_data*>::iterator it = data->m_fds.begin();
	for(;it != data->m_fds.end();++it)
	{
		close(it->second->m_fd);	
		release_read_buffer(&it->second->m_read_queue);
		release_write_buffer(&it->second->m_write_list);
		delete it->second;
	}
	data->m_fds.clear();
	if(data->m_epoll_fd > 0)
	{
		close(data->m_epoll_fd);
	}
	data->m_epoll_fd = 0;
	delete data;
}

//加入 epoll
static int epoll_add(cepoll_data *data,int sock,void *ud)
{
	epoll_event ev;
	ev.data.ptr = ud;
	ev.events   = EPOLLIN;
	if(epoll_ctl(data->m_epoll_fd,EPOLL_CTL_ADD,sock,&ev) == -1)
	{
		LOG_DBG("epoll add error %s",strerror(errno));
		return -1;
	}
	return 0;
}

static void epoll_write(int efd, int sock, void *ud, bool enable) 
{
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}

//从 epoll 中删除
static void epoll_delete(cepoll_data *data, int sock) 
{
	epoll_ctl(data->m_epoll_fd,EPOLL_CTL_DEL,sock,NULL);
}

//设置 fd reuse
static int set_reuse(int sock)
{
	//设置 reuseaddr
	int reuse = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))== -1) 
	{
		LOG_DBG("listen socket addr reuse error %s",strerror(errno));
		return -1;
	}	
	return 0;
}

//根据 socket 获得本机 ip、端口
static void get_local_ip_port(int sock,const char **local_ip,int &local_port)
{
	sockaddr_in addr; 
	socklen_t len = sizeof(addr);
	if(getsockname(sock,(sockaddr*)&addr,&len) == -1)
	{
		LOG_DBG("get local addr error=%s",strerror(errno));
		return;
	}
	static char temp[64];
	local_port = ntohs(addr.sin_port);		
	inet_ntop(AF_INET,(void*)&addr.sin_addr,temp,sizeof(temp));
	*local_ip = temp;
}

//根据 socket 获得远程 ip、端口
static void get_remote_ip_port(int sock,const char **remote_ip,int &remote_port)
{
	sockaddr_in addr; 
	socklen_t len = sizeof(addr);
	if(getpeername(sock,(sockaddr*)&addr,&len) == -1)
	{
		LOG_DBG("get remote addr error=%s",strerror(errno));
		return;
	}
	static char temp[64];
	remote_port = ntohs(addr.sin_port);		
	inet_ntop(AF_INET,(void*)&addr.sin_addr,temp,sizeof(temp));
	*remote_ip = temp;
}

//关闭一个 socket
static bool close_sock(cepoll_data *data,int id,const char *error)
{
	map<int,socket_data*>::iterator it = data->m_fds.find(id);
	if(it == data->m_fds.end())
	{
		LOG_DBG("not find id=%d error=%s",id,error);
		return false;
	}

	LOG_DBG("close id=%d sock=%d ip=%s port=%d error=\"%s\"",it->second->m_id,it->second->m_fd,it->second->m_ip,it->second->m_port,error);
	if(it->second->m_type != esock_invalid)
	{
		epoll_delete(data,it->second->m_fd);
	}
	close(it->second->m_fd);
	delete it->second;
	data->m_fds.erase(it);
	
	return true;
}

//创建一个 socket data
static socket_data* create_sock_data(cepoll_data *data,int sock,const char *ip,int port)
{
	socket_data *fd_data = new socket_data(); 
	uint32_t id = reserve_id(data);
	fd_data->m_fd = sock;
	fd_data->m_id = id;
	data->m_fds[id] = fd_data;
	strcpy(fd_data->m_ip,ip);
	fd_data->m_port = port;
	return fd_data;
}

//发送 tcp 数据
static void write_tcp_list(cepoll_data *data,socket_data *sock_data)
{
	wb_list *wr = &sock_data->m_write_list;
	while(wr->m_head != NULL)	
	{
		write_buffer *temp = wr->m_head;	
		for(;;)
		{
			ssize_t sz = write(sock_data->m_fd,temp->m_ptr,temp->m_len);
			if(sz < 0)
			{
				switch(errno)
				{
				case EINTR:
					continue;
				case EAGAIN:
					return;
				}
				char sz_error[64];
				sprintf(sz_error,"write_tcp_list [%s] errno=%d",strerror(errno),errno);
				close_sock(data,sock_data->m_fd,sz_error);
			}
			if(temp->m_len != sz)
			{
				temp->m_ptr += sz;
				temp->m_len -= sz;
				return;
			}
			break;
		}
		spinlock_lock(&wr->m_lock);
		wr->m_head = temp->m_next;
		spinlock_unlock(&wr->m_lock);
		delete  [] temp->m_buffer;
		delete temp;
	}
	spinlock_lock(&wr->m_lock);
	epoll_write(data->m_epoll_fd,sock_data->m_fd,sock_data,false);
	wr->m_tail = NULL;
	spinlock_unlock(&wr->m_lock);
}

//接收 tcp 数据
static void read_tcp_data(cepoll_data *data,socket_data *sock_data)
{
	static char temp_buff[MAX_BUFF_LEN];
	int n = read(sock_data->m_fd,temp_buff,sock_data->m_recv_size);
	if(n < 0)
	{
		switch(errno)
		{
		case EINTR:
			break;
		case EAGAIN:
			break;	
		default:
			char sz_error[64];
			sprintf(sz_error,"read data err [%s] errno=%d",strerror(errno),errno);
			close_sock(data,sock_data->m_fd,sz_error);
			break;
		}
		return ;
	}
	if(n == 0)
	{
		char sz_error[64];
		sprintf(sz_error,"read data zero [%s] errno=%d",strerror(errno),errno);
		close_sock(data,sock_data->m_fd,sz_error);
		return;
	}
	if(n == sock_data->m_recv_size)
	{
		sock_data->m_recv_size *= 2;
	}
	else if(sock_data->m_recv_size > MIN_READ_BUFFER && n*2 < sock_data->m_recv_size)
	{
		sock_data->m_recv_size /=2;
	}
	if(sock_data->m_recv_size > MAX_BUFF_LEN)
	{
		sock_data->m_recv_size = MAX_BUFF_LEN;
	}
	filter_data(&sock_data->m_read_queue,temp_buff,n);
	if(data->re_cb != NULL)
	{
		(*data->re_cb)(data,sock_data);
	}
}

//创建 epoll
cepoll_data* create_epoll()
{
	//创建 epoll
	int epoll_fd = epoll_create(1024);
	if(epoll_fd == -1)
	{
		LOG_DBG("create epoll error=%s",strerror(errno));
		return NULL;
	}	
	cepoll_data *data = new cepoll_data();
	data->m_epoll_fd = epoll_fd;
	return data;
}

//监听端口
socket_data* listen_sock(cepoll_data *data,const char *ip,int port,int opaque)
{
	addrinfo source_addr;
	addrinfo *target_addr = NULL;
	char port_str[16];
	sprintf(port_str,"%d",port);
	memset(&source_addr,0,sizeof(source_addr));
	source_addr.ai_socktype = SOCK_STREAM;
	source_addr.ai_protocol = IPPROTO_TCP;
	source_addr.ai_family   = AF_UNSPEC;
	int status = getaddrinfo(ip,port_str,&source_addr,&target_addr);
	if(status != 0)
	{
		freeaddrinfo(target_addr);
		LOG_DBG("get addr error=%s",gai_strerror(status));
		return NULL;
	}
	//创建 socket
	int listen_fd = socket(target_addr->ai_family,target_addr->ai_socktype,0);
	if(listen_fd == -1)
	{
		freeaddrinfo(target_addr);
		LOG_DBG("create socket error %s",strerror(errno));
		return NULL;
	}
	set_reuse(listen_fd);	
	//绑定
	char sz_error[64]; 
	status = bind(listen_fd,(sockaddr *)target_addr->ai_addr,target_addr->ai_addrlen);
	sockaddr_in *temp_addr = (sockaddr_in *)target_addr->ai_addr;
	inet_ntop(target_addr->ai_family,(void*)&temp_addr->sin_addr,sz_error,sizeof(sz_error));				
	socket_data* sock_data = create_sock_data(data,listen_fd,sz_error,ntohs(temp_addr->sin_port));
	freeaddrinfo(target_addr);
	if(status != 0)
	{
		sprintf(sz_error,"bind [%s]",strerror(errno));
		close_sock(data,listen_fd,sz_error);
		return NULL;
	}
	//监听端口
	status = listen(sock_data->m_fd,32);
	if(status != 0)
	{
		sprintf(sz_error,"listen [%s]",strerror(errno));
		close_sock(data,listen_fd,sz_error);
		return NULL;
	}
	//加入 epoll
	if(epoll_add(data,sock_data->m_fd,sock_data) == -1)
	{
		sprintf(sz_error,"epoll add [%s]",strerror(errno));
		close_sock(data,listen_fd,sz_error);
		return NULL;	
	}
	sock_data->m_type = esock_listen;
	sock_data->m_opaque = opaque;
	LOG_DBG("listen success sock=%d ip=%s port=%d",listen_fd,sock_data->m_ip,sock_data->m_port);
	return sock_data;
}

//设置 keepalive
static void set_keepalive(int sock) 
{
	int keepalive = 1;
	setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

//设置非阻塞
static void set_nonblocking(int sock) 
{
	int flag = fcntl(sock, F_GETFL, 0);
	if ( -1 == flag ) 
	{
		LOG_DBG("set non block error=%s",strerror(errno));
		return;
	}

	fcntl(sock, F_SETFL, flag | O_NONBLOCK);
}

//连接成功
static void connect_success(cepoll_data *data,socket_data* sock_data,const char *param)
{
	int keepalive = 1;
	socklen_t optlen = sizeof(keepalive);
	getsockopt(sock_data->m_fd, SOL_SOCKET, SO_KEEPALIVE,&keepalive,&optlen);
	sock_data->m_type = esock_connected;
	const char *local_ip = NULL;
	int local_port = 0;
	const char *remote_ip = NULL;
	int remote_port = 0;
	get_local_ip_port(sock_data->m_fd,&local_ip,local_port);
	get_remote_ip_port(sock_data->m_fd,&remote_ip,remote_port);
	LOG_DBG("connect success param=%s local_ip=%s local_port=%d remote_ip=%s remote_port=%d kiipalive=%d",param,local_ip,local_port,remote_ip,remote_port,keepalive);
}

//连接 socket
socket_data* connect_sock(cepoll_data *data,const char *ip,int port)
{
	addrinfo source_addr;
	addrinfo *target_addr = NULL;
	addrinfo *temp_addr   = NULL;
	char port_str[16];
	sprintf(port_str,"%d",port);
	memset(&source_addr,0,sizeof(source_addr));
	source_addr.ai_family   = AF_UNSPEC;
	source_addr.ai_socktype = SOCK_STREAM;
	source_addr.ai_protocol = IPPROTO_TCP;
	int status = getaddrinfo(ip,port_str,&source_addr,&target_addr);
	if(status != 0)
	{
		LOG_DBG("connect get addr error=%s",gai_strerror(status));
		freeaddrinfo(target_addr);
		return NULL;
	}
	int sock = -1;
	socket_data* sock_data = NULL;
	char sz_error[64]; 
	for(temp_addr = target_addr;temp_addr != NULL;temp_addr = temp_addr->ai_next)
	{
		sock = socket(temp_addr->ai_family,temp_addr->ai_socktype,temp_addr->ai_protocol);
		if(sock == -1)
		{
			continue;
		}
		set_reuse(sock);
		set_nonblocking(sock);
		status = connect(sock,temp_addr->ai_addr,temp_addr->ai_addrlen);
		if(status != 0 && errno != EINPROGRESS)
		{
			close(sock);
			sock = -1;
			continue;
		}
		sockaddr_in *temp_in = (sockaddr_in *)temp_addr->ai_addr;
		inet_ntop(temp_addr->ai_family,(void*)&temp_in->sin_addr,sz_error,sizeof(sz_error));
		sock_data = create_sock_data(data,sock,sz_error,ntohs(temp_in->sin_port));
		break;
	}
	freeaddrinfo(target_addr);
	if(sock == -1)
	{
		LOG_DBG("connect error=%s ip=%s port=%d",strerror(errno),ip,port);
		return NULL;
	}
	//加入 epoll
	if(epoll_add(data,sock_data->m_fd,sock_data) == -1)
	{
		char sz_error[64];
		sprintf(sz_error,"epoll add connect_sock [%s]",strerror(errno));
		close_sock(data,sock_data->m_fd,sz_error);
		return NULL;
	}

	if(status == 0)
	{
		connect_success(data,sock_data,"at once");
	}
	//三次握手
	else
	{
		sock_data->m_type = esock_connecting;
		epoll_write(data->m_epoll_fd,sock_data->m_fd,sock_data,true);
		LOG_DBG("three hand fd=%d ip=%s port=%d status=%d error=[%s]",sock,sock_data->m_ip,sock_data->m_port,status,strerror(errno));
	}

	return sock_data;
}

//发送数据
void send_data(cepoll_data *data,socket_data* sock_data,char *buffer,int len)
{
	char *temp = new char[len]; 
	memcpy(temp,buffer,len);
	write_buffer *wr_bf = new write_buffer();
	wr_bf->m_buffer = temp;
	wr_bf->m_ptr = temp;
	wr_bf->m_len = len;
	spinlock_lock(&sock_data->m_write_list.m_lock);
	if(sock_data->m_write_list.m_head == NULL)
	{
		sock_data->m_write_list.m_head = wr_bf;
		sock_data->m_write_list.m_tail = wr_bf;
		epoll_write(data->m_epoll_fd,sock_data->m_fd,sock_data,true);
	}
	else
	{
		sock_data->m_write_list.m_tail->m_next = wr_bf;
		sock_data->m_write_list.m_tail = wr_bf;
	}
	spinlock_unlock(&sock_data->m_write_list.m_lock);
	LOG_DBG("send data size=%d",len);
}

//循环
void server_loop(cepoll_data *data,epoll_event *evs,int max_count)
{
	static char sz_error[64]; 
	//等待 epoll 事件
	int count = epoll_wait(data->m_epoll_fd,evs,max_count,-1);
	if(count == -1)
	{
		LOG_DBG("epoll wait error=%s",strerror(errno));	
		return;
	}
	for(int i = 0;i < count;++i)
	{
		epoll_event *cur_ev = &evs[i];
		socket_data* sock_data = (socket_data*)cur_ev->data.ptr;
		switch(sock_data->m_type)
		{
		//accept
		case esock_listen:
			{
				sockaddr_in addr_in;
				socklen_t len = sizeof(sockaddr_in);
				//接收一个 socket
				int client_fd = accept(sock_data->m_fd,(sockaddr*)&addr_in,&len);
				if(client_fd == -1)
				{
					LOG_DBG("accept error=%s",strerror(errno));
					break;
				}
				set_reuse(client_fd);
				//获取 ip 和端口
				static char temp_ip[64]; 
				inet_ntop(AF_INET,(void*)&addr_in.sin_addr,temp_ip,sizeof(temp_ip));				
				int port = ntohs(addr_in.sin_port);
				LOG_DBG("accept fd=%d ip=%s port=%d",client_fd,temp_ip,port);
				socket_data *new_sock_data = create_sock_data(data,client_fd,temp_ip,port);
				//加入 epoll
				if(epoll_add(data,client_fd,new_sock_data) == -1)
				{
					sprintf(sz_error,"accept add epoll %s %s",strerror(errno));
					close_sock(data,client_fd,sz_error);
				}
				set_nonblocking(client_fd);
				new_sock_data->m_type = esock_connected;
				sock_data->m_opaque = sock_data->m_opaque;
				break;
			}
			//三次握手成功
		case esock_connecting:
			{
				int error;
				socklen_t len = sizeof(error);  
				int code = getsockopt(sock_data->m_fd, SOL_SOCKET, SO_ERROR, &error, &len);  
				if(code == -1 || error != 0)
				{
					if(code == -1)
					{
						sprintf(sz_error,"connecting [%s]",strerror(errno));
					}
					else
					{
						sprintf(sz_error,"connecting [%s]",strerror(error));
					}
					close_sock(data,sock_data->m_fd,sz_error);
				}
				else
				{
					epoll_write(data->m_epoll_fd,sock_data->m_fd,sock_data,false);
					connect_success(data,sock_data,"three hand");
				}
				break;
			}
		case esock_connected:
			{
				//read 事件
				bool ev_read  = cur_ev->events | EPOLLIN || cur_ev->events | EPOLLHUP;
				bool ev_write = cur_ev->events | EPOLLOUT;
				bool ev_error = cur_ev->events | EPOLLERR;
				if(ev_read)
				{
					read_tcp_data(data,sock_data);
					if(!ev_write)
						break;
				}
				//write 事件
				if(ev_write)
				{
					write_tcp_list(data,sock_data);	
					break;
				}
				//错误事件
				if(!ev_error)
				{
					break;
				}
				int error;
				socklen_t len = sizeof(error); 
				//获取 socket 错误码
				int code = getsockopt(cur_ev->data.fd,SOL_SOCKET,SO_ERROR, &error, &len);
				if(code == -1)
				{
					sprintf(sz_error,"epoll err errno [%s]",strerror(errno));
				}
				else
				{
					sprintf(sz_error,"epoll err error [%s]",strerror(error));
				}
				close_sock(data,sock_data->m_fd,sz_error);
				break;
			}
		default:
			{
				LOG_DBG("fd error");	
				break;
			}
		}
	}

	return;
}