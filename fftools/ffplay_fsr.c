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

#include <SDL.h>
#include <SDL_opengl.h>

#include "libavutil/log.h"
#include "libavutil/time.h"

#if CONFIG_D3D11VA
#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#include "libavutil/pixfmt.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include "libavutil/avstring.h"

#include "ffplay_fsr.h"

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
    void      (APIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
} gl;

static int          fsr_state;      /* 0 = uninitialized, 1 = ready, -1 = unavailable */
static char         gl_renderer_str[256];
static GLuint       easu_prog, rcas_prog, copy_prog;
static GLint        easu_con0_loc, rcas_sharp_loc, rcas_dns_loc, copy_invout_loc;
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
static const char *rcas_src =
    "#version 330\n"
    "uniform sampler2D srcTex;\n"
    "uniform float sharp;\n" /* exp2(-sharpness stops), computed on the CPU */
    "uniform float dns;\n"   /* 1.0 = FSR_RCAS_DENOISE behavior, 0.0 = off */
    "out vec4 fragColor;\n"
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
    "    fragColor = vec4(pix, 1.0);\n"
    "}\n";

/* Plain textured blit, used when a zero-copy hardware frame is displayed
 * with FSR off or bypassed (handles down/upscaling via linear filtering). */
static const char *copy_src =
    "#version 330\n"
    "uniform sampler2D srcTex;\n"
    "uniform vec2 invOut;\n" /* 1 / output size */
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vec4(texture(srcTex, gl_FragCoord.xy * invOut).rgb, 1.0);\n"
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
    if (prog == rcas_prog)
        gl.Uniform1f(rcas_sharp_loc, sharp);
    if (prog == copy_prog)
        gl.Uniform2f(copy_invout_loc, 1.0f / dw, 1.0f / dh);
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

        if (rb_count < 5 && SDL_SetRenderTarget(renderer, out_tex) == 0) {
            unsigned char px[4] = { 0 };

            SDL_RenderFlush(renderer);
            gl.ReadPixels(rect->w / 2, rect->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            av_log(NULL, AV_LOG_INFO, "FSR: out_tex center pixel: %u %u %u %u\n",
                   px[0], px[1], px[2], px[3]);
            rb_count++;
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

static const char *fg_src =
    "#version 330\n"
    "uniform sampler2D prevTex;\n"
    "uniform sampler2D curTex;\n"
    "uniform isampler2D flowFwd;\n" /* prev->cur, S10.5 px, grid 4 */
    "uniform isampler2D flowBwd;\n" /* cur->prev */
    "uniform float phase;\n"        /* interpolation position, 0=prev 1=cur */
    "uniform vec2 outSize;\n"       /* render size; may differ from native */
    "out vec4 fragColor;\n"
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
    "    vec2 pg = uv * ts / 4.0;\n"
    "    vec2 F = sampleFlow(flowFwd, pg);\n"
    "    vec2 B = sampleFlow(flowBwd, pg);\n"
    "    vec3 cPrev = texture(prevTex, uv - phase * F / ts).rgb;\n"
    "    vec3 cCur  = texture(curTex,  uv - (1.0 - phase) * B / ts).rgb;\n"
    /* Confidence from forward/backward consistency, relative to the motion
     * magnitude; where flow disagrees (occlusions, errors) fall back to the
     * unwarped temporally-nearer frame instead of ghosting. */
    "    float err = length(F + B);\n"
    "    float mag = length(F) + length(B);\n"
    "    float w = clamp(1.0 - err / (3.0 + 0.25 * mag), 0.0, 1.0);\n"
    "    vec3 fallback = phase < 0.5 ? texture(prevTex, uv).rgb\n"
    "                                : texture(curTex, uv).rgb;\n"
    "    vec3 mid = mix(fallback, mix(cPrev, cCur, phase), w);\n"
    "    fragColor = vec4(mid, 1.0);\n"
    "}\n";

static struct {
    int state;                    /* 0 = untried, 1 = ready, -1 = unavailable */
    CudaFunctions *cu;
    CUcontext ctx;
    NV_OF_CUDA_API_FUNCTION_LIST of;
    NvOFHandle hof;
    int w, h, gw, gh;             /* session and flow-grid dimensions */
    NvOFGPUBufferHandle in_buf[2];
    NvOFGPUBufferHandle flow_buf[2];      /* [0] = forward, [1] = backward */
    NV_OF_CUDA_BUFFER_STRIDE_INFO flow_stride[2];
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


    ip.width       = fg.w;
    ip.height      = fg.h;
    ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    ip.mode        = NV_OF_MODE_OPTICALFLOW;
    /* Best flow quality up to 1080p; above that the SLOW preset costs too
     * much of the frame budget to sustain 60 presented fps. */
    ip.perfLevel   = fg.w * fg.h > 1920 * 1080 ? NV_OF_PERF_LEVEL_MEDIUM
                                               : NV_OF_PERF_LEVEL_SLOW;
    if (fg.of.nvOFInit(fg.hof, &ip) != NV_OF_SUCCESS) {
        last_err_sz = sizeof(last_err);
        last_err[0] = 0;
        fg.of.nvOFGetLastError(fg.hof, last_err, &last_err_sz);
        av_log(NULL, AV_LOG_WARNING, "FG: optical flow init failed: %s\n", last_err);
        return -1;
    }

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
        fg.phase_loc   = gl.GetUniformLocation(fg.prog, "phase");
        fg.outsize_loc = gl.GetUniformLocation(fg.prog, "outSize");
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
    fg.gw = (fg.w + 3) / 4;
    fg.gh = (fg.h + 3) / 4;

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
                   int ow, int oh)
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

    *out_sp = sp;
    *out_sn = sn;
    return 0;
}

int fsr_fg_prepare(SDL_Renderer *renderer, AVFrame *prev, AVFrame *next)
{
    int sp, sn;

    return fg_prepare_pair(renderer, prev, next, &sp, &sn) == 0;
}

int fsr_fg_draw(SDL_Renderer *renderer, AVFrame *prev, AVFrame *next,
                const SDL_Rect *rect, int fsr_on, float sharpness, float phase)
{
    int sp, sn, engaged;

    phase = phase < 0.05f ? 0.05f : phase > 0.95f ? 0.95f : phase;

    if (fg_prepare_pair(renderer, prev, next, &sp, &sn) < 0)
        return 0;

    engaged = fsr_on && (rect->w > fg.w || rect->h > fg.h);
    if (engaged) {
        /* FSR path: warp at native size, then EASU+RCAS up to the target. */
        GLfloat con0[4];

        if (ensure_textures(renderer, fg.w, fg.h, rect->w, rect->h) < 0)
            return 0;
        if (fg_warp(renderer, sp, sn, phase, fg.w, fg.h) < 0) {
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
     * warp target directly. */
    if (fg_warp(renderer, sp, sn, phase, rect->w, rect->h) < 0) {
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
    case '/': return gSl;
    case '.': return gDot;
    default:  return NULL; /* rendered as blank */
    }
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
    if (toast_tex) {
        SDL_DestroyTexture(toast_tex);
        toast_tex = NULL;
    }
    toast_tex = toast_render(renderer, text, &toast_w, &toast_h);
    if (toast_tex)
        toast_until = av_gettime_relative() + TOAST_DURATION_US;
}

/* Persistent HUD (FPS display): same pixel font, top-right corner. */
static SDL_Texture *hud_tex;
static int          hud_w, hud_h;

void fsr_hud_set(SDL_Renderer *renderer, const char *text)
{
    if (hud_tex) {
        SDL_DestroyTexture(hud_tex);
        hud_tex = NULL;
    }
    if (text && text[0])
        hud_tex = toast_render(renderer, text, &hud_w, &hud_h);
}

int fsr_hud_draw(SDL_Renderer *renderer)
{
    int ow = 0, oh = 0, scale;
    SDL_Rect dst;

    if (!hud_tex)
        return 0;
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    scale = oh / 180;
    if (scale < 2)
        scale = 2;
    dst.w = hud_w * scale;
    dst.h = hud_h * scale;
    dst.x = ow - dst.w - 16;
    dst.y = 16;
    SDL_RenderCopy(renderer, hud_tex, NULL, &dst);
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
    scale = oh / 180;
    if (scale < 2)
        scale = 2;
    alpha = rem < TOAST_FADE_US ? (int)(255 * rem / TOAST_FADE_US) : 255;
    SDL_SetTextureAlphaMod(toast_tex, alpha);
    dst.x = 16;
    dst.y = 16;
    dst.w = toast_w * scale;
    dst.h = toast_h * scale;
    SDL_RenderCopy(renderer, toast_tex, NULL, &dst);
    return 1;
}

void fsr_uninit(void)
{
#if CONFIG_D3D11VA
    hwgl_exiting = 1;
    fg_destroy();
    hwgl_destroy();
#endif
    if (toast_tex)  { SDL_DestroyTexture(toast_tex);  toast_tex  = NULL; }
    if (hud_tex)    { SDL_DestroyTexture(hud_tex);    hud_tex    = NULL; }
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
