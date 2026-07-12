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

#ifndef FFTOOLS_FFPLAY_THUMB_H
#define FFTOOLS_FFPLAY_THUMB_H

#include <stdint.h>

/*
 * A tiny second decoder that produces preview frames for the position the
 * cursor hovers over the seek bar. It opens its own AVFormatContext on the
 * same file and decodes in software on a worker thread, so it never disturbs
 * playback. Keyframe-accurate (fast, seeks with BACKWARD) — good enough for a
 * hover preview. The whole module degrades to a no-op if the input can't be
 * reopened or has no seekable video.
 */

/* Open a preview decoder for `filename` (duration in seconds, <=0 disables the
 * feature). Safe to call again for a new file after thumb_close(). May be
 * called from a background thread. */
void thumb_open(const char *filename, double duration);

/* Tear down the decoder and worker thread. */
void thumb_close(void);

/* 1 if previews are available for the current file. */
int  thumb_available(void);

/* Ask the worker for the frame at `t_seconds`. Coalesced: only the most recent
 * request is decoded. Cheap to call every frame. */
void thumb_request(double t_seconds);

/* Render-thread only. If a decoded preview exists, returns 1 and points
 * *pixels at a BGRA (SDL_PIXELFORMAT_ARGB8888) image of *w x *h, valid until
 * the next thumb_acquire(). *gen is bumped whenever the image content changes,
 * so callers can skip re-uploading an unchanged texture. Returns 0 if nothing
 * has been decoded yet. */
int  thumb_acquire(int *w, int *h, const uint32_t **pixels, int64_t *gen);

#endif /* FFTOOLS_FFPLAY_THUMB_H */
