/*
 * Resource identifiers embedded into ffplay.exe via ffplay_res.rc.
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

#ifndef FFTOOLS_FFPLAY_RES_H
#define FFTOOLS_FFPLAY_RES_H

/* RIFE v4.6 model, embedded as RCDATA so ffplay.exe carries it and needs no
 * loose model folder beside it. flownet.param is the ncnn text prototext,
 * flownet.bin the binary weights; both are loaded from memory at startup
 * (see rife_init() in ffplay_rife.cpp). */
#define IDR_RIFE_FLOWNET_PARAM 101
#define IDR_RIFE_FLOWNET_BIN   102

#endif /* FFTOOLS_FFPLAY_RES_H */
