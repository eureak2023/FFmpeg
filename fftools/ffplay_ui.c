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

#include "libavutil/time.h"

#include "ffplay_fsr.h"
#include "ffplay_ui.h"

#define BAR_H          48       /* control bar height, px */
#define SEEK_H         16       /* seek row hit-area height, px */
#define BTN_W          52       /* control button width, px */
#define SEEK_PAD       10       /* seek track horizontal padding, px */
#define HIDE_DELAY_US  2500000  /* idle time before the overlay hides */
#define DRAG_SEEK_US   300000   /* min interval between drag-seeks */

enum {
    EL_NONE = 0,
    EL_SEEK,       /* seek track */
    EL_PAUSE,
    EL_STOP,
    EL_BACK,
    EL_FWD,
    EL_BAR,        /* control bar background (dead area) */
};

static struct {
    int64_t last_activity;
    int     hover;         /* element under the cursor */
    int     dragging;      /* dragging the seek handle */
    double  drag_frac;
    int64_t last_drag_seek;
    int     win_w, win_h;

    SDL_Texture *text_tex; /* rendered time string */
    int          text_w, text_h;
    char         text_str[64];
    SDL_Renderer *renderer;

    int last_drawn_visible;
    int last_drawn_hover;
} ui;

void ui_init(SDL_Renderer *renderer)
{
    ui.renderer = renderer;
    ui.last_activity = av_gettime_relative();
}

void ui_uninit(void)
{
    if (ui.text_tex) {
        SDL_DestroyTexture(ui.text_tex);
        ui.text_tex = NULL;
    }
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

    if (y >= seek_top && y < bar_top)
        return EL_SEEK;
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

static double seek_frac_at(int x)
{
    double f = (double)(x - SEEK_PAD) / (ui.win_w - 2 * SEEK_PAD);

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
        return UI_ACT_NONE; /* let ffplay keep its cursor logic */
    case SDL_MOUSEBUTTONDOWN:
        if (event->button.button != SDL_BUTTON_LEFT)
            return UI_ACT_NONE;
        x = event->button.x;
        y = event->button.y;
        ui.last_activity = av_gettime_relative();
        if (!ui_visible_now())
            return UI_ACT_NONE; /* first click only reveals the bar */
        switch (element_at(x, y)) {
        case EL_SEEK:
            ui.dragging  = 1;
            ui.drag_frac = seek_frac_at(x);
            ui.last_drag_seek = av_gettime_relative();
            *seek_frac = ui.drag_frac;
            return UI_ACT_SEEK_FRAC;
        case EL_PAUSE: return UI_ACT_TOGGLE_PAUSE;
        case EL_STOP:  return UI_ACT_STOP;
        case EL_BACK:  return UI_ACT_SEEK_BACK;
        case EL_FWD:   return UI_ACT_SEEK_FWD;
        case EL_BAR:   return UI_ACT_CONSUMED;
        default:       return UI_ACT_NONE;
        }
    case SDL_MOUSEBUTTONUP:
        if (event->button.button == SDL_BUTTON_LEFT && ui.dragging) {
            ui.dragging = 0;
            *seek_frac  = seek_frac_at(event->button.x);
            return UI_ACT_SEEK_FRAC;
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

/* Render the time string into a cached texture (transparent background,
 * white 8x8 pixel font); rebuilt only when the text changes. */
static void text_update(SDL_Renderer *r, const char *str)
{
    static uint32_t px[63 * 8 * 8]; /* up to 63 chars of 8x8 glyphs */
    int len = strlen(str);
    int w, h = 8;

    if (!strcmp(str, ui.text_str) && ui.text_tex)
        return;
    if (len > 63)
        len = 63;
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

    if (ui.text_tex)
        SDL_DestroyTexture(ui.text_tex);
    ui.text_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STATIC, w, h);
    if (!ui.text_tex)
        return;
    SDL_UpdateTexture(ui.text_tex, NULL, px, w * 4);
    SDL_SetTextureBlendMode(ui.text_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(ui.text_tex, SDL_ScaleModeNearest);
    ui.text_w = w;
    ui.text_h = h;
    snprintf(ui.text_str, sizeof(ui.text_str), "%s", str);
}

static SDL_Color icon_color(int el)
{
    SDL_Color on  = { 250, 200,  40, 255 }; /* hovered: PotPlayer yellow */
    SDL_Color off = { 225, 225, 225, 255 };

    return ui.hover == el ? on : off;
}

void ui_draw(SDL_Renderer *renderer, int win_w, int win_h,
             double pos, double dur, int paused)
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
    track_w  = win_w - 2 * SEEK_PAD;
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

    /* seek track, played fill, handle */
    fill(renderer, SEEK_PAD, track_y - 1, track_w, 3, 85, 85, 85, 255);
    fill(renderer, SEEK_PAD, track_y - 1, fill_w, 3, 250, 200, 40, 255);
    fill(renderer, SEEK_PAD + fill_w - 4, track_y - 6, 9, 12,
         ui.hover == EL_SEEK || ui.dragging ? 250 : 235,
         ui.hover == EL_SEEK || ui.dragging ? 200 : 235,
         ui.hover == EL_SEEK || ui.dragging ?  40 : 235, 255);

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
}
