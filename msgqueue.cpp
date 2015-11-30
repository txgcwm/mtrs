#include "msgqueue.h"

msg_queue *initMsgQueue()
{
	msg_queue *p_queue = (msg_queue *)malloc(sizeof(msg_queue));
	if (p_queue) {
		anc_mutex_init(&p_queue->p_lock);
		p_queue->i_size = 1024;
		p_queue->p_msg = (message**)malloc(sizeof(message*) * p_queue->i_size);
		memset(p_queue->p_msg, 0, sizeof(message*) * p_queue->i_size);
		p_queue->i_read = 0;
		p_queue->i_write = 0;
		p_queue->i_stop = 0;
	}
	return p_queue;
}

static message* createMessage(int code, int param1, void* userdata, int watting)
{
	message* msg = (message*)malloc(sizeof(message));
	if (msg) {
		msg->code = code;
		msg->param1 = param1;
		msg->user_data = userdata;

		msg->watting = watting;
		anc_sem_init(&msg->p_sem, 0);
	}
	return msg;
}

static void destroyMessage(message* p_msg)
{
	if (!p_msg)
		return;

	anc_sem_destroy(&p_msg->p_sem);
	free(p_msg);
}

static void pushMessage(msg_queue *p_queue, message* msg)
{
	if (!p_queue)
		return;

	anc_mutex_lock(&p_queue->p_lock);
    
	if (p_queue->p_msg)
	{
		uint32_t nextpos = (p_queue->i_write + 1) % p_queue->i_size;
		if (nextpos == p_queue->i_read)	//full
		{
			p_queue->i_read = (p_queue->i_read + 1) % p_queue->i_size;
		}
        
		if (p_queue->p_msg[p_queue->i_write])
			destroyMessage(p_queue->p_msg[p_queue->i_write]);
		p_queue->p_msg[p_queue->i_write] = 0;
		p_queue->p_msg[p_queue->i_write] = msg;

		p_queue->i_write = nextpos;
	}
    
	anc_mutex_unlock(&p_queue->p_lock);

	//if (p_queue->p_msg && msg)
	//	anc_sem_wait(&msg->p_sem);
}

void postMessage(msg_queue *p_queue, int code, int param1, void* userdata)
{
	if (!p_queue || p_queue->i_stop)
		return;

	message *p_msg = createMessage(code, param1, userdata, 0);
	if (p_msg) {
		pushMessage(p_queue, p_msg);
	}
}

void sendMessage(msg_queue *p_queue, int code, int param1, void* userdata)
{
	if (!p_queue || p_queue->i_stop)
		return;

	message *p_msg = createMessage(code, param1, userdata, 1);
	if (p_msg) {
		pushMessage(p_queue, p_msg);
		anc_sem_wait(&p_msg->p_sem);
	}
}

message* getMessage(msg_queue *p_queue)
{
	message* msg = 0;
	if (!p_queue)
		return 0;

	anc_mutex_lock(&p_queue->p_lock);
    
	if (p_queue->p_msg && p_queue->i_read != p_queue->i_write)
	{
		msg = p_queue->p_msg[p_queue->i_read];
		p_queue->i_read = (p_queue->i_read + 1) % p_queue->i_size;
	}
    
	anc_mutex_unlock(&p_queue->p_lock);
	return msg;
}

void signMessage(message* msg)
{
	if (msg)
		anc_sem_post(&msg->p_sem);
}

void destroyMsgQueue(msg_queue* p_queue)
{
	if (!p_queue)
		return;
	p_queue->i_stop = 1;

	while (p_queue->p_msg && p_queue->i_read != p_queue->i_write)
	{
		message* msg = p_queue->p_msg[p_queue->i_read];
		p_queue->i_read = (p_queue->i_read + 1) % p_queue->i_size;
		destroyMessage(msg);
	}
	
	anc_mutex_lock(&p_queue->p_lock);
	anc_mutex_unlock(&p_queue->p_lock);
	anc_mutex_destroy(&p_queue->p_lock);
	
	free(p_queue->p_msg);
	p_queue->p_msg = 0;
}
