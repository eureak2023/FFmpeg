/*
 * lada real-time mosaic-restoration client for ffplay.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "ffplay_lada.h"

#ifdef _WIN32
#include <windows.h>

/* Wire format of one restored frame streamed from lada_sidecar.py (little-endian,
 * packed): magic 'LADA' | u32 gen | i32 width | i32 height | f64 pts_sec | i32 len,
 * followed by len bytes of RGB24. len == 0 is an end-of-stream marker. */
#define LADA_MAGIC "LADA"
#define LADA_HEADER_SIZE 28

/* Cap the amount of restored video we buffer ahead; the sidecar then blocks on the
 * pipe (natural backpressure) rather than growing without bound. A large buffer lets
 * the sidecar build a big lead so restoration keeps up through throughput dips, and is
 * what the pause-to-buffer logic fills while playback is held. ~2 GB is ~340 1080p
 * frames (~11 s) or ~85 4K frames (~3 s). */
#define LADA_MAX_BUFFER_BYTES (2ull * 1024 * 1024 * 1024)

/* Restoration starts a few seconds AHEAD of the current playback position. The sidecar
 * needs ~5 s to emit its first restored frame (decode + detect + fill the first clip),
 * during which playback would move on and the restored output would land behind the
 * display forever (it produces only ~20% faster than real time, so catching up from a
 * standing start takes ~20 s of visible original). By opening ahead, the upcoming
 * segment is already restored by the time playback reaches it: the first ~LEAD seconds
 * show the original, then restoration is applied cleanly with no long catch-up. */
#define LADA_LEAD_SEC 7.0

/* Seek coalescing window. Dragging the seek bar fires a burst of seeks (one per
 * intermediate position). Each SEEK makes the sidecar tear down and re-open the whole
 * pipeline (~seconds, serialized), so acting on every one races the generation counter
 * and backs the sidecar up until the FIFO is empty forever ("LADA WAIT" that never
 * clears). Instead we record the latest seek target and only re-open once the position
 * has stopped moving for this long. */
#define LADA_SEEK_DEBOUNCE_MS 350

/* Give up on a pause-to-buffer that stops making progress. A healthy restorer emits its
 * first frame ~1-2 s after a SEEK and then streams continuously, so this many ms with the
 * buffered-ahead count never once growing means the sidecar is not coming back (a wedged
 * pass, or one that died without closing the stream). Rather than hold the picture frozen
 * in "BUFFER", we disengage and play the originals - degrade, never stall. */
#define LADA_BUFFER_STALL_MS 10000

typedef struct LadaFrame {
    uint32_t gen;
    double   pts;
    int      w, h;
    uint8_t *rgb;
    size_t   len;
    struct LadaFrame *next;
} LadaFrame;

static struct {
    int        running;      /* sidecar process + reader thread are alive     */
    int        enabled;      /* user wants restoration streamed (toggled by L) */
    int        died;         /* reader saw the sidecar stream end unexpectedly */
    HANDLE     proc;
    HANDLE     job;          /* kill-on-close job so the sidecar can't outlive us */
    HANDLE     stdin_wr;     /* we write commands here          */
    HANDLE     stdout_rd;    /* reader thread reads frames here */
    SDL_Thread *reader;
    volatile int quit;

    SDL_mutex *mtx;
    SDL_cond  *space;        /* signalled when the FIFO shrinks */

    LadaFrame *head, *tail;  /* FIFO of restored frames         */
    int        count;
    size_t     bytes;
    int        max_count;    /* 0 until known from first frame  */

    LadaFrame *held;         /* frame locked to the current displayed pts */
    uint32_t   gen;          /* generation of the current OPEN/SEEK        */
    int        ready;        /* sidecar has finished loading models        */
    int        opened;       /* OPEN already sent                          */
    double     last_pts;     /* previous requested pts (seek detection)    */

    char       path[4096];   /* UTF-8 input path                           */

    char       toast[24];    /* one-shot status message for the main thread */
    int        toast_ready;
    int        announced;    /* "LADA ACTIVE" already toasted this enable session */
    int        buffering;    /* pause-to-buffer state (hysteresis for lada_should_buffer) */
    Uint32     buffer_since; /* when the current buffer-pause last grew (stall watchdog) */
    int        buffer_best;  /* deepest `ahead` seen during the current buffer-pause     */
    int        warmed;       /* restoration for the current generation has started arriving
                             * (or been applied) since the last OPEN/SEEK; gates pause-to-
                             * buffer so it engages only once the restorer is actually live */
    int        seek_pending; /* a seek was detected, waiting for the scrub to settle */
    double     seek_pending_pts;
    Uint32     seek_pending_at;
} L;

static int dbg_applying;   /* whether restored frames are currently being shown */

/* Queue a short status toast for the main thread to pick up (caller holds L.mtx). */
static void lada_set_toast(const char *msg)
{
    av_strlcpy(L.toast, msg, sizeof(L.toast));
    L.toast_ready = 1;
}

/* Main thread: fetch a pending status toast (e.g. "LADA ACTIVE", "LADA FAILED").
 * Returns 1 and fills buf if one is waiting, else 0. */
int lada_poll_toast(char *buf, int buflen)
{
    int got = 0;
    if (!L.mtx)
        return 0;
    SDL_LockMutex(L.mtx);
    if (L.toast_ready) {
        av_strlcpy(buf, L.toast, buflen);
        L.toast_ready = 0;
        got = 1;
    }
    SDL_UnlockMutex(L.mtx);
    return got;
}

static void lada_frame_free(LadaFrame *f)
{
    if (f) {
        av_free(f->rgb);
        av_free(f);
    }
}

/* Read exactly n bytes from the sidecar pipe; 0 on clean EOF / error. */
static int read_full(uint8_t *buf, DWORD n)
{
    DWORD got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(L.stdout_rd, buf + got, n - got, &r, NULL) || r == 0)
            return 0;
        got += r;
    }
    return 1;
}

static int reader_thread(void *arg)
{
    uint8_t hdr[LADA_HEADER_SIZE];
    (void)arg;

    while (!L.quit) {
        uint32_t gen;
        int32_t w, h, len;
        double pts;
        LadaFrame *f;

        if (!read_full(hdr, LADA_HEADER_SIZE))
            break;
        if (memcmp(hdr, LADA_MAGIC, 4) != 0) {
            av_log(NULL, AV_LOG_WARNING, "lada: lost sync with sidecar stream\n");
            break;
        }
        memcpy(&gen, hdr + 4,  4);
        memcpy(&w,   hdr + 8,  4);
        memcpy(&h,   hdr + 12, 4);
        memcpy(&pts, hdr + 16, 8);
        memcpy(&len, hdr + 24, 4);

        if (w < 0) {           /* READY marker: models finished loading */
            SDL_LockMutex(L.mtx);
            L.ready = 1;
            SDL_UnlockMutex(L.mtx);
            continue;
        }
        if (len == 0) {        /* EOF marker: this generation produced its last frame */
            SDL_LockMutex(L.mtx);
            /* No more frames will ever arrive for this generation - either the file
             * ended, or the restore pass died (jasna logs "pass error" and keeps the
             * process alive, so the stream does NOT close and `died` never trips).
             * Either way, holding playback to wait for a refill would freeze the
             * picture in "BUFFER" forever, so disarm the gate here and let playback
             * continue on the original frames. `warmed` re-arms by itself as soon as
             * the next OPEN/SEEK starts delivering again. */
            if (gen == L.gen) {
                L.warmed    = 0;
                L.buffering = 0;
            }
            SDL_UnlockMutex(L.mtx);
            continue;
        }
        if (w <= 0 || h <= 0 || len != w * h * 3) {
            av_log(NULL, AV_LOG_WARNING, "lada: bad frame header %dx%d len=%d\n", w, h, len);
            break;
        }

        f = av_mallocz(sizeof(*f));
        if (f)
            f->rgb = av_malloc(len);
        if (!f || !f->rgb) {
            lada_frame_free(f);
            break;
        }
        if (!read_full(f->rgb, len)) {
            lada_frame_free(f);
            break;
        }
        f->gen = gen; f->pts = pts; f->w = w; f->h = h; f->len = len;

        SDL_LockMutex(L.mtx);
        if (!L.max_count) {
            /* Buffer budget in MB, overridable via LADA_BUFFER_MB (tuning / testing). */
            uint64_t budget = LADA_MAX_BUFFER_BYTES;
            const char *env = getenv("LADA_BUFFER_MB");
            if (env && *env) {
                long mb = atol(env);
                if (mb > 0) budget = (uint64_t)mb * 1024 * 1024;
            }
            L.max_count = (int)(budget / (uint64_t)len);
            if (L.max_count < 2)    L.max_count = 2;
            if (L.max_count > 4000) L.max_count = 4000;
        }
        while (!L.quit && L.count >= L.max_count)
            SDL_CondWait(L.space, L.mtx);
        if (L.quit) {
            SDL_UnlockMutex(L.mtx);
            lada_frame_free(f);
            break;
        }
        f->next = NULL;
        if (L.tail) L.tail->next = f; else L.head = f;
        L.tail = f;
        L.count++;
        L.bytes += len;
        /* Arm the buffer gate the moment restoration for the CURRENT generation starts
         * arriving - not only once a frame is successfully applied. After a seek the
         * sidecar restores LADA_LEAD_SEC ahead and takes a few seconds to produce the
         * first frame; by then playback may already have passed that pts, so the frame
         * arrives "behind" and lada_frame_for drops it -> no apply -> warmed never set ->
         * gate never engages -> playback outruns restoration forever (permanent WAIT).
         * Setting warmed here lets the gate pause playback so restoration can catch up. */
        if (f->gen == L.gen)
            L.warmed = 1;
        SDL_UnlockMutex(L.mtx);
    }
    /* If the stream ended while we were still meant to be streaming, the sidecar died
     * (crash, model-load failure, OOM). Flag it so lada_active() goes false and the main
     * thread can toast + clean up - see lada/lada_sidecar.log for the reason. Cleanup
     * (killing the proc, freeing handles) is left to the main thread to avoid racing a
     * concurrent lada_start(). */
    if (!L.quit) {
        SDL_LockMutex(L.mtx);
        L.died = 1;
        if (L.enabled) {
            char tmsg[24];
            snprintf(tmsg, sizeof(tmsg), "%s FAILED", lada_engine_name());
            lada_set_toast(tmsg);
        }
        SDL_UnlockMutex(L.mtx);
        av_log(NULL, AV_LOG_WARNING, "lada: sidecar stream ended unexpectedly (see lada/lada_sidecar.log)\n");
    }
    return 0;
}

/* FIFO helpers; caller holds L.mtx. */
static LadaFrame *fifo_pop(void)
{
    LadaFrame *f = L.head;
    if (!f) return NULL;
    L.head = f->next;
    if (!L.head) L.tail = NULL;
    L.count--;
    L.bytes -= f->len;
    SDL_CondSignal(L.space);
    return f;
}

static void fifo_flush(void)
{
    LadaFrame *f = L.head;
    while (f) { LadaFrame *n = f->next; lada_frame_free(f); f = n; }
    L.head = L.tail = NULL;
    L.count = 0;
    L.bytes = 0;
    SDL_CondSignal(L.space);
}

static void send_cmd(const char *cmd)
{
    DWORD wr = 0;
    if (L.stdin_wr && L.stdin_wr != INVALID_HANDLE_VALUE)
        WriteFile(L.stdin_wr, cmd, (DWORD)strlen(cmd), &wr, NULL);
}

/* The sidecar runs with its cwd at lada_home, so it must receive an absolute path.
 * input_path is UTF-8 (FFmpeg converts argv to UTF-8 on Windows); keep it UTF-8 by
 * building the prefix through the wide-char cwd API rather than the ANSI one. */
static void absolutize_path(const char *in, char *out, size_t out_sz)
{
    int unc  = (in[0] == '\\' || in[0] == '/') && (in[1] == '\\' || in[1] == '/');
    int drive = in[0] && in[1] == ':';
    if (unc || drive || in[0] == '/' || in[0] == '\\') {
        av_strlcpy(out, in, out_sz);          /* already absolute */
        return;
    }
    {
        wchar_t wcwd[4096];
        char cwd[4096];
        DWORD n = GetCurrentDirectoryW(4096, wcwd);
        if (n == 0 || n >= 4096 ||
            !WideCharToMultiByte(CP_UTF8, 0, wcwd, -1, cwd, sizeof(cwd), NULL, NULL)) {
            av_strlcpy(out, in, out_sz);      /* best effort */
            return;
        }
        snprintf(out, out_sz, "%s\\%s", cwd, in);
    }
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Build the sidecar's environment with ITS OWN torch\lib as the only cuDNN on PATH.
 *
 * cudnn64_9.dll does not link its sublibraries (cudnn_graph/ops/cnn/engines_*64_9.dll) -
 * it LoadLibrary()s them lazily, by BARE NAME, which searches the exe dir, system32 and
 * then PATH, but never torch\lib (torch registers that via os.add_dll_directory, which
 * bare-name loads ignore). So a CUDA Toolkit on the player's PATH can answer that load
 * with a DIFFERENT cuDNN than the sidecar bundles.
 *
 * That is not hypothetical: installing CUDA v13.3 (cuDNN 9.24) broke restoration on this
 * box. Its bin\x64 carries cudnn_engines_tensor_ir64_9.dll, a sublibrary the bundled 9.20
 * does not ship - so before that install the lazy load simply found nothing and 9.20 ran
 * fine without it, but afterwards it loads the 9.24 file, the version check fails, and
 * EVERY convolution dies with "CUDNN_STATUS_SUBLIBRARY_VERSION_MISMATCH". The sidecar
 * catches that per pass and stays alive, so it just stops emitting frames (the "pass
 * error" lines in jasna_sidecar.log) - which is what used to strand playback in "BUFFER".
 * Note this cannot be fixed by putting torch\lib first: the file is not there at all, so
 * the search falls through to the toolkit. The foreign directories must be REMOVED.
 *
 * The child gets its own copy of PATH, so the player's own stays untouched.
 * Returns an av_malloc'd "K=V\0...\0" block for CreateProcess, or NULL to inherit ours. */
static char *make_sidecar_env(const char *torchlib)
{
    char *env, *out, *w;
    const char *p;
    size_t total = 0;
    int patched = 0;

    if (!torchlib || !torchlib[0])
        return NULL;
    env = GetEnvironmentStringsA();
    if (!env)
        return NULL;
    for (p = env; *p; p += strlen(p) + 1)
        total += strlen(p) + 1;
    out = av_malloc(total + strlen(torchlib) + 32);
    if (!out) {
        FreeEnvironmentStringsA(env);
        return NULL;
    }
    w = out;
    for (p = env; *p; p += strlen(p) + 1) {
        if (!patched && !_strnicmp(p, "PATH=", 5)) {
            /* Rebuild PATH as: our torch\lib, then every inherited entry that does NOT
             * carry a cuDNN of its own. Dropping whole directories (rather than trying to
             * match individual files) is what keeps a toolkit-only sublibrary like
             * cudnn_engines_tensor_ir64_9.dll from being found at all. */
            char *dirs = av_strdup(p + 5), *dir, *save = NULL;
            w += sprintf(w, "PATH=%s", torchlib);
            for (dir = dirs ? av_strtok(dirs, ";", &save) : NULL; dir;
                 dir = av_strtok(NULL, ";", &save)) {
                char probe[4096];
                if (!*dir)
                    continue;
                snprintf(probe, sizeof(probe), "%s\\cudnn64_9.dll", dir);
                if (file_exists(probe)) {
                    av_log(NULL, AV_LOG_VERBOSE,
                           "lada: hiding foreign cuDNN from the sidecar: %s\n", dir);
                    continue;
                }
                w += sprintf(w, ";%s", dir);
            }
            av_free(dirs);
            w++;                        /* step over the NUL sprintf wrote */
            patched = 1;
        } else {
            size_t n = strlen(p) + 1;
            memcpy(w, p, n);
            w += n;
        }
    }
    if (!patched)                       /* no PATH inherited: give the child just ours */
        w += sprintf(w, "PATH=%s", torchlib) + 1;
    *w = 0;                             /* env blocks end with a second NUL */
    FreeEnvironmentStringsA(env);
    return out;
}

int lada_default_on(void)
{
    char exepath[4096], *base, *p;
    DWORD n = GetModuleFileNameA(NULL, exepath, sizeof(exepath));
    if (n == 0 || n >= sizeof(exepath))
        return 0;
    base = strrchr(exepath, '\\');
    base = base ? base + 1 : exepath;
    for (p = base; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;   /* ASCII lower-case */
    return strstr(base, "lada") != NULL;
}

/* Choose how to launch the sidecar and from which working directory (the sidecar
 * looks up model_weights/ relative to its cwd). Prefer the standalone PyInstaller exe
 * so no Python/venv is needed; fall back to the venv interpreter + script.
 *   1. <ffplay.exe dir>\lada_sidecar\lada_sidecar.exe   (bundled next to the player)
 *   2. <lada_home>\dist\lada_sidecar\lada_sidecar.exe    (built in the source tree)
 *   3. <lada_home>\.venv\Scripts\python.exe lada_sidecar.py   (dev fallback) */
/* Alternative "jasna" restoration engine (github.com/Kruk2/jasna): a from-source sidecar
 * that restores with jasna's GPU/TensorRT pipeline and emits the SAME raw-RGB wire format
 * as lada_sidecar, so the whole display path is reused. Enabled with -jasna. g_jasna_home
 * is the jasna source checkout (holds .venv + model_weights + the importable jasna pkg). */
/* The script lives inside the checkout (it is versioned with the jasna fork), so it moves
 * with -jasna_home instead of being pinned to one absolute path. */
#define JASNA_SIDECAR_PY "jasna_sidecar.py"
/* jasna is the only restoration engine (the older lada_sidecar path was removed). Default
 * ON so restoration works without any extra flag; -jasna_home can relocate the checkout. */
static int  g_use_jasna = 1;
static char g_jasna_home[4096] = "D:/Source_AI/ffplay-fsr1/jasna";

void lada_set_engine(int use_jasna, const char *jasna_home)
{
    g_use_jasna = use_jasna ? 1 : 0;
    if (g_use_jasna)
        av_strlcpy(g_jasna_home,
                   jasna_home && *jasna_home ? jasna_home : "D:/Source_AI/ffplay-fsr1/jasna",
                   sizeof(g_jasna_home));
}

void lada_set_jasna(const char *jasna_home) { lada_set_engine(1, jasna_home); }

int lada_is_jasna(void) { return g_use_jasna; }

/* Which restoration engine is active - for the status HUD / toasts, so the two are
 * distinguishable on screen (both otherwise share the lada display machinery). */
const char *lada_engine_name(void)
{
    return g_use_jasna ? "JASNA" : "LADA";
}

/* torchlib receives the sidecar's own torch\lib for make_sidecar_env() (empty if absent). */
static void resolve_sidecar(char *cmdline, size_t cmdsz, char *workdir, size_t wdsz,
                            char *torchlib, size_t tlsz,
                            const char *lada_home, const char *device)
{
    char exedir[4096] = {0}, cand[4096];
    (void)lada_home; (void)device;   /* jasna is the only restoration engine now */

    torchlib[0] = 0;

    DWORD n = GetModuleFileNameA(NULL, exedir, sizeof(exedir));
    if (n > 0 && n < sizeof(exedir)) {
        char *slash = strrchr(exedir, '\\');
        if (slash) *slash = 0; else exedir[0] = 0;
    } else {
        exedir[0] = 0;
    }

    /* jasna sidecar: prefer the bundled onedir exe (portable, no venv) next to the player,
     * else the dist build in the source tree, else the dev venv python + script. Its
     * model_weights sits alongside each. Streams restored RGB frames over the pipe. */
    if (exedir[0]) {
        snprintf(cand, sizeof(cand), "%s\\jasna_sidecar\\jasna_sidecar.exe", exedir);
        if (file_exists(cand)) {
            snprintf(cmdline, cmdsz,
                     "\"%s\" --model-weights \"%s\\jasna_sidecar\\model_weights\" "
                     "--log \"%s\\jasna_sidecar.log\"", cand, exedir, exedir);
            av_strlcpy(workdir, exedir, wdsz);
            snprintf(torchlib, tlsz, "%s\\jasna_sidecar\\_internal\\torch\\lib", exedir);
            if (!dir_exists(torchlib)) torchlib[0] = 0;
            return;
        }
    }
    snprintf(cand, sizeof(cand), "%s\\dist\\jasna_sidecar\\jasna_sidecar.exe", g_jasna_home);
    if (file_exists(cand)) {
        snprintf(cmdline, cmdsz,
                 "\"%s\" --model-weights \"%s\\dist\\jasna_sidecar\\model_weights\" "
                 "--log \"%s\\jasna_sidecar.log\"", cand, g_jasna_home, g_jasna_home);
        av_strlcpy(workdir, g_jasna_home, wdsz);
        snprintf(torchlib, tlsz, "%s\\dist\\jasna_sidecar\\_internal\\torch\\lib", g_jasna_home);
        if (!dir_exists(torchlib)) torchlib[0] = 0;
        return;
    }
    snprintf(cmdline, cmdsz,
             "\"%s\\.venv\\Scripts\\python.exe\" \"%s\\" JASNA_SIDECAR_PY "\" "
             "--model-weights \"%s\\model_weights\" --log \"%s\\jasna_sidecar.log\"",
             g_jasna_home, g_jasna_home, g_jasna_home, g_jasna_home);
    av_strlcpy(workdir, g_jasna_home, wdsz);
    snprintf(torchlib, tlsz, "%s\\.venv\\Lib\\site-packages\\torch\\lib", g_jasna_home);
    if (!dir_exists(torchlib)) torchlib[0] = 0;
}

int lada_start(const char *input_path, const char *lada_home, const char *device)
{
    char cmdline[8192];
    char workdir[4096];
    char torchlib[4096];
    char *envblk;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE child_stdin_rd = NULL, child_stdin_wr = NULL;
    HANDLE child_stdout_rd = NULL, child_stdout_wr = NULL;

    if (L.running)          /* the sidecar is a persistent process; spawn it once */
        return 0;

    memset(&L, 0, sizeof(L));
    L.last_pts = NAN;
    absolutize_path(input_path, L.path, sizeof(L.path));

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    /* Child stdout -> our read handle; child stdin <- our write handle. Only the
     * child ends are inheritable; keep our ends private so the pipes close cleanly. */
    if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0) ||
        !CreatePipe(&child_stdin_rd,  &child_stdin_wr,  &sa, 0)) {
        av_log(NULL, AV_LOG_WARNING, "lada: CreatePipe failed\n");
        goto fail;
    }
    SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdin_wr,  HANDLE_FLAG_INHERIT, 0);

    resolve_sidecar(cmdline, sizeof(cmdline), workdir, sizeof(workdir),
                    torchlib, sizeof(torchlib),
                    lada_home, device && *device ? device : "cuda");
    envblk = make_sidecar_env(torchlib);   /* pin its cuDNN - see make_sidecar_env() */

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = child_stdin_rd;
    si.hStdOutput = child_stdout_wr;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);   /* logs -> our console */
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        envblk, workdir, &si, &pi)) {
        av_free(envblk);
        av_log(NULL, AV_LOG_WARNING,
               "lada: could not launch sidecar (%s) - restoration disabled\n", cmdline);
        goto fail;
    }
    av_free(envblk);   /* CreateProcess copied it into the child */
    /* Put the sidecar in a kill-on-close job object: if this player exits for ANY
     * reason (including a crash or being force-killed, when our cleanup never runs),
     * Windows closes the job handle and terminates the sidecar with it - so a multi-GB
     * torch/CUDA process can never be left orphaned. */
    L.job = CreateJobObjectA(NULL, NULL);
    if (L.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(L.job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(L.job, pi.hProcess);
    }
    CloseHandle(pi.hThread);
    /* Hand the child ends to the child; we keep only our private ends. */
    CloseHandle(child_stdout_wr); child_stdout_wr = NULL;
    CloseHandle(child_stdin_rd);  child_stdin_rd  = NULL;

    L.proc      = pi.hProcess;
    L.stdout_rd = child_stdout_rd;
    L.stdin_wr  = child_stdin_wr;
    L.mtx   = SDL_CreateMutex();
    L.space = SDL_CreateCond();
    if (!L.mtx || !L.space)
        goto fail;
    L.reader = SDL_CreateThread(reader_thread, "lada-reader", NULL);
    if (!L.reader)
        goto fail;

    L.running = 1;
    av_log(NULL, AV_LOG_INFO, "lada: mosaic-restoration sidecar started (%s), loading models...\n", device);
    return 0;

fail:
    if (child_stdout_rd) CloseHandle(child_stdout_rd);
    if (child_stdout_wr) CloseHandle(child_stdout_wr);
    if (child_stdin_rd)  CloseHandle(child_stdin_rd);
    if (child_stdin_wr)  CloseHandle(child_stdin_wr);
    if (L.proc) { TerminateProcess(L.proc, 0); CloseHandle(L.proc); }
    if (L.job)  CloseHandle(L.job);
    if (L.mtx)   SDL_DestroyMutex(L.mtx);
    if (L.space) SDL_DestroyCond(L.space);
    memset(&L, 0, sizeof(L));
    return -1;
}

void lada_stop(void)
{
    if (!L.running && !L.proc)
        return;

    L.quit = 1;
    if (L.mtx) {
        SDL_LockMutex(L.mtx);
        SDL_CondSignal(L.space);   /* wake the reader if it is blocked on a full FIFO */
        SDL_UnlockMutex(L.mtx);
    }

    /* Kill the sidecar immediately rather than asking it to exit: a Python + torch/CUDA
     * process can take a second or more to unwind on its own, which made closing the
     * player feel sluggish. It holds no state worth saving, so TerminateProcess is fine.
     * Killing it closes the child's pipe ends, so the reader's ReadFile returns at once
     * and the thread joins without hanging. */
    if (L.proc) {
        TerminateProcess(L.proc, 0);
        CloseHandle(L.proc);
        L.proc = NULL;
    }
    if (L.job) { CloseHandle(L.job); L.job = NULL; }   /* also kills anything left in it */
    if (L.stdin_wr)  CloseHandle(L.stdin_wr),  L.stdin_wr  = NULL;
    if (L.stdout_rd) CloseHandle(L.stdout_rd), L.stdout_rd = NULL;
    if (L.reader) { SDL_WaitThread(L.reader, NULL); L.reader = NULL; }

    fifo_flush();
    lada_frame_free(L.held); L.held = NULL;
    if (L.space) SDL_DestroyCond(L.space), L.space = NULL;
    if (L.mtx)   SDL_DestroyMutex(L.mtx),  L.mtx = NULL;
    L.running = 0;
    L.enabled = 0;
}

/* Toggle restoration streaming without tearing the sidecar down: the models stay
 * resident so re-enabling is instant and starts from the current playback position.
 * On enable the next lada_frame_for() sends OPEN at the displayed pts; on disable we
 * tell the sidecar to stop its restorer and drop everything we had buffered. */
void lada_set_enabled(int on)
{
    if (!L.running || on == L.enabled)
        return;
    SDL_LockMutex(L.mtx);
    if (on) {
        L.enabled = 1;
        L.opened  = 0;             /* re-OPEN at the current pts on the next frame */
        L.last_pts = NAN;
        L.announced = 0;
        L.warmed = 0;
        L.seek_pending = 0;
    } else {
        L.enabled = 0;
        L.gen++;                   /* invalidate frames still in flight            */
        send_cmd("STOP\n");
        fifo_flush();
        lada_frame_free(L.held); L.held = NULL;
        L.opened = 0;
        dbg_applying = 0;
        L.buffering = 0;
        L.warmed = 0;
        L.seek_pending = 0;
    }
    SDL_UnlockMutex(L.mtx);
}

int lada_active(void)
{
    return L.running && L.enabled && !L.died;
}

/* Called from the seek path (stream_seek) the instant a seek is requested, off the
 * display thread. Without it a forward seek can strand playback in a permanent
 * "LADA BUFFER" pause: the buffer gate pauses because nothing is restored at the new
 * position yet, but while paused the display path never runs, so lada_frame_for can
 * never notice the seek and re-open the sidecar - a deadlock. Clearing `warmed` here
 * disengages the gate immediately (it early-returns while !warmed), so playback resumes,
 * the display path runs, and the seek is handled normally (debounced re-open). */
void lada_notify_seek(void)
{
    if (!L.running)
        return;
    SDL_LockMutex(L.mtx);
    L.warmed    = 0;
    L.buffering = 0;
    SDL_UnlockMutex(L.mtx);
}

const char *lada_status(void)
{
    if (!L.enabled || !L.running) return "OFF";
    if (L.died)                   return "FAILED";
    if (!L.ready)                 return "LOADING";
    if (L.buffering)              return "BUFFER";
    if (dbg_applying)             return "ACTIVE";
    return "WAIT";   /* models ready, buffering the lead - not applied yet */
}

/* Should playback pause to let restoration buffer? Once restoration has started
 * applying (announced), we watch how many restored frames are buffered at/ahead of the
 * displayed pts: if it empties (can't keep up) we ask to pause, and while paused the
 * sidecar keeps decoding and fills the buffer; we release once it is mostly full again.
 * Hysteresis (empty -> ~3/4 full) avoids rapid pause/resume flapping. Returns 1 to pause. */
int lada_should_buffer(double display_pts)
{
    int ahead = 0, cap, high;
    LadaFrame *f;

    /* Gate on `warmed`, not `announced`: `announced` stays set for the whole enable
     * session, so during the spin-up after an OPEN (nothing restored yet) it would see
     * ahead==0 and pause immediately with nothing to wait for. `warmed` is reset on every
     * OPEN/SEEK and set only once the restorer for the current generation is actually
     * producing frames, so the gate engages when there is real restoration to buffer. */
    if (!L.running || !L.enabled || L.died || !L.ready || !L.opened || !L.warmed)
        return 0;

    SDL_LockMutex(L.mtx);
    /* Right after a seek the audio master clock jumps to the new position while the
     * displayed video pts (last_pts, what lada_frame_for actually matches against) still
     * lags at the old one. In that window the restored buffer legitimately can't cover
     * the master pts - but pausing here freezes playback before the video can advance to
     * the new region and let the seek be detected + the sidecar re-opened, a permanent
     * "LADA BUFFER" hang (a stale old-position frame keeps re-arming `warmed`, and with
     * the master pts ahead of the whole buffer `ahead` is stuck at 0 so it never resumes).
     * So while the two clocks disagree, never pause; wait until they reconverge. */
    if (isnan(L.last_pts) || fabs(display_pts - L.last_pts) > 1.0) {
        L.buffering = 0;
        SDL_UnlockMutex(L.mtx);
        return 0;
    }
    for (f = L.head; f; f = f->next)
        if (f->gen == L.gen && f->pts >= display_pts - 0.05)
            ahead++;
    cap  = L.max_count > 0 ? L.max_count : 60;
    high = 45;                       /* resume once ~1.5 s (@30fps) is buffered ahead;
                                      * a fixed count, since the byte-budget cap can be
                                      * hundreds of frames (a fraction of which would be a
                                      * many-second pause). Override with LADA_RESUME_FRAMES. */
    {
        const char *e = getenv("LADA_RESUME_FRAMES");
        if (e && atoi(e) > 0) high = atoi(e);
    }
    if (high > cap) high = cap;
    if (high < 1)   high = 1;
    if (L.buffering) {
        if (ahead >= high) {
            L.buffering = 0;                  /* refilled -> resume */
        } else if (ahead > L.buffer_best) {
            L.buffer_best  = ahead;           /* still filling -> keep waiting */
            L.buffer_since = SDL_GetTicks();
        } else if ((int)(SDL_GetTicks() - L.buffer_since) >= LADA_BUFFER_STALL_MS) {
            /* Nothing new for LADA_BUFFER_STALL_MS: the restorer is not coming back.
             * Backstop for a sidecar that wedges without even closing its stream (the
             * EOF marker handles the cases where it does) - see LADA_BUFFER_STALL_MS. */
            L.buffering = 0;
            L.warmed    = 0;                  /* keep the gate off until it delivers again */
            av_log(NULL, AV_LOG_WARNING,
                   "lada: no restored frames for %d s while buffering at %.2f - "
                   "resuming on the original video\n", LADA_BUFFER_STALL_MS / 1000, display_pts);
        }
    } else {
        if (ahead <= 0) {                     /* drained -> pause (still on held frame) */
            L.buffering    = 1;
            L.buffer_best  = ahead;
            L.buffer_since = SDL_GetTicks();
        }
    }
    ahead = L.buffering;
    SDL_UnlockMutex(L.mtx);
    return ahead;
}

int lada_frame_for(double pts_sec, double dur_sec, const uint8_t **rgb, int *w, int *h)
{
    double tol, fwd;
    LadaFrame *f;

    if (!L.running || !L.enabled)
        return 0;

    tol = dur_sec * 0.5;
    if (tol < 0.005) tol = 0.005;
    if (tol > 0.050) tol = 0.050;
    fwd = dur_sec * 4.0;
    if (fwd < 0.5) fwd = 0.5;

    SDL_LockMutex(L.mtx);

    if (!L.opened) {
        char cmd[4160];
        if (!L.ready) {          /* wait for models to load, then OPEN at the *current*
                                  * pts so restoration starts here, not where playback
                                  * was when the sidecar was still loading. */
            SDL_UnlockMutex(L.mtx);
            return 0;
        }
        snprintf(cmd, sizeof(cmd), "OPEN\t%.6f\t%u\t%s\n", pts_sec + LADA_LEAD_SEC, L.gen, L.path);
        send_cmd(cmd);
        L.opened = 1;
        L.warmed = 0;
        L.last_pts = pts_sec;
        SDL_UnlockMutex(L.mtx);
        return 0;
    }

    /* Detect a seek from the pts stream: a jump backward, or a large jump forward.
     *
     * A backward jump is always a real seek. A large FORWARD jump, however, is often
     * NOT a seek: dragging/moving the window starves the video refresh for a moment
     * while the audio clock keeps running, so when it resumes the displayed pts leaps
     * forward to catch up. Because the sidecar restores LADA_LEAD_SEC ahead, the frame
     * for that new pts is usually ALREADY in the FIFO. Flushing + re-seeking there would
     * throw away good buffered frames and force a slow rebuffer - the "LADA BUFFER"
     * freeze the user sees when nudging the window. So on a forward jump we only treat it
     * as a seek if the buffer does NOT already reach the new pts; otherwise we fall
     * through and let the normal drop-stale-then-match logic below skip the gap. */
    /* A seek is pending and the position has settled: re-open once, at the final target.
     * This coalesces a whole seek-bar scrub into a single sidecar re-open (see
     * LADA_SEEK_DEBOUNCE_MS) instead of one per intermediate position. */
    if (L.seek_pending &&
        (int)(SDL_GetTicks() - L.seek_pending_at) >= LADA_SEEK_DEBOUNCE_MS) {
        char cmd[64];
        /* Restore from the seek point itself, NOT seek_pos + LEAD. The lead is right for
         * the initial open (show a little original while the restorer spins up), but wrong
         * for a seek: re-opening the sidecar takes several seconds, during which - with the
         * gate disengaged - playback races far past seek_pos+LEAD, so the restorer's output
         * lands behind the display and is wasted, and it must then re-restore everything the
         * viewer already skimmed (the multi-second "BUFFER" after a seek). Instead we target
         * seek_pos and immediately engage the buffer gate to HOLD playback right here until
         * restoration is ready, then resume with a cushion - no runaway, no wasted work. */
        double target = L.seek_pending_pts;
        L.gen++;
        fifo_flush();
        lada_frame_free(L.held); L.held = NULL;
        snprintf(cmd, sizeof(cmd), "SEEK\t%.6f\t%u\n", target, L.gen);
        send_cmd(cmd);
        L.seek_pending = 0;
        L.warmed    = 1;   /* engage the gate now... */
        L.buffering = 1;   /* ...and hold playback at the seek point until it refills */
        L.buffer_best  = 0;             /* arm the stall watchdog for this fresh pause, */
        L.buffer_since = SDL_GetTicks(); /* else a stale timestamp fires it immediately */
    }

    if (!isnan(L.last_pts)) {
        int backward = pts_sec < L.last_pts - tol * 2.0;
        int big_fwd  = pts_sec > L.last_pts + fwd;
        int covered  = 0;
        if (big_fwd && !backward)
            for (f = L.head; f; f = f->next)
                if (f->gen == L.gen && f->pts >= pts_sec - tol) { covered = 1; break; }
        if (backward || (big_fwd && !covered)) {
            /* Record (or re-arm) the pending seek rather than acting now; the fire block
             * above sends the SEEK once the position stops moving. Meanwhile keep showing
             * the original frame and don't pause-to-buffer. */
            L.seek_pending     = 1;
            L.seek_pending_pts = pts_sec;
            L.seek_pending_at  = SDL_GetTicks();
            L.warmed    = 0;
            L.buffering = 0;
            L.last_pts  = pts_sec;
            SDL_UnlockMutex(L.mtx);
            return 0;
        }
    }
    L.last_pts = pts_sec;

    /* Repaint of the same frame we already handed out. */
    if (L.held && L.held->gen == L.gen && fabs(L.held->pts - pts_sec) <= tol) {
        *rgb = L.held->rgb; *w = L.held->w; *h = L.held->h;
        SDL_UnlockMutex(L.mtx);
        return 1;
    }
    lada_frame_free(L.held); L.held = NULL;

    /* Drop pre-seek leftovers and frames that fell behind the display. */
    while ((f = L.head)) {
        if (f->gen != L.gen)          { lada_frame_free(fifo_pop()); continue; }
        if (f->pts < pts_sec - tol)   { lada_frame_free(fifo_pop()); continue; }
        break;
    }

    if (f && f->gen == L.gen && fabs(f->pts - pts_sec) <= tol) {
        L.held = fifo_pop();
        *rgb = L.held->rgb; *w = L.held->w; *h = L.held->h;
        L.warmed = 1;         /* restoration is applied at this position - arm the buffer gate */
        if (!dbg_applying) {
            dbg_applying = 1;
            if (!L.announced) {   /* announce only the first time it kicks in per enable */
                char tmsg[24];
                L.announced = 1;
                snprintf(tmsg, sizeof(tmsg), "%s ACTIVE", lada_engine_name());
                lada_set_toast(tmsg);
            }
            av_log(NULL, AV_LOG_INFO, "lada: restored frames now applied\n");
        }
        SDL_UnlockMutex(L.mtx);
        return 1;
    }

    if (dbg_applying) {   /* fell back to original after having applied restoration */
        dbg_applying = 0;
        av_log(NULL, AV_LOG_VERBOSE, "lada: underrun at pts=%.3f (q=%d head=%.3f) - showing original\n",
               pts_sec, L.count, L.head ? L.head->pts : -1.0);
    }

    SDL_UnlockMutex(L.mtx);
    return 0;   /* restored frame not ready yet -> caller shows the original */
}

#else /* !_WIN32 */

int  lada_default_on(void) { return 0; }
int  lada_start(const char *input_path, const char *lada_home, const char *device)
{ (void)input_path; (void)lada_home; (void)device; return -1; }
void lada_stop(void) {}
void lada_set_enabled(int on) { (void)on; }
int  lada_active(void) { return 0; }
void lada_notify_seek(void) {}
void lada_set_jasna(const char *jasna_home) { (void)jasna_home; }
void lada_set_engine(int use_jasna, const char *jasna_home) { (void)use_jasna; (void)jasna_home; }
int  lada_is_jasna(void) { return 0; }
const char *lada_engine_name(void) { return "LADA"; }
const char *lada_status(void) { return "OFF"; }
int  lada_should_buffer(double display_pts) { (void)display_pts; return 0; }
int  lada_poll_toast(char *buf, int buflen) { (void)buf; (void)buflen; return 0; }
int  lada_frame_for(double pts_sec, double dur_sec, const uint8_t **rgb, int *w, int *h)
{ (void)pts_sec; (void)dur_sec; (void)rgb; (void)w; (void)h; return 0; }

#endif /* _WIN32 */
