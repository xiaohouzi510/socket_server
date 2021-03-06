#include "netpack.h"

//扩展队列
static void expand_queue(pack_queue *q)
{
	netpack *new_pack = new netpack[q->m_cap * 2 * sizeof(netpack)];
	for(int i = 0;i < q->m_cap;++i)
	{
		new_pack[i] = q->m_queue[q->m_head + i];
	}

	delete [] q->m_queue;
	q->m_queue = new_pack;
	q->m_head = 0;
	q->m_tail = q->m_cap;
	q->m_cap *= 2;
}

//队列中加入一个包
netpack* push_data(pack_queue *q,char *buffer,unsigned int len,bool clone)
{
	netpack *pack = &q->m_queue[q->m_tail++];
	if(q->m_tail == q->m_cap)
	{
		q->m_tail = 0;
	}
	if(clone)
	{
		char *pack_buffer = new char[len+1];
		pack_buffer[len] = 0;
		memcpy(pack_buffer,buffer,len);
		buffer = pack_buffer;
	}

	pack->m_len = len;
	pack->m_buffer = buffer;

	if(q->m_tail == q->m_head)
	{
		expand_queue(q);
	}
	return pack;
}

//读取头部大小
static unsigned short read_head_size(char *buffer)
{
	unsigned short *result = (unsigned short*)buffer;
	return *result;
}

//处理一个包剩余数据
static void push_more(pack_queue *q,char *buffer,unsigned int len)
{
	if(len == 1)
	{
		q->m_un.m_read = -1;
		q->m_un.m_header = *buffer;
		return ;
	}

	unsigned short pack_len = read_head_size(buffer);
	buffer += 2;
	len -= 2;
	if(pack_len > len)
	{
		q->m_un.m_read = len;
		q->m_un.m_pack.m_len = pack_len;
		q->m_un.m_pack.m_buffer = new char[pack_len+1];
		q->m_un.m_pack.m_buffer[pack_len] = 0;
		memcpy(q->m_un.m_pack.m_buffer,buffer,len);
		return ;
	}
	push_data(q,buffer,pack_len,true);
	buffer += pack_len;
	len    -= pack_len;
	if(len > 0)
	{
		push_more(q,buffer,len);
	}

	return ;
}

//重置未完成包
static void reset_un(pack_queue *q)
{
	q->m_un.m_pack.m_buffer = NULL;
	q->m_un.m_pack.m_len = 0;
	q->m_un.m_read = -2;
	q->m_un.m_header = 0;	
}

//粘包
void filter_data(pack_queue *q,char *buffer,unsigned int len)
{
	//没有未完成数据
	if(q->m_un.m_read == -2)
	{
		push_more(q,buffer,len);
		return;
	}
	//只读一个字节
	if(q->m_un.m_read == -1)
	{
		unsigned short hight = (unsigned short)(*buffer);
		q->m_un.m_pack.m_len = q->m_un.m_header| (hight << 8);
		q->m_un.m_header = 0;
		q->m_un.m_read = 0;	
		q->m_un.m_pack.m_buffer = new char[q->m_un.m_pack.m_len+1];
		q->m_un.m_pack.m_buffer[q->m_un.m_pack.m_len] = 0;
		len -= 1;
		buffer += 1;
	}
	unsigned int need = q->m_un.m_pack.m_len - q->m_un.m_read;
	if(need > len)
	{
		memcpy(q->m_un.m_pack.m_buffer+q->m_un.m_read,buffer,len);
		q->m_un.m_read += len;
		return ;
	}
	memcpy(q->m_un.m_pack.m_buffer+q->m_un.m_read,buffer,need);
	push_data(q,q->m_un.m_pack.m_buffer,q->m_un.m_pack.m_len,false);
	buffer += need;
	len -= need;
	reset_un(q);
	if(len == 0)
	{
		return ;
	}
	push_more(q,buffer,len);
	return ;
}

//获得一个包
netpack* pop_queue(pack_queue *q)
{
	if(q->m_tail == q->m_head)
	{
		return NULL;
	}
	netpack *pack = &q->m_queue[q->m_head++];
	if(q->m_head == q->m_cap)
	{
		q->m_head = 0;
	}

	return pack;
}