/*
 * On-screen player controls (seek bar + control bar) for ffplay.
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

#ifndef FFTOOLS_FFPLAY_UI_H
#define FFTOOLS_FFPLAY_UI_H

#include <SDL.h>

/*
 * A PotPlayer-style overlay: a seek bar row and a control bar docked to the
 * bottom of the window. Shown on mouse activity, auto-hidden after a short
 * idle period. The module keeps all hover/drag/visibility state internally;
 * the caller feeds it mouse events and draws it right before presenting.
 */

/* Actions returned by ui_handle_event(). Nonzero means the event was
 * consumed and must not reach ffplay's default mouse handling. */
enum {
    UI_ACT_NONE = 0,      /* not consumed */
    UI_ACT_CONSUMED,      /* consumed, nothing further to do */
    UI_ACT_TOGGLE_PAUSE,
    UI_ACT_STOP,          /* seek to start + pause */
    UI_ACT_SEEK_BACK,     /* seek backward (short jump) */
    UI_ACT_SEEK_FWD,      /* seek forward (short jump) */
    UI_ACT_SEEK_FRAC,     /* seek to *seek_frac of the duration */
    UI_ACT_MINIMIZE,
    UI_ACT_MAXIMIZE,      /* toggle maximize/restore */
    UI_ACT_CLOSE,
    UI_ACT_SET_VOLUME,    /* set volume to *seek_frac (0..1) */
    UI_ACT_OPEN,          /* open-file button pressed */
};

void ui_init(SDL_Renderer *renderer);

/* Attach the window: enables the title bar (uppercased basename of title),
 * grab-anywhere window dragging and resize borders for borderless mode. */
void ui_set_window(SDL_Window *window, const char *title);

/* Feed a mouse event (motion/button); win_w/win_h are the current window
 * size. Returns a UI_ACT_* value; for UI_ACT_SEEK_FRAC the target position
 * fraction (0..1) is stored in *seek_frac. */
int  ui_handle_event(const SDL_Event *event, int win_w, int win_h,
                     double *seek_frac);

/* Draw the overlay (no-op while hidden). pos/dur in seconds,
 * volume 0..1. */
void ui_draw(SDL_Renderer *renderer, int win_w, int win_h,
             double pos, double dur, int paused, double volume);

/* Stream info badges shown at the right of the control bar. hw is the
 * highlighted decode badge ("H/W"), the rest are boxed labels; NULL keeps
 * a slot unchanged. Callable from the stream-open thread (textures are
 * built lazily on the render thread). */
void ui_set_badges(const char *hw, const char *vcodec,
                   const char *acodec, const char *chans);

/* Chapter start positions as 0..1 fractions of the duration. */
void ui_set_chapters(const double *fracs, int n);

/* Keep the overlay visible (counts as user activity); used by the
 * idle "no file loaded" screen. */
void ui_ping(void);

/* Native "open media file" dialog (blocks). Returns an av_strdup'ed
 * UTF-8 path, or NULL if cancelled/unavailable. */
char *ui_open_file_dialog(void);

/* Right-click context menu. Shown asynchronously so playback keeps
 * running; the selection arrives as an SDL user event, decode it with
 * ui_menu_result() (returns -1 for unrelated events, 0 for dismissed). */
enum { UI_MENU_OPEN = 1, UI_MENU_CLOSE = 2 };
void ui_context_menu(void);
int  ui_menu_result(const SDL_Event *event);

/* Text subtitles (SRT/SMI/ASS), rendered bottom-center with the system
 * font. Events may be added from decode threads; drawing and clearing
 * happen on the render thread. Times in seconds. */
void ui_sub_add(double start, double end, const char *text);
void ui_sub_add_ass(double start, double end, const char *ass);
void ui_sub_clear(void);
void ui_sub_draw(SDL_Renderer *renderer, int win_w, int win_h, double now);

/* Returns 1 once whenever visibility or hover changed since the last draw,
 * so a paused player knows to redraw. Poll from the refresh loop. */
int  ui_wants_refresh(void);

void ui_uninit(void);

#endif /* FFTOOLS_FFPLAY_UI_H */
