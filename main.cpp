#include "mtrs_comm.h"
#include "mpthread.h"
#include "msgqueue.h"
#include "mtrs.h"

#if defined( _MSC_VER )
#define strncasecmp _strnicmp
#define snprintf _snprintf
#define strcasecmp	_stricmp
#endif

typedef struct mtrs_instance {
	char *filename;
	char *output_dir;

	float f_offset;
	float f_duration;
	float f_size;
	char *base_url;
	float f_segment;

	void *p_trs;
	msg_queue *p_msgqueue;

	int i_end;
	int i_stop;
};


void usage()
{
	printf(	"-input input file name\n"
			"-offset start point(ms)\n"
			"-duration duration to convert(ms)\n"
			"-size size to convert(M, using duration first)\n"
			"-host m3u8 base_url\n"
			"-segment ts media file duration(ms)\n"
			"-output m3u8 and ts file output dir\n");
}

//callback...
void trs_event(void* handle, int32_t code, int32_t data1, int32_t data2, void* context)
{
	if (!context)
		return;

	mtrs_instance* p_sys = (mtrs_instance*)context;
	switch (code) {
		case TRANS_PLAYLISTREADY: 
			{
				LOGD("===== m3u8 file ready.\n");
			}
			break;

		case TRANS_ADDITEM: 
			{
				int i_index = data1;
			}
			break;

		case TRANS_PROGRESS: 
			{
				int i_index = data1;
				int i_percent = data2;
			}
			break;

		case TRANS_ABORTITEM:
			{
				int i_index = data1;
			}
			break;

		case TRANS_ERROR:	//error
			{
			}
			break;

		case TRANS_END:
			{
				int i_index = data1;
			}
			break;

		default:
			break;
	}

	postMessage(p_sys->p_msgqueue, code, data1, (void*)data2);
}

/////////////////////////////////////////////////////////////////////////////////////////
//filename start duration/size base_url
//-i filename -ss s -t ms(-size **M) -segment_host base_url -segment_time s -o path
/////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char* argv[])
{
	if (argc < 2 * 2 + 1) {
		usage();
	}

	do {
		mtrs_instance *p_sys = (mtrs_instance *)malloc(sizeof(mtrs_instance));
		if (!p_sys)
			break;

		memset(p_sys, 0, sizeof(mtrs_instance));
		p_sys->i_end = 0;
		p_sys->i_stop = 0;
		p_sys->f_duration = -1;
		p_sys->f_size = -1;

		for (int i = 1; (i < argc) && ((i+1) < argc); i +=2) {
			char* psz_cmd = (char*)argv[i];
			char* psz_value = (char*)argv[i+1];

			if (!strcmp(psz_cmd, "-i")) {
				p_sys->filename= strdup(psz_value);
			}
			else if (!strcmp(psz_cmd, "-ss")) {
				p_sys->f_offset = atof(psz_value);
				p_sys->f_offset *= 1000.0;
			}
			else if (!strcmp(psz_cmd, "-t")) {				//seconds
				p_sys->f_duration = atof(psz_value);
			}
			//else if (!strcmp(psz_cmd, "-fs")) {			//file size by (MB)
			//	p_sys->f_size = atof(psz_value);
			//}
			else if (!strcmp(psz_cmd, "-segment_host")) {
				int len = strlen(psz_value);
				if (psz_value[len-1] == '/')
					p_sys->base_url = strdup(psz_value);
				else {
					p_sys->base_url = (char*)malloc(len+8);
					memset(p_sys->base_url, 0, len+8);
					snprintf(p_sys->base_url, len+8, "%s/", psz_value);
				}
			}
			else if (!strcmp(psz_cmd, "-segment_time")) {
				p_sys->f_segment = atof(psz_value);
			}
			else if (!strcmp(psz_cmd, "-o")) {
				int len = strlen(psz_value);
#ifdef WIN32
				if (psz_value[len-1] == '\\')
					p_sys->output_dir = strdup(psz_value);
				else {
					p_sys->output_dir = (char*)malloc(len+32);
					memset(p_sys->output_dir, 0, len+32);
					snprintf(p_sys->output_dir, len+32, "%s\\", psz_value);
				}
#else
				if (psz_value[len-1] == '/')
					p_sys->output_dir = strdup(psz_value);
				else {
					p_sys->output_dir = (char*)malloc(len+32);
					memset(p_sys->output_dir, 0, len+32);
					snprintf(p_sys->output_dir, len+32, "%s/", psz_value);
				}
#endif
			}
		}

		if (!p_sys->filename || !p_sys->output_dir) {
			usage();
			break;
		}

		if (p_sys->f_offset <= 0)
			p_sys->f_offset = 0;
		if (p_sys->f_duration <= 0)
			p_sys->f_duration = -1;	//all file.
		if (p_sys->f_segment <= 0)
			p_sys->f_segment = 15.0;
		if (p_sys->f_size <= 0)
			p_sys->f_size = -1;		//end of file

		//if (!p_sys->base_url || !*p_sys->base_url)
		//	p_sys->base_url = strdup("http://127.0.0.1/mtrs/");

//		trs.... start ...
		AVDictionary* my_opts = 0;
		av_dict_set(&my_opts, "output_format", "mpegts", 0);
		av_dict_set(&my_opts, "output_ext", "ts", 0);
		av_dict_set(&my_opts, "playlist_type", "m3u8", 0);
		if (!p_sys->base_url || !*p_sys->base_url)
			av_dict_set(&my_opts, "local_host", p_sys->base_url, 0);

		if (p_sys->f_duration > 0) {
			char psz_opt[32] = "";
			snprintf(psz_opt, 32, "%.2f", p_sys->f_duration);
			av_dict_set(&my_opts, "limit_duration", psz_opt, 0);
		}
		//else if (p_sys->f_size > 0) {
		//	char psz_opt[32] = "";
		//	snprintf(psz_opt, 32, "%.2f", p_sys->f_size);
		//	av_dict_set(&my_opts, "limit_size", psz_opt, 0);
		//}

		p_sys->p_msgqueue = initMsgQueue();
		if (!p_sys->p_msgqueue)
			break;

		p_sys->p_trs = trs_open(p_sys->filename, p_sys->output_dir, p_sys->f_offset, p_sys->f_segment, 0, p_sys, 0, my_opts);
		if (!p_sys->p_trs) {
			if (p_sys->p_msgqueue)
				destroyMsgQueue(p_sys->p_msgqueue);
			break;
		}


		while (1) {
			if (p_sys->i_end || p_sys->i_stop)
				break;

			message* msg = getMessage(p_sys->p_msgqueue);
			if (msg) {
				switch(msg->code) {
					case TRANS_ADDITEM:
						{
							int i_index = msg->param1;
							LOGD("HLS segment %d trans ok\n", i_index);
						}
						break;

					case TRANS_ERROR:
						{
							int i_err = msg->param1;
							p_sys->i_end = 1;
							LOGD("HLS trans error %d\n", i_err);
						}
						break;

					case TRANS_END:
						{
							int i_index = msg->param1;
							p_sys->i_end = 1;
							LOGD("HLS segment %d trans over, the media trans over\n", i_index);
						}
						break;

					case TRANS_PROGRESS:
						{
							int i_index = msg->param1;
							long i_progress = (long)(msg->user_data);
							if (i_progress > 100)
								i_progress = 100;

							LOGD("HLS segment %d item progress %d%%\n", i_index, i_progress);
						}
						break;

					case TRANS_PLAYLISTREADY:
						{
							LOGD("HLS m3u8 file ready\n");
						}
						break;
						
					case TRANS_ABORTITEM:
						{
							int i_index = msg->param1;
							p_sys->i_end = 1;
							LOGD("HLS segment %d item abort\n");
						}
						break;

					default:
						break;
				}

				if (msg->watting) {
					signMessage(msg);
				}
			}
			else {
#ifdef WIN32
				Sleep(10);
#else
				msleep(CLOCK_FREQ / 100);
#endif
			}
		}

		if (p_sys->p_msgqueue) {
			while (1) {
				message* msg = getMessage(p_sys->p_msgqueue);
				if (!msg)
					break;

				if (msg->watting) {
					signMessage(msg);
				}
			}
			destroyMsgQueue(p_sys->p_msgqueue);
		}
		p_sys->p_msgqueue = 0;

		trs_stop(p_sys->p_trs);
		trs_close(p_sys->p_trs);
		p_sys->p_trs = 0;

		av_dict_free(&my_opts);
		my_opts = 0;
	} while(0);

	//free memory ...

	return 0;
}
