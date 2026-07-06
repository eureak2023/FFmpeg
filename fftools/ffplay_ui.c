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
#endif

#include "libavutil/time.h"

#include "ffplay_fsr.h"
#include "ffplay_ui.h"

#define BAR_H          48       /* control bar height, px */
#define SEEK_H         16       /* seek row hit-area height, px */
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
    EL_BAR,        /* control bar background (dead area) */
    EL_TITLE,      /* title bar drag area */
    EL_MIN,
    EL_MAX,
    EL_CLOSE,
};

#define VOL_AREA   150  /* right end of the seek row: icon + slider */
#define VOL_TRACK  100  /* slider track width */
#define MAX_CHAPTERS 128
#define NBADGE 4

static struct {
    int64_t last_activity;
    int     hover;         /* element under the cursor */
    int     dragging;      /* dragging the seek handle */
    double  drag_frac;
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
    SDL_Texture *title_tex;
    int          title_w, title_h, title_scale;
    SDL_Renderer *renderer;

    int last_drawn_visible;
    int last_drawn_hover;
} ui;

/* Rasterize UTF-8 text with the system font via GDI (handles Korean and
 * everything else the pixel font cannot). White glyphs, alpha from
 * coverage. Returns NULL on failure (caller falls back to the pixel font). */
static SDL_Texture *render_text_sys(SDL_Renderer *r, const char *utf8,
                                    int px_h, int *out_w, int *out_h)
{
#ifdef _WIN32
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
                       L"Malgun Gothic");
    if (!font)
        goto out;
    old_font = (HFONT)SelectObject(dc, font);
    DrawTextW(dc, wbuf, -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
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
    DrawTextW(dc, wbuf, -1, &rc, DT_SINGLELINE | DT_NOPREFIX);
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

void ui_init(SDL_Renderer *renderer)
{
    ui.renderer = renderer;
    ui.last_activity = av_gettime_relative();
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
    SDL_SetWindowHitTest(win, hit_test, NULL);
}

void ui_set_badges(const char *hw, const char *vcodec,
                   const char *acodec, const char *chans)
{
    const char *s[NBADGE] = { hw, vcodec, acodec, chans };

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
    for (int i = 0; i < NBADGE; i++)
        if (ui.badge_tex[i]) {
            SDL_DestroyTexture(ui.badge_tex[i]);
            ui.badge_tex[i] = NULL;
        }
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

    if (y < TITLE_H && !ui_fullscreen()) {
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
        default: return EL_BAR;
        }
    }
    return EL_NONE;
}

/* Start tracking a potential window-move drag from a left-button press. */
static void wdrag_arm(void)
{
    if (!ui.window || ui_fullscreen())
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
        ui.last_activity = av_gettime_relative();
        ui.hover = element_at(x, y);
        if (ui.dragging) {
            int64_t now = av_gettime_relative();

            ui.drag_frac = seek_frac_at(x);
            if (now - ui.last_drag_seek >= DRAG_SEEK_US) {
                ui.last_drag_seek = now;
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
            ui.dragging = 0;
            *seek_frac  = seek_frac_at(event->button.x);
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
            !ui.wdrag_armed)
            ui.hover = EL_NONE;
        return UI_ACT_NONE;
    }
    return UI_ACT_NONE;
}

int ui_wants_refresh(void)
{
    int vis = ui_visible_now();

    if (vis != ui.last_drawn_visible || ui.hover != ui.last_drawn_hover)
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

    /* backgrounds */
    fill(renderer, 0, seek_top, win_w, SEEK_H, 14, 14, 14, 200);
    fill(renderer, 0, bar_top, win_w, BAR_H, 14, 14, 14, 235);

    /* seek track, chapter markers, played fill, handle */
    fill(renderer, SEEK_PAD, track_y - 1, track_w, 3, 85, 85, 85, 255);
    fill(renderer, SEEK_PAD, track_y - 1, fill_w, 3, 250, 200, 40, 255);
    for (int i = 0; i < ui.nb_chapters; i++) {
        int cx = SEEK_PAD + (int)lrint(ui.chapters[i] * track_w);

        fill(renderer, cx - 2, track_y - 4, 5, 9, 235, 235, 235, 255);
    }
    fill(renderer, SEEK_PAD + fill_w - 4, track_y - 6, 9, 12,
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

    /* ---- title bar (hidden in fullscreen: video only + bottom controls) */
    if (ui_fullscreen())
        goto controls;
    fill(renderer, 0, 0, win_w, TITLE_H, 14, 14, 14, 235);
    if (!ui.title_tex && ui.title[0]) {
        ui.title_scale = 1; /* system font renders at final size */
        ui.title_tex = render_text_sys(renderer, ui.title, TITLE_H - 14,
                                       &ui.title_w, &ui.title_h);
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
    fill(renderer, 4 * BTN_W + 6, bar_top + 10, 1, BAR_H - 20, 70, 70, 70, 255);
    {
        int ps = pos < 0 || isnan(pos) ? 0 : (int)pos;
        int ts = dur < 0 ? 0 : (int)dur;

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d / %02d:%02d:%02d",
                 ps / 3600, ps % 3600 / 60, ps % 60,
                 ts / 3600, ts % 3600 / 60, ts % 60);
    }
    text_update(renderer, buf);
    if (ui.text_tex) {
        SDL_Rect dst = { 4 * BTN_W + 18, cy - ui.text_h,
                         ui.text_w * 2, ui.text_h * 2 };

        SDL_RenderCopy(renderer, ui.text_tex, NULL, &dst);
    }

    /* stream info badges, right-aligned: [H/W] [VCODEC] [ACODEC] [CH] */
    if (ui.badges_dirty) {
        for (int i = 0; i < NBADGE; i++) {
            if (ui.badge_tex[i]) {
                SDL_DestroyTexture(ui.badge_tex[i]);
                ui.badge_tex[i] = NULL;
            }
            if (ui.badge_str[i][0])
                ui.badge_tex[i] = render_text_sys(renderer, ui.badge_str[i],
                                                  16, &ui.badge_w[i],
                                                  &ui.badge_h[i]);
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
            if (i == 0) /* hardware decode: PotPlayer yellow */
                SDL_SetTextureColorMod(ui.badge_tex[i], 250, 200, 40);
            else
                fill(renderer, bx, cy - 11, ui.badge_w[i] + 12, 22,
                     45, 45, 45, 255);
            SDL_RenderCopy(renderer, ui.badge_tex[i], NULL, &dst);
        }
    }
}
