/*****************************************************************************
 * ffplay_def.c
 *****************************************************************************
 *
 * copyright (c) 2001 Fabrice Bellard
 * copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ff_ffplay.h"
#include <stdbool.h>
#include "ff_cmdutils.h"

static int packet_queue_put(PacketQueue *q, AVPacket *pkt);

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_free_packet(pkt);

    return ret;
}

/* packet queue handling */
static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    q->abort_request = 1;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

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

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
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

/*****************************************************************************
 * line 468 in ffplay.c
 * near fill_rectangle
 ****************************************************************************/

// line 517 */
#define ALPHA_BLEND(a, oldp, newp, s)\
((((oldp << s) * (255 - (a))) + (newp * (a))) / (255 << s))

#define RGBA_IN(r, g, b, a, s)\
{\
    unsigned int v = ((const uint32_t *)(s))[0];\
    a = (v >> 24) & 0xff;\
    r = (v >> 16) & 0xff;\
    g = (v >> 8) & 0xff;\
    b = v & 0xff;\
}

#define YUVA_IN(y, u, v, a, s, pal)\
{\
    unsigned int val = ((const uint32_t *)(pal))[*(const uint8_t*)(s)];\
    a = (val >> 24) & 0xff;\
    y = (val >> 16) & 0xff;\
    u = (val >> 8) & 0xff;\
    v = val & 0xff;\
}

#define YUVA_OUT(d, y, u, v, a)\
{\
    ((uint32_t *)(d))[0] = (a << 24) | (y << 16) | (u << 8) | v;\
}


#define BPP 1
// line 544 near blend_subrect

/* line 1082 */
/* get the current audio clock value */
static double get_audio_clock(VideoState *is)
{
    if (is->audio_clock_serial != is->audioq.serial)
        return NAN;
    if (is->paused) {
        return is->audio_current_pts;
    } else {
        return is->audio_current_pts_drift + av_gettime() / 1000000.0;
    }
}

/* get the current video clock value */
static double get_video_clock(VideoState *is)
{
    if (is->video_clock_serial != is->videoq.serial)
        return NAN;
    if (is->paused) {
        return is->video_current_pts;
    } else {
        return is->video_current_pts_drift + av_gettime() / 1000000.0;
    }
}

/* get the current external clock value */
static double get_external_clock(VideoState *is)
{
    if (is->paused) {
        return is->external_clock;
    } else {
        double time = av_gettime() / 1000000.0;
        return is->external_clock_drift + time - (time - is->external_clock_time / 1000000.0) * (1.0 - is->external_clock_speed);
    }
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_video_clock(is);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_audio_clock(is);
            break;
        default:
            val = get_external_clock(is);
            break;
    }
    return val;
}

static void update_external_clock_pts(VideoState *is, double pts)
{
   is->external_clock_time = av_gettime();
   is->external_clock = pts;
   is->external_clock_drift = pts - is->external_clock_time / 1000000.0;
}

static void check_external_clock_sync(VideoState *is, double pts) {
    double ext_clock = get_external_clock(is);
    if (isnan(ext_clock) || fabs(ext_clock - pts) > AV_NOSYNC_THRESHOLD) {
        update_external_clock_pts(is, pts);
    }
}

static void update_external_clock_speed(VideoState *is, double speed) {
    update_external_clock_pts(is, get_external_clock(is));
    is->external_clock_speed = speed;
}

#if IJK_FFPLAY_MERGE
static void check_external_clock_speed(VideoState *is) {
   if ((is->video_stream >= 0 && is->videoq.nb_packets <= MIN_FRAMES / 2) ||
       (is->audio_stream >= 0 && is->audioq.nb_packets <= MIN_FRAMES / 2)) {
       update_external_clock_speed(is, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->external_clock_speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > MIN_FRAMES * 2) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > MIN_FRAMES * 2)) {
       update_external_clock_speed(is, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->external_clock_speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->external_clock_speed;
       if (speed != 1.0)
           update_external_clock_speed(is, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}
#endif

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime() / 1000000.0 + is->video_current_pts_drift - is->video_current_pts;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->video_current_pts = is->video_current_pts_drift + av_gettime() / 1000000.0;
        }
        is->video_current_pts_drift = is->video_current_pts - av_gettime() / 1000000.0;
    }
    update_external_clock_pts(is, get_external_clock(is));
    is->paused = !is->paused;
}

#ifdef IJK_FFPLAY_MERGE
static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}
#endif

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}
/* 1227 near compute_target_delay */

/* line 1504 */
static void duplicate_right_border_pixels(SDL_Picture *bmp) {
    int i, width, height;
    Uint8 *p, *maxp;
    for (i = 0; i < 3; i++) {
        width  = bmp->w;
        height = bmp->h;
        if (i > 0) {
            width  >>= 1;
            height >>= 1;
        }
        if (bmp->pitches[i] > width) {
            maxp = bmp->pixels[i] + bmp->pitches[i] * height - 1;
            for (p = bmp->pixels[i] + width - 1; p < maxp; p += bmp->pitches[i])
                *(p+1) = *p;
        }
    }
}

static int queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, int64_t pos, int serial)
{
    VideoState *is = &ffp->is;
    VideoPicture *vp;

#if defined(DEBUG_SYNC) && 0
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    /* wait until we have space to put a new picture */
    SDL_LockMutex(is->pictq_mutex);

    /* keep the last already displayed picture in the queue */
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE - 2 &&
           !is->videoq.abort_request) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if (is->videoq.abort_request)
        return -1;

    vp = &is->pictq[is->pictq_windex];

#if CONFIG_AVFILTER
    vp->sample_aspect_ratio = ((AVFilterBufferRef *)src_frame->opaque)->video->sample_aspect_ratio;
#else
    vp->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, src_frame);
#endif

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || vp->reallocate || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height) {
        SDL_Event event;

        vp->allocated  = 0;
        vp->reallocate = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        /* wait until the picture is allocated */
        SDL_LockMutex(is->pictq_mutex);
        while (!vp->allocated && !is->videoq.abort_request) {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        /* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to complete */
        if (is->videoq.abort_request && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_USEREVENT, SDL_LASTEVENT) != 1) {
            while (!vp->allocated) {
                SDL_CondWait(is->pictq_cond, is->pictq_mutex);
            }
        }
        SDL_UnlockMutex(is->pictq_mutex);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        AVPicture pict = { { 0 } };
#if CONFIG_AVFILTER
        avfilter_unref_bufferp(&vp->picref);
        vp->picref = src_frame->opaque;
#endif

        /* get a pointer on the bitmap */
        SDL_VoutLockYUVPicture (vp->bmp);

        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];

        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_picture_copy(&pict, (AVPicture *)src_frame,
                        src_frame->format, vp->width, vp->height);
#else
        av_opt_get_int(ffp->sws_opts, "sws_flags", 0, &ffp->sws_flags);
        is->img_convert_ctx = sws_getCachedContext(is->img_convert_ctx,
            vp->width, vp->height, src_frame->format, vp->width, vp->height,
            AV_PIX_FMT_YUV420P, ffp->sws_flags, NULL, NULL, NULL);
        if (is->img_convert_ctx == NULL) {
            fprintf(stderr, "Cannot initialize the conversion context\n");
            exit(1);
        }
        sws_scale(is->img_convert_ctx, (const uint8_t **)src_frame->data, src_frame->linesize,
                  0, vp->height, pict.data, pict.linesize);
#endif
        /* workaround SDL PITCH_WORKAROUND */
        duplicate_right_border_pixels(vp->bmp);
        /* update the bitmap content */
        SDL_VoutUnlockYUVPicture(vp->bmp);

        vp->pts = pts;
        vp->pos = pos;
        vp->serial = serial;

        /* now we can update the picture count */
        if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
            is->pictq_windex = 0;
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

static int get_video_frame(FFPlayer *ffp, AVFrame *frame, int64_t *pts, AVPacket *pkt, int *serial)
{
    VideoState *is = &ffp->is;
    int got_picture;

    if (packet_queue_get(&is->videoq, pkt, 1, serial) < 0)
        return -1;

    if (pkt->data == flush_pkt.data) {
        avcodec_flush_buffers(is->video_st->codec);

        SDL_LockMutex(is->pictq_mutex);
        // Make sure there are no long delay timers (ideally we should just flush the queue but that's harder)
        while (is->pictq_size && !is->videoq.abort_request) {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        is->video_current_pos = -1;
        is->frame_last_pts = AV_NOPTS_VALUE;
        is->frame_last_duration = 0;
        is->frame_timer = (double)av_gettime() / 1000000.0;
        is->frame_last_dropped_pts = AV_NOPTS_VALUE;
        SDL_UnlockMutex(is->pictq_mutex);

        return 0;
    }

    if(avcodec_decode_video2(is->video_st->codec, frame, &got_picture, pkt) < 0)
        return 0;

    if (got_picture) {
        int ret = 1;

        if (ffp->decoder_reorder_pts == -1) {
            *pts = av_frame_get_best_effort_timestamp(frame);
        } else if (ffp->decoder_reorder_pts) {
            *pts = frame->pkt_pts;
        } else {
            *pts = frame->pkt_dts;
        }

        if (*pts == AV_NOPTS_VALUE) {
            *pts = 0;
        }

        if (ffp->framedrop>0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            SDL_LockMutex(is->pictq_mutex);
            if (is->frame_last_pts != AV_NOPTS_VALUE && *pts) {
                double clockdiff = get_video_clock(is) - get_master_clock(is);
                double dpts = av_q2d(is->video_st->time_base) * *pts;
                double ptsdiff = dpts - is->frame_last_pts;
                if (!isnan(clockdiff) && fabs(clockdiff) < AV_NOSYNC_THRESHOLD &&
                     ptsdiff > 0 && ptsdiff < AV_NOSYNC_THRESHOLD &&
                     clockdiff + ptsdiff - is->frame_last_filter_delay < 0) {
                    is->frame_last_dropped_pos = pkt->pos;
                    is->frame_last_dropped_pts = dpts;
                    is->frame_drops_early++;
                    ret = 0;
                }
            }
            SDL_UnlockMutex(is->pictq_mutex);
        }

        return ret;
    }
    return 0;
}
/* line 1705 near configure_filtergraph */

/* line 1803 */
static int video_thread(void *arg)
{
    AVPacket pkt = { 0 };
    FFPlayer *ffp = arg;
    VideoState *is = &ffp->is;
    AVFrame *frame = avcodec_alloc_frame();
    int64_t pts_int = AV_NOPTS_VALUE;
#if CONFIG_AVFILTER
    int64_t pos = -1;
#endif
    double pts;
    int ret;
    int serial = 0;

#if CONFIG_AVFILTER
    AVCodecContext *codec = is->video_st->codec;
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;

    if (codec->codec->capabilities & CODEC_CAP_DR1) {
        is->use_dr1 = 1;
        codec->get_buffer     = codec_get_buffer;
        codec->release_buffer = codec_release_buffer;
        codec->opaque         = &is->buffer_pool;
    }
#endif

    for (;;) {
#if CONFIG_AVFILTER
        AVFilterBufferRef *picref;
        AVRational tb;
#endif
        while (is->paused && !is->videoq.abort_request)
            SDL_Delay(10);

        avcodec_get_frame_defaults(frame);
        av_free_packet(&pkt);

        ret = get_video_frame(ffp, frame, &pts_int, &pkt, &serial);
        if (ret < 0)
            goto the_end;

        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != serial) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(graph, is, vfilters, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                av_free_packet(&pkt);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = serial;
        }

        frame->pts = pts_int;
        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);
        if (is->use_dr1 && frame->opaque) {
            FrameBuffer      *buf = frame->opaque;
            AVFilterBufferRef *fb = avfilter_get_video_buffer_ref_from_arrays(
                                        frame->data, frame->linesize,
                                        AV_PERM_READ | AV_PERM_PRESERVE,
                                        frame->width, frame->height,
                                        frame->format);

            avfilter_copy_frame_props(fb, frame);
            fb->buf->priv           = buf;
            fb->buf->free           = filter_release_buffer;

            buf->refcount++;
            av_buffersrc_add_ref(filt_in, fb, AV_BUFFERSRC_FLAG_NO_COPY);

        } else
            av_buffersrc_write_frame(filt_in, frame);

        av_free_packet(&pkt);

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime() / 1000000.0;

            ret = av_buffersink_get_buffer_ref(filt_out, &picref, 0);
            if (ret < 0) {
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;

            avfilter_copy_buf_props(frame, picref);

            pts_int = picref->pts;
            tb      = filt_out->inputs[0]->time_base;
            pos     = picref->pos;
            frame->opaque = picref;

            if (av_cmp_q(tb, is->video_st->time_base)) {
                av_unused int64_t pts1 = pts_int;
                pts_int = av_rescale_q(pts_int, tb, is->video_st->time_base);
                av_dlog(NULL, "video_thread(): "
                        "tb:%d/%d pts:%"PRId64" -> tb:%d/%d pts:%"PRId64"\n",
                        tb.num, tb.den, pts1,
                        is->video_st->time_base.num, is->video_st->time_base.den, pts_int);
            }
            pts = pts_int * av_q2d(is->video_st->time_base);
            ret = queue_picture(is, frame, pts, pos, serial);
        }
#else
        pts = pts_int * av_q2d(is->video_st->time_base);
        ret = queue_picture(ffp, frame, pts, pkt.pos, serial);
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
    avcodec_flush_buffers(is->video_st->codec);
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_free_packet(&pkt);
    avcodec_free_frame(&frame);
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = arg;
    SubPicture *sp;
    AVPacket pkt1, *pkt = &pkt1;
    int got_subtitle;
    double pts;
    int i, j;
    int r, g, b, y, u, v, a;

    for (;;) {
        while (is->paused && !is->subtitleq.abort_request) {
            SDL_Delay(10);
        }
        if (packet_queue_get(&is->subtitleq, pkt, 1, NULL) < 0)
            break;

        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(is->subtitle_st->codec);
            continue;
        }
        SDL_LockMutex(is->subpq_mutex);
        while (is->subpq_size >= SUBPICTURE_QUEUE_SIZE &&
               !is->subtitleq.abort_request) {
            SDL_CondWait(is->subpq_cond, is->subpq_mutex);
        }
        SDL_UnlockMutex(is->subpq_mutex);

        if (is->subtitleq.abort_request)
            return 0;

        sp = &is->subpq[is->subpq_windex];

       /* NOTE: ipts is the PTS of the _first_ picture beginning in
           this packet, if any */
        pts = 0;
        if (pkt->pts != AV_NOPTS_VALUE)
            pts = av_q2d(is->subtitle_st->time_base) * pkt->pts;

        avcodec_decode_subtitle2(is->subtitle_st->codec, &sp->sub,
                                 &got_subtitle, pkt);
        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;

            for (i = 0; i < sp->sub.num_rects; i++)
            {
                for (j = 0; j < sp->sub.rects[i]->nb_colors; j++)
                {
                    RGBA_IN(r, g, b, a, (uint32_t*)sp->sub.rects[i]->pict.data[1] + j);
                    y = RGB_TO_Y_CCIR(r, g, b);
                    u = RGB_TO_U_CCIR(r, g, b, 0);
                    v = RGB_TO_V_CCIR(r, g, b, 0);
                    YUVA_OUT((uint32_t*)sp->sub.rects[i]->pict.data[1] + j, y, u, v, a);
                }
            }

            /* now we can update the picture count */
            if (++is->subpq_windex == SUBPICTURE_QUEUE_SIZE)
                is->subpq_windex = 0;
            SDL_LockMutex(is->subpq_mutex);
            is->subpq_size++;
            SDL_UnlockMutex(is->subpq_mutex);
        }
        av_free_packet(pkt);
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_audio_clock(is) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = FFMIN(FFMAX(wanted_nb_samples, min_nb_samples), max_nb_samples);
                }
                av_dlog(NULL, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is)
{
    AVPacket *pkt_temp = &is->audio_pkt_temp;
    AVPacket *pkt = &is->audio_pkt;
    AVCodecContext *dec = is->audio_st->codec;
    int len1, len2, data_size, resampled_data_size;
    int64_t dec_channel_layout;
    int got_frame;
    av_unused double audio_clock0;
    int new_packet = 0;
    int flush_complete = 0;
    int wanted_nb_samples;

    for (;;) {
        /* NOTE: the audio packet can contain several frames */
        while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
            if (!is->frame) {
                if (!(is->frame = avcodec_alloc_frame()))
                    return AVERROR(ENOMEM);
            } else
                avcodec_get_frame_defaults(is->frame);

            if (is->audioq.serial != is->audio_pkt_temp_serial)
                break;

            if (is->paused)
                return -1;

            if (flush_complete)
                break;
            new_packet = 0;
            len1 = avcodec_decode_audio4(dec, is->frame, &got_frame, pkt_temp);
            if (len1 < 0) {
                /* if error, we skip the frame */
                pkt_temp->size = 0;
                break;
            }

            pkt_temp->data += len1;
            pkt_temp->size -= len1;

            if (!got_frame) {
                /* stop sending empty packets if the decoder is finished */
                if (!pkt_temp->data && (dec->codec->capabilities & CODEC_CAP_DELAY))
                    flush_complete = 1;
                continue;
            }
            data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(is->frame),
                                                   is->frame->nb_samples,
                                                   is->frame->format, 1);

            dec_channel_layout =
                (is->frame->channel_layout && av_frame_get_channels(is->frame) == av_get_channel_layout_nb_channels(is->frame->channel_layout)) ?
                is->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(is->frame));
            wanted_nb_samples = synchronize_audio(is, is->frame->nb_samples);

            if (is->frame->format        != is->audio_src.fmt            ||
                dec_channel_layout       != is->audio_src.channel_layout ||
                is->frame->sample_rate   != is->audio_src.freq           ||
                (wanted_nb_samples       != is->frame->nb_samples && !is->swr_ctx)) {
                swr_free(&is->swr_ctx);
                is->swr_ctx = swr_alloc_set_opts(NULL,
                                                 is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                                 dec_channel_layout,           is->frame->format, is->frame->sample_rate,
                                                 0, NULL);
                if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
                    fprintf(stderr, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                            is->frame->sample_rate, av_get_sample_fmt_name(is->frame->format), av_frame_get_channels(is->frame),
                            is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
                    break;
                }
                is->audio_src.channel_layout = dec_channel_layout;
                is->audio_src.channels       = av_frame_get_channels(is->frame);
                is->audio_src.freq = is->frame->sample_rate;
                is->audio_src.fmt = is->frame->format;
            }

            if (is->swr_ctx) {
                const uint8_t **in = (const uint8_t **)is->frame->extended_data;
                uint8_t **out = &is->audio_buf1;
                int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / is->frame->sample_rate + 256;
                int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
                if (wanted_nb_samples != is->frame->nb_samples) {
                    if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - is->frame->nb_samples) * is->audio_tgt.freq / is->frame->sample_rate,
                                                wanted_nb_samples * is->audio_tgt.freq / is->frame->sample_rate) < 0) {
                        fprintf(stderr, "swr_set_compensation() failed\n");
                        break;
                    }
                }
                av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
                if (!is->audio_buf1)
                    return AVERROR(ENOMEM);
                len2 = swr_convert(is->swr_ctx, out, out_count, in, is->frame->nb_samples);
                if (len2 < 0) {
                    fprintf(stderr, "swr_convert() failed\n");
                    break;
                }
                if (len2 == out_count) {
                    fprintf(stderr, "warning: audio buffer is probably too small\n");
                    swr_init(is->swr_ctx);
                }
                is->audio_buf = is->audio_buf1;
                resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
            } else {
                is->audio_buf = is->frame->data[0];
                resampled_data_size = data_size;
            }

            audio_clock0 = is->audio_clock;
            is->audio_clock += (double)data_size /
                (av_frame_get_channels(is->frame) * is->frame->sample_rate * av_get_bytes_per_sample(is->frame->format));
#ifdef DEBUG
            {
                static double last_clock;
                printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
                       is->audio_clock - last_clock,
                       is->audio_clock, audio_clock0);
                last_clock = is->audio_clock;
            }
#endif
            return resampled_data_size;
        }

        /* free the current packet */
        if (pkt->data)
            av_free_packet(pkt);
        memset(pkt_temp, 0, sizeof(*pkt_temp));

        if (is->audioq.abort_request) {
            return -1;
        }

        if (is->audioq.nb_packets == 0)
            SDL_CondSignal(is->continue_read_thread);

        /* read next packet */
        if ((new_packet = packet_queue_get(&is->audioq, pkt, 1, &is->audio_pkt_temp_serial)) < 0)
            return -1;

        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(dec);
            flush_complete = 0;
        }

        *pkt_temp = *pkt;

        /* if update the audio clock with the pts */
        if (pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
            is->audio_clock_serial = is->audio_pkt_temp_serial;
        }
    }

    return -1;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    FFPlayer *ffp = opaque;
    VideoState *is = &ffp->is;
    int audio_size, len1;
    int bytes_per_sec;
    int frame_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, 1, is->audio_tgt.fmt, 1);

    ffp->audio_callback_time = av_gettime();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf      = is->silence_buf;
               is->audio_buf_size = sizeof(is->silence_buf) / frame_size * frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    bytes_per_sec = is->audio_tgt.freq * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    is->audio_current_pts = is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / bytes_per_sec;
    is->audio_current_pts_drift = is->audio_current_pts - ffp->audio_callback_time / 1000000.0;
    if (is->audioq.serial == is->audio_clock_serial)
        check_external_clock_sync(is, is->audio_current_pts);
}

static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        fprintf(stderr, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio (%d channels): %s\n", wanted_spec.channels, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            fprintf(stderr, "No more channel combinations to try, audio open failed\n");
            return -1;
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        fprintf(stderr, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            fprintf(stderr, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(FFPlayer *ffp, int stream_index)
{
    VideoState *is = &ffp->is;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts;
    AVDictionaryEntry *t = NULL;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = ic->streams[stream_index]->codec;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    ffp->audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = ffp->subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    ffp->video_codec_name; break;
        default: break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) fprintf(stderr, "No codec could be found with name '%s'\n", forced_codec_name);
        else                   fprintf(stderr, "No codec could be found with id %d\n", avctx->codec_id);
        return -1;
    }

    avctx->codec_id = codec->id;
    avctx->workaround_bugs   = ffp->workaround_bugs;
    avctx->lowres            = ffp->lowres;
    if(avctx->lowres > codec->max_lowres){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                codec->max_lowres);
        avctx->lowres= codec->max_lowres;
    }
    avctx->idct_algo         = ffp->idct;
    avctx->skip_frame        = ffp->skip_frame;
    avctx->skip_idct         = ffp->skip_idct;
    avctx->skip_loop_filter  = ffp->skip_loop_filter;
    avctx->error_concealment = ffp->error_concealment;

    if(avctx->lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (ffp->fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;

    opts = filter_codec_opts(ffp->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (avcodec_open2(avctx, codec, &opts) < 0)
        return -1;
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        return AVERROR_OPTION_NOT_FOUND;
    }

    /* prepare audio output */
    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        int audio_hw_buf_size = audio_open(is, avctx->channel_layout, avctx->channels, avctx->sample_rate, &is->audio_src);
        if (audio_hw_buf_size < 0)
            return -1;
        is->audio_hw_buf_size = audio_hw_buf_size;
        is->audio_tgt = is->audio_src;
    }

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio fifo fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / av_samples_get_buffer_size(NULL, is->audio_tgt.channels, is->audio_tgt.freq, is->audio_tgt.fmt, 1);

        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        memset(&is->audio_pkt_temp, 0, sizeof(is->audio_pkt_temp));
        packet_queue_start(&is->audioq);
        SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        packet_queue_start(&is->videoq);
        is->video_tid = SDL_CreateThreadEx(&is->_video_tid, video_thread, is);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];
        packet_queue_start(&is->subtitleq);

        is->subtitle_tid = SDL_CreateThreadEx(&is->_video_tid, subtitle_thread, is);
        break;
    default:
        break;
    }
    return 0;
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        packet_queue_abort(&is->audioq);

        SDL_CloseAudio();

        packet_queue_flush(&is->audioq);
        av_free_packet(&is->audio_pkt);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;
        avcodec_free_frame(&is->frame);

        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        packet_queue_abort(&is->videoq);

        /* note: we also signal this mutex to make sure we deblock the
           video thread in all cases */
        SDL_LockMutex(is->pictq_mutex);
        SDL_CondSignal(is->pictq_cond);
        SDL_UnlockMutex(is->pictq_mutex);

        SDL_WaitThread(is->video_tid, NULL);

        packet_queue_flush(&is->videoq);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        packet_queue_abort(&is->subtitleq);

        /* note: we also signal this mutex to make sure we deblock the
           video thread in all cases */
        SDL_LockMutex(is->subpq_mutex);
        is->subtitle_stream_changed = 1;

        SDL_CondSignal(is->subpq_cond);
        SDL_UnlockMutex(is->subpq_mutex);

        SDL_WaitThread(is->subtitle_tid, NULL);

        packet_queue_flush(&is->subtitleq);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
#if CONFIG_AVFILTER
    free_buffer_pool(&is->buffer_pool);
#endif
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = &ffp->is;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int eof = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    int orig_nb_streams;
    SDL_mutex *wait_mutex = SDL_CreateMutex();

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;

    ic = avformat_alloc_context();
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    err = avformat_open_input(&ic, is->filename, is->iformat, &ffp->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if ((t = av_dict_get(ffp->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;

    if (ffp->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, opts);
    if (err < 0) {
        fprintf(stderr, "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }
    for (i = 0; i < orig_nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use url_feof() to test for the end

    if (ffp->seek_by_bytes < 0)
        ffp->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    /* if seeking requested, we execute it */
    if (ffp->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = ffp->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            fprintf(stderr, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;
    if (!ffp->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                ffp->wanted_stream[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!ffp->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                ffp->wanted_stream[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!ffp->video_disable && !ffp->subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                ffp->wanted_stream[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);
    if (ffp->show_status) {
        av_dump_format(ic, 0, is->filename, 0);
    }

    is->show_mode = ffp->show_mode;

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(ffp, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (ffp->infinite_buffer < 0 && is->realtime)
        ffp->infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                fprintf(stderr, "%s: error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    update_external_clock_pts(is, NAN);
                } else {
                    update_external_clock_pts(is, seek_target / (double)AV_TIME_BASE);
                }
            }
            is->seek_req = 0;
            eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            avformat_queue_attached_pictures(ic);
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (ffp->infinite_buffer<1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (   (is->audioq   .nb_packets > MIN_FRAMES || is->audio_stream < 0 || is->audioq.abort_request)
                && (is->videoq   .nb_packets > MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request)
                && (is->subtitleq.nb_packets > MIN_FRAMES || is->subtitle_stream < 0 || is->subtitleq.abort_request)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (eof) {
            if (is->video_stream >= 0) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = is->video_stream;
                packet_queue_put(&is->videoq, pkt);
            }
            if (is->audio_stream >= 0 &&
                (is->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY)) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = is->audio_stream;
                packet_queue_put(&is->audioq, pkt);
            }
            SDL_Delay(10);
            if (is->audioq.size + is->videoq.size + is->subtitleq.size == 0) {
                if (ffp->loop != 1 && (!ffp->loop || --ffp->loop)) {
                    stream_seek(is, ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0, 0, 0);
                } else if (ffp->autoexit) {
                    ret = AVERROR_EOF;
                    goto fail;
                }
            }
            eof=0;
            continue;
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || url_feof(ic->pb))
                eof = 1;
            if (ic->pb && ic->pb->error)
                break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
                (pkt->pts - ic->streams[pkt->stream_index]->start_time) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
                <= ((double)ffp->duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!is->abort_request) {
        SDL_Delay(100);
    }

    ret = 0;
 fail:
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);
    if (is->ic) {
        avformat_close_input(&is->ic);
    }

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

static VideoState *stream_open(FFPlayer *ffp, const char *filename, AVInputFormat *iformat)
{
    VideoState *is = &ffp->is;

#ifdef IJK_FFPLAY_MERGE
    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
#endif
    av_strlcpy(is->filename, filename, sizeof(is->filename));
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* start video display */
    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond  = SDL_CreateCond();

    is->subpq_mutex = SDL_CreateMutex();
    is->subpq_cond  = SDL_CreateCond();

    packet_queue_init(&is->videoq);
    packet_queue_init(&is->audioq);
    packet_queue_init(&is->subtitleq);

    is->continue_read_thread = SDL_CreateCond();

    update_external_clock_pts(is, NAN);
    update_external_clock_speed(is, 1.0);
    is->audio_current_pts_drift = -av_gettime() / 1000000.0;
    is->video_current_pts_drift = is->audio_current_pts_drift;
    is->audio_clock_serial = -1;
    is->video_clock_serial = -1;
    is->av_sync_type = ffp->av_sync_type;
    is->read_tid     = SDL_CreateThreadEx(&is->_read_tid, read_thread, is);
    if (!is->read_tid) {
        av_free(is);
        return NULL;
    }
    return is;
}
/* line 2872 near stream_cycle_channel */

/* line 3321 */
static int lockmgr(void **mtx, enum AVLockOp op)
{
   switch(op) {
      case AV_LOCK_CREATE:
          *mtx = SDL_CreateMutex();
          if(!*mtx)
              return 1;
          return 0;
      case AV_LOCK_OBTAIN:
          return !!SDL_LockMutex(*mtx);
      case AV_LOCK_RELEASE:
          return !!SDL_UnlockMutex(*mtx);
      case AV_LOCK_DESTROY:
          SDL_DestroyMutex(*mtx);
          return 0;
   }
   return 1;
}

#if IJK_FFPLAY_MERGE
/* Called from the main */
int main(int argc, char **argv)
{
    int flags;
    VideoState *is;
    char dummy_videodriver[] = "SDL_VIDEODRIVER=dummy";

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();
    avformat_network_init();

    init_opts();

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);

    parse_options(NULL, argc, argv, options, opt_input_file);

    if (!input_filename) {
        show_usage();
        fprintf(stderr, "An input file must be specified\n");
        fprintf(stderr, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    if (display_disable) {
        video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    if (display_disable)
        SDL_putenv(dummy_videodriver); /* For the event queue, we always need a video driver. */
#if !defined(__MINGW32__) && !defined(__APPLE__)
    flags |= SDL_INIT_EVENTTHREAD; /* Not supported on Windows or Mac OS X */
#endif
    if (SDL_Init (flags)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        fprintf(stderr, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    if (!display_disable) {
#if HAVE_SDL_VIDEO_SIZE
        const SDL_VideoInfo *vi = SDL_GetVideoInfo();
        fs_screen_width = vi->current_w;
        fs_screen_height = vi->current_h;
#endif
    }

    SDL_EventState(SDL_ACTIVEEVENT, SDL_IGNORE);
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    if (av_lockmgr_register(lockmgr)) {
        fprintf(stderr, "Could not initialize lock manager!\n");
        do_exit(NULL);
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (char *)(intptr_t)"FLUSH";

    is = stream_open(input_filename, file_iformat);
    if (!is) {
        fprintf(stderr, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    event_loop(is);

    /* never returns */

    return 0;
}
#endif

/*****************************************************************************
 * end last line in ffplay.c
 ****************************************************************************/

AVPacket flush_pkt;
static bool g_ffmpeg_global_inited = false;

void ijkff_global_init()
{
    if (g_ffmpeg_global_inited)
        return;

    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();
    avformat_network_init();

    av_lockmgr_register(lockmgr);

    /* FIXME: SDL_Init() */

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)(intptr_t)"FLUSH";

    g_ffmpeg_global_inited = true;

    /* test link */
    stream_open(NULL, NULL, NULL);
}

void ijkff_global_uninit()
{
    if (!g_ffmpeg_global_inited)
        return;

    av_lockmgr_register(NULL);

#if CONFIG_AVFILTER
    avfilter_uninit();
    av_freep(&vfilters);
#endif
    avformat_network_deinit();
    /* FIXME: SDL_Quit(); */

    g_ffmpeg_global_inited = false;
}