//Amit Papushe Shely 301384939
//Naor Haimov 311254528
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/mathematics.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <stdbool.h>
#ifdef __MINGW32__
#undef main
#endif

#include <stdio.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (500 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (500 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 1.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20
#define MAX_AUDIO_FRAME_SIZE 192000
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER
#define SINGLE_VIDEO 1
#define MULTIPLE_VIDEO 2

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct VideoPicture {
    SDL_Overlay *bmp;
    int width, height;
    int allocated;
    double pts;
} VideoPicture;

typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    int videoStream, audioStream;
    int av_sync_type;
    double external_clock;
    int64_t external_clock_time;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    double audio_clock;
    AVStream *audio_st;
	bool gracescale;
    PacketQueue audioq;
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AVFrame audio_frame;
    AVStream *video_st;
    PacketQueue videoq;
    int audio_hw_buf_size;
    double audio_diff_cum;
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    double frame_timer;
    double frame_last_pts;
    double frame_last_delay;
    double video_current_pts;
    int64_t video_current_pts_time;
    double video_clock;
    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex *pictq_mutex;
    SDL_cond *pictq_cond;
    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;
    AVIOContext *io_ctx;
    struct SwsContext *sws_ctx;
    char filename[1024];
    int quit;
    int x_pos, y_pos;
    int mainChannel;
} VideoState;

enum {AV_SYNC_AUDIO_MASTER, AV_SYNC_VIDEO_MASTER, AV_SYNC_EXTERNAL_MASTER,};

struct rhythm {
    double FAST,SLOW,NORMAL;
};
struct rhythm rhythm_instance={0.5,2,1};

int is_multiple_video;

double film_speed;
SDL_Surface *screen;

VideoState *global_video_state;

int main_audio;

VideoState* channel_1;
VideoState* channel_2;

AVPacket flush_pkt;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    
    AVPacketList *pkt1;
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
    return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;
    
    SDL_LockMutex(q->mutex);
    
    for (;;) {
        
        if (global_video_state->quit) {
            ret = -1;
            break;
        }
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}
static void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pkt1;
    
    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}
double get_audio_clock(VideoState *is) {
    double pts;
    int hw_buf_size, bytes_per_sec, n;
    
    pts = is->audio_clock;
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    bytes_per_sec = 0;
    n = is->audio_st->codec->channels * 2;
    if (is->audio_st) {
        bytes_per_sec = is->audio_st->codec->sample_rate * n;
    }
    if (bytes_per_sec) {
        pts -= (double) hw_buf_size / bytes_per_sec;
    }
    return pts;
}

double get_video_clock(VideoState *is) {
    double delta;
    
    delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
    return is->video_current_pts + delta;
}
double get_external_clock(VideoState *is) {
    return av_gettime() / 1000000.0;
}

double get_master_clock(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        return get_video_clock(is);
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        return get_audio_clock(is);
    } else {
        return get_external_clock(is);
    }
}
int synchronize_audio(VideoState *is, short *samples, int samples_size,
                      double pts) {
    int n;
    double ref_clock;
    
    n = 2 * is->audio_st->codec->channels;
    
    if (is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int wanted_size, min_size, max_size;
        
        ref_clock = get_master_clock(is);
        diff = get_audio_clock(is) - ref_clock;
        
        if (diff < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff
            + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_size = samples_size
                    + ((int) (diff * is->audio_st->codec->sample_rate)
                       * n);
                    min_size = samples_size
                    * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    max_size = samples_size
                    * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    if (wanted_size < min_size) {
                        wanted_size = min_size;
                    } else if (wanted_size > max_size) {
                        wanted_size = max_size;
                    }
                    if (wanted_size < samples_size) {
                        samples_size = wanted_size;
                    } else if (wanted_size > samples_size) {
                        uint8_t *samples_end, *q;
                        int nb;
                        nb = (samples_size - wanted_size);
                        samples_end = (uint8_t *) samples + samples_size - n;
                        q = samples_end + n;
                        while (nb > 0) {
                            memcpy(q, samples_end, n);
                            q += n;
                            nb -= n;
                        }
                        samples_size = wanted_size;
                    }
                }
            }
        } else {
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }
    return samples_size;
}

int audio_decode_frame(VideoState *is, double *pts_ptr) {
    
    int len1, data_size = 0, n;
    AVPacket *pkt = &is->audio_pkt;
    double pts;
    
    for (;;) {
        while (is->audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(is->audio_st->codec, &is->audio_frame,
                                         &got_frame, pkt);
            if (len1 < 0) {
                is->audio_pkt_size = 0;
                break;
            }
            
            if (got_frame) {
                data_size = is->audio_frame.linesize[0];
                memcpy(is->audio_buf, is->audio_frame.data[0], data_size);
            }
            
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            if (data_size <= 0) {
                continue;
            }
            
            pts = is->audio_clock;
            *pts_ptr = pts;
            n = 2 * is->audio_st->codec->channels;
            is->audio_clock += (double) data_size
            / (double) (n * is->audio_st->codec->sample_rate);
            return data_size;
        }
        if (pkt->data)
            av_free_packet(pkt);
        
        if (is->quit) {
            return -1;
        }
        if (packet_queue_get(&is->audioq, pkt, 1) < 0) {
            return -1;
        }
        
        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(is->audio_st->codec);
            continue;
        }
        
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
        
        if (pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audio_st->time_base) * pkt->pts;
        }
    }
    return 0;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = (VideoState *) userdata;
    if (is->mainChannel != main_audio) {
        if (main_audio == 1)
            is = channel_1;
        if (main_audio == 2)
            is = channel_2;
    }
    int len1, audio_size;
    double pts;
    
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is, &pts);
            if (audio_size < 0) {
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                audio_size = synchronize_audio(is, (int16_t *) is->audio_buf,
                                               audio_size, pts);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}
void video_display(VideoState *is) {
    
    if (is_multiple_video == 0 && main_audio != is->mainChannel) return;
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    int w, h, x, y;
    
    vp = &is->pictq[is->pictq_rindex];
    if (vp->bmp) {
        if (is->video_st->codec->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio)
            * is->video_st->codec->width / is->video_st->codec->height;
        }
        if (aspect_ratio <= 0.0) {
            aspect_ratio = (float) is->video_st->codec->width
            / (float) is->video_st->codec->height;
        }
        h = screen->h;
        w = ((int) rint(h * aspect_ratio)) & -3;
        if (w > screen->w) {
            w = screen->w;
            h = ((int) rint(w / aspect_ratio)) & -3;
        }
        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;
        
        rect.x = x + is->x_pos;
        rect.y = y + is->y_pos;
        if (is_multiple_video) {
            rect.w = w/2;
            rect.h = h/2;
        }
        else {
            rect.w = w;
            rect.h = h;
        }
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
    }
}
void video_refresh_timer(void *userdata) {
    
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;
    
    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            
            vp = &is->pictq[is->pictq_rindex];
            
            is->video_current_pts = vp->pts;
            is->video_current_pts_time = av_gettime();
            
            delay = (vp->pts - is->frame_last_pts)*film_speed;
            if (delay <= 0 || delay >= 1.0*film_speed) {
                delay = (is->frame_last_delay)*film_speed;
            }
            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;
            
            ref_clock = get_audio_clock(is);
            diff = vp->pts - ref_clock;
            
            if (is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
                ref_clock = get_master_clock(is);
                diff = vp->pts - ref_clock;
                
                sync_threshold =
                (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
                if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
                    if (diff <= -sync_threshold) {
                        delay = 0;
                    } else if (diff >= sync_threshold) {
                        delay = 2 * delay;
                    }
                }
            }
            is->frame_timer += delay;
            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            if (actual_delay < 0.010) {
                actual_delay = 0.010;
            }
            schedule_refresh(is, (int) (actual_delay * 1000 + 0.5));
            
            video_display(is);
            
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void alloc_picture(void *userdata) {
    
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;
    
    vp = &is->pictq[is->pictq_windex];
    if (vp->bmp) {
        SDL_FreeYUVOverlay(vp->bmp);
    }
    vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,
                                   is->video_st->codec->height, SDL_YV12_OVERLAY, screen);
    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;
    
    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
    
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts) {
    
    VideoPicture *vp;
    AVPicture pict;
    
    SDL_LockMutex(is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);
    
    if (is->quit)
        return -1;
    
    vp = &is->pictq[is->pictq_windex];
    
    if (!vp->bmp || vp->width != is->video_st->codec->width
        || vp->height != is->video_st->codec->height) {
        SDL_Event event;
        
        vp->allocated = 0;
        
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
        
        SDL_LockMutex(is->pictq_mutex);
        while (!vp->allocated && !is->quit) {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        SDL_UnlockMutex(is->pictq_mutex);
        if (is->quit) {
            return -1;
        }
    }
    
    if (vp->bmp) {
        
        SDL_LockYUVOverlay(vp->bmp);
        
        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];
        
        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];
        
		
        sws_scale(is->sws_ctx, (const uint8_t * const *) pFrame->data,
                  pFrame->linesize, 0, is->video_st->codec->height, pict.data,
                  pict.linesize);
        
		if(is->gracescale){		// Change color to black & white
		
			memset(pict.data[1],128, 480*240);
			memset(pict.data[2],128, 480*240);
		}

        SDL_UnlockYUVOverlay(vp->bmp);
        vp->pts = pts;
        
        if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {
    
    double frame_delay;
    
    if (pts != 0) {
        is->video_clock = pts;
    } else {
        pts = is->video_clock;
    }
    frame_delay = av_q2d(is->video_st->codec->time_base);
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    return pts;
}
uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

int our_get_buffer(struct AVCodecContext *c, AVFrame *pic) {
    int ret = avcodec_default_get_buffer(c, pic);
    uint64_t *pts = av_malloc(sizeof(uint64_t));
    *pts = global_video_pkt_pts;
    pic->opaque = pts;
    return ret;
}
void our_release_buffer(struct AVCodecContext *c, AVFrame *pic) {
    if (pic)
        av_freep(&pic->opaque);
    avcodec_default_release_buffer(c, pic);
}

int video_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVPacket pkt1, *packet = &pkt1;
    //int len1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;
    
    pFrame = avcodec_alloc_frame();
    
    for (;;) {
        if (packet_queue_get(&is->videoq, packet, 1) < 0) {
            break;
        }
        
        pts = 0;
        
        global_video_pkt_pts = packet->pts;
        
        avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished,
                              packet);
        
        if (packet->dts == AV_NOPTS_VALUE && pFrame->opaque
            && *(uint64_t*) pFrame->opaque != AV_NOPTS_VALUE) {
            pts = *(uint64_t *) pFrame->opaque;
        } else if (packet->dts != AV_NOPTS_VALUE) {
            pts = packet->dts;
        } else {
            pts = 0;
        }
        pts *= av_q2d(is->video_st->time_base);
        
        if (frameFinished) {
            pts = synchronize_video(is, pFrame, pts);
            if (queue_picture(is, pFrame, pts) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }
    av_free(pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    
    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }
    
    codecCtx = pFormatCtx->streams[stream_index]->codec;
    
    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;
        
        if (is->mainChannel==1) {
            if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
                fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
                return -1;
            }
        }
        is->audio_hw_buf_size = spec.size;
    }
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            
            is->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
            is->audio_diff_avg_count = 0;
            is->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE
            / codecCtx->sample_rate;
            
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            if (is->mainChannel==1) SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            
            is->sws_ctx = sws_getContext(is->video_st->codec->width,
                                         is->video_st->codec->height, is->video_st->codec->pix_fmt,
                                         is->video_st->codec->width, is->video_st->codec->height,
                                         PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            
            is->frame_timer = (double) av_gettime() / 1000000.0;
            is->frame_last_delay = 40e-3;
            is->video_current_pts_time = av_gettime();
            
            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, is);
            codecCtx->get_buffer = our_get_buffer;
            codecCtx->release_buffer = our_release_buffer;
            break;
        default:
            break;
    }
    
    return 0;
}

int decode_interrupt_cb(void *opaque) {
    return (global_video_state && global_video_state->quit);
}

int decode_thread(void *arg) {
    
    VideoState *is = (VideoState *) arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;
    
    int video_index = -1;
    int audio_index = -1;
    int i;
    
    is->videoStream = -1;
    is->audioStream = -1;
    
    AVIOInterruptCB interupt_cb;
    
    global_video_state = channel_1;
    
    interupt_cb.callback = decode_interrupt_cb;
    interupt_cb.opaque = is;
    if (avio_open2(&is->io_ctx, is->filename, 0, &interupt_cb, NULL)) {
        fprintf(stderr, "Cannot open I/O for %s\n", is->filename);
        return -1;
    }
    
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
        return -1;
    is->pFormatCtx = pFormatCtx;
    
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;
    av_dump_format(pFormatCtx, 0, is->filename, 0);
    
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO
            && video_index < 0) {
            video_index = i;
        }
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
            && audio_index < 0) {
            audio_index = i;
        }
    }
    if (audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if (video_index >= 0) {
        stream_component_open(is, video_index);
    }
    
    if (is->videoStream < 0 || is->audioStream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }
    
    for (;;) {
        if (is->quit) {
            break;
        }
        
        if (is->seek_req) {
            int stream_index = -1;
            int64_t seek_target = is->seek_pos;
            
            if (is->videoStream >= 0)
                stream_index = is->videoStream;
            else if (is->audioStream >= 0)
                stream_index = is->audioStream;
            
            if (stream_index >= 0) {
                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                           pFormatCtx->streams[stream_index]->time_base);
            }
            if (av_seek_frame(is->pFormatCtx, stream_index, seek_target,
                              is->seek_flags) < 0) {
                fprintf(stderr, "%s: error while seeking\n",
                        is->pFormatCtx->filename);
            } else {
                if (is->audioStream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->videoStream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
            }
            is->seek_req = 0;
        }
        
        if (is->audioq.size > MAX_AUDIOQ_SIZE
            || is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        
        if (av_read_frame(is->pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error == 0) {
                continue;
            } else {
                break;
            }
        }
        
        if (packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
    
    while (!is->quit) {
        SDL_Delay(100);
    }
    
fail: {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
}
    return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel) {
    
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
    }
}

void switch_screen_display_mode(int screen_mode,int screen_w,int screen_h){
    
    if(screen_mode==SINGLE_VIDEO){
        is_multiple_video=0;
        channel_1->x_pos = 0;
        channel_1->y_pos = 0;
        channel_2->x_pos = 0;
        channel_2->y_pos = 0;
    }
    else if(screen_mode==MULTIPLE_VIDEO){
        is_multiple_video=1;
        channel_1->x_pos = 0;
        channel_1->y_pos = 0;
        channel_2->x_pos = screen_w/2 + 1;
        channel_2->y_pos = 0;
    }
}

int main(int argc, char *argv[]) {
    
    SDL_Event event;
    VideoState *is1, *is2;
    
    film_speed = rhythm_instance.NORMAL;
    int screen_width, screen_height;
    is_multiple_video=1;
    
    main_audio = 1;
    
    is1 = av_mallocz(sizeof(VideoState));
    is2 = av_mallocz(sizeof(VideoState));
    is1->mainChannel = 1;
    is2->mainChannel = 2;
    channel_1 = is1;
    channel_2 = is2;
    
    if (argc < 5) {
        fprintf(stderr, "You Should insert 4 arguments\n the first+second for screen size and the other 2 for the movie names.\n");
        exit(1);
    }
    av_register_all();
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    
    screen_width = atoi(argv[1]);
    screen_height = atoi(argv[2]);
    
    is1->x_pos = 0;
    is1->y_pos = 0;
    is2->x_pos = screen_width/2 + 1;
    is2->y_pos = 0;
    
    
#ifndef __DARWIN__
    screen = SDL_SetVideoMode(screen_width, screen_height, 0, 0);
#else
    screen = SDL_SetVideoMode(screen_w, screen_h, 24, 0);
#endif
    if (!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }
    
    av_strlcpy(is1->filename, argv[3], sizeof(is1->filename));
    av_strlcpy(is2->filename, argv[4], sizeof(is2->filename));
    
    is1->pictq_mutex = SDL_CreateMutex();
    is1->pictq_cond = SDL_CreateCond();
    
    is2->pictq_mutex = SDL_CreateMutex();
    is2->pictq_cond = SDL_CreateCond();
    
    schedule_refresh(is1, 40);
    schedule_refresh(is2, 40);
    
    is1->av_sync_type = DEFAULT_AV_SYNC_TYPE;
    is2->av_sync_type = DEFAULT_AV_SYNC_TYPE;
    
    is1->parse_tid = SDL_CreateThread(decode_thread, is1);
    if (!is1->parse_tid) {
        av_free(is1);
        return -1;
    }
    sleep(1);
    
    is2->parse_tid = SDL_CreateThread(decode_thread, is2);
    if (!is2->parse_tid) {
        av_free(is2);
        return -1;
    }
    sleep(1);
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (unsigned char *) "FLUSH";
    for (;;) {
        double incr, pos;
        SDL_WaitEvent(&event);
        switch (event.type) {
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                    case SDLK_1:
                        printf("Playing the first video: %s\n",argv[3]);
                        main_audio = 1;
                        break;
                    case SDLK_2:
                        printf("Playing the second video: %s\n",argv[4]);
                        main_audio = 2;
                        break;
                    case SDLK_o:
                        printf("Changing to single channel mode\n");
                        switch_screen_display_mode(SINGLE_VIDEO,screen_width,screen_height);
                        break;
                    case SDLK_m:
                        printf("Changing to multiple channel mode\n");
                        switch_screen_display_mode(MULTIPLE_VIDEO,screen_width,screen_height);
                        break;
                    case SDLK_f:
                        printf("Changing to fast video mode\n");
                        film_speed = rhythm_instance.FAST;
                        break;
                    case SDLK_s:
                        printf("Changing to slow video mode\n");
                        film_speed = rhythm_instance.SLOW;
                        break;
                    case SDLK_n:
                        printf("Changing to normal video mode\n");
                        film_speed = rhythm_instance.NORMAL;
                        break;
					case SDLK_w:
						is1->gracescale = true;
						is2->gracescale = true;
						break;
                    case SDLK_LEFT:
                        incr = -10.0;
                        goto do_seek;
                    case SDLK_RIGHT:
                        incr = 10.0;
                        goto do_seek;
                    case SDLK_UP:
                        incr = 60.0;
                        goto do_seek;
                    case SDLK_DOWN:
                        incr = -60.0;
                        goto do_seek;
                    do_seek: if (global_video_state) {
                        pos = get_master_clock(global_video_state);
                        pos += incr;
                        stream_seek(global_video_state,
                                    (int64_t) (pos * AV_TIME_BASE), incr);
                    }
                        break;
                    default:
                        break;
                }
                break;
			
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                SDL_CondSignal(is1->audioq.cond);
                SDL_CondSignal(is1->videoq.cond);
                is1->quit = 1;
                SDL_CondSignal(is2->audioq.cond);
                SDL_CondSignal(is2->videoq.cond);
                is2->quit = 1;
                SDL_Quit();
                exit(0);
                break;
            case FF_ALLOC_EVENT:
                alloc_picture(event.user.data1);
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    return 0;
}
