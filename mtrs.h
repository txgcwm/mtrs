#ifndef MTRSLIB_H
#define MTRSLIB_H 1

/* ffmpeg header */
#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include <libavutil/samplefmt.h>
#include <libavutil/fifo.h>
#include <libavutil/pixdesc.h>

#define USING_AVFILTER	1
#ifdef	USING_AVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#endif

#ifdef __cplusplus
}
#endif //__cplusplus

////////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#define VSYNC_AUTO       -1
#define VSYNC_PASSTHROUGH 0
#define VSYNC_CFR         1
#define VSYNC_VFR         2

#define		FILENAME_LEN		(1024)
typedef struct trs_input_audio {
	int32_t			i_audio_st;
	AVStream		*p_audio;
	AVFrame			*p_audio_frame;
	AVCodecContext	*p_audio_dec;
	int64_t			audio_next_pts;
    int64_t         audio_rescale_delta_last;
	bool			b_need_audio_dec;
    bool            b_decoder_support;
} trs_input_audio;

typedef struct trs_input_audio_map {
	int32_t i_stread_id;
	int32_t i_audio_index;
} trs_input_audio_map;

typedef struct trs_input
{
	char	p_filename[FILENAME_LEN];
	int64_t i_start_time;
	int64_t	i_start_write;
	double	f_duration;

	AVFormatContext	*ic;
	AVInputFormat	*ifmt;

	int32_t			*p_audio_map;
	int32_t			i_audio_streams;
	int32_t			i_audio_stream_supported;
	trs_input_audio **p_audio_input;


	int32_t			i_video_st;
	AVStream		*p_video;
	AVFrame			*p_video_frame;
    int64_t         video_next_pts;
	bool			b_need_video_dec;
	bool			b_video_decoder_support;
} trs_input;

typedef struct trs_output_list
{
	char	p_filename[FILENAME_LEN];
	int32_t	i_index;
    int32_t i_state;                    //0:unknow, 1:trsing, 2:over
	double	f_start_time;
	double f_duration;
    double f_duration_m3u8;

	trs_output_list *p_next;
} trs_output_list;

typedef struct trs_filter_audio_item {
	int		i_off;
	int		i_samples;
	int64_t ii_pts;
	AVFrame	*p_filter_frame;

	trs_filter_audio_item *pNext;
} trs_filter_audio_item;

typedef struct trs_filter {
#ifndef	USING_AVFILTER
	int		i_off;
	int		i_samples;

	trs_filter_audio_item *pHeader;
	trs_filter_audio_item *pLast;
#else
	AVFilterGraph *filter_graph;
	AVFilterContext *buffersink_ctx;
	AVFilterContext *buffersrc_ctx;
	AVFrame *filt_frame;
	AVFrame *ifilt_frame;
#endif
} trs_filter;

typedef struct trs_output_audio {
	int32_t			i_audio_stream_index;

	AVStream		*p_audio;
	AVCodecContext	*p_audio_enc;
	AVFrame			*audio_output_frame;
	int64_t			audio_sync_opts;       /* output frame counter, could be changed to some true timestamp */ // FIXME look at frame_number
    int64_t         ii_audio_nextpts;
    int64_t         ii_audio_lastmuxdts;
	bool			b_need_audio_enc;
#if LIBAVFORMAT_VERSION_MAJOR >= 53    
    AVFifoBuffer    *fifo;
#endif
	trs_filter		*p_audio_filter;

    AVBitStreamFilterContext *p_audio_bitstreamfilter;
    
	//aac encoder
    int         i_aac_encoder;
    uint8_t*    p_u8_aacbuf;
    int64_t     i_size_aacbuf;
} trs_output_audio;

typedef struct trs_output
{
	AVFormatContext	*oc;
	
	AVOutputFormat	*p_ofmt;

	int32_t				i_audio_streams;
	trs_output_audio	**p_output_audio;

    int64_t         ii_video_lastmuxdts;	//do_copy_pkt
	int64_t			ii_video_nextpts;		//for do_video_trs, using as encoded pkt's pts
	int64_t			ii_video_sync_opts;		//for do_video_trs, uing as recode frame count
	int				top_field_first;
	AVStream		*p_video;
	trs_filter		*p_video_filter;
    AVBitStreamFilterContext *p_video_bitstreamfilter;
	bool			b_need_video_enc;
	int				i_video_bit_buffer_size;
	uint8_t			*p_video_bit_buffer;

    int             err_code;
    char*       psz_format;
} trs_output;

typedef void (*pfn_trs_event)(void* p_trs, int32_t code, int32_t data1, int32_t data2, void* p_context);
typedef struct trans_event {
    pfn_trs_event pfn_cb;
    void* p_context;
}trans_event;

typedef struct trs_state
{
	void*       p_context;
	char		p_filename[FILENAME_LEN];
    char        p_dir[FILENAME_LEN];

	anc_thread_t	trs_tid;

	float		f_segmenter_duration;      //s
	float		f_start_time;              //s

	int32_t		i_output_total;
	int32_t		i_output_start;
	int32_t		i_output_last;
	int32_t		i_output_playing;
	int32_t		i_output_trsing;

	trs_input	*p_input;
	trs_output	*p_output;
	trs_output_list	*p_list;

	//controls
	int			pause;
	int			abort;
	int			sek_req;
	float		f_seek;
    int32_t     i_seekindex;
    
    //aac encoder
    int         i_aac_encoder;
    
    AVDictionary*   av_options;
    char           psz_playlist[1024];
    trans_event     *event_cb;
    int         i_entry_avread;

	int			i_limit_type;
	float		f_limit_duration;
	//float		f_limit_filesize;

	//
	float f_total_duration;
	float f_total_duration_m3u8;
	int64_t audio_seg_start;
	int64_t seg_start;
	int64_t seg_duration;

	//
	int			i_crash_times;
	FILE		*fp_log;
	int			i_recover_index;
	float		f_recover_pos_skip;
	float		f_recover_pos;
	FILE		*fp_iframe;
	FILE		*fp_segments;

	//video pts dump
	int64_t		ii_video_pre_pts;
	FILE		*fp_vpts;
} trs_state;

//ffp trans
#define TRANS_ADDITEM   0
#define TRANS_ERROR     1
#define TRANS_END       2
#define TRANS_PROGRESS  3
#define TRANS_PLAYLISTREADY    4
#define TRANS_ABORTITEM    5

void* trs_open(const char* filename, const char* dir, float f_start, float f_seg_duration, int aac_encoder, void* p_lib, trans_event *cb, void* options);
void trs_close(void* handle);
void trs_seek(void* handle, float f_pos);
void trs_start(void* handle);
void trs_pause(void* handle);
void trs_stop(void* handle);
void trs_event(void* handle, int32_t code, int32_t data1, int32_t data2, void* context);
void trs_getItemByIndex(void* handle, int32_t i_index, char**, double *f_start, double *f_duration);
double trs_getDuration(void* handle);
int trs_canwork(const char* filename, void* options);
    
#ifdef __cplusplus
}
#endif //__cplusplus

#endif
