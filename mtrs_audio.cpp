#include "mtrs_comm.h"
#include "mpthread.h"
#include "mtrs.h"
#include "mtrs_internal.h"

#if defined( _MSC_VER )
#define strncasecmp _strnicmp
#define snprintf _snprintf
#define strcasecmp	_stricmp
#endif

#if LIBAVFORMAT_VERSION_MAJOR >= 54
/*
 * 初始化AVFrame
 */
static void get_frame_defaults(AVFrame *frame)
{
    if (frame->extended_data != frame->data)
        av_freep(&frame->extended_data);

    memset(frame, 0, sizeof(*frame));

    frame->pts                   =
    frame->pkt_dts               =
    frame->pkt_pts               = AV_NOPTS_VALUE;
    av_frame_set_best_effort_timestamp(frame, AV_NOPTS_VALUE);
    av_frame_set_pkt_duration         (frame, 0);
    av_frame_set_pkt_pos              (frame, -1);
    av_frame_set_pkt_size             (frame, -1);
    frame->key_frame           = 1;
#if defined( _MSC_VER )
	AVRational avr_ratio = {0, 1};
	frame->sample_aspect_ratio = avr_ratio;
#else
	frame->sample_aspect_ratio = (AVRational){ 0, 1 };
#endif
    frame->format              = -1; /* unknown */
    frame->colorspace          = AVCOL_SPC_UNSPECIFIED;
    frame->extended_data       = frame->data;
}

#ifdef	USING_AVFILTER
/*
 * configure avfilter，用到了 abuffer->aformat->aresample->abuffersink
 */
static int config_audio_filter(trs_output_audio* p_output, trs_input_audio* p_input)
{
	int ret = 0;
	char args[512];
	char filters_descr[512] = {0};
	char filters_aformat[512] = {0};
	AVCodecContext *enc_ctx = p_output->p_audio->codec;
	AVCodecContext *dec_ctx = p_input->p_audio->codec;
	AVRational time_base = p_input->p_audio->time_base;

	trs_filter* p_filter = p_output->p_audio_filter;
	if (!p_filter) 
		p_filter = (trs_filter*)av_mallocz(sizeof(trs_filter));

	avfilter_register_all();
    AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVCodec *codec = (AVCodec*)enc_ctx->codec;

    p_filter->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !p_filter->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    if (!dec_ctx->channel_layout)
        dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
	
    snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, time_base.num, 
		time_base.den, dec_ctx->sample_rate, av_get_sample_fmt_name(dec_ctx->sample_fmt), dec_ctx->channel_layout);

	//create abuffer
    ret = avfilter_graph_create_filter(&p_filter->buffersrc_ctx, abuffersrc, "in", args, NULL, p_filter->filter_graph);
    if (ret < 0) {
        LOGD("Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&p_filter->buffersink_ctx, abuffersink, "out", NULL, NULL, p_filter->filter_graph);
    if (ret < 0) {
        LOGD("Cannot create audio buffer sink\n");
        goto end;
    }
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = p_filter->buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = p_filter->buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

	//指定sample_fmts为fl32p
	if (dec_ctx->sample_fmt != enc_ctx->sample_fmt) {
		snprintf(filters_aformat, 512, "aformat=sample_fmts=%s", av_get_sample_fmt_name(enc_ctx->sample_fmt));
		if (dec_ctx->channels != enc_ctx->channels) {	//指定channel为stereo
			strcat(filters_aformat, ":channel_layouts=stereo");
		}
	}

	//指定采样率，必要时重新采样
	if (dec_ctx->sample_rate != enc_ctx->sample_rate) {
		snprintf(filters_descr, 512, "aresample=%d,%s", enc_ctx->sample_rate, filters_aformat);
	}
	else {
		strcpy(filters_descr, filters_aformat);
	}

	if (strlen(filters_descr) > 0) {
		if ((ret = avfilter_graph_parse_ptr(p_filter->filter_graph, filters_descr, &inputs, &outputs, NULL)) < 0)
			goto end;

		if ((ret = avfilter_graph_config(p_filter->filter_graph, NULL)) < 0)
			goto end;

		/* Print summary of the sink buffer
		* Note: args buffer is reused to store channel layout string */
		const AVFilterLink *outlink;
		outlink = p_filter->buffersink_ctx->inputs[0];
		av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
		LOGD("Output: srate:%dHz fmt:%s chlayout:%s\n",
			(int)outlink->sample_rate,
			(char *)av_x_if_null(av_get_sample_fmt_name((AVSampleFormat)outlink->format), "?"),
			args);
	}
	else {
		avfilter_link(p_filter->buffersrc_ctx, 0, p_filter->buffersink_ctx, 0);

		if ((ret = avfilter_graph_config(p_filter->filter_graph, NULL)) < 0)
			goto end;
	}

    if ((ret = av_opt_set_int(p_filter->buffersink_ctx, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

	if (codec == 0)
		codec = avcodec_find_encoder_by_name("aac");
	if (codec && !(codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)) 
	{
		av_buffersink_set_frame_size(p_filter->buffersink_ctx, enc_ctx->frame_size);
	}

	p_output->p_audio_filter = p_filter;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
	return ret;
}
#endif

/*
 * encode frame.
 */
static int encode_audio_frame(trs_state *p_trs, trs_output_audio* p_output, AVFormatContext *oc,
                              trs_input_audio* p_input, AVFrame *frame)
{
	AVFormatContext *s = oc;

    AVCodecContext *enc = p_output->p_audio->codec;
    AVFrame *oframe = NULL;
    AVPacket pkt;
    int ret, got_packet;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

#ifndef	USING_AVFILTER
    if (!p_output->audio_output_frame) {
        p_output->audio_output_frame = avcodec_alloc_frame();
        if (!p_output->audio_output_frame) {
            LOGD("out-of-memory in encode_audio_frame()\n");
            return AVERROR(ENOMEM);
        }
    }
	else {
		avcodec_get_frame_defaults(p_output->audio_output_frame);
	}
    oframe = p_output->audio_output_frame;

    if (frame) {
        if (oframe->extended_data != oframe->data)
            av_freep(&oframe->extended_data);
        avcodec_get_frame_defaults(oframe);
		*oframe = *frame;

		oframe->pts = AV_NOPTS_VALUE;
		if (frame->pts != AV_NOPTS_VALUE) {
			oframe->pts = av_rescale_q(frame->pts, p_input->p_audio->codec->time_base, enc->time_base);
		}
		else {
			AVRational bq = {1, AV_TIME_BASE};
			oframe->pts = av_rescale_q(p_input->audio_next_pts, bq, enc->time_base);
		}

		if (frame->extended_data == frame->data)
			oframe->extended_data = oframe->data;

		memset(frame, 0, sizeof(*frame));
		get_frame_defaults(frame);
    }
#else
	oframe = frame;
#endif

    got_packet = 0;
    if (avcodec_encode_audio2(enc, &pkt, oframe, &got_packet) < 0) {
        LOGD("Audio encoding failed\n");
#if LIBAVFORMAT_VERSION_MAJOR >= 54
#ifndef	USING_AVFILTER
		av_frame_unref(oframe);
#endif
#endif
        return -1;
    }

    ret = pkt.size;
    if (oframe)
        p_output->audio_sync_opts += oframe->nb_samples;

    if (got_packet) {
		pkt.stream_index = p_output->i_audio_stream_index;
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts      = av_rescale_q(pkt.pts,      enc->time_base, p_output->p_audio->time_base);
        else
#if defined( _MSC_VER )
		{
			AVRational bq = {1, AV_TIME_BASE};
            pkt.pts     = av_rescale_q(p_output->ii_audio_nextpts, bq, p_output->p_audio->time_base);
		}
#else
        pkt.pts     = av_rescale_q(p_output->ii_audio_nextpts, (AVRational){1,AV_TIME_BASE}, p_output->p_audio->time_base);//AV_NOPTS_VALUE;//
#endif
        
        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts      = AV_NOPTS_VALUE;//av_rescale_q(pkt.dts,      enc->time_base, p_output->p_audio->time_base);

		if (pkt.duration > 0)
            pkt.duration = av_rescale_q(pkt.duration, enc->time_base, p_output->p_audio->time_base);

		//叠加已经处理过的audio sample
		{
			AVRational bq = {1, AV_TIME_BASE};
			AVRational sample_rate_q;
			sample_rate_q.num = 1;
			sample_rate_q.den = enc->sample_rate;
			p_output->ii_audio_nextpts += av_rescale_q(enc->frame_size, sample_rate_q, bq);
		}

        write_frame(p_trs, s, &pkt, enc, p_output->p_audio_bitstreamfilter);
        av_free_packet(&pkt);
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 54
#ifndef	USING_AVFILTER
	av_frame_unref(oframe);
#endif
#endif

    return ret;
}

/*
 * 转码函数：音频解码后，调用encode函数编码
 */
static int do_audio_out(trs_state *p_trs, trs_input_audio* p_input, trs_output_audio* p_output, AVFormatContext *oc, AVFrame *decode_frame)
{
	int ret = 0;
	AVCodecContext *enc = p_output->p_audio->codec;
	AVCodecContext *dec = p_input->p_audio->codec;
	uint8_t *buf = decode_frame->extended_data[0];

#if 1
#ifdef	USING_AVFILTER
	if (p_output->p_audio_filter == 0)	//第一次初始化filtr
		config_audio_filter(p_output, p_input);

	trs_filter *p_filter = p_output->p_audio_filter;
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
			AVRational bq = {1, AV_TIME_BASE};
			p_filter->ifilt_frame->pts = av_rescale_q(p_input->audio_next_pts, bq, p_input->p_audio->time_base);
		}

		//添加到filter中等待处理
		int err = av_buffersrc_add_frame_flags(p_filter->buffersrc_ctx, p_filter->ifilt_frame, AV_BUFFERSRC_FLAG_PUSH);
		av_frame_unref(p_filter->ifilt_frame);
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
			p_filter->filt_frame->pts = av_rescale_q(p_filter->filt_frame->pts, p_input->p_audio->time_base, enc->time_base);;

			//encode并写入output stream
			encode_audio_frame(p_trs, p_output, oc, p_input, p_filter->filt_frame);
			av_frame_unref(p_filter->filt_frame);
		}
	}
#else
	if (p_output->p_audio_filter == 0)
		p_output->p_audio_filter = (trs_filter *)av_mallocz(sizeof(trs_filter));

	if (decode_frame) {
		trs_filter_audio_item *pItem = (trs_filter_audio_item *)av_mallocz(sizeof(trs_filter_audio_item));
		pItem->ii_pts = decode_frame->pts;
		if (pItem->ii_pts == AV_NOPTS_VALUE) {
			AVRational bq = {1, AV_TIME_BASE};
			pItem->ii_pts = av_rescale_q(p_input->audio_next_pts, bq, p_input->p_audio->codec->time_base);
		}

		pItem->i_off = 0;
		pItem->i_samples = decode_frame->nb_samples;

		//LOGD("--->>> psh sample: pts=%lld, samples=%d, off=%d\n", pItem->ii_pts, decode_frame->nb_samples, 0);

		AVFrame *copy = 0;
		copy = av_frame_alloc();
		if (copy == 0) {
			LOGD("error\n");
		}
		int ret = av_frame_ref(copy, decode_frame);
		pItem->p_filter_frame = copy;

		if (!p_output->p_audio_filter->pHeader) {
			p_output->p_audio_filter->pHeader = pItem;
			p_output->p_audio_filter->i_off = pItem->i_off;
		}
		else {
			p_output->p_audio_filter->pLast->pNext = pItem;
		}

		p_output->p_audio_filter->i_samples += pItem->i_samples;
		p_output->p_audio_filter->pLast = pItem;
	}

	int n_last_filter_off = p_output->p_audio_filter->i_off;
	int n_last_filter_frames = p_output->p_audio_filter->i_samples;
	trs_filter_audio_item *pHeader = p_output->p_audio_filter->pHeader;
	int n_last_frames = n_last_filter_frames - n_last_filter_off;
	int64_t ii_pts = AV_NOPTS_VALUE;
	//LOGD("--->>>before enter encode_audio_frame n_last_frames:%d\n", n_last_frames);

	while (n_last_frames >= enc->frame_size) {
		AVFrame *copy2 = NULL;
		copy2 = av_frame_alloc();
		if (copy2 == 0) {
			LOGD("error\n");
		}
		copy2->format         = decode_frame->format;
		copy2->width          = decode_frame->width;
		copy2->height         = decode_frame->height;
		copy2->channels       = decode_frame->channels;
		av_frame_set_channels(copy2, decode_frame->channels);
		copy2->channel_layout = decode_frame->channel_layout;
		copy2->nb_samples     = enc->frame_size;
		av_frame_copy_props(copy2, decode_frame);
		ret = av_frame_get_buffer(copy2, 0);

		if (ret >= 0) {
			int n_copy_dest = 0;

			while ( (n_last_filter_frames - n_last_filter_off > 0) && (n_copy_dest < enc->frame_size) ) {
				if (!pHeader) {
					break;
				}

				AVFrame* p_last_frame = pHeader->p_filter_frame;

				int ch = p_last_frame->channels;
				int copy_samples = FFMIN(enc->frame_size - n_copy_dest, pHeader->i_samples - pHeader->i_off);
				if (ii_pts == AV_NOPTS_VALUE) {
					int64_t ii_off = ((int64_t)AV_TIME_BASE * pHeader->i_off) / p_input->p_audio->codec->sample_rate;
					AVRational bq = {1, AV_TIME_BASE};

					ii_pts = pHeader->ii_pts + av_rescale_q(ii_off, bq, p_input->p_audio->codec->time_base);
				}

				av_samples_copy(copy2->extended_data, p_last_frame->extended_data, n_copy_dest, pHeader->i_off,
					copy_samples, ch, (AVSampleFormat)copy2->format);

				//LOGD("--->>> pop sample: pts=%lld, samples=%d, off=%d, copy=%d\n", ii_pts, pHeader->i_samples, pHeader->i_off, copy_samples);
				//LOGD("--->>> dst sample: pts=%lld, samples=%d, off=%d, copy=%d\n", ii_pts, enc->frame_size, n_copy_dest, copy_samples);

				n_copy_dest += copy_samples;
				pHeader->i_off += copy_samples;
				n_last_filter_off += copy_samples;

				if (pHeader->i_samples > pHeader->i_off) 
					break;

				p_output->p_audio_filter->i_samples -= pHeader->i_samples;
				av_frame_unref(p_last_frame);
				av_frame_free(&p_last_frame);

				trs_filter_audio_item *pItem = pHeader;
				pHeader = pHeader->pNext;
				av_free(pItem);
			}

			copy2->extended_data = copy2->data;
			copy2->pts = ii_pts;

			if (dec->sample_fmt != AV_SAMPLE_FMT_FLTP)
				LOGD("Need format the sample format as flt\n");

			encode_audio_frame(p_output, oc, p_input, copy2);
			//LOGD("<<<---out encode_audio_frame\n");
		}
		n_last_frames -= enc->frame_size;
		av_frame_unref(copy2);
		av_frame_free(&copy2);
	}

	p_output->p_audio_filter->pHeader = pHeader;
	//p_output->p_audio_filter->i_samples = n_last_filter_frames - n_last_filter_off;
	p_output->p_audio_filter->i_off = 0;
	if (!pHeader && p_output->p_audio_filter->i_samples > 0) {
		LOGD("====>>>>error\n");
	}

	if (pHeader) {
		//p_output->p_audio_filter->i_samples += pHeader->i_off;
		p_output->p_audio_filter->i_off = pHeader->i_off;
	}
#endif
#else
	AVFrame *copy = NULL;
	copy = av_frame_alloc();
	int ret = av_frame_ref(copy, decode_frame);
	if (ret >= 0) {
		encode_audio_frame(p_output, oc, p_input, copy);
	}
	av_frame_unref(copy);
	av_frame_free(&copy);
#endif

	decode_frame->pts = AV_NOPTS_VALUE;
    return ret;
}

#else
static int encode_audio_frame(trs_output_audio* p_output, AVFormatContext *oc,
                              const uint8_t *buf, int buf_size)

{
	AVFormatContext *s = oc;

    AVCodecContext *enc = p_output->p_audio->codec;
    AVFrame *frame = NULL;
    AVPacket pkt;
    int ret, got_packet;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (!p_output->audio_output_frame) {
        p_output->audio_output_frame = avcodec_alloc_frame();
        if (!p_output->audio_output_frame) {
            LOGD("out-of-memory in encode_audio_frame()\n");
            return AVERROR(ENOMEM);
        }
    }
    frame = p_output->audio_output_frame;

    if (buf) {
        if (frame->extended_data != frame->data)
            av_freep(&frame->extended_data);
        avcodec_get_frame_defaults(frame);

        frame->nb_samples  = buf_size / (enc->channels * av_get_bytes_per_sample(enc->sample_fmt));
        if ((ret = avcodec_fill_audio_frame(frame, enc->channels, enc->sample_fmt, buf, buf_size, 1)) < 0) {
            LOGD("Audio encoding failed\n");
#if LIBAVFORMAT_VERSION_MAJOR >= 54
			av_frame_unref(frame);
#endif
            return -1;
        }
    }

    if (frame->pts == AV_NOPTS_VALUE) {
        AVRational bq = {1, enc->sample_rate};
        frame->pts =  av_rescale_q(p_output->ii_audio_nextpts, bq, enc->time_base);
    }
    
    got_packet = 0;
    if (avcodec_encode_audio2(enc, &pkt, frame, &got_packet) < 0) {
        LOGD("Audio encoding failed\n");
#if LIBAVFORMAT_VERSION_MAJOR >= 54
		av_frame_unref(frame);
#endif
        return -1;
    }

    ret = pkt.size;

    if (got_packet) {
		pkt.stream_index = p_output->i_audio_stream_index;
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts      = av_rescale_q(pkt.pts,      enc->time_base, p_output->p_audio->time_base);
        else
#if defined( _MSC_VER )
		{
			AVRational bq = {1, enc->sample_rate};
            pkt.pts     = av_rescale_q(p_output->ii_audio_nextpts, bq, p_output->p_audio->time_base);
		}
#else
        pkt.pts     = av_rescale_q(p_output->ii_audio_nextpts, (AVRational){1,enc->sample_rate}, p_output->p_audio->time_base);//AV_NOPTS_VALUE;//
#endif
        
        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts      = AV_NOPTS_VALUE;//av_rescale_q(pkt.dts,      enc->time_base, p_output->p_audio->time_base);
//        else
//#if defined( _MSC_VER )
//		{
//			AVRational bq = {1, enc->sample_rate};
//            pkt.dts     = av_rescale_q(p_output->ii_audio_nextpts, bq, p_output->p_audio->time_base);
//		}
//#else
//        pkt.dts     = av_rescale_q(p_output->ii_audio_nextpts, (AVRational){1,enc->sample_rate}, p_output->p_audio->time_base);//AV_NOPTS_VALUE;//
//#endif
        
        if (pkt.duration > 0)
            pkt.duration = av_rescale_q(pkt.duration, enc->time_base, p_output->p_audio->time_base);

        p_output->ii_audio_nextpts += enc->frame_size;

		if (pkt.dts >= pkt.pts)
			pkt.pts += 1;
        write_frame(s, &pkt, enc, p_output->p_audio_bitstreamfilter);
        av_free_packet(&pkt);
    }

    if (frame)
        p_output->audio_sync_opts += frame->nb_samples;
#if LIBAVFORMAT_VERSION_MAJOR >= 54
	av_frame_unref(frame);
#endif

    return ret;
}

#ifdef __cplusplus
extern "C" {
#endif
    void int16_to_float_fmul_scalar_neon(int16_t* i_s16, int i_len, float* f_flt, float f_expr);
#ifdef __cplusplus
}
#endif
static int do_audio_out(trs_input_audio* p_input, trs_output_audio* p_output, AVFormatContext *oc, AVFrame *decode_frame)
{
	AVCodecContext *enc = p_output->p_audio->codec;
	AVCodecContext *dec = p_input->p_audio->codec;
    uint8_t *buf = decode_frame->data[0];
	int isize = av_get_bytes_per_sample(dec->sample_fmt);
    int osize = av_get_bytes_per_sample(enc->sample_fmt);
    int size     = decode_frame->nb_samples * dec->channels * isize;
    int frame_bytes = enc->frame_size * osize * enc->channels;
	uint8_t *buftmp;
	int64_t size_out;
	buftmp = buf;
	size_out = size;
    uint8_t* audio_buf = (uint8_t*)av_mallocz(frame_bytes);
    
    if (dec->sample_fmt == AV_SAMPLE_FMT_S16 && buf)
    {
        size_out = osize * decode_frame->nb_samples * dec->channels;
        if (p_output->i_size_aacbuf < size_out) {
            p_output->p_u8_aacbuf = (uint8_t*)av_realloc(p_output->p_u8_aacbuf, size_out);
            p_output->i_size_aacbuf = size_out;
        }
        
        float *f_flt = (float*)p_output->p_u8_aacbuf;
        int16_t* i_s16 = (int16_t*)buf;
#if 0
        float expr = 1.0 / (1<<15);
        for (int i = 0; i < decode_frame->nb_samples * dec->channels; i ++)
        {
            *f_flt = *i_s16 * expr;
            f_flt++;
            i_s16++;
        }
#else
#if defined(HAVE_NEON)
        float expr = 1.0 / (1<<15);
        int16_to_float_fmul_scalar_neon(i_s16, decode_frame->nb_samples * dec->channels, f_flt, expr);
#else
        union {float f; int32_t i;}u;
        for (int i = 0; i < decode_frame->nb_samples * dec->channels; i ++)
        {
            u.i = *i_s16 + 0x43c00000;
            *f_flt = u.f - 384.0;
            i_s16 ++;
            f_flt ++;
        }
#endif
#endif
        buftmp = p_output->p_u8_aacbuf;
    }
    else if (dec->sample_fmt == AV_SAMPLE_FMT_S32 && buf) {
        size_out = osize * decode_frame->nb_samples * dec->channels;
        if (p_output->i_size_aacbuf < size_out) {
            p_output->p_u8_aacbuf = (uint8_t*)av_realloc(p_output->p_u8_aacbuf, size_out);
            p_output->i_size_aacbuf = size_out;
        }
        
        float *f_flt = (float*)p_output->p_u8_aacbuf;
        int32_t* i_s32 = (int32_t*)buf;
        float expr = 1.0/(1<<31);
        
        for (int i = 0; i < decode_frame->nb_samples * dec->channels; i ++)
        {
            *f_flt = *i_s32 * expr;
            f_flt++;
            i_s32++;
        }
        
        buftmp = p_output->p_u8_aacbuf;
    }
    
    decode_frame->pts = AV_NOPTS_VALUE;
    int ret = 0;
    if (!(enc->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE))
    {
        av_fifo_realloc2(p_output->fifo, av_fifo_size(p_output->fifo)+size_out);
        av_fifo_generic_write(p_output->fifo, buftmp, size_out, NULL);
        while (av_fifo_size(p_output->fifo) >= frame_bytes)
        {
            av_fifo_generic_read(p_output->fifo, audio_buf, frame_bytes, NULL);
            encode_audio_frame(p_output, oc, audio_buf, frame_bytes);
//            p_output->ii_audio_nextpts += enc->frame_size;
        }
        
        if (!buf)
            encode_audio_frame(p_output, oc, NULL, 0);
        
//        p_output->ii_audio_nextpts = AV_NOPTS_VALUE;
    }
    if (audio_buf)
        av_free(audio_buf);
    audio_buf = 0;
    
    return ret;
}
#endif

/*
 * 音频转码函数，将输入packet解码后调用do_audio_out转码
 */
int do_trans_audio(trs_state *p_trs, trs_input_audio* p_input, trs_output_audio* p_output, AVFormatContext *oc, AVPacket* avpkt, int64_t start_time)
{
	AVFrame *decode_frame;
	int i, ret;

	AVCodecContext *avctx = p_input->p_audio->codec;
	if (!p_input->p_audio_frame && !(p_input->p_audio_frame = avcodec_alloc_frame()))
		return AVERROR(ENOMEM);
	else
		avcodec_get_frame_defaults(p_input->p_audio_frame);

	decode_frame = p_input->p_audio_frame;
	int got_output;
//    if (start_time != AV_NOPTS_VALUE){
//        avpkt->pts -= start_time;
//        avpkt->dts -= start_time;
//    }
	//音频解码
    ret = avcodec_decode_audio4(avctx, decode_frame, &got_output, avpkt);
    if (ret < 0) {
        return ret;
    }

    if (!got_output) {
        /* no audio frame */
        return ret;
    }

	//计算解码后packet的有效时间戳
    AVRational time_base = avctx->time_base;
	int64_t decode_frame_pts = decode_frame->pts;
    if (decode_frame->pts != AV_NOPTS_VALUE) {
        
    }
    else if (decode_frame->pkt_pts != AV_NOPTS_VALUE) {
        decode_frame_pts = decode_frame->pkt_pts;
        time_base = p_input->p_audio->time_base;
    }
    else if (avpkt->pts != AV_NOPTS_VALUE) {
        decode_frame_pts = avpkt->pts;
        time_base = p_input->p_audio->time_base;
        avpkt->pts = AV_NOPTS_VALUE;
    }
    
	if (decode_frame_pts != AV_NOPTS_VALUE) {
//		p_input->audio_next_pts = av_rescale_q(decode_frame_pts, time_base, AV_TIME_BASE_Q);
#if defined( _MSC_VER )
        AVRational sample_rate_q;
        sample_rate_q.num = 1;
        //sample_rate_q.den = avctx->sample_rate;
		sample_rate_q.den =	AV_TIME_BASE;
		int64_t ost_tb_start_time = av_rescale_q(start_time, p_input->p_audio->time_base, sample_rate_q);
        p_input->audio_next_pts = my_rescale_delta(time_base, decode_frame_pts, sample_rate_q,
                                                   decode_frame->nb_samples, &p_input->audio_rescale_delta_last, sample_rate_q) - ost_tb_start_time;
#else
		int64_t ost_tb_start_time = av_rescale_q(start_time, p_input->p_audio->time_base, AV_TIME_BASE_Q/*(AVRational){1,avctx->sample_rate}*/);
        p_input->audio_next_pts = my_rescale_delta(time_base, decode_frame_pts, (AVRational){1,avctx->sample_rate},
                                                   decode_frame->nb_samples, &p_input->audio_rescale_delta_last, AV_TIME_BASE_Q/*(AVRational){1,avctx->sample_rate}*/) - ost_tb_start_time;
#endif
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 54
    if (p_output->ii_audio_nextpts == AV_NOPTS_VALUE) {
        AVRational sample_rate_q;
        sample_rate_q.num = 1;
        sample_rate_q.den = avctx->sample_rate;
		
		AVRational time_base_q;
		time_base_q.num = 1;
		time_base_q.den = AV_TIME_BASE;

        int64_t ost_tb_start_time = av_rescale_q(start_time, p_input->p_audio->time_base, time_base_q);
        //p_output->ii_audio_nextpts = av_rescale_q(p_input->audio_next_pts, time_base_q, sample_rate_q) - ost_tb_start_time;
		p_output->ii_audio_nextpts = p_input->audio_next_pts;
    }
#else
    if (p_output->ii_audio_nextpts == AV_NOPTS_VALUE) {
        AVRational sample_rate_q;
        sample_rate_q.num = 1;
        sample_rate_q.den = avctx->sample_rate;

		AVRational time_base_q;
		time_base_q.num = 1;
		time_base_q.den = AV_TIME_BASE;

		int64_t ost_tb_start_time = av_rescale_q(start_time, p_input->p_audio->time_base, sample_rate_q);
        //p_output->ii_audio_nextpts = p_input->audio_next_pts - ost_tb_start_time;
		p_output->ii_audio_nextpts = av_rescale_q(p_input->audio_next_pts, time_base_q, sample_rate_q) - ost_tb_start_time;
    }
#endif

	ret = do_audio_out(p_trs, p_input, p_output, oc, decode_frame);

	p_input->audio_next_pts += ((int64_t)AV_TIME_BASE * decode_frame->nb_samples) / avctx->sample_rate;

#if LIBAVFORMAT_VERSION_MAJOR >= 54
	av_frame_unref(decode_frame);
#endif
	return ret;
}
