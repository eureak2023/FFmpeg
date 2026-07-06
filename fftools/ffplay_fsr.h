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

/* Frame generation: draw an interpolated midpoint frame between two
 * consecutive D3D11 hardware frames using NVIDIA hardware optical flow.
 * Returns 1 if the frame was drawn (present it), 0 otherwise. */
int  fsr_fg_draw(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next,
                 const SDL_Rect *rect, int fsr_on, float sharpness, float phase);
/* Precompute the optical flow for a frame pair (cached, cheap to repeat);
 * call while waiting for a presentation slot so fsr_fg_draw is fast. */
int  fsr_fg_prepare(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next);
int  fsr_fg_available(void);
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

/* Destroy GL programs and intermediate textures. Call before the renderer
 * is destroyed. */
void fsr_uninit(void);

#endif /* FFTOOLS_FFPLAY_FSR_H */
