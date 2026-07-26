/*
 * RIFE (Real-Time Intermediate Flow Estimation) frame interpolation for ffplay,
 * running on ncnn + Vulkan.
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

#ifndef FFTOOLS_FFPLAY_RIFE_H
#define FFTOOLS_FFPLAY_RIFE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A learned alternative to the NVIDIA-optical-flow frame generation in
 * ffplay_fsr.c. The hardware flow works on a 4x4 block grid, so around a moving
 * subject one block mixes the subject's motion with the background's and the
 * warp smears the silhouette; RIFE estimates flow per pixel with a small
 * convolutional network instead, which is what removes that shimmering edge.
 *
 * Measured on an RTX 5080 (in-process, no image I/O): 4.9 ms per generated
 * 720p frame, 10.2 ms at 1080p, 45.9 ms at 4K. Only 1080p and below leave room
 * inside a 23.976 fps frame interval, so the caller keeps using the optical-flow
 * path above that (see rife_size_supported).
 *
 * Everything degrades gracefully: if the model or Vulkan is unavailable every
 * entry point reports failure and the caller falls back to the existing path.
 */

/* Load the model and create the Vulkan/ncnn session. Safe to call repeatedly;
 * only the first call does work. Returns 0 on success, negative on failure
 * (missing model directory, no Vulkan device, ...). */
int  rife_init(void);

/* Nonzero once rife_init() has succeeded. */
int  rife_available(void);

/* Nonzero if a frame this size is worth interpolating with RIFE rather than
 * the optical-flow path (i.e. one generated frame fits the time budget). */
int  rife_size_supported(int w, int h);

/* Interpolate the frame at "timestep" (0 = rgb0, 1 = rgb1) between two
 * packed-RGB24 images of the same size. "out" must already point at
 * w * h * 3 writable bytes. Returns 0 on success. */
int  rife_interpolate(const uint8_t *rgb0, const uint8_t *rgb1,
                      int w, int h, float timestep, uint8_t *out);

/* Release the session. Call before the GPU/Vulkan device goes away. */
void rife_uninit(void);

#ifdef __cplusplus
}
#endif

#endif /* FFTOOLS_FFPLAY_RIFE_H */
