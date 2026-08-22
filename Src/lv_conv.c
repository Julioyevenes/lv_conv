/**
  ******************************************************************************
  * @file    ${file_name} 
  * @author  ${user}
  * @version 
  * @date    ${date}
  * @brief   
  ******************************************************************************
  * @attention
  *
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "lv_conv.h"
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>

#include <libavformat/avformat.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

#include <libswresample/swresample.h>

#include <windows.h>
#include <shlobj.h>
#include <dirent.h>

/* Private types -------------------------------------------------------------*/
typedef enum
{
  LV_CONV_OK    = 0,
  LV_CONV_ERR   = -1
} lv_conv_err_t;

typedef enum
{
  LV_CONV_TRANSCODING = 0,
  LV_CONV_WRITING_HEADER,
  LV_CONV_PACKING
} lv_conv_state_t;

typedef struct
{
  char name[256];
  char full_path[256];
} lv_conv_obj_t;

typedef struct _lv_conv_queue_t
{
  lv_conv_obj_t data;
  struct _lv_conv_queue_t *next;
} lv_conv_queue_t;

typedef struct
{
  AVFilterGraph *filter_graph;
  AVFilterContext *filter_src, *filter_sink;  
} lv_conv_libav_flt_t;

typedef struct
{
  AVCodec * codec;
  AVCodecContext * ctx;
  AVStream * stream;
  int stream_idx;  
} lv_conv_libav_ctx_t;

typedef struct
{
  AVFrame * frame;
  AVPacket * pkt;  
} lv_conv_libav_obj_t;

typedef struct
{
  int32_t buf_fill;
  int32_t buf_size;
  int32_t max_frame_bytes;
  int32_t cur_quality;
} lv_conv_libav_vbv_t;

typedef struct
{
  AVFormatContext * fmt_ctx;
  SwrContext * swr_ctx;
  
  lv_conv_libav_obj_t enc_objs;
  lv_conv_libav_obj_t dec_objs;
  lv_conv_libav_obj_t avr_objs;
  lv_conv_libav_obj_t flt_objs;
  
  lv_conv_libav_ctx_t video_enc;
  lv_conv_libav_ctx_t video_dec;
  lv_conv_libav_ctx_t audio_dec;
  
  lv_conv_libav_flt_t video_flt;
  
  lv_conv_libav_vbv_t vbv_ctrl;
} lv_conv_libav_t;

typedef struct
{
  /* video */
  uint16_t frame_width;
  uint16_t frame_height;
  uint8_t  frame_bytedepth;
  uint8_t  frame_rate;
  uint8_t  frame_jpeg;
  uint32_t frame_nb;
  uint32_t frame_vect_size;  
  uint32_t frame_maxsize;

  /* audio */
  uint8_t  audio_numchannels;
  uint8_t  audio_bytedepth;
  uint16_t audio_samplerate;
  uint32_t audio_byterate;
  uint32_t audio_totalsize;

  /* padding */
  uint8_t  pad[481];
} __attribute__((packed, aligned(1))) lv_conv_fileheader_t;

typedef struct
{
  /* video */
  uint32_t frame_size;

  /* padding */
  uint8_t  pad[508];
} __attribute__((packed, aligned(1))) lv_conv_frameheader_t;

typedef struct
{
  /* file structs */
  FILE * dst_file;
  FILE * video_file;
  FILE * audio_file;
  lv_conv_fileheader_t fileheader;
  lv_conv_frameheader_t frameheader;
  
  /* file splitting vars */
  uint32_t parcial_frames;
  uint32_t cur_frames;
  uint8_t  part_num;
  uint8_t  total_parts;
  char     src_name[256];  
  
  /* process vars */
  uint8_t *audio_buf, *image_buf;
  uint32_t *frame_size_buf, audio_size, image_size;
  
  /* settings vars */
  uint16_t frame_width;
  uint16_t frame_height;
  uint8_t  frame_rate;
  uint8_t  frame_quality;
  uint8_t  bitrate_limit;
  uint8_t  bitrate_limit_en;
  uint16_t audio_samplerate;

  /* progress bar vars */
  uint32_t count;
  uint32_t total;
  uint8_t file_count;
  uint8_t file_total;

  /* output folder path string */
  char dst_path[256];

  /* input files queue */
  lv_conv_queue_t * head;
  lv_conv_queue_t * tail;

  /* codec library */
  lv_conv_libav_t libav;
  
  /* conversion state */
  lv_conv_state_t state;

  /* error handling */
  char err_msg[256];
  lv_conv_err_t err;  
} lv_conv_handle_t;

/* Private constants ---------------------------------------------------------*/
#define MAX_FILE_SIZE 0xE0000000
#define DD_OPTS_NUM 20

const char * btns00[] = {"Ok", ""};

/* Private macro -------------------------------------------------------------*/
#define LV_CONV_FREE(ptr)       if (ptr != NULL) \
                                    { \
                                      free(ptr); ptr = NULL; \
                                    }

#define LV_CONV_OBJ_DEL(ptr)    if (ptr != NULL) \
                                    { \
                                      lv_obj_del(ptr); ptr = NULL; \
                                    }


#define LV_CONV_TASK_DEL(ptr)   if (ptr != NULL) \
                                    { \
                                      lv_task_del(ptr); ptr = NULL; \
                                    }

#define LV_CONV_FILE_CLOSE(ptr) if (ptr != NULL) \
                                    { \
                                      fclose(ptr); \
                                    }

/* Private variables ---------------------------------------------------------*/
char * dd_pstr[DD_OPTS_NUM] =
{
  /* Resolution */
  "320x240",
  "480x272",
  "640x480",
  "720x480",
  "800x480",

  /* Frame rate */
  "15",
  "20",
  "25",
  "30",
  
  /* Frame quality */
  "best",
  "high",
  "medium",
  "low",
  
  /* Bitrate limit */
  "1 Mbit/s",
  "2 Mbit/s",
  "5 Mbit/s",
  "8 Mbit/s",
  "10 Mbit/s",  
  
  /* Sample rate */
  "32000",
  "48000"
};

uint32_t dd_opts[DD_OPTS_NUM][2] =
{
  /* Resolution */
  {320, 240},
  {480, 272},
  {640, 480},
  {720, 480},
  {800, 480},

  /* Frame rate */
  {15, 0},
  {20, 0},
  {25, 0},
  {30, 0},
  
  /* Frame quality */
  {4, 0},
  {7, 0},
  {15, 0},
  {30, 0},
  
  /* Bitrate limit */
  {1, 0},
  {2, 0},
  {5, 0},
  {8, 0},
  {10, 0},  

  /* Sample rate */
  {32000, 0},
  {48000, 0},
};

lv_conv_handle_t hconv;

lv_conv_main_state_t conv_main_state;

lv_task_t * conv_task;
lv_task_t * bar_task;
lv_task_t * mbox_task;

static lv_obj_t * tv;
static lv_obj_t * t1;
static lv_obj_t * t2;
static lv_obj_t * ta;
static lv_obj_t * list_files;
static lv_obj_t * h_bar;
static lv_obj_t * bar;
static lv_obj_t * dd_res;
static lv_obj_t * dd_framerate;
static lv_obj_t * dd_quality;
static lv_obj_t * dd_bitrate;
static lv_obj_t * dd_audiorate;
static lv_obj_t * mbox_err;
static lv_obj_t * cb_bitrate;

static lv_style_t style;
static lv_style_t style_hide;

static const uint8_t zero_pad[512] = {0};

/* Private function prototypes -----------------------------------------------*/
static void                   lv_conv_start_tab_create(lv_obj_t * parent);
static void                   lv_conv_settings_tab_create(lv_obj_t * parent);
static lv_obj_t *             lv_conv_mbox_create(lv_obj_t * parent, const char * txt, 
                                                      const char ** str, lv_event_cb_t event_cb);

static void                   lv_conv_btn_event_cb(lv_obj_t * obj, lv_event_t e);
static void                   lv_conv_dropdown_event_cb(lv_obj_t * obj, lv_event_t e);
static void                   lv_conv_conv_btn_event_cb(lv_obj_t * obj, lv_event_t e);
static void                   lv_conv_mbox_err_btn_event_cb(lv_obj_t * btn, lv_event_t e);
static void                   lv_conv_checkbox_event_cb(lv_obj_t * obj, lv_event_t e);

static int                    lv_conv_file_get(lv_conv_obj_t * obj);
static void                   lv_conv_folder_get(char * path);
static void                   lv_conv_file_conv(lv_conv_handle_t * handle);

static uint64_t fsize(FILE *fp);
static void remove_dir(char *path);
static void ansi_to_utf8(const char *ansi_src, char *utf8_dst, size_t dst_size);

const char *av_get_media_type_string(enum AVMediaType media_type);
static int output_pkt(AVCodecContext *ctx, AVPacket *pkt);
static int output_frame(AVCodecContext *ctx, AVFrame *frame);
static int output_sample(uint8_t *out_data, int buff_size);
static int encode (AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame,
                   char *err_msg);
static int decode (AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame,
                   char *err_msg);
static int resample(SwrContext **ctx, AVFrame *out_frame, AVFrame *in_frame,
                    char *err_msg);
static int open_encoder_context (enum AVCodecID id, AVCodecContext **ctx, 
                                 enum AVMediaType type, 
                                 enum AVPixelFormat pix_fmt,
                                 int frame_rate, int global_quality, 
                                 int width, int height,
                                 enum AVSampleFormat sample_fmt,
                                 int sample_rate, int channels,
                                 char *err_msg);
static int open_decoder_context (int *stream_idx, AVCodecContext **ctx,
                                 AVFormatContext *fmt_ctx, 
                                 enum AVMediaType type,
                                 char *src_filename, 
                                 char *err_msg);
static int open_resampler_context (SwrContext **ctx, uint64_t in_channel_layout_mask,
                                   enum AVSampleFormat in_sample_fmt, int in_sample_rate,
                                   uint64_t out_channel_layout_mask, enum AVSampleFormat out_sample_fmt,
                                   int out_sample_rate, char *err_msg);
static int init_filter_graph(enum AVMediaType type,
                             AVFilterGraph **graph, 
                             AVFilterContext **src,
                             AVFilterContext **sink,
                             char *src_option_str,
                             char *filter_option_str,
                             char *err_msg);

static void                   lv_conv_queue_put(lv_conv_queue_t ** head, lv_conv_queue_t ** tail, lv_conv_obj_t * data);
static lv_conv_queue_t *      lv_conv_queue_get(lv_conv_queue_t ** head, lv_conv_queue_t ** tail);

static void                   lv_conv_conv_task_cleanup(lv_conv_handle_t * handle);
static void                   lv_conv_conv_task_kill(lv_conv_handle_t * handle);

void                          lv_conv_bar_task(lv_task_t * task);
void                          lv_conv_mbox_task(lv_task_t * task);

void lv_conv_init(void)
{
  /* default settings */
  hconv.frame_width = 800;
  hconv.frame_height = 480;
  hconv.frame_rate = 25;
  hconv.frame_quality = 7;
  hconv.bitrate_limit = 5;
  hconv.bitrate_limit_en = 0;
  hconv.audio_samplerate = 48000;
  
  tv = lv_tabview_create(lv_scr_act(), NULL);
  t1 = lv_tabview_add_tab(tv, "Start");
  t2 = lv_tabview_add_tab(tv, "Settings");

  lv_style_init(&style);
  lv_style_set_value_align(&style, LV_STATE_DEFAULT, LV_ALIGN_OUT_TOP_LEFT);
  
  lv_style_init(&style_hide);
  lv_style_set_bg_opa(&style_hide, LV_STATE_DEFAULT, 0);
  lv_style_set_border_opa(&style_hide, LV_STATE_DEFAULT, 0);

  lv_conv_start_tab_create(t1);
  lv_conv_settings_tab_create(t2);
  
  mbox_task = lv_task_create(lv_conv_mbox_task, 500, LV_TASK_PRIO_MID, &hconv);
}

void lv_conv_run(void)
{
  lv_conv_handle_t * handle = &hconv;
  lv_conv_libav_t * libav = &handle->libav;
  lv_conv_libav_ctx_t * video_enc = &libav->video_enc;
  lv_conv_libav_ctx_t * video_dec = &libav->video_dec;
  lv_conv_libav_ctx_t * audio_dec = &libav->audio_dec;
  lv_conv_libav_flt_t * video_flt = &libav->video_flt;
  lv_conv_queue_t * queue;
  int ret = 0, ret_read_frame = 0;
  char * pstr, str[256] = {0};
  DIR *dir;
  uint32_t pad_size, max_byterate;
  uint32_t start_idx, end_idx;
  uint64_t file_size, header_size, frame_vect_size, data_size;
  
  char srt_src_path[256];
  char srt_dst_path[256];
  char temp_folder[256];
  uint8_t has_subs = 0;
  FILE *srt_file;  

  /* initialize */
  if (!libav->fmt_ctx)
  {
    /* get src file name from queue */
    queue = lv_conv_queue_get(&handle->head, &handle->tail);
    if (queue)
    {
      /* delete current file list entry */
      lv_list_remove(list_files, 0);

      /* open input file, and allocate format context */
      if (avformat_open_input (&libav->fmt_ctx, queue->data.full_path, NULL, NULL) < 0)
      {
        sprintf (handle->err_msg, "Could not open source file %s\n", queue->data.full_path);
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }

      /* retrieve stream information */
      if (avformat_find_stream_info (libav->fmt_ctx, NULL) < 0)
      {
        sprintf (handle->err_msg, "Could not find stream information\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* set video stream decoder context */
      if (open_decoder_context (&video_dec->stream_idx, &video_dec->ctx, 
                                libav->fmt_ctx,
                                AVMEDIA_TYPE_VIDEO,
                                queue->data.full_path, 
                                handle->err_msg) >= 0)
      {
        video_dec->stream = libav->fmt_ctx->streams[video_dec->stream_idx];
      }
      
      /* Bitrate control initialization */
      if (handle->bitrate_limit_en)
      {
        max_byterate = (handle->bitrate_limit * 1000 * 1000) / 8;  

        handle->libav.vbv_ctrl.max_frame_bytes = max_byterate / handle->frame_rate;
        handle->libav.vbv_ctrl.buf_size = max_byterate / 2;
        
        handle->libav.vbv_ctrl.buf_fill = 0;
        handle->libav.vbv_ctrl.cur_quality = handle->frame_quality;
      }      
      
      /* set video stream encoder context */
      if (open_encoder_context (AV_CODEC_ID_MJPEG, &video_enc->ctx, 
                                AVMEDIA_TYPE_VIDEO,
                                AV_PIX_FMT_YUVJ420P,
                                handle->frame_rate, handle->frame_quality, 
                                handle->frame_width, handle->frame_height, 
                                AV_SAMPLE_FMT_S16, handle->audio_samplerate, 2,
                                handle->err_msg) < 0)
      {
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;        
      }     

      /* set audio stream decoder context */
      if (open_decoder_context (&audio_dec->stream_idx, &audio_dec->ctx, 
                                libav->fmt_ctx,
                                AVMEDIA_TYPE_AUDIO,
                                queue->data.full_path, 
                                handle->err_msg) >= 0)
      {
        audio_dec->stream = libav->fmt_ctx->streams[audio_dec->stream_idx];
      }

      if (!audio_dec->stream || !video_dec->stream)
      {
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* init frame count variables */
      handle->count = 0;
      handle->total = av_rescale_rnd(video_dec->stream->nb_frames, handle->frame_rate, 
                                     video_dec->stream->avg_frame_rate.num / video_dec->stream->avg_frame_rate.den, 
                                     AV_ROUND_DOWN);      
      
      /* alloc memory for buf with frame sizes */
      handle->frame_size_buf = (uint32_t *)malloc(handle->total * sizeof(uint32_t));
      if (!handle->frame_size_buf)
      {
        sprintf (handle->err_msg, "Could not allocate memory for buf with frame sizes\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* alloc memory for frame and packet structs */
      libav->enc_objs.frame = av_frame_alloc ();
      if (!libav->enc_objs.frame)
      {
        sprintf (handle->err_msg, "Could not allocate frame\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }

      libav->enc_objs.pkt = av_packet_alloc ();
      if (!libav->enc_objs.pkt)
      {
        sprintf (handle->err_msg, "Could not allocate packet\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }      

      libav->dec_objs.frame = av_frame_alloc ();
      if (!libav->dec_objs.frame)
      {
        sprintf (handle->err_msg, "Could not allocate frame\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }

      libav->dec_objs.pkt = av_packet_alloc ();
      if (!libav->dec_objs.pkt)
      {
        sprintf (handle->err_msg, "Could not allocate packet\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      libav->avr_objs.frame = av_frame_alloc ();
      if (!libav->avr_objs.frame)
      {
        sprintf (handle->err_msg, "Could not allocate frame\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }

      libav->avr_objs.pkt = av_packet_alloc ();
      if (!libav->avr_objs.pkt)
      {
        sprintf (handle->err_msg, "Could not allocate packet\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      libav->flt_objs.frame = av_frame_alloc ();
      if (!libav->flt_objs.frame)
      {
        sprintf (handle->err_msg, "Could not allocate frame\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }

      libav->flt_objs.pkt = av_packet_alloc ();
      if (!libav->flt_objs.pkt)
      {
        sprintf (handle->err_msg, "Could not allocate packet\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* check if handle->dst_path is valid */
      if (handle->dst_path[0] == '\0')
      {
        sprintf (handle->err_msg, "Invalid output folder\n");
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;        
      }
      
      /* create temp dir */
      snprintf(str, sizeof(str), "%s\\temp", handle->dst_path);
      dir = opendir(str);
      if (!dir)
      {
        if (mkdir(str) < 0)
        {
          sprintf (handle->err_msg, "Could not create temp dir in %s\n",
                   str);
          handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;     
        }
      }      
      
      /* check if subtitles are available */
      strcpy(srt_src_path, queue->data.full_path);
      pstr = strrchr(srt_src_path, '.');
      if (pstr) strcpy(pstr, ".srt");
      else strcat(srt_src_path, ".srt");
      
      srt_file = fopen(srt_src_path, "r");
      if (srt_file)
      {
        fclose(srt_file);
        
        snprintf(srt_dst_path, sizeof(srt_dst_path), "%s\\temp\\temp.srt", handle->dst_path);
        CopyFile(srt_src_path, srt_dst_path, FALSE);
        
        snprintf(temp_folder, sizeof(temp_folder), "%s\\temp", handle->dst_path);
        SetCurrentDirectory(temp_folder);        
        
        has_subs = 1;
      }
      
      /* set up the video filtergraph */
      char src_option_str[1024], filter_option_str[1024];

      snprintf(src_option_str, sizeof(src_option_str),
               "width=%d:height=%d:pix_fmt=%s:sar=%d/%d:time_base=%d/%d:frame_rate=%d/%d",
               video_dec->ctx->width, video_dec->ctx->height, 
               av_get_pix_fmt_name(video_dec->ctx->pix_fmt),
               video_dec->stream->sample_aspect_ratio.num, video_dec->stream->sample_aspect_ratio.den,
               video_dec->stream->time_base.num, video_dec->stream->time_base.den,
               video_dec->stream->avg_frame_rate.num, video_dec->stream->avg_frame_rate.den);
      
      if (has_subs)
      {
        snprintf(filter_option_str, sizeof(filter_option_str),
                 "fps=%d,scale=%d:%d,subtitles=temp.srt",
                 handle->frame_rate, handle->frame_width, handle->frame_height);        
      }
      else
      {
        snprintf(filter_option_str, sizeof(filter_option_str),
                 "fps=%d,scale=%d:%d",
                 handle->frame_rate, handle->frame_width, handle->frame_height);
      }
               
      if (init_filter_graph(AVMEDIA_TYPE_VIDEO,
                            &video_flt->filter_graph, 
                            &video_flt->filter_src,
                            &video_flt->filter_sink,
                            src_option_str,
                            filter_option_str,
                            handle->err_msg) < 0)
      {
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;  
      }
      
      /* create resampler context */
      if (open_resampler_context (&libav->swr_ctx, audio_dec->ctx->ch_layout.u.mask,
                                   audio_dec->ctx->sample_fmt, audio_dec->ctx->sample_rate,
                                   AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                                   handle->audio_samplerate, handle->err_msg) < 0)
      {
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* create video temp file */
      snprintf(str, sizeof(str), "%s\\temp\\video.raw", handle->dst_path);
      handle->video_file = fopen (str, "wb");
      if (!handle->video_file)
      {
        sprintf (handle->err_msg, "Could not open video temp file in %s\n",
                 str);
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }      
      
      /* create audio temp file */
      snprintf(str, sizeof(str), "%s\\temp\\audio.raw", handle->dst_path);
      handle->audio_file = fopen (str, "wb");
      if (!handle->audio_file)
      {
        sprintf (handle->err_msg, "Could not open audio temp file in %s\n",
                 str);
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }
      
      /* save file name */
      strcpy(handle->src_name, queue->data.name);
      
      handle->file_count++;
      handle->state = LV_CONV_TRANSCODING;
      
      LV_CONV_FREE(queue)
    }
    else
    {
      sprintf (handle->err_msg, "File list is empty\n");
      handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
    }
  }
  
  /* process */
  switch(handle->state)
  {
    case LV_CONV_TRANSCODING:
      /* read frames from the file */
      ret_read_frame = av_read_frame (libav->fmt_ctx, libav->dec_objs.pkt);
      if (ret_read_frame >= 0)
      {
        /* check if the packet belongs to a stream we are interested in, otherwise */
        /* skip it */
        if (libav->dec_objs.pkt->stream_index == video_dec->stream_idx)
          ret = decode(video_dec->ctx, libav->dec_objs.pkt, libav->dec_objs.frame, handle->err_msg);
        else if (libav->dec_objs.pkt->stream_index == audio_dec->stream_idx)
          ret = decode(audio_dec->ctx, libav->dec_objs.pkt, libav->dec_objs.frame, handle->err_msg);

        av_packet_unref (libav->dec_objs.pkt);
        if (ret < 0)
          ret_read_frame = -1;  
      }
      
      /* finish stage */
      if (ret_read_frame < 0)
      {
        /* flush the decoders */
        if (video_dec->ctx)
            decode(video_dec->ctx, NULL, libav->dec_objs.frame, handle->err_msg);
        if (audio_dec->ctx)
            decode(audio_dec->ctx, NULL, libav->dec_objs.frame, handle->err_msg);
        
        fclose(handle->video_file);
        snprintf(str, sizeof(str), "%s\\temp\\video.raw", handle->dst_path);
        handle->video_file = fopen (str, "rb");
        if (!handle->video_file)
        {
          sprintf (handle->err_msg, "Could not open audio file %s\n",
                   str);
          handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
        }        

        fclose(handle->audio_file);
        snprintf(str, sizeof(str), "%s\\temp\\audio.raw", handle->dst_path);
        handle->audio_file = fopen (str, "rb");
        if (!handle->audio_file)
        {
          sprintf (handle->err_msg, "Could not open audio file %s\n",
                   str);
          handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
        }
        
        /* If the file size exceeds MAX_FILE_SIZE, split the output file into parts */
        handle->total = handle->count;        
        header_size = sizeof(handle->fileheader);
        frame_vect_size = ceil((double) (handle->total * sizeof(uint32_t)) / 512) * 512;
        data_size = fsize(handle->video_file) + fsize(handle->audio_file);
        file_size = header_size + frame_vect_size + data_size;
        if (file_size > MAX_FILE_SIZE)
        {
          uint32_t avg_bytes_per_frame = data_size / handle->total;
          handle->parcial_frames = (MAX_FILE_SIZE - header_size) / (avg_bytes_per_frame + sizeof(uint32_t));
        }
        else
        {
          handle->parcial_frames = handle->total;
        }

        handle->total_parts = ceil((double)handle->total / handle->parcial_frames);
        handle->part_num = 1;        
        
        ret_read_frame = 0;        
        handle->state = LV_CONV_WRITING_HEADER;
      }
      break;
      
    case LV_CONV_WRITING_HEADER:
      /* Determine the frame limit for this specific part */
      if (handle->part_num < handle->total_parts)
      {
        handle->cur_frames = handle->parcial_frames;
      }
      else
      {
        handle->cur_frames = handle->total - (handle->parcial_frames * (handle->part_num - 1));
      }    
    
      /* prepare a output file name with same name as src file, */
      /* but in the dest path */
      char name[256] = {0};
      strcpy(name, handle->src_name);
      pstr = strrchr (name, '.'); * pstr = '\0';
      if (handle->total_parts == 1)
      {
        if (handle->bitrate_limit_en)
        {
          snprintf (str, sizeof(str), "%s\\%s_%dx%d,%d,%d,%d.jmv", 
                   handle->dst_path, name, 
                   handle->frame_width, handle->frame_height, 
                   handle->frame_rate, handle->frame_quality,
                   handle->bitrate_limit);
        }
        else
        {
          snprintf (str, sizeof(str), "%s\\%s_%dx%d,%d,%d.jmv", 
                   handle->dst_path, name, 
                   handle->frame_width, handle->frame_height, 
                   handle->frame_rate, handle->frame_quality);  
        }
      }
      else
      {
        if (handle->bitrate_limit_en)
        {
          snprintf (str, sizeof(str), "%s\\%d_%s_%dx%d,%d,%d,%d.jmv", 
                   handle->dst_path, handle->part_num, name, 
                   handle->frame_width, handle->frame_height, 
                   handle->frame_rate, handle->frame_quality,
                   handle->bitrate_limit);
        }
        else
        {
          snprintf (str, sizeof(str), "%s\\%d_%s_%dx%d,%d,%d.jmv", 
                   handle->dst_path, handle->part_num, name, 
                   handle->frame_width, handle->frame_height, 
                   handle->frame_rate, handle->frame_quality);  
        }        
      }

      /* create and open output file */
      handle->dst_file = fopen (str, "wb");
      if (!handle->dst_file)
      {
        sprintf (handle->err_msg, "Could not open destination file %s\n",
                 str);
        handle->err = LV_CONV_ERR; lv_conv_conv_task_kill(handle); return;
      }    
    
      /* write file header */
      handle->fileheader.frame_width = handle->frame_width;
      handle->fileheader.frame_height = handle->frame_height;
      handle->fileheader.frame_rate = handle->frame_rate;
      handle->fileheader.frame_jpeg = 1;
      handle->fileheader.frame_nb = handle->cur_frames;
      handle->fileheader.frame_vect_size = ceil((double) (handle->cur_frames * sizeof(uint32_t)) / 512) * 512;
      handle->fileheader.audio_numchannels = 2;
      handle->fileheader.audio_bytedepth = 2;
      handle->fileheader.audio_samplerate = handle->audio_samplerate;
      handle->fileheader.audio_byterate = 2 * 2 * handle->audio_samplerate;
      handle->fileheader.audio_totalsize = fsize(handle->audio_file);
      fwrite (&handle->fileheader, 1, sizeof(handle->fileheader), handle->dst_file);
      
      /* write frame size vector to output file */
      start_idx = handle->parcial_frames * (handle->part_num - 1);
      fwrite (&handle->frame_size_buf[start_idx], 1, handle->cur_frames * sizeof(uint32_t), handle->dst_file);
      
      /* write zeros until complete sector */
      pad_size = handle->fileheader.frame_vect_size - (handle->cur_frames * sizeof(uint32_t));
      if (pad_size > 0)
      {
        fwrite(zero_pad, 1, pad_size, handle->dst_file);
      }
      
      handle->audio_size = handle->fileheader.audio_byterate / handle->fileheader.frame_rate;
      handle->image_size = handle->fileheader.frame_maxsize;
      handle->audio_buf = (uint8_t *)malloc(handle->audio_size);
      handle->image_buf = (uint8_t *)malloc(handle->image_size); 
      
      handle->count = start_idx;
      handle->state = LV_CONV_PACKING;
      break;
    
    case LV_CONV_PACKING:      
      file_size = handle->frame_size_buf[handle->count];
      
      /* write audio data to output file */
      fread(handle->audio_buf, 1, handle->audio_size, handle->audio_file);
      fwrite(handle->audio_buf, 1, handle->audio_size, handle->dst_file);
      
      /* write picture frame to output file */
      fread(handle->image_buf, 1, (size_t)file_size, handle->video_file);
      fwrite(handle->image_buf, 1, (size_t)file_size, handle->dst_file);
      
      handle->count++;
      end_idx = (handle->parcial_frames * (handle->part_num - 1)) + handle->cur_frames;
      if (handle->count >= end_idx)
      {
        fclose(handle->dst_file);
        LV_CONV_FREE(handle->audio_buf);
        LV_CONV_FREE(handle->image_buf);        
        
        if (handle->part_num < handle->total_parts)
        {
          handle->part_num++;
          handle->state = LV_CONV_WRITING_HEADER;
        }
        else
        {
          ret_read_frame = -1;
        }
      }
      break;
  }

  /* exit condition */
  if (ret_read_frame < 0)
  {    
    /* cleanup */
    lv_conv_conv_task_cleanup(handle);    
    
    if (handle->file_count == handle->file_total)
    {
      lv_conv_conv_task_kill(handle);
    }
  }  
}

static void lv_conv_start_tab_create(lv_obj_t * parent)
{
  lv_obj_t * h1, * h2, * h3;
  lv_obj_t * btn;
  lv_obj_t * label;
  lv_coord_t grid_w = lv_page_get_width_grid(parent, 1, 1);
  lv_coord_t pad = lv_obj_get_style_pad_inner(parent, LV_PAGE_PART_SCROLLABLE);
  
  lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY_TOP);
  
  h1 = lv_cont_create(parent, NULL);
  lv_cont_set_fit2(h1, LV_FIT_NONE, LV_FIT_TIGHT);
  lv_obj_set_width(h1, grid_w);
  lv_cont_set_layout(h1, LV_LAYOUT_ROW_TOP);
  lv_obj_add_style(h1, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(h1, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Input files");
  
  h2 = lv_cont_create(h1, NULL);
  lv_cont_set_fit2(h2, LV_FIT_TIGHT, LV_FIT_TIGHT);
  lv_cont_set_layout(h2, LV_LAYOUT_COLUMN_MID);
  lv_obj_add_style(h2, LV_CONT_PART_MAIN, &style_hide);
  
  btn = lv_btn_create(h2, NULL);  
  label = lv_label_create(btn, NULL);
  lv_label_set_text(label, "Add");
  lv_obj_set_event_cb(btn, lv_conv_btn_event_cb);
  
  btn = lv_btn_create(h2, NULL);  
  label = lv_label_create(btn, NULL);
  lv_label_set_text(label, "Remove");
  lv_obj_set_event_cb(btn, lv_conv_btn_event_cb);
  
  list_files = lv_list_create(h1, NULL);
  lv_obj_set_size(list_files, lv_obj_get_width(h1) - lv_obj_get_width(h2) - pad * 3, 
                  lv_obj_get_height(h2));
  
  h3 = lv_cont_create(parent, NULL);
  lv_cont_set_fit2(h3, LV_FIT_NONE, LV_FIT_TIGHT);
  lv_obj_set_width(h3, grid_w);
  lv_cont_set_layout(h3, LV_LAYOUT_ROW_TOP);
  lv_obj_add_style(h3, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(h3, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Output folder");
  
  btn = lv_btn_create(h3, NULL);  
  label = lv_label_create(btn, NULL);
  lv_label_set_text(label, "Path");
  lv_obj_set_event_cb(btn, lv_conv_btn_event_cb);
  
  ta = lv_textarea_create(h3, NULL);
  lv_obj_set_size(ta, lv_obj_get_width(h1) - lv_obj_get_width(btn) - pad * 3, 
                  lv_obj_get_height(btn));
  lv_textarea_set_text(ta, "");
  lv_textarea_set_placeholder_text(ta, "Output directory path");
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_cursor_hidden(ta, true);
  
  btn = lv_btn_create(parent, NULL);  
  label = lv_label_create(btn, NULL);
  lv_label_set_text(label, "Start conversion");
  lv_obj_set_event_cb(btn, lv_conv_btn_event_cb);
}

static void lv_conv_settings_tab_create(lv_obj_t * parent)
{
  lv_obj_t * h;
  lv_coord_t grid_w = lv_page_get_width_grid(parent, 2, 1);
  lv_coord_t grid_h = lv_page_get_height_grid(parent, 1, 1);
  lv_coord_t pad = lv_obj_get_style_pad_inner(parent, LV_PAGE_PART_SCROLLABLE);

  lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY_TOP);

  h = lv_cont_create(parent, NULL);
  lv_obj_set_size(h, grid_w, grid_h);
  lv_cont_set_layout(h, LV_LAYOUT_COLUMN_LEFT);
  lv_obj_add_style(h, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(h, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Video attributes");

  dd_res = lv_dropdown_create(h, NULL);
  lv_obj_set_width(dd_res, grid_w - pad * 2);
  lv_obj_align(dd_res, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_obj_add_style(dd_res, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(dd_res, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Resolution");
  lv_obj_set_event_cb(dd_res, lv_conv_dropdown_event_cb);
  lv_dropdown_set_options(dd_res, "320x240\n" "480x272\n" "640x480\n" "720x480\n" "800x480");
  lv_dropdown_set_selected(dd_res, 4);

  dd_framerate = lv_dropdown_create(h, NULL);
  lv_obj_set_width(dd_framerate, grid_w - pad * 2);
  lv_obj_align(dd_framerate, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_obj_add_style(dd_framerate, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(dd_framerate, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Frame rate");
  lv_obj_set_event_cb(dd_framerate, lv_conv_dropdown_event_cb);
  lv_dropdown_set_options(dd_framerate, "15\n" "20\n" "25\n" "30");
  lv_dropdown_set_selected(dd_framerate, 2);
  
  dd_quality = lv_dropdown_create(h, NULL);
  lv_obj_set_width(dd_quality, grid_w - pad * 2);
  lv_obj_align(dd_quality, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_obj_add_style(dd_quality, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(dd_quality, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Frame quality");
  lv_obj_set_event_cb(dd_quality, lv_conv_dropdown_event_cb);
  lv_dropdown_set_options(dd_quality, "best\n" "high\n" "medium\n" "low");
  lv_dropdown_set_selected(dd_quality, 1);
  
  cb_bitrate = lv_checkbox_create(h, NULL);
  lv_obj_align(cb_bitrate, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_checkbox_set_text(cb_bitrate, "Enable bitrate limit");
  lv_obj_set_event_cb(cb_bitrate, lv_conv_checkbox_event_cb);
  
  dd_bitrate = lv_dropdown_create(h, NULL);
  lv_obj_set_width(dd_bitrate, grid_w - pad * 2);
  lv_obj_align(dd_bitrate, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_obj_add_style(dd_bitrate, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(dd_bitrate, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Bitrate");
  lv_obj_set_event_cb(dd_bitrate, lv_conv_dropdown_event_cb);
  lv_dropdown_set_options(dd_bitrate, "1 Mbit/s\n" "2 Mbit/s\n" "5 Mbit/s\n" "8 Mbit/s\n" "10 Mbit/s");
  lv_dropdown_set_selected(dd_bitrate, 2);  

  h = lv_cont_create(parent, NULL);
  lv_obj_set_size(h, grid_w, grid_h);
  lv_cont_set_layout(h, LV_LAYOUT_COLUMN_LEFT);
  lv_obj_add_style(h, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(h, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Audio attributes");

  dd_audiorate = lv_dropdown_create(h, NULL);
  lv_obj_set_width(dd_audiorate, grid_w - pad * 2);
  lv_obj_align(dd_audiorate, h, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  lv_obj_add_style(dd_audiorate, LV_CONT_PART_MAIN, &style);
  lv_obj_set_style_local_value_str(dd_audiorate, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, "Sample rate");
  lv_obj_set_event_cb(dd_audiorate, lv_conv_dropdown_event_cb);
  lv_dropdown_set_options(dd_audiorate, "32000\n" "48000");
  lv_dropdown_set_selected(dd_audiorate, 3);
}

static lv_obj_t * lv_conv_mbox_create(lv_obj_t * parent, const char * txt, const char ** str, lv_event_cb_t event_cb)
{
  lv_obj_t * m = lv_msgbox_create(parent, NULL);
  lv_msgbox_set_text(m, txt);
  lv_msgbox_add_btns(m, str);
  lv_obj_set_event_cb(m, event_cb);
  lv_obj_align(m, NULL, LV_ALIGN_CENTER, 0, 0);

  return m;
}

static void lv_conv_btn_event_cb(lv_obj_t * obj, lv_event_t e)
{
  char * pstr;
  lv_obj_t * btn;
  lv_conv_obj_t tobj = {0};
  
  if (h_bar == NULL)
  {
    if (e == LV_EVENT_CLICKED)
    {
      pstr = (char *) lv_list_get_btn_text(obj);
      
      if (strcmp(pstr, "Add") == 0)
      {
        if (lv_conv_file_get(&tobj) == 0)
        {
          btn = lv_list_add_btn(list_files, LV_SYMBOL_VIDEO, tobj.name);
          lv_obj_set_click(btn, false);
          
          lv_conv_queue_put(&hconv.head, &hconv.tail, &tobj);
          
          hconv.file_total++;
        }
      }
      else if (strcmp(pstr, "Remove") == 0)
      {
        lv_list_remove(list_files, 0);
        
        if (hconv.head != NULL)
        {
          free(lv_conv_queue_get(&hconv.head, &hconv.tail));
        }
        
        if (hconv.file_total > 0)
        {
          hconv.file_total--;
        }
      }
      else if (strcmp(pstr, "Path") == 0)
      {
        lv_conv_folder_get(hconv.dst_path);
        
        lv_textarea_set_text(ta, hconv.dst_path);
      }
      else if (strcmp(pstr, "Start conversion") == 0)
      {
        lv_conv_file_conv(&hconv);
      }    
    }
  }
}

static void lv_conv_dropdown_event_cb(lv_obj_t * obj, lv_event_t e)
{
  char str[32];
  uint8_t i;
  
  if(e == LV_EVENT_VALUE_CHANGED)
  {
    lv_dropdown_get_selected_str(obj, str, sizeof(str));
    
    for(i = 0; i < DD_OPTS_NUM; i++)
    {
      if (strcmp(&str[0], dd_pstr[i]) == 0)
      {
        if(obj == dd_res)
        {
          hconv.frame_width = dd_opts[i][0];
          hconv.frame_height = dd_opts[i][1];
        }
        else if(obj == dd_framerate)
        {
          hconv.frame_rate = dd_opts[i][0];
          if (hconv.frame_rate == 30 ||\
              hconv.frame_rate == 15)
          {
            hconv.audio_samplerate = 48000;
            lv_dropdown_set_selected(dd_audiorate, 1);
            lv_obj_invalidate(dd_audiorate);
          }          
        }
        else if(obj == dd_quality)
        {
          hconv.frame_quality = dd_opts[i][0];
        }
        else if(obj == dd_bitrate)
        {
          hconv.bitrate_limit = dd_opts[i][0];
        }        
        else if(obj == dd_audiorate)
        {
          hconv.audio_samplerate = dd_opts[i][0];
          if (hconv.audio_samplerate == 32000)
          {
            if (hconv.frame_rate > 25)
            {
              hconv.frame_rate = 25;
              lv_dropdown_set_selected(dd_framerate, 2);
            }
            else if (hconv.frame_rate < 20)
            {
              hconv.frame_rate = 20;
              lv_dropdown_set_selected(dd_framerate, 1);
            }
            lv_obj_invalidate(dd_framerate);
          }          
        }
      }
    }
  }
}

static void lv_conv_conv_btn_event_cb(lv_obj_t * obj, lv_event_t e)
{
  if (e == LV_EVENT_CLICKED)
  {
    lv_conv_conv_task_kill(&hconv);
  }  
}

static void lv_conv_mbox_err_btn_event_cb(lv_obj_t * btn, lv_event_t e)
{
  char * pstr;

  if (e == LV_EVENT_CLICKED)
  {
    pstr = (char *) lv_msgbox_get_active_btn_text(mbox_err);
    if (!pstr) return;

    if (strcmp(pstr, "Ok") == 0)
    {
      LV_CONV_OBJ_DEL(mbox_err)
    }
  }
}

static void lv_conv_checkbox_event_cb(lv_obj_t * obj, lv_event_t e)
{
  if(e == LV_EVENT_VALUE_CHANGED)
  {
    if(obj == cb_bitrate)
    {
      if(lv_checkbox_is_checked(obj))
      {
        hconv.bitrate_limit_en = 1;
      }
      else
      {
        hconv.bitrate_limit_en = 0;
      }
    }    
  }
}

static int lv_conv_file_get(lv_conv_obj_t * obj)
{
  char raw_path[256] = {0};
  
  uint8_t i;
  OPENFILENAME ofn = {0};
  HWND hwnd = GetActiveWindow();

  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFile = raw_path;
  ofn.nMaxFile = 256;
  ofn.lpstrFilter = "Video files\0*.mp4\0";

  if (GetOpenFileName(&ofn))
  {
    ansi_to_utf8(raw_path, obj->full_path, sizeof(obj->full_path));
    
    for (i = strlen (obj->full_path) - 1; i; i--)
    {
      if (obj->full_path[i] == '\\' || obj->full_path[i] == '/')
      {
        memcpy(&obj->name, &obj->full_path[i + 1], strlen(&obj->full_path[i + 1]));
        return 0;
      }
    }
  }
  
  return -1;
}

static void lv_conv_folder_get(char * path)
{
  HWND hwnd = GetActiveWindow();
  BROWSEINFO binfo = {0};

  binfo.hwndOwner = hwnd;
  binfo.pszDisplayName = path;
  binfo.lpszTitle = "Select output directory";
  LPITEMIDLIST id_list = SHBrowseForFolder(&binfo);

  if (id_list != NULL)
  {
    SHGetPathFromIDList(id_list, path);
  }
}

static void lv_conv_file_conv(lv_conv_handle_t * handle)
{  
  h_bar = lv_cont_create(lv_scr_act(), NULL);
  lv_cont_set_layout(h_bar, LV_LAYOUT_PRETTY_MID);
  lv_cont_set_fit2(h_bar, LV_FIT_NONE, LV_FIT_TIGHT);
  lv_obj_set_width(h_bar, lv_page_get_width_grid(t1, 1, 1));

  bar = lv_bar_create(h_bar, NULL);
  lv_obj_set_width(bar, lv_obj_get_width_fit(h_bar));

  lv_obj_t * btn = lv_btn_create(h_bar, NULL);
  lv_obj_t * label = lv_label_create(btn, NULL);
  lv_label_set_text(label ,"Cancel");
  lv_btn_set_fit2(btn, LV_FIT_TIGHT, LV_FIT_TIGHT);
  lv_obj_set_width(btn, lv_obj_get_width_fit(h_bar));
  lv_obj_set_event_cb(btn, lv_conv_conv_btn_event_cb);
  
  lv_obj_align(h_bar, NULL, LV_ALIGN_CENTER, 0, 0);
  
  bar_task = lv_task_create(lv_conv_bar_task, 100, LV_TASK_PRIO_LOW, handle);

  conv_main_state = LV_CONV_MAIN_RUN;
}

static uint64_t fsize(FILE *fp)
{
  uint64_t prev, sz;
  
  prev = _ftelli64(fp);
  _fseeki64(fp, 0, SEEK_END);
  sz = _ftelli64(fp);
  _fseeki64(fp, prev, SEEK_SET);
  
  return sz;
}

static void remove_dir(char *path)
{
  DIR *dir;
  struct dirent *entry;
  char full_name[256] = {0};

  dir = opendir(path);

  do
  {
    entry = readdir(dir);
    if (entry)
    {
      snprintf(full_name, sizeof(full_name), "%s\\%s", path, entry->d_name);
      remove(full_name);
    }
  }
  while(entry);

  if (dir)
  {
    closedir(dir);
    rmdir(path);
  }
}

static void ansi_to_utf8(const char *ansi_src, char *utf8_dst, size_t dst_size)
{
  wchar_t wbuf[512] = {0};
  
  MultiByteToWideChar(CP_ACP, 0, ansi_src, -1, wbuf, 512);
  
  WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8_dst, (int)dst_size, NULL, NULL);  
}

const char *av_get_media_type_string(enum AVMediaType media_type)
{
    switch (media_type) {
    case AVMEDIA_TYPE_VIDEO:      return "video";
    case AVMEDIA_TYPE_AUDIO:      return "audio";
    case AVMEDIA_TYPE_DATA:       return "data";
    case AVMEDIA_TYPE_SUBTITLE:   return "subtitle";
    case AVMEDIA_TYPE_ATTACHMENT: return "attachment";
    default:                      return NULL;
    }
}

static int output_pkt(AVCodecContext *ctx, AVPacket *pkt)
{
  uint32_t pad_size;
  
  if (ctx->codec->type == AVMEDIA_TYPE_VIDEO)
  {
    hconv.frame_size_buf[hconv.count] = ceil((double) pkt->size / 512) * 512;    
    if (hconv.frame_size_buf[hconv.count] > hconv.fileheader.frame_maxsize)
    {
      hconv.fileheader.frame_maxsize = hconv.frame_size_buf[hconv.count];
    }

    fwrite(pkt->data, 1, pkt->size, hconv.video_file);
    
    pad_size = hconv.frame_size_buf[hconv.count] - pkt->size;
    if (pad_size > 0)
    {
      fwrite(zero_pad, 1, pad_size, hconv.video_file);
    }
    
    /* Dynamic mjpeg bitrate control */
    if (hconv.bitrate_limit_en)
    {
      /* Add the actual encoded size and subtract the constant leakage flow rate */
      hconv.libav.vbv_ctrl.buf_fill += pkt->size - hconv.libav.vbv_ctrl.max_frame_bytes;
      
      /* Limit the minimum buffer level to 0 */
      if (hconv.libav.vbv_ctrl.buf_fill < 0) { 
        hconv.libav.vbv_ctrl.buf_fill = 0;
      }

      /* Calculate the buffer saturation (0.0 to 1.0) */
      float fill_rate = (float)hconv.libav.vbv_ctrl.buf_fill / (float)hconv.libav.vbv_ctrl.buf_size;

      /* Readjust cur_quality proportionally to saturation */
      if (fill_rate > 0.8f) {
        hconv.libav.vbv_ctrl.cur_quality += 3;
      } else if (fill_rate > 0.4f) {
        hconv.libav.vbv_ctrl.cur_quality += 1;
      } else if (fill_rate < 0.2f && hconv.libav.vbv_ctrl.cur_quality > hconv.frame_quality) {
        hconv.libav.vbv_ctrl.cur_quality -= 1;
      }

      /* Valid range for JPEG quantizers in FFmpeg (2 = best, 31 = worst) */
      if (hconv.libav.vbv_ctrl.cur_quality < hconv.frame_quality) {
        hconv.libav.vbv_ctrl.cur_quality = hconv.frame_quality;
      } else if (hconv.libav.vbv_ctrl.cur_quality > 31) {
        hconv.libav.vbv_ctrl.cur_quality = 31;
      }

      /* Apply the new qscale to the encoder context for the next frame */
      ctx->qmin = hconv.libav.vbv_ctrl.cur_quality;
      ctx->qmax = hconv.libav.vbv_ctrl.cur_quality;
    }    
    
    hconv.count++;
  }
  
  return 0;
}

static int output_frame(AVCodecContext *ctx, AVFrame *frame)
{
  int ret = 0;
  lv_conv_libav_t *libav = &hconv.libav;
  lv_conv_libav_ctx_t *video_enc = &libav->video_enc;
  lv_conv_libav_flt_t *video_flt = &libav->video_flt;
  
  if (ctx->codec->type == AVMEDIA_TYPE_VIDEO)
  {
    /* Send the frame to the input of the filtergraph. */
    ret = av_buffersrc_write_frame(video_flt->filter_src, frame);
    if (ret < 0)
      return ret;
    
    /* Get all the filtered output that is available. */
    while (ret >= 0)
    {
      ret = av_buffersink_get_frame(video_flt->filter_sink, libav->flt_objs.frame);
      if (ret < 0)
      {
        /* those two return values are special and mean there is no output */
        /* frame available, but there were no errors during decoding */
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
          ret = 0;
          
        return ret;
      }
      
      /* convert to mjpeg */
      ret = encode (video_enc->ctx, libav->enc_objs.pkt, libav->flt_objs.frame, hconv.err_msg);
      if (ret < 0)
        return ret;
      
      av_frame_unref(libav->flt_objs.frame);      
    }
  }
  else if (ctx->codec->type == AVMEDIA_TYPE_AUDIO)
  {
    /* resample audio */
    ret = resample (&libav->swr_ctx, libav->avr_objs.frame, frame, hconv.err_msg);
    if (ret < 0)
      return ret;
  }
  
  return 0;
}

static int output_sample(uint8_t *out_data, int buff_size)
{  
  fwrite(out_data, 1, buff_size, hconv.audio_file);
  
  return 0;
}

static int encode (AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame,
                   char *err_msg)
{
  int ret = 0;

  /* submit the frame to the decoder */
  ret = avcodec_send_frame (ctx, frame);
  if (ret < 0)
  {
    sprintf (err_msg, "Error submitting a frame for decoding (%s)\n",
             av_err2str (ret));
    return ret;
  }

  /* get all the available packets from the encoder */
  while (ret >= 0)
  {
    ret = avcodec_receive_packet (ctx, pkt);
    if (ret < 0)
    {
      /* those two return values are special and mean there is no output */
      /* frame available, but there were no errors during decoding */
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        return 0;

      sprintf (err_msg, "Error during encoding (%s)\n", av_err2str (ret));
      return ret;
    }

    ret = output_pkt(ctx, pkt);

    av_packet_unref(pkt);
    if (ret < 0)
      return ret;
  }

  return 0;
}

static int decode (AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame,
                   char *err_msg)
{
  int ret = 0;

  /* submit the packet to the decoder */
  ret = avcodec_send_packet (ctx, pkt);
  if (ret < 0)
  {
    sprintf (err_msg, "Error submitting a packet for decoding (%s)\n",
             av_err2str (ret));
    return ret;
  }

  /* get all the available frames from the decoder */
  while (ret >= 0)
  {
    ret = avcodec_receive_frame (ctx, frame);
    if (ret < 0)
    {
      /* those two return values are special and mean there is no output */
      /* frame available, but there were no errors during decoding */
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        return 0;

      sprintf (err_msg, "Error during decoding (%s)\n", av_err2str (ret));
      return ret;
    }

    ret = output_frame(ctx, frame);

    av_frame_unref(frame);
    if (ret < 0)
      return ret;
  }

  return 0;
}

static int resample(SwrContext **ctx, AVFrame *out_frame, AVFrame *in_frame,
                    char *err_msg)
{
  int ret = 0, out_linesize, out_samples, dst_bufsize; 
  uint8_t **in_data = (uint8_t **)in_frame->data;
  uint8_t *out_data = NULL;

  out_samples = swr_get_out_samples(*ctx, in_frame->nb_samples);

  if((ret = av_samples_alloc(&out_data, &out_linesize, 2, out_samples, AV_SAMPLE_FMT_S16, 1)) < 0)
  {
    sprintf (err_msg, "Alloc samples not succeeded (%s)\n", av_err2str (ret));
    return ret;
  }

  out_samples = swr_convert(*ctx, &out_data, out_samples, (const uint8_t **)in_data, in_frame->nb_samples);
  dst_bufsize = av_samples_get_buffer_size(&out_linesize, 2, out_samples, AV_SAMPLE_FMT_S16, 1);

  ret = output_sample(out_data, dst_bufsize);

  av_freep(&out_data);
  return ret;
}

static int open_encoder_context (enum AVCodecID id, AVCodecContext **ctx, 
                                 enum AVMediaType type, 
                                 enum AVPixelFormat pix_fmt,
                                 int frame_rate, int global_quality, 
                                 int width, int height,
                                 enum AVSampleFormat sample_fmt,
                                 int sample_rate, int channels,
                                 char *err_msg)
{
  int ret;
  AVCodec *enc = NULL;
  AVDictionary *opts = NULL;  
  
  /* find encoder for the stream */
  enc = avcodec_find_encoder (id);
  if (!enc)
  {
    sprintf (err_msg, "Failed to find %s codec\n",
               av_get_media_type_string (type));
    return AVERROR(EINVAL);
  }

  /* allocate a codec context for the encoder */
  *ctx = avcodec_alloc_context3 (enc);
  if (!*ctx)
  {
    sprintf (err_msg, "Failed to allocate the %s codec context\n",
               av_get_media_type_string (type));
    return AVERROR(ENOMEM);
  }
  
  if (type == AVMEDIA_TYPE_VIDEO)
  {
    /* video param */
    (*ctx)->time_base = (AVRational) {1, frame_rate};
    (*ctx)->framerate = (AVRational) {frame_rate, 1};  
    (*ctx)->width = width;
    (*ctx)->height = height;
    (*ctx)->pix_fmt = pix_fmt;
    
    /* Base quantizer configuration */
    (*ctx)->qmin = global_quality;
    (*ctx)->qmax = global_quality;
    (*ctx)->flags |= AV_CODEC_FLAG_QSCALE;
  }
  else if (type == AVMEDIA_TYPE_AUDIO)
  {
    /* audio param */
    (*ctx)->sample_rate = sample_rate;
    (*ctx)->sample_fmt = sample_fmt;
    
    av_channel_layout_default(&(*ctx)->ch_layout, channels);
  }  
  
  /* init the encoder */
  if ((ret = avcodec_open2 (*ctx, enc, &opts)) < 0)
  {
    sprintf (err_msg, "Failed to open %s codec\n",
             av_get_media_type_string (type));
    return ret;
  }

  return 0;
}

static int open_decoder_context (int *stream_idx, AVCodecContext **ctx,
                                 AVFormatContext *fmt_ctx, 
                                 enum AVMediaType type,
                                 char *src_filename, 
                                 char *err_msg)
{
  int ret, stream_index;
  AVStream *st;
  AVCodec *dec = NULL;
  AVDictionary *opts = NULL;

  ret = av_find_best_stream (fmt_ctx, type, -1, -1, NULL, 0);
  if (ret < 0)
  {
    sprintf (err_msg, "Could not find %s stream in input file '%s'\n",
             av_get_media_type_string (type), src_filename);
    return ret;
  }
  else
  {
    stream_index = ret;
    st = fmt_ctx->streams[stream_index];

    /* find decoder for the stream */
    dec = avcodec_find_decoder (st->codecpar->codec_id);
    if (!dec)
    {
      sprintf (err_msg, "Failed to find %s codec\n",
               av_get_media_type_string (type));
      return AVERROR(EINVAL);
    }

    /* allocate a codec context for the decoder */
    *ctx = avcodec_alloc_context3 (dec);
    if (!*ctx)
    {
      sprintf (err_msg, "Failed to allocate the %s codec context\n",
               av_get_media_type_string (type));
      return AVERROR(ENOMEM);
    }

    /* copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context (*ctx, st->codecpar)) < 0)
    {
      sprintf (err_msg,
               "Failed to copy %s codec parameters to decoder context\n",
               av_get_media_type_string (type));
      return ret;
    }
    
    /* init the decoders */
    if ((ret = avcodec_open2 (*ctx, dec, &opts)) < 0)
    {
      sprintf (err_msg, "Failed to open %s codec\n",
               av_get_media_type_string (type));
      return ret;
    }
    *stream_idx = stream_index;
  }

  return 0;
}

static int open_resampler_context (SwrContext **ctx, uint64_t in_channel_layout_mask,
                                   enum AVSampleFormat in_sample_fmt, int in_sample_rate,
                                   uint64_t out_channel_layout_mask, enum AVSampleFormat out_sample_fmt,
                                   int out_sample_rate, char *err_msg)
{
  int ret;
  AVChannelLayout in_ch_layout = {0};
  AVChannelLayout out_ch_layout = {0};

  av_channel_layout_from_mask(&in_ch_layout, in_channel_layout_mask);
  av_channel_layout_from_mask(&out_ch_layout, out_channel_layout_mask);

  /* create resampler context */
  ret = swr_alloc_set_opts2(ctx,
                            &out_ch_layout,
                            out_sample_fmt,
                            out_sample_rate,
                            &in_ch_layout,
                            in_sample_fmt,
                            in_sample_rate,
                            0, NULL);

  av_channel_layout_uninit(&in_ch_layout);
  av_channel_layout_uninit(&out_ch_layout);

  if (ret < 0 || !*ctx)
  {
    sprintf (err_msg, "Could not allocate/configure resampler context (%s)\n", av_err2str(ret));
    return ret < 0 ? ret : AVERROR(ENOMEM);
  }  
  
  /* init resampler context */
  if ((ret = swr_init(*ctx)) < 0)
  {
    sprintf (err_msg, "Could not open resampler context buffer\n");
    return ret;        
  }
  
  return 0;
}

static int init_filter_graph(enum AVMediaType type,
                             AVFilterGraph **graph, 
                             AVFilterContext **src,
                             AVFilterContext **sink,
                             char *src_option_str,
                             char *filter_option_str,
                             char *err_msg)
{
  AVFilterGraph *filter_graph;
  AVFilterContext *buffersrc_ctx;
  const AVFilter  *buffersrc;
  AVFilterContext *buffersink_ctx;
  const AVFilter  *buffersink;
  
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  int err;  
    
  if (!outputs || !inputs) {
    sprintf(err_msg, "Unable to allocate inout structs.\n");
    return AVERROR(ENOMEM);
  }

  /* Create a new filtergraph, which will contain all the filters. */
  filter_graph = avfilter_graph_alloc();
  if (!filter_graph) {
    sprintf (err_msg, "Unable to create filter graph.\n");
    return AVERROR(ENOMEM);
  }
  
  if (type == AVMEDIA_TYPE_VIDEO) {
    buffersrc  = avfilter_get_by_name("buffer");
    buffersink = avfilter_get_by_name("buffersink");
  } else {
    buffersrc  = avfilter_get_by_name("abuffer");
    buffersink = avfilter_get_by_name("abuffersink");
  }
  
  if (!buffersrc || !buffersink) {
    sprintf (err_msg, "Could not find the buffersrc/sink filter.\n");
    return AVERROR_FILTER_NOT_FOUND;
  }

  buffersrc_ctx = avfilter_graph_alloc_filter(filter_graph, buffersrc, "in");
  if (!buffersrc_ctx) {
    sprintf (err_msg, "Could not allocate the buffersrc instance.\n");
    return AVERROR(ENOMEM);
  }
  
  err = avfilter_init_str(buffersrc_ctx, src_option_str);
  if (err < 0) {
    sprintf (err_msg, "Could not initialize the buffersrc filter.\n");
    return err;
  }
  
  buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
  if (!buffersink_ctx) {
    sprintf (err_msg, "Could not allocate the buffersink instance.\n");
    return  AVERROR(ENOMEM);
  }

  err = avfilter_init_str(buffersink_ctx, NULL);
  if (err < 0) {
    sprintf (err_msg, "Could not initialize the buffersink instance.\n");
    return err;
  }
  
  /* Endpoints for the filter graph. 
   * outputs: The output of the buffersrc filter 
   * inputs:  The input of the buffersink filter */
  outputs->name       = av_strdup("in");
  outputs->filter_ctx = buffersrc_ctx;
  outputs->pad_idx    = 0;
  outputs->next       = NULL;

  inputs->name       = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx;
  inputs->pad_idx    = 0;
  inputs->next       = NULL;

  /* Analyze filter_option_str and link automatically */
  err = avfilter_graph_parse(filter_graph, filter_option_str,
                             inputs, outputs, NULL);
  if (err < 0) {
    sprintf(err_msg, "Error parsing the filter graph: %s\n", av_err2str(err));
    return err;
  }
  
  /* Configure the graph. */
  err = avfilter_graph_config(filter_graph, NULL);
  if (err < 0) {
    sprintf(err_msg, "Error configuring the filter graph\n");
    return err;
  }  

  *graph = filter_graph;
  *src   = buffersrc_ctx;
  *sink  = buffersink_ctx;
      
  return 0;  
}

static void lv_conv_queue_put(lv_conv_queue_t ** head, lv_conv_queue_t ** tail, lv_conv_obj_t * data)
{
  lv_conv_queue_t * q = (lv_conv_queue_t *) malloc(sizeof(lv_conv_queue_t));
  q->data = *data;
  q->next = NULL;

  if(*head == NULL)
  {
    *head = *tail = q;
  }
  else
  {
    (*tail)->next = q;
    *tail = q;
  }  
}

static lv_conv_queue_t * lv_conv_queue_get(lv_conv_queue_t ** head, lv_conv_queue_t ** tail)
{
  if (*head == NULL)
  {
    return NULL;
  }

  lv_conv_queue_t * q = *head;
  *head = (*head)->next;
  q->next = NULL;

  if (*head == NULL)
  {
    *tail = NULL;
  }

  return q;  
}

static void lv_conv_conv_task_cleanup(lv_conv_handle_t * handle)
{
  char temp_path[256] = {0};
  lv_conv_libav_t * libav = &handle->libav;
  lv_conv_libav_ctx_t * video_enc = &libav->video_enc;
  lv_conv_libav_ctx_t * video_dec = &libav->video_dec;
  lv_conv_libav_ctx_t * audio_dec = &libav->audio_dec;
  lv_conv_libav_flt_t * video_flt = &libav->video_flt;

  if (video_enc->ctx)
  {
    avcodec_free_context(&video_enc->ctx); video_enc->ctx = NULL;
  }
  if (video_dec->ctx)
  {
    avcodec_free_context(&video_dec->ctx); video_dec->ctx = NULL;
  }
  if (audio_dec->ctx)
  {
    avcodec_free_context(&audio_dec->ctx); audio_dec->ctx = NULL;
  }
  if (video_flt->filter_graph)
  {
    avfilter_graph_free(&video_flt->filter_graph);
  }  
  
  if (libav->fmt_ctx)
  {
    avformat_close_input(&libav->fmt_ctx); libav->fmt_ctx = NULL;
  }
  if (libav->swr_ctx)
  {
    swr_free(&libav->swr_ctx); libav->swr_ctx = NULL;
  }
  if (libav->enc_objs.pkt)
  {
    av_packet_free(&libav->enc_objs.pkt); libav->enc_objs.pkt = NULL;
  }
  if (libav->enc_objs.frame)
  {
    av_frame_free(&libav->enc_objs.frame); libav->enc_objs.frame = NULL;
  }
  if (libav->dec_objs.pkt)
  {
    av_packet_free(&libav->dec_objs.pkt); libav->dec_objs.pkt = NULL;
  }
  if (libav->dec_objs.frame)
  {
    av_frame_free(&libav->dec_objs.frame); libav->dec_objs.frame = NULL;
  }
  if (libav->avr_objs.pkt)
  {
    av_packet_free(&libav->avr_objs.pkt); libav->avr_objs.pkt = NULL;
  }
  if (libav->avr_objs.frame)
  {
    av_frame_free(&libav->avr_objs.frame); libav->avr_objs.frame = NULL;
  }
  if (libav->flt_objs.pkt)
  {
    av_packet_free(&libav->flt_objs.pkt); libav->flt_objs.pkt = NULL;
  }
  if (libav->flt_objs.frame)
  {
    av_frame_free(&libav->flt_objs.frame); libav->flt_objs.frame = NULL;
  }  
  
  LV_CONV_FILE_CLOSE(handle->dst_file)
  LV_CONV_FILE_CLOSE(handle->video_file)
  LV_CONV_FILE_CLOSE(handle->audio_file)
  
  LV_CONV_FREE(handle->image_buf)
  LV_CONV_FREE(handle->audio_buf)
  LV_CONV_FREE(handle->frame_size_buf)  
  
  if (hconv.dst_path[0] != '\0')
  {
    SetCurrentDirectory(hconv.dst_path);
    
    snprintf(temp_path, sizeof(temp_path), "%s\\temp", hconv.dst_path);
    remove_dir(temp_path);
  }
}

static void lv_conv_conv_task_kill(lv_conv_handle_t * handle)
{
  LV_CONV_TASK_DEL(bar_task)
  LV_CONV_TASK_DEL(conv_task)
  
  lv_conv_conv_task_cleanup(handle);

  while(handle->head != NULL)
  {
    free(lv_conv_queue_get(&handle->head, &handle->tail));
  }

  lv_list_clean(list_files);
  handle->total = handle->count = 0;
  handle->file_total = handle->file_count = 0;

  LV_CONV_OBJ_DEL(h_bar)
  
  conv_main_state = LV_CONV_MAIN_UI;
}

void lv_conv_bar_task(lv_task_t * task)
{
  static char buf[64];
  lv_conv_handle_t * data = task->user_data;

  if (data->state == LV_CONV_TRANSCODING)
  {
    if (data->bitrate_limit_en)
    {
      lv_snprintf(buf, sizeof(buf), "Converting %d  /  %d   frames    %d  /  %d   qscale %d", 
                  data->count, data->total, data->file_count, data->file_total, data->libav.vbv_ctrl.cur_quality);
    }
    else
    {
      lv_snprintf(buf, sizeof(buf), "Converting %d  /  %d   frames    %d  /  %d   qscale %d", 
                  data->count, data->total, data->file_count, data->file_total, data->frame_quality);      
    }
  }
  else if (data->state == LV_CONV_PACKING)
  {
    lv_snprintf(buf, sizeof(buf), "Writing %d  /  %d   frames    %d  /  %d", 
                data->count, data->total, data->file_count, data->file_total);    
  }
  lv_obj_set_style_local_value_str(bar, LV_BAR_PART_BG, LV_STATE_DEFAULT, buf);
  
  int16_t x = (int16_t) ((data->count * 100) / data->total);
  lv_bar_set_value(bar, x, LV_ANIM_OFF);  
}

void lv_conv_mbox_task(lv_task_t * task)
{
  lv_conv_handle_t * handle = task->user_data;

  if (handle->err)
  {
    handle->err = LV_CONV_OK;
    
    if (mbox_err)
    {
      return;
    }
    
    mbox_err = lv_conv_mbox_create(lv_scr_act(), handle->err_msg,
                                   btns00, lv_conv_mbox_err_btn_event_cb);
  }
}
