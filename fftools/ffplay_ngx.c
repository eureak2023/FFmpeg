/*
 * DLSS 5 Neural Rendering (NGX "dlssnr") detail synthesis for ffplay.
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
 *
 * What this does
 * --------------
 * NVIDIA's neural-rendering model ("cg2r" inside the snippet) resynthesises
 * fine detail across a whole frame. Games reach it through the upscaler; for a
 * video player it is a way to put back the texture that a low-bitrate encode
 * threw away - pores, cloth weave, foliage - rather than just sharpening the
 * mush that is left.
 *
 * Everything here is undocumented. What it took to make it run, all of it
 * established by asking the snippet directly (see ffplay_ngxshim.c for the
 * caller check and the parameter-object layout):
 *   - the feature id is NVSDK_NGX_Feature_Reserved14;
 *   - the D3D11 backend is a stub, so the pass runs on a private D3D12 device
 *     sharing textures with the decoder's D3D11 device;
 *   - motion vectors and depth are optional, which is what makes this usable
 *     on plain video at all - an encoded frame has neither.
 *
 * The model returns a complete picture, not a correction: its absolute
 * luminance is arbitrary (measured 1.05x to 1.4x off, scene-dependent) and its
 * hue drifts. Composing that back over the original is done in GL, in
 * ffplay_fsr.c; this file only produces the answer.
 *
 * Cost on an RTX 5080, measured end to end: 2.8 ms at 1280x544, ~6 ms for the
 * first frame after a resize.
 *
 * Requires an RTX 50 series GPU (the snippet demands Blackwell2) and
 * nvngx_dlssnr.dll beside ffplay.exe. Absent any of that the pass reports
 * unavailable and the player carries on exactly as before.
 */

#include "config.h"

#if defined(_WIN32) && CONFIG_D3D11VA

#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "ffplay_ngx.h"
#include "ffplay_res.h"

#define NGX_OK 0x1u

/* Two command allocators so the next frame can be recorded while the previous
 * one is still in flight; the CPU only ever waits on work two frames old. */
#define NR_FRAMES 2

static struct {
    int state;                  /* 0 = untried, 1 = ready, -1 = unavailable */

    HMODULE shim;
    PFN_ffnr_load     f_load;
    PFN_ffnr_init     f_init;
    PFN_ffnr_create   f_create;
    PFN_ffnr_evaluate f_evaluate;
    PFN_ffnr_release  f_release;
    PFN_ffnr_shutdown f_shutdown;

    /* D3D11 side: the decoder's device, plus the 11.4 interfaces needed to
     * drive a shared fence. */
    ID3D11Device         *dev;
    ID3D11DeviceContext  *ctx;
    ID3D11Device5        *dev5;
    ID3D11DeviceContext4 *ctx4;

    /* D3D12 side. */
    ID3D12Device              *dev12;
    ID3D12CommandQueue        *queue;
    ID3D12CommandAllocator    *alloc[NR_FRAMES];
    ID3D12GraphicsCommandList *list;
    UINT64                     alloc_val[NR_FRAMES];
    int                        frame;

    /* One fence, opened on both sides of the same shared handle. */
    ID3D11Fence *fence11;
    ID3D12Fence *fence12;
    HANDLE       fence_share;
    HANDLE       fence_event;
    UINT64       fence_val;

    /* Per-size state. in_tex is the player's VideoProcessor output (created
     * shareable by ffplay_fsr.c); out_tex is ours. */
    ID3D11Texture2D *in_tex, *out_tex;
    ID3D12Resource  *in12, *out12;
    HANDLE           in_share, out_share;
    void            *feature;
    int              w, h;

    FFNRTune tune;
} nr = {
    .tune = { .intensity = 1.0f, .local_structure = 1.0f, .local_tone = 1.0f,
              .skin_structure = -1.0f, .style = 2, .auto_mask = 1, .reset = 1 },
};

FFNRTune *ngx_nr_tune(void)   { return &nr.tune; }
int  ngx_nr_available(void)   { return nr.state == 1 && nr.feature != NULL; }
void ngx_nr_reset_history(void) { nr.tune.reset = 1; }

/* ---- the forwarder DLL ---- */

/* Write the embedded forwarder to a private temp folder and load it from
 * there. It cannot simply be linked in: the snippet identifies its caller by
 * module path and only accepts one containing "nvngx.dll", so the calls have
 * to originate from a separately named module (see ffplay_ngxshim.c). */
static HMODULE nr_load_shim(void)
{
    static const wchar_t name[] = L"nvngx.dll_ffplay.dll";
    HMODULE self = GetModuleHandleW(NULL);
    HRSRC   res  = FindResourceW(self, MAKEINTRESOURCEW(IDR_NGX_SHIM_DLL),
                                 (LPCWSTR)RT_RCDATA);
    HGLOBAL blob;
    const void *data;
    DWORD len, n;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    HANDLE f;
    HMODULE mod;

    if (!res)
        return NULL;
    blob = LoadResource(self, res);
    if (!blob)
        return NULL;
    data = LockResource(blob);
    len  = SizeofResource(self, res);
    if (!data || !len)
        return NULL;

    /* Temp first, then next to the executable. With TMP and TEMP unset or
     * bogus GetTempPathW hands back the Windows directory, which is not
     * writable - and then the whole feature would be lost to an environment
     * quirk rather than to anything about the GPU. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0) {
            n = GetTempPathW(MAX_PATH, dir);
            if (!n || n >= MAX_PATH - 32)
                continue;
            wcscat(dir, L"ffplay-ngx");
            CreateDirectoryW(dir, NULL);   /* ERROR_ALREADY_EXISTS is fine */
        } else {
            wchar_t *slash;

            if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
                continue;
            slash = wcsrchr(dir, L'\\');
            if (!slash)
                continue;
            *slash = 0;
        }
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
        path[MAX_PATH - 1] = 0;

        /* A second ffplay already has this file mapped, so the write fails
         * with a sharing violation - the copy on disk is ours and current
         * either way, so load it regardless. */
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            BOOL ok = WriteFile(f, data, len, &wrote, NULL) && wrote == len;

            CloseHandle(f);
            if (!ok) {
                DeleteFileW(path);
                continue;
            }
        }
        mod = LoadLibraryW(path);
        if (mod)
            return mod;
        av_log(NULL, AV_LOG_VERBOSE,
               "DLSS-NR: LoadLibrary(%ls) failed with %lu\n", path,
               (unsigned long)GetLastError());
    }
    return NULL;
}

/* nvngx_dlssnr.dll is 165 MB of driver blob, so it is not embedded: it sits
 * beside the executable, or in ../bin next to it (where this repo keeps the
 * other vendored runtime pieces), or wherever FFPLAY_DLSSNR_DLL points. */
static int nr_find_snippet(wchar_t *out, size_t out_len)
{
    static const wchar_t dll[] = L"nvngx_dlssnr.dll";
    wchar_t exe[MAX_PATH], *slash;
    DWORD n;

    n = GetEnvironmentVariableW(L"FFPLAY_DLSSNR_DLL", out, (DWORD)out_len);
    if (n && n < out_len)
        return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES ? 0 : -1;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return -1;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return -1;
    *slash = 0;

    _snwprintf(out, out_len, L"%ls\\%ls", exe, dll);
    out[out_len - 1] = 0;
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 0;

    _snwprintf(out, out_len, L"%ls\\..\\bin\\%ls", exe, dll);
    out[out_len - 1] = 0;
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
        return 0;
    return -1;
}

/* ---- fence plumbing ---- */

/* Hand the GPU work queued on D3D11 over to the D3D12 queue and back again.
 * Both sides hold the same fence, opened from one shared handle, so this is a
 * GPU-side handoff with no CPU stall in it. */
static void nr_signal_d3d11(void)
{
    ID3D11DeviceContext4_Signal(nr.ctx4, nr.fence11, ++nr.fence_val);
    ID3D11DeviceContext_Flush(nr.ctx);
}

static void nr_wait_d3d11(UINT64 value)
{
    ID3D11DeviceContext4_Wait(nr.ctx4, nr.fence11, value);
}

static void nr_cpu_wait(UINT64 value)
{
    if (!value || ID3D12Fence_GetCompletedValue(nr.fence12) >= value)
        return;
    if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(nr.fence12, value, nr.fence_event)))
        WaitForSingleObject(nr.fence_event, 2000);
}

static void nr_barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { 0 };

    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    ID3D12GraphicsCommandList_ResourceBarrier(nr.list, 1, &b);
}

/* ---- setup ---- */

void ngx_nr_free_size(void)
{
    if (nr.feature) {
        nr_cpu_wait(nr.fence_val);
        nr.f_release(nr.feature);
        nr.feature = NULL;
    }
    if (nr.in12)  { ID3D12Resource_Release(nr.in12);  nr.in12  = NULL; }
    if (nr.out12) { ID3D12Resource_Release(nr.out12); nr.out12 = NULL; }
    if (nr.in_share)  { CloseHandle(nr.in_share);  nr.in_share  = NULL; }
    if (nr.out_share) { CloseHandle(nr.out_share); nr.out_share = NULL; }
    if (nr.out_tex) { ID3D11Texture2D_Release(nr.out_tex); nr.out_tex = NULL; }
    nr.in_tex = NULL;
    nr.w = nr.h = 0;
}

static int nr_open_shared(ID3D11Texture2D *tex, HANDLE *share,
                          ID3D12Resource **out)
{
    IDXGIResource1 *dxgi = NULL;
    HRESULT hr;

    hr = ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dxgi);
    if (FAILED(hr))
        return -1;
    hr = IDXGIResource1_CreateSharedHandle(dxgi, NULL, GENERIC_ALL, NULL, share);
    IDXGIResource1_Release(dxgi);
    if (FAILED(hr))
        return -1;
    hr = ID3D12Device_OpenSharedHandle(nr.dev12, *share, &IID_ID3D12Resource,
                                       (void **)out);
    return SUCCEEDED(hr) ? 0 : -1;
}

int ngx_nr_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL,
                                                    REFIID, void **);
    PFN_D3D12CreateDevice create12;
    D3D12_COMMAND_QUEUE_DESC qd = { 0 };
    IDXGIDevice  *dxgi_dev = NULL;
    IDXGIAdapter *adapter  = NULL;
    HMODULE d3d12;
    wchar_t snippet[MAX_PATH], data_dir[MAX_PATH];
    unsigned res;

    if (nr.state == 1 && nr.dev == dev)
        return 0;
    if (nr.state == 1) {
        /* Same GPU, different D3D11 device - opening another file builds a
         * new one. The D3D12 device and the NGX runtime hang off the adapter
         * and survive; the fence and the 11.4 views do not. */
        ngx_nr_free_size();
        if (nr.fence12) { ID3D12Fence_Release(nr.fence12); nr.fence12 = NULL; }
        if (nr.fence11) { ID3D11Fence_Release(nr.fence11); nr.fence11 = NULL; }
        if (nr.fence_share) { CloseHandle(nr.fence_share); nr.fence_share = NULL; }
        if (nr.ctx4) { ID3D11DeviceContext4_Release(nr.ctx4); nr.ctx4 = NULL; }
        if (nr.dev5) { ID3D11Device5_Release(nr.dev5); nr.dev5 = NULL; }
        nr.dev = dev;
        nr.ctx = ctx;
        if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&nr.dev5)) ||
            FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4,
                                                      (void **)&nr.ctx4)) ||
            FAILED(ID3D11Device5_CreateFence(nr.dev5, 0, D3D11_FENCE_FLAG_SHARED,
                                             &IID_ID3D11Fence, (void **)&nr.fence11)) ||
            FAILED(ID3D11Fence_CreateSharedHandle(nr.fence11, NULL, GENERIC_ALL, NULL,
                                                  &nr.fence_share)) ||
            FAILED(ID3D12Device_OpenSharedHandle(nr.dev12, nr.fence_share,
                                                 &IID_ID3D12Fence, (void **)&nr.fence12))) {
            av_log(NULL, AV_LOG_WARNING,
                   "DLSS-NR: could not re-bind to the new D3D11 device\n");
            ngx_nr_uninit();
            nr.state = -1;
            return -1;
        }
        nr.fence_val = 0;
        for (int i = 0; i < NR_FRAMES; i++)
            nr.alloc_val[i] = 0;
        return 0;
    }
    if (nr.state)
        return -1;
    nr.state = -1;                      /* every failure below is permanent */

    nr.dev = dev;
    nr.ctx = ctx;

    if (nr_find_snippet(snippet, MAX_PATH) < 0) {
        av_log(NULL, AV_LOG_INFO,
               "DLSS-NR: nvngx_dlssnr.dll not found beside ffplay.exe; "
               "neural rendering off\n");
        return -1;
    }

    nr.shim = nr_load_shim();
    if (!nr.shim) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: could not stage the NGX forwarder\n");
        return -1;
    }
    nr.f_load     = (PFN_ffnr_load)    (void *)GetProcAddress(nr.shim, "ffnr_load");
    nr.f_init     = (PFN_ffnr_init)    (void *)GetProcAddress(nr.shim, "ffnr_init");
    nr.f_create   = (PFN_ffnr_create)  (void *)GetProcAddress(nr.shim, "ffnr_create");
    nr.f_evaluate = (PFN_ffnr_evaluate)(void *)GetProcAddress(nr.shim, "ffnr_evaluate");
    nr.f_release  = (PFN_ffnr_release) (void *)GetProcAddress(nr.shim, "ffnr_release");
    nr.f_shutdown = (PFN_ffnr_shutdown)(void *)GetProcAddress(nr.shim, "ffnr_shutdown");
    if (!nr.f_load || !nr.f_init || !nr.f_create || !nr.f_evaluate ||
        nr.f_load(snippet) != 0) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: %ls could not be loaded\n", snippet);
        return -1;
    }

    /* 11.4 is what carries fences; it has shipped since Windows 10 1703, but
     * an old runtime just means no neural rendering rather than a failure. */
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5, (void **)&nr.dev5)) ||
        FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4,
                                                  (void **)&nr.ctx4))) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: D3D11.4 fences unavailable\n");
        return -1;
    }

    /* Run D3D12 on the very adapter the decoder is on, or the shared handles
     * would cross GPUs. Taking the adapter straight off the D3D11 device is
     * both simpler and safer than matching LUIDs through a DXGI factory. */
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev)) ||
        FAILED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter))) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: cannot reach the DXGI adapter\n");
        goto fail;
    }

    /* Loaded by hand so ffplay.exe keeps its import table free of d3d12.dll,
     * exactly like nvofapi and CUDA on the frame-generation path. */
    d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12)
        d3d12 = LoadLibraryA("d3d12.dll");
    create12 = d3d12 ? (PFN_D3D12CreateDevice)(void *)
                       GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    if (!create12 ||
        FAILED(create12((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                        &IID_ID3D12Device, (void **)&nr.dev12))) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: no D3D12 device on this adapter\n");
        goto fail;
    }

    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(ID3D12Device_CreateCommandQueue(nr.dev12, &qd, &IID_ID3D12CommandQueue,
                                               (void **)&nr.queue)))
        goto fail;
    for (int i = 0; i < NR_FRAMES; i++)
        if (FAILED(ID3D12Device_CreateCommandAllocator(nr.dev12,
                D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                (void **)&nr.alloc[i])))
            goto fail;
    if (FAILED(ID3D12Device_CreateCommandList(nr.dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              nr.alloc[0], NULL,
                                              &IID_ID3D12GraphicsCommandList,
                                              (void **)&nr.list)))
        goto fail;
    ID3D12GraphicsCommandList_Close(nr.list);

    if (FAILED(ID3D11Device5_CreateFence(nr.dev5, 0, D3D11_FENCE_FLAG_SHARED,
                                         &IID_ID3D11Fence, (void **)&nr.fence11)) ||
        FAILED(ID3D11Fence_CreateSharedHandle(nr.fence11, NULL, GENERIC_ALL, NULL,
                                              &nr.fence_share)) ||
        FAILED(ID3D12Device_OpenSharedHandle(nr.dev12, nr.fence_share,
                                             &IID_ID3D12Fence, (void **)&nr.fence12))) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: shared fence setup failed\n");
        goto fail;
    }
    nr.fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!nr.fence_event)
        goto fail;

    /* The snippet writes its own logs here when a driver-side log level is
     * set; it must be a directory it can create files in. */
    if (!GetTempPathW(MAX_PATH - 16, data_dir))
        goto fail;
    wcscat(data_dir, L"ffplay-ngx");
    CreateDirectoryW(data_dir, NULL);

    res = nr.f_init(nr.dev12, data_dir);
    if (res != NGX_OK) {
        /* 0xBAD00001 is FeatureNotSupported, which is what a pre-Blackwell
         * GPU (or a driver without the model) answers. */
        av_log(NULL, AV_LOG_WARNING,
               "DLSS-NR: NGX init refused (0x%X); neural rendering off\n", res);
        goto fail;
    }

    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);
    av_log(NULL, AV_LOG_INFO, "DLSS-NR: neural rendering ready (%ls)\n", snippet);
    nr.state = 1;
    return 0;

fail:
    if (adapter)
        IDXGIAdapter_Release(adapter);
    if (dxgi_dev)
        IDXGIDevice_Release(dxgi_dev);
    ngx_nr_uninit();
    nr.state = -1;
    return -1;
}

int ngx_nr_ensure(ID3D11Texture2D *in_tex, int w, int h,
                  ID3D11Texture2D **out_tex)
{
    D3D11_TEXTURE2D_DESC td = { 0 };
    unsigned res;

    if (nr.state != 1)
        return -1;
    if (nr.feature && nr.w == w && nr.h == h && nr.in_tex == in_tex) {
        *out_tex = nr.out_tex;
        return 0;
    }
    ngx_nr_free_size();

    /* The model writes through a UAV, and B8G8R8A8 cannot be one; R8G8B8A8
     * can, and the model is equally happy reading the player's BGRA frame and
     * writing RGBA (verified against both). GL sorts the channel order out
     * when it registers the texture. */
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(ID3D11Device_CreateTexture2D(nr.dev, &td, NULL, &nr.out_tex))) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: output texture creation failed\n");
        goto fail;
    }

    if (nr_open_shared(in_tex, &nr.in_share, &nr.in12) < 0 ||
        nr_open_shared(nr.out_tex, &nr.out_share, &nr.out12) < 0) {
        av_log(NULL, AV_LOG_WARNING, "DLSS-NR: D3D11/D3D12 texture sharing failed\n");
        goto fail;
    }

    /* CreateFeature records its own setup work, so it needs an open list and
     * a submission before the first evaluate. */
    nr_cpu_wait(nr.alloc_val[0]);
    ID3D12CommandAllocator_Reset(nr.alloc[0]);
    ID3D12GraphicsCommandList_Reset(nr.list, nr.alloc[0], NULL);
    res = nr.f_create(nr.list, w, h, 0 /* preset: model default */,
                      nr.tune.style, &nr.feature);
    ID3D12GraphicsCommandList_Close(nr.list);
    if (res != NGX_OK || !nr.feature) {
        av_log(NULL, AV_LOG_WARNING,
               "DLSS-NR: CreateFeature failed (0x%X) at %dx%d\n", res, w, h);
        nr.feature = NULL;
        goto fail;
    }
    ID3D12CommandQueue_ExecuteCommandLists(nr.queue, 1,
                                           (ID3D12CommandList *const *)&nr.list);
    ID3D12CommandQueue_Signal(nr.queue, nr.fence12, ++nr.fence_val);
    nr.alloc_val[0] = nr.fence_val;
    nr_cpu_wait(nr.fence_val);

    nr.in_tex = in_tex;
    nr.w = w;
    nr.h = h;
    nr.frame = 0;
    nr.tune.reset = 1;
    *out_tex = nr.out_tex;
    av_log(NULL, AV_LOG_INFO, "DLSS-NR: model up at %dx%d (style %d)\n",
           w, h, nr.tune.style);
    return 0;

fail:
    ngx_nr_free_size();
    return -1;
}

int ngx_nr_run(void)
{
    int slot = nr.frame % NR_FRAMES;
    unsigned res;

    if (nr.state != 1 || !nr.feature)
        return -1;

    /* Everything D3D11 has queued for this frame - the VideoProcessor blt
     * into the shared input - must land before the model reads it. */
    nr_signal_d3d11();
    ID3D12CommandQueue_Wait(nr.queue, nr.fence12, nr.fence_val);

    nr_cpu_wait(nr.alloc_val[slot]);
    ID3D12CommandAllocator_Reset(nr.alloc[slot]);
    ID3D12GraphicsCommandList_Reset(nr.list, nr.alloc[slot], NULL);

    /* Shared resources sit in COMMON whenever the other API might touch them,
     * so each frame transitions in and straight back out. */
    nr_barrier(nr.in12,  D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    nr_barrier(nr.out12, D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    res = nr.f_evaluate(nr.list, nr.feature, nr.in12, nr.out12,
                        nr.w, nr.h, &nr.tune);

    nr_barrier(nr.in12,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
    nr_barrier(nr.out12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList_Close(nr.list);

    if (res != NGX_OK) {
        av_log(NULL, AV_LOG_WARNING,
               "DLSS-NR: evaluate failed (0x%X); neural rendering off\n", res);
        ngx_nr_free_size();
        return -1;
    }

    ID3D12CommandQueue_ExecuteCommandLists(nr.queue, 1,
                                           (ID3D12CommandList *const *)&nr.list);
    ID3D12CommandQueue_Signal(nr.queue, nr.fence12, ++nr.fence_val);
    nr.alloc_val[slot] = nr.fence_val;
    nr.frame++;

    /* Hand back to D3D11 (and through it to GL, which the interop lock
     * synchronises against the D3D11 timeline). */
    nr_wait_d3d11(nr.fence_val);
    nr.tune.reset = 0;
    return 0;
}

void ngx_nr_uninit(void)
{
    ngx_nr_free_size();
    if (nr.f_shutdown && nr.state == 1)
        nr.f_shutdown();
    if (nr.list)    { ID3D12GraphicsCommandList_Release(nr.list); nr.list = NULL; }
    for (int i = 0; i < NR_FRAMES; i++)
        if (nr.alloc[i]) { ID3D12CommandAllocator_Release(nr.alloc[i]); nr.alloc[i] = NULL; }
    if (nr.queue)   { ID3D12CommandQueue_Release(nr.queue); nr.queue = NULL; }
    if (nr.fence12) { ID3D12Fence_Release(nr.fence12); nr.fence12 = NULL; }
    if (nr.fence11) { ID3D11Fence_Release(nr.fence11); nr.fence11 = NULL; }
    if (nr.fence_share) { CloseHandle(nr.fence_share); nr.fence_share = NULL; }
    if (nr.fence_event) { CloseHandle(nr.fence_event); nr.fence_event = NULL; }
    if (nr.dev12)   { ID3D12Device_Release(nr.dev12); nr.dev12 = NULL; }
    if (nr.ctx4)    { ID3D11DeviceContext4_Release(nr.ctx4); nr.ctx4 = NULL; }
    if (nr.dev5)    { ID3D11Device5_Release(nr.dev5); nr.dev5 = NULL; }
    /* nr.shim stays loaded: the snippet holds CUDA state that does not
     * survive being unloaded and reloaded in the same process. */
    if (nr.state == 1)
        nr.state = 0;
}

#else /* !_WIN32 || !CONFIG_D3D11VA */

#include "ffplay_ngx.h"

static FFNRTune nr_tune_stub;

int  ngx_nr_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx)
{ (void)dev; (void)ctx; return -1; }
int  ngx_nr_ensure(struct ID3D11Texture2D *in_tex, int w, int h,
                   struct ID3D11Texture2D **out_tex)
{ (void)in_tex; (void)w; (void)h; (void)out_tex; return -1; }
int  ngx_nr_run(void)            { return -1; }
FFNRTune *ngx_nr_tune(void)      { return &nr_tune_stub; }
void ngx_nr_reset_history(void)  { }
int  ngx_nr_available(void)      { return 0; }
void ngx_nr_free_size(void)      { }
void ngx_nr_uninit(void)         { }

#endif
