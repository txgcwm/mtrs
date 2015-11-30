#include "mtrs_comm.h"
#include "mpthread.h"
#include "mtrs.h"
#include "mtrs_internal.h"

#if defined( _MSC_VER )
#define strncasecmp _strnicmp
#define snprintf _snprintf
#define strcasecmp	_stricmp
#endif

//static int check_output_constraints(InputStream *ist, OutputStream *ost)
//{
//    OutputFile *of = &output_files[ost->file_index];
//    int ist_index  = ist - input_streams;
//
//    if (ost->source_index != ist_index)
//        return 0;
//
//    if (of->start_time && ist->pts < of->start_time)
//        return 0;
//
//    if (of->recording_time != INT64_MAX &&
//        av_compare_ts(ist->pts, AV_TIME_BASE_Q, of->recording_time + of->start_time,
//                      (AVRational){ 1, 1000000 }) >= 0) {
//        ost->is_past_recording_time = 1;
//        return 0;
//    }
//
//    return 1;
//}

#ifdef WIN32
static double rint(double x)
{
    return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5);
}

static long int lrintf(float x)
{
    return (int)(rint(x));
}
#endif

#ifdef	USING_AVFILTER
static int config_video_filter (trs_output *p_output, trs_input *p_input)
{
	int ret = 0;
	char args[512];
	char filters_descr[512] = {0};

	AVCodecContext *enc_ctx = p_output->p_video->codec;
	AVCodecContext *dec_ctx = p_input->p_video->codec;
	AVRational time_base = p_input->p_video->time_base;

	trs_filter* p_filter = p_output->p_video_filter;
	if (!p_filter) 
		p_filter = (trs_filter*)av_mallocz(sizeof(trs_filter));
	avfilter_register_all();

	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVBufferSinkParams *buffersink_params;
	AVRational sar;
	sar.num = 0;
	sar.den = 1;

	AVRational tb;
	tb.den = AV_TIME_BASE;
	tb.num = 1;
	//tb = p_input->p_video->time_base;

	enum PixelFormat pix_fmts[] = {enc_ctx->pix_fmt, PIX_FMT_NONE};

    p_filter->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !p_filter->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

	if (p_input->p_video->sample_aspect_ratio.num)
		sar = p_input->p_video->sample_aspect_ratio;
	else
		sar = p_input->p_video->codec->sample_aspect_ratio;

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		tb.num, tb.den, sar.num, sar.den,
		SWS_BILINEAR + ((dec_ctx->flags&CODEC_FLAG_BITEXACT) ? SWS_BITEXACT:0));

	ret = avfilter_graph_create_filter(&p_filter->buffersrc_ctx, buffersrc, "in", args, NULL, p_filter->filter_graph);
	if (ret < 0) {
		LOGD("Cannot create buffer source\n");
		return ret;
	}

	buffersink_params = av_buffersink_params_alloc();
	buffersink_params->pixel_fmts = pix_fmts;
	ret = avfilter_graph_create_filter(&p_filter->buffersink_ctx, buffersink, "out", NULL, buffersink_params, p_filter->filter_graph);
	av_free(buffersink_params);
	//ret = avfilter_graph_create_filter(&p_filter->buffersink_ctx, buffersink, "out", NULL, NULL, p_filter->filter_graph);
    if (ret < 0) {
        LOGD("Cannot create buffer sink\n");
        return ret;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = p_filter->buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = p_filter->buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

	if (enc_ctx->pix_fmt != dec_ctx->pix_fmt) {
		//format=pix_fmts=yuv420p
		//av_get_pix_fmt_name
		const char* pix_fmt_name = av_get_pix_fmt_name(enc_ctx->pix_fmt);
		if (pix_fmt_name) {
			snprintf(filters_descr, sizeof(filters_descr), "format=pix_fmts=%s", pix_fmt_name);
		}
		else {
			snprintf(filters_descr, sizeof(filters_descr), "format=pix_fmts=yuv420p");
		}
	}


	if (strlen(filters_descr) > 0) {
		if ((ret = avfilter_graph_parse_ptr(p_filter->filter_graph, filters_descr, &inputs, &outputs, NULL)) < 0)
			goto end;

		if ((ret = avfilter_graph_config(p_filter->filter_graph, NULL)) < 0)
			goto end;
	}
	else {
		avfilter_link(p_filter->buffersrc_ctx, 0, p_filter->buffersink_ctx, 0);

		if ((ret = avfilter_graph_config(p_filter->filter_graph, NULL)) < 0)
			goto end;
	}

	p_output->p_video_filter = p_filter;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
	return 0;
}
#endif

/*
 * 编码函数
 */
static void do_video_out(trs_state *p_trs, trs_output *p_output, trs_input *p_input, AVFrame *in_picture, int *frame_size, float quality)
{
	AVFormatContext *s = p_output->oc;

    int nb_frames, i, ret, format_video_sync;
    AVCodecContext *enc;
    double sync_ipts;
    double duration = 0;
    enc = p_output->p_video->codec;

	//计算输入packet的帧数
    if (p_input->p_video->start_time != AV_NOPTS_VALUE && p_input->p_video->first_dts != AV_NOPTS_VALUE) {
        duration = FFMAX(av_q2d(p_input->p_video->time_base), av_q2d(p_input->p_video->codec->time_base));
        if(p_input->p_video->avg_frame_rate.num)
            duration= FFMAX(duration, 1/av_q2d(p_input->p_video->avg_frame_rate));

        duration /= av_q2d(enc->time_base);
    }

	//计算当前的要写到多少帧
	//sync_ipts = p_output->ii_video_nextpts;
	//sync_ipts /= AV_TIME_BASE;
	//sync_ipts /= av_q2d(enc->time_base);

	AVRational bq = {1, AV_TIME_BASE};
	sync_ipts = av_rescale_q(p_output->ii_video_nextpts, bq, enc->time_base);

	if (p_output->ii_video_sync_opts == 0)
		p_output->ii_video_sync_opts = sync_ipts;

    /* by default, we output a single frame */
    nb_frames = 1;

    *frame_size = 0;

    format_video_sync = (s->oformat->flags & AVFMT_VARIABLE_FPS) ? ((s->oformat->flags & AVFMT_NOTIMESTAMPS) ? VSYNC_PASSTHROUGH : VSYNC_VFR) : VSYNC_CFR;

	//计算需要写入几个重复帧以便同步
    if (format_video_sync != VSYNC_PASSTHROUGH) {
        double vdelta = sync_ipts - p_output->ii_video_sync_opts + duration;

        // FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
		if (vdelta < -1.1)
			nb_frames = 0;
		else if (format_video_sync == VSYNC_VFR) {
			if (vdelta <= -0.6) {
				nb_frames = 0;
			} 
			else if (vdelta > 0.6)
				p_output->ii_video_sync_opts = lrintf(sync_ipts);
		} 
		else if (vdelta > 1.1)
			nb_frames = lrintf(vdelta);

        if (nb_frames == 0) {
            //++nb_frames_drop;
            LOGD("*** drop!\n");
        } else if (nb_frames > 1) {
            //nb_frames_dup += nb_frames - 1;
            LOGD("*** %d dup!\n", nb_frames - 1);
        }
    } else
        p_output->ii_video_sync_opts = lrintf(sync_ipts);

    //nb_frames = FFMIN(nb_frames, ost->max_frames - ost->frame_number);
    if (nb_frames <= 0)
        return;

    //do_video_resample(ost, ist, in_picture, &final_picture);

    /* duplicates frame if needed */
    for (i = 0; i < nb_frames; i++) {
        AVPacket pkt;
        av_init_packet(&pkt);
		pkt.data = NULL;
		pkt.size = 0;
		pkt.stream_index = 0;

        if (s->oformat->flags & AVFMT_RAWPICTURE &&
            enc->codec->id == CODEC_ID_RAWVIDEO) {
            /* raw pictures are written as AVPicture structure to
               avoid any copies. We support temporarily the older
               method. */
			//interlaced_frame
            enc->coded_frame->interlaced_frame = in_picture->interlaced_frame;
            enc->coded_frame->top_field_first  = in_picture->top_field_first;
            pkt.data   = (uint8_t *)in_picture;
            pkt.size   =  sizeof(AVPicture);
            pkt.pts    = av_rescale_q(p_output->ii_video_sync_opts, enc->time_base, p_output->p_video->time_base);
            pkt.flags |= AV_PKT_FLAG_KEY;

			write_frame(p_trs, s, &pkt, enc, p_output->p_video_bitstreamfilter);
        } else {
#if LIBAVFORMAT_VERSION_MAJOR <= 53
            AVFrame big_picture;

            big_picture = *in_picture;
            /* better than nothing: use input picture interlaced
               settings */
            big_picture.interlaced_frame = in_picture->interlaced_frame;
            if (p_output->p_video->codec->flags & (CODEC_FLAG_INTERLACED_DCT|CODEC_FLAG_INTERLACED_ME)) {
                if (p_output->top_field_first == -1)
                    big_picture.top_field_first = in_picture->top_field_first;
                else
                    big_picture.top_field_first = !!p_output->top_field_first;
            }

            /* handles same_quant here. This is not correct because it may
               not be a global option */
            big_picture.quality = quality;
            if (!enc->me_threshold)
                big_picture.pict_type = AV_PICTURE_TYPE_NONE;
            big_picture.pts = p_output->ii_video_sync_opts;

			if (p_output->i_video_bit_buffer_size == 0) {
				int size = enc->width * enc->height;
				p_output->i_video_bit_buffer_size = FFMAX(1024*256, 9*size+10000);
				p_output->p_video_bit_buffer = (uint8_t*)av_mallocz(p_output->i_video_bit_buffer_size);
			}

            ret = avcodec_encode_video(enc,
                                       p_output->p_video_bit_buffer, p_output->i_video_bit_buffer_size,
                                       &big_picture);
            if (ret < 0) {
                LOGD("Video encoding failed\n");
                break;
            }

            if (ret > 0) {
                pkt.data = p_output->p_video_bit_buffer;
                pkt.size = ret;
                if (enc->coded_frame->pts != AV_NOPTS_VALUE)
                    pkt.pts = av_rescale_q(enc->coded_frame->pts, enc->time_base, p_output->p_video->time_base);

                if (enc->coded_frame->key_frame)
                    pkt.flags |= AV_PKT_FLAG_KEY;
                write_frame(s, &pkt, enc, p_output->p_video_bitstreamfilter);
                *frame_size = ret;
                //video_size += ret;
            }
#else
			int got_packet;
			in_picture->pts = p_output->ii_video_sync_opts;

			if (p_output->p_video->codec->flags & (CODEC_FLAG_INTERLACED_DCT|CODEC_FLAG_INTERLACED_ME) &&
				p_output->top_field_first >= 0)
				in_picture->top_field_first = !!p_output->top_field_first;

			//interlaced_frame
			if (in_picture->interlaced_frame) {
				if (enc->codec->id == AV_CODEC_ID_MJPEG)
					enc->field_order = in_picture->top_field_first ? AV_FIELD_TT:AV_FIELD_BB;
				else
					enc->field_order = in_picture->top_field_first ? AV_FIELD_TB:AV_FIELD_BT;
			} else
				enc->field_order = AV_FIELD_PROGRESSIVE;

			in_picture->quality = p_output->p_video->codec->global_quality;
			if (!enc->me_threshold)
				in_picture->pict_type = AV_PICTURE_TYPE_NONE;

			ret = avcodec_encode_video2(enc, &pkt, in_picture, &got_packet);
			if (ret < 0) {
				LOGD("Video encoding failed\n");
				break;
			}

			if (got_packet) {
				if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & CODEC_CAP_DELAY))
					pkt.pts = p_output->ii_video_sync_opts;

				if (pkt.pts != AV_NOPTS_VALUE)
					pkt.pts = av_rescale_q(pkt.pts, enc->time_base, p_output->p_video->time_base);
				if (pkt.dts != AV_NOPTS_VALUE)
					pkt.dts = av_rescale_q(pkt.dts, enc->time_base, p_output->p_video->time_base);

				*frame_size = pkt.size;
				//video_size += pkt.size;
				//LOGD("-->Video Frame PTS:%lld\n", pkt.pts);
				write_frame(p_trs, s, &pkt, enc, p_output->p_video_bitstreamfilter);
				av_free_packet(&pkt);
			}
#endif
        }
        p_output->ii_video_sync_opts++;

        /*
         * For video, number of frames in == number of packets out.
         * But there may be reordering, so we can't throw away frames on encoder
         * flush, we need to limit them here, before they go into encoder.
         */
        //ost->frame_number++;
    }
}

/*
 * 视频转码函数：将输入packet解码后调用do_video_out转码
 */
int do_trans_video(trs_state *p_trs, trs_input* p_input, trs_output* p_output, AVPacket* avpkt, int64_t start_time, int *got_output, int64_t *pkt_pts, int64_t *pkt_dts)
{
	AVFrame *decode_frame = 0;
	int i = 0, ret = 0;
    float quality = 0;
    int duration=0;
#ifndef USING_AVFILTER
	int64_t *best_effort_timestamp = 0;
#else
    int64_t best_effort_timestamp = AV_NOPTS_VALUE;
#endif
	AVRational av_time_base_q;
	av_time_base_q.num = 1;av_time_base_q.den = AV_TIME_BASE;

	if (!p_input->p_video_frame && !(p_input->p_video_frame = avcodec_alloc_frame()))
		return AVERROR(ENOMEM);
	else
		avcodec_get_frame_defaults(p_input->p_video_frame);
    decode_frame = p_input->p_video_frame;

	//重新制定avpacket的时间戳及时长
    avpkt->pts  = *pkt_pts;
    avpkt->dts  = *pkt_dts;
    *pkt_pts  = AV_NOPTS_VALUE;

	if (*pkt_dts != AV_NOPTS_VALUE)
		p_input->video_next_pts = *pkt_dts;

    if (avpkt->duration) {
        duration = av_rescale_q(avpkt->duration, p_input->p_video->time_base, av_time_base_q);
    } 
	else if(p_input->p_video->codec->time_base.num != 0) {
        int ticks= p_input->p_video->parser ? p_input->p_video->parser->repeat_pict+1 : p_input->p_video->codec->ticks_per_frame;
        duration = ((int64_t)AV_TIME_BASE *
                          p_input->p_video->codec->time_base.num * ticks) /
                          p_input->p_video->codec->time_base.den;
    }

    if(*pkt_dts != AV_NOPTS_VALUE && duration) {
        *pkt_dts += duration;
    }else
        *pkt_dts = AV_NOPTS_VALUE;
	avpkt->duration = duration;

	ret = avcodec_decode_video2(p_input->p_video->codec, decode_frame, got_output, avpkt);
    if (ret < 0)
        return ret;

	quality = decode_frame->quality;
	//LOGD("using avpkt size=%d ret=%d\n", avpkt->size, ret);
    if (!*got_output) {
        /* no picture yet */
        return ret;
    }

	//获取解码后时间戳
#ifndef USING_AVFILTER
    best_effort_timestamp = (int64_t*)av_opt_ptr(avcodec_get_frame_class(), decode_frame, "best_effort_timestamp");
	if(*best_effort_timestamp != AV_NOPTS_VALUE) {
		p_input->video_next_pts = *best_effort_timestamp;
	}
#else
	best_effort_timestamp = av_frame_get_best_effort_timestamp(decode_frame);
	if(best_effort_timestamp != AV_NOPTS_VALUE) {
		p_input->video_next_pts = best_effort_timestamp;
		decode_frame->pts = best_effort_timestamp - start_time;
	}
#endif
    avpkt->size = 0;

	//
#ifdef USING_AVFILTER
	if (p_output->p_video_filter == 0)	//第一次初始化filtr
		config_video_filter(p_output, p_input);

	trs_filter *p_filter = p_output->p_video_filter;
	if (p_filter) {
		if (!p_filter->filt_frame && !(p_filter->filt_frame=av_frame_alloc()))
			return -1;
		else 
			avcodec_get_frame_defaults(p_filter->filt_frame);

		if (!p_filter->ifilt_frame && !(p_filter->ifilt_frame=av_frame_alloc()))
			return -1;
		else 
			avcodec_get_frame_defaults(p_filter->ifilt_frame);

		av_frame_ref(p_filter->ifilt_frame, decode_frame);		//copy decoded frame
		if (p_filter->ifilt_frame->pts == AV_NOPTS_VALUE) {		//重新计算filter的时间戳
			p_filter->ifilt_frame->pts = p_input->video_next_pts - start_time;
		}

		//添加到filter中等待处理
		int err = av_buffersrc_add_frame_flags(p_filter->buffersrc_ctx, p_filter->ifilt_frame, AV_BUFFERSRC_FLAG_PUSH);
        if (err == AVERROR_EOF)
            err = 0; /* ignore */
        if (err < 0)
            return -1;

		//int r = avfilter_graph_request_oldest(p_filter->filter_graph);

		while (1) {
			//读取filter中已经处理过的frame
			ret = av_buffersink_get_frame_flags(p_filter->buffersink_ctx, p_filter->filt_frame, AV_BUFFERSINK_FLAG_NO_REQUEST);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0)
				return -1;

			//转换frame时间戳
			AVRational bq = {1, AV_TIME_BASE};
			p_filter->filt_frame->pts = av_rescale_q(p_filter->filt_frame->pts, p_output->p_video_filter->buffersink_ctx->inputs[0]->time_base, bq);

			//encode并写入output stream
			p_output->ii_video_nextpts = p_filter->filt_frame->pts;
			int frame_size;
			do_video_out(p_trs, p_output, p_input, p_filter->filt_frame, &frame_size, quality);

			av_frame_unref(p_filter->filt_frame);
		}
		av_frame_unref(p_filter->ifilt_frame);
	}
#else
	p_output->ii_video_nextpts = (p_input->video_next_pts - start_time);
	int frame_size;
	do_video_out(p_trs, p_output, p_input, decode_frame, &frame_size, quality);
#endif

	p_input->video_next_pts += duration;
#if LIBAVFORMAT_VERSION_MAJOR >= 54
	//av_frame_unref(decode_frame);
#endif
	return ret;
}
