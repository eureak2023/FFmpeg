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
#include "ffplay_res.h"

#ifdef _WIN32
#include <windows.h>
#include <setjmp.h>
#endif

/* Model name, used only for log messages; the weights themselves are embedded
 * in the executable as RCDATA (see ffplay_res.rc) and loaded from memory. */
#define RIFE_MODEL_NAME "rife-v4.6"

/* Upper bound on what we hand to RIFE. Measured cost per generated frame on
 * an RTX 5080: 4.9 ms at 720p, 10.2 ms at 1080p, 45.9 ms at 4K - against a
 * budget of 41.7 ms for a 23.976 fps source at 2x. 4K was tried at 9 MP and
 * did stutter in practice, matching the measurement, so the cap is back just
 * above 1080p; larger sources stay on the optical-flow warp. (1440p, 3.7 MP,
 * has not been measured - it would likely fit, if ever wanted.) */
#define RIFE_MAX_PIXELS (2200000)

static RIFE *rife_ctx;
static int   rife_state;        /* 0 = untried, 1 = ready, -1 = unavailable */

/* Point at an embedded RCDATA resource; returns NULL and leaves *len untouched
 * if the resource is missing. The returned pointer is owned by the loaded
 * module image and stays valid for the process lifetime. */
#ifdef _WIN32
static const unsigned char *rife_resource(int id, size_t *len)
{
    HMODULE mod = GetModuleHandleW(NULL);
    HRSRC   res = FindResourceW(mod, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL h;

    if (!res)
        return NULL;
    h = LoadResource(mod, res);
    if (!h)
        return NULL;
    *len = SizeofResource(mod, res);
    return (const unsigned char *)LockResource(h);
}
#endif

#ifdef _WIN32
/* Write one embedded blob to <dir>\<name>. Returns 0 on success. */
static int rife_write_file(const std::wstring& dir, const wchar_t* name,
                           const unsigned char* data, size_t len)
{
    std::wstring path = dir + L"\\" + name;
    FILE *f = _wfopen(path.c_str(), L"wb");
    size_t wrote;

    if (!f)
        return -1;
    wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len ? 0 : -1;
}

/* Extract the embedded flownet model to a private temp directory and return
 * its path, so the validated file-based loader can read it. ncnn's in-memory
 * loader references (and internally writes to) the buffer, which faults on the
 * read-only resource section, so a real file is used instead. Empty on error. */
static std::wstring rife_extract_model(const unsigned char* param, size_t param_len,
                                       const unsigned char* bin, size_t bin_len)
{
    wchar_t tmp[MAX_PATH];
    DWORD   n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir;

    if (!n || n >= MAX_PATH)
        return std::wstring();
    dir.assign(tmp, n);
    dir += L"ffplay-rife-v4.6";
    /* ERROR_ALREADY_EXISTS is fine; any other failure is fatal below. */
    CreateDirectoryW(dir.c_str(), NULL);

    if (rife_write_file(dir, L"flownet.param", param, param_len) != 0 ||
        rife_write_file(dir, L"flownet.bin",   bin,   bin_len)   != 0)
        return std::wstring();
    return dir;
}
#endif

#ifdef _WIN32
/* ncnn's Vulkan weight upload segfaults on some GPUs/drivers (observed on the
 * GTX 1660 Ti) instead of returning an error. MinGW's GCC has no __try/__except,
 * so a vectored exception handler catches the access violation and longjmps
 * back, turning the crash into a clean failure; the player then falls back to
 * optical-flow frame generation rather than dying at startup. Only faults on
 * the loading thread are handled, so a stray fault elsewhere is left alone. */
static jmp_buf       rife_crash_jmp;
static DWORD         rife_load_tid;

static LONG CALLBACK rife_crash_veh(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        GetCurrentThreadId() == rife_load_tid) {
        longjmp(rife_crash_jmp, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Returns load()'s value, or -2 if the load crashed. Holds no C++ locals that
 * would need unwinding across the longjmp. */
static int rife_guarded_load(RIFE *ctx, const std::wstring &dir)
{
    PVOID h;
    int   r;

    rife_load_tid = GetCurrentThreadId();
    h = AddVectoredExceptionHandler(1, rife_crash_veh);
    if (setjmp(rife_crash_jmp))
        r = -2;                 /* reached via longjmp after a crash */
    else
        r = ctx->load(dir);
    if (h)
        RemoveVectoredExceptionHandler(h);
    return r;
}
#endif

int rife_init(void)
{
    if (rife_state)
        return rife_state > 0 ? 0 : -1;
    rife_state = -1;

#ifdef _WIN32
    size_t         param_len = 0, bin_len = 0;
    const unsigned char *param = rife_resource(IDR_RIFE_FLOWNET_PARAM, &param_len);
    const unsigned char *bin   = rife_resource(IDR_RIFE_FLOWNET_BIN,   &bin_len);

    if (!param || !bin) {
        av_log(NULL, AV_LOG_WARNING,
               "RIFE: embedded model missing from the executable; "
               "using optical-flow frame generation\n");
        return -1;
    }

    /* ncnn needs its Vulkan instance up before any device is opened. */
    if (ncnn::create_gpu_instance() != 0 || ncnn::get_gpu_count() <= 0) {
        av_log(NULL, AV_LOG_WARNING, "RIFE: no Vulkan device available\n");
        return -1;
    }

    std::wstring dir = rife_extract_model(param, param_len, bin, bin_len);
    if (dir.empty()) {
        av_log(NULL, AV_LOG_WARNING,
               "RIFE: could not stage the embedded model; "
               "using optical-flow frame generation\n");
        ncnn::destroy_gpu_instance();
        return -1;
    }

    /* v4 models take an arbitrary timestep, which is what lets a single
     * source interval be filled with more than one generated frame. */
    rife_ctx = new RIFE(0 /* gpu 0 */, false /* tta */, false /* tta temporal */,
                        false /* uhd */, 1 /* threads */, false /* v2 */,
                        true /* v4 */);
    int lr = rife_guarded_load(rife_ctx, dir);
    if (lr != 0) {
        av_log(NULL, AV_LOG_WARNING,
               "RIFE: model load %s on this GPU; using optical-flow frame generation\n",
               lr == -2 ? "crashed" : "failed");
        /* After a crash inside the Vulkan driver, do not touch ncnn again
         * (delete/destroy could fault too); leak the instance and continue. */
        if (lr != -2) {
            delete rife_ctx;
            ncnn::destroy_gpu_instance();
        }
        rife_ctx = NULL;
        return -1;
    }

    av_log(NULL, AV_LOG_INFO, "RIFE: %s ready on %s\n", RIFE_MODEL_NAME,
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
