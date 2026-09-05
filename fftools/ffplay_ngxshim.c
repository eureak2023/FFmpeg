/*
 * NGX forwarder for the DLSS 5 Neural Rendering snippet.
 *
 * Built as a standalone DLL (nvngx.dll_ffplay.dll), embedded in ffplay.exe as
 * RCDATA and written to a temp folder at startup - it is never shipped loose.
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
 * Why this file exists at all
 * ---------------------------
 * nvngx_dlssnr.dll refuses to run for anyone but NGX itself: it walks back to
 * the calling module, reads its path, and bails out with "Error: Not called
 * from NGX runtime" unless that path contains "nvngx.dll". Normally the caller
 * is the driver's nvngx.dll dispatcher, but this driver's dispatcher has no
 * entry for the neural-rendering snippet at all (no "dlssnr" string in it), so
 * the feature can only be reached by driving the snippet directly.
 *
 * Hence the file name: every NGX call is made from inside this module, whose
 * path contains "nvngx.dll", so the check passes. OptiScaler ships a file with
 * the same trick and for the same stated reason; this is our own minimal
 * version of it, so no third-party binary has to be trusted or shipped.
 *
 * Two details are load-bearing:
 *   - NVSDK_NGX_Parameter is a C++ abstract class compiled by MSVC, and MSVC
 *     emits each overload group in *reverse* declaration order. That puts
 *     Set(float) at vtable slot 6 rather than slot 1, which is exactly where
 *     the snippet looks for it. param_vtbl below is hand-ordered to match.
 *   - Only NVSDK_NGX_D3D12_Init_Ext works. Plain NVSDK_NGX_D3D12_Init passes a
 *     null parameter object internally and the snippet answers
 *     FeatureNotSupported; the D3D11 backend answers the same thing whatever
 *     it is handed, because it is a stub ("DLSSNR: D3D11 is not yet
 *     supported"). D3D12 it is.
 */

#define COBJMACROS
#include <windows.h>
#include <string.h>

#define FFNR_SHIM_BUILD
#include "ffplay_ngx.h"

#define NGX_OK 0x1u
#define NGX_FAIL_NOT_INITIALIZED 0xBAD00007u
#define NGX_FAIL_INVALID_PARAM   0xBAD00005u

/* NVSDK_NGX_Feature_Reserved14. Found by asking the snippet: every other id in
 * the enum is refused, 14 creates. */
#define NGX_FEATURE_DLSSNR 14

/* NVSDK_NGX_VERSION_API_MACRO from the public NGX SDK (1.5.0). */
#define NGX_API_VERSION 0x0000015

typedef unsigned int NGXResult;
typedef struct NVSDK_NGX_Handle { unsigned int Id; } NVSDK_NGX_Handle;

/* ---- parameter object ---- */

enum { PV_ULL, PV_F, PV_D, PV_UI, PV_I, PV_RES, PV_PTR };

typedef struct {
    const char *name;
    int         type;
    union {
        unsigned long long ull;
        float              f;
        double             d;
        unsigned int       ui;
        int                i;
        void              *p;
    } v;
} PEntry;

/* Comfortably above the ~35 keys the snippet actually reads. */
#define PMAX 64

typedef struct Param {
    void  **vtbl;
    PEntry  e[PMAX];
    int     n;
} Param;

static PEntry *p_find(Param *p, const char *name)
{
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].name, name))
            return &p->e[i];
    return NULL;
}

/* Names are string literals owned by this module, so they can be held by
 * pointer rather than copied. */
static PEntry *p_slot(Param *p, const char *name, int type)
{
    PEntry *e = p_find(p, name);

    if (!e) {
        if (p->n >= PMAX)
            return &p->e[PMAX - 1];
        e = &p->e[p->n++];
        e->name = name;
    }
    e->type = type;
    return e;
}

static void set_ull(Param *p, const char *n, unsigned long long v) { p_slot(p, n, PV_ULL)->v.ull = v; }
static void set_f  (Param *p, const char *n, float v)              { p_slot(p, n, PV_F  )->v.f   = v; }
static void set_d  (Param *p, const char *n, double v)             { p_slot(p, n, PV_D  )->v.d   = v; }
static void set_ui (Param *p, const char *n, unsigned int v)       { p_slot(p, n, PV_UI )->v.ui  = v; }
static void set_i  (Param *p, const char *n, int v)                { p_slot(p, n, PV_I  )->v.i   = v; }
static void set_res(Param *p, const char *n, void *v)              { p_slot(p, n, PV_RES)->v.p   = v; }
static void set_ptr(Param *p, const char *n, void *v)              { p_slot(p, n, PV_PTR)->v.p   = v; }

/* NGX's own parameter store converts between the numeric types on read, and
 * the snippet relies on that - it stores widths as UI and reads some of them
 * back as float. Missing keys must answer with a failure rather than a zero:
 * that is how the snippet learns that the optional guides (motion vectors,
 * depth, control mask, UI overlay) were not supplied, which is the whole
 * reason this works on plain video. */
#define GET_NUM(fn, ctype)                                                     \
static NGXResult fn(Param *p, const char *n, ctype *out)                       \
{                                                                              \
    PEntry *e = p_find(p, n);                                                  \
                                                                               \
    if (!e)                                                                    \
        return NGX_FAIL_INVALID_PARAM;                                         \
    switch (e->type) {                                                         \
    case PV_ULL: *out = (ctype)e->v.ull; break;                                \
    case PV_F:   *out = (ctype)e->v.f;   break;                                \
    case PV_D:   *out = (ctype)e->v.d;   break;                                \
    case PV_UI:  *out = (ctype)e->v.ui;  break;                                \
    case PV_I:   *out = (ctype)e->v.i;   break;                                \
    default:     return NGX_FAIL_INVALID_PARAM;                                \
    }                                                                          \
    return NGX_OK;                                                             \
}
GET_NUM(get_ull, unsigned long long)
GET_NUM(get_f,   float)
GET_NUM(get_d,   double)
GET_NUM(get_ui,  unsigned int)
GET_NUM(get_i,   int)

static NGXResult get_ptr(Param *p, const char *n, void **out)
{
    PEntry *e = p_find(p, n);

    if (!e)
        return NGX_FAIL_INVALID_PARAM;
    *out = e->v.p;
    return NGX_OK;
}

static void p_reset(Param *p) { p->n = 0; }

/* MSVC vtable order: Set(void*), Set(ID3D12Resource*), Set(ID3D11Resource*),
 * Set(int), Set(unsigned), Set(double), Set(float), Set(unsigned long long),
 * then the eight Get overloads in the same reversed order, then Reset(). The
 * resource setters and getters differ only in the pointer type, so one
 * implementation serves both. */
static void *param_vtbl[] = {
    (void *)set_ptr, (void *)set_res, (void *)set_res, (void *)set_i,
    (void *)set_ui,  (void *)set_d,   (void *)set_f,   (void *)set_ull,
    (void *)get_ptr, (void *)get_ptr, (void *)get_ptr, (void *)get_i,
    (void *)get_ui,  (void *)get_d,   (void *)get_f,   (void *)get_ull,
    (void *)p_reset,
};

static Param g_param;

static void param_init(void)
{
    g_param.vtbl = param_vtbl;
    g_param.n    = 0;
}

/* ---- snippet entry points ---- */

typedef NGXResult (*PFN_InitExt)(unsigned long long appid, const wchar_t *path,
                                 void *device, unsigned int sdkver,
                                 const void *params);
typedef NGXResult (*PFN_Create)(void *cmdlist, int feature, void *params,
                                NVSDK_NGX_Handle **out);
typedef NGXResult (*PFN_Evaluate)(void *cmdlist, const NVSDK_NGX_Handle *h,
                                  const void *params, void *callback);
typedef NGXResult (*PFN_Release)(NVSDK_NGX_Handle *h);
typedef NGXResult (*PFN_Shutdown)(void);

static HMODULE      snippet;
static PFN_InitExt  ngx_init_ext;
static PFN_Create   ngx_create;
static PFN_Evaluate ngx_evaluate;
static PFN_Release  ngx_release;
static PFN_Shutdown ngx_shutdown;

/* The snippet identifies its caller from the return address, so the call must
 * not be tail-called out of this module: parking the result in a volatile
 * keeps a real frame here. The build also passes -fno-optimize-sibling-calls,
 * belt and braces. */
#define NGX_CALL(res, expr) do { volatile NGXResult r_ = (expr); (res) = r_; } while (0)

__declspec(dllexport)
int ffnr_load(const wchar_t *snippet_path)
{
    if (snippet)
        return 0;
    snippet = LoadLibraryW(snippet_path);
    if (!snippet)
        return -1;
    ngx_init_ext = (PFN_InitExt) GetProcAddress(snippet, "NVSDK_NGX_D3D12_Init_Ext");
    ngx_create   = (PFN_Create)  GetProcAddress(snippet, "NVSDK_NGX_D3D12_CreateFeature");
    ngx_evaluate = (PFN_Evaluate)GetProcAddress(snippet, "NVSDK_NGX_D3D12_EvaluateFeature");
    ngx_release  = (PFN_Release) GetProcAddress(snippet, "NVSDK_NGX_D3D12_ReleaseFeature");
    ngx_shutdown = (PFN_Shutdown)GetProcAddress(snippet, "NVSDK_NGX_D3D12_Shutdown");
    if (ngx_init_ext && ngx_create && ngx_evaluate)
        return 0;
    FreeLibrary(snippet);
    snippet = NULL;
    return -1;
}

__declspec(dllexport)
unsigned ffnr_init(void *d3d12_device, const wchar_t *data_path)
{
    NGXResult res;

    if (!ngx_init_ext)
        return NGX_FAIL_NOT_INITIALIZED;
    /* The parameter object is not read here, but it must not be null: that is
     * the only difference between Init_Ext (works) and Init (refused). */
    param_init();
    NGX_CALL(res, ngx_init_ext(0x46465000ull /* 'FFP\0' */, data_path,
                               d3d12_device, NGX_API_VERSION, &g_param));
    return res;
}

__declspec(dllexport)
unsigned ffnr_create(void *cmdlist, int w, int h, int preset, int style,
                     void **out_handle)
{
    NVSDK_NGX_Handle *handle = NULL;
    NGXResult res;

    if (!ngx_create)
        return NGX_FAIL_NOT_INITIALIZED;
    param_init();
    set_ui(&g_param, "CreationNodeMask",   1);
    set_ui(&g_param, "VisibilityNodeMask", 1);
    set_ui(&g_param, "DLSSNR.Width",  (unsigned)w);
    set_ui(&g_param, "DLSSNR.Height", (unsigned)h);
    /* Baked in at creation, unlike everything in FFNRTune. */
    set_ui(&g_param, "DLSSNR.Hint.Render.Preset", (unsigned)preset);
    set_ui(&g_param, "DLSSNR.Style", (unsigned)style);
    set_ui(&g_param, "PerfQualityValue", 0);

    NGX_CALL(res, ngx_create(cmdlist, NGX_FEATURE_DLSSNR, &g_param, &handle));
    *out_handle = handle;
    return res;
}

__declspec(dllexport)
unsigned ffnr_evaluate(void *cmdlist, void *handle, void *color, void *output,
                       int w, int h, const FFNRTune *t)
{
    NGXResult res;

    if (!ngx_evaluate || !handle)
        return NGX_FAIL_NOT_INITIALIZED;
    param_init();
    set_res(&g_param, "DLSSNR.Color",  color);
    set_res(&g_param, "DLSSNR.Output", output);
    set_ui (&g_param, "DLSSNR.Width",  (unsigned)w);
    set_ui (&g_param, "DLSSNR.Height", (unsigned)h);
    /* Whole-frame subrects. The model asks for these unconditionally. */
    set_ui (&g_param, "DLSSNR.ColorSubrectBaseX", 0);
    set_ui (&g_param, "DLSSNR.ColorSubrectBaseY", 0);
    set_ui (&g_param, "DLSSNR.ColorSubrectWidth",  (unsigned)w);
    set_ui (&g_param, "DLSSNR.ColorSubrectHeight", (unsigned)h);
    set_ui (&g_param, "DLSSNR.OutputSubrectBaseX", 0);
    set_ui (&g_param, "DLSSNR.OutputSubrectBaseY", 0);
    set_ui (&g_param, "DLSSNR.OutputSubrectWidth",  (unsigned)w);
    set_ui (&g_param, "DLSSNR.OutputSubrectHeight", (unsigned)h);
    /* DLSSNR.MVec, .Depth, .ControlMask, .UI and .Backbuffer are deliberately
     * left unset. A game supplies them from its engine; encoded video has
     * none of it, and the snippet is happy to work without them - it asks,
     * takes the failure, and falls back to its own automatic mask. */
    set_f  (&g_param, "DLSSNR.MVecScaleX", 1.0f);
    set_f  (&g_param, "DLSSNR.MVecScaleY", 1.0f);
    set_f  (&g_param, "DLSSNR.ScalingRatio", 1.0f);   /* model has no scaler  */
    set_i  (&g_param, "DLSSNR.DepthInverted", 0);
    set_i  (&g_param, "DLSSNR.UICorrection", 0);
    set_i  (&g_param, "DLSSNR.Enabled", 1);
    set_i  (&g_param, "DLSSNR.Reset", t->reset);
    set_i  (&g_param, "DLSSNR.UseAutoMask", t->auto_mask);
    set_ui (&g_param, "DLSSNR.Style", (unsigned)t->style);
    set_f  (&g_param, "DLSSNR.Intensity", t->intensity);
    set_f  (&g_param, "DLSSNR.LocalStructureStrength", t->local_structure);
    set_f  (&g_param, "DLSSNR.LocalToneStrength", t->local_tone);
    set_f  (&g_param, "DLSSNR.SkinStructureStrength", t->skin_structure);
    set_ui (&g_param, "CreationNodeMask",   1);
    set_ui (&g_param, "VisibilityNodeMask", 1);

    NGX_CALL(res, ngx_evaluate(cmdlist, (const NVSDK_NGX_Handle *)handle,
                               &g_param, NULL));
    return res;
}

__declspec(dllexport)
unsigned ffnr_release(void *handle)
{
    NGXResult res = NGX_OK;

    if (ngx_release && handle)
        NGX_CALL(res, ngx_release((NVSDK_NGX_Handle *)handle));
    return res;
}

__declspec(dllexport)
void ffnr_shutdown(void)
{
    NGXResult res;

    if (ngx_shutdown)
        NGX_CALL(res, ngx_shutdown());
    (void)res;
    /* The snippet is left loaded on purpose: it holds CUDA and driver state
     * that does not survive a reload cleanly, and the process is on its way
     * out by the time this runs. */
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
