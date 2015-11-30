#include "mtrs_comm.h"
#include "mpthread.h"
#include "mtrs.h"
#include "mtrs_internal.h"

#include <sys/types.h>
#include <sys/stat.h>

#if defined( _MSC_VER )
#define strncasecmp _strnicmp
#define snprintf _snprintf
#define strcasecmp	_stricmp
#endif

#ifndef WIN32
#include <signal.h>
#include <execinfo.h>
#endif
/*
 *
 */
static av_always_inline av_const int64_t my_clip64_c(int64_t a, int64_t amin, int64_t amax)
{
#if defined(HAVE_AV_CONFIG_H) && defined(ASSERT_LEVEL) && ASSERT_LEVEL >= 2
    if (amin > amax) abort();
#endif
    if      (a < amin) return amin;
    else if (a > amax) return amax;
    else               return a;
}

/*
 *
 */
static int64_t my_rescale_q_rnd(int64_t a, AVRational bq, AVRational cq,
                         enum AVRounding rnd)
{
    int64_t b= bq.num * (int64_t)cq.den;
    int64_t c= cq.num * (int64_t)bq.den;
    return av_rescale_rnd(a, b, c, rnd);
}

/*
 *
 */
int64_t my_rescale_delta(AVRational in_tb, int64_t in_ts,  AVRational fs_tb, int duration, int64_t *last, AVRational out_tb)
{
    int64_t a, b, thiz;
        
    if (*last == AV_NOPTS_VALUE || !duration || in_tb.num*(int64_t)out_tb.den <= out_tb.num*(int64_t)in_tb.den) {
    simple_round:
        *last = av_rescale_q(in_ts, in_tb, fs_tb) + duration;
        return av_rescale_q(in_ts, in_tb, out_tb);
    }
    
    a =  my_rescale_q_rnd(2*in_ts-1, in_tb, fs_tb, AV_ROUND_DOWN)   >>1;
    b = (my_rescale_q_rnd(2*in_ts+1, in_tb, fs_tb, AV_ROUND_UP  )+1)>>1;
    if (*last < 2*a - b || *last > 2*b - a)
        goto simple_round;
    
    thiz = my_clip64_c(*last, a, b);
    *last = thiz + duration;
    
    return av_rescale_q(thiz, fs_tb, out_tb);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////////////////
static int trs_newfile_keyframe(void* p, AVPacket *pkt)
{
	int ret = 0;
	if (!p || !pkt)
		return -1;

	trs_state	*p_trs = (trs_state	*)p;
	trs_input	*p_input = p_trs->p_input;
	trs_output	*p_output = p_trs->p_output;
	trs_output_list *p_list = p_trs->p_list;
	int err = 0;

	if (pkt->stream_index == 0 && (pkt->flags & AV_PKT_FLAG_KEY))		//关键帧是判断是否已经满足切片duration，满足则关闭当前文件，新建下一个文件
	{
		double f_progress = 0.0;
		if (p_trs->f_segmenter_duration > 0)
			f_progress = p_trs->seg_duration * av_q2d(p_input->p_video->time_base) / p_trs->f_segmenter_duration;
		else if (p_input->f_duration)
			f_progress = p_trs->seg_duration * av_q2d(p_input->p_video->time_base) / p_input->f_duration;
		f_progress *= 100;
		trs_fireout(p_trs, TRANS_PROGRESS, p_trs->i_output_trsing, (int32_t)f_progress);

#ifdef	PAD_0_TSFILE
		if (p_trs->seg_duration * av_q2d(p_input->p_video->time_base) >= p_trs->f_segmenter_duration)
#else
		if ((p_trs->seg_duration * av_q2d(p_input->p_video->time_base) >= p_trs->f_segmenter_duration) 
			&& (p_input->f_duration - p_trs->f_total_duration - p_trs->seg_duration * av_q2d(p_input->p_video->time_base)) > 5.0)
#endif
		{
			if (p_trs->i_output_trsing + 1 < p_trs->i_output_total)
			{
#if 0
				trs_close_output(p_output);
#else
				close_output_file(p_output);
#endif
				LOGI("===>>> close item index: %d\n", p_trs->i_output_trsing);

				p_list[p_trs->i_output_trsing].i_index = p_trs->i_output_trsing;
				p_list[p_trs->i_output_trsing].i_state = 2;
				p_list[p_trs->i_output_trsing].f_start_time = p_trs->seg_start * av_q2d(p_input->p_video->time_base);
				p_list[p_trs->i_output_trsing].f_duration = p_trs->seg_duration * av_q2d(p_input->p_video->time_base);
				if (p_trs->fp_segments) {
					char isegments[256] = "";
					snprintf(isegments, 256, "#index=%d,start=%f,duration=%f\n", 
						p_list[p_trs->i_output_trsing].i_index, 
						p_list[p_trs->i_output_trsing].f_start_time, 
						p_list[p_trs->i_output_trsing].f_duration);
					int wd = fwrite(isegments, 1, strlen(isegments), p_trs->fp_segments);
					if (wd == -1) {
						LOGE("----- write file error with errno=%d\n", ferror(p_trs->fp_iframe));
					}
					
					fflush(p_trs->fp_segments);
				}

				trs_fireout(p_trs, TRANS_ADDITEM, p_trs->i_output_trsing, 0);
				p_trs->f_total_duration += p_list[p_trs->i_output_trsing].f_duration;
				p_trs->f_total_duration_m3u8 += p_list[p_trs->i_output_trsing].f_duration_m3u8;

				p_trs->i_output_trsing ++;
#ifdef	PAD_0_TSFILE
				while (i_ismpegts>0&&(p_trs->i_output_trsing+1<p_trs->i_output_total) && (p_trs->f_total_duration > p_trs->f_total_duration_m3u8+p_list[p_trs->i_output_trsing+1].f_duration_m3u8)) {
					FILE* fd = fopen (p_list[p_trs->i_output_trsing].p_filename, "wb");
					fclose(fd);

					p_list[p_trs->i_output_trsing].i_index = p_trs->i_output_trsing;
					p_list[p_trs->i_output_trsing].i_state = 2;
					p_list[p_trs->i_output_trsing].f_start_time = p_trs->seg_start * av_q2d(p_input->p_video->time_base);
					p_list[p_trs->i_output_trsing].f_duration = 0;
					p_trs->f_total_duration_m3u8 += p_list[p_trs->i_output_trsing].f_duration_m3u8;

					trs_fireout(p_trs, TRANS_ADDITEM, p_trs->i_output_trsing, 0);
					LOGD("=====>>>skip item index: %d, %f, total:%f\n", p_trs->i_output_trsing, p_trs->seg_duration * av_q2d(p_input->p_video->time_base), p_trs->f_total_duration);
					p_trs->i_output_trsing ++;
				}
#endif

				LOGI("===>>> new item index: %d\n", p_trs->i_output_trsing);
#if 0
				err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, p_output->psz_format);
#else
				err = open_output_file(p_input, p_output, p_list, p_trs->i_output_trsing, p_output->psz_format);
#endif
				if (err < 0)
				{
					av_free_packet(pkt);
					ret = -1;
					//break;
				}

				p_trs->seg_start += p_trs->seg_duration;//av_rescale_q(pkt->pts, p_output->p_video->time_base, p_input->p_video->time_base);
				p_trs->seg_duration = 0;
				p_list[p_trs->i_output_trsing].i_state = 1;
				p_trs->audio_seg_start = AV_NOPTS_VALUE;
			}
		}
	}
	return ret;
}

/*
 * 写入avpacket,写入之前需要将packet做bitstream_filter
 */
int write_frame(trs_state *p_trs, AVFormatContext *oc, AVPacket *pkt, AVCodecContext *avctx, AVBitStreamFilterContext* bsfc)
{
    int ret;
    
    AVBitStreamFilterContext* p_temp_bsfc = bsfc;
    while (p_temp_bsfc) {
        AVPacket new_pkt = *pkt;
        int a = av_bitstream_filter_filter(p_temp_bsfc, avctx, NULL, &new_pkt.data, &new_pkt.size, pkt->data, pkt->size, pkt->flags&AV_PKT_FLAG_KEY);
        if (a == 0 && new_pkt.data != pkt->data && new_pkt.destruct) {
            uint8_t *t = (uint8_t *)av_malloc(new_pkt.size + FF_INPUT_BUFFER_PADDING_SIZE);
            if (t) {
                memcpy(t, new_pkt.data, new_pkt.size);
                memset(t+new_pkt.size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                new_pkt.data = t;
                a = 1;
            }
            else
                a = AVERROR(ENOMEM);
        }
        
        if (a > 0) {
            av_free_packet(pkt);
            new_pkt.destruct = av_destruct_packet;
        }
        else if (a < 0) {
            LOGE("failed to open bitstream filter!\n");
            ret = a;
        }
        
        *pkt = new_pkt;
        p_temp_bsfc = p_temp_bsfc->next;
    }
	
	if (p_trs && p_trs->p_input->i_start_write > 0) {
		int64_t timestamp = p_trs->p_input->i_start_write;
		if (p_trs->p_input->ic->start_time != AV_NOPTS_VALUE) {
			timestamp += p_trs->p_input->ic->start_time;
		}

		AVRational cq = {1, AV_TIME_BASE};
		int64_t vpts = av_rescale_q(pkt->pts, p_trs->p_output->p_video->time_base, cq);
		if (vpts != AV_NOPTS_VALUE && vpts < timestamp) 
			return 0;
	}

	if (p_trs && pkt->stream_index == 0) {
		trs_newfile_keyframe(p_trs, pkt);
	}

    ret = av_interleaved_write_frame(oc, pkt);
    if(ret < 0)
	{
        trs_print_error("av_interleaved_write_frame()", ret);
    }
	return ret;
}

/*
 * copy packet函数
 */
int do_copy_pkt(trs_state *p_trs, AVFormatContext *oc, AVStream* p_input, AVStream* p_output, AVPacket* avpkt, int64_t start_time, int64_t &next_pts, int64_t &rescale_delta_last, 
				int64_t &last_mux_dts,AVBitStreamFilterContext* bsfc, int pkt_index)
{
    if (!oc || !p_input || !p_output || !avpkt)
        return -1;
    
	AVFrame avframe; //FIXME/XXX remove this
	AVPacket opkt;
	int64_t ost_tb_start_time = av_rescale_q(start_time, p_input->time_base, p_output->time_base);

	uint8_t *data_buf = avpkt->data;
	int		data_size = avpkt->size;
	int		video_size = 0;
	int		audio_size = 0;
	int		index = 0;
	int		ret = 0;
    int64_t duration = 0;

	av_init_packet(&opkt);

	/* no reencoding needed : output the packet directly */
	/* force the input stream PTS */

//	avcodec_get_frame_defaults(&avframe);
//	p_output->codec->coded_frame = &avframe;
//	avframe.key_frame = avpkt->flags & AV_PKT_FLAG_KEY;
	//指定packet的stream_index
	if(p_output->codec->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		audio_size += data_size;
		index = pkt_index;
	}
	else if (p_output->codec->codec_type == AVMEDIA_TYPE_VIDEO) 
	{
		video_size += data_size;
		index = pkt_index;
	}

	//将输入时间戳转换成输出时间戳
	opkt.stream_index = index;
	if(avpkt->pts != AV_NOPTS_VALUE)
		opkt.pts = av_rescale_q(avpkt->pts, p_input->time_base, p_output->time_base) - ost_tb_start_time;//av_rescale_q(pkt->pts, ist->st->time_base, ost->st->time_base) - ost_tb_start_time;
	else
		opkt.pts = AV_NOPTS_VALUE;

	if (avpkt->dts == AV_NOPTS_VALUE)
#if defined( _MSC_VER )
	{
		AVRational base_q;
		base_q.num = 1;
		base_q.den = AV_TIME_BASE;
		opkt.dts = av_rescale_q(next_pts, base_q, p_output->time_base);
	}
#else
    {
		opkt.dts = av_rescale_q(next_pts, AV_TIME_BASE_Q, p_output->time_base);
    }
#endif
	else
		opkt.dts = av_rescale_q(avpkt->dts, p_input->time_base, p_output->time_base);
    opkt.dts -= ost_tb_start_time;

	if (p_trs && p_trs->fp_vpts && p_output->codec->codec_type == AVMEDIA_TYPE_VIDEO)  {
		char buf[1024] = "";
		snprintf(buf, 1024, "dPTS: %lld/%lld ePTS: %lld/%lld", avpkt->dts, avpkt->pts, opkt.dts, opkt.pts);
		if (p_trs->ii_video_pre_pts != AV_NOPTS_VALUE && p_trs->ii_video_pre_pts == avpkt->pts) {
			strcat(buf, " ******");
		}
		strcat(buf, "\n");
		int wd = fwrite(buf, 1, strlen(buf), p_trs->fp_vpts);
		if (wd == -1) {
			LOGE("----- write file error with errno=%d\n", ferror(p_trs->fp_iframe));
		}
		fflush(p_trs->fp_vpts);

		p_trs->ii_video_pre_pts = avpkt->pts;
	}

	//针对音频计算时间戳，根据输入音频的sample index校正
    if (p_output->codec->codec_type == AVMEDIA_TYPE_AUDIO && avpkt->dts != AV_NOPTS_VALUE)
    {
//        duration = ((int64_t)AV_TIME_BASE *p_input->codec->frame_size) / p_input->codec->sample_rate;
#if LIBAVFORMAT_VERSION_MAJOR >= 54
        duration = av_get_audio_frame_duration(p_input->codec, avpkt->size);
#else
		int bps = av_get_bits_per_sample(p_input->codec->codec_id);
        duration = (avpkt->size * 8LL) / (bps * p_input->codec->channels);
#endif
//        if (!duration && avpkt->duration > 0)
//            duration = av_rescale_q(avpkt->duration, p_input->time_base, (AVRational){1, p_input->codec->sample_rate});
        if (!duration)
            duration = p_input->codec->frame_size;
#if defined( _MSC_VER )
        AVRational sample_rate_q;
        sample_rate_q.num = 1;
        sample_rate_q.den = p_input->codec->sample_rate;
        opkt.dts = opkt.pts = my_rescale_delta(p_input->time_base, avpkt->dts, sample_rate_q, duration, &rescale_delta_last, p_output->time_base) - ost_tb_start_time;
#else
        opkt.dts = opkt.pts = my_rescale_delta(p_input->time_base, avpkt->dts, (AVRational){1, p_input->codec->sample_rate}, duration, &rescale_delta_last, p_output->time_base) - ost_tb_start_time;
#endif
    }
    
#if defined( _MSC_VER )
    AVRational base_q;
    base_q.num = 1;
    base_q.den = AV_TIME_BASE;
    duration = av_rescale_q(avpkt->duration, p_input->time_base, base_q);
#else
    duration = av_rescale_q(avpkt->duration, p_input->time_base, AV_TIME_BASE_Q);
#endif
    next_pts += duration;
    

    opkt.duration = av_rescale_q(avpkt->duration, p_input->time_base, p_output->time_base);
	opkt.flags = avpkt->flags;

	//FIXME remove the following 2 lines they shall be replaced by the bitstream filters
	if(   p_output->codec->codec_id != CODEC_ID_H264
		&& p_output->codec->codec_id != CODEC_ID_MPEG1VIDEO
		&& p_output->codec->codec_id != CODEC_ID_MPEG2VIDEO
		) {
			if(av_parser_change(p_input->parser, p_output->codec, &opkt.data, &opkt.size, data_buf, data_size, avpkt->flags & AV_PKT_FLAG_KEY))
				opkt.destruct= av_destruct_packet;
	} else {
		opkt.data = data_buf;
		opkt.size = data_size;
	}

    if (last_mux_dts != AV_NOPTS_VALUE
        && p_output->codec->codec_type == AVMEDIA_TYPE_VIDEO
        && !(oc->oformat->flags & AVFMT_NOTIMESTAMPS))
    {
        int64_t max = last_mux_dts + !(oc->oformat->flags & AVFMT_TS_NONSTRICT);
        if (opkt.dts < max) {
            if (opkt.pts >= opkt.dts)
                opkt.pts = FFMAX(opkt.pts, max);
            opkt.dts = max;
        }
    }
    last_mux_dts = opkt.dts;
    
	ret = write_frame(p_trs, oc, &opkt, p_output->codec, bsfc);
	p_output->codec->frame_number++;
	//ost->frame_number++;
	av_free_packet(&opkt);
	return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////////////////////////
/*
 * 事件发送函数
 */
void trs_fireout(trs_state* p_trs, int32_t code, int32_t data1, int32_t data2)
{
	if (p_trs)
	{
        if (p_trs->p_context)
            trs_event(p_trs, code, data1, data2, p_trs->p_context);
        //else if (p_trs->event_cb)
            //(p_trs->event_cb->pfn_cb)(p_trs, code, data1, data2, p_trs->event_cb->p_context);
    }
}

/*
 * 错误打印函数
 */
void trs_print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
	LOGI("%s: %s\n", filename, errbuf_ptr);
}

/*
 *
 *
 */
int FileExist(const char* psz_name)
{
	int ret = -1;
#ifdef WIN32
	struct _stat64 st;
	ret = _stat64(psz_name, &st);
#else
	struct stat st;
	ret = stat(psz_name, &st);
#endif
	if (ret < 0) 
		ret = 0;
	else
		ret = 1;
	return ret;
}

#define MAX_PRE_READ		2048
typedef struct readline_state {
	FILE* fp;
	char buf_catch[2048];
	int i_lasted;
	int ii_pos;
};

static int readline(readline_state* p_sys, char* buf, int i_max) 
{
	FILE* fp = p_sys->fp;

	int len = 0;
	int src_len = 0;
	if (fp && buf && i_max > 0)
	{
		for(;;)
		{
			if (p_sys->i_lasted <= 0)
			{
				p_sys->i_lasted = fread(p_sys->buf_catch, 1, MAX_PRE_READ, fp);
				p_sys->ii_pos = 0;
				src_len = 0;
			}
			if (p_sys->i_lasted <= 0)		//file eof
				break;

			bool b_newline = false;
			do{
				if (len < i_max)
					buf[len] = p_sys->buf_catch[p_sys->ii_pos + src_len];
				b_newline = (p_sys->buf_catch[p_sys->ii_pos + src_len] == '\n');
				p_sys->i_lasted --;
				len ++;
				src_len ++;
			}while(p_sys->i_lasted > 0 && !b_newline);
			p_sys->ii_pos += src_len;

			if (b_newline)
				break;
		}

		while (len > 0 && (buf[len - 1] == ' ' || buf[len-1] == '\t' || buf[len-1] == '\n' || buf[len-1] == '\r'))
			buf[--len] = '\0';
	}
	return len;
}

/*
 * crash handler
 *
 */
#ifdef WIN32
void setup_crash_handler(void *p_context) 
{
	if (!p_context)
		return;
}

void cleanup_crash_handler()
{
}
#else
pthread_key_t tls_crash_key = 0;
typedef struct crash_context {
	struct sigaction p_abrt;
	struct sigaction p_segv;
	struct sigaction p_bus;
	struct sigaction p_term;

	void	*p_context;
};

static void crash_log_write(FILE* fp, const char* line)
{
	if (fp && line) {
		fprintf(fp,"%s ",line);
		fflush(fp);
	}
}

static void dump_tracestack(FILE* fp)
{
	int j, nptrs;
#define SIZE 128
	void *buffer[128];
	char **strings;
	char buf_log[2048] = {0};

	nptrs = backtrace(buffer, SIZE);
	snprintf(buf_log, 2048, "backtrace() returned %d addresses\n", nptrs);
	crash_log_write(fp, buf_log);

	/* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
	would produce similar output to the following: */

	strings = backtrace_symbols(buffer, nptrs);
	if (strings == NULL) {
		snprintf(buf_log, 2048, "backtrace_symbols error.\n");
		crash_log_write(fp, buf_log);
		return;
	}

	for (j = 0; j < nptrs; j++)
	{
		snprintf(buf_log, 2048, "%s.\n", strings[j]);
		crash_log_write(fp, buf_log);
	}

	free(strings);
}

static void crash_handler(int signal)
{
	if (signal == SIGPIPE) {
		return;
	}

#if 0
    FILE *fp = fopen("/root/mtrs_log.txt","a");
    if (fp)
    {
        dump_tracestack(fp);
        fclose(fp);
    }
    
    sync();
    _exit(0);	

#else
	void *p = pthread_getspecific(tls_crash_key);
	if (p) {
		crash_context *pp = (crash_context*)p;

		trs_state *p_trs = (trs_state *)pp->p_context;
		if (p_trs) {
			size_t dest_len = strlen(p_trs->p_dir);
			
			char psz_log1[2048];
			memset(psz_log1, 0, sizeof(psz_log1));
			strcpy(psz_log1, p_trs->p_dir);
			strcat(psz_log1, "1_crash_report");

			char psz_log2[2048];
			memset(psz_log2, 0, sizeof(psz_log2));
			strcpy(psz_log2, p_trs->p_dir);
			strcat(psz_log2, "2_crash_report");

			FILE *fp = 0;
			if (FileExist(psz_log2) || FileExist(psz_log1)) {
				fp = fopen(psz_log2, "wb");
			} else {
				fp = fopen(psz_log1, "wb");
			}

			if (fp) {
				//crash dump
				dump_tracestack(fp);
				fclose(fp);
			}
		}

		//call old crash handler
		if (SIGABRT == signal) {
			//LOGI("--- sigabrt\n");
//			pp->p_abrt.sa_handler(signal);
//			exit(1);
		} else if (SIGSEGV == signal) {
			//LOGI("--- sigsegv\n");
//			pp->p_segv.sa_handler(signal);
//			exit(1);
		} else if (SIGBUS == signal) {
			//LOGI("--- sigbus\n");
//			pp->p_bus.sa_handler(signal);
//			exit(1);
		} else if (SIGTERM == signal) {
			//LOGI("--- sigterm\n");
//			pp->p_term.sa_handler(signal);
//			exit(1);
		}

		sync();
		_exit(0);
	}
#endif
}

static void crash_context_destroy(void *p)
{
	if (p) {
		free(p);
	}
}

static void setup_crash_handler(void *p_context) 
{
	if (!p_context)
		return;

	crash_context *p = (crash_context*)malloc(sizeof(crash_context));
	if (!p)
		return;
	p->p_context = p_context;

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = crash_handler;
	
	sigaction(SIGABRT, &act, &p->p_abrt);
	sigaction(SIGSEGV, &act, &p->p_segv);
	sigaction(SIGBUS, &act, &p->p_bus);
	sigaction(SIGTERM, &act, &p->p_term);
	sigaction(SIGFPE, &act, 0);
	sigaction(SIGILL, &act, 0);
	sigaction(SIGSYS, &act, 0);
	pthread_key_create(&tls_crash_key, crash_context_destroy);
	pthread_setspecific(tls_crash_key, p);
}

static void cleanup_crash_handler()
{
	pthread_key_delete(tls_crash_key);
}
#endif
/*
 *
 *
 */
int trs_initcr(trs_state *p_trs) 
{
	if (!p_trs)
		return -1;

	size_t dest_len = strlen(p_trs->p_dir);
	p_trs->fp_log = 0;
	p_trs->i_recover_index = 0;
	p_trs->f_recover_pos = 0.0;
	p_trs->f_recover_pos_skip = 0.0;
	p_trs->i_crash_times = 0;

	char *psz_log1 = (char*)malloc(dest_len + 128);
	if (psz_log1) {
		strcpy(psz_log1, p_trs->p_dir);
		strcat(psz_log1, "1_crash_report");
	}
	char *psz_log2 = (char*)malloc(dest_len + 128);
	if (psz_log2) {
		strcpy(psz_log2, p_trs->p_dir);
		strcat(psz_log2, "2_crash_report");
	}

	//if crash more than 2, overwrite the segment from next key frame
	if (FileExist(psz_log2)) {
		p_trs->i_crash_times = 2;
	}
	//if crash first time, overwrite the segment
	else if (FileExist(psz_log1)) {
		p_trs->i_crash_times = 1;
	}

	//i-frame recode
	char *psz_iframe = (char*)malloc(dest_len+128);
	if (psz_iframe) {
		strcpy(psz_iframe, p_trs->p_dir);
		strcat(psz_iframe, "v_iframe.txt");
	}
	char *psz_segments = (char*)malloc(dest_len+128);
	if (psz_segments) {
		strcpy(psz_segments, p_trs->p_dir);
		strcat(psz_segments, "segments.txt");
	}
	char *psz_vpts = (char*)malloc(dest_len+128);
	if (psz_vpts) {
		strcpy(psz_vpts, p_trs->p_dir);
		strcat(psz_vpts, "vpts.txt");
	}

	if (p_trs->i_crash_times > 0) {
		int64_t ii_vk_tm = AV_NOPTS_VALUE;
		float f_last_vkpos = 0.0;
		int last_index = 0;
		float f_last_start = 0.0;
		float f_last_duration = 0.0;

		if (psz_iframe) {
			p_trs->fp_iframe = fopen(psz_iframe, "rb");
			if (p_trs->fp_iframe) {
				fseek(p_trs->fp_iframe, 0, SEEK_END);
				long len = ftell(p_trs->fp_iframe);
				fseek(p_trs->fp_iframe, 0, SEEK_SET);

				char* last_line = 0;
				char* buf = (char*)malloc(len+1);
				if (buf) {
					memset(buf, 0, len+1);
					int rd = fread(buf, 1, len, p_trs->fp_iframe);
					if (rd > 0) {
						char *p = (char*)strrchr(buf, '#');
						if (p && strlen(p) > 1) {
							last_line = strdup(p+1);
						}
					}
					free(buf);
				}
				fclose(p_trs->fp_iframe);
				p_trs->fp_iframe = 0;

				if (last_line) {
					if (!strcmp(last_line, "end")) {
					} else {
						sscanf(last_line, "%lld,%f\n", &ii_vk_tm, &f_last_vkpos);
					}

					free(last_line);
				}
			}

			p_trs->fp_iframe = fopen(psz_iframe, "ab");
		}

		//ts-segment recode
		if (psz_segments) {
			p_trs->fp_segments = fopen(psz_segments, "rb");
			if (p_trs->fp_segments) {
				fseek(p_trs->fp_segments, 0, SEEK_END);
				long len = ftell(p_trs->fp_segments);
				fseek(p_trs->fp_segments, 0, SEEK_SET);

				char* last_line = 0;
				char* buf = (char*)malloc(len+1);
				if (buf) {
					memset(buf, 0, len+1);
					int rd = fread(buf, 1, len, p_trs->fp_segments);
					if (rd > 0) {
						char *p = (char*)strrchr(buf, '#');
						if (p && strlen(p) > 1) {
							last_line = strdup(p+1);
						}
					}
					free(buf);
				}

				if (last_line) {
					if (!strcmp(last_line, "end")) {
					} else {
						sscanf(last_line, "index=%d,start=%f,duration=%f\n", &last_index, &f_last_start, &f_last_duration);

						last_index += 1;
						f_last_start += f_last_duration;
					}
					free(last_line);
				}

				fclose(p_trs->fp_segments);
				p_trs->fp_segments = 0;
			}
			p_trs->fp_segments = fopen(psz_segments, "ab");
		}

		//第一次崩溃，采用最后一次崩溃的片段开始时间，但是从当前时间向前30秒开始转码
		//多次崩溃，采用最后一个关键帧最为开始转码的时间节点
		if (p_trs->i_crash_times == 2) {
			p_trs->i_recover_index = last_index;
			p_trs->f_recover_pos = f_last_vkpos;
			p_trs->f_recover_pos_skip = f_last_vkpos;	//30.0 sec
		} else if (p_trs->i_crash_times == 1) {
			p_trs->i_recover_index = last_index;
			p_trs->f_recover_pos = f_last_start;
			p_trs->f_recover_pos_skip = f_last_start - 30.0;	//30.0 sec
		}
	} else {
		if (psz_iframe) {
			p_trs->fp_iframe = fopen(psz_iframe, "wb");
		}
		if (psz_segments) {
			p_trs->fp_segments = fopen(psz_segments, "wb");
		}
	}

	if (psz_vpts) {
		p_trs->fp_vpts = fopen(psz_vpts, "wb");
	}

	if (psz_log1)
		free(psz_log1);
	psz_log1 = 0;

	if (psz_log2)
		free(psz_log2);
	psz_log2 = 0;

	if (psz_iframe)
		free(psz_iframe);
	psz_iframe = 0;

	if (psz_segments)
		free(psz_segments);
	psz_segments = 0;

	if (psz_vpts)
		free(psz_vpts);
	psz_vpts = 0;

	//setup crash handler
	setup_crash_handler(p_trs);
	
	return 0;
}

int trs_closecr(trs_state *p_trs)
{
	if (!p_trs)
		return -1;

	if (p_trs->fp_iframe) {
		fwrite("#end", 1, 4, p_trs->fp_iframe);
		fclose(p_trs->fp_iframe);
	}
	p_trs->fp_iframe = 0;

	if (p_trs->fp_segments) {
		fwrite("#end", 1, 4, p_trs->fp_segments);
		fclose(p_trs->fp_segments);
	}
	p_trs->fp_segments = 0;

	if (p_trs->fp_log)
		fclose(p_trs->fp_log);
	p_trs->fp_log = 0;

	if (p_trs->fp_vpts)
		fclose(p_trs->fp_vpts);
	p_trs->fp_vpts = 0;

	//remove crash handler
	cleanup_crash_handler();

	return 0;
}

int trs_recover(trs_state *p_trs) 
{
	int ret = 0;
	if (!p_trs || !p_trs->p_input)
		return -1;

	if (p_trs->i_crash_times <= 0)
		return 0;

	p_trs->i_output_start = p_trs->i_recover_index;
	p_trs->p_input->i_start_time = p_trs->f_recover_pos_skip * AV_TIME_BASE;
	p_trs->p_input->i_start_write = p_trs->f_recover_pos * AV_TIME_BASE;
	if (p_trs->p_input->i_start_time == p_trs->p_input->i_start_write)
		p_trs->p_input->i_start_write = 0;

	size_t dest_len = strlen(p_trs->p_dir);
	char *psz_segments = (char*)malloc(dest_len+128);
	if (psz_segments) {
		strcpy(psz_segments, p_trs->p_dir);
		strcat(psz_segments, "segments.txt");
	}

	if (p_trs->fp_segments && psz_segments) {
		fclose(p_trs->fp_segments);
		p_trs->fp_segments = fopen(psz_segments, "rb");
		if (p_trs->fp_segments) {
			readline_state* p_readline = (readline_state*)malloc(sizeof(readline_state));
			p_readline->fp = p_trs->fp_segments;
			p_readline->i_lasted = 0;
			p_readline->ii_pos = 0;
			char line[2048] = "";
			while (1) {
				int i_readed = readline(p_readline, line, 2048);
				if (i_readed > 0) {
					int index = 0;
					float start = 0.0;
					float duration = 0.0;

					if (!strcmp(line, "#end")) {
						break;
					} else {
						sscanf(line, "#index=%d,start=%f,duration=%f", &index, &start, &duration);
					}
					if (index >= 0 && start >= 0 && duration > 0) {
						p_trs->p_list[index].f_duration = duration;
						p_trs->p_list[index].f_start_time = start;
						p_trs->p_list[index].i_index = index;
						p_trs->p_list[index].i_state = 2;
						p_trs->p_list[index].f_duration_m3u8 = duration;
						p_trs->p_list[index].p_next = 0;
					}
				}
				if (i_readed <= 0 && feof(p_trs->fp_segments))
					break;
			}
			free(p_readline);

			fclose(p_trs->fp_segments);
		}
		p_trs->fp_segments = fopen(psz_segments, "ab");
	}

	if (psz_segments)
		free(psz_segments);
	psz_segments = 0;
	return ret;
}
