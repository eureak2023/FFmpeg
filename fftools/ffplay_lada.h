/*
 * lada real-time mosaic-restoration client for ffplay.
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

#ifndef FFTOOLS_FFPLAY_LADA_H
#define FFTOOLS_FFPLAY_LADA_H

#include <stdint.h>

/*
 * lada removes mosaic/pixelation from video using a Python/PyTorch neural pipeline
 * (YOLO detection + BasicVSR++ restoration). It cannot run inside this process, so it
 * lives in a long-lived sidecar (lada_sidecar.py, launched in lada's own venv). The
 * sidecar decodes the same file, restores frames, and streams RGB24 frames back to us
 * over a pipe. ffplay keeps ownership of audio, the clock, subtitles and A/V timing;
 * at display time we swap in the restored pixels for the frame whose pts matches, and
 * fall back to the original frame whenever the restored one is not ready yet.
 */

/* 1 if the running executable's name marks it as a lada build (basename contains
 * "lada", e.g. ffplay_lada.exe), so mosaic restoration should default ON. Lets the
 * same code ship as ffplay.exe (off) or ffplay_lada.exe (on); -nolada still overrides. */
int  lada_default_on(void);

/* Launch the sidecar for the given input file. lada_home is the lada project root
 * (contains .venv/ and lada_sidecar.py); device is a torch device string ("cuda").
 * Returns 0 on success, negative if the sidecar could not be started (feature then
 * silently disabled, like every other optional path in this player). */
int  lada_start(const char *input_path, const char *lada_home, const char *device);

/* Stop the sidecar and free everything (kills the process). Safe when not started. */
void lada_stop(void);

/* Turn restoration streaming on/off without killing the sidecar - the models stay
 * resident so toggling is instant and resumes at the current playback position. The
 * first enable still pays the one-time model-load cost inside the sidecar. */
void lada_set_enabled(int on);

/* Nonzero while restoration is enabled and the sidecar is running (and hasn't died). */
int  lada_active(void);

/* Notify that a seek was just requested (call from the seek path, not the display
 * thread). Disengages the pause-to-buffer gate immediately so a forward seek can't
 * strand playback in a permanent "LADA BUFFER" pause. Safe when lada is off. */
void lada_notify_seek(void);

/* Use the "jasna" restoration engine instead of lada_sidecar: lada_start then launches
 * the from-source jasna sidecar (jasna_home holds .venv + model_weights). Same wire
 * protocol, so everything else is unchanged. Call before lada_start (e.g. from -jasna). */
void lada_set_jasna(const char *jasna_home);

/* Select the restoration engine (0 = lada, 1 = jasna) before (re)starting the sidecar;
 * for the runtime engine toggle. lada_set_jasna is the CLI shorthand for (1, home). */
void lada_set_engine(int use_jasna, const char *jasna_home);

/* 1 if the jasna engine is selected, 0 for lada. */
int  lada_is_jasna(void);

/* Uppercase name of the active restoration engine ("LADA" or "JASNA"), for the status
 * overlay/toasts so the two are distinguishable on screen. */
const char *lada_engine_name(void);

/* Short state word for the status overlay: "OFF", "LOADING" (models loading),
 * "WAIT" (ready, buffering the lead), "ACTIVE" (restored frames showing),
 * "BUFFER" (paused to refill), "FAILED". */
const char *lada_status(void);

/* Pause-to-buffer control (throughput below real time). Returns 1 when playback should
 * be paused so the sidecar can refill the restored-frame buffer, 0 to keep playing.
 * display_pts is the current master-clock position in seconds. Has hysteresis. */
int  lada_should_buffer(double display_pts);

/* Main thread: fetch a pending one-shot status toast ("LADA ACTIVE" once restoration
 * starts showing, "LADA FAILED" if the sidecar died). Returns 1 and fills buf (which
 * should be >= 24 bytes) if one is waiting, else 0. On "LADA FAILED" the caller should
 * lada_stop() to clean up the dead process. */
int  lada_poll_toast(char *buf, int buflen);

/* Ask for the restored frame matching a displayed frame at presentation time
 * pts_sec (seconds) with duration dur_sec (seconds, used as the match tolerance).
 * On success returns 1 and points *rgb at an internal RGB24 buffer (tightly packed,
 * stride == *w * 3) valid until the next lada_frame_for() call. Returns 0 when no
 * matching restored frame is ready (caller shows the original frame). Seek detection
 * is handled internally from the pts stream. */
int  lada_frame_for(double pts_sec, double dur_sec,
                    const uint8_t **rgb, int *w, int *h);

#endif /* FFTOOLS_FFPLAY_LADA_H */
