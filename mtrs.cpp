#include "mtrs_comm.h"
#include "mpthread.h"
#include "mtrs.h"
#include "mtrs_internal.h"

#if defined( _MSC_VER )
#define strncasecmp _strnicmp
#define snprintf _snprintf
#define strcasecmp	_stricmp
#endif

//#define	PAD_0_TSFILE	//允许出现0字节文件，再实时播放时使用

///////////////////////////////////////////////////////////////////////////////////////////////////////////
static void* trs_thread(void* param);
static int decode_interrupt_cb(void*);
anc_threadvar_t	tls_trsstate_key = 0;

//#define LOG_FFMPEG
#ifdef LOG_FFMPEG
static inline void LibavutilCallback( void *p_opaque, int i_level, const char *psz_format, va_list va )
{
	AVCodecContext *p_avctx = (AVCodecContext *)p_opaque;
	const AVClass *p_avc;
	char buffer[2048];

	switch( i_level )
	{
	case AV_LOG_DEBUG:
	case AV_LOG_INFO:
	case AV_LOG_ERROR:
	case AV_LOG_QUIET:
		vsnprintf( buffer, sizeof(buffer), psz_format, va );
		LOGD( buffer );
		break;
	}
}
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
 * filename:		输入要转码的文件名
 * dir:				转码的输出目录
 * f_start:			转码开始位置(秒)
 * f_seg_duration:	转码片段长度(秒)
 * aac_encoder:		aac encoder 函数：0(default)，1(faac)，2(twoloop)
 * p_lib:			context for callback
 * cb:				callback
 * options:			其他输入参数(key-value对)
 */

void* trs_open(const char* filename, const char* dir, float f_start, float f_seg_duration, int aac_encoder, void* p_lib, trans_event *cb, void* options)
{
	if (!filename || !*filename || !dir || !*dir)
		return 0;

	trs_state* p_trs = (trs_state*)av_mallocz(sizeof(trs_state));
	if (!p_trs)
		return 0;
	
    if (tls_trsstate_key == 0)
	{
		anc_threadvar_create(&tls_trsstate_key, NULL);
	}
    
	av_log_set_flags(AV_LOG_SKIP_REPEATED);
    
	/* register all codecs, demux and protocols */
	avcodec_register_all();
	av_register_all();
#ifdef LOG_FFMPEG
	av_log_set_callback( LibavutilCallback );
#endif

	p_trs->f_start_time = f_start;
	p_trs->f_segmenter_duration = f_seg_duration;
    if (p_trs->f_segmenter_duration <= 0)
        p_trs->f_segmenter_duration = 0;
    else if (p_trs->f_segmenter_duration < 10)
        p_trs->f_segmenter_duration = 10;
	p_trs->pause = 0;
	p_trs->abort = 0;
	p_trs->sek_req = 0;
	p_trs->f_seek = 0;
    p_trs->i_seekindex = -1;
	p_trs->p_context = p_lib;
    p_trs->i_aac_encoder = 2;
    if (aac_encoder == 2 || aac_encoder == 1)
        p_trs->i_aac_encoder = aac_encoder;
	strcpy(p_trs->p_filename, filename);
    
    strcpy(p_trs->p_dir, dir);
    size_t dest_len = strlen(p_trs->p_dir);
    if (dest_len < FILENAME_LEN)
    {
#ifdef WIN32
        if (p_trs->p_dir[dest_len-1] != '\\')
        {
            p_trs->p_dir[dest_len] = '\\';
        }
#else
        if (p_trs->p_dir[dest_len-1] != '/')
        {
            p_trs->p_dir[dest_len] = '/';
        }
#endif
    }
    if (dest_len + 1 < FILENAME_LEN)
        p_trs->p_dir[dest_len+1] = '\0';
    
#if LIBAVFORMAT_VERSION_MAJOR >= 53
    p_trs->av_options = 0;
    if (options) {
        AVDictionary* opts = (AVDictionary*)options;
        av_dict_copy(&p_trs->av_options, opts, 0);
    }
#endif
    memset(p_trs->psz_playlist, 0, sizeof(p_trs->psz_playlist));
    p_trs->event_cb = cb;

	trs_initcr(p_trs);
	if(anc_clone(&p_trs->trs_tid, trs_thread, p_trs, 0))
	{
		av_free(p_trs);
		return 0;
	}

	return p_trs;
}

void trs_close(void* handle)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs)
	{
		if (p_trs->trs_tid)
		{
            if (p_trs->i_entry_avread) {
                //anc_cancel(p_trs->trs_tid);
                LOGD("=====>>>cancel the trrans thread!\n");
            }
			anc_join(p_trs->trs_tid, 0);
			p_trs->trs_tid = 0;
		}
        
        if (p_trs->av_options)
            av_dict_free(&p_trs->av_options);
        p_trs->av_options = 0;
        
        if (p_trs->event_cb)
            av_free(p_trs->event_cb);
        p_trs->event_cb = 0;
        
		av_free(p_trs);
	}
}

void trs_seek(void* handle, float f_pos)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs && !p_trs->sek_req)
	{
		p_trs->sek_req = 1;
		p_trs->f_seek = f_pos;
        p_trs->i_seekindex = -1;
	}
}

void trs_seekindex(void* handle, int32_t i_index)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs && !p_trs->sek_req)
	{
		p_trs->sek_req = 1;
		p_trs->f_seek = 0;
        p_trs->i_seekindex = i_index;
	}
}

void trs_start(void* handle)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs)
	{
		p_trs->pause = 0;
		p_trs->abort = 0;
	}
}

void trs_pause(void* handle)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs)
	{
		p_trs->pause = 1;
	}
}

void trs_stop(void* handle)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs)
	{
		p_trs->abort = 1;
	}
}

void trs_getItemByIndex(void* handle, int32_t i_index, char** url, double *f_start, double *f_duration)
{
	trs_state* p_trs = (trs_state*)handle;
	if (p_trs && url && f_start && f_duration)
    {
        *f_start = 0;
        *f_duration = 0;
        if (*url)
            free(*url);
        *url = 0;
        if (i_index < p_trs->i_output_total)
        {
            *url = strdup(p_trs->p_list[i_index].p_filename);
            *f_start = p_trs->p_list[i_index].f_start_time;
            *f_duration = p_trs->p_list[i_index].f_duration;
        }
    }
}

double trs_getDuration(void* handle)
{
	trs_state* p_trs = (trs_state*)handle;
    if (p_trs && p_trs->p_input)
        return p_trs->p_input->f_duration;
    return 0;
}

void trs_getPlaylist(void* handle, char** url, int32_t *i_segments)
{
	trs_state* p_trs = (trs_state*)handle;
    if (p_trs && url) {
        if (*url)
            free(*url);
        *url = 0;
        *url = strdup(p_trs->psz_playlist);
        
        if (i_segments)
            *i_segments = p_trs->i_output_total;
    }
}

static int trs_open_input_file(AVFormatContext** ppic, const char* filename, AVInputFormat* iformat);
int trs_canwork(const char* filename, void* options)
{
    if (!filename || !*filename)
        return -1;
		
	avformat_network_init();
	av_log_set_flags(AV_LOG_SKIP_REPEATED);
    
	/* register all codecs, demux and protocols */
	avcodec_register_all();
	av_register_all();
#ifdef LOG_FFMPEG
	av_log_set_callback( LibavutilCallback );
#endif
    
    int ret = -1, i;
    int i_support_audio = 0, i_support_video = 0, i_support_format = 0;
    
#if LIBAVFORMAT_VERSION_MAJOR >= 54
    enum AVCodecID vcodec_id;
    enum AVCodecID *acodec_id = 0;
#else
    enum CodecID vcodec_id;
    enum CodecID *acodec_id = 0;
#endif
    int i_audio_tracks = 0;
    
    AVFormatContext* pic = 0;
    AVInputFormat *ifmt = 0;
	int err = trs_open_input_file(&pic, filename, ifmt);
	if (err < 0)
	{
		ret = -1;
		goto fail;
	}
    
	err = av_find_stream_info(pic);
	if (err < 0)
	{
		LOGI("%s: could not find codec parameters\n", filename);
		ret = -1;
		goto fail;
	}
    if (pic->pb)
        pic->pb->eof_reached = 0;
    
	if (pic->iformat && pic->iformat->name)
	{
		if (!strcasecmp(pic->iformat->name, "matroska,webm") 
			|| !strcasecmp(pic->iformat->name, "mpegts")
            || !strcasecmp(pic->iformat->name, "flv"))
		{
			i_support_format = 1;
		}
		else
		{
			i_support_format = 0;
		}
	}
	else
	{
		i_support_format = 1;
	}
	
#if LIBAVFORMAT_VERSION_MAJOR >= 54
    acodec_id = (enum AVCodecID*)av_mallocz(sizeof(enum AVCodecID)*pic->nb_streams);
#else
	acodec_id = (enum CodecID*)av_mallocz(sizeof(enum AVCodecID)*pic->nb_streams);
#endif
	for(i=0; i < pic->nb_streams; i++)
	{
        AVStream *st = pic->streams[i];
        AVCodecContext *dec = st->codec;
        switch (dec->codec_type) 
		{
            case AVMEDIA_TYPE_AUDIO:
			{
				i_support_audio = 1;
				//if (dec->codec_id == CODEC_ID_AAC)
				//{
				//	i_support_audio = 1;
				//	LOGI("just copy audio codec\n");
				//}
#ifdef __IOS__
#if LIBAVFORMAT_VERSION_MAJOR >= 53
                if (dec->codec_id == CODEC_ID_AC3 || dec->codec_id == CODEC_ID_EAC3 || dec->codec_id == CODEC_ID_TRUEHD) {
                    if (options) {
                        AVDictionary* opts = (AVDictionary*)options;
                        AVDictionaryEntry *ac3 = av_dict_get(opts, "ac3", NULL, 0);
                        if (ac3 && !strcmp(ac3->value, "1")) {
                            i_support_audio = 1;
                        }
                        else
                            i_support_audio = 0;
                    }
                    else
                        i_support_audio = 0;
                }
                else if (dec->codec_id == CODEC_ID_DTS) {
                    if (options) {
                        AVDictionary* opts = (AVDictionary*)options;
                        AVDictionaryEntry *dts = av_dict_get(opts, "dts", NULL, 0);
                        if (dts && !strcmp(dts->value, "1")) {
                            i_support_audio = 1;
                        }
                        else
                            i_support_audio = 0;
                    }
                    else
                        i_support_audio = 0;
                }
                else
#endif
#endif
                    if (acodec_id)
                        acodec_id[i_audio_tracks] = dec->codec_id;
                    i_audio_tracks ++;
                {
                    AVCodec *decoder;
                    decoder = avcodec_find_decoder(dec->codec_id);
                    if (decoder == 0)
                    {
                        i_support_audio = 0;
                    }
                }
			}
                break;
                
            case AVMEDIA_TYPE_VIDEO:
			{
				int width = dec->width;
				int height = dec->height;
				vcodec_id = dec->codec_id;
//                if ((dec->codec_id == CODEC_ID_H264 || dec->codec_id == CODEC_ID_MPEG4) && height >= 576)
                if (dec->codec_id == CODEC_ID_H264 && height >= 576)
                {
                    i_support_video = 1;
                    LOGI("just copy video codec\n");
                }
			}
                break;
                
            default:
                break;
		}
	}
    
    if (i_support_audio > 0 && i_support_video > 0 && i_support_format > 0)
    {
        ret = 2;//mpegts
    
//        if (vcodec_id == CODEC_ID_H264 || vcodec_id == CODEC_ID_MPEG4) {
//            int i_track = 0;
//            for (i_track = 0; i_track < i_audio_tracks; i_track++) {
//                if (acodec_id[i_track] == CODEC_ID_AAC)
//                    break;
//            }
//            
//            if (i_track < i_audio_tracks) {
//                if (pic->duration <= 10*60*AV_TIME_BASE)
//                    ret = 0;//3gp copy
//                else
//                    ret = 1;//3gp fragment
//            }
//            else {
//                if (vcodec_id == CODEC_ID_MPEG4)
//                    ret = 1;//3gp fragment
//            }
//        }
    }
    
fail:
    if (pic)
    {
        av_close_input_file(pic);
//        if (pic->pb)
//            avio_close(pic->pb);
//        avformat_free_context(pic);
    }
    if (acodec_id)
        av_free(acodec_id);
	
	avformat_network_deinit();
    
    return ret;
}

static int trs_open_input_file(AVFormatContext** ppic, const char* filename, AVInputFormat* iformat)
{
    *ppic = avformat_alloc_context();
    (*ppic)->interrupt_callback.callback = decode_interrupt_cb;
    (*ppic)->interrupt_callback.opaque = 0;    
    return avformat_open_input(ppic, filename, iformat, 0);
}

static void choose_sample_rate(AVStream *st, AVCodec *codec)
{
    if (codec && codec->supported_samplerates) {
        const int *p  = codec->supported_samplerates;
        int best      = 0;
        int best_dist = INT_MAX;
        for (; *p; p++) {
            int dist = abs(st->codec->sample_rate - *p);
            if (dist < best_dist) {
                best_dist = dist;
                best      = *p;
            }
        }
        if (best_dist) {
            LOGD("Requested sampling rate unsupported using closest supported (%d)\n", best);
        }
        st->codec->sample_rate = best;
    }
}

static void choose_sample_fmt(AVStream *st, AVCodec *codec)
{
    if (codec && codec->sample_fmts) {
        const enum AVSampleFormat *p = codec->sample_fmts;
        for (; *p != -1; p++) {
            if (*p == st->codec->sample_fmt)
                break;
        }
        if (*p == -1) {
            if((codec->capabilities & CODEC_CAP_LOSSLESS) && av_get_sample_fmt_name(st->codec->sample_fmt) > av_get_sample_fmt_name(codec->sample_fmts[0]))
                av_log(NULL, AV_LOG_ERROR, "Conversion will not be lossless.\n");
            if(av_get_sample_fmt_name(st->codec->sample_fmt))
            LOGD("Incompatible sample format '%s' for codec '%s', auto-selecting format '%s'\n",
                   av_get_sample_fmt_name(st->codec->sample_fmt),
                   codec->name,
                   av_get_sample_fmt_name(codec->sample_fmts[0]));
            st->codec->sample_fmt = codec->sample_fmts[0];
        }
    }
}

static void choose_pixel_fmt(AVStream *st, AVCodec *codec)
{
    if (codec && codec->pix_fmts) {
        const enum PixelFormat *p = codec->pix_fmts;
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(st->codec->pix_fmt);
        int has_alpha= desc ? desc->nb_components % 2 == 0 : 0;
        enum PixelFormat best= PIX_FMT_NONE;
        if (st->codec->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL) {
            if (st->codec->codec_id == CODEC_ID_MJPEG) {
				const enum PixelFormat p1[5] = { PIX_FMT_YUVJ420P, PIX_FMT_YUVJ422P, PIX_FMT_YUV420P, PIX_FMT_YUV422P, PIX_FMT_NONE };
                p = p1;
            } else if (st->codec->codec_id == CODEC_ID_LJPEG) {
				const enum PixelFormat p1[8] = { PIX_FMT_YUVJ420P, PIX_FMT_YUVJ422P, PIX_FMT_YUVJ444P, PIX_FMT_YUV420P,
                                                 PIX_FMT_YUV422P, PIX_FMT_YUV444P, PIX_FMT_BGRA, PIX_FMT_NONE };
                p = p1;
            }
        }
        for (; *p != PIX_FMT_NONE; p++) {
            best= avcodec_find_best_pix_fmt2(best, *p, st->codec->pix_fmt, has_alpha, NULL);
            if (*p == st->codec->pix_fmt)
                break;
        }
        if (*p == PIX_FMT_NONE) {
			st->codec->pix_fmt = best;
        }
    }
}

/*
 * 根据输入流打开一个输出流，根据需要，判断是否转码或者Codec Copy
 * oc:				AVFormatContext，输入文件的context
 * p_input_st:		AVStream，输入流
 * st_index:		输出流的media track index
 * aac_encoder:		aac encoder
 * out_format:		输出文件格式，默认mpegts
 */
static AVStream *add_input_stream(AVFormatContext *oc, AVStream *p_input_st, int32_t st_index, int aac_encoder, const char* out_format)
{
	AVStream *p_output_st = av_new_stream(oc, st_index);
	if (!p_output_st)
		return 0;

	bool b_audio_trans = true;
	bool b_video_trans = true;

	AVCodecContext	*dec = p_input_st->codec;
	AVCodecContext	*enc = 0;

	switch(dec->codec_type)
	{
        case AVMEDIA_TYPE_AUDIO:
			{
                if (!strcasecmp(out_format, "mpegts")) {
                    if (dec->codec_id == CODEC_ID_AAC || dec->codec_id == CODEC_ID_MP3 
						|| dec->codec_id == CODEC_ID_AC3 || dec->codec_id == CODEC_ID_MP2)//如果是AAC、AC-3、MP3、MP2则copy
                        b_audio_trans = false;
                }
                else if (!strcasecmp(out_format, "3gp") || !strcasecmp(out_format, "ipod")) {
                    if (dec->codec_id == CODEC_ID_AAC)
                        b_audio_trans = false;
                }

				//aac
				if (b_audio_trans)
				{
					AVCodec *encoder;
                    encoder = avcodec_find_encoder_by_name("aac");
                    p_output_st->codec->codec_id = encoder->id;
					avcodec_get_context_defaults3(p_output_st->codec, encoder);
                    p_output_st->codec->codec_type = dec->codec_type;

                    if (oc->flags & AVFMT_GLOBALHEADER)
                        p_output_st->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
                    
                    if (encoder->capabilities & CODEC_CAP_EXPERIMENTAL)
                        p_output_st->codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

					if (!p_output_st->codec->sample_rate)
						p_output_st->codec->sample_rate = dec->sample_rate;
					if (p_output_st->codec->sample_rate < 44100)
						p_output_st->codec->sample_rate = 44100;
					choose_sample_rate(p_output_st, encoder);

#if defined( _MSC_VER )
					p_output_st->codec->time_base.num = 1;
					p_output_st->codec->time_base.den = p_output_st->codec->sample_rate;
#else
					p_output_st->codec->time_base = (AVRational){1, p_output_st->codec->sample_rate};
#endif
					if (p_output_st->codec->sample_fmt == AV_SAMPLE_FMT_NONE)
						p_output_st->codec->sample_fmt = dec->sample_fmt;
					choose_sample_fmt(p_output_st, encoder);

					if (!p_output_st->codec->channels)
					{
						p_output_st->codec->channel_layout = dec->channel_layout;
                        p_output_st->codec->channels = dec->channels;
						if (p_output_st->codec->channels < 2) {
							p_output_st->codec->channels = 2;
							p_output_st->codec->channel_layout = 3;
						}
					}
					if (av_get_channel_layout_nb_channels(p_output_st->codec->channel_layout) != p_output_st->codec->channels)
						p_output_st->codec->channel_layout = 0;


                    if (aac_encoder == 1 || aac_encoder == 2)
                    {
                        AVDictionary* opts = NULL;
                        if (aac_encoder == 1)
                            av_dict_set(&opts, "aac_coder", "1", 0);
                        else
                            av_dict_set(&opts, "aac_coder", "2", 0);
                        avcodec_open2(p_output_st->codec, encoder, &opts);
                        av_dict_free(&opts);
                    }
                    else
                        avcodec_open2(p_output_st->codec, encoder, NULL);

					AVCodec *decoder;
					decoder = avcodec_find_decoder(dec->codec_id);
                    if (decoder != 0)
                        avcodec_open2(p_input_st->codec, decoder, NULL);
                    else
                        LOGE("----- no audio decoder.\n");
				}
				else
				{
					avcodec_get_context_defaults3(p_output_st->codec, dec->codec);
					enc = p_output_st->codec;

					enc->codec_id = dec->codec_id;
					enc->codec_type = dec->codec_type;
                    if (!enc->codec_tag)
                    {
                        if (!oc->oformat->codec_tag 
                            || av_codec_get_id(oc->oformat->codec_tag, dec->codec_id) == enc->codec_id 
                            || av_codec_get_tag(oc->oformat->codec_tag, dec->codec_id) <= 0)
                        enc->codec_tag = dec->codec_tag;
                    }
                    enc->bit_rate = dec->bit_rate;
                    enc->rc_max_rate = dec->rc_max_rate;
                    enc->rc_buffer_size = dec->rc_buffer_size;
                    enc->field_order = dec->field_order;

					uint64_t extra_size = dec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE;
                    enc->extradata = (uint8_t*)av_mallocz(extra_size);
                    memcpy(enc->extradata, dec->extradata, dec->extradata_size);
					enc->extradata_size = dec->extradata_size;
//					enc->time_base = dec->time_base;
                    enc->time_base = p_input_st->time_base;
                    if (av_q2d(dec->time_base)*dec->ticks_per_frame > av_q2d(p_input_st->time_base) 
                        && av_q2d(p_input_st->time_base) < 1.0 / 500)
                    {
                        enc->time_base = dec->time_base;
                        enc->time_base.num *= dec->ticks_per_frame;
                    }
                    av_reduce(&enc->time_base.num, &enc->time_base.den, enc->time_base.num, enc->time_base.den, INT_MAX);
                    enc->bits_per_coded_sample = dec->bits_per_coded_sample;
                    
					enc->channel_layout = dec->channel_layout;
					enc->sample_rate = dec->sample_rate;
					enc->channels = dec->channels;
                    enc->frame_size = dec->frame_size;
					enc->sample_fmt = dec->sample_fmt;
                    enc->audio_service_type = dec->audio_service_type;
                    enc->block_align = dec->block_align;
                }
			}
			break;

        case AVMEDIA_TYPE_VIDEO:
			{
                if (!strcasecmp(out_format, "mpegts")) {
                    if (dec->codec_id == CODEC_ID_H264 || dec->codec_id == CODEC_ID_MPEG2VIDEO)//如果是H.264或者Mpeg2Video则Copy
                        b_video_trans = false;
                }
                else if (!strcasecmp(out_format, "3gp") || !strcasecmp(out_format, "ipod")) {
                    if (dec->codec_id == CODEC_ID_H264)
                        b_video_trans = false;
                }

				if (b_video_trans == true)
				{
					AVCodec *encoder;
                    //encoder = avcodec_find_encoder_by_name("mpeg2video");
					//encoder = avcodec_find_encoder_by_name("libx264");
					//if (!encoder)
						encoder = avcodec_find_encoder_by_name("mpeg2video");

                    p_output_st->codec->codec_id = encoder->id;
					avcodec_get_context_defaults3(p_output_st->codec, encoder);
                    p_output_st->codec->codec_type = dec->codec_type;

					p_output_st->codec->flags |= CODEC_FLAG_QSCALE;		//***image quality

                    if (oc->flags & AVFMT_GLOBALHEADER)
                        p_output_st->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

					if (p_output_st->codec->pix_fmt == PIX_FMT_NONE)
						p_output_st->codec->pix_fmt = dec->pix_fmt;
					choose_pixel_fmt(p_output_st, encoder);

					if (p_output_st->codec->pix_fmt == PIX_FMT_NONE)
						LOGD("can not find pix_fmt ...\n");

					if (!p_output_st->codec->width || !p_output_st->codec->height)
					{
						p_output_st->codec->width = dec->width;
						p_output_st->codec->height = dec->height;
					}

					AVRational frame_rate;
					frame_rate.num = 1; frame_rate.den = 25;

					if (p_input_st->r_frame_rate.num > 0)
						frame_rate = p_input_st->r_frame_rate;
					if (encoder && encoder->supported_framerates)
					{
						int idx = av_find_nearest_q_idx(frame_rate, encoder->supported_framerates);
						frame_rate = encoder->supported_framerates[idx];
					}
					p_output_st->codec->time_base.num = frame_rate.den;
					p_output_st->codec->time_base.den = frame_rate.num;

					p_output_st->codec->bits_per_raw_sample = 0;

					AVRational sar;
					sar.num = 0;
					sar.den = 1;
					if (p_output_st->sample_aspect_ratio.num)
						sar = p_input_st->sample_aspect_ratio;
					else
						sar = p_input_st->codec->sample_aspect_ratio;
					p_output_st->sample_aspect_ratio = sar;
					p_output_st->codec->sample_aspect_ratio = sar;

					//p_output_st->codec->gop_size = 0;		//intra_only
					//p_output_st->codec->rc_override_count = 0;
					p_output_st->codec->intra_dc_precision = 0;


					avcodec_open2(p_output_st->codec, encoder, NULL);

					AVCodec *decoder;
					decoder = avcodec_find_decoder(dec->codec_id);
                    if (decoder != 0)
                        avcodec_open2(p_input_st->codec, decoder, NULL);
                    else
                        LOGE("----- no audio decoder.\n");
				}
				else
				{
					avcodec_get_context_defaults3(p_output_st->codec, dec->codec);
					enc = p_output_st->codec;

					//codec
					enc->codec_id = dec->codec_id;
					enc->codec_type = dec->codec_type;
					if (!enc->codec_tag)
					{
						if (!oc->oformat->codec_tag 
							|| av_codec_get_id(oc->oformat->codec_tag, dec->codec_id) == enc->codec_id 
							|| av_codec_get_tag(oc->oformat->codec_tag, dec->codec_id) <= 0)
							enc->codec_tag = dec->codec_tag;
					}
					enc->bit_rate = dec->bit_rate;
					enc->rc_max_rate = dec->rc_max_rate;
					enc->rc_buffer_size = dec->rc_buffer_size;
					enc->field_order = dec->field_order;

					uint64_t extra_size = dec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE;
					enc->extradata = (uint8_t*)av_mallocz(extra_size);
					memcpy(enc->extradata, dec->extradata, dec->extradata_size);
					enc->extradata_size = dec->extradata_size;
					//				enc->time_base = dec->time_base;
					enc->time_base = p_input_st->time_base;
					if (av_q2d(dec->time_base)*dec->ticks_per_frame > av_q2d(p_input_st->time_base) 
						&& av_q2d(p_input_st->time_base) < 1.0 / 500)
					{
						enc->time_base = dec->time_base;
						enc->time_base.num *= dec->ticks_per_frame;
					}
					av_reduce(&enc->time_base.num, &enc->time_base.den, enc->time_base.num, enc->time_base.den, INT_MAX);
					enc->bits_per_coded_sample = dec->bits_per_coded_sample;

					enc->pix_fmt = dec->pix_fmt;
					enc->width = dec->width;
					enc->height = dec->height;
					enc->has_b_frames = dec->has_b_frames;
					//				enc->sample_aspect_ratio = dec->sample_aspect_ratio;
					if (!enc->sample_aspect_ratio.num)
					{
#if defined( _MSC_VER )
						AVRational ratio;
						ratio.num = 0;
						ratio.den = 1;
						enc->sample_aspect_ratio = 
							p_output_st->sample_aspect_ratio =
							p_input_st->sample_aspect_ratio.num ? p_input_st->sample_aspect_ratio :
							dec->sample_aspect_ratio.num ? dec->sample_aspect_ratio : ratio;
#else
						enc->sample_aspect_ratio = 
							p_output_st->sample_aspect_ratio =
							p_input_st->sample_aspect_ratio.num ? p_input_st->sample_aspect_ratio :
							dec->sample_aspect_ratio.num ? dec->sample_aspect_ratio : (AVRational){0,1};
#endif
					}

					//stream
					p_output_st->avg_frame_rate = p_input_st->avg_frame_rate;
				}
			}
			break;

		default:
			break;
	}
	return p_output_st;
}

/*
 *	打开输出片段文件,并写入header信息
 *	p_input,p_output输入输出的context
 */
int open_output_file(trs_input* p_input, trs_output* p_output, trs_output_list* p_list, int32_t i_index, const char *format) 
{
    int ret = 0;
    int i = 0;
    int err = 0;
    p_output->err_code = 0;
	if (!p_input || !p_output || !p_list || !format || !*format)
        return -1;
	err = avio_open(&p_output->oc->pb, p_list[i_index].p_filename, AVIO_FLAG_WRITE);
    
	if (err < 0)
	{
        LOGI("Could not open output file\n");
        p_output->err_code = err;
		ret = -1;
		goto end;
	}
    
    av_dict_copy(&p_output->oc->metadata, p_input->ic->metadata, AV_DICT_DONT_OVERWRITE);
#ifdef TRS_COPYCHAPTERS
    for (i = 0; i < p_input->ic->nb_chapters; i ++)
    {
        AVChapter *in_ch = p_input->ic->chapters[i], *out_ch;
        
        out_ch = (AVChapter*)av_mallocz(sizeof(AVChapter));
        if (out_ch)
        {
            out_ch->id = in_ch->id;
            out_ch->time_base = in_ch->time_base;
            out_ch->start = in_ch->start;
            out_ch->end = in_ch->end;
            av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);
            p_output->oc->nb_chapters++;
            p_output->oc->chapters = (AVChapter**)av_realloc(p_output->oc->chapters, sizeof(AVChapter)*p_output->oc->nb_chapters);
            if (p_output->oc->chapters)
            {
                p_output->oc->chapters[p_output->oc->nb_chapters-1] = out_ch;
            }
        }
    }
#endif
    
    err = avformat_write_header(p_output->oc, 0);
	if (err < 0)
	{
		LOGI("Could not write header for output file (incorrect codec parameters ?)\n");
		ret = AVERROR(EINVAL);
        p_output->err_code = err;
		goto end;
	}
    
end:
    return ret;
}

/*
 * 打开输出文件的context，并根据需要增加相应的流，打开bitstream filter
 */
static int trs_open_output(trs_input* p_input, trs_output* p_output, trs_output_list* p_list, int32_t i_index, const char *format)
{
    int ret = 0;
    int i = 0;
    int err = 0;
    p_output->err_code = 0;
	if (!p_input || !p_output || !p_list || !format || !*format)
        return -1;
    
    p_output->oc = avformat_alloc_context();
    if (!p_output->oc) 
	{
        LOGI("Could not allocated output context\n");
		ret = -1;
        p_output->err_code = -1;
        goto end;
    }
    
	p_output->p_ofmt = av_guess_format(format, NULL, NULL);
	if (!p_output->p_ofmt)
	{
		ret = -1;
        p_output->err_code = -1;
		goto end;
	}
    
    p_output->oc->oformat = p_output->p_ofmt;
    strcpy(p_output->oc->filename, p_list[i_index].p_filename);
    if (p_output->oc->oformat->priv_data_size > 0) {
        p_output->oc->priv_data = av_mallocz(p_output->oc->oformat->priv_data_size);
        if (p_output->oc->oformat->priv_class) {
            *(const AVClass**)p_output->oc->priv_data= p_output->oc->oformat->priv_class;
            av_opt_set_defaults(p_output->oc->priv_data);
        }
    } else
        p_output->oc->priv_data = NULL;

	if (p_input->i_video_st >= 0)
	{
		AVStream *st = p_input->ic->streams[p_input->i_video_st];
		if (st->discard < AVDISCARD_ALL) {
			p_output->p_video = add_input_stream(p_output->oc, st, 0, 0, p_output->psz_format);		//打开输出流
			if (!strcasecmp(format, "mpegts") && p_output->p_video->codec->codec_id == CODEC_ID_H264 && p_input->p_video->codec->codec_id == CODEC_ID_H264
				&& strcasecmp(p_input->ic->iformat->name, "mpegts"))
				p_output->p_video_bitstreamfilter = av_bitstream_filter_init("h264_mp4toannexb");
			av_dict_copy(&p_output->p_video->metadata, p_input->p_video->metadata, AV_DICT_DONT_OVERWRITE);
		}

		//p_input->b_video_decoder_support = true;
		//if (p_input->b_need_video_dec)
		//{
		//		AVCodec *decoder;
		//		decoder = avcodec_find_decoder(st->codec->codec_id);
		//		if (decoder == 0)
		//		{
		//			p_input->b_video_decoder_support = false;
		//		}
		//}
	}

	for (int i = 0; i < p_input->i_audio_streams; i ++) 
	{
		trs_input_audio *p_input_audio = p_input->p_audio_input[i];

		if (p_input_audio->i_audio_st >= 0)
		{
			AVStream *st = p_input->ic->streams[p_input_audio->i_audio_st];
			if (st->discard < AVDISCARD_ALL) {
				p_output->p_output_audio[i]->i_audio_stream_index = i+1;
				p_output->p_output_audio[i]->p_audio = add_input_stream(p_output->oc, st, i+1, p_output->p_output_audio[i]->i_aac_encoder, p_output->psz_format); //打开输出流

				if (!strcasecmp(format, "3gp") && p_output->p_output_audio[i]->p_audio->codec->codec_id == CODEC_ID_AAC)
					p_output->p_output_audio[i]->p_audio_bitstreamfilter = av_bitstream_filter_init("aac_adtstoasc");

				av_dict_copy(&p_output->p_output_audio[i]->p_audio->metadata, p_input_audio->p_audio->metadata, AV_DICT_DONT_OVERWRITE);
			}

			//p_input_audio->b_decoder_support = true;
			//if (p_input_audio->b_need_audio_dec)
			//{
			//	AVCodec *decoder;
			//	decoder = avcodec_find_decoder(st->codec->codec_id);
			//	if (decoder == 0)
			//	{
			//		p_input_audio->b_decoder_support = false;
			//		p_input->i_audio_stream_supported --;
			//	}
			//}
		}
	}

#if 0
	err = avio_open(&p_output->oc->pb, p_list[i_index].p_filename, AVIO_FLAG_WRITE);
    
	if (err < 0)
	{
        LOGI("Could not open output file\n");
        p_output->err_code = err;
		ret = -1;
		goto end;
	}
    
    av_dict_copy(&p_output->oc->metadata, p_input->ic->metadata, AV_DICT_DONT_OVERWRITE);
#ifdef TRS_COPYCHAPTERS
    for (i = 0; i < p_input->ic->nb_chapters; i ++)
    {
        AVChapter *in_ch = p_input->ic->chapters[i], *out_ch;
        
        out_ch = (AVChapter*)av_mallocz(sizeof(AVChapter));
        if (out_ch)
        {
            out_ch->id = in_ch->id;
            out_ch->time_base = in_ch->time_base;
            out_ch->start = in_ch->start;
            out_ch->end = in_ch->end;
            av_metadata_copy(&out_ch->metadata, in_ch->metadata, 0);
            p_output->oc->nb_chapters++;
            p_output->oc->chapters = (AVChapter**)av_realloc(p_output->oc->chapters, sizeof(AVChapter)*p_output->oc->nb_chapters);
            if (p_output->oc->chapters)
            {
                p_output->oc->chapters[p_output->oc->nb_chapters-1] = out_ch;
            }
        }
    }
#endif
    
    err = avformat_write_header(p_output->oc, 0);
	if (err < 0)
	{
		LOGI("Could not write header for output file (incorrect codec parameters ?)");
		ret = AVERROR(EINVAL);
        p_output->err_code = err;
		goto end;
	}
#endif
    
end:
    return ret;
}

/*
 * 关闭输出文件
 */
int close_output_file(trs_output* p_output) 
{
    int ret = 0;
    if (!p_output)
        return -1;
	if (p_output->oc) {
		if (p_output->err_code >= 0)
            av_write_trailer(p_output->oc);
        if (p_output->oc->pb) {
            avio_flush(p_output->oc->pb);
            avio_close(p_output->oc->pb);
        }
	}
	return ret;
}

static int trs_flush_output(trs_output* p_output)
{
	int ret = 0;
    if (!p_output || !p_output->oc)
        return -1;

	for (int i = 0; i < p_output->i_audio_streams; i ++) {
		if (p_output->p_output_audio[i]->b_need_audio_enc == true && p_output->p_output_audio[i]->p_audio && p_output->p_output_audio[i]->p_audio->codec->frame_size>1) {
			AVCodecContext *enc = p_output->p_output_audio[i]->p_audio->codec;
			for (;;) {
				AVPacket pkt;
				int got_packet;
				av_init_packet(&pkt);
				pkt.data = NULL;
				pkt.size = 0;
				ret = avcodec_encode_video2(enc, &pkt, NULL, &got_packet);
				if (ret < 0) {
					break;
				}
				if (!got_packet) {
					break;
				}
				if (pkt.pts != AV_NOPTS_VALUE)
					pkt.pts = av_rescale_q(pkt.pts, enc->time_base, p_output->p_output_audio[i]->p_audio->time_base);
				if (pkt.dts != AV_NOPTS_VALUE)
					pkt.dts = av_rescale_q(pkt.dts, enc->time_base, p_output->p_output_audio[i]->p_audio->time_base);
				if (pkt.duration > 0)
					pkt.duration = av_rescale_q(pkt.duration, enc->time_base, p_output->p_output_audio[i]->p_audio->time_base);
				write_frame(0, p_output->oc, &pkt, enc, p_output->p_output_audio[i]->p_audio_bitstreamfilter);
			}		
		}
	}

	if (p_output->p_video && p_output->b_need_video_enc == true) {
		AVCodecContext *enc = p_output->p_video->codec;
		if ((p_output->oc->oformat->flags&AVFMT_RAWPICTURE) && enc->codec->id == AV_CODEC_ID_RAWVIDEO)
			return ret;

		for (;;) {
			AVPacket pkt;
			int got_packet;
			av_init_packet(&pkt);
			pkt.data = NULL;
			pkt.size = 0;
			ret = avcodec_encode_video2(enc, &pkt, NULL, &got_packet);
			if (ret < 0) {
				break;
			}
			if (!got_packet) {
				break;
			}
			if (pkt.pts != AV_NOPTS_VALUE)
				pkt.pts = av_rescale_q(pkt.pts, enc->time_base, p_output->p_video->time_base);
			if (pkt.dts != AV_NOPTS_VALUE)
				pkt.dts = av_rescale_q(pkt.dts, enc->time_base, p_output->p_video->time_base);
			if (pkt.duration > 0)
				pkt.duration = av_rescale_q(pkt.duration, enc->time_base, p_output->p_video->time_base);
			write_frame(0, p_output->oc, &pkt, enc, p_output->p_video_bitstreamfilter);
		}
	}

	return ret;
}

/*
 * 关闭输出文件的context，及codec等
 */
static int trs_close_output(trs_output* p_output)
{
    int ret = 0;
    if (!p_output)
        return -1;
    
    if (p_output->oc)
    {
		if (p_output->b_need_video_enc == true)
		{
			avcodec_close(p_output->p_video->codec);
		}
		
        if (p_output->p_video->codec->extradata)
            av_freep(&p_output->p_video->codec->extradata);

		for (int i = 0; i < p_output->i_audio_streams; i ++) 
		{
			if (p_output->p_output_audio[i]->p_audio && p_output->p_output_audio[i]->b_need_audio_enc == true)
				avcodec_close(p_output->p_output_audio[i]->p_audio->codec);

			if (p_output->p_output_audio[i]->p_audio && p_output->p_output_audio[i]->p_audio->codec->extradata && !p_output->p_output_audio[i]->b_need_audio_enc)
				av_freep(&p_output->p_output_audio[i]->p_audio->codec->extradata);
		}
        
        if (p_output->p_video_bitstreamfilter != NULL)
            av_bitstream_filter_close(p_output->p_video_bitstreamfilter);
		for (int i = 0; i < p_output->i_audio_streams; i ++) 
		{
			if (p_output->p_output_audio[i]->p_audio_bitstreamfilter != NULL)
				av_bitstream_filter_close(p_output->p_output_audio[i]->p_audio_bitstreamfilter);
		}

#if 0
		if (p_output->err_code >= 0)
            av_write_trailer(p_output->oc);
        if (p_output->oc->pb)
        {
            avio_flush(p_output->oc->pb);
            avio_close(p_output->oc->pb);
        }
#endif
        avformat_free_context(p_output->oc);
    }
    
	for (int i = 0; i < p_output->i_audio_streams; i ++) 
	{
		p_output->p_output_audio[i]->p_audio = 0;
		p_output->p_output_audio[i]->p_audio_enc = 0;
		p_output->p_output_audio[i]->p_audio_bitstreamfilter = 0;
	}
    p_output->p_video = 0;
    p_output->p_video_bitstreamfilter = 0;

	p_output->oc = 0;
    p_output->p_ofmt = 0;
    return ret;
}


/*
 * 根据文件的keyframe创建m3u8文件，以便切片文件都在keyframe上
 */
static bool createM3u8(trs_input* p_input, const char* filename, const char* local_host, float duration, int32_t i_segments, float total_duration, trs_output_list* p_list)
{
    if (!filename || !p_input)
        return false;
    char *host = 0;
    if (local_host)
    {
        host = strdup(local_host);
        char* snash = (char*)strrchr(host, '/');
        if (snash && *snash)
        {
            snash ++;
            *snash = '\0';
        }
    }
    
	FILE* fd = fopen (filename, "wb");
	if (fd == NULL) {
		LOGE("----- open file: %s ERROR\n", filename);
		return false;
	}
    
	char line[1024] ={0};
	int32_t i_writed = 0;
    
    int i_duration = (int)duration;
    snprintf(line, 1024, "#EXTM3U\r\n#EXT-X-TARGETDURATION:%d\r\n#EXT-X-VERSION:3\r\n#EXT-X-MEDIA-SEQUENCE: 0\r\n", i_duration + 1);
	i_writed = fwrite(line, sizeof(char), strlen(line), fd);
	if (i_writed == -1) {
		LOGE("----- write file error with errno=%d\n", ferror(fd));
	}

    int64_t pre_timestamp = 0;
    int64_t cur_timestamp = duration * AV_TIME_BASE;
    int32_t i_index = -1;
    int32_t i_nb_entries = 3;
    int32_t i_using_default_duration = 0;
    
    for (int i = 0; i < i_segments && pre_timestamp < total_duration * AV_TIME_BASE; i ++) {
        float f_duration = duration;
        if (cur_timestamp >= total_duration * AV_TIME_BASE) {
#if defined( _MSC_VER )
            AVRational base_q;
            base_q.num = 1;
            base_q.den = AV_TIME_BASE;
            f_duration = total_duration-pre_timestamp*av_q2d(base_q);
#else
            f_duration = total_duration-pre_timestamp*av_q2d(AV_TIME_BASE_Q);
#endif
            snprintf(line, 1024, "#EXTINF:%.2f,\r\n", f_duration);
        }
        else {
            AVStream *st = p_input->p_video;

            int64_t seek_target = cur_timestamp;
            int64_t seek_min = 0;
            int64_t seek_max = INT64_MAX;
            seek_target = av_rescale(seek_target, st->time_base.den, AV_TIME_BASE * (int64_t)st->time_base.num);
            int ret = avformat_seek_file(p_input->ic, p_input->i_video_st, seek_min, seek_target, seek_max, AVSEEK_FLAG_ANY);
            
            i_index = av_index_search_timestamp(st, seek_target, AVSEEK_FLAG_ANY);		//seek to keyframe
			if (i_index > st->nb_index_entries)
				i_index = -1;

            i_nb_entries ++;
            if (i_using_default_duration == 0 && st->nb_index_entries < i_nb_entries)
                i_using_default_duration = 1;
            
            if (i_index < 0 || i_using_default_duration == 1) {
#if defined( _MSC_VER )
				AVRational base_q;
				base_q.num = 1;
				base_q.den = AV_TIME_BASE;
				f_duration = (cur_timestamp - pre_timestamp)*av_q2d(base_q);
#else 
				f_duration = (cur_timestamp - pre_timestamp)*av_q2d(AV_TIME_BASE_Q);
#endif
            }
            else {
                int64_t ii_timestamp = st->index_entries[i_index].timestamp;
#if defined( _MSC_VER )
				AVRational base_q;
				base_q.num = 1;
				base_q.den = AV_TIME_BASE;
				f_duration = ii_timestamp*av_q2d(st->time_base) - pre_timestamp*av_q2d(base_q);
#else 
				f_duration = ii_timestamp*av_q2d(st->time_base) - pre_timestamp*av_q2d(AV_TIME_BASE_Q);
#endif
                int ii_temp = f_duration * 100;
                f_duration = float(ii_temp) / 100.0;
                cur_timestamp = pre_timestamp + f_duration * AV_TIME_BASE;
//                cur_timestamp = ii_timestamp*AV_TIME_BASE*av_q2d(st->time_base);
            }
            snprintf(line, 1024, "#EXTINF:%.2f,\r\n", f_duration);
        }
		i_writed = fwrite(line, sizeof(char), strlen(line), fd);
		if (i_writed == -1) {
			LOGE("----- write file error with errno=%d\n", ferror(fd));
		}
		
		if (host)
			snprintf(line, 1024, "%s%d.ts\r\n", host, i);
		else
			snprintf(line, 1024, "trs_m4v_%d.ts\r\n", i);

		i_writed = fwrite(line, sizeof(char), strlen(line), fd);
		if (i_writed == -1) {
			LOGE("----- write file error with errno=%d\n", ferror(fd));
		}
	        
        if (p_list) {
#if defined( _MSC_VER )
			AVRational base_q;
			base_q.num = 1;
			base_q.den = AV_TIME_BASE;
			p_list[i].f_start_time = pre_timestamp * av_q2d(base_q);
#else 
			p_list[i].f_start_time = pre_timestamp * av_q2d(AV_TIME_BASE_Q);
#endif
            p_list[i].f_duration = f_duration;
            p_list[i].f_duration_m3u8 = f_duration;
        }

        pre_timestamp = cur_timestamp;
        cur_timestamp += i_duration * AV_TIME_BASE;
    }
    
    strcpy(line, "#EXT-X-ENDLIST\r\n");
	i_writed = fwrite(line, sizeof(char), strlen(line), fd);
	if (i_writed == -1) {
		LOGE("----- write file error with errno=%d\n", ferror(fd));
	}
		    
    fclose(fd);
    free(host);
    
    avformat_seek_file(p_input->ic, p_input->i_video_st, 0, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    if (p_input->ic->pb)
        p_input->ic->pb->eof_reached = 0;
    return true;
}

/*
 * 根据已经切片后的片段文件信息创建m3u8文件
 */
static bool createM3u8ByList(const char* filename, const char* local_host, float duration, int32_t i_segments, float total_duration, trs_output_list* p_list)
{
    if (!filename)
        return false;
    char *host = 0;
    if (local_host)
    {
        host = strdup(local_host);
        char* snash = (char*)strrchr(host, '/');
        if (snash && *snash)
        {
            snash ++;
            *snash = '\0';
        }
    }
    
	FILE* fd = fopen (filename, "wb");
	if (fd == NULL) {
		LOGE("----- open file: %s ERROR\n", filename);
		return false;
	}
    
	char line[1024] ={0};
	int32_t i_writed = 0;
    
	float f_max = 0.0;
	for (int i = 0; i < i_segments; i ++)
		if (f_max < p_list[i].f_duration)
			f_max = p_list[i].f_duration;
	int ii_max = (int)(f_max+1);

    int i_duration = (int)duration;
    snprintf(line, 1024, "#EXTM3U\r\n#EXT-X-TARGETDURATION:%d\r\n#EXT-X-VERSION:3\r\n#EXT-X-MEDIA-SEQUENCE: 0\r\n", ii_max);
	i_writed = fwrite(line, sizeof(char), strlen(line), fd);
	if (i_writed == -1) {
		LOGE("----- write file error with errno=%d\n", ferror(fd));
	}
		
    int64_t pre_timestamp = 0;
    int64_t cur_timestamp = duration * AV_TIME_BASE;
    int32_t i_index = -1;
    int32_t i_nb_entries = 3;
    int32_t i_using_default_duration = 0;

	for (int i = 0; i < i_segments && pre_timestamp < total_duration * AV_TIME_BASE; i ++) {
		if (cur_timestamp >= total_duration * AV_TIME_BASE) {
#if defined( _MSC_VER )
			AVRational base_q;
			base_q.num = 1;
			base_q.den = AV_TIME_BASE;
			float f_duration = total_duration-pre_timestamp*av_q2d(base_q);
#else
			float f_duration = total_duration-pre_timestamp*av_q2d(AV_TIME_BASE_Q);
#endif
			snprintf(line, 1024, "#EXTINF:%.2f,\r\n", f_duration);
		}
		else {
			float f_duration = p_list[i].f_duration;
			cur_timestamp = pre_timestamp + f_duration * AV_TIME_BASE;
			snprintf(line, 1024, "#EXTINF:%.2f,\r\n", f_duration);
		}
		i_writed = fwrite(line, sizeof(char), strlen(line), fd);
		if (i_writed == -1) {
			LOGE("----- write file error with errno=%d\n", ferror(fd));
		}
		
		if (host)
			snprintf(line, 1024, "%s%d.ts\r\n", host, i);
		else
			snprintf(line, 1024, "trs_m4v_%d.ts\r\n", i);

		i_writed = fwrite(line, sizeof(char), strlen(line), fd);
		if (i_writed == -1) {
			LOGE("----- write file error with errno=%d\n", ferror(fd));
		}
		
        pre_timestamp = cur_timestamp;
        cur_timestamp += i_duration * AV_TIME_BASE;
    }
    
    strcpy(line, "#EXT-X-ENDLIST\r\n");
	i_writed = fwrite(line, sizeof(char), strlen(line), fd);
   	if (i_writed == -1) {
		LOGE("----- write file error with errno=%d\n", ferror(fd));
	}
		 
    fclose(fd);
    free(host);
    return true;
}

static int trs_seek_internal(trs_input	*p_input, int64_t pos, int64_t rel, int seek_flags)
{
	int64_t seek_target = pos;
	int64_t seek_min = 0;
    //	int64_t seek_min = rel > 0 ? seek_target - rel + 2 : INT64_MIN;
	int64_t seek_max = rel < 0 ? seek_target - rel - 2 : INT64_MAX;
    
#if 0
	int seek_flags = AVSEEK_FLAG_BACKWARD;
    if (seek_by_bytes)
        seek_flags |= AVSEEK_FLAG_BYTE;
#else
    int seek_by_bytes = 0;
    if (seek_flags & AVSEEK_FLAG_BYTE)
        seek_by_bytes = 1;
#endif
    
	//using working stream index to seek
	int stream_index;
	if (p_input->i_video_st >= 0)
		stream_index = p_input->i_video_st;
	else if (p_input->i_audio_streams > 0)
		stream_index = p_input->p_audio_input[0]->i_audio_st;
    
	AVFormatContext *ic = p_input->ic;
	if (stream_index >= 0 && !seek_by_bytes)
	{
		AVStream *st = ic->streams[stream_index];
        
		//rescale timestamp to stream time_base
		//avformat_seek_file won't rescale if stream_index > -1, we do by ourself
		seek_target = av_rescale(seek_target, st->time_base.den, AV_TIME_BASE * (int64_t)st->time_base.num);
	}
    
	int ret = avformat_seek_file(ic, stream_index, seek_min, seek_target, seek_max, seek_flags);
    
	if (ret < 0)
	{
		LOGI("%s: could not seek keyframe, try Seek_Any\n",p_input->p_filename);
		seek_flags |= AVSEEK_FLAG_ANY;
        //		seek_target = pos;
		ret = avformat_seek_file(ic, stream_index, seek_min, seek_target, seek_max, seek_flags);
	}
    
	if (ret < 0)
	{
        //fprintf(stderr, "%s: could not seek to position %0.3f\n",
        //        is->filename, (double)timestamp / AV_TIME_BASE);
		LOGI("%s: could not seek to position %0.3f, try seek_byte\n",p_input->p_filename, (double)seek_target / AV_TIME_BASE);
        
		//reaching here means not try seek_byte yet
		seek_flags |= AVSEEK_FLAG_BYTE;
        seek_target = pos;
		if (ic->start_time != AV_NOPTS_VALUE)
		{
			seek_target -= ic->start_time;
			if (seek_target < 0)
				seek_target = 0;
		}
        
		if (ic->bit_rate > 0)
		{
			seek_target = (ic->bit_rate / 8) * (double)seek_target / AV_TIME_BASE;
		}
		else if (ic->duration > 0)
		{
            seek_target = avio_size(ic->pb) * seek_target / ic->duration;
		}
		else
		{
			seek_target = 0;
		}
        
		ret = avformat_seek_file(ic, stream_index, seek_min, seek_target, seek_max, seek_flags);
		if (ret < 0)
		{
			LOGI("%s: try seek_by_byte:%lld failed, could not seek at all!!",p_input->p_filename, seek_target);
			
			//reset file pos
			//avio_seek(ic->pb, timestamp, SEEK_SET);
		}
    }
    
	return ret;
}

//退出中断函数
static int decode_interrupt_cb(void* opaque)
{
	trs_state *p_trs = (trs_state *)anc_threadvar_get (tls_trsstate_key);
	return (p_trs && p_trs->abort);
    
}

static int32_t get_audio_tracks_count(AVFormatContext *ic)
{
	int32_t i_count = 0;
	if (!ic)
		return 0;

	for(int i=0; i<ic->nb_streams; i++)
	{
        AVStream *st = ic->streams[i];
        AVCodecContext *dec = st->codec;
        switch (dec->codec_type) 
		{
        case AVMEDIA_TYPE_AUDIO:
			{
				i_count ++;
			}
			break;

        case AVMEDIA_TYPE_VIDEO:
			{
			}
			break;

		default:
			break;
		}
	}
	return i_count;
}

//转码主线程函数
static void* trs_thread(void* param)
{
	trs_state	*p_trs = (trs_state*)param;
	if (!p_trs)
		return 0;

	trs_input* p_input = 0;
	trs_output* p_output = 0;
	trs_output_list	*p_list = 0;
    char *format = strdup("mp4");
    char *fileext = strdup("mp4");
    char *playlist_type = 0;
    char *local_host = 0;
	int eof = 0;
//	AVPacket pkt1, *pkt = &pkt1;
    //int64_t audio_seg_start = AV_NOPTS_VALUE;
	//int64_t seg_start = AV_NOPTS_VALUE;
    int64_t ii_first_pts = AV_NOPTS_VALUE;
	//int64_t seg_duration = 0;
    int64_t timestamp = 0;
    //float f_total_duration = 0.0;
    //float f_total_duration_m3u8 = 0.0;

	p_trs->ii_video_pre_pts = AV_NOPTS_VALUE;
	p_trs->audio_seg_start = AV_NOPTS_VALUE;
	p_trs->seg_start = AV_NOPTS_VALUE;
	p_trs->seg_duration = 0;
	p_trs->f_total_duration = 0.0;
	p_trs->f_total_duration_m3u8 = 0.0;
    
	int ret = 0;
	int err = 0;
	int i = 0;
    int i_ismpegts = 0;
    int i_waiting = 0;
	int i_input_audiostream_num = 0;

	p_input = (trs_input*)av_mallocz(sizeof(trs_input));
	if (!p_input)
		return 0;
	strcpy(p_input->p_filename, p_trs->p_filename);
	p_input->i_start_time = p_trs->f_start_time * AV_TIME_BASE;
	p_input->i_start_write = 0;

	p_output = (trs_output*)av_mallocz(sizeof(trs_output));
	if (!p_output)
		return 0;

	anc_threadvar_set (tls_trsstate_key, p_trs);
	avformat_network_init();
    
    p_output->psz_format = 0;
    p_output->ii_video_lastmuxdts = AV_NOPTS_VALUE;
	p_output->top_field_first = -1;
	p_output->i_video_bit_buffer_size = 0;
	p_output->p_video_bit_buffer = 0;

	p_trs->p_output = p_output;
	p_trs->p_input = p_input;

	p_trs->i_output_trsing = 0;
	err = trs_open_input_file(&p_input->ic, p_input->p_filename, p_input->ifmt);
	if (err < 0)
	{
		trs_print_error(p_input->p_filename, err);
		ret = -1;
		goto fail;
	}

	err = av_find_stream_info(p_input->ic);
	if (err < 0)
	{
		LOGI("%s: could not find codec parameters\n", p_input->p_filename);
		ret = -1;
		goto fail;
	}
    if (p_input->ic->pb)
        p_input->ic->pb->eof_reached = 0;

	p_input->f_duration = (double)((float)p_input->ic->duration / AV_TIME_BASE);//s
    if (p_trs->f_segmenter_duration <= 0)
    {
        p_trs->i_output_total = 1;
        p_trs->i_output_start = 0;
        p_trs->i_output_last = 1;        
    }
    else
    {
		if (p_trs->i_limit_type > 0) 
		{
			p_trs->i_output_total = p_trs->f_limit_duration / p_trs->f_segmenter_duration;
			if (p_trs->i_output_total * p_trs->f_segmenter_duration < p_trs->f_limit_duration)
				p_trs->i_output_total += 1;
		}
		else 
		{
			p_trs->i_output_total = p_input->f_duration / p_trs->f_segmenter_duration;
			if (p_trs->i_output_total * p_trs->f_segmenter_duration < p_input->f_duration)
				p_trs->i_output_total += 1;
		}

        p_trs->i_output_start = p_trs->f_start_time / p_trs->f_segmenter_duration;
//        if (p_trs->i_output_start * p_trs->f_segmenter_duration < p_trs->f_start_time)
//            p_trs->i_output_start += 1;
        p_trs->i_output_last = p_trs->i_output_start + 7;
    }
	
    p_list = (trs_output_list*)av_mallocz(sizeof(trs_output_list) * p_trs->i_output_total);
	if (!p_list)
    {
        ret = -1;
        goto fail;
    }
	p_trs->p_list = p_list;

	p_trs->i_limit_type = -1;
	p_trs->f_limit_duration = -1;
	//p_trs->f_limit_filesize = -1;

	//读取options中的参数
    if (p_trs->av_options) {
        AVDictionaryEntry *output_format = av_dict_get(p_trs->av_options, "output_format", NULL, 0);
        if (output_format && output_format->value) {
            if (format)
                free(format);
            format = strdup(output_format->value);
        }

        output_format = av_dict_get(p_trs->av_options, "output_ext", NULL, 0);
        if (output_format && output_format->value) {
            if (fileext)
                free(fileext);
            fileext = strdup(output_format->value);
        }

        output_format = av_dict_get(p_trs->av_options, "playlist_type", NULL, 0);
        if (output_format && output_format->value) {
            if (playlist_type)
                free(playlist_type);
            playlist_type = strdup(output_format->value);
        }
        
        output_format = av_dict_get(p_trs->av_options, "local_host", NULL, 0);
        if (output_format && output_format->value) {
            if (local_host)
                free(local_host);
            local_host = strdup(output_format->value);
        }

        output_format = av_dict_get(p_trs->av_options, "limit_duration", NULL, 0);
        if (output_format && output_format->value) {
            //if (local_host)
            //    free(local_host);
			p_trs->f_limit_duration = atof(output_format->value);
			p_trs->i_limit_type = 1;
        }

   //     output_format = av_dict_get(p_trs->av_options, "limit_size", NULL, 0);
   //     if (output_format && output_format->value) {
   //         if (local_host)
   //             free(local_host);
			//p_trs->f_limit_filesize = atof(output_format->value);
			//p_trs->i_limit_type = 2;
   //     }
	}
    p_output->psz_format = strdup(format);
    
    for (i=0; i < p_trs->i_output_total; i++)
    {
        char myname[128] = "";
        strcpy(p_list[i].p_filename, p_trs->p_dir);
        snprintf(myname, 128, "trs_m4v_%d.%s", i, fileext);
        strcat(p_list[i].p_filename, myname);
        p_list[i].i_state = 0;
    }

	//input...
	//输入信息处理，多音轨对应到输出多音轨的处理
	p_input->i_video_st = -1;
//	p_input->i_audio_st = -1;
	
	p_output->i_audio_streams = 0;
	p_output->p_output_audio = 0;
	p_input->p_audio_input = 0;
	p_input->p_audio_map = 0;
	i_input_audiostream_num = 0;
	p_input->i_audio_streams = get_audio_tracks_count(p_input->ic);
	p_input->i_audio_stream_supported = p_input->i_audio_streams;
	if (p_input->i_audio_streams > 0) {
		p_input->p_audio_input = (trs_input_audio**)av_mallocz(sizeof(trs_input_audio*)*p_input->i_audio_streams);
		p_input->p_audio_map = (int32_t*)av_mallocz(sizeof(int32_t)*p_input->i_audio_streams * 2);

		p_output->i_audio_streams = p_input->i_audio_streams;
		p_output->p_output_audio = (trs_output_audio **)av_mallocz(sizeof(trs_output_audio*)*p_output->i_audio_streams);
	}

	//
	for(i=0; i<p_input->ic->nb_streams; i++)
	{
        AVStream *st = p_input->ic->streams[i];
        AVCodecContext *dec = st->codec;
        switch (dec->codec_type) 
		{
        case AVMEDIA_TYPE_AUDIO:
			{
				bool b_need_audio_dec = true;
				bool b_need_audio_enc = true;

				if (!strcasecmp(p_output->psz_format, "mpegts")) {
					if (dec->codec_id == CODEC_ID_AAC || dec->codec_id == CODEC_ID_MP3 || dec->codec_id == CODEC_ID_AC3 || dec->codec_id == CODEC_ID_MP2) {
						LOGI("just copy audio codec\n");
						b_need_audio_dec = false;
						b_need_audio_enc = false;
					}
				}
				else if (!strcasecmp(p_output->psz_format, "3gp") || !strcasecmp(p_output->psz_format, "ipod") || !strcasecmp(p_output->psz_format, "mp4")) {
					if (dec->codec_id == CODEC_ID_AAC) {
						LOGI("just copy audio codec\n");
						b_need_audio_dec = false;
						b_need_audio_enc = false;
					}
				}

				//多音轨的输入映射map build
				p_input->p_audio_input[i_input_audiostream_num] = (trs_input_audio*)av_mallocz(sizeof(trs_input_audio));
				p_input->p_audio_map[i_input_audiostream_num*2] = i;
				p_input->p_audio_map[i_input_audiostream_num*2+1] = i_input_audiostream_num;

				p_input->p_audio_input[i_input_audiostream_num]->i_audio_st = i;
				p_input->p_audio_input[i_input_audiostream_num]->p_audio = st;
				p_input->p_audio_input[i_input_audiostream_num]->p_audio_dec = dec;
				st->discard = AVDISCARD_NONE;

				p_input->p_audio_input[i_input_audiostream_num]->b_decoder_support = true;
				if (b_need_audio_dec)
				{
					AVCodec *decoder;
					decoder = avcodec_find_decoder(st->codec->codec_id);
					if (decoder == 0)
					{
						p_input->p_audio_input[i_input_audiostream_num]->b_decoder_support = false;
						p_input->i_audio_stream_supported --;
						st->discard = AVDISCARD_ALL;
					}
				}


				p_input->p_audio_input[i_input_audiostream_num]->b_need_audio_dec = b_need_audio_dec;

				p_output->p_output_audio[i_input_audiostream_num] = (trs_output_audio *)av_mallocz(sizeof(trs_output_audio));
				p_output->p_output_audio[i_input_audiostream_num]->fifo = av_fifo_alloc(1024);
				p_output->p_output_audio[i_input_audiostream_num]->p_u8_aacbuf = 0;
				p_output->p_output_audio[i_input_audiostream_num]->i_size_aacbuf = 0;
				p_output->p_output_audio[i_input_audiostream_num]->audio_sync_opts = 0;
				p_output->p_output_audio[i_input_audiostream_num]->ii_audio_nextpts = AV_NOPTS_VALUE;
				p_output->p_output_audio[i_input_audiostream_num]->ii_audio_lastmuxdts = AV_NOPTS_VALUE;
				p_output->p_output_audio[i_input_audiostream_num]->b_need_audio_enc = b_need_audio_enc;
				p_output->p_output_audio[i_input_audiostream_num]->i_aac_encoder = p_trs->i_aac_encoder;

				i_input_audiostream_num ++;
			}
			break;

        case AVMEDIA_TYPE_VIDEO:
			{
				bool b_need_video_dec = true;
				bool b_need_video_enc = true;

				if (!strcasecmp(p_output->psz_format, "mpegts")) {
					if (dec->codec_id == CODEC_ID_H264 || dec->codec_id == CODEC_ID_MPEG2VIDEO)
					{
						b_need_video_dec = false;
						b_need_video_enc = false;
					}
				}
				else if (!strcasecmp(p_output->psz_format, "3gp") || !strcasecmp(p_output->psz_format, "ipod") || !strcasecmp(p_output->psz_format, "mp4")) {
					if (dec->codec_id == CODEC_ID_H264)
					{
						b_need_video_dec = false;
						b_need_video_enc = false;
					}
				}

				p_input->b_video_decoder_support = true;
				if (b_need_video_dec)
				{
					AVCodec *decoder;
					decoder = avcodec_find_decoder(st->codec->codec_id);
					if (decoder == 0)
					{
						p_input->b_video_decoder_support = false;
					}
				}

				p_input->i_video_st = i;
				p_input->p_video = st;
				st->discard = AVDISCARD_NONE;

				p_input->b_need_video_dec = b_need_video_dec;
				p_output->b_need_video_enc = b_need_video_enc;
				p_output->ii_video_nextpts = AV_NOPTS_VALUE;
				p_output->ii_video_sync_opts = 0;
			}
			break;

		default:
			st->discard = AVDISCARD_ALL;
			break;
		}
	}

    //building m3u8 playlist
    if (playlist_type && !strcasecmp(playlist_type, "m3u8")) {
        snprintf(p_trs->psz_playlist, 1024, "%s1.m3u8", p_trs->p_dir);
        i_ismpegts = 1;
        
        AVStream *st = 0;
		if (p_input->i_audio_streams > 0)
			st = p_input->p_audio_input[0]->p_audio;
        if (p_input->i_video_st >= 0)
            st = p_input->p_video;
        
#ifdef	PAD_0_TSFILE
		if (p_trs->i_limit_type > 0)
			createM3u8(p_input, p_trs->psz_playlist, local_host, p_trs->f_segmenter_duration, p_trs->i_output_total, p_trs->f_limit_duration, p_list);
		else
			createM3u8(p_input, p_trs->psz_playlist, local_host, p_trs->f_segmenter_duration, p_trs->i_output_total, p_input->f_duration, p_list);
#endif
        trs_fireout(p_trs, TRANS_PLAYLISTREADY, 0, 0);
    }
    
    //recover from crash.
	trs_recover(p_trs);

    //seeking...
	timestamp = p_input->i_start_time;
	if (p_input->ic->start_time != AV_NOPTS_VALUE)
        timestamp += p_input->ic->start_time;
    
	if (p_input->i_start_time > 0)
	{
		int64_t seek_target = av_rescale(timestamp, p_input->p_video->time_base.den, AV_TIME_BASE * (int64_t)p_input->p_video->time_base.num);
		ret = av_seek_frame(p_input->ic, p_input->i_video_st, seek_target, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
		{
			LOGI("%s: could not seek to position %0.3f\n",
                 p_input->p_filename, (double)timestamp / AV_TIME_BASE);
			ret = -1;
			goto fail;
		}
	}
    p_trs->i_output_trsing = p_trs->i_output_start;
	for (i = 0; i < p_input->i_audio_streams; i ++) {
		p_input->p_audio_input[i]->audio_rescale_delta_last = AV_NOPTS_VALUE;
		p_input->p_audio_input[i]->audio_next_pts = AV_NOPTS_VALUE;
	}
    p_input->video_next_pts = AV_NOPTS_VALUE;

	//output...
	LOGD("===>>> new item index with start: %d\n", p_trs->i_output_trsing);
#if 0
	err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#else
    err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, format);
	err = open_output_file(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#endif
    if (err < 0)
    {
        ret = -1;
        goto fail;
    }
    if (!p_input->i_audio_stream_supported)
    {
        ret = -1;
        goto  fail;
    }

	for(;;)
	{
        AVPacket pkt;
		if (p_trs->abort)
			break;

		if (p_trs->pause && !p_trs->abort)
		{
#ifdef WIN32
			Sleep(10);
#else
			msleep(CLOCK_FREQ / 100);
#endif
		}
        
		//处理中间seek事件
        if (p_trs->sek_req) {
            if (p_trs->i_seekindex != -1) {
                p_trs->f_seek = p_list[p_trs->i_seekindex].f_start_time;;//p_trs->i_seekindex * p_trs->f_segmenter_duration;//p_list[p_trs->i_seekindex].f_start_time;
            }
            else {
                p_trs->i_seekindex = p_trs->f_seek / p_trs->f_segmenter_duration;
                if (p_trs->i_seekindex * p_trs->f_segmenter_duration < p_trs->f_seek)
                    p_trs->i_seekindex ++;
            }
            
            if (p_trs->i_seekindex != p_trs->i_output_trsing && p_list[p_trs->i_seekindex].i_state < 1) {
                timestamp = p_trs->f_seek*AV_TIME_BASE;
                /* add the stream start time */
                if (p_input->ic->start_time != AV_NOPTS_VALUE)
                {
                    timestamp += p_input->ic->start_time;
                }
                ret = trs_seek_internal(p_input, timestamp, 0, 0);
                
                if (ret == 0 && p_trs->i_seekindex < p_trs->i_output_total)
                {
#if 0
                    trs_close_output(p_output);
#else
					trs_flush_output(p_output);
					close_output_file(p_output);
					if (p_output && p_output->oc)
						LOGI("===>>> close item index: %d\n", p_trs->i_output_trsing);
					trs_close_output(p_output);
#endif
                    
                    p_list[p_trs->i_output_trsing].i_index = p_trs->i_output_trsing;
                    p_list[p_trs->i_output_trsing].i_state = 0;                     //abort
                    p_list[p_trs->i_output_trsing].f_start_time = p_trs->seg_start * av_q2d(p_input->p_video->time_base);
                    p_list[p_trs->i_output_trsing].f_duration = p_trs->seg_duration * av_q2d(p_input->p_video->time_base);
                    
                    trs_fireout(p_trs, TRANS_ABORTITEM, p_trs->i_output_trsing, 0);
                    
                    //start new segment
                    p_trs->i_output_trsing = p_trs->i_seekindex;
                    
                    LOGD("===>>> new item index with seek: %d\n", p_trs->i_output_trsing);
#if 0
                    err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#else
					err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, format);
					err = open_output_file(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#endif
                    if (err < 0)
                    {
                        av_free_packet(&pkt);
                        ret = -1;
                        break;
                    }
                    
                    eof = 0;
                    p_list[p_trs->i_output_trsing].i_state = 1;
                    p_trs->seg_duration = 0;
                    p_trs->seg_start = AV_NOPTS_VALUE;
					p_trs->audio_seg_start = AV_NOPTS_VALUE;
					p_trs->ii_video_pre_pts = AV_NOPTS_VALUE;

//                    int ticks = p_input->p_video->parser ? p_input->p_video->parser->repeat_pict + 1 : p_input->p_video->codec->ticks_per_frame;
//                    p_input->video_next_pts = timestamp - ((int64_t)AV_TIME_BASE * p_input->p_video->time_base.num * ticks) / p_input->p_video->time_base.den;
//                    p_input->audio_next_pts = timestamp - av_rescale_q(1, p_input->p_audio->time_base, AV_TIME_BASE_Q);

//                    int64_t audio_timestamp = av_rescale_q(timestamp, AV_TIME_BASE_Q, (AVRational){1, p_input->p_audio->codec->sample_rate});
//                    p_input->audio_rescale_delta_last = audio_timestamp;
                    
					for (int i = 0; i < p_input->i_audio_streams; i ++)
					{
						p_input->p_audio_input[i]->audio_rescale_delta_last = AV_NOPTS_VALUE;
						p_input->p_audio_input[i]->audio_next_pts = timestamp;
					}
					p_input->video_next_pts = timestamp;
					for (int i = 0; i < p_output->i_audio_streams; i ++) 
					{
						p_output->p_output_audio[i]->audio_sync_opts = 0;
						p_output->p_output_audio[i]->ii_audio_nextpts = AV_NOPTS_VALUE;
						p_output->p_output_audio[i]->ii_audio_lastmuxdts = AV_NOPTS_VALUE;
					}
                    p_output->ii_video_lastmuxdts = AV_NOPTS_VALUE;
					p_output->ii_video_nextpts = AV_NOPTS_VALUE;
					p_output->ii_video_sync_opts = 0;
                }
            }
            
            p_trs->sek_req = 0;
            p_trs->i_seekindex = -1;
            p_trs->f_seek = 0;
            i_waiting = 0;
        }
        
		//如果等待，则等待结束标记
        if (i_waiting == 1) {
#ifdef WIN32
			Sleep(10);
#else
			msleep(CLOCK_FREQ / 100);
#endif
            break;//continue???
        }

		if (eof)	//文件结束标记
        {
            trs_fireout(p_trs, TRANS_PROGRESS, p_trs->i_output_trsing, 100);
            p_list[p_trs->i_output_trsing].i_state = 2;
            if (p_trs->p_output)
            {
#if 0
                trs_close_output(p_output);
#else
				trs_flush_output(p_output);
				close_output_file(p_output);
				if (p_output && p_output->oc)
					LOGI("===>>> close item index: %d\n", p_trs->i_output_trsing);
				trs_close_output(p_output);
#endif
            }
            trs_fireout(p_trs, TRANS_ADDITEM, p_trs->i_output_trsing, 0);
            
            i_waiting = 1;
//            break;
        }
        
		//读取文件packet
        p_trs->i_entry_avread = 1;
		ret= av_read_frame(p_input->ic, &pkt);
        p_trs->i_entry_avread = 0;
        if (ret < 0)
		{
            if (ret == AVERROR_EOF || url_feof(p_input->ic->pb))
			{
                eof = 1;
			}

            if (p_input->ic->pb && p_input->ic->pb->error)
			{
				LOGI("---------- av_read_frame failed, ret=%d\n", ret);
                eof = 1;
//                break;
			}

#ifdef WIN32
			Sleep(10);
#else
			msleep(CLOCK_FREQ / 100);
#endif
            continue;
        }

		//处理音频packet
		if (pkt.stream_index != p_input->i_video_st)
		{
			//多音轨支持，找到当前输入音轨的Input/Output Context
			trs_input_audio *p_input_audio = 0;
			trs_output_audio *p_output_audio = 0;
			for (int i = 0; i < p_input->i_audio_streams; i ++)
			{
				if (p_input->p_audio_map[i*2] == pkt.stream_index)
				{
					p_input_audio = p_input->p_audio_input[p_input->p_audio_map[i*2+1]];
					p_output_audio = p_output->p_output_audio[p_input->p_audio_map[i*2+1]];
					break;
				}
			}

			if (!p_input_audio)
				continue;
				
            if (pkt.pts != AV_NOPTS_VALUE)
            {
				//初始化分片音轨起始时间
                if (p_trs->audio_seg_start == AV_NOPTS_VALUE) {
                    AVRational cq = {1, AV_TIME_BASE};
                    if (i_ismpegts == 0) {
                        p_trs->audio_seg_start = pkt.pts;
                    }
                    else {
                        if (p_trs->i_output_trsing > 0)
                            p_trs->audio_seg_start = pkt.pts;
                        else
                            p_trs->audio_seg_start = av_rescale_q(p_input->ic->start_time, cq, p_input_audio->p_audio->time_base);
                    }

					//初始化输入的第一帧时间戳
                    ii_first_pts = av_rescale_q(p_input->ic->start_time, cq, p_input_audio->p_audio->time_base);
                }

				//初始化下一帧时间戳
                if (p_input_audio->audio_next_pts == AV_NOPTS_VALUE) {
                    
                    p_input_audio->audio_next_pts = p_input_audio->p_audio->avg_frame_rate.num ?
                                            (-p_input_audio->p_audio->codec->has_b_frames*AV_TIME_BASE/av_q2d(p_input_audio->p_audio->avg_frame_rate))
                                            : 0;

                    if (p_input_audio->b_need_audio_dec == false && p_output_audio->b_need_audio_enc == false)
#if defined( _MSC_VER )
					{
						AVRational cq = {1, AV_TIME_BASE};
                        p_input_audio->audio_next_pts += av_rescale_q(pkt.pts, p_input_audio->p_audio->time_base, cq);
					}
#else
                        p_input_audio->audio_next_pts += av_rescale_q(pkt.pts, p_input_audio->p_audio->time_base, AV_TIME_BASE_Q);
#endif
                    if (p_input_audio->audio_next_pts < 0)
                        p_input_audio->audio_next_pts = 0;
                }
            }
            
			if (p_input_audio->b_need_audio_dec == true && p_output_audio->b_need_audio_enc == true)	//转码
                do_trans_audio(p_trs, p_input_audio, p_output_audio, p_output->oc, &pkt, i_ismpegts>0?ii_first_pts:p_trs->audio_seg_start);
			else																						//just copy
				do_copy_pkt(p_trs, p_output->oc, p_input_audio->p_audio, p_output_audio->p_audio, &pkt, i_ismpegts>0?ii_first_pts:p_trs->audio_seg_start, p_input_audio->audio_next_pts, 
							p_input_audio->audio_rescale_delta_last, p_output_audio->ii_audio_lastmuxdts, p_output_audio->p_audio_bitstreamfilter, p_output_audio->i_audio_stream_index);
		}
		else if (pkt.stream_index == p_input->i_video_st)	//处理video packet
		{
            if (pkt.pts != AV_NOPTS_VALUE)
            {
                if (p_trs->seg_start == AV_NOPTS_VALUE) {
                    AVRational cq = {1, AV_TIME_BASE};
                    if (i_ismpegts == 0) {
                        p_trs->seg_start = pkt.pts;
                    }
                    else {
                        if (p_trs->i_output_trsing > 0)
                            p_trs->seg_start = pkt.pts;
                        else
                            p_trs->seg_start = av_rescale_q(p_input->ic->start_time, cq, p_input->p_video->time_base);
                    }
                    ii_first_pts = av_rescale_q(p_input->ic->start_time, cq, p_input->p_video->time_base);
                }
                p_trs->seg_duration = pkt.pts - p_trs->seg_start;//sample index
                if (p_input->video_next_pts == AV_NOPTS_VALUE) {
                    p_input->video_next_pts = p_input->p_video->avg_frame_rate.num ?
                                            (-p_input->p_video->codec->has_b_frames*AV_TIME_BASE/av_q2d(p_input->p_video->avg_frame_rate))
                                            : 0;

					if (p_input->b_need_video_dec == false && p_output->b_need_video_enc == false)
					{
#if defined( _MSC_VER )
						AVRational cq = {1, AV_TIME_BASE};
						p_input->video_next_pts += av_rescale_q(pkt.pts, p_input->p_video->time_base, cq);
#else
						p_input->video_next_pts += av_rescale_q(pkt.pts, p_input->p_video->time_base, AV_TIME_BASE_Q);
#endif
					}

					//FIXME: video_next_pts must >= 0???
                    //if (p_input->video_next_pts < 0)
                    //    p_input->video_next_pts = 0;
                }
            }
            
			if (pkt.flags & AV_PKT_FLAG_KEY) {
				LOGD("Key Frame:-------- %lld", pkt.pts);

				char iframe[256] = "";
				snprintf(iframe, 256, "#%lld,%f\n", pkt.pts, pkt.pts*av_q2d(p_input->p_video->time_base));
				int wd = fwrite(iframe, 1, strlen(iframe), p_trs->fp_iframe);
				if (wd == -1) {
					LOGE("----- write file error with errno=%d\n", ferror(p_trs->fp_iframe));
				}
				
				fflush(p_trs->fp_iframe);
			}
#if 0
			if (pkt.flags & AV_PKT_FLAG_KEY)		//关键帧是判断是否已经满足切片duration，满足则关闭当前文件，新建下一个文件
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
                        err = trs_open_output(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#else
						err = open_output_file(p_input, p_output, p_list, p_trs->i_output_trsing, format);
#endif
                        if (err < 0)
                        {
                            av_free_packet(&pkt);
                            ret = -1;
                            break;
                        }
                        
                        p_trs->seg_duration = 0;
                        p_list[p_trs->i_output_trsing].i_state = 1;
                        p_trs->seg_start = pkt.pts;
                        p_trs->audio_seg_start = AV_NOPTS_VALUE;
                    }
				}
			}
#endif

			if (p_input->b_need_video_dec == true && p_output->b_need_video_enc == true)	//video 转码
			{
				int got_output = 0;
				int64_t pkt_pts = AV_NOPTS_VALUE, pkt_dts = AV_NOPTS_VALUE;
				AVRational av_time_base_q;
				av_time_base_q.num = 1;av_time_base_q.den = AV_TIME_BASE;

				//LOGD("==>Video pkt_dts=%lld\n", pkt.dts);
				if (pkt.dts != AV_NOPTS_VALUE)
					pkt_dts = av_rescale_q(pkt.dts, p_input->p_video->time_base, av_time_base_q);
				if(pkt.pts != AV_NOPTS_VALUE)
					pkt_pts = av_rescale_q(pkt.pts, p_input->p_video->time_base, av_time_base_q);

				do_trans_video(p_trs, p_input, p_output, &pkt, i_ismpegts>0?ii_first_pts:p_trs->seg_start, &got_output, &pkt_pts, &pkt_dts);
			}
			else																			//video copy
			{
				int64_t ii_temp = 0;
				do_copy_pkt(p_trs, p_output->oc, p_input->p_video, p_output->p_video, &pkt, i_ismpegts>0?ii_first_pts:p_trs->seg_start, p_input->video_next_pts, ii_temp, 
					p_output->ii_video_lastmuxdts, p_output->p_video_bitstreamfilter, 0);
			}
		}
        av_free_packet(&pkt);
	}

fail:
	if (ret == AVERROR_EOF || eof == 1)
	{
        trs_fireout(p_trs, TRANS_END, p_trs->i_output_trsing, 0);
	}
	else if (ret < 0)
	{
        trs_fireout(p_trs, TRANS_ERROR, ret, 0);
	}

#ifndef	PAD_0_TSFILE
	if (p_list) {
		p_list[p_trs->i_output_trsing].i_index = p_trs->i_output_trsing;
		p_list[p_trs->i_output_trsing].i_state = 2;
		p_list[p_trs->i_output_trsing].f_start_time = p_trs->seg_start * av_q2d(p_input->p_video->time_base);
		p_list[p_trs->i_output_trsing].f_duration = p_trs->seg_duration * av_q2d(p_input->p_video->time_base);
	}

	if (p_trs->p_output) {
#if 0
        trs_close_output(p_output);
#else
		trs_flush_output(p_output);
		close_output_file(p_output);
		if (p_output && p_output->oc)
			LOGI("===>>> close item index: %d\n", p_trs->i_output_trsing);
		trs_close_output(p_output);
#endif
		trs_fireout(p_trs, TRANS_END, p_trs->i_output_trsing, 0);

		if (p_list) {
			p_trs->f_total_duration += p_list[p_trs->i_output_trsing].f_duration;
			p_trs->f_total_duration_m3u8 += p_list[p_trs->i_output_trsing].f_duration_m3u8;
		}
		createM3u8ByList(p_trs->psz_playlist, local_host, p_trs->f_segmenter_duration, p_trs->i_output_trsing+1, p_input->f_duration, p_list);
	}
#endif

	if (p_trs->p_input)
	{
		for (int i = 0; i < p_trs->p_input->i_audio_streams; i ++)
		{
			trs_input_audio *p_input_audio = p_trs->p_input->p_audio_input[i];

			if (p_input_audio->b_need_audio_dec == true)
				avcodec_close(p_input_audio->p_audio->codec);

			if (p_input_audio->p_audio_frame)
				av_frame_free(&p_input_audio->p_audio_frame);
			p_input_audio->p_audio_frame = 0;

			av_free(p_input_audio);
			p_trs->p_input->p_audio_input[i] = 0;
		}
		av_free(p_trs->p_input->p_audio_input);

		if (p_trs->p_input->b_need_video_dec == true) 
			avcodec_close(p_trs->p_input->p_video->codec);

		//if (p_trs->p_input->p_video_frame)
		//	av_frame_free(&p_trs->p_input->p_video_frame);
		//p_trs->p_input->p_video_frame = 0;

		if (p_input->ic != 0)
            av_close_input_file(p_input->ic);
        p_input->ic = 0;

//		avio_close(p_input->ic->pb);
//		avformat_free_context(p_input->ic);
        
		av_free(p_trs->p_input);
	}
	p_trs->p_input = 0;

	if (p_trs->p_output)
	{
#ifdef	PAD_0_TSFILE
#if 0
        trs_close_output(p_output);
#else
		trs_flush_output(p_output);
		close_output_file(p_output);
		trs_close_output(p_output);
#endif
		trs_fireout(p_trs, TRANS_END, p_trs->i_output_trsing, 0);
#endif

        if (p_output->psz_format)
            free(p_output->psz_format);
        p_output->psz_format = 0;

		if (p_output->p_video_bit_buffer)
			av_free(p_output->p_video_bit_buffer);
		p_output->p_video_bit_buffer = 0;

#ifdef	USING_AVFILTER
		if (p_output->p_video_filter) {
			if (p_output->p_video_filter->filt_frame)
				av_frame_free(&p_output->p_video_filter->filt_frame);
			p_output->p_video_filter->filt_frame = 0;

			if (p_output->p_video_filter->ifilt_frame)
				av_frame_free(&p_output->p_video_filter->ifilt_frame);
			p_output->p_video_filter->ifilt_frame = 0;

			if (p_output->p_video_filter->filter_graph)
				avfilter_graph_free(&p_output->p_video_filter->filter_graph);
			p_output->p_video_filter->filter_graph = 0;
		}
		p_output->p_video_filter = 0;
#endif

		if (p_output->p_output_audio) 
		{
			for (int i = 0; i < p_output->i_audio_streams; i ++)
			{
				if (p_output->p_output_audio[i])
				{
					if (p_output->p_output_audio[i]->fifo)
						av_fifo_free(p_output->p_output_audio[i]->fifo);
					p_output->p_output_audio[i]->fifo = 0;

					if (p_output->p_output_audio[i]->audio_output_frame)
						av_freep(&p_output->p_output_audio[i]->audio_output_frame);
					p_output->p_output_audio[i]->audio_output_frame = 0;

					if (p_output->p_output_audio[i]->p_u8_aacbuf)
						av_free(p_output->p_output_audio[i]->p_u8_aacbuf);
					p_output->p_output_audio[i]->p_u8_aacbuf = 0;
					p_output->p_output_audio[i]->i_size_aacbuf = 0;
					
#ifdef	USING_AVFILTER
					if (p_output->p_output_audio[i]->p_audio_filter) {
						if (p_output->p_output_audio[i]->p_audio_filter->filt_frame)
							av_frame_free(&p_output->p_output_audio[i]->p_audio_filter->filt_frame);
						p_output->p_output_audio[i]->p_audio_filter->filt_frame = 0;

						if (p_output->p_output_audio[i]->p_audio_filter->ifilt_frame)
							av_frame_free(&p_output->p_output_audio[i]->p_audio_filter->ifilt_frame);
						p_output->p_output_audio[i]->p_audio_filter->ifilt_frame = 0;

						if (p_output->p_output_audio[i]->p_audio_filter->filter_graph)
							avfilter_graph_free(&p_output->p_output_audio[i]->p_audio_filter->filter_graph);
						p_output->p_output_audio[i]->p_audio_filter->filter_graph = 0;
					}
#else
					if (p_output->p_output_audio[i]->p_audio_filter) {
						trs_filter_audio_item *pHeader = p_output->p_output_audio[i]->p_audio_filter->pHeader;
						while (pHeader != 0) {
							trs_filter_audio_item *pItem = pHeader;
							pHeader = pHeader->pNext;
							
							AVFrame* p_last_frame = pItem->p_filter_frame;
							if (p_last_frame != 0) {
								av_frame_unref(p_last_frame);
								av_frame_free(&p_last_frame);
							}
							av_free(pItem);
						}
						av_free(p_output->p_output_audio[i]->p_audio_filter);
					}
#endif
					p_output->p_output_audio[i]->p_audio_filter = 0;

					av_free(p_output->p_output_audio[i]);
				}
				p_output->p_output_audio[i] = 0;
			}
			av_free(p_output->p_output_audio);
		}
		p_output->p_output_audio = 0;
        
		av_free(p_trs->p_output);
	}
	p_trs->p_output = 0;
    
    if (p_trs->p_list)
    {
        av_free(p_trs->p_list);
        p_trs->p_list = 0;
    }

	trs_closecr(p_trs);
    if (format)
        free(format);
    if (fileext)
        free(fileext);
    if (playlist_type)
        free(playlist_type);
    if (local_host)
        free(local_host);

	anc_threadvar_set (tls_trsstate_key, NULL);
	avformat_network_deinit();
	return 0;
}
