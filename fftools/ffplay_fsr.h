/*
 * AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) display path for ffplay.
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

#ifndef FFTOOLS_FFPLAY_FSR_H
#define FFTOOLS_FFPLAY_FSR_H

#include <SDL.h>

/*
 * FSR runs as two raw OpenGL fragment passes (EASU upscale, RCAS sharpen)
 * interleaved with an SDL_Renderer that must use the "opengl" backend.
 * The decoded frame is first converted to RGB at native size by SDL itself
 * (render-target copy), so every pixel format ffplay can display flows
 * through the FSR path unchanged.
 */

/* Probe the renderer and build the GL pipeline. Returns 0 on success,
 * a negative value if FSR is unavailable (non-GL renderer, old GL, ...). */
int  fsr_init(SDL_Renderer *renderer);

/* Raise the OS timer resolution so short sleeps in the refresh loop are
 * accurate (Windows: timeBeginPeriod(1)); call once at startup. */
void fsr_timer_init(void);

/* Sleep with sub-millisecond accuracy (high-resolution waitable timer on
 * Windows, av_usleep elsewhere). Main-thread only. */
void fsr_precise_sleep(int64_t usec);

/* Drop the process console when it is ours alone (double-click launch),
 * so no empty terminal window shows behind the video. */
void fsr_detach_console(void);

/* Register (1) or remove (0) current-user .mp4/.mkv file associations
 * pointing at this executable. Windows only; returns 0 on success. */
int  fsr_register_associations(int install);

/* Draw vid_texture upscaled into rect through EASU+RCAS.
 * Returns 1 if the frame was drawn, 0 if the caller must draw the stock
 * way (FSR unavailable, or no upscaling needed for this rect). */
int  fsr_draw(SDL_Renderer *renderer, SDL_Texture *vid_texture,
              int frame_width, int frame_height,
              const SDL_Rect *rect, int flip_v, float sharpness);

int  fsr_available(void);

/* Enable/disable the RCAS denoise term (reduces sharpening of noise and
 * film grain). Safe to call any time from the main thread. */
void fsr_set_denoise(SDL_Renderer *renderer, int enable);

/* Exposure of the HDR10 -> SDR tone map, as a 0..2 factor:
 *   0   BT.2446-A by the book - the signal is referred to the content peak,
 *       which is faithful but lands well below a studio SDR grade;
 *   1   diffuse white (203 nits, BT.2408) is exposed to SDR white, i.e. the
 *       brightness an SDR release of the same title would show (default);
 *   >1  brighter still.
 * Highlights roll off instead of clipping, so raising it trades highlight
 * separation for midtone brightness. No effect on SDR sources. Takes effect
 * on the next frame; safe to call from the main thread at any time. */
void fsr_set_hdr_brightness(float level);
/* Nonzero while the displayed stream is being tone-mapped from HDR10. */
int  fsr_hdr_active(void);

/* Zero-copy display of AV_PIX_FMT_D3D11 hardware frames: the decoded NV12
 * texture is converted to RGBA by the D3D11 VideoProcessor on the GPU and
 * shared into GL via WGL_NV_DX_interop2, then flows through the FSR passes
 * (or a passthrough blit when FSR is off/bypassed). Returns 1 if the frame
 * was drawn, 0 if the caller must fall back to copy-back display. */
struct AVFrame;
int  fsr_hw_draw(SDL_Renderer *renderer, struct AVFrame *frame,
                 const SDL_Rect *rect, int fsr_on, float sharpness);
/* Nonzero once zero-copy interop is known to be unusable; decoder threads
 * should then copy hardware frames back to system memory. */
int  fsr_hw_interop_failed(void);

/* DXGI adapter index matching the GL context's GPU (so the D3D11 decode
 * device can be created on the same adapter), or -1 if unknown. */
int  fsr_d3d11_adapter_index(void);

/* Nonzero if the D3D11 device inside hw_device_ctx can hardware-decode
 * codec_id. Only AV1 is actually probed (its native hw decoder has no
 * software fallback); all other codecs return 1. Used to avoid swapping to a
 * hardware-only decoder on a GPU that cannot decode the codec. */
struct AVBufferRef;
int  fsr_d3d11_supports_codec(struct AVBufferRef *hw_device_ctx, int codec_id);

/* Frame generation: draw an interpolated midpoint frame between two
 * consecutive D3D11 hardware frames using NVIDIA hardware optical flow.
 * Returns 1 if the frame was drawn (present it), 0 otherwise. */
int  fsr_fg_draw(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next,
                 const SDL_Rect *rect, int fsr_on, float sharpness, float phase);
/* Precompute the optical flow for a frame pair (cached, cheap to repeat);
 * call while waiting for a presentation slot so fsr_fg_draw is fast. */
int  fsr_fg_prepare(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next);
int  fsr_fg_available(void);

/* RIFE frame generation: a learned per-pixel alternative to the block-grid
 * optical flow, used in place of the warp while it is on and the frame is
 * small enough to fit the time budget (see ffplay_rife.h). fsr_rife_active()
 * reports whether it is both requested and actually usable. */
void fsr_rife_set(int on);
/* Nonzero if a source of this size will actually be interpolated by RIFE
 * rather than the flow warp (requested, available, and small enough). */
int  fsr_rife_active(int w, int h);
/* Create the Vulkan device. Call BEFORE SDL_CreateRenderer: bringing ncnn's
 * Vulkan device up after the SDL OpenGL renderer exists corrupts the renderer
 * and the next SDL_CreateTexture() crashes. Returns 0 if RIFE is usable. */
int  fsr_rife_boot(void);

/* Load driver libraries and create the CUDA context; call at startup,
 * before decoding begins (device probing races with active decode). */
int  fsr_fg_boot(void);

/* Small on-screen toast (built-in bitmap font, uppercase letters only).
 * Shown top-left for ~1.8s with a fade-out; draw it right before
 * SDL_RenderPresent(). fsr_toast_draw()/fsr_toast_active() return whether
 * the toast is still live (callers use this to force refreshes). */
void fsr_toast_show(SDL_Renderer *renderer, const char *text);
int  fsr_toast_draw(SDL_Renderer *renderer);
int  fsr_toast_active(void);

/* Persistent top-right HUD (FPS display). NULL/empty text hides it. */
void fsr_hud_set(SDL_Renderer *renderer, const char *text);
int  fsr_hud_draw(SDL_Renderer *renderer);

/* Album-art "now playing" visualizer for audio-only playback (a port of
 * WinVibe's LPPlayerView): an animated rainbow-blob background with the cover
 * art laid flat and a vinyl LP spinning out from behind it. Drawn with the
 * plain SDL_Renderer, so it works on any backend.
 *
 * fsr_album_set_cover() feeds the cover pixels once (BGRA byte order, i.e.
 * AV_PIX_FMT_BGRA); with no cover a blank record still spins. fsr_album_draw()
 * renders one frame (playing = advance the spin, 0 = frozen while paused).
 * fsr_album_reset() drops the current cover/disc when a new file loads. */
void fsr_album_set_cover(SDL_Renderer *renderer, const uint8_t *bgra, int w, int h);
void fsr_album_draw(SDL_Renderer *renderer, int playing);
void fsr_album_reset(void);
/* Install a generated placeholder cover (gradient + music note) when the file
 * has no embedded art, so a real image shows instead of a blank disc. No-op
 * once any cover is present. */
void fsr_album_ensure_default(SDL_Renderer *renderer);

/* Bottom FFT-spectrum bar visualizer (a port of WinVibe's LineBarVisualizer2):
 * a horizontally-mirrored rainbow bar spectrum with a triangle-wave centre
 * line, drawn as a band across the bottom over the album view. Feed the newest
 * mono samples in [-1,1] each frame (nsamp must be >= the internal FFT size);
 * playing = 0 lets the bars decay while paused. fsr_vis_reset() clears the bar
 * state on a new file. */
void fsr_vis_draw(SDL_Renderer *renderer, const float *mono, int nsamp, int playing);
void fsr_vis_reset(void);

/* 8x8 bitmap for a pixel-font character (uppercase letters, digits,
 * ':', '/', '.'), or NULL for characters rendered as blanks. */
const uint8_t *fsr_glyph(char c);

/* Single-instance support (Windows). fsr_single_instance_begin() creates the
 * named mutex and returns 1 for the first (primary) instance, 0 if another is
 * already running. A secondary instance hands its file to the primary with
 * fsr_single_instance_forward(path) (WM_COPYDATA) and then exits. The primary
 * calls fsr_single_instance_setup(window) once its window exists (tags the
 * window and installs the message hook), and polls fsr_single_instance_take_
 * path() from the event loop for an incoming path (av_malloc'd, caller frees),
 * or NULL. Non-Windows builds are single-instance no-ops. */
int   fsr_single_instance_begin(void);
int   fsr_single_instance_forward(const char *path);
void  fsr_single_instance_setup(SDL_Window *window);
char *fsr_single_instance_take_path(void);

/* Return an av_malloc'd path to the next (dir=+1) or previous (dir=-1)
 * playable media file in the same directory as cur_path, sorted
 * case-insensitively by name and wrapping around at the ends. NULL if there
 * is no other media file or on error. Used by the PgUp/PgDn playlist hotkeys.
 * Non-Windows builds return NULL. */
char *fsr_sibling_media_path(const char *cur_path, int dir);

/* Show a modal confirmation (owned by window) asking whether to delete
 * utf8_path; returns 1 if the user confirmed. fsr_delete_file() sends the
 * (already-closed) file to the Recycle Bin, returning 0 on success. Used by
 * the Delete hotkey. Non-Windows builds are no-ops. */
int   fsr_confirm_delete(SDL_Window *window, const char *utf8_path);
int   fsr_delete_file(const char *utf8_path);

/* Lift a modal dialog above a fullscreen (topmost) player window. The call
 * that shows such a dialog blocks, so a helper thread does it: call begin()
 * with the owner HWND before the blocking call and end() with its return value
 * right after. Both tolerate NULL. Non-Windows builds are no-ops. */
void *fsr_raise_modal_begin(void *owner_hwnd);
void  fsr_raise_modal_end(void *handle);

/* Destroy GL programs and intermediate textures. Call before the renderer
 * is destroyed. */
void fsr_uninit(void);

#endif /* FFTOOLS_FFPLAY_FSR_H */
