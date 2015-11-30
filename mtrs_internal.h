#ifndef __MTRS_INTERNAL__
#define __MTRS_INTERNAL__ 1

#include "mtrs.h"

int write_frame(trs_state *p_trs, AVFormatContext *oc, AVPacket *pkt, AVCodecContext *avctx, AVBitStreamFilterContext* bsfc);
int64_t my_rescale_delta(AVRational in_tb, int64_t in_ts,  AVRational fs_tb, int duration, int64_t *last, AVRational out_tb);
int do_trans_audio(trs_state *p_trs, trs_input_audio* p_input, trs_output_audio* p_output, AVFormatContext *oc, AVPacket* avpkt, int64_t start_time);
int do_trans_video(trs_state *p_trs, trs_input* p_input, trs_output* p_output, AVPacket* avpkt, int64_t start_time, int *got_output, int64_t *pkt_pts, int64_t *pkt_dts);
int do_copy_pkt(trs_state *p_trs, AVFormatContext *oc, AVStream* p_input, AVStream* p_output, AVPacket* avpkt, int64_t start_time, int64_t &next_pts, int64_t &rescale_delta_last, 
				int64_t &last_mux_dts, AVBitStreamFilterContext* bsfc, int pkt_index);

void trs_fireout(trs_state* p_trs, int32_t code, int32_t data1, int32_t data2);
void trs_print_error(const char *filename, int err);

int open_output_file(trs_input* p_input, trs_output* p_output, trs_output_list* p_list, int32_t i_index, const char *format);
int close_output_file(trs_output* p_output);

int FileExist(const char* psz_name);
int trs_initcr(trs_state *p_trs);
int trs_closecr(trs_state *p_trs);
int trs_recover(trs_state *p_trs);
#endif