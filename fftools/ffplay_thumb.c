/*
 * Seek-bar thumbnail preview for ffplay.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <math.h>
#include <string.h>

#include <SDL.h>

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libswscale/swscale.h"

#include "ffplay_thumb.h"

/* Preview image height in pixels; width follows the source aspect (incl. SAR),
 * capped so an ultrawide source can't produce a huge popup. */
#define THUMB_H       120
#define THUMB_MAX_W   360
/* Don't re-decode when the cursor barely moved (seconds of media time). */
#define THUMB_MIN_STEP 0.20

static struct ThumbCtx {
    int              enabled;

    AVFormatContext *fmt;
    AVCodecContext  *dec;
    int              vstream;
    struct SwsContext *sws;
    int              sws_w, sws_h, sws_fmt;   /* what sws was built for */

    AVPacket        *pkt;
    AVFrame         *frame;

    int              tw, th;                  /* preview dimensions */

    /* worker thread + request/result handoff */
    SDL_Thread      *thread;
    SDL_mutex       *lock;
    SDL_cond        *cond;
    int              quit;
    int              req_pending;
    double           req_time;
    double           done_time;               /* last time we actually decoded */

    uint32_t        *result;                  /* worker writes (tw*th BGRA) */
    int64_t          result_gen;

    uint32_t        *front;                   /* thumb_acquire() hands this out */
    int64_t          front_gen;
} T;

/* Decode one frame at/near `t` seconds into T.result. Runs on the worker
 * thread only. Returns 0 on success. */
static int decode_at(double t)
{
    int64_t ts = (int64_t)(t * AV_TIME_BASE);
    int got = 0, guard = 0;

    if (avformat_seek_file(T.fmt, -1, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD) < 0 &&
        av_seek_frame(T.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return -1;
    avcodec_flush_buffers(T.dec);

    /* Read until the decoder hands back the first frame after the keyframe we
     * landed on. Bounded so a broken stream can't spin forever. */
    while (!got && guard++ < 1000) {
        int ret = av_read_frame(T.fmt, T.pkt);

        if (ret < 0) {
            /* flush the decoder at EOF in case a frame is still buffered */
            avcodec_send_packet(T.dec, NULL);
        } else if (T.pkt->stream_index != T.vstream) {
            av_packet_unref(T.pkt);
            continue;
        } else {
            ret = avcodec_send_packet(T.dec, T.pkt);
            av_packet_unref(T.pkt);
            if (ret < 0)
                continue;
        }

        while (avcodec_receive_frame(T.dec, T.frame) >= 0) {
            got = 1;
            break;
        }
        if (ret < 0 && !got)   /* EOF and nothing buffered */
            break;
    }
    if (!got)
        return -1;

    /* (re)build the scaler for this frame's format/size */
    if (!T.sws || T.sws_w != T.frame->width || T.sws_h != T.frame->height ||
        T.sws_fmt != T.frame->format) {
        sws_freeContext(T.sws);
        T.sws = sws_getContext(T.frame->width, T.frame->height, T.frame->format,
                               T.tw, T.th, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, NULL, NULL, NULL);
        T.sws_w = T.frame->width;
        T.sws_h = T.frame->height;
        T.sws_fmt = T.frame->format;
    }
    if (!T.sws) {
        av_frame_unref(T.frame);
        return -1;
    }

    {
        uint8_t *dst[4] = { (uint8_t *)T.result, NULL, NULL, NULL };
        int dst_stride[4] = { T.tw * 4, 0, 0, 0 };

        sws_scale(T.sws, (const uint8_t *const *)T.frame->data, T.frame->linesize,
                  0, T.frame->height, dst, dst_stride);
    }
    av_frame_unref(T.frame);
    return 0;
}

static int thumb_worker(void *arg)
{
    (void)arg;
    for (;;) {
        double t;

        SDL_LockMutex(T.lock);
        while (!T.req_pending && !T.quit)
            SDL_CondWait(T.cond, T.lock);
        if (T.quit) {
            SDL_UnlockMutex(T.lock);
            break;
        }
        t = T.req_time;
        T.req_pending = 0;
        SDL_UnlockMutex(T.lock);

        if (decode_at(t) == 0) {
            SDL_LockMutex(T.lock);
            T.result_gen++;
            T.done_time = t;
            SDL_UnlockMutex(T.lock);
        }
    }
    return 0;
}

void thumb_open(const char *filename, double duration)
{
    const AVCodec *dec;
    AVStream *st;
    double aspect;
    int tw;

    thumb_close();
    if (!filename || duration <= 0)
        return;

    if (avformat_open_input(&T.fmt, filename, NULL, NULL) < 0)
        return;
    if (avformat_find_stream_info(T.fmt, NULL) < 0)
        goto fail;
    T.vstream = av_find_best_stream(T.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (T.vstream < 0 || !dec)
        goto fail;
    st = T.fmt->streams[T.vstream];
    /* cover art / single-picture "video" streams aren't seekable previews */
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
        goto fail;

    T.dec = avcodec_alloc_context3(dec);
    if (!T.dec || avcodec_parameters_to_context(T.dec, st->codecpar) < 0)
        goto fail;
    /* Software decode. Skipping the loop filter speeds up the keyframe decode
     * and only costs a little deblocking quality, which a thumbnail won't show.
     * (Don't skip the IDCT — that would blank the image.) */
    T.dec->skip_loop_filter = AVDISCARD_ALL;
    T.dec->thread_count     = 0;   /* let it auto-thread the keyframe decode */
    if (avcodec_open2(T.dec, dec, NULL) < 0)
        goto fail;

    /* preview size from coded size and sample aspect ratio */
    aspect = T.dec->height ? (double)T.dec->width / T.dec->height : 16.0 / 9.0;
    if (st->sample_aspect_ratio.num > 0 && st->sample_aspect_ratio.den > 0)
        aspect *= av_q2d(st->sample_aspect_ratio);
    tw = (int)(THUMB_H * aspect + 0.5);
    tw &= ~1;                          /* even width keeps swscale happy */
    if (tw < 80)          tw = 80;
    if (tw > THUMB_MAX_W) tw = THUMB_MAX_W;
    T.tw = tw;
    T.th = THUMB_H;

    T.result = av_malloc((size_t)T.tw * T.th * 4);
    T.front  = av_malloc((size_t)T.tw * T.th * 4);
    T.pkt    = av_packet_alloc();
    T.frame  = av_frame_alloc();
    if (!T.result || !T.front || !T.pkt || !T.frame)
        goto fail;

    T.lock = SDL_CreateMutex();
    T.cond = SDL_CreateCond();
    if (!T.lock || !T.cond)
        goto fail;
    T.done_time = -1e9;
    T.enabled = 1;
    T.thread = SDL_CreateThread(thumb_worker, "thumb", NULL);
    if (!T.thread) {
        T.enabled = 0;
        goto fail;
    }
    return;

fail:
    thumb_close();
}

void thumb_close(void)
{
    if (T.thread) {
        SDL_LockMutex(T.lock);
        T.quit = 1;
        SDL_CondSignal(T.cond);
        SDL_UnlockMutex(T.lock);
        SDL_WaitThread(T.thread, NULL);
    }
    if (T.lock) SDL_DestroyMutex(T.lock);
    if (T.cond) SDL_DestroyCond(T.cond);
    sws_freeContext(T.sws);
    av_frame_free(&T.frame);
    av_packet_free(&T.pkt);
    avcodec_free_context(&T.dec);
    if (T.fmt) avformat_close_input(&T.fmt);
    av_freep(&T.result);
    av_freep(&T.front);
    memset(&T, 0, sizeof(T));
}

int thumb_available(void)
{
    return T.enabled;
}

void thumb_request(double t_seconds)
{
    if (!T.enabled)
        return;
    SDL_LockMutex(T.lock);
    /* ignore near-duplicate requests so a still cursor doesn't re-decode */
    if (T.req_pending || fabs(t_seconds - T.done_time) >= THUMB_MIN_STEP) {
        T.req_time = t_seconds;
        T.req_pending = 1;
        SDL_CondSignal(T.cond);
    }
    SDL_UnlockMutex(T.lock);
}

int thumb_acquire(int *w, int *h, const uint32_t **pixels, int64_t *gen)
{
    if (!T.enabled)
        return 0;
    SDL_LockMutex(T.lock);
    if (T.result_gen == 0) {
        SDL_UnlockMutex(T.lock);
        return 0;
    }
    if (T.front_gen != T.result_gen) {
        memcpy(T.front, T.result, (size_t)T.tw * T.th * 4);
        T.front_gen = T.result_gen;
    }
    SDL_UnlockMutex(T.lock);

    *w = T.tw;
    *h = T.th;
    *pixels = T.front;
    *gen = T.front_gen;
    return 1;
}
