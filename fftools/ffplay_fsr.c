/*
 * AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) display path for ffplay.
 *
 * The EASU and RCAS shader passes are ports of AMD's official FSR 1.0
 * reference (ffx_fsr1.h / ffx_a.h, MIT licensed):
 *   Copyright (c) 2021 Advanced Micro Devices, Inc.
 *   https://github.com/GPUOpen-Effects/FidelityFX-FSR
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

#include "config.h"

#include <math.h>
#include <string.h>
#include <wchar.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/tx.h"

#if CONFIG_D3D11VA
#define COBJMACROS
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <dxgi.h>
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/pixfmt.h"
#include "libavcodec/codec_id.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <SDL_syswm.h>
#endif

#include "libavutil/avstring.h"

#include "ffplay_fsr.h"
#include "ffplay_rife.h"
#include "ffplay_ui.h"      /* ui_render_text: system font for the HUD */

/* Ask Windows for 1 ms timer resolution: av_usleep() otherwise rounds up to
 * the default 15.6 ms tick, which ruins frame-generation slot pacing (and
 * silently "fixes itself" whenever some other process holds the resolution).
 * No-op elsewhere. */
void fsr_timer_init(void)
{
#ifdef _WIN32
    HMODULE winmm = LoadLibraryA("winmm.dll");

    if (winmm) {
        typedef UINT (WINAPI *tbp_fn)(UINT);
        tbp_fn tbp = (tbp_fn)GetProcAddress(winmm, "timeBeginPeriod");

        if (tbp)
            tbp(1);
    }
    /* Windows 11 ignores the raised timer resolution for unfocused GUI
     * processes (power throttling), which wrecks frame pacing whenever the
     * player is not the foreground window. Opt out explicitly. */
    {
        typedef BOOL (WINAPI *spi_fn)(HANDLE, PROCESS_INFORMATION_CLASS,
                                      LPVOID, DWORD);
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        spi_fn spi = k32 ? (spi_fn)GetProcAddress(k32, "SetProcessInformation")
                         : NULL;

        if (spi) {
            PROCESS_POWER_THROTTLING_STATE st = { 0 };

            st.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            st.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION |
                             PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            st.StateMask   = 0; /* keep classic behavior: never throttle */
            spi(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
        }
    }
#endif
}

/* Sleep with sub-millisecond accuracy. Windows 11 may ignore the raised
 * timer resolution for unfocused processes, so use a high-resolution
 * waitable timer (Win10 1803+) that is exempt from tick rounding; falls
 * back to av_usleep(). Main-thread only. */
#if defined(_WIN32) && !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void fsr_precise_sleep(int64_t usec)
{
#ifdef _WIN32
    static HANDLE timer;
    static int timer_failed;
    LARGE_INTEGER due;

    if (usec <= 0)
        return;
    if (!timer && !timer_failed) {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer)
            timer_failed = 1;
    }
    due.QuadPart = -(usec * 10); /* relative, 100 ns units */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
#endif
    av_usleep(usec);
}

/* When launched by double-click (Explorer file association) the process
 * owns a console no one else uses - drop it so no empty terminal window
 * sits behind the video. Launches from a shell keep their console. */
/* ffplay links as a GUI-subsystem binary so double-click launches never
 * flash a console window. When stderr is not already connected (shell
 * redirection keeps its handle), attach to the parent console if there is
 * one, else log to %TEMP%\ffplay.log so failures stay diagnosable. */
void fsr_detach_console(void)
{
#ifdef _WIN32
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

    if (err && err != INVALID_HANDLE_VALUE)
        return; /* launched with stderr connected (shell/redirection) */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (freopen("CONOUT$", "w", stderr))
            setvbuf(stderr, NULL, _IONBF, 0);
        freopen("CONOUT$", "w", stdout);
    } else {
        char logpath[MAX_PATH + 16];
        DWORD n = GetTempPathA(sizeof(logpath) - 12, logpath);

        if (n > 0) {
            snprintf(logpath + n, sizeof(logpath) - n, "ffplay.log");
            if (freopen(logpath, "w", stderr))
                setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
#endif
}

/* Register (install=1) or remove (install=0) current-user .mp4/.mkv file
 * associations pointing at this executable. HKCU only - no admin rights
 * needed. On Windows 10+ an existing user default still wins until the
 * user picks ffplay via "Open with"; this makes it appear there and
 * become the default where none is set. Returns 0 on success. */
int fsr_register_associations(int install)
{
#ifdef _WIN32
    typedef LONG (WINAPI *create_fn)(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
                                     REGSAM, const SECURITY_ATTRIBUTES *,
                                     PHKEY, LPDWORD);
    typedef LONG (WINAPI *setval_fn)(HKEY, LPCSTR, DWORD, DWORD,
                                     const BYTE *, DWORD);
    typedef LONG (WINAPI *close_fn)(HKEY);
    typedef LONG (WINAPI *deltree_fn)(HKEY, LPCSTR);
    typedef LONG (WINAPI *delkeyval_fn)(HKEY, LPCSTR, LPCSTR);
    typedef void (WINAPI *shnotify_fn)(LONG, UINT, const void *, const void *);
    static const char *const exts[] = { ".mp4", ".mkv" };
    static const char progid[] = "ffplay.media";
    HMODULE adv = LoadLibraryA("advapi32.dll");
    HMODULE sh  = LoadLibraryA("shell32.dll");
    create_fn    reg_create;
    setval_fn    reg_setval;
    close_fn     reg_close;
    deltree_fn   reg_deltree;
    delkeyval_fn reg_delkeyval;
    shnotify_fn  sh_notify = NULL;
    char exe[MAX_PATH], buf[MAX_PATH + 16], path[160];
    int err = 0;

    if (!adv)
        return -1;
    reg_create    = (create_fn)   GetProcAddress(adv, "RegCreateKeyExA");
    reg_setval    = (setval_fn)   GetProcAddress(adv, "RegSetValueExA");
    reg_close     = (close_fn)    GetProcAddress(adv, "RegCloseKey");
    reg_deltree   = (deltree_fn)  GetProcAddress(adv, "RegDeleteTreeA");
    reg_delkeyval = (delkeyval_fn)GetProcAddress(adv, "RegDeleteKeyValueA");
    if (sh)
        sh_notify = (shnotify_fn)GetProcAddress(sh, "SHChangeNotify");
    if (!reg_create || !reg_setval || !reg_close || !reg_deltree ||
        !reg_delkeyval)
        return -1;
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return -1;

#define SET_KEY(keypath, valname, value) do {                                \
        HKEY k;                                                              \
        if (reg_create(HKEY_CURRENT_USER, keypath, 0, NULL, 0, KEY_WRITE,    \
                       NULL, &k, NULL) == 0) {                               \
            if (reg_setval(k, valname, 0, 1 /* REG_SZ */,                    \
                           (const BYTE *)(value),                            \
                           (DWORD)strlen(value) + 1) != 0)                   \
                err = -1;                                                    \
            reg_close(k);                                                    \
        } else                                                               \
            err = -1;                                                        \
    } while (0)

    if (install) {
        SET_KEY("Software\\Classes\\ffplay.media", NULL, "Media file (ffplay)");
        snprintf(buf, sizeof(buf), "\"%s\",0", exe);
        SET_KEY("Software\\Classes\\ffplay.media\\DefaultIcon", NULL, buf);
        snprintf(buf, sizeof(buf), "\"%s\" \"%%1\"", exe);
        SET_KEY("Software\\Classes\\ffplay.media\\shell\\open\\command", NULL, buf);
        for (int i = 0; i < (int)FF_ARRAY_ELEMS(exts); i++) {
            snprintf(path, sizeof(path), "Software\\Classes\\%s\\OpenWithProgids",
                     exts[i]);
            SET_KEY(path, progid, "");
            /* Becomes the default when the user has not chosen another
             * player; otherwise it shows up under "Open with". */
            snprintf(path, sizeof(path), "Software\\Classes\\%s", exts[i]);
            SET_KEY(path, NULL, progid);
        }
    } else {
        reg_deltree(HKEY_CURRENT_USER, "Software\\Classes\\ffplay.media");
        for (int i = 0; i < (int)FF_ARRAY_ELEMS(exts); i++) {
            snprintf(path, sizeof(path), "Software\\Classes\\%s\\OpenWithProgids",
                     exts[i]);
            reg_delkeyval(HKEY_CURRENT_USER, path, progid);
        }
    }
#undef SET_KEY

    if (sh_notify)
        sh_notify(0x08000000L /* SHCNE_ASSOCCHANGED */, 0 /* SHCNF_IDLIST */,
                  NULL, NULL);
    return err;
#else
    return -1;
#endif
}

/* All GL entry points are resolved through SDL_GL_GetProcAddress so no
 * OpenGL import library is needed. */
static struct {
    const GLubyte *(APIENTRY *GetString)(GLenum);
    void      (APIENTRY *GetIntegerv)(GLenum, GLint *);
    GLboolean (APIENTRY *IsEnabled)(GLenum);
    void      (APIENTRY *Enable)(GLenum);
    void      (APIENTRY *Disable)(GLenum);
    void      (APIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void      (APIENTRY *DrawArrays)(GLenum, GLint, GLsizei);
    GLenum    (APIENTRY *GetError)(void);
    GLuint    (APIENTRY *CreateShader)(GLenum);
    void      (APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void      (APIENTRY *CompileShader)(GLuint);
    void      (APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    void      (APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint    (APIENTRY *CreateProgram)(void);
    void      (APIENTRY *AttachShader)(GLuint, GLuint);
    void      (APIENTRY *LinkProgram)(GLuint);
    void      (APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void      (APIENTRY *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void      (APIENTRY *DeleteShader)(GLuint);
    void      (APIENTRY *DeleteProgram)(GLuint);
    void      (APIENTRY *UseProgram)(GLuint);
    GLint     (APIENTRY *GetUniformLocation)(GLuint, const GLchar *);
    void      (APIENTRY *Uniform1i)(GLint, GLint);
    void      (APIENTRY *Uniform1f)(GLint, GLfloat);
    void      (APIENTRY *Uniform2f)(GLint, GLfloat, GLfloat);
    void      (APIENTRY *Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void      (APIENTRY *ActiveTexture)(GLenum);
    void      (APIENTRY *BindTexture)(GLenum, GLuint);
    void      (APIENTRY *TexParameteri)(GLenum, GLenum, GLint);
    void      (APIENTRY *GenTextures)(GLsizei, GLuint *);
    void      (APIENTRY *DeleteTextures)(GLsizei, const GLuint *);
    void      (APIENTRY *Finish)(void);
    void      (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void      (APIENTRY *GetTexImage)(GLenum, GLint, GLenum, GLenum, void *);
    void      (APIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
} gl;

static int          fsr_state;      /* 0 = uninitialized, 1 = ready, -1 = unavailable */
static char         gl_renderer_str[256];
static GLuint       easu_prog, rcas_prog, copy_prog;
static GLint        easu_con0_loc, rcas_sharp_loc, rcas_dns_loc, copy_invout_loc;
static GLint        rcas_hdr_loc, rcas_peak_loc, copy_hdr_loc, copy_peak_loc;
static int          hdr_active;       /* zero-copy stream is PQ BT.2020 */
static float        hdr_peak = 1000.0f; /* content peak, nits */
static int          rcas_denoise;
static SDL_Texture *native_tex, *easu_tex, *out_tex;
static int          native_w, native_h, out_w, out_h;
static int          last_engaged = -1;

static const char *vertex_src =
    "#version 330\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,\n"
    "                  gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

/* EASU: edge-adaptive spatial upsampling, port of FsrEasuF() with the
 * gather4 callbacks replaced by direct clamped texel fetches. */
static const char *easu_src =
    "#version 330\n"
    "uniform sampler2D srcTex;\n"
    "uniform vec4 con0;\n" /* xy: out->in pixel scale, zw: offset */
    "out vec4 fragColor;\n"
    "float APrxLoRcp(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }\n"
    "float APrxLoRsq(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1)); }\n"
    "vec3 fetch(ivec2 p, ivec2 sz) {\n"
    "    return texelFetch(srcTex, clamp(p, ivec2(0), sz - 1), 0).rgb;\n"
    "}\n"
    "void easuSet(inout vec2 dir, inout float len, float w,\n"
    "             float lA, float lB, float lC, float lD, float lE) {\n"
    "    float lenX = max(abs(lD - lC), abs(lC - lB));\n"
    "    lenX = APrxLoRcp(lenX);\n"
    "    float dirX = lD - lB;\n"
    "    dir.x += dirX * w;\n"
    "    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);\n"
    "    lenX *= lenX;\n"
    "    len += lenX * w;\n"
    "    float lenY = max(abs(lE - lC), abs(lC - lA));\n"
    "    lenY = APrxLoRcp(lenY);\n"
    "    float dirY = lE - lA;\n"
    "    dir.y += dirY * w;\n"
    "    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);\n"
    "    lenY *= lenY;\n"
    "    len += lenY * w;\n"
    "}\n"
    "void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,\n"
    "             float lob, float clp, vec3 c) {\n"
    "    vec2 v = vec2(off.x * dir.x + off.y * dir.y,\n"
    "                  off.x * (-dir.y) + off.y * dir.x);\n"
    "    v *= len;\n"
    "    float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
    "    float wB = 0.4 * d2 - 1.0;\n"
    "    float wA = lob * d2 - 1.0;\n"
    "    wB *= wB;\n"
    "    wA *= wA;\n"
    "    wB = 1.5625 * wB - 0.5625;\n"
    "    float w = wB * wA;\n"
    "    aC += c * w;\n"
    "    aW += w;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 isz = textureSize(srcTex, 0);\n"
    "    vec2 pp = vec2(ivec2(gl_FragCoord.xy)) * con0.xy + con0.zw;\n"
    "    vec2 fp = floor(pp);\n"
    "    pp -= fp;\n"
    "    ivec2 sp = ivec2(fp);\n"
    /*      b c
     *    e f g h
     *    i j k l
     *      n o     */
    "    vec3 cB = fetch(sp + ivec2( 0, -1), isz);\n"
    "    vec3 cC = fetch(sp + ivec2( 1, -1), isz);\n"
    "    vec3 cE = fetch(sp + ivec2(-1,  0), isz);\n"
    "    vec3 cF = fetch(sp + ivec2( 0,  0), isz);\n"
    "    vec3 cG = fetch(sp + ivec2( 1,  0), isz);\n"
    "    vec3 cH = fetch(sp + ivec2( 2,  0), isz);\n"
    "    vec3 cI = fetch(sp + ivec2(-1,  1), isz);\n"
    "    vec3 cJ = fetch(sp + ivec2( 0,  1), isz);\n"
    "    vec3 cK = fetch(sp + ivec2( 1,  1), isz);\n"
    "    vec3 cL = fetch(sp + ivec2( 2,  1), isz);\n"
    "    vec3 cN = fetch(sp + ivec2( 0,  2), isz);\n"
    "    vec3 cO = fetch(sp + ivec2( 1,  2), isz);\n"
    "    float bL = cB.b * 0.5 + (cB.r * 0.5 + cB.g);\n"
    "    float cL_ = cC.b * 0.5 + (cC.r * 0.5 + cC.g);\n"
    "    float eL = cE.b * 0.5 + (cE.r * 0.5 + cE.g);\n"
    "    float fL = cF.b * 0.5 + (cF.r * 0.5 + cF.g);\n"
    "    float gL = cG.b * 0.5 + (cG.r * 0.5 + cG.g);\n"
    "    float hL = cH.b * 0.5 + (cH.r * 0.5 + cH.g);\n"
    "    float iL = cI.b * 0.5 + (cI.r * 0.5 + cI.g);\n"
    "    float jL = cJ.b * 0.5 + (cJ.r * 0.5 + cJ.g);\n"
    "    float kL = cK.b * 0.5 + (cK.r * 0.5 + cK.g);\n"
    "    float lL = cL.b * 0.5 + (cL.r * 0.5 + cL.g);\n"
    "    float nL = cN.b * 0.5 + (cN.r * 0.5 + cN.g);\n"
    "    float oL = cO.b * 0.5 + (cO.r * 0.5 + cO.g);\n"
    "    vec2 dir = vec2(0.0);\n"
    "    float len = 0.0;\n"
    "    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "    easuSet(dir, len,        pp.x  * (1.0 - pp.y), cL_, fL, gL, hL, kL);\n"
    "    easuSet(dir, len, (1.0 - pp.x) *        pp.y , fL, iL, jL, kL, nL);\n"
    "    easuSet(dir, len,        pp.x  *        pp.y , gL, jL, kL, lL, oL);\n"
    "    vec2 dir2 = dir * dir;\n"
    "    float dirR = dir2.x + dir2.y;\n"
    "    bool zro = dirR < (1.0 / 32768.0);\n"
    "    dirR = APrxLoRsq(dirR);\n"
    "    dirR = zro ? 1.0 : dirR;\n"
    "    dir.x = zro ? 1.0 : dir.x;\n"
    "    dir *= dirR;\n"
    "    len = len * 0.5;\n"
    "    len *= len;\n"
    "    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcp(max(abs(dir.x), abs(dir.y)));\n"
    "    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
    "    float clp = APrxLoRcp(lob);\n"
    "    vec3 min4 = min(min(cF, cG), min(cJ, cK));\n"
    "    vec3 max4 = max(max(cF, cG), max(cJ, cK));\n"
    "    vec3 aC = vec3(0.0);\n"
    "    float aW = 0.0;\n"
    "    easuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, cB);\n"
    "    easuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
    "    easuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, cI);\n"
    "    easuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, cJ);\n"
    "    easuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, cF);\n"
    "    easuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, cE);\n"
    "    easuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, cK);\n"
    "    easuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, cL);\n"
    "    easuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, cH);\n"
    "    easuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, cG);\n"
    "    easuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, cO);\n"
    "    easuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, cN);\n"
    "    vec3 pix = min(max4, max(min4, aC * (1.0 / aW)));\n"
    "    fragColor = vec4(pix, 1.0);\n"
    "}\n";

/* RCAS: robust contrast-adaptive sharpening, port of FsrRcasF().
 * Small epsilons keep the flat-black / flat-white limiters away from 0/0. */
/* HDR10 (PQ/BT.2020) to SDR BT.709: PQ EOTF, gamut map, white-preserving
 * extended-Reinhard tone map against the content peak, 2.2 gamma encode.
 * Appended to the shaders that produce final SDR output. */
#define HDR_TM_GLSL \
    "uniform float hdrMode;\n" /* 0 = passthrough, 1 = PQ BT.2020 input */ \
    "uniform float hdrPeak;\n" /* content peak, nits */ \
    "vec3 hdr_tonemap(vec3 v) {\n" \
    "    const float m1 = 0.1593017578125, m2 = 78.84375;\n" \
    "    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n" \
    "    vec3 p = pow(max(v, vec3(0.0)), vec3(1.0 / m2));\n" \
    "    vec3 lin = pow(max(p - c1, vec3(0.0)) / (c2 - c3 * p),\n" \
    "               vec3(1.0 / m1)) * 10000.0;\n" \
    "    lin = mat3(1.6605, -0.1246, -0.0182,\n" \
    "               -0.5876, 1.1329, -0.1006,\n" \
    "               -0.0728, -0.0083, 1.1187) * lin;\n" \
    /* Hue-preserving gamut compression. A BT.2020 colour outside the smaller \
     * BT.709 gamut converts to a negative channel here; hard-clipping that to \
     * zero (the old max(lin,0)) leaves the other channels untouched, which \
     * over-saturates the colour and shifts its hue - deep reds and blues come \
     * out far too heavy on an SDR display. Instead desaturate toward the \
     * equal-luminance grey just enough to bring the colour back to the gamut \
     * boundary. In-gamut colours (no negative channel) are left untouched. */ \
    "    float luma = dot(max(lin, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));\n" \
    "    float mn = min(lin.r, min(lin.g, lin.b));\n" \
    "    if (mn < 0.0)\n" \
    "        lin = mix(lin, vec3(luma), -mn / (luma - mn + 1e-4));\n" \
    /* Normalise by the 100-nit SDR reference white and tone-map against the \
     * real content peak (MaxCLL). The old code divided by 230 and forced the \
     * peak up to at least 400 nits; on the many titles graded to a low peak \
     * (e.g. MaxCLL ~200) that squeezed the whole image into the bottom of the \
     * range, so it came out dark and, with the saturation preserved, heavy on \
     * reds and blues. Matching the 100-nit white keeps diffuse white bright \
     * while highlights above it roll off toward the peak. */ \
    "    vec3 n = max(lin, vec3(0.0)) / 100.0;\n" \
    "    float L = max(max(n.r, n.g), n.b);\n" \
    "    float Lp = max(hdrPeak, 100.0) / 100.0;\n" \
    "    float Lt = L * (1.0 + L / (Lp * Lp)) / (1.0 + L);\n" \
    "    n *= L > 1e-6 ? Lt / L : 0.0;\n" \
    /* Midtone luminance lift. Compared crop-for-crop against a reference \
     * render (PotPlayer) of the same frames, skin came out the right hue and \
     * saturation but ~2x too dark - a dim, saturated orange face reads as \
     * muddy "red", where the same colour lifted to full brightness reads as \
     * natural skin. (The whole-frame average matched because this curve lifts \
     * shadows and crushes midtones - flat contrast - so the darkened skin was \
     * masked by brighter background; earlier saturation cuts chased the wrong \
     * axis and only greyed things out.) Lift brightness with a gamma on the \
     * luminance and rescale the channels by the same factor, so skin gets \
     * brighter WITHOUT desaturating (a per-channel gamma would wash it out). \
     * 0.80 matched the reference's skin (sRGB ~95,58,36 vs 97,57,33); Y=1 \
     * white is untouched so highlights don't blow. */ \
    "    float Y = dot(n, vec3(0.2126, 0.7152, 0.0722));\n" \
    "    n *= Y > 1e-6 ? pow(Y, 0.80) / Y : 1.0;\n" \
    "    return pow(clamp(n, 0.0, 1.0), vec3(1.0 / 2.2));\n" \
    "}\n"

static const char *rcas_src =
    "#version 330\n"
    "uniform sampler2D srcTex;\n"
    "uniform float sharp;\n" /* exp2(-sharpness stops), computed on the CPU */
    "uniform float dns;\n"   /* 1.0 = FSR_RCAS_DENOISE behavior, 0.0 = off */
    "out vec4 fragColor;\n"
    HDR_TM_GLSL
    "float APrxMedRcp(float a) {\n"
    "    float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a));\n"
    "    return b * (-b * a + 2.0);\n"
    "}\n"
    "vec3 fetch(ivec2 p, ivec2 sz) {\n"
    "    return texelFetch(srcTex, clamp(p, ivec2(0), sz - 1), 0).rgb;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(srcTex, 0);\n"
    "    ivec2 sp = ivec2(gl_FragCoord.xy);\n"
    /*      b
     *    d e f
     *      h    */
    "    vec3 b = fetch(sp + ivec2( 0, -1), sz);\n"
    "    vec3 d = fetch(sp + ivec2(-1,  0), sz);\n"
    "    vec3 e = fetch(sp,                 sz);\n"
    "    vec3 f = fetch(sp + ivec2( 1,  0), sz);\n"
    "    vec3 h = fetch(sp + ivec2( 0,  1), sz);\n"
    "    vec3 mn4 = min(min(b, d), min(f, h));\n"
    "    vec3 mx4 = max(max(b, d), max(f, h));\n"
    /* Noise detection: luma highpass over local contrast (FSR_RCAS_DENOISE). */
    "    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);\n"
    "    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);\n"
    "    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);\n"
    "    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);\n"
    "    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);\n"
    "    float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
    "    nz = clamp(abs(nz) * APrxMedRcp(max(max(max(bL, dL), eL), max(fL, hL))\n"
    "                                  - min(min(min(bL, dL), eL), min(fL, hL))), 0.0, 1.0);\n"
    "    nz = -0.5 * nz + 1.0;\n"
    "    vec3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-4);\n"
    "    vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-4);\n"
    "    vec3 lobeRGB = max(-hitMin, hitMax);\n"
    "    float lobe = max(-(0.25 - 1.0 / 16.0),\n"
    "                     min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharp;\n"
    "    lobe *= mix(1.0, nz, dns);\n"
    "    float rcpL = APrxMedRcp(4.0 * lobe + 1.0);\n"
    "    vec3 pix = (lobe * (b + d + f + h) + e) * rcpL;\n"
    "    if (hdrMode > 0.5)\n"
    "        pix = hdr_tonemap(pix);\n"
    "    fragColor = vec4(pix, 1.0);\n"
    "}\n";

/* Plain textured blit, used when a zero-copy hardware frame is displayed
 * with FSR off or bypassed (handles down/upscaling via linear filtering). */
static const char *copy_src =
    "#version 330\n"
    "uniform sampler2D srcTex;\n"
    "uniform vec2 invOut;\n" /* 1 / output size */
    "out vec4 fragColor;\n"
    HDR_TM_GLSL
    "void main() {\n"
    "    vec3 c = texture(srcTex, gl_FragCoord.xy * invOut).rgb;\n"
    "    if (hdrMode > 0.5)\n"
    "        c = hdr_tonemap(c);\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

static int load_gl_functions(void)
{
#define LOAD(name)                                                          \
    do {                                                                    \
        gl.name = (void *)SDL_GL_GetProcAddress("gl" #name);                \
        if (!gl.name) {                                                     \
            av_log(NULL, AV_LOG_WARNING, "FSR: missing GL entry point gl%s\n", #name); \
            return -1;                                                      \
        }                                                                   \
    } while (0)
    LOAD(GetString);
    LOAD(GetIntegerv);
    LOAD(IsEnabled);
    LOAD(Enable);
    LOAD(Disable);
    LOAD(Viewport);
    LOAD(DrawArrays);
    LOAD(GetError);
    LOAD(CreateShader);
    LOAD(ShaderSource);
    LOAD(CompileShader);
    LOAD(GetShaderiv);
    LOAD(GetShaderInfoLog);
    LOAD(CreateProgram);
    LOAD(AttachShader);
    LOAD(LinkProgram);
    LOAD(GetProgramiv);
    LOAD(GetProgramInfoLog);
    LOAD(DeleteShader);
    LOAD(DeleteProgram);
    LOAD(UseProgram);
    LOAD(GetUniformLocation);
    LOAD(Uniform1i);
    LOAD(Uniform1f);
    LOAD(Uniform2f);
    LOAD(Uniform4f);
    LOAD(ActiveTexture);
    LOAD(BindTexture);
    LOAD(TexParameteri);
    LOAD(GenTextures);
    LOAD(DeleteTextures);
    LOAD(Finish);
    LOAD(ReadPixels);
    LOAD(GetTexImage);
    LOAD(TexImage2D);
#undef LOAD
    return 0;
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = gl.CreateShader(type);
    GLint ok = 0;

    if (!sh)
        return 0;
    gl.ShaderSource(sh, 1, &src, NULL);
    gl.CompileShader(sh);
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[1024] = "";
        gl.GetShaderInfoLog(sh, sizeof(info), NULL, info);
        av_log(NULL, AV_LOG_WARNING, "FSR: shader compilation failed: %s\n", info);
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = 0;
    GLint ok = 0;

    if (vs && fs) {
        prog = gl.CreateProgram();
        if (prog) {
            gl.AttachShader(prog, vs);
            gl.AttachShader(prog, fs);
            gl.LinkProgram(prog);
            gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) {
                char info[1024] = "";
                gl.GetProgramInfoLog(prog, sizeof(info), NULL, info);
                av_log(NULL, AV_LOG_WARNING, "FSR: program link failed: %s\n", info);
                gl.DeleteProgram(prog);
                prog = 0;
            }
        }
    }
    if (vs)
        gl.DeleteShader(vs);
    if (fs)
        gl.DeleteShader(fs);
    return prog;
}

int fsr_init(SDL_Renderer *renderer)
{
    SDL_RendererInfo info;
    GLint prev_prog = 0;
    GLint loc;

    fsr_state = -1;

    if (SDL_GetRendererInfo(renderer, &info) < 0)
        return -1;
    if (strcmp(info.name, "opengl")) {
        av_log(NULL, AV_LOG_WARNING, "FSR: SDL renderer is '%s', not 'opengl'\n", info.name);
        return -1;
    }
    if (!(info.flags & SDL_RENDERER_TARGETTEXTURE)) {
        av_log(NULL, AV_LOG_WARNING, "FSR: renderer lacks render-target support\n");
        return -1;
    }

    /* Make sure the renderer's GL context is current before resolving
     * GL entry points or issuing GL calls. */
    SDL_RenderFlush(renderer);

    if (load_gl_functions() < 0)
        return -1;

    av_log(NULL, AV_LOG_VERBOSE, "FSR: OpenGL version: %s\n",
           (const char *)gl.GetString(GL_VERSION));
    av_strlcpy(gl_renderer_str, (const char *)gl.GetString(GL_RENDERER),
               sizeof(gl_renderer_str));
    av_log(NULL, AV_LOG_INFO, "FSR: OpenGL renderer: %s\n", gl_renderer_str);

    easu_prog = build_program(vertex_src, easu_src);
    rcas_prog = build_program(vertex_src, rcas_src);
    copy_prog = build_program(vertex_src, copy_src);
    if (!easu_prog || !rcas_prog || !copy_prog) {
        fsr_uninit();
        return -1;
    }

    easu_con0_loc   = gl.GetUniformLocation(easu_prog, "con0");
    rcas_sharp_loc  = gl.GetUniformLocation(rcas_prog, "sharp");
    rcas_dns_loc    = gl.GetUniformLocation(rcas_prog, "dns");
    copy_invout_loc = gl.GetUniformLocation(copy_prog, "invOut");
    rcas_hdr_loc    = gl.GetUniformLocation(rcas_prog, "hdrMode");
    rcas_peak_loc   = gl.GetUniformLocation(rcas_prog, "hdrPeak");
    copy_hdr_loc    = gl.GetUniformLocation(copy_prog, "hdrMode");
    copy_peak_loc   = gl.GetUniformLocation(copy_prog, "hdrPeak");

    /* Bind the sampler uniforms to texture unit 0 once. */
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.UseProgram(easu_prog);
    loc = gl.GetUniformLocation(easu_prog, "srcTex");
    if (loc >= 0)
        gl.Uniform1i(loc, 0);
    gl.UseProgram(rcas_prog);
    loc = gl.GetUniformLocation(rcas_prog, "srcTex");
    if (loc >= 0)
        gl.Uniform1i(loc, 0);
    gl.UseProgram(copy_prog);
    loc = gl.GetUniformLocation(copy_prog, "srcTex");
    if (loc >= 0)
        gl.Uniform1i(loc, 0);
    gl.UseProgram(prev_prog);

    fsr_state = 1;
    av_log(NULL, AV_LOG_INFO, "FSR: EASU+RCAS OpenGL pipeline initialized\n");
    return 0;
}

void fsr_set_denoise(SDL_Renderer *renderer, int enable)
{
    GLint prev_prog = 0;

    rcas_denoise = !!enable;
    if (fsr_state != 1)
        return;
    SDL_RenderFlush(renderer); /* make the GL context current */
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.UseProgram(rcas_prog);
    gl.Uniform1f(rcas_dns_loc, rcas_denoise ? 1.0f : 0.0f);
    gl.UseProgram(prev_prog);
}

int fsr_available(void)
{
    return fsr_state == 1;
}

static int ensure_textures(SDL_Renderer *renderer, int nw, int nh, int ow, int oh)
{
    if (native_tex && nw == native_w && nh == native_h &&
        out_tex && ow == out_w && oh == out_h)
        return 0;

    if (native_tex) { SDL_DestroyTexture(native_tex); native_tex = NULL; }
    if (easu_tex)   { SDL_DestroyTexture(easu_tex);   easu_tex   = NULL; }
    if (out_tex)    { SDL_DestroyTexture(out_tex);    out_tex    = NULL; }

    native_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_TARGET, nw, nh);
    easu_tex   = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_TARGET, ow, oh);
    out_tex    = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_TARGET, ow, oh);
    if (!native_tex || !easu_tex || !out_tex) {
        av_log(NULL, AV_LOG_WARNING, "FSR: cannot create intermediate textures: %s\n",
               SDL_GetError());
        return -1;
    }
    SDL_SetTextureBlendMode(native_tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureBlendMode(easu_tex,   SDL_BLENDMODE_NONE);
    SDL_SetTextureBlendMode(out_tex,    SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(native_tex, SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(easu_tex,   SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(out_tex,    SDL_ScaleModeNearest);

    native_w = nw;
    native_h = nh;
    out_w    = ow;
    out_h    = oh;
    return 0;
}

/* Run one fullscreen fragment pass: src -> dst (dst must be a target
 * texture of dw x dh). The source is either an SDL texture (src) or a raw
 * GL texture name (src_gl, used for D3D11-interop textures). Interleaves
 * raw GL with the SDL renderer; every GL state we touch is either restored
 * exactly or tracked by SDL itself (SDL texture binds go through
 * SDL_GL_Bind/UnbindTexture, the viewport is re-applied by SDL because
 * SDL_SetRenderTarget marks it dirty). */
static int run_pass2(SDL_Renderer *renderer, GLuint prog, SDL_Texture *src,
                     GLuint src_gl, SDL_Texture *dst, int dw, int dh,
                     const GLfloat *con0, float sharp)
{
    GLint prev_prog = 0, prev_active = 0, prev_tex0 = 0;
    GLboolean blend, scissor;
    float texw = 1.0f, texh = 1.0f;

    if (SDL_SetRenderTarget(renderer, dst) < 0)
        return -1;
    SDL_RenderFlush(renderer);

    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    blend   = gl.IsEnabled(GL_BLEND);
    scissor = gl.IsEnabled(GL_SCISSOR_TEST);

    gl.ActiveTexture(GL_TEXTURE0);
    if (src) {
        if (SDL_GL_BindTexture(src, &texw, &texh) < 0) {
            gl.ActiveTexture(prev_active);
            return -1;
        }
        if (texw != 1.0f || texh != 1.0f) {
            /* Rectangle textures (no NPOT support) cannot back a sampler2D. */
            SDL_GL_UnbindTexture(src);
            gl.ActiveTexture(prev_active);
            av_log(NULL, AV_LOG_WARNING, "FSR: unsupported texture layout, disabling\n");
            fsr_state = -1;
            return -1;
        }
    } else {
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
        gl.BindTexture(GL_TEXTURE_2D, src_gl);
    }

    gl.Disable(GL_BLEND);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, dw, dh);

    gl.UseProgram(prog);
    if (prog == easu_prog && con0)
        gl.Uniform4f(easu_con0_loc, con0[0], con0[1], con0[2], con0[3]);
    if (prog == rcas_prog) {
        gl.Uniform1f(rcas_sharp_loc, sharp);
        gl.Uniform1f(rcas_hdr_loc, hdr_active ? 1.0f : 0.0f);
        gl.Uniform1f(rcas_peak_loc, hdr_peak);
    }
    if (prog == copy_prog) {
        gl.Uniform2f(copy_invout_loc, 1.0f / dw, 1.0f / dh);
        gl.Uniform1f(copy_hdr_loc, hdr_active ? 1.0f : 0.0f);
        gl.Uniform1f(copy_peak_loc, hdr_peak);
    }
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.UseProgram(prev_prog);

    if (src)
        SDL_GL_UnbindTexture(src);
    else
        gl.BindTexture(GL_TEXTURE_2D, prev_tex0);
    if (blend)
        gl.Enable(GL_BLEND);
    if (scissor)
        gl.Enable(GL_SCISSOR_TEST);
    gl.ActiveTexture(prev_active);

    return 0;
}

static int run_pass(SDL_Renderer *renderer, GLuint prog, SDL_Texture *src,
                    SDL_Texture *dst, int dw, int dh,
                    const GLfloat *con0, float sharp)
{
    return run_pass2(renderer, prog, src, 0, dst, dw, dh, con0, sharp);
}

static void log_engage_transition(int engaged, int fw, int fh, const SDL_Rect *rect)
{
    if (engaged == last_engaged)
        return;
    if (engaged)
        av_log(NULL, AV_LOG_INFO, "FSR: EASU+RCAS engaged: %dx%d -> %dx%d\n",
               fw, fh, rect->w, rect->h);
    else
        av_log(NULL, AV_LOG_INFO, "FSR: bypass (no upscale needed: %dx%d -> %dx%d)\n",
               fw, fh, rect->w, rect->h);
    last_engaged = engaged;
}

/* ---- D3D11 zero-copy display (WGL_NV_DX_interop2 + VideoProcessor) ---- */

#if CONFIG_D3D11VA

typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL   (WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void *dxObject,
                                                   GLuint name, GLenum type, GLenum access);
typedef BOOL   (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL   (WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL   (WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);

#define WGL_ACCESS_READ_ONLY_NV 0x0000

static struct {
    int state;                    /* 0 = untried, 1 = ready, -1 = failed */
    PFN_wglDXOpenDeviceNV       DXOpenDevice;
    PFN_wglDXCloseDeviceNV      DXCloseDevice;
    PFN_wglDXRegisterObjectNV   DXRegisterObject;
    PFN_wglDXUnregisterObjectNV DXUnregisterObject;
    PFN_wglDXLockObjectsNV      DXLockObjects;
    PFN_wglDXUnlockObjectsNV    DXUnlockObjects;
    AVBufferRef *device_ref;      /* keeps the decoder's device alive */
    ID3D11Device *device;
    ID3D11DeviceContext *dcontext;
    ID3D11VideoDevice *vdevice;
    ID3D11VideoContext *vcontext;
    ID3D11VideoContext1 *vcontext1; /* for DXGI color spaces (HDR input) */
    ID3D11VideoContext2 *vcontext2; /* for HDR10 metadata (tone-map hints) */
    int cs_dxgi;                    /* DXGI input color space currently set */
    int hdr_md_state;               /* 0 unset, 1 defaults, 2 from stream */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *lock_ctx;
    HANDLE gl_device;
    ID3D11VideoProcessorEnumerator *vp_enum;
    ID3D11VideoProcessor *vp;
    ID3D11Texture2D *vp_tex;      /* VideoProcessor output (not GL-registered) */
    ID3D11VideoProcessorOutputView *out_view;
    /* Two GL-shared frame slots (current + previous) so frame generation
     * can warp between consecutive frames. */
    ID3D11Texture2D *rgb_tex[2];  /* filled via 3D-engine CopyResource */
    HANDLE gl_object[2];
    GLuint gl_tex[2];
    const void *slot_frame[2];    /* which decoded frame each slot holds */
    int64_t slot_pts[2];
    int cur_slot;
    int w, h;
    int cs_matrix, cs_range;      /* color space currently set on the stream */
    /* Cache key of the last fully rendered frame: repeated refreshes of the
     * same frame (pause, toast) redraw the cached out_tex without touching
     * D3D11 again. */
    const void *last_frame;
    int64_t last_pts;
    int last_rect_w, last_rect_h, last_engaged, last_denoise;
    float last_sharp;
} hwgl = { .cs_matrix = -1, .cs_range = -1 };

static const GUID iid_IDXGIFactory =
    { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID iid_IDXGIDevice =
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID iid_ID3D11VideoContext1 =
    { 0xa7f026da, 0xa5f8, 0x4487, { 0xa5, 0x64, 0x15, 0xe3, 0x43, 0x57, 0x65, 0x1e } };
static const GUID iid_ID3D11VideoContext2 =
    { 0xc4e7374c, 0x6243, 0x4d1b, { 0xae, 0x87, 0x52, 0xb4, 0xf7, 0x40, 0xe2, 0x61 } };

/* ASCII-fold a DXGI adapter description for matching against GL_RENDERER. */
static void adapter_desc_to_ascii(const WCHAR *src, char *dst, size_t dst_size)
{
    size_t i;

    for (i = 0; i + 1 < dst_size && src[i]; i++)
        dst[i] = src[i] < 128 ? (char)src[i] : '?';
    while (i > 0 && dst[i - 1] == ' ')
        i--;
    dst[i] = 0;
}

int fsr_d3d11_adapter_index(void)
{
    typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
    PFN_CreateDXGIFactory create;
    HMODULE dxgi;
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    int found = -1;

    if (!gl_renderer_str[0])
        return -1;
    dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi)
        dxgi = LoadLibraryA("dxgi.dll");
    if (!dxgi)
        return -1;
    create = (PFN_CreateDXGIFactory)GetProcAddress(dxgi, "CreateDXGIFactory");
    if (!create || FAILED(create(&iid_IDXGIFactory, (void **)&factory)))
        return -1;

    for (UINT i = 0; found < 0 &&
                     IDXGIFactory_EnumAdapters(factory, i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC desc;

        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
            char name[128];

            adapter_desc_to_ascii(desc.Description, name, sizeof(name));
            av_log(NULL, AV_LOG_VERBOSE, "FSR: DXGI adapter %u: %s\n", i, name);
            if (name[0] && strstr(gl_renderer_str, name))
                found = i;
        }
        IDXGIAdapter_Release(adapter);
    }
    IDXGIFactory_Release(factory);
    av_log(NULL, AV_LOG_INFO, "FSR: GL runs on '%s' -> DXGI adapter %d\n",
           gl_renderer_str, found);
    return found;
}

/* Can the D3D11 device inside hw_device_ctx actually hardware-decode codec_id?
 *
 * Only AV1 is gated: FFmpeg's default AV1 decoder (libdav1d) is a separate
 * software-only wrapper, while the native "av1" decoder that carries the
 * d3d11va config is hardware-ONLY (no software fallback). Creating a D3D11
 * device always succeeds, so a decoder swap based on device creation alone
 * commits us to a decoder that fails every frame on GPUs without an AV1
 * decode block (e.g. GTX 1660 Ti). Verify the actual decoder profile GUID so
 * such GPUs keep libdav1d and play in software. Native h264/hevc/vp9 decoders
 * can decode in software, so they are never gated (return 1). */
int fsr_d3d11_supports_codec(struct AVBufferRef *hw_device_ctx, int codec_id)
{
    /* DXVA_ModeAV1_VLD_Profile0 (not defined by the mingw d3d11.h headers). */
    static const GUID av1_vld_profile0 =
        { 0xb8be4cce, 0xcf65, 0x4682,
          { 0x8b, 0xe8, 0x9c, 0x8b, 0x25, 0x6f, 0xa3, 0xd3 } };
    AVHWDeviceContext *devctx;
    AVD3D11VADeviceContext *d3d;
    ID3D11VideoDevice *vdev;
    UINT count, i;
    int supported = 0;

    if (codec_id != AV_CODEC_ID_AV1)
        return 1;                       /* not gated */
    if (!hw_device_ctx)
        return 0;
    devctx = (AVHWDeviceContext *)hw_device_ctx->data;
    /* Only the D3D11 profile GUID can be probed here; for any other hw type we
     * cannot confirm AV1 support, so stay on the software decoder. */
    if (devctx->type != AV_HWDEVICE_TYPE_D3D11VA)
        return 0;
    d3d  = devctx->hwctx;
    vdev = d3d->video_device;
    if (!vdev)
        return 0;

    count = ID3D11VideoDevice_GetVideoDecoderProfileCount(vdev);
    for (i = 0; i < count; i++) {
        GUID g;
        if (SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderProfile(vdev, i, &g)) &&
            IsEqualGUID(&g, &av1_vld_profile0)) {
            supported = 1;
            break;
        }
    }
    av_log(NULL, AV_LOG_INFO,
           "FSR: D3D11 AV1 hardware decode profile %s (%u decoder profiles)\n",
           supported ? "present" : "absent", count);
    return supported;
}

static void hwgl_lock(void)
{
    if (hwgl.lock)
        hwgl.lock(hwgl.lock_ctx);
}

static void hwgl_unlock(void)
{
    if (hwgl.unlock)
        hwgl.unlock(hwgl.lock_ctx);
}

/* At process exit the NVIDIA driver can crash on interop unregistration
 * (observed with locked/remote sessions); the OS reclaims everything at
 * process death anyway, so teardown is skipped entirely then. */
static int hwgl_exiting;

static void fg_on_convert(AVFrame *frame, int slot); /* frame generation hook */

static void hwgl_destroy_size(void)
{
    if (hwgl_exiting) {
        for (int i = 0; i < 2; i++) {
            hwgl.gl_object[i] = NULL;
            hwgl.gl_tex[i]    = 0;
            hwgl.rgb_tex[i]   = NULL;
            hwgl.slot_frame[i] = NULL;
        }
        hwgl.out_view  = NULL;
        hwgl.vp        = NULL;
        hwgl.vp_enum   = NULL;
        hwgl.vp_tex    = NULL;
        hwgl.w = hwgl.h = 0;
        hwgl.last_frame = NULL;
        return;
    }
    /* Drain both pipelines before unregistering the shared textures; the
     * NVIDIA GL/D3D drivers crash on teardown with in-flight work. */
    if (hwgl.gl_object[0] || hwgl.gl_object[1]) {
        if (gl.Finish)
            gl.Finish();
        if (hwgl.dcontext) {
            hwgl_lock();
            ID3D11DeviceContext_Flush(hwgl.dcontext);
            hwgl_unlock();
        }
    }
    for (int i = 0; i < 2; i++) {
        if (hwgl.gl_object[i]) {
            hwgl.DXUnregisterObject(hwgl.gl_device, hwgl.gl_object[i]);
            hwgl.gl_object[i] = NULL;
        }
        if (hwgl.gl_tex[i]) {
            gl.DeleteTextures(1, &hwgl.gl_tex[i]);
            hwgl.gl_tex[i] = 0;
        }
        if (hwgl.rgb_tex[i]) {
            ID3D11Texture2D_Release(hwgl.rgb_tex[i]);
            hwgl.rgb_tex[i] = NULL;
        }
        hwgl.slot_frame[i] = NULL;
    }
    if (hwgl.out_view) {
        ID3D11VideoProcessorOutputView_Release(hwgl.out_view);
        hwgl.out_view = NULL;
    }
    if (hwgl.vp_tex) {
        ID3D11Texture2D_Release(hwgl.vp_tex);
        hwgl.vp_tex = NULL;
    }
    if (hwgl.vp) {
        ID3D11VideoProcessor_Release(hwgl.vp);
        hwgl.vp = NULL;
    }
    if (hwgl.vp_enum) {
        ID3D11VideoProcessorEnumerator_Release(hwgl.vp_enum);
        hwgl.vp_enum = NULL;
    }
    hwgl.w = hwgl.h = 0;
    hwgl.cs_matrix = hwgl.cs_range = -1;
    hwgl.last_frame = NULL;
}

static void hwgl_destroy(void)
{
    hwgl_destroy_size();
    if (hwgl.gl_device) {
        if (!hwgl_exiting)
            hwgl.DXCloseDevice(hwgl.gl_device);
        hwgl.gl_device = NULL;
    }
    hwgl.device   = NULL;
    hwgl.dcontext = NULL;
    hwgl.vdevice  = NULL;
    hwgl.vcontext = NULL;
    if (hwgl.vcontext1) {
        if (!hwgl_exiting)
            ID3D11VideoContext1_Release(hwgl.vcontext1);
        hwgl.vcontext1 = NULL;
    }
    if (hwgl.vcontext2) {
        if (!hwgl_exiting)
            ID3D11VideoContext2_Release(hwgl.vcontext2);
        hwgl.vcontext2 = NULL;
    }
    hwgl.cs_dxgi = -1;
    hwgl.hdr_md_state = 0;
    av_buffer_unref(&hwgl.device_ref);
    if (hwgl.state == 1)
        hwgl.state = 0;
}

static int hwgl_init(AVFrame *frame)
{
    AVHWFramesContext *fctx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVD3D11VADeviceContext *d3d = fctx->device_ctx->hwctx;

#define LOADWGL(name)                                                       \
    do {                                                                    \
        hwgl.name = (void *)SDL_GL_GetProcAddress("wgl" #name "NV");        \
        if (!hwgl.name)                                                     \
            return -1;                                                      \
    } while (0)
    LOADWGL(DXOpenDevice);
    LOADWGL(DXCloseDevice);
    LOADWGL(DXRegisterObject);
    LOADWGL(DXUnregisterObject);
    LOADWGL(DXLockObjects);
    LOADWGL(DXUnlockObjects);
#undef LOADWGL

    if (!d3d->video_device || !d3d->video_context)
        return -1;

    /* Interop only shares content when the D3D11 device and the GL context
     * live on the same GPU; a cross-adapter share "succeeds" but stays
     * black. Verify before committing to zero-copy. */
    {
        IDXGIDevice *dxgi_dev = NULL;
        IDXGIAdapter *adapter = NULL;
        DXGI_ADAPTER_DESC desc;
        char name[128] = "";

        if (SUCCEEDED(ID3D11Device_QueryInterface(d3d->device, &iid_IDXGIDevice,
                                                  (void **)&dxgi_dev))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter))) {
                if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc)))
                    adapter_desc_to_ascii(desc.Description, name, sizeof(name));
                IDXGIAdapter_Release(adapter);
            }
            IDXGIDevice_Release(dxgi_dev);
        }
        av_log(NULL, AV_LOG_INFO, "FSR: D3D11 decode device on '%s'\n", name);
        if (!name[0] || !gl_renderer_str[0] || !strstr(gl_renderer_str, name)) {
            av_log(NULL, AV_LOG_WARNING,
                   "FSR: decode GPU '%s' differs from GL GPU '%s'\n",
                   name, gl_renderer_str);
            return -1;
        }
    }

    hwgl.gl_device = hwgl.DXOpenDevice(d3d->device);
    if (!hwgl.gl_device)
        return -1;

    hwgl.device_ref = av_buffer_ref(fctx->device_ref);
    if (!hwgl.device_ref) {
        hwgl.DXCloseDevice(hwgl.gl_device);
        hwgl.gl_device = NULL;
        return -1;
    }
    hwgl.device   = d3d->device;
    hwgl.dcontext = d3d->device_context;
    hwgl.vdevice  = d3d->video_device;
    hwgl.vcontext = d3d->video_context;
    hwgl.lock     = d3d->lock;
    hwgl.unlock   = d3d->unlock;
    hwgl.lock_ctx = d3d->lock_ctx;
    /* DXGI color spaces (BT.2020/PQ aware, Win10+); NULL falls back to the
     * legacy matrix-only API */
    hwgl.cs_dxgi = -1;
    hwgl.hdr_md_state = 0;
    if (FAILED(ID3D11VideoContext_QueryInterface(hwgl.vcontext,
                                                 &iid_ID3D11VideoContext1,
                                                 (void **)&hwgl.vcontext1)))
        hwgl.vcontext1 = NULL;
    if (FAILED(ID3D11VideoContext_QueryInterface(hwgl.vcontext,
                                                 &iid_ID3D11VideoContext2,
                                                 (void **)&hwgl.vcontext2)))
        hwgl.vcontext2 = NULL;
    return 0;
}

static int hwgl_ensure_size(int w, int h)
{
    D3D11_TEXTURE2D_DESC tdesc = { 0 };
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc = { 0 };
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC odesc = { 0 };
    GLint prev_tex0 = 0, prev_active = 0;
    HRESULT hr;

    if (hwgl.rgb_tex[0] && hwgl.w == w && hwgl.h == h)
        return 0;
    hwgl_destroy_size();

    tdesc.Width            = w;
    tdesc.Height           = h;
    tdesc.MipLevels        = 1;
    tdesc.ArraySize        = 1;
    tdesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdesc.SampleDesc.Count = 1;
    tdesc.Usage            = D3D11_USAGE_DEFAULT;
    tdesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    {
        /* Tell-tale initial fill: a broken conversion chain shows orange
         * instead of black, which is much easier to diagnose. */
        uint32_t *init_px = av_malloc((size_t)w * h * 4);
        D3D11_SUBRESOURCE_DATA init = { 0 };

        if (init_px) {
            for (int i = 0; i < w * h; i++)
                init_px[i] = 0xFFFF8000; /* BGRA orange */
            init.pSysMem     = init_px;
            init.SysMemPitch = w * 4;
        }
        hr = S_OK;
        for (int i = 0; i < 2 && SUCCEEDED(hr); i++)
            hr = ID3D11Device_CreateTexture2D(hwgl.device, &tdesc,
                                              init_px ? &init : NULL, &hwgl.rgb_tex[i]);
        av_free(init_px);
    }
    if (FAILED(hr))
        goto fail;

    cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cdesc.InputWidth       = w;
    cdesc.InputHeight      = h;
    cdesc.OutputWidth      = w;
    cdesc.OutputHeight     = h;
    cdesc.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(hwgl.vdevice, &cdesc, &hwgl.vp_enum);
    if (FAILED(hr))
        goto fail;
    hr = ID3D11VideoDevice_CreateVideoProcessor(hwgl.vdevice, hwgl.vp_enum, 0, &hwgl.vp);
    if (FAILED(hr))
        goto fail;

    /* Disable the driver's automatic video processing. With it on (the D3D11
     * default) the NVIDIA driver applies its own "enhancement" - a saturation/
     * contrast boost - on top of our colour conversion, which pumped skin into
     * a clipped over-red orange (verified: an offline swscale replica of the
     * exact same shader math produced natural skin, while the live VP path came
     * out far redder and blown out; the VP was the only difference). We want the
     * VP to do nothing but the YCbCr->RGB de-matrix we asked for, so the GL
     * tone-map shader is the only thing shaping colour. */
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(hwgl.vcontext,
        hwgl.vp, 0, FALSE);

    /* The VideoProcessor writes with the GPU's video engine, whose writes
     * the NV_DX_interop path does not synchronize into GL (verified: 3D
     * clears arrive, Blt output does not). So the VP renders into its own
     * texture and a 3D-engine CopyResource feeds the GL-shared one. */
    hr = ID3D11Device_CreateTexture2D(hwgl.device, &tdesc, NULL, &hwgl.vp_tex);
    if (FAILED(hr))
        goto fail;
    odesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(hwgl.vdevice,
             (ID3D11Resource *)hwgl.vp_tex, hwgl.vp_enum, &odesc, &hwgl.out_view);
    if (FAILED(hr))
        goto fail;

    /* Set sampling state once per slot; the texture must be interop-locked
     * for GL use. All interop lock/unlock calls are serialized against the
     * decoder's D3D11 submissions via the hwdevice lock. */
    for (int i = 0; i < 2; i++) {
        gl.GenTextures(1, &hwgl.gl_tex[i]);
        hwgl.gl_object[i] = hwgl.DXRegisterObject(hwgl.gl_device, hwgl.rgb_tex[i],
                                                  hwgl.gl_tex[i], GL_TEXTURE_2D,
                                                  WGL_ACCESS_READ_ONLY_NV);
        if (!hwgl.gl_object[i])
            goto fail;

        hwgl_lock();
        if (!hwgl.DXLockObjects(hwgl.gl_device, 1, &hwgl.gl_object[i])) {
            hwgl_unlock();
            goto fail;
        }
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
        gl.BindTexture(GL_TEXTURE_2D, hwgl.gl_tex[i]);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.BindTexture(GL_TEXTURE_2D, prev_tex0);
        gl.ActiveTexture(prev_active);
        hwgl.DXUnlockObjects(hwgl.gl_device, 1, &hwgl.gl_object[i]);
        hwgl_unlock();
    }

    hwgl.w = w;
    hwgl.h = h;
    return 0;

fail:
    av_log(NULL, AV_LOG_WARNING, "FSR: D3D11-GL interop setup failed\n");
    hwgl_destroy_size();
    return -1;
}

static int hwgl_convert(AVFrame *frame, int slot)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC idesc = { 0 };
    ID3D11VideoProcessorInputView *in_view = NULL;
    D3D11_VIDEO_PROCESSOR_STREAM stream = { 0 };
    int matrix, range;
    HRESULT hr;

    if (hwgl.vcontext1) {
        /* Full DXGI color spaces: BT.2020 + PQ/HLG inputs get tone-mapped
         * to SDR BT.709 by the video processor (washed-out HDR fix). */
        int full = frame->color_range == AVCOL_RANGE_JPEG;
        int cs;

        int out_cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            /* Keep the PQ code values intact through the VP (the driver's own
             * PQ->SDR conversion is not a tone map and crushes darks) and tone-
             * map in the GL shaders. Tag BOTH sides honestly as PQ (G2084) so
             * the transfer is a no-op and the VP does only the BT.2020 YCbCr
             * de-matrix + studio->full range expansion.
             *
             * This previously tagged both sides as G22 to fake the no-op, but on
             * current NVIDIA drivers that lie makes the VP compute the studio->
             * full expansion in the wrong space and over-saturates the output -
             * skin railed into a clipped over-red orange (headless A/B confirmed
             * the G22 tag gave forehead PQ RGB 0.273/0.162/0.079 vs swscale's
             * correct 0.233/0.169/0.092; the G2084 tag reproduces swscale). */
            cs = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
            out_cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        } else if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
            cs = DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        else if (frame->colorspace == AVCOL_SPC_BT2020_NCL)
            cs = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        else if (frame->colorspace == AVCOL_SPC_BT709 ||
                 (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720))
            cs = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        else
            cs = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        if (cs != hwgl.cs_dxgi) {
            if (hwgl.lock)
                hwgl.lock(hwgl.lock_ctx);
            ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(hwgl.vcontext1,
                hwgl.vp, 0, cs);
            ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(hwgl.vcontext1,
                hwgl.vp, out_cs);
            if (hwgl.unlock)
                hwgl.unlock(hwgl.lock_ctx);
            hwgl.cs_dxgi = cs;
            if (frame->color_trc == AVCOL_TRC_SMPTE2084)
                av_log(NULL, AV_LOG_INFO,
                       "FSR: HDR10 input, PQ tone mapping in the shader\n");
        }
        hdr_active = frame->color_trc == AVCOL_TRC_SMPTE2084;
        /* Content peak for the shader tone mapper: MaxCLL when present,
         * otherwise the mastering display peak, otherwise 1000 nits. */
        if (hdr_active && hwgl.hdr_md_state < 2) {
            AVFrameSideData *sd_m =
                av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
            AVFrameSideData *sd_c =
                av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);

            if (sd_c && ((AVContentLightMetadata *)sd_c->data)->MaxCLL)
                hdr_peak = (float)((AVContentLightMetadata *)sd_c->data)->MaxCLL;
            else if (sd_m && ((AVMasteringDisplayMetadata *)sd_m->data)->has_luminance)
                hdr_peak = (float)av_q2d(((AVMasteringDisplayMetadata *)sd_m->data)->max_luminance);
            if (sd_c || sd_m) {
                hwgl.hdr_md_state = 2;
                av_log(NULL, AV_LOG_INFO, "FSR: HDR content peak %.0f nits\n",
                       hdr_peak);
            }
        }
        goto colorspace_done;
    }

    matrix = frame->colorspace == AVCOL_SPC_BT709 ||
             (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720) ? 1 : 0;
    range  = frame->color_range == AVCOL_RANGE_JPEG
             ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
             : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    if (matrix != hwgl.cs_matrix || range != hwgl.cs_range) {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE incs = { 0 }, outcs = { 0 };

        incs.YCbCr_Matrix  = matrix;
        incs.Nominal_Range = range;
        outcs.RGB_Range    = 0; /* full-range RGB out */
        if (hwgl.lock)
            hwgl.lock(hwgl.lock_ctx);
        ID3D11VideoContext_VideoProcessorSetStreamColorSpace(hwgl.vcontext, hwgl.vp, 0, &incs);
        ID3D11VideoContext_VideoProcessorSetOutputColorSpace(hwgl.vcontext, hwgl.vp, &outcs);
        if (hwgl.unlock)
            hwgl.unlock(hwgl.lock_ctx);
        hwgl.cs_matrix = matrix;
        hwgl.cs_range  = range;
    }
colorspace_done:

    idesc.FourCC               = 0;
    idesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    idesc.Texture2D.MipSlice   = 0;
    idesc.Texture2D.ArraySlice = (UINT)(intptr_t)frame->data[1];
    hr = ID3D11VideoDevice_CreateVideoProcessorInputView(hwgl.vdevice,
             (ID3D11Resource *)frame->data[0], hwgl.vp_enum, &idesc, &in_view);
    if (FAILED(hr))
        return -1;

    stream.Enable        = TRUE;
    stream.pInputSurface = in_view;
    hwgl_lock();
    /* Restrict the processor to the frame's valid display region. D3D11
     * decode surfaces are coded-size textures whose height/width are padded up
     * to a codec alignment (e.g. HEVC 1606 -> 1664); without a source rect the
     * processor scales the whole padded surface into the output, so the black
     * padding rows show up as a bar at the bottom (very visible in fill mode). */
    {
        RECT src_rect = { 0, 0, frame->width, frame->height };
        ID3D11VideoContext_VideoProcessorSetStreamSourceRect(hwgl.vcontext, hwgl.vp,
                                                             0, TRUE, &src_rect);
    }
    hr = ID3D11VideoContext_VideoProcessorBlt(hwgl.vcontext, hwgl.vp, hwgl.out_view, 0, 1, &stream);
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext_CopyResource(hwgl.dcontext,
                                         (ID3D11Resource *)hwgl.rgb_tex[slot],
                                         (ID3D11Resource *)hwgl.vp_tex);
        ID3D11DeviceContext_Flush(hwgl.dcontext);
    }
    hwgl_unlock();
    ID3D11VideoProcessorInputView_Release(in_view);
    if (FAILED(hr))
        return -1;
    hwgl.slot_frame[slot] = frame;
    hwgl.slot_pts[slot]   = frame->pts;
    fg_on_convert(frame, slot);
    return 0;
}

/* Make sure the given decoded frame's RGB image sits in one of the two GL
 * slots, converting it if needed. Returns the slot index or -1 on error.
 * make_current selects whether the slot becomes the "current" one that the
 * plain display path samples from. */
static int hwgl_frame_to_slot(AVFrame *frame, int make_current)
{
    int slot = -1;

    for (int i = 0; i < 2; i++)
        if (hwgl.slot_frame[i] == frame && hwgl.slot_pts[i] == frame->pts)
            slot = i;
    if (slot < 0) {
        slot = hwgl.cur_slot ^ 1;
        if (hwgl_convert(frame, slot) < 0)
            return -1;
    }
    if (make_current)
        hwgl.cur_slot = slot;
    return slot;
}

int fsr_hw_interop_failed(void)
{
    return hwgl.state < 0;
}

int fsr_hw_draw(SDL_Renderer *renderer, AVFrame *frame, const SDL_Rect *rect,
                int fsr_on, float sharpness)
{
    int engaged;

    if (hwgl.state < 0 || fsr_state != 1)
        return 0;

    SDL_RenderFlush(renderer); /* make sure the renderer's GL context is current */

    if (hwgl.state == 0) {
        if (hwgl_init(frame) < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "FSR: D3D11-GL zero-copy interop unavailable, using copy-back\n");
            hwgl_destroy();
            hwgl.state = -1;
            return 0;
        }
        av_log(NULL, AV_LOG_INFO, "FSR: D3D11-GL zero-copy display active\n");
        hwgl.state = 1;
    }

    if (hwgl_ensure_size(frame->width, frame->height) < 0)
        goto fail_permanent;
    if (ensure_textures(renderer, frame->width, frame->height, rect->w, rect->h) < 0)
        return 0;

    engaged = fsr_on && (rect->w > frame->width || rect->h > frame->height);
    if (fsr_on)
        log_engage_transition(engaged, frame->width, frame->height, rect);

    /* Repeated refreshes of the same frame (pause, toast overlay) reuse the
     * already-rendered out_tex; only new content touches D3D11 and GL. */
    if (frame == hwgl.last_frame && frame->pts == hwgl.last_pts &&
        rect->w == hwgl.last_rect_w && rect->h == hwgl.last_rect_h &&
        engaged == hwgl.last_engaged && sharpness == hwgl.last_sharp &&
        rcas_denoise == hwgl.last_denoise) {
        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderCopy(renderer, out_tex, NULL, rect);
        return 1;
    }
    hwgl.last_frame = NULL;

    {
        int slot = hwgl_frame_to_slot(frame, 1);

        if (slot < 0)
            goto fail_permanent;
    }

    hwgl_lock();
    if (!hwgl.DXLockObjects(hwgl.gl_device, 1, &hwgl.gl_object[hwgl.cur_slot])) {
        hwgl_unlock();
        goto fail_permanent;
    }
    if (engaged) {
        GLfloat con0[4];

        con0[0] = (GLfloat)frame->width  / rect->w;
        con0[1] = (GLfloat)frame->height / rect->h;
        con0[2] = 0.5f * con0[0] - 0.5f;
        con0[3] = 0.5f * con0[1] - 0.5f;
        if (run_pass2(renderer, easu_prog, NULL, hwgl.gl_tex[hwgl.cur_slot], easu_tex,
                      rect->w, rect->h, con0, 0.0f) < 0 ||
            run_pass(renderer, rcas_prog, easu_tex, out_tex,
                     rect->w, rect->h, NULL, exp2f(-sharpness)) < 0) {
            hwgl.DXUnlockObjects(hwgl.gl_device, 1, &hwgl.gl_object[hwgl.cur_slot]);
            hwgl_unlock();
            goto fail_soft;
        }
    } else {
        if (run_pass2(renderer, copy_prog, NULL, hwgl.gl_tex[hwgl.cur_slot], out_tex,
                      rect->w, rect->h, NULL, 0.0f) < 0) {
            hwgl.DXUnlockObjects(hwgl.gl_device, 1, &hwgl.gl_object[hwgl.cur_slot]);
            hwgl_unlock();
            goto fail_soft;
        }
    }
    hwgl.DXUnlockObjects(hwgl.gl_device, 1, &hwgl.gl_object[hwgl.cur_slot]);
    hwgl_unlock();

    hwgl.last_frame   = frame;
    hwgl.last_pts     = frame->pts;
    hwgl.last_rect_w  = rect->w;
    hwgl.last_rect_h  = rect->h;
    hwgl.last_engaged = engaged;
    hwgl.last_denoise = rcas_denoise;
    hwgl.last_sharp   = sharpness;

    if (getenv("FSR_DEBUG_READBACK")) {
        static int rb_count;

        rb_count++;
        if (rb_count > 60 && rb_count <= 65 &&
            SDL_SetRenderTarget(renderer, out_tex) == 0) {
            unsigned char px[4] = { 0 };
            unsigned char *buf = av_malloc((size_t)rect->w * 4);

            SDL_RenderFlush(renderer);
            gl.ReadPixels(rect->w / 2, rect->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (buf) { /* average luma over a sparse grid of rows */
                double sum = 0;
                int n = 0;

                for (int ry = rect->h / 8; ry < rect->h; ry += rect->h / 8) {
                    gl.ReadPixels(0, ry, rect->w, 1, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                    for (int rx = 0; rx < rect->w; rx += 8) {
                        sum += 0.299 * buf[rx * 4] + 0.587 * buf[rx * 4 + 1] +
                               0.114 * buf[rx * 4 + 2];
                        n++;
                    }
                }
                av_log(NULL, AV_LOG_INFO,
                       "FSR: out_tex center %u %u %u, avg luma %.1f (%d samples)\n",
                       px[0], px[1], px[2], n ? sum / n : 0, n);
                av_free(buf);
            }
        }
    }

    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, out_tex, NULL, rect);
    return 1;

fail_soft:
    SDL_SetRenderTarget(renderer, NULL);
    return 0;

fail_permanent:
    SDL_SetRenderTarget(renderer, NULL);
    av_log(NULL, AV_LOG_WARNING, "FSR: D3D11 zero-copy display failed, using copy-back\n");
    hwgl_destroy();
    hwgl.state = -1;
    return 0;
}

/* ---- frame generation: NVIDIA hardware optical flow + GL midpoint warp -- */

#include <ffnvcodec/dynlink_loader.h>
#include "nvOpticalFlowCuda.h"

typedef NV_OF_STATUS (NVOFAPI *PFN_NvOFAPICreateInstanceCuda)(uint32_t apiVer,
        NV_OF_CUDA_API_FUNCTION_LIST *functionList);

/* Output grid of the hardware optical flow: one motion vector per FG_GRID
 * pixels. A coarser grid means a cell straddling a moving subject's edge
 * mixes the subject's motion with the background's, which is what makes the
 * silhouette shimmer, so this is the main quality lever on the warp path.
 * Measured per frame pair on an RTX 5080 (forward + backward, real frames),
 * against the 41.7 ms a 23.976 fps interval allows:
 *     1080p  4x4 3.2 ms   2x2 8.3 ms   1x1 17.4 ms
 *     4K     4x4 6.0 ms   2x2 18.8 ms  1x1 63.8 ms  <- 1x1 cannot hold 4K
 * 2x2 buys 4x the flow resolution and still fits at both sizes. Note the
 * flow is computed once per *pair*, not per generated frame, so this cost
 * does not scale with the FG multiplier (unlike RIFE).
 * The grid is a runtime value (fg.grid): nvOFInit is tried at FG_GRID first and
 * falls back to 4 on GPUs that reject the finer grid (Turing and earlier only
 * support 4), so the shader gets it through the flowGrid uniform rather than a
 * compile-time constant. */
#define FG_GRID     2

static const char *fg_src =
    "#version 330\n"
    "uniform sampler2D prevTex;\n"
    "uniform sampler2D curTex;\n"
    "uniform isampler2D flowFwd;\n" /* prev->cur, S10.5 px, flowGrid px/vec */
    "uniform isampler2D flowBwd;\n" /* cur->prev */
    "uniform float phase;\n"        /* interpolation position, 0=prev 1=cur */
    "uniform vec2 outSize;\n"       /* render size; may differ from native */
    "uniform float flowGrid;\n"     /* flow-field grid size (px per vector) */
    "out vec4 fragColor;\n"
    HDR_TM_GLSL
    /* Bilinearly sample the 4x4-grid flow field (integer texture, so the
     * filtering is done by hand); smooths out block-shaped artifacts. */
    "vec2 sampleFlow(isampler2D t, vec2 pg) {\n"
    "    vec2 fs = vec2(textureSize(t, 0));\n"
    "    vec2 g  = clamp(pg - 0.5, vec2(0.0), fs - 1.0);\n"
    "    ivec2 g0 = ivec2(floor(g));\n"
    "    ivec2 g1 = min(g0 + 1, ivec2(fs) - 1);\n"
    "    vec2 f = g - vec2(g0);\n"
    "    vec2 a = vec2(texelFetch(t, g0, 0).xy);\n"
    "    vec2 b = vec2(texelFetch(t, ivec2(g1.x, g0.y), 0).xy);\n"
    "    vec2 c = vec2(texelFetch(t, ivec2(g0.x, g1.y), 0).xy);\n"
    "    vec2 d = vec2(texelFetch(t, g1, 0).xy);\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) / 32.0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 ts = vec2(textureSize(curTex, 0));\n"
    "    vec2 uv = gl_FragCoord.xy / outSize;\n"
    "    vec2 pg = uv * ts / flowGrid;\n"
    "    vec2 F = sampleFlow(flowFwd, pg);\n"
    "    vec2 B = sampleFlow(flowBwd, pg);\n"
    "    vec3 cPrev = texture(prevTex, uv - phase * F / ts).rgb;\n"
    "    vec3 cCur  = texture(curTex,  uv - (1.0 - phase) * B / ts).rgb;\n"
    /* Confidence from forward/backward consistency, relative to the motion
     * magnitude; where flow disagrees (occlusions, errors) fall back to the
     * unwarped temporally-nearer frame instead of ghosting. */
    "    float err = length(F + B);\n"
    "    float mag = length(F) + length(B);\n"
    /* tolerance grows with motion but is capped: unlimited slack let large
     * shaky motion pass garbage through (image tearing) */
    "    float w = clamp(1.0 - err / (3.0 + 0.25 * min(mag, 32.0)), 0.0, 1.0);\n"
    "    vec3 fallback = phase < 0.5 ? texture(prevTex, uv).rgb\n"
    "                                : texture(curTex, uv).rgb;\n"
    "    vec3 mid = mix(fallback, mix(cPrev, cCur, phase), w);\n"
    "    if (hdrMode > 0.5)\n"
    "        mid = hdr_tonemap(mid);\n"
    "    fragColor = vec4(mid, 1.0);\n"
    "}\n";

static struct {
    int state;                    /* 0 = untried, 1 = ready, -1 = unavailable */
    CudaFunctions *cu;
    CUcontext ctx;
    NV_OF_CUDA_API_FUNCTION_LIST of;
    NvOFHandle hof;
    int w, h, gw, gh;             /* session and flow-grid dimensions */
    int grid;                     /* flow-grid size actually in use (2 or 4) */
    NvOFGPUBufferHandle in_buf[2];
    NvOFGPUBufferHandle flow_buf[2];      /* [0] = forward, [1] = backward */
    NV_OF_CUDA_BUFFER_STRIDE_INFO flow_stride[2];
    int16_t *flow_host[2];   /* host copy (every 2nd row) for shake stats */
    int pair_skip;           /* current pair too shaky to interpolate */
    /* CUDA-registered staging texture. The VideoProcessor writes with the
     * GPU video engine, which must never touch a CUDA-registered resource
     * (driver crash); a 3D-engine CopyResource feeds this one instead. */
    ID3D11Texture2D *cuda_tex;
    CUgraphicsResource vp_res;            /* CUDA view of fg.cuda_tex */
    CUgraphicsResource flow_res[2];       /* CUDA views of the GL flow textures */
    GLuint flow_tex[2];
    GLuint prog;
    GLint phase_loc;
    GLint outsize_loc;
    GLint flowgrid_loc;
    GLint hdr_loc, peak_loc;
    const void *in_frame[2];              /* what each OF input buffer holds */
    int64_t in_pts[2];
    const void *pair_a, *pair_b;          /* frames the current flow refers to */
    int64_t pair_apts, pair_bpts;
    SDL_Texture *fg_tex;                  /* interpolated frame, native size */
    int fg_w, fg_h;
} fg;

static void fg_push_ctx(void);
static void fg_pop_ctx(void);

static void fg_destroy(void)
{
    if (!hwgl_exiting) {
        for (int i = 0; i < 2; i++) {
            if (fg.flow_res[i])
                fg.cu->cuGraphicsUnregisterResource(fg.flow_res[i]);
            if (fg.flow_tex[i])
                gl.DeleteTextures(1, &fg.flow_tex[i]);
            if (fg.in_buf[i])
                fg.of.nvOFDestroyGPUBufferCuda(fg.in_buf[i]);
            if (fg.flow_buf[i])
                fg.of.nvOFDestroyGPUBufferCuda(fg.flow_buf[i]);
            av_freep(&fg.flow_host[i]);
        }
        if (fg.vp_res)
            fg.cu->cuGraphicsUnregisterResource(fg.vp_res);
        if (fg.cuda_tex)
            ID3D11Texture2D_Release(fg.cuda_tex);
        /* fg.hof and fg.ctx are boot-owned and live for the process. */
        if (fg.fg_tex)
            SDL_DestroyTexture(fg.fg_tex);
    }
    for (int i = 0; i < 2; i++) {
        fg.flow_res[i] = NULL;
        fg.flow_tex[i] = 0;
        fg.in_buf[i]   = NULL;
        fg.flow_buf[i] = NULL;
        fg.in_frame[i] = NULL;
    }
    fg.vp_res   = NULL;
    fg.cuda_tex = NULL;
    fg.fg_tex   = NULL;
    fg.w = fg.h = fg.fg_w = fg.fg_h = 0;
    fg.pair_a = fg.pair_b = NULL;
    if (fg.state == 1)
        fg.state = 0;
}

/* Copy the just-converted frame (sitting in hwgl.vp_tex) into the optical
 * flow input buffer for this slot. Called from hwgl_convert(). */
static void fg_on_convert(AVFrame *frame, int slot)
{
    CUarray arr = NULL;
    CUDA_MEMCPY2D cp = { 0 };


    if (fg.state != 1 || !fg.vp_res || !fg.in_buf[slot])
        return;
    /* 3D-engine copy into the CUDA-registered staging texture (the video
     * engine's output must never hit a CUDA-registered resource), then map
     * it. Serialized against the decoder like all our other D3D usage. */
    fg_push_ctx();
    hwgl_lock();
    ID3D11DeviceContext_CopyResource(hwgl.dcontext,
                                     (ID3D11Resource *)fg.cuda_tex,
                                     (ID3D11Resource *)hwgl.vp_tex);
    ID3D11DeviceContext_Flush(hwgl.dcontext);
    if (fg.cu->cuGraphicsMapResources(1, &fg.vp_res, 0) != CUDA_SUCCESS) {
        hwgl_unlock();
        fg_pop_ctx();
        return;
    }
    if (fg.cu->cuGraphicsSubResourceGetMappedArray(&arr, fg.vp_res, 0, 0) == CUDA_SUCCESS) {
        NV_OF_CUDA_BUFFER_STRIDE_INFO si;

        fg.of.nvOFGPUBufferGetStrideInfo(fg.in_buf[slot], &si);
        cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.srcArray      = arr;
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstDevice     = fg.of.nvOFGPUBufferGetCUdeviceptr(fg.in_buf[slot]);
        cp.dstPitch      = si.strideInfo[0].strideXInBytes;
        cp.WidthInBytes  = (size_t)fg.w * 4;
        cp.Height        = fg.h;
        if (fg.cu->cuMemcpy2D(&cp) == CUDA_SUCCESS) {
            fg.in_frame[slot] = frame;
            fg.in_pts[slot]   = frame->pts;
        }
    }
    fg.cu->cuGraphicsUnmapResources(1, &fg.vp_res, 0);
    hwgl_unlock();
    fg_pop_ctx();
}

/* Startup part of frame-generation setup: driver libraries and a CUDA
 * context on the GL GPU. Deliberately avoids any D3D11 calls (CUDA's
 * cuD3D11GetDevice creates a probe D3D device, which races with active
 * decoding inside the NVIDIA driver) so it can also run safely before
 * decoding starts. Returns 0 on success. */
static int fg_booted; /* 0 = not tried, 1 = ok, -1 = failed */

static int fg_boot(void)
{
    PFN_NvOFAPICreateInstanceCuda create_of;
    HMODULE nvof_dll;
    CUdevice cudev = -1;
    int count = 0;

    if (fg_booted)
        return fg_booted > 0 ? 0 : -1;
    fg_booted = -1;

    nvof_dll = LoadLibraryA("nvofapi64.dll");
    if (!nvof_dll) {
        av_log(NULL, AV_LOG_WARNING, "FG: nvofapi64.dll not found (NVIDIA driver required)\n");
        return -1;
    }
    create_of = (PFN_NvOFAPICreateInstanceCuda)GetProcAddress(nvof_dll, "NvOFAPICreateInstanceCuda");
    if (!create_of || create_of(NV_OF_API_VERSION, &fg.of) != NV_OF_SUCCESS) {
        av_log(NULL, AV_LOG_WARNING, "FG: NvOFAPICreateInstanceCuda failed\n");
        return -1;
    }

    if (cuda_load_functions(&fg.cu, NULL) < 0) {
        av_log(NULL, AV_LOG_WARNING, "FG: cannot load CUDA driver API\n");
        return -1;
    }
    if (fg.cu->cuInit(0) != CUDA_SUCCESS)
        return -1;
    /* Pick the CUDA device whose name appears in GL_RENDERER (avoids the
     * D3D-probing cuD3D11GetDevice). */
    fg.cu->cuDeviceGetCount(&count);
    for (int i = 0; i < count; i++) {
        CUdevice d;
        char name[128] = "";

        if (fg.cu->cuDeviceGet(&d, i) != CUDA_SUCCESS)
            continue;
        fg.cu->cuDeviceGetName(name, sizeof(name), d);
        if (name[0] && gl_renderer_str[0] && strstr(gl_renderer_str, name)) {
            cudev = d;
            break;
        }
    }
    if (cudev < 0) {
        av_log(NULL, AV_LOG_WARNING, "FG: no CUDA device matching '%s'\n", gl_renderer_str);
        return -1;
    }
    if (fg.cu->cuCtxCreate(&fg.ctx, 0, cudev) != CUDA_SUCCESS)
        return -1;
    /* Create the optical flow session while no decoding is running; doing
     * this with an active D3D11 decoder crashes inside the driver. */
    if (fg.of.nvCreateOpticalFlowCuda(fg.ctx, &fg.hof) != NV_OF_SUCCESS) {
        av_log(NULL, AV_LOG_WARNING, "FG: optical flow session creation failed\n");
        return -1;
    }
    fg.of.nvOFSetIOCudaStreams(fg.hof, 0, 0);
    /* Keep the CUDA context off this (GL) thread except around our calls. */
    {
        CUcontext dummy;

        fg.cu->cuCtxPopCurrent(&dummy);
    }
    av_log(NULL, AV_LOG_INFO, "FG: CUDA optical flow session ready\n");
    fg_booted = 1;
    return 0;
}

static void fg_push_ctx(void)
{
    if (fg.cu && fg.ctx)
        fg.cu->cuCtxPushCurrent(fg.ctx);
}

static void fg_pop_ctx(void)
{
    CUcontext dummy;

    if (fg.cu && fg.ctx)
        fg.cu->cuCtxPopCurrent(&dummy);
}

int fsr_fg_boot(void)
{
    return fg_boot();
}

static int fg_init_body(void)
{
    NV_OF_INIT_PARAMS ip = { 0 };
    NV_OF_BUFFER_DESCRIPTOR bd = { 0 };
    uint32_t last_err_sz;
    char last_err[256];


    /* Try the preferred (finer) grid first, then fall back to 4. Turing and
     * earlier only support grid 4 and reject 2/1 with an init error; Ampere+
     * take the finer grids. */
    int grids[2] = { FG_GRID, 4 };
    int ngrids   = FG_GRID == 4 ? 1 : 2;
    int got      = 0;

    ip.width       = fg.w;
    ip.height      = fg.h;
    ip.mode        = NV_OF_MODE_OPTICALFLOW;
    /* Best flow quality up to 1080p; above that the SLOW preset costs too
     * much of the frame budget to sustain 60 presented fps. */
    ip.perfLevel   = fg.w * fg.h > 1920 * 1080 ? NV_OF_PERF_LEVEL_MEDIUM
                                               : NV_OF_PERF_LEVEL_SLOW;
    for (int i = 0; i < ngrids; i++) {
        ip.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)grids[i];
        if (fg.of.nvOFInit(fg.hof, &ip) == NV_OF_SUCCESS) {
            fg.grid = grids[i];
            got = 1;
            break;
        }
    }
    if (!got) {
        last_err_sz = sizeof(last_err);
        last_err[0] = 0;
        fg.of.nvOFGetLastError(fg.hof, last_err, &last_err_sz);
        av_log(NULL, AV_LOG_WARNING, "FG: optical flow init failed: %s\n", last_err);
        return -1;
    }
    if (fg.grid != FG_GRID)
        av_log(NULL, AV_LOG_INFO,
               "FG: grid %d unsupported here, using grid %d\n", FG_GRID, fg.grid);

    /* Flow-buffer dimensions follow the grid actually granted. */
    fg.gw = (fg.w + fg.grid - 1) / fg.grid;
    fg.gh = (fg.h + fg.grid - 1) / fg.grid;

    bd.width        = fg.w;
    bd.height       = fg.h;
    bd.bufferUsage  = NV_OF_BUFFER_USAGE_INPUT;
    bd.bufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    for (int i = 0; i < 2; i++)
        if (fg.of.nvOFCreateGPUBufferCuda(fg.hof, &bd, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
                                          &fg.in_buf[i]) != NV_OF_SUCCESS)
            return -1;
    bd.width        = fg.gw;
    bd.height       = fg.gh;
    bd.bufferUsage  = NV_OF_BUFFER_USAGE_OUTPUT;
    bd.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
    for (int i = 0; i < 2; i++) {
        if (fg.of.nvOFCreateGPUBufferCuda(fg.hof, &bd, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
                                          &fg.flow_buf[i]) != NV_OF_SUCCESS)
            return -1;
        fg.of.nvOFGPUBufferGetStrideInfo(fg.flow_buf[i], &fg.flow_stride[i]);
        fg.flow_host[i] = av_malloc((size_t)fg.gw * 4 * (fg.gh / 2 + 1));
    }

    {
        D3D11_TEXTURE2D_DESC tdesc = { 0 };

        tdesc.Width            = fg.w;
        tdesc.Height           = fg.h;
        tdesc.MipLevels        = 1;
        tdesc.ArraySize        = 1;
        tdesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        tdesc.SampleDesc.Count = 1;
        tdesc.Usage            = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(hwgl.device, &tdesc, NULL, &fg.cuda_tex))) {
            av_log(NULL, AV_LOG_WARNING, "FG: staging texture creation failed\n");
            return -1;
        }
    }
    hwgl_lock();
    if (fg.cu->cuGraphicsD3D11RegisterResource(&fg.vp_res, fg.cuda_tex,
                                               CU_GRAPHICS_REGISTER_FLAGS_NONE) != CUDA_SUCCESS) {
        hwgl_unlock();
        av_log(NULL, AV_LOG_WARNING, "FG: CUDA-D3D11 interop registration failed\n");
        return -1;
    }
    hwgl_unlock();

    /* GL flow textures (RG16I) shared with CUDA */
    {
        GLint prev_tex0 = 0, prev_active = 0;

        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
        for (int i = 0; i < 2; i++) {
            gl.GenTextures(1, &fg.flow_tex[i]);
            gl.BindTexture(GL_TEXTURE_2D, fg.flow_tex[i]);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RG16I, fg.gw, fg.gh, 0,
                          GL_RG_INTEGER, GL_SHORT, NULL);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        gl.BindTexture(GL_TEXTURE_2D, prev_tex0);
        gl.ActiveTexture(prev_active);
    }
    for (int i = 0; i < 2; i++)
        if (fg.cu->cuGraphicsGLRegisterImage(&fg.flow_res[i], fg.flow_tex[i], GL_TEXTURE_2D,
                                             CU_GRAPHICS_REGISTER_FLAGS_NONE) != CUDA_SUCCESS) {
            av_log(NULL, AV_LOG_WARNING, "FG: CUDA-GL interop registration failed\n");
            return -1;
        }

    fg.prog = build_program(vertex_src, fg_src);
    if (!fg.prog)
        return -1;
    {
        GLint prev_prog = 0, loc;
        static const char *const names[4] = { "prevTex", "curTex", "flowFwd", "flowBwd" };

        gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        gl.UseProgram(fg.prog);
        for (int i = 0; i < 4; i++) {
            loc = gl.GetUniformLocation(fg.prog, names[i]);
            if (loc >= 0)
                gl.Uniform1i(loc, i);
        }
        gl.UseProgram(prev_prog);
        fg.phase_loc    = gl.GetUniformLocation(fg.prog, "phase");
        fg.outsize_loc  = gl.GetUniformLocation(fg.prog, "outSize");
        fg.flowgrid_loc = gl.GetUniformLocation(fg.prog, "flowGrid");
        fg.hdr_loc      = gl.GetUniformLocation(fg.prog, "hdrMode");
        fg.peak_loc     = gl.GetUniformLocation(fg.prog, "hdrPeak");
    }
    return 0;
}

static int fg_init(void)
{
    int ret;

    if (fg_boot() < 0)
        return -1;

    fg.w  = hwgl.w;
    fg.h  = hwgl.h;
    fg.gw = (fg.w + FG_GRID - 1) / FG_GRID;
    fg.gh = (fg.h + FG_GRID - 1) / FG_GRID;

    fg_push_ctx();
    ret = fg_init_body();
    fg_pop_ctx();
    return ret;
}

static int fg_compute_flow_body(int sp, int sn)
{
    NV_OF_EXECUTE_INPUT_PARAMS ein = { 0 };
    NV_OF_EXECUTE_OUTPUT_PARAMS eout = { 0 };

    ein.disableTemporalHints = 1;
    ein.inputFrame     = fg.in_buf[sp];
    ein.referenceFrame = fg.in_buf[sn];
    eout.outputBuffer  = fg.flow_buf[0];
    if (fg.of.nvOFExecute(fg.hof, &ein, &eout) != NV_OF_SUCCESS)
        return -1;
    ein.inputFrame     = fg.in_buf[sn];
    ein.referenceFrame = fg.in_buf[sp];
    eout.outputBuffer  = fg.flow_buf[1];
    if (fg.of.nvOFExecute(fg.hof, &ein, &eout) != NV_OF_SUCCESS)
        return -1;

    if (fg.cu->cuGraphicsMapResources(2, fg.flow_res, 0) != CUDA_SUCCESS)
        return -1;
    for (int i = 0; i < 2; i++) {
        CUarray arr = NULL;
        CUDA_MEMCPY2D cp = { 0 };

        if (fg.cu->cuGraphicsSubResourceGetMappedArray(&arr, fg.flow_res[i], 0, 0) != CUDA_SUCCESS)
            continue;
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.srcDevice     = fg.of.nvOFGPUBufferGetCUdeviceptr(fg.flow_buf[i]);
        cp.srcPitch      = fg.flow_stride[i].strideInfo[0].strideXInBytes;
        cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.dstArray      = arr;
        cp.WidthInBytes  = (size_t)fg.gw * 4;
        cp.Height        = fg.gh;
        fg.cu->cuMemcpy2D(&cp);
    }
    fg.cu->cuGraphicsUnmapResources(2, fg.flow_res, 0);

    /* Shake detector: pull both flow fields to the host (every 2nd grid
     * row) and measure the average motion magnitude and forward/backward
     * inconsistency. Violent shake produces huge, inconsistent flow - the
     * warp would tear the image apart, and interpolation buys nothing
     * there anyway, so such pairs are flagged and skipped. */
    fg.pair_skip = 0;
    if (fg.flow_host[0] && fg.flow_host[1]) {
        double sum_mag = 0, sum_err = 0;
        int rows = fg.gh / 2, n = 0;

        for (int i = 0; i < 2; i++) {
            CUDA_MEMCPY2D cp = { 0 };

            cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            cp.srcDevice     = fg.of.nvOFGPUBufferGetCUdeviceptr(fg.flow_buf[i]);
            cp.srcPitch      = fg.flow_stride[i].strideInfo[0].strideXInBytes * 2;
            cp.dstMemoryType = CU_MEMORYTYPE_HOST;
            cp.dstHost       = fg.flow_host[i];
            cp.dstPitch      = (size_t)fg.gw * 4;
            cp.WidthInBytes  = (size_t)fg.gw * 4;
            cp.Height        = rows;
            if (fg.cu->cuMemcpy2D(&cp) != CUDA_SUCCESS)
                goto stats_done;
        }
        for (int y = 0; y < rows; y++) {
            const int16_t *f = fg.flow_host[0] + (size_t)y * fg.gw * 2;
            const int16_t *b = fg.flow_host[1] + (size_t)y * fg.gw * 2;

            for (int x = 0; x < fg.gw * 2; x += 4) { /* every 2nd cell */
                double fx = f[x] / 32.0, fy = f[x + 1] / 32.0;
                double bx = b[x] / 32.0, by = b[x + 1] / 32.0;

                sum_mag += fabs(fx) + fabs(fy);
                sum_err += fabs(fx + bx) + fabs(fy + by);
                n++;
            }
        }
        if (n) {
            double mag = sum_mag / n, err = sum_err / n;

            /* Inconsistency is the tear signal; consistent motion (smooth
             * pans) stays interpolated up to a generous bound. Hand-held
             * footage sits around 10-14 px of inconsistency and still
             * interpolates fine (the per-pixel fallback covers it), so
             * only reject genuinely torn pairs. */
            fg.pair_skip = err > 20.0 || mag > 80.0;
            if (fg.pair_skip)
                av_log(NULL, AV_LOG_VERBOSE,
                       "FG: shaky pair skipped (flow %.1f px, inconsistency %.1f px)\n",
                       mag, err);
        }
    }
stats_done:
    return 0;
}

static int fg_compute_flow(int sp, int sn)
{
    int ret;

    fg_push_ctx();
    ret = fg_compute_flow_body(sp, sn);
    fg_pop_ctx();
    return ret;
}

/* Warp pass: prev+cur+flow -> fg_tex (native size, SDL target texture). */
static int fg_warp(SDL_Renderer *renderer, int sp, int sn, float phase,
                   int ow, int oh, int tonemap)
{
    GLint prev_prog = 0, prev_active = 0, prev_tex[4] = { 0 };
    GLboolean blend, scissor;
    HANDLE objs[2] = { hwgl.gl_object[sp], hwgl.gl_object[sn] };
    GLuint texs[4] = { hwgl.gl_tex[sp], hwgl.gl_tex[sn], fg.flow_tex[0], fg.flow_tex[1] };

    if (fg.fg_w != ow || fg.fg_h != oh || !fg.fg_tex) {
        if (fg.fg_tex)
            SDL_DestroyTexture(fg.fg_tex);
        fg.fg_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET, ow, oh);
        if (!fg.fg_tex)
            return -1;
        SDL_SetTextureBlendMode(fg.fg_tex, SDL_BLENDMODE_NONE);
        fg.fg_w = ow;
        fg.fg_h = oh;
    }

    if (SDL_SetRenderTarget(renderer, fg.fg_tex) < 0)
        return -1;
    SDL_RenderFlush(renderer);

    hwgl_lock();
    if (!hwgl.DXLockObjects(hwgl.gl_device, 2, objs)) {
        hwgl_unlock();
        return -1;
    }

    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    blend   = gl.IsEnabled(GL_BLEND);
    scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    for (int i = 3; i >= 0; i--) {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex[i]);
        gl.BindTexture(GL_TEXTURE_2D, texs[i]);
    }
    gl.Disable(GL_BLEND);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Viewport(0, 0, ow, oh);
    gl.UseProgram(fg.prog);
    gl.Uniform1f(fg.phase_loc, phase);
    gl.Uniform2f(fg.outsize_loc, (GLfloat)ow, (GLfloat)oh);
    gl.Uniform1f(fg.flowgrid_loc, (GLfloat)fg.grid);
    gl.Uniform1f(fg.hdr_loc, tonemap && hdr_active ? 1.0f : 0.0f);
    gl.Uniform1f(fg.peak_loc, hdr_peak);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.UseProgram(prev_prog);
    for (int i = 3; i >= 0; i--) {
        gl.ActiveTexture(GL_TEXTURE0 + i);
        gl.BindTexture(GL_TEXTURE_2D, prev_tex[i]);
    }
    if (blend)
        gl.Enable(GL_BLEND);
    if (scissor)
        gl.Enable(GL_SCISSOR_TEST);
    gl.ActiveTexture(prev_active);

    hwgl.DXUnlockObjects(hwgl.gl_device, 2, objs);
    hwgl_unlock();
    return 0;
}

int fsr_fg_available(void)
{
    return fg.state == 1;
}

/* Everything a generated frame needs except the warp and draw: OF input
 * conversion and the (cached) flow computation for the pair. Cheap when the
 * pair is already prepared, so callers may invoke it while waiting for a
 * presentation slot. Returns the frame slots in *out_sp / *out_sn. */
static int fg_prepare_pair(SDL_Renderer *renderer, AVFrame *prev, AVFrame *next,
                           int *out_sp, int *out_sn)
{
    int sp, sn;

    if (fg.state < 0 || hwgl.state != 1 || fsr_state != 1)
        return -1;
    if (prev->width != next->width || prev->height != next->height)
        return -1;

    SDL_RenderFlush(renderer);

    if (hwgl_ensure_size(prev->width, prev->height) < 0)
        return -1;

    if (fg.state == 0 || fg.w != hwgl.w || fg.h != hwgl.h) {
        fg_destroy();
        if (fg_init() < 0) {
            fg_destroy();
            fg.state = -1;
            av_log(NULL, AV_LOG_WARNING, "FG: frame generation unavailable\n");
            return -1;
        }
        fg.state = 1;
        av_log(NULL, AV_LOG_INFO,
               "FG: NVIDIA optical-flow frame generation active (%dx%d, grid %dx%d)\n",
               fg.w, fg.h, fg.gw, fg.gh);
    }

    sp = hwgl_frame_to_slot(prev, 0);
    if (sp < 0) {
        av_log(NULL, AV_LOG_VERBOSE, "FG: prev slot failed\n");
        return -1;
    }
    /* Refill the OF input if this frame was converted before FG was on. */
    if (fg.in_frame[sp] != prev || fg.in_pts[sp] != prev->pts)
        if (hwgl_convert(prev, sp) < 0) {
            av_log(NULL, AV_LOG_VERBOSE, "FG: prev refill failed\n");
            return -1;
        }
    sn = hwgl_frame_to_slot(next, 0);
    if (sn < 0 || sn == sp) {
        av_log(NULL, AV_LOG_VERBOSE, "FG: next slot failed (%d/%d)\n", sn, sp);
        return -1;
    }
    if (fg.in_frame[sn] != next || fg.in_pts[sn] != next->pts)
        if (hwgl_convert(next, sn) < 0) {
            av_log(NULL, AV_LOG_VERBOSE, "FG: next refill failed\n");
            return -1;
        }

    if (fg.pair_a != prev || fg.pair_apts != prev->pts ||
        fg.pair_b != next || fg.pair_bpts != next->pts) {
        if (fg_compute_flow(sp, sn) < 0) {
            av_log(NULL, AV_LOG_VERBOSE, "FG: flow computation failed\n");
            return -1;
        }
        fg.pair_a    = prev;
        fg.pair_apts = prev->pts;
        fg.pair_b    = next;
        fg.pair_bpts = next->pts;
    }

    if (fg.pair_skip)
        return -1; /* too shaky: show the real frames only */

    *out_sp = sp;
    *out_sn = sn;
    return 0;
}

int fsr_fg_prepare(SDL_Renderer *renderer, AVFrame *prev, AVFrame *next)
{
    int sp, sn;

    return fg_prepare_pair(renderer, prev, next, &sp, &sn) == 0;
}

/* ---- RIFE frame generation ----------------------------------------------
 * An alternative to the optical-flow warp above. The hardware flow is computed
 * on a 4x4 block grid, so a block straddling a moving subject's silhouette
 * mixes the subject's motion with the background's and the warp smears the
 * edge; RIFE estimates the flow per pixel instead. It runs on ncnn/Vulkan and
 * consumes plain RGB, so the pair is read back out of the shared GL textures
 * once per pair - that cost is amortised over every frame generated from it -
 * and only the result is uploaded per generated frame. */
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif

static int fsr_rife;                    /* set from ffplay.c (-rife / hotkey) */

void fsr_rife_set(int on)
{
    /* Only ever enable what fsr_rife_boot() already brought up: creating the
     * Vulkan device now, with the renderer live, would crash (see below). */
    fsr_rife = on && rife_available();
}

/* Bring the Vulkan device up. This MUST run before SDL_CreateRenderer:
 * creating ncnn's Vulkan device after the SDL OpenGL renderer exists leaves
 * the renderer broken, and the next SDL_CreateTexture() segfaults (reproduced
 * standalone - it is not specific to the player). Doing it first also keeps
 * the ~1 s model load out of the refresh loop. */
int fsr_rife_boot(void)
{
    return rife_init();
}

int fsr_rife_active(int w, int h)
{
    /* All three gates, so callers can report the engine actually in use:
     * a 4K source keeps the flow warp even with RIFE switched on. */
    return fsr_rife && rife_available() && rife_size_supported(w, h);
}

static uint8_t     *rife_rgb[2];        /* the pair, packed RGB24 */
static uint8_t     *rife_outbuf;
static int          rife_bw, rife_bh;   /* size the buffers were made for */
/* Which pair rife_rgb[] currently holds. The pts has to be part of the key:
 * AVFrame structs come from a pool and their addresses are recycled, so a
 * pointer-only check occasionally mistakes a *new* pair for the cached one
 * and re-interpolates stale pixels - seen as an intermittent flicker. This
 * mirrors how fg_prepare_pair() keys fg.pair_a/apts/b/bpts. */
static const void  *rife_pa, *rife_pb;
static int64_t      rife_apts, rife_bpts;
static SDL_Texture *rife_tex;

static void rife_free_buffers(void)
{
    av_freep(&rife_rgb[0]);
    av_freep(&rife_rgb[1]);
    av_freep(&rife_outbuf);
    if (rife_tex) {
        SDL_DestroyTexture(rife_tex);
        rife_tex = NULL;
    }
    rife_bw = rife_bh = 0;
    rife_pa = rife_pb = NULL;
    rife_apts = rife_bpts = AV_NOPTS_VALUE;
}

static int rife_ensure_buffers(SDL_Renderer *renderer, int w, int h)
{
    if (rife_bw == w && rife_bh == h && rife_rgb[0] && rife_outbuf && rife_tex)
        return 0;
    rife_free_buffers();
    rife_rgb[0] = av_malloc((size_t)w * h * 3);
    rife_rgb[1] = av_malloc((size_t)w * h * 3);
    rife_outbuf = av_malloc((size_t)w * h * 3);
    rife_tex    = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!rife_rgb[0] || !rife_rgb[1] || !rife_outbuf || !rife_tex) {
        rife_free_buffers();
        return -1;
    }
    SDL_SetTextureBlendMode(rife_tex, SDL_BLENDMODE_NONE);
    rife_bw = w;
    rife_bh = h;
    return 0;
}

/* Copy both frames of the current pair out of their shared GL textures. */
static int rife_fetch_pair(int sp, int sn)
{
    HANDLE objs[2] = { hwgl.gl_object[sp], hwgl.gl_object[sn] };
    int    slots[2] = { sp, sn };
    GLint  prev_tex = 0;

    hwgl_lock();
    if (!hwgl.DXLockObjects(hwgl.gl_device, 2, objs)) {
        hwgl_unlock();
        return -1;
    }
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    for (int i = 0; i < 2; i++) {
        gl.BindTexture(GL_TEXTURE_2D, hwgl.gl_tex[slots[i]]);
        gl.GetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rife_rgb[i]);
    }
    gl.BindTexture(GL_TEXTURE_2D, prev_tex);
    hwgl.DXUnlockObjects(hwgl.gl_device, 2, objs);
    hwgl_unlock();
    return 0;
}

/* Returns 1 if the interpolated frame was drawn, 0 to fall back to the warp. */
static int rife_fg_draw(SDL_Renderer *renderer, int sp, int sn,
                        const SDL_Rect *rect, float phase,
                        int fsr_on, float sharpness)
{
    void *pixels;
    int   pitch, engaged;

    if (!rife_size_supported(fg.w, fg.h))
        return 0;
    if (!rife_available())              /* boot happens before the renderer */
        return 0;
    if (rife_ensure_buffers(renderer, fg.w, fg.h) < 0)
        return 0;

    /* Re-read only when this is a different frame pair (pointer *and* pts). */
    if (rife_pa != fg.pair_a || rife_apts != fg.pair_apts ||
        rife_pb != fg.pair_b || rife_bpts != fg.pair_bpts) {
        if (rife_fetch_pair(sp, sn) < 0)
            return 0;
        rife_pa   = fg.pair_a;
        rife_apts = fg.pair_apts;
        rife_pb   = fg.pair_b;
        rife_bpts = fg.pair_bpts;
    }

    if (rife_interpolate(rife_rgb[0], rife_rgb[1], fg.w, fg.h,
                         phase, rife_outbuf) < 0) {
        av_log(NULL, AV_LOG_VERBOSE, "RIFE: interpolation failed\n");
        return 0;
    }

    if (SDL_LockTexture(rife_tex, NULL, &pixels, &pitch) < 0)
        return 0;
    for (int y = 0; y < fg.h; y++)
        memcpy((uint8_t *)pixels + (size_t)y * pitch,
               rife_outbuf + (size_t)y * fg.w * 3, (size_t)fg.w * 3);
    SDL_UnlockTexture(rife_tex);

    /* Send the generated frame through exactly the passes a real frame takes
     * (fsr_hw_draw): EASU+RCAS when upscaling, the copy shader otherwise -
     * both of which also tone-map HDR. Presenting rife_tex directly instead
     * left the generated frames as the only unsharpened, un-tone-mapped ones,
     * so every other frame looked different and the picture pulsed. */
    engaged = fsr_on && (rect->w > fg.w || rect->h > fg.h);
    if (ensure_textures(renderer, fg.w, fg.h, rect->w, rect->h) >= 0) {
        int ok;

        if (engaged) {
            GLfloat con0[4];

            con0[0] = (GLfloat)fg.w / rect->w;
            con0[1] = (GLfloat)fg.h / rect->h;
            con0[2] = 0.5f * con0[0] - 0.5f;
            con0[3] = 0.5f * con0[1] - 0.5f;
            ok = run_pass(renderer, easu_prog, rife_tex, easu_tex,
                          rect->w, rect->h, con0, 0.0f) >= 0 &&
                 run_pass(renderer, rcas_prog, easu_tex, out_tex,
                          rect->w, rect->h, NULL, exp2f(-sharpness)) >= 0;
        } else {
            ok = run_pass(renderer, copy_prog, rife_tex, out_tex,
                          rect->w, rect->h, NULL, 0.0f) >= 0;
        }
        if (ok) {
            hwgl.last_frame = NULL;     /* out_tex no longer holds a real frame */
            SDL_SetRenderTarget(renderer, NULL);
            SDL_RenderCopy(renderer, out_tex, NULL, rect);
            return 1;
        }
    }

    /* Passes unavailable: show the frame unprocessed rather than dropping it. */
    hwgl.last_frame = NULL;
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, rife_tex, NULL, rect);
    return 1;
}

int fsr_fg_draw(SDL_Renderer *renderer, AVFrame *prev, AVFrame *next,
                const SDL_Rect *rect, int fsr_on, float sharpness, float phase)
{
    int sp, sn, engaged;

    phase = phase < 0.05f ? 0.05f : phase > 0.95f ? 0.95f : phase;

    if (fg_prepare_pair(renderer, prev, next, &sp, &sn) < 0)
        return 0;

    /* RIFE replaces the warp entirely when it is on and the frame is small
     * enough to fit the time budget; it falls through to the warp otherwise. */
    if (fsr_rife && rife_fg_draw(renderer, sp, sn, rect, phase, fsr_on, sharpness))
        return 1;

    engaged = fsr_on && (rect->w > fg.w || rect->h > fg.h);
    if (engaged) {
        /* FSR path: warp at native size, then EASU+RCAS up to the target. */
        GLfloat con0[4];

        if (ensure_textures(renderer, fg.w, fg.h, rect->w, rect->h) < 0)
            return 0;
        /* keep PQ through the warp; RCAS tone-maps at the end */
        if (fg_warp(renderer, sp, sn, phase, fg.w, fg.h, 0) < 0) {
            av_log(NULL, AV_LOG_VERBOSE, "FG: warp failed\n");
            return 0;
        }
        con0[0] = (GLfloat)fg.w / rect->w;
        con0[1] = (GLfloat)fg.h / rect->h;
        con0[2] = 0.5f * con0[0] - 0.5f;
        con0[3] = 0.5f * con0[1] - 0.5f;
        if (run_pass(renderer, easu_prog, fg.fg_tex, easu_tex,
                     rect->w, rect->h, con0, 0.0f) < 0 ||
            run_pass(renderer, rcas_prog, easu_tex, out_tex,
                     rect->w, rect->h, NULL, exp2f(-sharpness)) < 0)
            goto fail;
        hwgl.last_frame = NULL; /* out_tex no longer holds the real frame */
        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderCopy(renderer, out_tex, NULL, rect);
        return 1;
    }

    /* No upscaling: warp straight at the display size (much cheaper than
     * shading at native size for high-resolution sources) and present the
     * warp target directly (tone-mapping HDR inline). */
    if (fg_warp(renderer, sp, sn, phase, rect->w, rect->h, 1) < 0) {
        av_log(NULL, AV_LOG_VERBOSE, "FG: warp failed\n");
        return 0;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, fg.fg_tex, NULL, rect);
    return 1;

fail:
    SDL_SetRenderTarget(renderer, NULL);
    return 0;
}

#else /* !CONFIG_D3D11VA */

int fsr_d3d11_adapter_index(void)
{
    return -1;
}

int fsr_d3d11_supports_codec(struct AVBufferRef *hw_device_ctx, int codec_id)
{
    return 1;
}

int fsr_hw_interop_failed(void)
{
    return 1;
}

int fsr_hw_draw(SDL_Renderer *renderer, struct AVFrame *frame, const SDL_Rect *rect,
                int fsr_on, float sharpness)
{
    return 0;
}

int fsr_fg_available(void)
{
    return 0;
}

int fsr_fg_prepare(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next)
{
    return 0;
}

int fsr_fg_boot(void)
{
    return -1;
}

int fsr_fg_draw(SDL_Renderer *renderer, struct AVFrame *prev, struct AVFrame *next,
                const SDL_Rect *rect, int fsr_on, float sharpness, float phase)
{
    return 0;
}

void        fsr_rife_set(int on)        { (void)on; }
int         fsr_rife_active(int w, int h) { (void)w; (void)h; return 0; }
int         fsr_rife_boot(void)         { return -1; }

#endif /* CONFIG_D3D11VA */

int fsr_draw(SDL_Renderer *renderer, SDL_Texture *vid_texture,
             int frame_width, int frame_height,
             const SDL_Rect *rect, int flip_v, float sharpness)
{
    GLfloat con0[4];
    int engaged;

    if (fsr_state != 1 || frame_width <= 0 || frame_height <= 0)
        return 0;

    engaged = rect->w > frame_width || rect->h > frame_height;
    log_engage_transition(engaged, frame_width, frame_height, rect);
    if (!engaged)
        return 0;

    if (ensure_textures(renderer, frame_width, frame_height, rect->w, rect->h) < 0)
        return 0;

    /* Pass 0: let SDL convert the (possibly YUV) frame to RGB, 1:1. */
    if (SDL_SetRenderTarget(renderer, native_tex) < 0)
        return 0;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopyEx(renderer, vid_texture, NULL, NULL, 0, NULL,
                     flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);

    /* Pass 1: EASU upscale to the display rect size. */
    con0[0] = (GLfloat)frame_width  / rect->w;
    con0[1] = (GLfloat)frame_height / rect->h;
    con0[2] = 0.5f * con0[0] - 0.5f;
    con0[3] = 0.5f * con0[1] - 0.5f;
    if (run_pass(renderer, easu_prog, native_tex, easu_tex,
                 rect->w, rect->h, con0, 0.0f) < 0)
        goto fail;

    /* Pass 2: RCAS sharpen at output resolution. */
    if (run_pass(renderer, rcas_prog, easu_tex, out_tex,
                 rect->w, rect->h, NULL, exp2f(-sharpness)) < 0)
        goto fail;

    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, out_tex, NULL, rect);
    return 1;

fail:
    SDL_SetRenderTarget(renderer, NULL);
    return 0;
}

/* ---- on-screen toast ----------------------------------------------- */

#define TOAST_DURATION_US 1800000
#define TOAST_FADE_US      400000
#define TOAST_MAX_CHARS    15
#define TOAST_PAD          2

static SDL_Texture *toast_tex;
static int          toast_w, toast_h;
static int          toast_sys;  /* the texture came from the system font */
static int64_t      toast_until;

/* 8x8 bitmap glyphs, one byte per row, MSB = leftmost pixel. Only the
 * characters the toast messages need. */
static const uint8_t *toast_glyph(char c)
{
    static const uint8_t gA[8] = {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00};
    static const uint8_t gF[8] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00};
    static const uint8_t gG[8] = {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00};
    static const uint8_t gH[8] = {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00};
    static const uint8_t gN[8] = {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00};
    static const uint8_t gO[8] = {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00};
    static const uint8_t gP[8] = {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00};
    static const uint8_t gR[8] = {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00};
    static const uint8_t gS[8] = {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00};
    static const uint8_t gL[8] = {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00};
    static const uint8_t gV[8] = {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00};
    static const uint8_t gT[8] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00};
    static const uint8_t gE[8] = {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00};
    static const uint8_t gU[8] = {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00};
    static const uint8_t gC[8] = {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00};
    static const uint8_t gB[8] = {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00};
    static const uint8_t gD[8] = {0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x00};
    static const uint8_t gI[8] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00};
    static const uint8_t gJ[8] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00};
    static const uint8_t gW[8] = {0x42,0x42,0x42,0x5A,0x5A,0x66,0x24,0x00};
    static const uint8_t gSl[8] = {0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00};
    static const uint8_t gDigits[10][8] = {
        {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
        {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
        {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
        {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
        {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},
        {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
        {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00},
        {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
        {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
        {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00},
    };
    static const uint8_t gDot[8] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00};
    static const uint8_t gColon[8] = {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00};
    static const uint8_t gX[8] = {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00};
    static const uint8_t gDash[8] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00};
    static const uint8_t gGt[8] = {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00};

    if (c >= '0' && c <= '9')
        return gDigits[c - '0'];
    switch (c) {
    case 'A': return gA;
    case 'F': return gF;
    case 'G': return gG;
    case 'H': return gH;
    case 'N': return gN;
    case 'O': return gO;
    case 'P': return gP;
    case 'R': return gR;
    case 'S': return gS;
    case 'L': return gL;
    case 'V': return gV;
    case 'T': return gT;
    case 'E': return gE;
    case 'U': return gU;
    case 'C': return gC;
    case 'B': return gB;
    case 'D': return gD;
    case 'I': return gI;
    case 'J': return gJ;
    case 'W': return gW;
    case '/': return gSl;
    case '.': return gDot;
    case ':': return gColon;
    case 'X': return gX;
    case '-': return gDash;
    case '>': return gGt;
    default:  return NULL; /* rendered as blank */
    }
}

/* Public alias so other UI modules can reuse the pixel font. */
const uint8_t *fsr_glyph(char c)
{
    return toast_glyph(c);
}

/* Compose text (up to 6 lines, '\n'-separated) into a pixel-font texture.
 * Returns NULL on failure. */
#define TOAST_MAX_LINES 6
#define TOAST_LINE_H    10 /* 8 px glyphs + 2 px spacing */

static SDL_Texture *toast_render(SDL_Renderer *renderer, const char *text,
                                 int *out_w, int *out_h)
{
    static uint32_t px[(TOAST_MAX_CHARS * 8 + 2 * TOAST_PAD) *
                       (TOAST_MAX_LINES * TOAST_LINE_H + 2 * TOAST_PAD)];
    const char *lines[TOAST_MAX_LINES];
    int lens[TOAST_MAX_LINES];
    SDL_Texture *tex;
    int nlines = 0, maxlen = 0;
    int w, h;

    for (const char *p = text; nlines < TOAST_MAX_LINES; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        if (len > TOAST_MAX_CHARS)
            len = TOAST_MAX_CHARS;
        lines[nlines] = p;
        lens[nlines]  = len;
        if (len > maxlen)
            maxlen = len;
        nlines++;
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!nlines || !maxlen)
        return NULL;

    w = maxlen * 8 + 2 * TOAST_PAD;
    h = nlines * TOAST_LINE_H - 2 + 2 * TOAST_PAD;

    for (int i = 0; i < w * h; i++)
        px[i] = 0xC0101010; /* translucent dark background */
    for (int li = 0; li < nlines; li++) {
        int y0 = TOAST_PAD + li * TOAST_LINE_H;

        for (int ci = 0; ci < lens[li]; ci++) {
            const uint8_t *g = toast_glyph(lines[li][ci]);

            if (!g)
                continue;
            for (int row = 0; row < 8; row++)
                for (int col = 0; col < 8; col++)
                    if (g[row] & (0x80 >> col))
                        px[(y0 + row) * w + TOAST_PAD + ci * 8 + col] = 0xFFFFFFFF;
        }
    }

    tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex)
        return NULL;
    SDL_UpdateTexture(tex, NULL, px, w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    *out_w = w;
    *out_h = h;
    return tex;
}

void fsr_toast_show(SDL_Renderer *renderer, const char *text)
{
    int ow = 0, oh = 0;

    if (toast_tex) {
        SDL_DestroyTexture(toast_tex);
        toast_tex = NULL;
    }
    if (!text || !text[0])
        return;
    /* System font first, for the same reason as the HUD: the pixel font is
     * missing half the alphabet and drops those letters silently. The size
     * matches what the old 8px font scaled up to (8 * oh/180). */
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    toast_sys = 0;
    if (oh > 0) {
        int px = oh / 22;

        if (px < 16)
            px = 16;
        toast_tex = ui_render_text(renderer, text, px, &toast_w, &toast_h);
        toast_sys = toast_tex != NULL;
    }
    if (!toast_tex)                     /* GDI unavailable: pixel font */
        toast_tex = toast_render(renderer, text, &toast_w, &toast_h);
    if (toast_tex)
        toast_until = av_gettime_relative() + TOAST_DURATION_US;
}

/* Persistent HUD (FPS display), top-right corner. Rendered with the system
 * font (as the subtitles are), falling back to the pixel font. */
static SDL_Texture *hud_tex;
static int          hud_w, hud_h;
static int          hud_sys;    /* the texture came from the system font */

void fsr_hud_set(SDL_Renderer *renderer, const char *text)
{
    int ow = 0, oh = 0;

    if (hud_tex) {
        SDL_DestroyTexture(hud_tex);
        hud_tex = NULL;
    }
    if (!text || !text[0])
        return;
    /* Prefer the system font the subtitles use: the pixel font only carries
     * about half the alphabet, so anything else silently loses letters.
     * Sized against the output height so it matches the old apparent size. */
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    hud_sys = 0;
    if (oh > 0) {
        int px = oh / 45;

        if (px < 12)
            px = 12;
        hud_tex = ui_render_text(renderer, text, px, &hud_w, &hud_h);
        hud_sys = hud_tex != NULL;
    }
    if (!hud_tex)                       /* GDI unavailable: pixel font */
        hud_tex = toast_render(renderer, text, &hud_w, &hud_h);
}

int fsr_hud_draw(SDL_Renderer *renderer)
{
    int ow = 0, oh = 0, scale;
    SDL_Rect dst;

    if (!hud_tex)
        return 0;
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    /* The system font is already rasterized at the right size; only the 8px
     * pixel-font fallback needs scaling up. */
    scale = hud_sys ? 1 : oh / 300;
    if (scale < 1)
        scale = 1;
    dst.w = hud_w * scale;
    dst.h = hud_h * scale;
    dst.x = ow - dst.w - 16;
    /* Sit below the UI title bar (34px tall, window buttons on the right) so
     * the TAB status readout does not overlap it. */
    dst.y = 50;

    /* Dark translucent plate behind the text: white glyphs alone wash out
     * over bright video. Drawn first so the text sits on top of it. */
    {
        int pad = scale * 6;
        SDL_Rect bg = { dst.x - pad, dst.y - pad,
                        dst.w + 2 * pad, dst.h + 2 * pad };
        SDL_BlendMode prev_bm;
        uint8_t r, g, b, a;

        SDL_GetRenderDrawBlendMode(renderer, &prev_bm);
        SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
        SDL_RenderFillRect(renderer, &bg);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);
        SDL_SetRenderDrawBlendMode(renderer, prev_bm);
    }

    SDL_RenderCopy(renderer, hud_tex, NULL, &dst);
    return 1;
}

/* Second persistent HUD, top-LEFT corner (used for the lada status line). */
static SDL_Texture *hud_left_tex;
static int          hud_left_w, hud_left_h;

void fsr_hud_left_set(SDL_Renderer *renderer, const char *text)
{
    if (hud_left_tex) {
        SDL_DestroyTexture(hud_left_tex);
        hud_left_tex = NULL;
    }
    if (text && text[0])
        hud_left_tex = toast_render(renderer, text, &hud_left_w, &hud_left_h);
}

int fsr_hud_left_draw(SDL_Renderer *renderer)
{
    int ow = 0, oh = 0, scale;
    SDL_Rect dst;

    if (!hud_left_tex)
        return 0;
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    scale = oh / 300;
    if (scale < 1)
        scale = 1;
    dst.w = hud_left_w * scale;
    dst.h = hud_left_h * scale;
    dst.x = 16;
    dst.y = 50;   /* below the UI title bar (34px) so the status doesn't overlap it */
    SDL_RenderCopy(renderer, hud_left_tex, NULL, &dst);
    return 1;
}

int fsr_toast_active(void)
{
    return toast_tex && av_gettime_relative() < toast_until;
}

int fsr_toast_draw(SDL_Renderer *renderer)
{
    int64_t rem = toast_until - av_gettime_relative();
    int ow = 0, oh = 0, scale, alpha;
    SDL_Rect dst;

    if (!toast_tex || rem <= 0)
        return 0;
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    /* Only the 8px pixel-font fallback needs scaling up; the system font is
     * already rasterized at the intended size. */
    scale = toast_sys ? 1 : oh / 180;
    if (scale < (toast_sys ? 1 : 2))
        scale = toast_sys ? 1 : 2;
    alpha = rem < TOAST_FADE_US ? (int)(255 * rem / TOAST_FADE_US) : 255;
    SDL_SetTextureAlphaMod(toast_tex, alpha);
    dst.x = 16;
    dst.y = 16;
    dst.w = toast_w * scale;
    dst.h = toast_h * scale;
    SDL_RenderCopy(renderer, toast_tex, NULL, &dst);
    return 1;
}

/* ------------------------------------------------------------------------
 * Album-art "now playing" visualizer (audio-only playback)
 *
 * A port of WinVibe's LPPlayerView: an animated rainbow-blob background with
 * the cover art laid flat on the left and a vinyl LP spinning out from behind
 * it. Everything is drawn with the plain SDL_Renderer (no GL), so it works on
 * any backend, mirroring the "degrade gracefully" rule for the audio path.
 * --------------------------------------------------------------------- */

#define ALB_BLOB_COUNT 6
#define ALB_BLOB_ALPHA 0xA0     /* blob centre alpha (edges fade to clear)   */
#define ALB_BLOB_PX    256      /* radial-gradient sprite resolution         */
#define ALB_LP_PX      1024     /* built vinyl-disc texture resolution       */

static SDL_Texture *alb_blob;   /* soft radial gradient (white -> clear)     */
static SDL_Texture *alb_shine;  /* fixed specular gleam over the spinning LP  */
static SDL_Texture *alb_lp;     /* vinyl disc built from the cover           */
static SDL_Texture *alb_cover;  /* raw cover, drawn flat on the left         */
static int          alb_have_cover;
static int          alb_default_built;  /* generated placeholder cover in use */
static float        alb_phase_x[ALB_BLOB_COUNT];
static float        alb_phase_y[ALB_BLOB_COUNT];
static float        alb_phase_r[ALB_BLOB_COUNT];
static float        alb_base_hue[ALB_BLOB_COUNT];
static int          alb_phase_init;
static float        alb_angle;      /* LP rotation, degrees                  */
static uint32_t     alb_last_ticks;
static uint32_t     alb_start_ticks;

static void alb_hsv(float h, float s, float v,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c, rr = 0, gg = 0, bb = 0;

    if      (h <  60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else              { rr = c; bb = x; }
    *r = (uint8_t)((rr + m) * 255.0f + 0.5f);
    *g = (uint8_t)((gg + m) * 255.0f + 0.5f);
    *b = (uint8_t)((bb + m) * 255.0f + 0.5f);
}

/* Build the soft radial-gradient blob sprite once (reused for the rainbow
 * background and, tinted black, for drop shadows). */
static void alb_build_blob(SDL_Renderer *renderer)
{
    static uint32_t px[ALB_BLOB_PX * ALB_BLOB_PX];
    const int N = ALB_BLOB_PX;

    if (alb_blob)
        return;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float dx = (x - N / 2.0f) / (N / 2.0f);
            float dy = (y - N / 2.0f) / (N / 2.0f);
            float d  = sqrtf(dx * dx + dy * dy);
            float a  = 1.0f - d;
            uint8_t A;

            if (a < 0.0f) a = 0.0f;
            a = a * a;                          /* softer falloff */
            A = (uint8_t)(a * 255.0f + 0.5f);
            px[y * N + x] = ((uint32_t)A << 24) | 0x00FFFFFFu;
        }
    alb_blob = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STATIC, N, N);
    if (alb_blob) {
        SDL_UpdateTexture(alb_blob, NULL, px, N * 4);
        SDL_SetTextureBlendMode(alb_blob, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(alb_blob, SDL_ScaleModeLinear);
    }
}

/* Build the fixed specular gleam laid over the spinning disc: a circular,
 * additive white streak (a broad diagonal band + bright core, plus a fainter
 * crossing band) that fades over the centre label and at the rim. It is drawn
 * WITHOUT rotation, so as the record spins the reflection stays put and the
 * grooves catch the light — the realistic look. Built once, cover-independent. */
static void alb_build_shine(SDL_Renderer *renderer)
{
    static uint32_t px[ALB_LP_PX * ALB_LP_PX];
    const int S = ALB_LP_PX;
    float c = S / 2.0f, R = S / 2.0f - 1.0f;
    float labelR = S * 0.19f;

    if (alb_shine)
        return;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float dx = x - c, dy = y - c;
            float d  = sqrtf(dx * dx + dy * dy);
            uint32_t out = 0x00000000u;

            if (d <= R) {
                float u = dx / R, v = dy / R;
                /* Broad soft specular sweeps, like light reflecting off glossy
                 * vinyl: a wide main band along one diagonal, a narrower and
                 * brighter crossing streak, and a faint wide halo. Kept broad
                 * and bright so the near-black platter reads as a shiny surface
                 * and the fine grooves shimmer beneath it as the disc turns. */
                float a1 = (u + v) * 0.70710678f;   /* signed dist to a diagonal */
                float a2 = (u - v) * 0.70710678f;   /* to the crossing diagonal  */
                float inten =
                    0.85f * expf(-(a1 * a1) / (2.0f * 0.20f * 0.20f)) +
                    0.55f * expf(-(a2 * a2) / (2.0f * 0.09f * 0.09f)) +
                    0.20f * expf(-(a1 * a1) / (2.0f * 0.50f * 0.50f));
                uint8_t A;

                if (d < labelR)   inten *= 0.30f;          /* softer on label */
                if (d > R - 2.0f) inten *= (R - d) / 2.0f;  /* fade rim edge  */
                if (inten < 0.0f) inten = 0.0f;
                if (inten > 1.0f) inten = 1.0f;
                A = (uint8_t)(inten * 255.0f);
                out = ((uint32_t)A << 24) | 0x00FFFFFFu;
            }
            px[y * S + x] = out;
        }
    alb_shine = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STATIC, S, S);
    if (alb_shine) {
        SDL_UpdateTexture(alb_shine, NULL, px, S * 4);
        SDL_SetTextureBlendMode(alb_shine, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(alb_shine, SDL_ScaleModeLinear);
    }
}

/* Build the vinyl disc: a black platter with fine concentric grooves, a faint
 * crossed shine, the cover cropped into a circular centre label, and a punched
 * spindle hole. cover may be NULL for a blank record. */
static void alb_build_lp(SDL_Renderer *renderer,
                         const uint8_t *cover, int cw, int ch)
{
    static uint32_t px[ALB_LP_PX * ALB_LP_PX];
    const int S = ALB_LP_PX;
    float c = S / 2.0f, R = S / 2.0f - 1.0f;
    int labelR = (int)(S * 0.19f);
    int holeR  = (int)(S * 0.02f);
    int cmin = cover ? (cw < ch ? cw : ch) : 0;
    int cx0  = cover ? (cw - cmin) / 2 : 0;
    int cy0  = cover ? (ch - cmin) / 2 : 0;

    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float dx = x - c, dy = y - c;
            float d  = sqrtf(dx * dx + dy * dy);
            uint32_t out;

            if (d > R + 0.5f || d < holeR) {
                out = 0x00000000u;                  /* outside / spindle hole */
            } else if (cover && d < labelR - 1.0f) {
                float u  = (x - (c - labelR)) / (2.0f * labelR);
                float v  = (y - (c - labelR)) / (2.0f * labelR);
                int   sx = cx0 + (int)(u * cmin);
                int   sy = cy0 + (int)(v * cmin);
                const uint8_t *p;

                if (sx < 0)   sx = 0;
                if (sy < 0)   sy = 0;
                if (sx >= cw) sx = cw - 1;
                if (sy >= ch) sy = ch - 1;
                p = cover + ((size_t)sy * cw + sx) * 4;   /* BGRA */
                out = 0xFF000000u | ((uint32_t)p[2] << 16) |
                      ((uint32_t)p[1] << 8) | p[0];
            } else if (d < labelR - 1.0f) {
                out = 0xFFC01530u;                  /* blank label: crimson red */
            } else {
                /* Deep glossy black. The vinyl is nearly pure black; the grooves
                 * are fine, low-contrast concentric lines and the realism comes
                 * from the broad specular reflection laid over it (alb_shine),
                 * not from high groove contrast. A soft, rotation-symmetric
                 * radial sheen gives the surface a smooth glossy body. */
                float warp   = 1.5f * sinf(d * 0.012f);         /* slight drift */

                /* Grooved annulus from just outside the label to a thin outer
                 * rim, split into exactly 5 dark-gray track bands separated by
                 * thin black rings, like the reference. A small lead-in gap and
                 * a thin rim stay black. */
                float rInner = labelR + S * 0.020f;   /* first track starts here */
                float rOuter = R - S * 0.020f;        /* thin outer rim */
                float land   = 0.0f;                  /* black by default */

                if (d >= rInner && d <= rOuter) {
                    /* 5 tracks of unequal width (25/25/15/25/10%): the thin
                     * black separators sit at these cumulative fractions. */
                    static const float sepAt[4] = { 0.25f, 0.50f, 0.65f, 0.90f };
                    float rn    = (d - rInner) / (rOuter - rInner);  /* 0..1 */
                    float fine  = 0.5f + 0.5f * cosf(d * 0.55f + warp); /* grooves */
                    float track = 8.0f + fine * 3.0f;                /* darker gray */
                    float sep   = 0.0f;

                    for (int k = 0; k < 4; k++) {
                        float dd = fabsf(rn - sepAt[k]);
                        if (dd < 0.018f) {           /* thin black separator ring */
                            float s = 1.0f - dd / 0.018f;
                            if (s > sep) sep = s;
                        }
                    }
                    land = track * (1.0f - sep);   /* gray track, 0 at separator */
                }

                /* Soft radial tone so the black body looks shaded, not flat. */
                float rr     = (d - labelR) / (R - labelR);     /* 0..1 */
                if (rr < 0.0f) rr = 0.0f;
                if (rr > 1.0f) rr = 1.0f;
                float radial = 3.0f * (0.5f + 0.5f * cosf(rr * 12.566371f));

                int   base = 6                       /* deep black floor/separator */
                           + (int)land               /* dark-gray track band */
                           + (int)radial;            /* smooth radial sheen 0..3 */
                uint8_t A = 255;

                if (d < labelR + 1.5f)   base += 10;   /* faint label lip */
                if (base < 0)   base = 0;
                if (base > 255) base = 255;
                if (d > R - 1.0f) {                        /* AA outer edge */
                    float f = R + 0.5f - d;
                    A = (uint8_t)((f < 0 ? 0 : f > 1 ? 1 : f) * 255.0f);
                }
                out = ((uint32_t)A << 24) | ((uint32_t)base << 16) |
                      ((uint32_t)base << 8) | base;
            }
            px[y * S + x] = out;
        }

    if (alb_lp) { SDL_DestroyTexture(alb_lp); alb_lp = NULL; }
    alb_lp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STATIC, S, S);
    if (alb_lp) {
        SDL_UpdateTexture(alb_lp, NULL, px, S * 4);
        SDL_SetTextureBlendMode(alb_lp, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(alb_lp, SDL_ScaleModeLinear);
    }
}

/* Alpha-blend an RGB colour into a BGRA pixel. */
static void alb_blend_px(uint8_t *p, uint8_t r, uint8_t g, uint8_t b, float a)
{
    if (a <= 0.0f) return;
    if (a > 1.0f)  a = 1.0f;
    p[0] = (uint8_t)(p[0] * (1.0f - a) + b * a);
    p[1] = (uint8_t)(p[1] * (1.0f - a) + g * a);
    p[2] = (uint8_t)(p[2] * (1.0f - a) + r * a);
    p[3] = 255;
}

static void alb_fill_rect(uint8_t *bgra, int S, int x0, int y0, int x1, int y1,
                          uint8_t r, uint8_t g, uint8_t b, float a)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > S) x1 = S; if (y1 > S) y1 = S;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            alb_blend_px(bgra + ((size_t)y * S + x) * 4, r, g, b, a);
}

/* Filled, tilted ellipse with a 1px soft edge (for the note heads). */
static void alb_fill_ellipse(uint8_t *bgra, int S, float cx, float cy,
                             float rx, float ry, float ang,
                             uint8_t r, uint8_t g, uint8_t b)
{
    float ca = cosf(ang), sa = sinf(ang);
    int   rad = (int)(rx + ry + 2);
    int   x0 = (int)(cx - rad), x1 = (int)(cx + rad);
    int   y0 = (int)(cy - rad), y1 = (int)(cy + rad);

    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > S) x1 = S; if (y1 > S) y1 = S;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x - cx, dy = y - cy;
            float u  =  dx * ca + dy * sa;
            float v  = -dx * sa + dy * ca;
            float e  = (u * u) / (rx * rx) + (v * v) / (ry * ry);
            float a;

            if (e > 1.0f) continue;
            a = e > 0.85f ? 1.0f - (e - 0.85f) / 0.15f : 1.0f;
            alb_blend_px(bgra + ((size_t)y * S + x) * 4, r, g, b, a);
        }
}

/* Build a placeholder cover for music with no embedded art: a diagonal
 * purple->blue gradient with a soft vignette and a centred beamed-eighth-note
 * (♫) in off-white. Written as BGRA into a caller S*S*4 buffer. */
static void alb_make_default_cover(uint8_t *bgra, int S)
{
    const uint8_t nr = 235, ng = 236, nb = 248;   /* note colour */
    float rx = 0.085f * S, ry = 0.062f * S, tilt = -0.32f;
    float hlx = 0.36f * S, hrx = 0.60f * S, hy = 0.64f * S;
    int   stemW  = (int)(0.024f * S);
    int   stemTop = (int)(0.30f * S);
    int   slx = (int)(hlx + rx * 0.88f), srx = (int)(hrx + rx * 0.88f);

    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float u = (float)x / S, v = (float)y / S;
            float t = (u + v) * 0.5f;
            float dx = u - 0.5f, dy = v - 0.5f;
            float vig = 1.0f - 0.55f * sqrtf(dx * dx + dy * dy);
            uint8_t *p = bgra + ((size_t)y * S + x) * 4;

            if (vig < 0.0f) vig = 0.0f;
            p[2] = (uint8_t)((58 * (1 - t) + 18 * t) * vig);   /* R */
            p[1] = (uint8_t)((30 * (1 - t) + 74 * t) * vig);   /* G */
            p[0] = (uint8_t)((90 * (1 - t) + 122 * t) * vig);  /* B */
            p[3] = 255;
        }

    /* stems (up from each head's right edge) + top beam connecting them */
    alb_fill_rect(bgra, S, slx, stemTop, slx + stemW, (int)hy, nr, ng, nb, 0.95f);
    alb_fill_rect(bgra, S, srx, stemTop, srx + stemW, (int)hy, nr, ng, nb, 0.95f);
    alb_fill_rect(bgra, S, slx, stemTop, srx + stemW, stemTop + (int)(0.055f * S),
                  nr, ng, nb, 0.95f);
    /* note heads */
    alb_fill_ellipse(bgra, S, hlx, hy, rx, ry, tilt, nr, ng, nb);
    alb_fill_ellipse(bgra, S, hrx, hy, rx, ry, tilt, nr, ng, nb);
}

void fsr_album_set_cover(SDL_Renderer *renderer,
                         const uint8_t *bgra, int w, int h)
{
    if (alb_cover) { SDL_DestroyTexture(alb_cover); alb_cover = NULL; }
    alb_have_cover = 0;

    if (bgra && w > 0 && h > 0) {
        /* SDL_PIXELFORMAT_ARGB8888 is byte order B,G,R,A on little-endian,
         * which is exactly AV_PIX_FMT_BGRA, so the buffer uploads as-is. */
        alb_cover = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STATIC, w, h);
        if (alb_cover) {
            SDL_UpdateTexture(alb_cover, NULL, bgra, w * 4);
            SDL_SetTextureBlendMode(alb_cover, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(alb_cover, SDL_ScaleModeLinear);
            alb_have_cover = 1;
        }
    }
    alb_build_lp(renderer, bgra, w, h);
}

void fsr_album_ensure_default(SDL_Renderer *renderer)
{
    static uint8_t defc[1024 * 1024 * 4];

    if (alb_have_cover)
        return;                     /* a real (or already-built) cover exists */
    alb_make_default_cover(defc, 1024);
    fsr_album_set_cover(renderer, defc, 1024, 1024);
    alb_default_built = 1;
}

void fsr_album_reset(void)
{
    if (alb_cover) { SDL_DestroyTexture(alb_cover); alb_cover = NULL; }
    if (alb_lp)    { SDL_DestroyTexture(alb_lp);    alb_lp    = NULL; }
    alb_have_cover    = 0;
    alb_default_built = 0;
    /* Keep the blob sprite, phases and rotation angle across files. */
}

/* Bottom margin below the FFT-visualizer band, as a fraction of the output
 * height. Raised well above the panel edge so the band clears the text-subtitle
 * area (subtitles sit at ~win_h - win_h/12), which would otherwise overlap. */
#define VIS_BAND_BOTTOM_FRAC 0.12f

/* Height of the bottom FFT-visualizer band for a given output height. Shared
 * by the album view (to sit clear above it) and fsr_vis_draw (which draws it).
 * The band occupies this height plus the VIS_BAND_BOTTOM_FRAC bottom margin. */
static float vis_band_h(int oh)
{
    float h = oh * 0.20f;
    return h < 80.0f ? 80.0f : h;
}

void fsr_album_draw(SDL_Renderer *renderer, int playing)
{
    const float twoPi = 6.2831853f;
    int   ow = 0, oh = 0;
    uint32_t now;
    float t, minside;

    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ow <= 0 || oh <= 0)
        return;

    alb_build_blob(renderer);
    alb_build_shine(renderer);
    if (!alb_lp)                              /* no cover set yet: blank disc */
        alb_build_lp(renderer, NULL, 0, 0);
    if (!alb_blob || !alb_lp)
        return;

    now = SDL_GetTicks();
    if (!alb_phase_init) {
        uint32_t s = now ^ 0x9E3779B9u;       /* small LCG for blob phases */
        for (int i = 0; i < ALB_BLOB_COUNT; i++) {
            s = s * 1664525u + 1013904223u; alb_phase_x[i] = (s >> 8) / 16777216.0f * twoPi;
            s = s * 1664525u + 1013904223u; alb_phase_y[i] = (s >> 8) / 16777216.0f * twoPi;
            s = s * 1664525u + 1013904223u; alb_phase_r[i] = (s >> 8) / 16777216.0f * twoPi;
            alb_base_hue[i] = fmodf(i * (360.0f / ALB_BLOB_COUNT), 360.0f);
        }
        alb_phase_init  = 1;
        alb_start_ticks = now;
        alb_last_ticks  = now;
    }

    /* Spin only while playing (~18 deg/s, matching the reference view). */
    if (playing)
        alb_angle = fmodf(alb_angle + (now - alb_last_ticks) * 0.018f, 360.0f);
    alb_last_ticks = now;

    t       = (now - alb_start_ticks) / 1000.0f;
    minside = (float)(ow < oh ? ow : oh);

    /* 1. black base + drifting rainbow blobs */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(renderer, NULL);
    for (int i = 0; i < ALB_BLOB_COUNT; i++) {
        float cx    = ow * (0.5f + 0.45f * sinf(t * twoPi * 0.05f + alb_phase_x[i]));
        float cy    = oh * (0.5f + 0.45f * cosf(t * twoPi * 0.07f + alb_phase_y[i]));
        float pulse = 0.5f + 0.5f * sinf(t * twoPi * 0.04f + alb_phase_r[i]);
        float rad   = minside * (0.55f + 0.55f * pulse);
        float hue   = fmodf(alb_base_hue[i] + t * 8.0f, 360.0f);
        uint8_t r, g, b;
        SDL_Rect dst;

        if (rad < 1.0f) rad = 1.0f;
        alb_hsv(hue, 0.75f, 1.0f, &r, &g, &b);
        SDL_SetTextureColorMod(alb_blob, r, g, b);
        SDL_SetTextureAlphaMod(alb_blob, ALB_BLOB_ALPHA);
        dst.x = (int)(cx - rad);
        dst.y = (int)(cy - rad);
        dst.w = dst.h = (int)(rad * 2.0f);
        SDL_RenderCopy(renderer, alb_blob, NULL, &dst);
    }

    /* 2. layout: cover flat on the left, LP spinning out to its right.
     * Vertically centre the pair in the room ABOVE the visualizer band so the
     * two never overlap. */
    {
        float avail     = oh - (oh * VIS_BAND_BOTTOM_FRAC + vis_band_h(oh));
        float albumSize = minside * 0.55f;
        float lpSize, exposed, totalW, startX, startY;

        if (avail < 1.0f)               avail = oh;      /* degenerate guard */
        if (albumSize > avail * 0.90f)  albumSize = avail * 0.90f;
        lpSize  = albumSize * 0.90f;
        exposed = lpSize * 0.55f;
        totalW  = albumSize + (exposed - (albumSize - lpSize) / 2.0f);
        startX  = (ow - totalW) / 2.0f;
        /* Bias below centre (0.80 of the slack, vs 0.5) so the cover + record
         * sit lower in the room above the visualizer band. */
        startY  = (avail - albumSize) * 0.80f;
        float wob       = sinf(alb_angle * 0.01745329f) * (albumSize * 0.012f);
        float lpcx      = startX + albumSize + lpSize * 0.05f;
        float lpcy      = startY + albumSize / 2.0f + wob;
        SDL_Rect dst, sh;
        SDL_Point ctr;

        /* Ground contact shadow: soft, flat ellipses under the whole assembly
         * so the cover + record read as standing on a surface. Drawn first,
         * their upper half is covered by the art -> a grounded shadow, not a
         * floating blob. Two passes (a wide soft base plus a darker, tighter
         * core) make it read clearly against the coloured background. */
        {
            float gcx = startX + totalW / 2.0f;
            float gcy = startY + albumSize;        /* cover's bottom edge */
            /* pass 0: wide soft base, pass 1: dark tight core */
            const float gw[2] = { totalW * 1.06f, totalW * 0.72f };
            const float gh[2] = { albumSize * 0.19f, albumSize * 0.12f };
            const int   ga[2] = { 130, 165 };

            SDL_SetTextureColorMod(alb_blob, 0, 0, 0);
            for (int s = 0; s < 2; s++) {
                SDL_SetTextureAlphaMod(alb_blob, ga[s]);
                sh.w = (int)gw[s];
                sh.h = (int)gh[s];
                sh.x = (int)(gcx - gw[s] / 2.0f);
                sh.y = (int)(gcy - gh[s] / 2.0f);
                SDL_RenderCopy(renderer, alb_blob, NULL, &sh);
            }
        }

        /* LP drop shadow (blob tinted black, offset down-right) */
        SDL_SetTextureColorMod(alb_blob, 0, 0, 0);
        SDL_SetTextureAlphaMod(alb_blob, 120);
        sh.w = sh.h = (int)(lpSize * 1.06f);
        sh.x = (int)(lpcx - sh.w / 2.0f + lpSize * 0.03f);
        sh.y = (int)(lpcy - sh.h / 2.0f + lpSize * 0.04f);
        SDL_RenderCopy(renderer, alb_blob, NULL, &sh);

        /* the disc, rotating about its centre */
        dst.w = dst.h = (int)lpSize;
        dst.x = (int)(lpcx - lpSize / 2.0f);
        dst.y = (int)(lpcy - lpSize / 2.0f);
        ctr.x = ctr.y = (int)(lpSize / 2.0f);
        SDL_RenderCopyEx(renderer, alb_lp, NULL, &dst, alb_angle, &ctr,
                         SDL_FLIP_NONE);

        /* fixed specular gleam on top (same rect, NOT rotated) */
        if (alb_shine) {
            SDL_SetTextureColorMod(alb_shine, 235, 242, 255);
            SDL_SetTextureAlphaMod(alb_shine, 45);
            SDL_RenderCopy(renderer, alb_shine, NULL, &dst);
        }

        /* cover laid flat on the left (soft shadow behind) */
        if (alb_have_cover && alb_cover) {
            float ccx = startX + albumSize / 2.0f;
            float ccy = startY + albumSize / 2.0f;

            SDL_SetTextureColorMod(alb_blob, 0, 0, 0);
            SDL_SetTextureAlphaMod(alb_blob, 150);
            sh.w = sh.h = (int)(albumSize * 1.10f);
            sh.x = (int)(ccx - sh.w / 2.0f + albumSize * 0.03f);
            sh.y = (int)(ccy - sh.h / 2.0f + albumSize * 0.04f);
            SDL_RenderCopy(renderer, alb_blob, NULL, &sh);

            dst.x = (int)startX;
            dst.y = (int)startY;
            dst.w = dst.h = (int)albumSize;
            SDL_RenderCopy(renderer, alb_cover, NULL, &dst);
        }
    }

    /* leave the blob sprite un-modulated for any later reuse */
    SDL_SetTextureColorMod(alb_blob, 255, 255, 255);
    SDL_SetTextureAlphaMod(alb_blob, 255);
}

/* ------------------------------------------------------------------------
 * Bottom FFT-spectrum bar visualizer (audio-only playback)
 *
 * A port of WinVibe's LineBarVisualizer2: a horizontally-mirrored bar
 * spectrum (bass at the centre, treble at the edges) with a time-cycled
 * rainbow palette and a translucent triangle-wave centre line, drawn as a
 * band across the bottom of the window over the album view. The caller feeds
 * the newest mono samples each frame; the FFT, response smoothing and drawing
 * all happen here.
 * --------------------------------------------------------------------- */

#define VIS_BARS          96        /* total bars (mirrored -> 48 per side) */
#define VIS_FFT_LEN       2048      /* FFT window size (power of two)        */
#define VIS_BAR_ALPHA     0xB0
#define VIS_LINE_ALPHA    0x50
#define VIS_INPUT_SMOOTH  0.20f     /* IIR smoothing on the FFT targets      */
#define VIS_ATTACK        0.30f     /* rise speed (staged, centre-out)       */
#define VIS_RELEASE       0.45f     /* fall speed                            */
#define VIS_CASCADE       0.10f     /* centre-out rise delay per band        */
#define VIS_IDLE_DECAY    0.85f     /* decay toward 0 while paused/stopped   */
#define VIS_DB_MIN       (-58.0f)   /* magnitude (dBFS) mapped to bar 0.0    */
#define VIS_DB_MAX       (-8.0f)    /* magnitude (dBFS) mapped to bar 1.0    */
#define VIS_COLOR_SPEED   28.0f     /* palette indices/sec (~9s per cycle)   */

static AVTXContext    *vis_tx;
static av_tx_fn        vis_tx_fn;
static float          *vis_win;     /* Hann window, VIS_FFT_LEN              */
static float          *vis_tin;     /* windowed input, VIS_FFT_LEN          */
static AVComplexFloat *vis_tout;    /* spectrum, VIS_FFT_LEN/2 + 1          */
static float           vis_target[VIS_BARS];
static float           vis_disp[VIS_BARS];
static float           vis_rise[VIS_BARS];
static uint32_t        vis_last_ticks;  /* for frame-rate-independent smoothing */
static float           vis_accum;        /* elapsed-time accumulator for substeps */
static uint32_t        vis_color_start;

/* Mirror map: visual position p -> frequency-band index (0 = bass at the two
 * centre bars, half-1 = treble at both edges). Keeps both sides symmetric. */
static int vis_freq_idx(int p)
{
    int half = VIS_BARS / 2;
    return (p < half) ? (half - 1 - p) : (p - half);
}

static int vis_fft_init(void)
{
    float scale = 1.0f;

    if (vis_tx)
        return 0;
    vis_win  = av_malloc_array(VIS_FFT_LEN, sizeof(*vis_win));
    vis_tin  = av_malloc_array(VIS_FFT_LEN, sizeof(*vis_tin));
    vis_tout = av_malloc_array(VIS_FFT_LEN / 2 + 1, sizeof(*vis_tout));
    if (!vis_win || !vis_tin || !vis_tout)
        return -1;
    for (int i = 0; i < VIS_FFT_LEN; i++)   /* Hann window */
        vis_win[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (VIS_FFT_LEN - 1));
    if (av_tx_init(&vis_tx, &vis_tx_fn, AV_TX_FLOAT_RDFT,
                   0, VIS_FFT_LEN, &scale, 0) < 0) {
        vis_tx = NULL;
        return -1;
    }
    return 0;
}

void fsr_vis_reset(void)
{
    for (int i = 0; i < VIS_BARS; i++)
        vis_target[i] = vis_disp[i] = vis_rise[i] = 0.0f;
}

/* Fill a vertical "capsule": a bar of width w whose top and bottom ends are
 * rounded into semicircles of radius w/2, so the spectrum bars read as pills
 * instead of hard rectangles. The current render draw color is used. The two
 * rounded caps are painted as stacked 1px horizontal scanlines whose width
 * follows the circle chord, meeting the straight middle section seamlessly. */
static void vis_fill_capsule(SDL_Renderer *renderer, float x, float y,
                             float w, float h)
{
    float r  = w * 0.5f;
    float cx = x + r;
    int   ri, dy;
    SDL_FRect rc;

    if (r > h * 0.5f) r = h * 0.5f;         /* short bar: bound cap by height */

    /* straight middle section between the two caps */
    rc.x = x;  rc.y = y + r;  rc.w = w;  rc.h = h - 2.0f * r;
    if (rc.h > 0.0f)
        SDL_RenderFillRectF(renderer, &rc);

    ri = (int)ceilf(r);
    for (dy = 0; dy < ri; dy++) {
        float fy   = dy + 0.5f;
        float half = (fy < r) ? sqrtf(r * r - fy * fy) : 0.0f;
        if (half <= 0.0f) continue;
        rc.x = cx - half;
        rc.w = half * 2.0f;
        rc.h = 1.0f;
        rc.y = (y + r) - dy - 1.0f;         /* top cap, growing upward */
        SDL_RenderFillRectF(renderer, &rc);
        rc.y = (y + h - r) + dy;            /* bottom cap, growing downward */
        SDL_RenderFillRectF(renderer, &rc);
    }
}

void fsr_vis_draw(SDL_Renderer *renderer, const float *mono, int nsamp,
                  int playing)
{
    const int half   = VIS_BARS / 2;
    const int maxbin = VIS_FFT_LEN / 2;
    float raw[VIS_BARS / 2];
    int   ow = 0, oh = 0;
    uint32_t now;
    float t, bandH, midY, barW, offset, maxAmp;
    SDL_BlendMode prev_bm;

    if (nsamp < VIS_FFT_LEN || vis_fft_init() < 0)
        return;

    /* window + forward real FFT over the newest VIS_FFT_LEN samples */
    for (int i = 0; i < VIS_FFT_LEN; i++)
        vis_tin[i] = mono[nsamp - VIS_FFT_LEN + i] * vis_win[i];
    vis_tx_fn(vis_tx, vis_tout, vis_tin, sizeof(float));

    /* log-spaced frequency bands -> normalized magnitude (dBFS window).
     * A running cursor makes each band's bin range distinct and non-overlapping:
     * at low frequencies the log spacing falls below one bin, so without this
     * several centre bars would read the *same* bin and rise identically. The
     * cursor forces one distinct bin per band there (sequential), keeping the
     * bass bars differentiated, and stays log-spaced once bands span many bins. */
    int nextbin = 1;
    for (int b = 0; b < half; b++) {
        int   i0 = nextbin;
        int   i1 = (int)powf((float)maxbin, (float)(b + 1) / half);
        double pw = 0.0;
        int    cnt = 0;
        float  mag, db, lvl;

        if (i1 <= i0)     i1 = i0 + 1;      /* at least one distinct bin */
        if (i1 > maxbin)  i1 = maxbin;
        for (int k = i0; k < i1; k++) {
            float re = vis_tout[k].re, im = vis_tout[k].im;
            pw += (double)re * re + (double)im * im;
            cnt++;
        }
        nextbin = i1;                       /* next band starts where this ends */
        if (nextbin >= maxbin) nextbin = maxbin - 1;
        mag = cnt ? sqrtf((float)(pw / cnt)) * (2.0f / VIS_FFT_LEN) : 0.0f;
        db  = 20.0f * log10f(mag + 1e-6f);
        lvl = (db - VIS_DB_MIN) / (VIS_DB_MAX - VIS_DB_MIN);
        lvl *= 1.0f + 0.6f * ((float)b / (half - 1));   /* lift the treble */
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        raw[b] = lvl;
    }

    /* input smoothing + attack/release with a centre-out rise cascade. */
    now = SDL_GetTicks();
    if (!vis_color_start)
        vis_color_start = now;
    /* Advance the bars on a FIXED 60fps timestep decoupled from the draw rate:
     * accumulate the real time since the last call and run the original
     * per-frame smoothing once per ~16ms of it. This makes the motion identical
     * whether we are drawn at the album view's steady 60fps or at a video's
     * lower / irregular present rate (the FG slot pacing is uneven) - the state
     * evolves on its own steady clock, the draw just samples it. */
    {
        uint32_t d = vis_last_ticks ? now - vis_last_ticks : 16;
        vis_last_ticks = now;
        if (d > 200) d = 200;                 /* cap catch-up after a stall */
        vis_accum += (float)d;
    }
    for (int step = 0; vis_accum >= 16.0f && step < 8; step++) {
        vis_accum -= 16.0f;
        for (int p = 0; p < VIS_BARS; p++) {
            int   fi     = vis_freq_idx(p);
            float target = vis_target[p] * VIS_INPUT_SMOOTH +
                           raw[fi] * (1.0f - VIS_INPUT_SMOOTH);
            float delta;

            if (!playing)
                target *= VIS_IDLE_DECAY;
            vis_target[p] = target;
            delta = target - vis_disp[p];
            if (delta >= 0.0f) {
                if (vis_rise[p] < fi * VIS_CASCADE)
                    vis_rise[p] += VIS_ATTACK;          /* still delayed */
                else {
                    vis_rise[p] += VIS_ATTACK;
                    vis_disp[p] += delta * VIS_ATTACK;
                }
            } else {
                vis_disp[p] += delta * VIS_RELEASE;
                vis_rise[p]  = 0.0f;
            }
        }
    }

    /* geometry: a band across the bottom, bars mirrored about their midline */
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ow <= 0 || oh <= 0)
        return;
    bandH  = vis_band_h(oh);
    midY   = oh - oh * VIS_BAND_BOTTOM_FRAC - bandH * 0.5f;
    barW   = (float)ow / (VIS_BARS * 2.0f);
    offset = barW * 0.5f;
    maxAmp = bandH;
    t      = (now - vis_color_start) / 1000.0f;

    SDL_GetRenderDrawBlendMode(renderer, &prev_bm);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* centre triangle-wave line (behind the bars), rainbow per segment */
    {
        float pad   = barW * 0.5f;
        float startX = pad, endX = ow - pad;
        float xStep = (endX - startX) / (VIS_BARS - 1);
        float px = startX, py = midY;

        for (int i = 0; i < VIS_BARS; i++) {
            float x    = startX + i * xStep;
            float sign = (i % 2 == 0) ? -1.0f : 1.0f;
            float amp  = vis_disp[i] * maxAmp;
            float y, hue;
            uint8_t r, g, b;

            if (amp < barW) amp = barW;
            y   = midY + sign * (amp * 0.5f) * 0.5f;
            hue = fmodf((float)i / VIS_BARS * 360.0f + t * VIS_COLOR_SPEED * 1.4f,
                        360.0f);
            alb_hsv(hue, 0.85f, 1.0f, &r, &g, &b);
            SDL_SetRenderDrawColor(renderer, r, g, b, VIS_LINE_ALPHA);
            /* 2px-thick segment */
            for (int o = 0; o <= 1; o++) {
                SDL_RenderDrawLineF(renderer, px, py + o, x, y + o);
                SDL_RenderDrawLineF(renderer, px + o, py, x + o, y);
            }
            px = x; py = y;
        }
    }

    /* the mirrored spectrum bars, time-cycled rainbow */
    for (int i = 0; i < VIS_BARS; i++) {
        float amp = vis_disp[i] * maxAmp;
        float hue = fmodf((float)i / VIS_BARS * 360.0f + t * VIS_COLOR_SPEED,
                          360.0f);
        uint8_t r, g, b;

        if (amp < barW) amp = barW;                     /* min visible nub */
        alb_hsv(hue, 0.85f, 1.0f, &r, &g, &b);
        SDL_SetRenderDrawColor(renderer, r, g, b, VIS_BAR_ALPHA);
        vis_fill_capsule(renderer, barW * i * 2.0f + offset,
                         midY - amp * 0.5f, barW, amp);
    }

    SDL_SetRenderDrawBlendMode(renderer, prev_bm);
}

/* ------------------------------------------------------------------------
 * Single-instance support (Windows)
 *
 * A named mutex marks the first ("primary") player. Later launches — e.g.
 * double-clicking a media file associated with ffplay — detect the mutex,
 * hand their file path to the primary over WM_COPYDATA and exit, so the file
 * opens in the already-running player instead of a second window. The primary
 * hosts a hidden message-only window (found by class name) whose WndProc
 * receives the paths; SDL's message pump on the main thread dispatches the
 * WM_COPYDATA to it.
 * --------------------------------------------------------------------- */

#ifdef _WIN32
#define SI_MUTEX_NAME "ffplay_single_instance_v1"
#define SI_WNDCLASS   "ffplay_single_instance_msgwin_v1"
#define SI_MAGIC      0x4646504Cu   /* 'FFPL' — WM_COPYDATA dwData tag */

static HANDLE     si_mutex;
static SDL_mutex *si_lock;          /* guards si_pending */
static char      *si_pending;       /* received path, taken by the main loop */
static Uint32     si_wake_event;    /* pushed to wake the event loop (0 = none) */

static HWND si_msgwin;              /* primary's hidden message-only window */

/* Window proc for the primary's message-only window. A secondary instance
 * SendMessage()s WM_COPYDATA here with the file path; the buffer is valid for
 * the duration of the call, so copy it out and wake the event loop. This runs
 * synchronously when SDL pumps the main thread's queue. */
static LRESULT CALLBACK si_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;

        if (cds && cds->dwData == SI_MAGIC && cds->lpData && cds->cbData) {
            char *p = av_malloc(cds->cbData + 1);

            if (p) {
                memcpy(p, cds->lpData, cds->cbData);
                p[cds->cbData] = '\0';
                if (si_lock)
                    SDL_LockMutex(si_lock);
                av_free(si_pending);
                si_pending = p;
                if (si_lock)
                    SDL_UnlockMutex(si_lock);
                /* wake refresh_loop_wait_event (it only returns on an SDL
                 * event, so an idle player would otherwise not notice). */
                if (si_wake_event) {
                    SDL_Event ev;
                    SDL_memset(&ev, 0, sizeof(ev));
                    ev.type = si_wake_event;
                    SDL_PushEvent(&ev);
                }
            }
            return TRUE;
        }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int fsr_single_instance_begin(void)
{
    si_mutex = CreateMutexA(NULL, FALSE, SI_MUTEX_NAME);
    if (si_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;                   /* another instance already owns it */
    return 1;                       /* primary (or mutex failed -> act primary) */
}

int fsr_single_instance_forward(const char *path)
{
    HWND target;
    COPYDATASTRUCT cds;
    DWORD_PTR res = 0;

    if (!path || !path[0])
        return -1;
    /* the primary's receiver is a message-only window, found by class name */
    target = FindWindowExA(HWND_MESSAGE, NULL, SI_WNDCLASS, NULL);
    if (!target)
        return -1;                  /* primary not up yet */

    cds.dwData = SI_MAGIC;
    cds.cbData = (DWORD)strlen(path);
    cds.lpData = (void *)path;
    /* SMTO_NORMAL: wait until the primary next pumps its queue and copies the
     * path (it spends most time in a waitable-timer sleep, so SMTO_ABORTIFHUNG
     * would wrongly treat it as unresponsive). Bounded so we never hang. */
    if (!SendMessageTimeoutA(target, WM_COPYDATA, 0, (LPARAM)&cds,
                             SMTO_NORMAL, 5000, &res))
        return -1;

    /* bring the existing player's visible window forward */
    {
        HWND vis = FindWindowExA(NULL, NULL, "SDL_app", NULL);
        if (vis) { ShowWindow(vis, SW_RESTORE); SetForegroundWindow(vis); }
    }
    return 0;
}

void fsr_single_instance_setup(SDL_Window *window)
{
    WNDCLASSA wc;

    (void)window;
    if (!si_lock)
        si_lock = SDL_CreateMutex();
    if (!si_wake_event) {
        si_wake_event = SDL_RegisterEvents(1);
        if (si_wake_event == (Uint32)-1)
            si_wake_event = 0;      /* registration failed; poll-only */
        else
            /* The registered type lands in the SDL_USEREVENT range, which main()
             * sets to SDL_IGNORE; re-enable ours so the wake push is delivered. */
            SDL_EventState(si_wake_event, SDL_ENABLE);
    }
    if (!si_msgwin) {
        SDL_memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = si_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = SI_WNDCLASS;
        RegisterClassA(&wc);        /* harmless if already registered */
        si_msgwin = CreateWindowExA(0, SI_WNDCLASS, SI_WNDCLASS, 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, NULL, wc.hInstance, NULL);
    }
}

char *fsr_single_instance_take_path(void)
{
    char *p = NULL;

    if (!si_lock)
        return NULL;
    SDL_LockMutex(si_lock);
    p = si_pending;
    si_pending = NULL;
    SDL_UnlockMutex(si_lock);
    return p;
}

/* True if a wide name ends with one of our playable media extensions. */
static int fsr_is_media_name_w(const wchar_t *name)
{
    static const wchar_t *const exts[] = {
        L".mp4", L".mkv", L".avi", L".mov", L".m4v", L".webm", L".flv", L".wmv",
        L".ts", L".m2ts", L".mts", L".mpg", L".mpeg", L".vob", L".ogv", L".3gp",
        L".mp3", L".flac", L".wav", L".m4a", L".aac", L".ogg", L".opus", L".wma",
        L".ape", L".alac", L".wv", L".mka", L".mid", L".aiff", L".dsf",
    };
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t i;

    if (!dot)
        return 0;
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (lstrcmpiW(dot, exts[i]) == 0)
            return 1;
    return 0;
}

static int fsr_name_cmp_w(const void *a, const void *b)
{
    /* locale-aware, case-insensitive — close to Explorer's ordering */
    return lstrcmpiW(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

static wchar_t *fsr_wcsdup(const wchar_t *s)
{
    size_t   n   = wcslen(s) + 1;
    wchar_t *dup = av_malloc(n * sizeof(wchar_t));

    if (dup)
        wmemcpy(dup, s, n);
    return dup;
}

/* ffplay filenames are UTF-8 (SDL's main hands us a UTF-8 argv, and the
 * single-instance forwarder passes UTF-8 too), so directories/files with
 * non-ASCII names — e.g. "Café Cubano Playlist" — are only found through the
 * wide (UTF-16) file API. We convert in, enumerate wide, and convert the
 * chosen path back to UTF-8 for the caller. */
char *fsr_sibling_media_path(const char *cur_path, int dir)
{
    wchar_t        *wpath = NULL, *wpattern = NULL, *wres = NULL;
    wchar_t       **names = NULL;
    const wchar_t  *base;
    size_t          dirlen, i, count = 0, cap = 0;
    int             cur_idx = -1, next_idx, need;
    char           *result = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE          h = INVALID_HANDLE_VALUE;

    if (!cur_path || !cur_path[0] || (dir != 1 && dir != -1))
        return NULL;

    /* UTF-8 -> UTF-16 */
    need = MultiByteToWideChar(CP_UTF8, 0, cur_path, -1, NULL, 0);
    if (need <= 0)
        return NULL;
    wpath = av_malloc((size_t)need * sizeof(wchar_t));
    if (!wpath)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, cur_path, -1, wpath, need);

    /* split directory prefix (with trailing separator) from the file name */
    base = wpath;
    for (const wchar_t *p = wpath; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    dirlen = (size_t)(base - wpath);

    wpattern = av_malloc((dirlen + 2) * sizeof(wchar_t));
    if (!wpattern)
        goto done;
    wmemcpy(wpattern, wpath, dirlen);
    wpattern[dirlen]     = L'*';
    wpattern[dirlen + 1] = L'\0';

    h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        goto done;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!fsr_is_media_name_w(fd.cFileName))
            continue;
        if (count == cap) {
            size_t    ncap = cap ? cap * 2 : 64;
            wchar_t **tmp  = av_realloc(names, ncap * sizeof(*names));
            if (!tmp)
                goto done;
            names = tmp;
            cap   = ncap;
        }
        names[count] = fsr_wcsdup(fd.cFileName);
        if (!names[count])
            goto done;
        count++;
    } while (FindNextFileW(h, &fd));

    if (count <= 1)
        goto done;                      /* nothing to move to */

    qsort(names, count, sizeof(*names), fsr_name_cmp_w);

    /* locate the current file within the sorted list */
    for (i = 0; i < count; i++)
        if (lstrcmpiW(names[i], base) == 0) {
            cur_idx = (int)i;
            break;
        }
    if (cur_idx < 0)
        cur_idx = (dir == 1) ? -1 : 0;  /* unknown: start before first / at first */

    next_idx = (int)(((size_t)cur_idx + count + (size_t)dir) % count);

    /* rebuild the full path (dir prefix + chosen name) and convert to UTF-8 */
    {
        size_t nlen = wcslen(names[next_idx]);
        wres = av_malloc((dirlen + nlen + 1) * sizeof(wchar_t));
        if (wres) {
            wmemcpy(wres, wpath, dirlen);
            wmemcpy(wres + dirlen, names[next_idx], nlen + 1);
            need = WideCharToMultiByte(CP_UTF8, 0, wres, -1, NULL, 0, NULL, NULL);
            if (need > 0 && (result = av_malloc((size_t)need)))
                WideCharToMultiByte(CP_UTF8, 0, wres, -1, result, need, NULL, NULL);
        }
    }

done:
    if (h != INVALID_HANDLE_VALUE)
        FindClose(h);
    for (i = 0; i < count; i++)
        av_free(names[i]);
    av_free(names);
    av_free(wpattern);
    av_free(wpath);
    av_free(wres);
    return result;
}

/* Modal "delete this file?" confirmation, owned by the player window so it
 * comes to front even over a fullscreen window. Returns 1 if the user chose
 * OK. utf8_path is shown so it is clear which file is about to go. */
int fsr_confirm_delete(SDL_Window *window, const char *utf8_path)
{
    wchar_t wpath[1024], wmsg[1200];
    HWND    owner = NULL;

    if (!utf8_path || !utf8_path[0])
        return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0)
        wpath[0] = L'\0';

    if (window) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info) &&
            info.subsystem == SDL_SYSWM_WINDOWS)
            owner = info.info.win.window;
    }

    _snwprintf(wmsg, sizeof(wmsg) / sizeof(wmsg[0]),
               L"이 파일을 삭제하시겠습니까?\n\n%ls", wpath);
    return MessageBoxW(owner, wmsg, L"파일 삭제",
                       MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 |
                       MB_SETFOREGROUND | MB_TOPMOST) == IDOK;
}

/* Send utf8_path to the Recycle Bin (FOF_ALLOWUNDO, so a mistaken delete is
 * recoverable). The file must already be closed by the player. Returns 0 on
 * success. The SHFILEOP source list is double-NUL terminated. */
int fsr_delete_file(const char *utf8_path)
{
    wchar_t          wpath[1024 + 1];
    SHFILEOPSTRUCTW  op;
    int              n;

    if (!utf8_path || !utf8_path[0])
        return -1;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0])) - 1);
    if (n <= 0)
        return -1;
    wpath[n] = L'\0';                    /* extra terminator for the list */

    SDL_memset(&op, 0, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = wpath;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 ? 0 : -1;
}
#else  /* !_WIN32 */
int   fsr_single_instance_begin(void)             { return 1; }
int   fsr_single_instance_forward(const char *p)  { (void)p; return -1; }
void  fsr_single_instance_setup(SDL_Window *w)    { (void)w; }
char *fsr_single_instance_take_path(void)         { return NULL; }
char *fsr_sibling_media_path(const char *p, int d){ (void)p; (void)d; return NULL; }
int   fsr_confirm_delete(SDL_Window *w, const char *p) { (void)w; (void)p; return 0; }
int   fsr_delete_file(const char *p)              { (void)p; return -1; }
#endif

void fsr_uninit(void)
{
#if CONFIG_D3D11VA
    hwgl_exiting = 1;
    fg_destroy();
    hwgl_destroy();
    rife_free_buffers();
    rife_uninit();          /* drop the Vulkan session before the GPU goes */
#endif
    if (vis_tx) { av_tx_uninit(&vis_tx); vis_tx = NULL; }
    av_freep(&vis_win);
    av_freep(&vis_tin);
    av_freep(&vis_tout);
    if (alb_blob)   { SDL_DestroyTexture(alb_blob);   alb_blob   = NULL; }
    if (alb_shine)  { SDL_DestroyTexture(alb_shine);  alb_shine  = NULL; }
    if (alb_lp)     { SDL_DestroyTexture(alb_lp);     alb_lp     = NULL; }
    if (alb_cover)  { SDL_DestroyTexture(alb_cover);  alb_cover  = NULL; }
    if (toast_tex)  { SDL_DestroyTexture(toast_tex);  toast_tex  = NULL; }
    if (hud_tex)    { SDL_DestroyTexture(hud_tex);    hud_tex    = NULL; }
    if (hud_left_tex) { SDL_DestroyTexture(hud_left_tex); hud_left_tex = NULL; }
    if (native_tex) { SDL_DestroyTexture(native_tex); native_tex = NULL; }
    if (easu_tex)   { SDL_DestroyTexture(easu_tex);   easu_tex   = NULL; }
    if (out_tex)    { SDL_DestroyTexture(out_tex);    out_tex    = NULL; }
    native_w = native_h = out_w = out_h = 0;
    if (easu_prog) { gl.DeleteProgram(easu_prog); easu_prog = 0; }
    if (rcas_prog) { gl.DeleteProgram(rcas_prog); rcas_prog = 0; }
    if (copy_prog) { gl.DeleteProgram(copy_prog); copy_prog = 0; }
    if (fsr_state == 1)
        fsr_state = 0;
}
