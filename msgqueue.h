#ifndef __MSGQUEUE_H__
#define __MSGQUEUE_H__ 1
#include "mtrs_comm.h"
#include "mpthread.h"

typedef struct message {
	int code;
	int param1;
	void* user_data;

	int			watting;
	anc_sem_t	p_sem;
};

typedef struct msg_queue {
	anc_mutex_t	p_lock;
	message		**p_msg;
	int			i_size;
	int			i_read;
	int			i_write;
	int			i_stop;
};

msg_queue *initMsgQueue();
void postMessage(msg_queue *p_queue, int code, int param1, void* userdata);
void sendMessage(msg_queue *p_queue, int code, int param1, void* userdata);

message* getMessage(msg_queue *p_queue);
void signMessage(message* msg);
void destroyMsgQueue(msg_queue* p_queue);
#endif