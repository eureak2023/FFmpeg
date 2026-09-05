/*
 * DLSS 5 Neural Rendering (NVIDIA NGX "dlssnr") detail synthesis for ffplay.
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

#ifndef FFTOOLS_FFPLAY_NGX_H
#define FFTOOLS_FFPLAY_NGX_H

/* Shared between ffplay_ngx.c (the player side) and ffplay_ngxshim.c (the
 * forwarder DLL); keep the two in sync through this header only. */

/* Model knobs. The snippet reads these on every evaluate except preset, which
 * is baked in when the feature is created. Defaults are what the model itself
 * uses; none of it is documented by NVIDIA. */
typedef struct FFNRTune {
    float intensity;        /* DLSSNR.Intensity, 1.0                        */
    float local_structure;  /* DLSSNR.LocalStructureStrength, 1.0           */
    float local_tone;       /* DLSSNR.LocalToneStrength, 1.0                */
    float skin_structure;   /* DLSSNR.SkinStructureStrength; -1 = follow the
                             * local structure term, the model's own default,
                             * which is not the same as a strength of 0      */
    int   style;            /* DLSSNR.Style                                  */
    int   auto_mask;        /* DLSSNR.UseAutoMask                            */
    int   reset;            /* DLSSNR.Reset - drop temporal history (cut/seek)*/
} FFNRTune;

/* Forwarder entry points, resolved by name out of nvngx.dll_ffplay.dll.
 * All of them return an NVSDK_NGX_Result (0x1 == success) except ffnr_load. */
typedef int      (*PFN_ffnr_load)(const wchar_t *snippet_path);
typedef unsigned (*PFN_ffnr_init)(void *d3d12_device, const wchar_t *data_path);
typedef unsigned (*PFN_ffnr_create)(void *cmdlist, int w, int h, int preset,
                                    int style, void **out_handle);
typedef unsigned (*PFN_ffnr_evaluate)(void *cmdlist, void *handle, void *color,
                                      void *output, int w, int h,
                                      const FFNRTune *tune);
typedef unsigned (*PFN_ffnr_release)(void *handle);
typedef void     (*PFN_ffnr_shutdown)(void);

#ifndef FFNR_SHIM_BUILD

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

/* Bring the neural-rendering pass up on the decoder's D3D11 device. Safe to
 * call repeatedly; returns 0 once ready, negative when unavailable (no
 * snippet DLL, no D3D12, pre-Blackwell GPU, ...).
 *
 * All three entry points below must be called with the hardware device lock
 * held - they issue D3D11 work on the decoder's immediate context, and that
 * lock is not recursive. */
int  ngx_nr_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx);

/* Size the shared textures and (re)create the feature. in_tex is the player's
 * VideoProcessor output, which must have been created shareable. On success
 * *out_tex receives the D3D11 texture the model writes into - the caller
 * registers it with GL and composes it over the original. */
int  ngx_nr_ensure(struct ID3D11Texture2D *in_tex, int w, int h,
                   struct ID3D11Texture2D **out_tex);

/* Run the model for one frame: in_tex -> out_tex, fenced against D3D11 both
 * ways. Returns 0 on success. */
int  ngx_nr_run(void);

/* The player's copy of the tuning state, so the hotkeys and the HUD have one
 * place to read and write. */
FFNRTune *ngx_nr_tune(void);

void ngx_nr_reset_history(void);   /* seek / cut / format change */
int  ngx_nr_available(void);       /* 1 once the model is loaded and sized */

/* Release the feature and the shared textures but keep the D3D12 device and
 * the NGX runtime alive. A resolution change or a toggle goes through here:
 * the snippet is not built to be initialised, shut down and initialised again
 * inside one process, and rebuilding just the feature is cheap by comparison. */
void ngx_nr_free_size(void);

/* Full teardown, at exit only. */
void ngx_nr_uninit(void);

#endif /* !FFNR_SHIM_BUILD */

#endif /* FFTOOLS_FFPLAY_NGX_H */
