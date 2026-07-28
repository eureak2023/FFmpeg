/*
 * On-screen player controls (seek bar + control bar) for ffplay.
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
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#include "ffplay_fsr.h"
#include "ffplay_ui.h"
#include "ffplay_thumb.h"
#include "ffplay_res.h"

#define BAR_H          48       /* control bar height, px */
#define SEEK_H         28       /* seek row hit-area height, px (taller = easier to click) */
#define BTN_W          52       /* control button width, px */
#define SEEK_PAD       10       /* seek track horizontal padding, px */
#define TITLE_H        34       /* title bar height, px */
#define WBTN_W         48       /* title-bar window button width, px */
#define EDGE           6        /* resize border thickness, px */
#define HIDE_DELAY_US  2500000  /* idle time before the overlay hides */
#define DRAG_SEEK_US   300000   /* min interval between drag-seeks */
#define WDRAG_THRESH   4        /* px of motion before a click becomes a drag */

enum {
    EL_NONE = 0,
    EL_SEEK,       /* seek track */
    EL_VOL,        /* volume slider */
    EL_PAUSE,
    EL_STOP,
    EL_BACK,
    EL_FWD,
    EL_OPEN,       /* open file */
    EL_BAR,        /* control bar background (dead area) */
    EL_TITLE,      /* title bar drag area */
    EL_MIN,
    EL_MAX,
    EL_CLOSE,
};

#define VOL_AREA   150  /* right end of the seek row: icon + slider */
#define VOL_TRACK  100  /* slider track width */
#define MAX_CHAPTERS 128
#define NBADGE 5
#define BADGE_HW  0     /* highlighted (PotPlayer yellow) */
#define BADGE_HDR 2     /* highlighted unless it reads "SDR" */

static struct {
    int64_t last_activity;
    int     hover;         /* element under the cursor */
    int     dragging;      /* dragging the seek handle */
    double  drag_frac;
    double  last_seek_frac; /* frac of the last emitted SEEK_FRAC (dedup clicks) */
    int64_t last_drag_seek;
    int     win_w, win_h;

    SDL_Window *window;
    char    title[512];    /* UTF-8 basename */
    /* window-move drag (grab the video or the title bar) */
    int     wdrag_armed, wdrag_moving;
    int     wdrag_mouse_x, wdrag_mouse_y; /* global, at buttondown */
    int     wdrag_win_x, wdrag_win_y;

    /* volume slider */
    int     vol_dragging;

    int     mouse_x, mouse_y; /* last cursor position in the window */

    /* stream info badges: [0] highlighted (H/W), [1] vcodec, [2] acodec,
     * [3] channels. Set once from the open threads, rendered lazily on the
     * main thread. */
    char         badge_str[NBADGE][16];
    SDL_Texture *badge_tex[NBADGE];
    int          badge_w[NBADGE], badge_h[NBADGE];
    int          badges_dirty;

    /* chapter start positions as fractions of the duration */
    double  chapters[MAX_CHAPTERS];
    int     nb_chapters;

    SDL_Texture *text_tex; /* rendered time string */
    int          text_w, text_h;
    char         text_str[64];

    /* seek-bar hover thumbnail preview */
    SDL_Texture *thumb_tex;
    int          thumb_tw, thumb_th;
    int64_t      thumb_gen;
    char         thumb_label[16];
    SDL_Texture *thumb_label_tex;
    int          thumb_label_w, thumb_label_h;
    SDL_Texture *title_tex;
    int          title_w, title_h, title_scale;
    SDL_Renderer *renderer;

    int last_drawn_visible;
    int last_drawn_hover;
} ui;

/* Register an embedded TTF (RCDATA) with GDI - process-private, no install and
 * no admin rights. Returns 1 if it became available under its family name. */
#ifdef _WIN32
static int ui_register_font(int resid)
{
    HMODULE mod = GetModuleHandleW(NULL);
    HRSRC   res = FindResourceW(mod, MAKEINTRESOURCEW(resid), (LPCWSTR)RT_RCDATA);
    HGLOBAL h;
    DWORD   sz, n = 0;
    void   *p;

    if (!res)
        return 0;
    h  = LoadResource(mod, res);
    sz = SizeofResource(mod, res);
    p  = h ? LockResource(h) : NULL;
    return p && sz && AddFontMemResourceEx(p, sz, NULL, &n) && n;
}

/* Default UI font (title bar, badges, thumbnails, HUD): bundled Noto Sans KR if
 * it registered, else the system Malgun Gothic. */
static const wchar_t *ui_font_name(void)
{
    static const wchar_t *name = L"Malgun Gothic";
    static int tried;

    if (!tried) {
        tried = 1;
        if (ui_register_font(IDR_FONT_NOTOSANS))
            name = L"Noto Sans KR";
    }
    return name;
}

#else
static const wchar_t *ui_font_name(void) { return NULL; }
#endif

/* Rasterize UTF-8 text with the bundled font via GDI (handles Korean and
 * everything else the pixel font cannot). White glyphs, alpha from
 * coverage. Returns NULL on failure (caller falls back to the pixel font). */
static SDL_Texture *render_text_sys(SDL_Renderer *r, const char *utf8,
                                    int px_h, int *out_w, int *out_h,
                                    int multiline, const wchar_t *face)
{
#ifdef _WIN32
    const UINT dtflags = (multiline ? 0u : (UINT)DT_SINGLELINE) | DT_NOPREFIX;
    wchar_t wbuf[512];
    BITMAPINFO bmi = { 0 };
    SDL_Texture *tex = NULL;
    uint32_t *bits = NULL, *px = NULL;
    HDC dc = NULL;
    HFONT font = NULL, old_font = NULL;
    HBITMAP bmp = NULL, old_bmp = NULL;
    RECT rc = { 0, 0, 0, 0 };
    int tw, th;

    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf,
                             (int)(sizeof(wbuf) / sizeof(*wbuf))))
        return NULL;
    dc = CreateCompatibleDC(NULL);
    if (!dc)
        return NULL;
    font = CreateFontW(-px_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       face ? face : ui_font_name());
    if (!font)
        goto out;
    old_font = (HFONT)SelectObject(dc, font);
    DrawTextW(dc, wbuf, -1, &rc, DT_CALCRECT | dtflags);
    tw = rc.right;
    th = rc.bottom;
    if (tw <= 0 || th <= 0)
        goto out;

    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = tw;
    bmi.bmiHeader.biHeight      = -th; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp)
        goto out;
    old_bmp = (HBITMAP)SelectObject(dc, bmp);
    memset(bits, 0, (size_t)tw * th * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, wbuf, -1, &rc, dtflags);
    GdiFlush();

    px = SDL_malloc((size_t)tw * th * 4);
    if (!px)
        goto out;
    for (int i = 0; i < tw * th; i++)
        px[i] = ((bits[i] & 0xFF) << 24) | 0x00E8E8E8; /* coverage -> alpha */

    tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STATIC, tw, th);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, px, tw * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        *out_w = tw;
        *out_h = th;
    }
out:
    SDL_free(px);
    if (old_bmp)
        SelectObject(dc, old_bmp);
    if (bmp)
        DeleteObject(bmp);
    if (old_font)
        SelectObject(dc, old_font);
    if (font)
        DeleteObject(font);
    if (dc)
        DeleteDC(dc);
    return tex;
#else
    return NULL;
#endif
}

/* Public wrapper so other modules (the TAB status HUD in ffplay_fsr.c) can
 * use the same system font as the subtitles instead of the built-in pixel
 * font, which only carries a handful of letters and silently drops the rest.
 * Multi-line: embedded newlines are laid out by GDI. NULL on failure. */
SDL_Texture *ui_render_text(SDL_Renderer *r, const char *utf8, int px_h,
                            int *out_w, int *out_h)
{
    return render_text_sys(r, utf8, px_h, out_w, out_h, 1, NULL);
}

/* ---- text subtitles (SRT/SMI/ASS events rendered with the system font) */
#define SUB_MAX_LINES 4

typedef struct SubEvent {
    double start, end;
    char  *text;
} SubEvent;

static struct {
    SDL_mutex *lock;
    SubEvent  *ev;
    int        n, cap;
    /* draw cache */
    char         cur[1024];
    SDL_Texture *line_tex[SUB_MAX_LINES];
    int          line_w[SUB_MAX_LINES], line_h[SUB_MAX_LINES];
    int          nlines, font_px;
} subs;

/* Append Unicode code point cp to out (UTF-8), if it fits. */
static size_t utf8_put(char *out, size_t o, size_t outsz, unsigned cp)
{
    if (cp < 0x80) {
        if (o + 1 < outsz) out[o++] = (char)cp;
    } else if (cp < 0x800) {
        if (o + 2 < outsz) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (o + 3 < outsz) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0x10FFFF) {
        if (o + 4 < outsz) {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

/* Replace HTML/XML character references common in SRT/SAMI subtitles with the
 * characters they stand for (&nbsp; &amp; &#233; &#xE9; ...). Without this the
 * raw entity text shows on screen; a lone &nbsp; spacer line is the usual
 * culprit. Unknown entities are left as-is. */
static void sub_decode_entities(const char *in, char *out, size_t outsz)
{
    static const struct { const char *name; unsigned cp; } ents[] = {
        { "nbsp;",   0x20   }, { "amp;",    '&'    }, { "lt;",     '<'    },
        { "gt;",     '>'    }, { "quot;",   '"'    }, { "apos;",   '\''   },
        { "mdash;",  0x2014 }, { "ndash;",  0x2013 }, { "hellip;", 0x2026 },
        { "lsquo;",  0x2018 }, { "rsquo;",  0x2019 }, { "ldquo;",  0x201C },
        { "rdquo;",  0x201D },
    };
    size_t o = 0;

    while (*in && o + 4 < outsz) {
        if (*in == '&') {
            const char *s = in + 1;
            unsigned cp = 0;
            int ok = 0;

            if (*s == '#') {                       /* numeric reference */
                s++;
                if (*s == 'x' || *s == 'X') {
                    s++;
                    while ((*s >= '0' && *s <= '9') ||
                           (*s >= 'a' && *s <= 'f') ||
                           (*s >= 'A' && *s <= 'F')) {
                        int d = *s <= '9' ? *s - '0'
                              : (*s | 0x20) - 'a' + 10;
                        cp = cp * 16 + d; s++; ok = 1;
                    }
                } else {
                    while (*s >= '0' && *s <= '9') { cp = cp * 10 + (*s - '0'); s++; ok = 1; }
                }
                if (ok && *s == ';') {
                    o = utf8_put(out, o, outsz, cp);
                    in = s + 1;
                    continue;
                }
            } else {                               /* named reference */
                for (size_t i = 0; i < FF_ARRAY_ELEMS(ents); i++) {
                    size_t len = strlen(ents[i].name);
                    if (!strncmp(s, ents[i].name, len)) {
                        o = utf8_put(out, o, outsz, ents[i].cp);
                        in = s + len;
                        ok = 1;
                        break;
                    }
                }
                if (ok)
                    continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

void ui_sub_add(double start, double end, const char *text)
{
    char decoded[1024];

    if (!subs.lock || !text || !text[0])
        return;
    sub_decode_entities(text, decoded, sizeof(decoded));
    text = decoded;
    /* A line that decoded to nothing but spaces (a &nbsp; spacer) would draw an
     * empty caption box; treat it as blank. */
    {
        const char *t = text;
        while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r')
            t++;
        if (!*t)
            return;
    }
    SDL_LockMutex(subs.lock);
    for (int i = subs.n - 1; i >= 0; i--)  /* dedup (seek re-decodes) */
        if (subs.ev[i].start == start && !strcmp(subs.ev[i].text, text)) {
            SDL_UnlockMutex(subs.lock);
            return;
        }
    if (subs.n == subs.cap) {
        int ncap = subs.cap ? subs.cap * 2 : 256;
        SubEvent *nev = av_realloc_array(subs.ev, ncap, sizeof(*nev));

        if (!nev) {
            SDL_UnlockMutex(subs.lock);
            return;
        }
        subs.ev  = nev;
        subs.cap = ncap;
    }
    subs.ev[subs.n].start = start;
    subs.ev[subs.n].end   = end;
    subs.ev[subs.n].text  = av_strdup(text);
    if (subs.ev[subs.n].text)
        subs.n++;
    SDL_UnlockMutex(subs.lock);
}

/* Add a decoded ASS event line ("ReadOrder,Layer,Style,...,Text"): keep the
 * text field, drop {\override} blocks, convert \N to newlines. */
void ui_sub_add_ass(double start, double end, const char *ass)
{
    char out[1024];
    const char *p = ass;
    int commas = 0, o = 0;

    while (*p && commas < 8)   /* skip to the 9th field (the text) */
        if (*p++ == ',')
            commas++;
    if (commas < 8)
        p = ass;               /* not an ASS line: take it as plain text */
    while (*p && o < (int)sizeof(out) - 1) {
        if (*p == '{') {       /* {\...} style override */
            const char *q = strchr(p, '}');

            if (q) {
                p = q + 1;
                continue;
            }
        }
        if (p[0] == '\\' && (p[1] == 'N' || p[1] == 'n')) {
            out[o++] = '\n';
            p += 2;
            continue;
        }
        if (p[0] == '\\' && p[1] == 'h') {
            out[o++] = ' ';
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    ui_sub_add(start, end, out);
}

static void sub_drop_cache(void)
{
    for (int i = 0; i < SUB_MAX_LINES; i++)
        if (subs.line_tex[i]) {
            SDL_DestroyTexture(subs.line_tex[i]);
            subs.line_tex[i] = NULL;
        }
    subs.nlines = 0;
    subs.cur[0] = 0;
}

/* Main thread only. */
void ui_sub_clear(void)
{
    if (!subs.lock)
        return;
    SDL_LockMutex(subs.lock);
    for (int i = 0; i < subs.n; i++)
        av_free(subs.ev[i].text);
    subs.n = 0;
    SDL_UnlockMutex(subs.lock);
    sub_drop_cache();
}

void ui_sub_draw(SDL_Renderer *renderer, int win_w, int win_h, double now)
{
    char text[1024];
    int px, y;

    if (!subs.lock || !subs.n)
        return;
    text[0] = 0;
    SDL_LockMutex(subs.lock);
    {
        /* Latest-starting active cue wins: cues with an open/huge end time
         * (common in SAMI) are superseded by the next one. */
        double best = -1.0;

        for (int i = 0; i < subs.n; i++)
            if (now >= subs.ev[i].start && now < subs.ev[i].end &&
                subs.ev[i].start > best) {
                best = subs.ev[i].start;
                snprintf(text, sizeof(text), "%s", subs.ev[i].text);
            }
    }
    SDL_UnlockMutex(subs.lock);

    /* Size and place the subtitle against the actual render canvas, not the
     * passed window size: the latter comes from window events in logical points
     * and can lag or mismatch the physical drawable (High-DPI, fullscreen), so
     * the text ended up small on fullscreen. */
    {
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
        if (ow > 0 && oh > 0) {
            win_w = ow;
            win_h = oh;
        }
    }

    px = win_h * 49 / 1600;               /* ~49% of win_h/16 (70% of former) */
    px = px < 9 ? 9 : px > 78 ? 78 : px;
    if (strcmp(text, subs.cur) || px != subs.font_px) {
        char *line, *save = NULL;

        sub_drop_cache();
        snprintf(subs.cur, sizeof(subs.cur), "%s", text);
        subs.font_px = px;
        line = av_strtok(text, "\n", &save);
        while (line && subs.nlines < SUB_MAX_LINES) {
            if (line[0]) {
                int li = subs.nlines;

                subs.line_tex[li] = render_text_sys(renderer, line, px,
                                                    &subs.line_w[li],
                                                    &subs.line_h[li], 0, NULL);
                if (subs.line_tex[li])
                    subs.nlines++;
            }
            line = av_strtok(NULL, "\n", &save);
        }
    }
    if (!subs.nlines)
        return;

    y = win_h - win_h / 18;
    for (int i = subs.nlines - 1; i >= 0; i--) {
        SDL_Texture *t = subs.line_tex[i];
        SDL_Rect dst;

        y -= subs.line_h[i];
        dst.x = (win_w - subs.line_w[i]) / 2;
        dst.y = y;
        dst.w = subs.line_w[i];
        dst.h = subs.line_h[i];
        /* outline: the same texture tinted black around, then yellow on top */
        SDL_SetTextureColorMod(t, 0, 0, 0);
        for (int dy = -2; dy <= 2; dy += 2)
            for (int dx = -2; dx <= 2; dx += 2) {
                SDL_Rect od = { dst.x + dx, dst.y + dy, dst.w, dst.h };

                if (dx || dy)
                    SDL_RenderCopy(renderer, t, NULL, &od);
            }
        SDL_SetTextureColorMod(t, 255, 235, 0);
        SDL_RenderCopy(renderer, t, NULL, &dst);
        y -= 4;
    }
}

void ui_init(SDL_Renderer *renderer)
{
    ui.renderer = renderer;
    ui.last_activity = av_gettime_relative();
    subs.lock = SDL_CreateMutex();
    /* Deliver the click that focuses the window too, so grabbing an
     * unfocused player and dragging it works in one motion. */
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
}

static int ui_fullscreen(void)
{
    return ui.window &&
           (SDL_GetWindowFlags(ui.window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
}

/* Resize borders for the borderless window; everything else stays normal so
 * regular mouse events keep flowing. */
static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *p, void *data)
{
    int w, h, l, r, t, b;

    if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)
        return SDL_HITTEST_NORMAL;
    SDL_GetWindowSize(win, &w, &h);
    l = p->x < EDGE;
    r = p->x >= w - EDGE;
    t = p->y < EDGE;
    b = p->y >= h - EDGE;
    if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (t)      return SDL_HITTEST_RESIZE_TOP;
    if (b)      return SDL_HITTEST_RESIZE_BOTTOM;
    if (l)      return SDL_HITTEST_RESIZE_LEFT;
    if (r)      return SDL_HITTEST_RESIZE_RIGHT;
    return SDL_HITTEST_NORMAL;
}

void ui_set_window(SDL_Window *win, const char *title)
{
    const char *base = title, *p;

    ui.window = win;
    for (p = title; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    snprintf(ui.title, sizeof(ui.title), "%s", base);
    if (ui.title_tex) { /* re-render on the next draw (file switched) */
        SDL_DestroyTexture(ui.title_tex);
        ui.title_tex = NULL;
    }
    for (int i = 0; i < NBADGE; i++)
        ui.badge_str[i][0] = 0;
    ui.badges_dirty = 1;
    ui.nb_chapters  = 0;
    ui_sub_clear();
    SDL_SetWindowHitTest(win, hit_test, NULL);
}

void ui_ping(void)
{
    ui.last_activity = av_gettime_relative();
}

/* Right-click context menu (blocks until dismissed). Owned by the real,
 * already-foreground player window and run on the calling (main) thread, so
 * it displays and takes input reliably. To keep playback going while the
 * menu's modal loop runs, the player window is temporarily subclassed and a
 * timer drives on_idle (which presents a frame) roughly every 15ms.
 * Returns the chosen UI_MENU_* command, or 0. */
#ifdef _WIN32
#define MENU_TIMER_ID 0xF5A1
static WNDPROC     menu_prev_wndproc;
static void      (*menu_idle_fn)(void *);
static void       *menu_idle_ctx;

static LRESULT CALLBACK menu_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Both fire while the menu's modal loop is idle: the timer keeps waking
     * the loop, WM_ENTERIDLE / WM_TIMER let us present a frame. */
    if ((msg == WM_TIMER && wp == MENU_TIMER_ID) || msg == WM_ENTERIDLE) {
        if (menu_idle_fn)
            menu_idle_fn(menu_idle_ctx); /* present a video frame */
        if (msg == WM_TIMER)
            return 0;
    }
    return CallWindowProcW(menu_prev_wndproc, h, msg, wp, lp);
}
#endif

int ui_context_menu(int fsr_on, int nr_on, int fg_on, int fg_mult, int stereo_on,
                    int scale_mode, int sub_on, const UIAudioTracks *atracks,
                    void (*on_idle)(void *), void *idle_ctx)
{
#ifdef _WIN32
    SDL_SysWMinfo wm;
    HWND owner = NULL;
    HMENU menu, fx, ao, sc, su, fm, at = NULL;
    POINT pt;
    int cmd = 0;

    SDL_VERSION(&wm.version);
    if (ui.window && SDL_GetWindowWMInfo(ui.window, &wm))
        owner = wm.info.win.window;
    menu = CreatePopupMenu();
    fx   = CreatePopupMenu();
    ao   = CreatePopupMenu();
    sc   = CreatePopupMenu();
    su   = CreatePopupMenu();
    fm   = CreatePopupMenu();
    if (owner && menu && fx && ao && sc && su && fm) {
        AppendMenuW(menu, MF_STRING, UI_MENU_OPEN, L"파일 열기(&O)...");
        AppendMenuW(fx, MF_STRING | (fsr_on ? MF_CHECKED : 0),
                    UI_MENU_FSR, L"FSR 업스케일");
        AppendMenuW(fx, MF_STRING | (nr_on ? MF_CHECKED : 0),
                    UI_MENU_NR, L"NR 노이즈 제거");
        AppendMenuW(fx, MF_STRING | (fg_on ? MF_CHECKED : 0),
                    UI_MENU_FG, L"FG 프레임 생성");
        AppendMenuW(fm, MF_STRING | (fg_mult == 2 ? MF_CHECKED : 0),
                    UI_MENU_FGMULT_2X, L"2X");
        AppendMenuW(fm, MF_STRING | (fg_mult == 3 ? MF_CHECKED : 0),
                    UI_MENU_FGMULT_3X, L"3X");
        AppendMenuW(fm, MF_STRING | (fg_mult == 4 ? MF_CHECKED : 0),
                    UI_MENU_FGMULT_4X, L"4X");
        /* Greyed out unless FG is on -- the multiplier only matters then. */
        AppendMenuW(fx, MF_POPUP | (fg_on ? 0 : MF_GRAYED), (UINT_PTR)fm,
                    L"FG 프레임 생성 배수");
        AppendMenuW(sc, MF_STRING | (scale_mode == 0 ? MF_CHECKED : 0),
                    UI_MENU_SCALE_FIT, L"비율 유지");
        AppendMenuW(sc, MF_STRING | (scale_mode == 1 ? MF_CHECKED : 0),
                    UI_MENU_SCALE_FILL, L"비율 유지 꽉찬 화면");
        AppendMenuW(sc, MF_STRING | (scale_mode == 2 ? MF_CHECKED : 0),
                    UI_MENU_SCALE_STRETCH, L"비율 유지하지 않음");
        AppendMenuW(fx, MF_SEPARATOR, 0, NULL);
        AppendMenuW(fx, MF_POPUP, (UINT_PTR)sc, L"화면 비율(&R)");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)fx, L"영상 효과(&E)");
        AppendMenuW(ao, MF_STRING | (!stereo_on ? MF_CHECKED : 0),
                    UI_MENU_AOUT_ORIG, L"원본 그대로 출력");
        AppendMenuW(ao, MF_STRING | (stereo_on ? MF_CHECKED : 0),
                    UI_MENU_AOUT_STEREO, L"2.0 스테레오");
        /* Only worth showing when the file actually has something to choose
         * between; a single-track file gets the plain 소리 출력 menu. */
        if (atracks && atracks->nb > 1 && (at = CreatePopupMenu())) {
            int n = FFMIN(atracks->nb, UI_MAX_ATRACKS);

            for (int i = 0; i < n; i++) {
                wchar_t w[128];

                if (!MultiByteToWideChar(CP_UTF8, 0, atracks->name[i], -1,
                                         w, FF_ARRAY_ELEMS(w)))
                    continue;
                AppendMenuW(at, MF_STRING | (i == atracks->cur ? MF_CHECKED : 0),
                            UI_MENU_ATRACK_BASE + i, w);
            }
            AppendMenuW(ao, MF_SEPARATOR, 0, NULL);
            AppendMenuW(ao, MF_POPUP, (UINT_PTR)at, L"소리 선택(&T)");
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)ao, L"소리 출력(&A)");
        AppendMenuW(su, MF_STRING | (sub_on ? MF_CHECKED : 0),
                    UI_MENU_SUB_SHOW, L"자막 보이기");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)su, L"자막(&S)");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, UI_MENU_CLOSE, L"닫기(&X)");
        GetCursorPos(&pt);
        /* Keep painting the video during the menu's modal loop. */
        menu_idle_fn  = on_idle;
        menu_idle_ctx = idle_ctx;
        if (on_idle) {
            menu_prev_wndproc = (WNDPROC)SetWindowLongPtrW(owner, GWLP_WNDPROC,
                                    (LONG_PTR)menu_wndproc);
            SetTimer(owner, MENU_TIMER_ID, 15, NULL);
        }
        SetForegroundWindow(owner);
        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, owner, NULL);
        if (on_idle) {
            KillTimer(owner, MENU_TIMER_ID);
            SetWindowLongPtrW(owner, GWLP_WNDPROC, (LONG_PTR)menu_prev_wndproc);
        }
        menu_idle_fn = NULL;
    }
    if (menu) {
        DestroyMenu(menu); /* also destroys attached submenus */
    } else {                /* not attached: free them individually */
        if (fx)
            DestroyMenu(fx);
        if (ao)
            DestroyMenu(ao);
        if (sc)
            DestroyMenu(sc);
        if (su)
            DestroyMenu(su);
        if (fm)
            DestroyMenu(fm);
    }
    return cmd;
#else
    return 0;
#endif
}

/* Native "open file" dialog; returns an av_strdup'ed UTF-8 path or NULL. */
char *ui_open_file_dialog(void)
{
#ifdef _WIN32
    typedef BOOL (WINAPI *gofn_fn)(LPOPENFILENAMEW);
    static const wchar_t filter[] =
        L"Media files\0*.mp4;*.mkv;*.avi;*.webm;*.mov;*.ts;*.m2ts;*.wmv;"
        L"*.flv;*.mpg;*.vob;*.mp3;*.flac;*.aac;*.m4a;*.wav;*.ogg\0"
        L"All files\0*.*\0";
    HMODULE dlg = LoadLibraryA("comdlg32.dll");
    gofn_fn gofn;
    OPENFILENAMEW ofn = { 0 };
    wchar_t path[MAX_PATH] = L"";
    char utf8[MAX_PATH * 3];
    SDL_SysWMinfo wm;
    HWND owner = NULL;
    void *raise;
    BOOL ok;

    if (!dlg)
        return NULL;
    gofn = (gofn_fn)GetProcAddress(dlg, "GetOpenFileNameW");
    if (!gofn)
        return NULL;
    ofn.lStructSize = sizeof(ofn);
    SDL_VERSION(&wm.version);
    if (ui.window && SDL_GetWindowWMInfo(ui.window, &wm))
        owner = wm.info.win.window;
    ofn.hwndOwner   = owner;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    /* Keep the player fullscreen, but lift the dialog above it (gofn() blocks,
     * so a helper thread does the raising — see fsr_raise_modal_begin). */
    raise = fsr_raise_modal_begin(owner);
    ok = gofn(&ofn);
    fsr_raise_modal_end(raise);

    if (!ok)
        return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8),
                             NULL, NULL))
        return NULL;
    return av_strdup(utf8);
#else
    return NULL;
#endif
}

void ui_set_badges(const char *hw, const char *vcodec, const char *hdr,
                   const char *acodec, const char *chans)
{
    const char *s[NBADGE] = { hw, vcodec, hdr, acodec, chans };

    for (int i = 0; i < NBADGE; i++)
        if (s[i])
            snprintf(ui.badge_str[i], sizeof(ui.badge_str[i]), "%s", s[i]);
    ui.badges_dirty = 1;
}

void ui_set_chapters(const double *fracs, int n)
{
    if (n > MAX_CHAPTERS)
        n = MAX_CHAPTERS;
    memcpy(ui.chapters, fracs, n * sizeof(*fracs));
    ui.nb_chapters = n;
}

void ui_uninit(void)
{
    if (ui.text_tex) {
        SDL_DestroyTexture(ui.text_tex);
        ui.text_tex = NULL;
    }
    if (ui.title_tex) {
        SDL_DestroyTexture(ui.title_tex);
        ui.title_tex = NULL;
    }
    if (ui.thumb_tex) {
        SDL_DestroyTexture(ui.thumb_tex);
        ui.thumb_tex = NULL;
    }
    if (ui.thumb_label_tex) {
        SDL_DestroyTexture(ui.thumb_label_tex);
        ui.thumb_label_tex = NULL;
    }
    for (int i = 0; i < NBADGE; i++)
        if (ui.badge_tex[i]) {
            SDL_DestroyTexture(ui.badge_tex[i]);
            ui.badge_tex[i] = NULL;
        }
    ui_sub_clear();
    if (subs.lock) {
        SDL_DestroyMutex(subs.lock);
        subs.lock = NULL;
    }
    av_freep(&subs.ev);
    subs.cap = 0;
    if (ui.window)
        SDL_SetWindowHitTest(ui.window, NULL, NULL);
    ui.window   = NULL;
    ui.renderer = NULL;
}

static int ui_visible_now(void)
{
    if (ui.dragging || ui.hover != EL_NONE)
        return 1;
    return av_gettime_relative() - ui.last_activity < HIDE_DELAY_US;
}

/* Which element sits at window coordinates (x, y)? */
static int element_at(int x, int y)
{
    int bar_top  = ui.win_h - BAR_H;
    int seek_top = bar_top - SEEK_H;

    if (y < TITLE_H) {
        int from_right = (ui.win_w - x) / WBTN_W;

        switch (from_right) {
        case 0:  return EL_CLOSE;
        case 1:  return EL_MAX;
        case 2:  return EL_MIN;
        default: return EL_TITLE;
        }
    }
    if (y >= seek_top && y < bar_top)
        return x >= ui.win_w - VOL_AREA ? EL_VOL : EL_SEEK;
    if (y >= bar_top && y < ui.win_h) {
        int i = x / BTN_W;

        switch (i) {
        case 0:  return EL_PAUSE;
        case 1:  return EL_STOP;
        case 2:  return EL_BACK;
        case 3:  return EL_FWD;
        case 4:  return EL_OPEN;
        default: return EL_BAR;
        }
    }
    return EL_NONE;
}

/* Start tracking a potential window-move drag from a left-button press. */
static void wdrag_arm(void)
{
    /* No grab-drag while fullscreen or maximized: a maximized window sits at
     * a negative origin, so moving it would un-maximize and jump it to a
     * weird position under the cursor. */
    if (!ui.window || ui_fullscreen() ||
        (SDL_GetWindowFlags(ui.window) & SDL_WINDOW_MAXIMIZED))
        return;
    ui.wdrag_armed  = 1;
    ui.wdrag_moving = 0;
    SDL_GetGlobalMouseState(&ui.wdrag_mouse_x, &ui.wdrag_mouse_y);
    SDL_GetWindowPosition(ui.window, &ui.wdrag_win_x, &ui.wdrag_win_y);
    /* keep receiving motion when the cursor outruns the window */
    SDL_CaptureMouse(SDL_TRUE);
}

/* Returns 1 while a window drag is in progress (event consumed). */
static int wdrag_motion(void)
{
    int mx, my, dx, dy;

    if (!ui.wdrag_armed)
        return 0;
    SDL_GetGlobalMouseState(&mx, &my);
    dx = mx - ui.wdrag_mouse_x;
    dy = my - ui.wdrag_mouse_y;
    if (!ui.wdrag_moving &&
        (dx > WDRAG_THRESH || dx < -WDRAG_THRESH ||
         dy > WDRAG_THRESH || dy < -WDRAG_THRESH))
        ui.wdrag_moving = 1;
    if (ui.wdrag_moving) {
        SDL_SetWindowPosition(ui.window, ui.wdrag_win_x + dx,
                              ui.wdrag_win_y + dy);
        return 1;
    }
    return 0;
}

static int seek_track_w(void)
{
    return ui.win_w - 2 * SEEK_PAD - VOL_AREA;
}

static double seek_frac_at(int x)
{
    double f = (double)(x - SEEK_PAD) / seek_track_w();

    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
}

static double vol_frac_at(int x)
{
    int x0 = ui.win_w - VOL_TRACK - 12;
    double f = (double)(x - x0) / VOL_TRACK;

    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
}

int ui_handle_event(const SDL_Event *event, int win_w, int win_h,
                    double *seek_frac)
{
    int x, y;

    ui.win_w = win_w;
    ui.win_h = win_h;

    switch (event->type) {
    case SDL_MOUSEMOTION:
        x = event->motion.x;
        y = event->motion.y;
        ui.mouse_x = x;
        ui.mouse_y = y;
        ui.last_activity = av_gettime_relative();
        ui.hover = element_at(x, y);
        if (ui.dragging) {
            int64_t now = av_gettime_relative();

            ui.drag_frac = seek_frac_at(x);
            if (now - ui.last_drag_seek >= DRAG_SEEK_US) {
                ui.last_drag_seek = now;
                ui.last_seek_frac = ui.drag_frac;
                *seek_frac = ui.drag_frac;
                return UI_ACT_SEEK_FRAC;
            }
            return UI_ACT_CONSUMED;
        }
        if (ui.vol_dragging) {
            *seek_frac = vol_frac_at(x);
            return UI_ACT_SET_VOLUME;
        }
        if ((event->motion.state & SDL_BUTTON_LMASK) && wdrag_motion())
            return UI_ACT_CONSUMED;
        return UI_ACT_NONE; /* let ffplay keep its cursor logic */
    case SDL_MOUSEBUTTONDOWN:
        if (event->button.button != SDL_BUTTON_LEFT)
            return UI_ACT_NONE;
        x = event->button.x;
        y = event->button.y;
        ui.last_activity = av_gettime_relative();
        if (!ui_visible_now()) {
            wdrag_arm(); /* grab-anywhere window drag */
            return UI_ACT_NONE; /* first click only reveals the bar */
        }
        switch (element_at(x, y)) {
        case EL_SEEK:
            ui.dragging  = 1;
            ui.drag_frac = seek_frac_at(x);
            ui.last_drag_seek = av_gettime_relative();
            ui.last_seek_frac = ui.drag_frac;
            *seek_frac = ui.drag_frac;
            return UI_ACT_SEEK_FRAC;
        case EL_VOL:
            ui.vol_dragging = 1;
            *seek_frac = vol_frac_at(x);
            return UI_ACT_SET_VOLUME;
        case EL_PAUSE: return UI_ACT_TOGGLE_PAUSE;
        case EL_STOP:  return UI_ACT_STOP;
        case EL_BACK:  return UI_ACT_SEEK_BACK;
        case EL_FWD:   return UI_ACT_SEEK_FWD;
        case EL_OPEN:  return UI_ACT_OPEN;
        case EL_BAR:   return UI_ACT_CONSUMED;
        case EL_MIN:   return UI_ACT_MINIMIZE;
        case EL_MAX:   return UI_ACT_MAXIMIZE;
        case EL_CLOSE: return UI_ACT_CLOSE;
        case EL_TITLE:
            wdrag_arm();
            return UI_ACT_CONSUMED;
        default:
            wdrag_arm(); /* video area: drag moves the window */
            return UI_ACT_NONE;
        }
    case SDL_MOUSEBUTTONUP:
        if (event->button.button != SDL_BUTTON_LEFT)
            return UI_ACT_NONE;
        if (ui.dragging) {
            double frac = seek_frac_at(event->button.x);

            ui.dragging = 0;
            /* A plain click already seeked on button-down (and drag motion
             * seeks as it moves); only issue a final seek if the release
             * landed somewhere we have not seeked to yet. Re-seeking to the
             * same spot restarts the in-flight decode and doubles the latency. */
            if (fabs(frac - ui.last_seek_frac) < 0.0015)
                return UI_ACT_CONSUMED;
            ui.last_seek_frac = frac;
            *seek_frac = frac;
            return UI_ACT_SEEK_FRAC;
        }
        if (ui.vol_dragging) {
            ui.vol_dragging = 0;
            *seek_frac = vol_frac_at(event->button.x);
            return UI_ACT_SET_VOLUME;
        }
        if (ui.wdrag_armed) {
            int moved = ui.wdrag_moving;

            ui.wdrag_armed = ui.wdrag_moving = 0;
            SDL_CaptureMouse(SDL_FALSE);
            if (moved)
                return UI_ACT_CONSUMED;
        }
        return UI_ACT_NONE;
    case SDL_WINDOWEVENT:
        /* cursor left the window: clear hover so the overlay can hide */
        if (event->window.event == SDL_WINDOWEVENT_LEAVE && !ui.dragging &&
            !ui.wdrag_armed) {
            ui.hover   = EL_NONE;
            ui.mouse_y = TITLE_H; /* also drop the fullscreen title reveal */
        }
        return UI_ACT_NONE;
    }
    return UI_ACT_NONE;
}

int ui_wants_refresh(void)
{
    int vis = ui_visible_now();

    if (vis != ui.last_drawn_visible || ui.hover != ui.last_drawn_hover)
        return 1;
    /* Keep redrawing while hovering the seek bar so an async preview frame
     * shows up even when the cursor is still and the player is paused. */
    if (vis && (ui.hover == EL_SEEK || ui.dragging) && thumb_available())
        return 1;
    return 0;
}

/* ---- drawing ---- */

static void fill(SDL_Renderer *r, int x, int y, int w, int h,
                 Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_Rect rc = { x, y, w, h };

    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_RenderFillRect(r, &rc);
}

/* Vertical gradient rectangle: alpha a_top at the top edge fading to a_bot at
 * the bottom. Used for the control-bar scrim so it reads as controls floating
 * over full-bleed video (no hard black edge) instead of a black letterbox. */
static void fill_vgrad(SDL_Renderer *r, int x, int y, int w, int h,
                       Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a_top, Uint8 a_bot)
{
    SDL_Color ct = { cr, cg, cb, a_top };
    SDL_Color cb2 = { cr, cg, cb, a_bot };
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    SDL_Vertex v[4] = {
        { { x0, y0 }, ct,  { 0, 0 } },   /* top-left */
        { { x1, y0 }, ct,  { 0, 0 } },   /* top-right */
        { { x1, y1 }, cb2, { 0, 0 } },   /* bottom-right */
        { { x0, y1 }, cb2, { 0, 0 } },   /* bottom-left */
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };

    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

static void tri(SDL_Renderer *r, float x1, float y1, float x2, float y2,
                float x3, float y3, SDL_Color c)
{
    SDL_Vertex v[3] = {
        { { x1, y1 }, c, { 0, 0 } },
        { { x2, y2 }, c, { 0, 0 } },
        { { x3, y3 }, c, { 0, 0 } },
    };

    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* Render a string into a texture (transparent background, light 8x8 pixel
 * font). Returns NULL on failure. */
static SDL_Texture *render_text(SDL_Renderer *r, const char *str,
                                int *out_w, int *out_h)
{
    static uint32_t px[63 * 8 * 8]; /* up to 63 chars of 8x8 glyphs */
    SDL_Texture *tex;
    int len = strlen(str);
    int w;

    if (len > 63)
        len = 63;
    if (!len)
        return NULL;
    w = len * 8;

    memset(px, 0, sizeof(px));
    for (int ci = 0; ci < len; ci++) {
        const uint8_t *g = fsr_glyph(str[ci]);

        if (!g)
            continue;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    px[row * w + ci * 8 + col] = 0xFFE8E8E8;
    }

    tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STATIC, w, 8);
    if (!tex)
        return NULL;
    SDL_UpdateTexture(tex, NULL, px, w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    *out_w = w;
    *out_h = 8;
    return tex;
}

/* Cached time-string texture, rebuilt only when the text changes. */
static void text_update(SDL_Renderer *r, const char *str)
{
    if (!strcmp(str, ui.text_str) && ui.text_tex)
        return;
    if (ui.text_tex)
        SDL_DestroyTexture(ui.text_tex);
    ui.text_tex = render_text(r, str, &ui.text_w, &ui.text_h);
    if (ui.text_tex)
        snprintf(ui.text_str, sizeof(ui.text_str), "%s", str);
}

static SDL_Color icon_color(int el)
{
    SDL_Color on  = { 250, 200,  40, 255 }; /* hovered: PotPlayer yellow */
    SDL_Color off = { 225, 225, 225, 255 };

    return ui.hover == el ? on : off;
}

/* Preview popup: while the cursor is over the seek track, decode the frame at
 * that time (in a background thread) and float a thumbnail above the bar with
 * the timecode under it. No-op until the preview decoder produces something. */
static void draw_seek_thumb(SDL_Renderer *r, int win_w, int seek_top,
                            int track_w, double dur)
{
    const uint32_t *px;
    int tw, th, bx, by, pad = 4, labelh = 20;
    int64_t gen;
    double frac, t;
    char tc[16];

    if (dur <= 0 || !(ui.hover == EL_SEEK || ui.dragging) || !thumb_available())
        return;

    frac = seek_frac_at(ui.mouse_x);
    t    = frac * dur;
    thumb_request(t);
    if (!thumb_acquire(&tw, &th, &px, &gen))
        return;

    if (ui.thumb_tex && (ui.thumb_tw != tw || ui.thumb_th != th)) {
        SDL_DestroyTexture(ui.thumb_tex);
        ui.thumb_tex = NULL;
    }
    if (!ui.thumb_tex) {
        ui.thumb_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, tw, th);
        ui.thumb_tw = tw;
        ui.thumb_th = th;
        ui.thumb_gen = -1;
        if (!ui.thumb_tex)
            return;
    }
    if (gen != ui.thumb_gen) {
        SDL_UpdateTexture(ui.thumb_tex, NULL, px, tw * 4);
        ui.thumb_gen = gen;
    }

    /* box centered on the cursor, clamped to the window, floating above the bar */
    bx = ui.mouse_x - (tw + 2 * pad) / 2;
    if (bx < 4)
        bx = 4;
    if (bx + tw + 2 * pad > win_w - 4)
        bx = win_w - 4 - tw - 2 * pad;
    by = seek_top - (th + 2 * pad + labelh) - 8;
    if (by < 4)
        by = 4;

    fill(r, bx, by, tw + 2 * pad, th + 2 * pad + labelh, 20, 20, 20, 235);
    {
        SDL_Rect dst = { bx + pad, by + pad, tw, th };

        SDL_RenderCopy(r, ui.thumb_tex, NULL, &dst);
        SDL_SetRenderDrawColor(r, 90, 90, 90, 255);
        SDL_RenderDrawRect(r, &dst);
    }

    {
        int s = t < 0 ? 0 : (int)t;

        snprintf(tc, sizeof(tc), "%02d:%02d:%02d", s / 3600, s % 3600 / 60, s % 60);
    }
    if (strcmp(tc, ui.thumb_label) || !ui.thumb_label_tex) {
        if (ui.thumb_label_tex)
            SDL_DestroyTexture(ui.thumb_label_tex);
        ui.thumb_label_tex = render_text_sys(r, tc, 15,
                                             &ui.thumb_label_w, &ui.thumb_label_h, 0, NULL);
        av_strlcpy(ui.thumb_label, tc, sizeof(ui.thumb_label));
    }
    if (ui.thumb_label_tex) {
        SDL_Rect ld = { bx + (tw + 2 * pad - ui.thumb_label_w) / 2,
                        by + th + 2 * pad + (labelh - ui.thumb_label_h) / 2,
                        ui.thumb_label_w, ui.thumb_label_h };

        SDL_RenderCopy(r, ui.thumb_label_tex, NULL, &ld);
    }
}

void ui_draw(SDL_Renderer *renderer, int win_w, int win_h,
             double pos, double dur, int paused, double volume)
{
    int bar_top, seek_top, track_y, track_w, fill_w, cy;
    double frac;
    char buf[64];
    SDL_Color c;

    ui.win_w = win_w;
    ui.win_h = win_h;
    ui.last_drawn_visible = ui_visible_now();
    ui.last_drawn_hover   = ui.hover;
    if (!ui.last_drawn_visible)
        return;

    bar_top  = win_h - BAR_H;
    seek_top = bar_top - SEEK_H;
    track_y  = seek_top + SEEK_H / 2;
    track_w  = seek_track_w();
    cy       = bar_top + BAR_H / 2;

    if (dur > 0) {
        frac = ui.dragging ? ui.drag_frac : pos / dur;
        frac = frac < 0.0 ? 0.0 : frac > 1.0 ? 1.0 : frac;
    } else
        frac = 0.0;
    fill_w = (int)lrint(frac * track_w);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* background scrim: a single vertical gradient over the whole overlay,
     * transparent at the top of the seek row so the video shows through and
     * fill mode still reads as full-screen, darkening toward the bottom where
     * the buttons/timecode need contrast. */
    fill_vgrad(renderer, 0, seek_top, win_w, SEEK_H + BAR_H, 14, 14, 14, 0, 220);

    /* seek track, chapter markers, played fill, handle */
    fill(renderer, SEEK_PAD, track_y - 2, track_w, 4, 85, 85, 85, 255);
    fill(renderer, SEEK_PAD, track_y - 2, fill_w, 4, 250, 200, 40, 255);
    for (int i = 0; i < ui.nb_chapters; i++) {
        int cx = SEEK_PAD + (int)lrint(ui.chapters[i] * track_w);

        fill(renderer, cx - 2, track_y - 6, 5, 13, 235, 235, 235, 255);
    }
    fill(renderer, SEEK_PAD + fill_w - 5, track_y - 9, 11, 18,
         ui.hover == EL_SEEK || ui.dragging ? 250 : 235,
         ui.hover == EL_SEEK || ui.dragging ? 200 : 235,
         ui.hover == EL_SEEK || ui.dragging ?  40 : 235, 255);

    /* volume: speaker icon + slider at the right end of the seek row */
    {
        int vx = win_w - VOL_TRACK - 12;   /* track left edge */
        int sx = vx - 24;                  /* speaker icon */
        int vw = (int)lrint(volume * VOL_TRACK);

        c = icon_color(EL_VOL);
        fill(renderer, sx, track_y - 3, 4, 7, c.r, c.g, c.b, 255);
        tri(renderer, sx + 4.f, (float)track_y,
                      sx + 12.f, track_y - 8.f,
                      sx + 12.f, track_y + 8.f, c);
        fill(renderer, vx, track_y - 1, VOL_TRACK, 3, 85, 85, 85, 255);
        fill(renderer, vx, track_y - 1, vw, 3, 250, 200, 40, 255);
        fill(renderer, vx + vw - 3, track_y - 5, 7, 11,
             ui.hover == EL_VOL || ui.vol_dragging ? 250 : 235,
             ui.hover == EL_VOL || ui.vol_dragging ? 200 : 235,
             ui.hover == EL_VOL || ui.vol_dragging ?  40 : 235, 255);
    }

    /* pause / play */
    c = icon_color(EL_PAUSE);
    if (paused)
        tri(renderer, BTN_W / 2 - 7.f, cy - 9.f,
                      BTN_W / 2 - 7.f, cy + 9.f,
                      BTN_W / 2 + 9.f, (float)cy, c);
    else {
        fill(renderer, BTN_W / 2 - 8, cy - 9, 6, 18, c.r, c.g, c.b, 255);
        fill(renderer, BTN_W / 2 + 2, cy - 9, 6, 18, c.r, c.g, c.b, 255);
    }

    /* stop */
    c = icon_color(EL_STOP);
    fill(renderer, BTN_W + BTN_W / 2 - 8, cy - 8, 16, 16, c.r, c.g, c.b, 255);

    /* back: |< */
    c = icon_color(EL_BACK);
    fill(renderer, 2 * BTN_W + BTN_W / 2 - 9, cy - 8, 3, 16, c.r, c.g, c.b, 255);
    tri(renderer, 2 * BTN_W + BTN_W / 2 + 9.f, cy - 8.f,
                  2 * BTN_W + BTN_W / 2 + 9.f, cy + 8.f,
                  2 * BTN_W + BTN_W / 2 - 4.f, (float)cy, c);

    /* forward: >| */
    c = icon_color(EL_FWD);
    tri(renderer, 3 * BTN_W + BTN_W / 2 - 9.f, cy - 8.f,
                  3 * BTN_W + BTN_W / 2 - 9.f, cy + 8.f,
                  3 * BTN_W + BTN_W / 2 + 4.f, (float)cy, c);
    fill(renderer, 3 * BTN_W + BTN_W / 2 + 6, cy - 8, 3, 16, c.r, c.g, c.b, 255);

    /* open file: eject-style triangle over a bar */
    c = icon_color(EL_OPEN);
    tri(renderer, 4 * BTN_W + BTN_W / 2 - 9.f, cy + 2.f,
                  4 * BTN_W + BTN_W / 2 + 9.f, cy + 2.f,
                  4 * BTN_W + BTN_W / 2, cy - 9.f, c);
    fill(renderer, 4 * BTN_W + BTN_W / 2 - 9, cy + 6, 19, 3, c.r, c.g, c.b, 255);

    /* ---- title bar; in fullscreen it appears only while the cursor sits
     * in the top zone and hides as soon as it moves away */
    if (ui_fullscreen() && ui.mouse_y >= TITLE_H)
        goto controls;
    fill(renderer, 0, 0, win_w, TITLE_H, 14, 14, 14, 235);
    if (!ui.title_tex && ui.title[0]) {
        ui.title_scale = 1; /* system font renders at final size */
        ui.title_tex = render_text_sys(renderer, ui.title, TITLE_H - 14,
                                       &ui.title_w, &ui.title_h, 0, NULL);
        if (!ui.title_tex) { /* fallback: pixel font (ASCII only), drawn x2 */
            ui.title_scale = 2;
            ui.title_tex = render_text(renderer, ui.title,
                                       &ui.title_w, &ui.title_h);
        }
    }
    if (ui.title_tex) {
        int s = ui.title_scale;
        SDL_Rect dst = { 14, TITLE_H / 2 - ui.title_h * s / 2,
                         ui.title_w * s, ui.title_h * s };
        int max_w = win_w - 3 * WBTN_W - 28;

        if (dst.w > max_w) { /* clip long names against the buttons */
            SDL_Rect src = { 0, 0, max_w / s, ui.title_h };

            dst.w = src.w * s;
            SDL_RenderCopy(renderer, ui.title_tex, &src, &dst);
        } else
            SDL_RenderCopy(renderer, ui.title_tex, NULL, &dst);
    }
    {
        int ty = TITLE_H / 2;
        int bx;

        /* close: red hover background + X */
        if (ui.hover == EL_CLOSE)
            fill(renderer, win_w - WBTN_W, 0, WBTN_W, TITLE_H, 200, 30, 30, 255);
        bx = win_w - WBTN_W / 2;
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        for (int i = -1; i <= 0; i++) {
            SDL_RenderDrawLine(renderer, bx - 6 + i, ty - 6, bx + 6 + i, ty + 6);
            SDL_RenderDrawLine(renderer, bx + 6 + i, ty - 6, bx - 6 + i, ty + 6);
        }
        /* maximize: square outline */
        c = icon_color(EL_MAX);
        bx = win_w - WBTN_W - WBTN_W / 2;
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        {
            SDL_Rect rc = { bx - 6, ty - 6, 13, 13 };

            SDL_RenderDrawRect(renderer, &rc);
        }
        /* minimize: line */
        c = icon_color(EL_MIN);
        bx = win_w - 2 * WBTN_W - WBTN_W / 2;
        fill(renderer, bx - 6, ty, 13, 2, c.r, c.g, c.b, 255);
    }

controls:
    /* separator + time text "cur / total" */
    fill(renderer, 5 * BTN_W + 6, bar_top + 10, 1, BAR_H - 20, 70, 70, 70, 255);
    {
        int ps = pos < 0 || isnan(pos) ? 0 : (int)pos;
        int ts = dur < 0 ? 0 : (int)dur;

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d / %02d:%02d:%02d",
                 ps / 3600, ps % 3600 / 60, ps % 60,
                 ts / 3600, ts % 3600 / 60, ts % 60);
    }
    text_update(renderer, buf);
    if (ui.text_tex) {
        SDL_Rect dst = { 5 * BTN_W + 18, cy - ui.text_h,
                         ui.text_w * 2, ui.text_h * 2 };

        SDL_RenderCopy(renderer, ui.text_tex, NULL, &dst);
    }

    /* stream info badges, right-aligned: [H/W] [VCODEC] [HDR] [ACODEC] [CH] */
    if (ui.badges_dirty) {
        for (int i = 0; i < NBADGE; i++) {
            if (ui.badge_tex[i]) {
                SDL_DestroyTexture(ui.badge_tex[i]);
                ui.badge_tex[i] = NULL;
            }
            if (ui.badge_str[i][0])
                ui.badge_tex[i] = render_text_sys(renderer, ui.badge_str[i],
                                                  16, &ui.badge_w[i],
                                                  &ui.badge_h[i], 0, NULL);
        }
        ui.badges_dirty = 0;
    }
    {
        int bx = win_w - 14;

        for (int i = NBADGE - 1; i >= 0; i--) {
            SDL_Rect dst;

            if (!ui.badge_tex[i])
                continue;
            bx -= ui.badge_w[i] + 12;
            dst.x = bx + 6;
            dst.y = cy - ui.badge_h[i] / 2;
            dst.w = ui.badge_w[i];
            dst.h = ui.badge_h[i];
            if (i == BADGE_HW) /* hardware decode: PotPlayer yellow */
                SDL_SetTextureColorMod(ui.badge_tex[i], 250, 200, 40);
            else if (i == BADGE_HDR && strcmp(ui.badge_str[i], "SDR"))
                /* an actual HDR flavour (HDR10/HLG/DV) is worth spotting at
                 * a glance; plain SDR stays a normal grey badge */
                fill(renderer, bx, cy - 11, ui.badge_w[i] + 12, 22,
                     150, 90, 200, 255);
            else
                fill(renderer, bx, cy - 11, ui.badge_w[i] + 12, 22,
                     45, 45, 45, 255);
            SDL_RenderCopy(renderer, ui.badge_tex[i], NULL, &dst);
        }
    }

    /* hover preview last, so it floats above the bar */
    draw_seek_thumb(renderer, win_w, seek_top, track_w, dur);
}
