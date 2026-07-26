/*
 * RIFE frame interpolation for ffplay, on ncnn + Vulkan.
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
 *
 * The model wrapper under fftools/rife/ (rife.cpp, warp.cpp and the compiled
 * shader headers) comes from rife-ncnn-vulkan by nihui, MIT licensed; see
 * fftools/rife/LICENSE. Two changes were needed to build it here:
 *   - ncnn dropped Option::use_shader_pack8, and no longer produces
 *     elempack-8 Vulkan blobs, so that pipeline is unreachable (warp.cpp);
 *   - MinGW's wide printf reads "%s" as a *narrow* string, which mangled the
 *     model path, so the path format uses "%ls" (rife.cpp).
 */

#include <string>
#include <vector>

#include "rife/rife.h"

extern "C" {
#include "libavutil/log.h"
}

#include "ffplay_rife.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* Model directory name, looked up next to the executable. */
#define RIFE_MODEL_DIR "rife-v4.6"

/* Upper bound on what we hand to RIFE. Measured cost per generated frame on
 * an RTX 5080: 4.9 ms at 720p, 10.2 ms at 1080p, 45.9 ms at 4K - against a
 * budget of 41.7 ms for a 23.976 fps source at 2x. 4K was tried at 9 MP and
 * did stutter in practice, matching the measurement, so the cap is back just
 * above 1080p; larger sources stay on the optical-flow warp. (1440p, 3.7 MP,
 * has not been measured - it would likely fit, if ever wanted.) */
#define RIFE_MAX_PIXELS (2200000)

static RIFE *rife_ctx;
static int   rife_state;        /* 0 = untried, 1 = ready, -1 = unavailable */

/* Absolute path of the model directory that sits beside ffplay.exe. Returns
 * an empty string if it cannot be determined. */
#ifdef _WIN32
static std::wstring rife_model_dir(void)
{
    wchar_t exe[MAX_PATH];
    DWORD   n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring p;

    if (!n || n >= MAX_PATH)
        return p;
    p.assign(exe, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return std::wstring();
    p.resize(slash + 1);
    p += L"" RIFE_MODEL_DIR;
    return p;
}
#endif

int rife_init(void)
{
    if (rife_state)
        return rife_state > 0 ? 0 : -1;
    rife_state = -1;

#ifdef _WIN32
    std::wstring dir = rife_model_dir();

    if (dir.empty()) {
        av_log(NULL, AV_LOG_WARNING, "RIFE: cannot locate the executable\n");
        return -1;
    }
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        av_log(NULL, AV_LOG_WARNING,
               "RIFE: model directory '%s' not found next to ffplay.exe; "
               "using optical-flow frame generation\n", RIFE_MODEL_DIR);
        return -1;
    }

    /* ncnn needs its Vulkan instance up before any device is opened. */
    if (ncnn::create_gpu_instance() != 0 || ncnn::get_gpu_count() <= 0) {
        av_log(NULL, AV_LOG_WARNING, "RIFE: no Vulkan device available\n");
        return -1;
    }

    /* v4 models take an arbitrary timestep, which is what lets a single
     * source interval be filled with more than one generated frame. */
    rife_ctx = new RIFE(0 /* gpu 0 */, false /* tta */, false /* tta temporal */,
                        false /* uhd */, 1 /* threads */, false /* v2 */,
                        true /* v4 */);
    if (rife_ctx->load(dir) != 0) {
        av_log(NULL, AV_LOG_WARNING, "RIFE: failed to load the model\n");
        delete rife_ctx;
        rife_ctx = NULL;
        ncnn::destroy_gpu_instance();
        return -1;
    }

    av_log(NULL, AV_LOG_INFO, "RIFE: %s ready on %s\n", RIFE_MODEL_DIR,
           ncnn::get_gpu_info(0).device_name());
    rife_state = 1;
    return 0;
#else
    return -1;
#endif
}

int rife_available(void)
{
    return rife_state > 0;
}

int rife_size_supported(int w, int h)
{
    return w > 0 && h > 0 && (int64_t)w * h <= RIFE_MAX_PIXELS;
}

int rife_interpolate(const uint8_t *rgb0, const uint8_t *rgb1,
                     int w, int h, float timestep, uint8_t *out)
{
    if (rife_state <= 0 || !rife_ctx || !rgb0 || !rgb1 || !out)
        return -1;
    if (w <= 0 || h <= 0)
        return -1;

    /* process_v4() writes into a caller-provided buffer and never allocates
     * the output itself, so all three Mats wrap memory we already own. */
    ncnn::Mat in0(w, h, (void *)rgb0, (size_t)3, 3);
    ncnn::Mat in1(w, h, (void *)rgb1, (size_t)3, 3);
    ncnn::Mat dst(w, h, (void *)out,  (size_t)3, 3);

    return rife_ctx->process(in0, in1, timestep, dst) == 0 ? 0 : -1;
}

void rife_uninit(void)
{
    if (rife_ctx) {
        delete rife_ctx;
        rife_ctx = NULL;
    }
    if (rife_state > 0)
        ncnn::destroy_gpu_instance();
    rife_state = 0;
}
