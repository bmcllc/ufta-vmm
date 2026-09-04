/*
 * UFTA-VMM — gui.c — Real-time monitoring dashboard (SDL2 + OpenGL)
 *
 * Self-contained immediate-mode 2D renderer built on SDL2 + OpenGL.
 * Draws the commercial-facing dashboard: VRAM recovered, latency
 * reduction, FPS stability, tier utilization, migration and worker
 * stats — all updated in real time.
 *
 * Build: gcc -o uvm-gui src/gui.c ... $(pkg-config --cflags --libs sdl2) -lGL
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ufta/gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <GL/gl.h>

/* ── Color helpers ────────────────────────────────────────────── */

typedef struct { float r, g, b, a; } gui_color_t;

static const gui_color_t COL_BG      = {0.10f, 0.11f, 0.13f, 1.0f};
static const gui_color_t COL_PANEL   = {0.16f, 0.17f, 0.20f, 1.0f};
static const gui_color_t COL_TEXT    = {0.90f, 0.90f, 0.92f, 1.0f};
static const gui_color_t COL_DIM     = {0.55f, 0.56f, 0.60f, 1.0f};
static const gui_color_t COL_ACCENT  = {0.20f, 0.65f, 0.95f, 1.0f};
static const gui_color_t COL_GREEN   = {0.25f, 0.80f, 0.45f, 1.0f};
static const gui_color_t COL_YELLOW  = {0.95f, 0.80f, 0.25f, 1.0f};
static const gui_color_t COL_RED     = {0.90f, 0.30f, 0.30f, 1.0f};
static const gui_color_t COL_ORANGE  = {0.95f, 0.55f, 0.20f, 1.0f};

/* ── Simple bitmap font (5x7) for text rendering ────────────────
 * We render text using a minimal 5x7 pixel font stored as bitmaps.
 * This keeps the GUI dependency-free (no external font files). */

#define FONT_W 5
#define FONT_H 7
#define FONT_CHARS 95   /* ' ' .. '~' */

/* 5x7 font data — each char is 7 rows of 5 bits (LSB = leftmost) */
static const unsigned char font_data[FONT_CHARS][FONT_H] = {
    /* space */ {0,0,0,0,0,0,0},
    /* ! */ {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    /* " */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* # */ {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00},
    /* $ */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* % */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* & */ {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    /* ' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* ( */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* ) */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* * */ {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00},
    /* + */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* , */ {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    /* - */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* . */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    /* / */ {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    /* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    /* ; */ {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    /* < */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* = */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* > */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* ? */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* @ */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    /* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* D */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    /* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* J */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    /* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* V */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* Z */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* [ */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* \ */ {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    /* ] */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* ^ */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* _ */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* ` */ {0x08,0x04,0x00,0x00,0x00,0x00,0x00},
    /* a */ {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* b */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
    /* c */ {0x00,0x00,0x0E,0x10,0x10,0x10,0x0E},
    /* d */ {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
    /* e */ {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /* f */ {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    /* g */ {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E},
    /* h */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
    /* i */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /* j */ {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /* k */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /* l */ {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* m */ {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
    /* n */ {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
    /* o */ {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /* p */ {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10},
    /* q */ {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01},
    /* r */ {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /* s */ {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    /* t */ {0x08,0x08,0x1C,0x08,0x08,0x09,0x06},
    /* u */ {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    /* v */ {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    /* w */ {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /* x */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /* y */ {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /* z */ {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /* { */ {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /* | */ {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    /* } */ {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /* ~ */ {0x08,0x15,0x02,0x00,0x00,0x00,0x00},
};

/* ── Render state ─────────────────────────────────────────────── */

typedef struct {
    SDL_Window   *window;
    SDL_GLContext glctx;
    int w, h;
    /* History buffers for charts */
    float hist_cycle_us[UFTA_GUI_MAX_SAMPLES];
    float hist_latency_us[UFTA_GUI_MAX_SAMPLES];
    float hist_vram_mb[UFTA_GUI_MAX_SAMPLES];
    int   hist_count;
} gui_state_t;

/* ── OpenGL primitives ────────────────────────────────────────── */

static void gl_set_color(gui_color_t c)
{
    glColor4f(c.r, c.g, c.b, c.a);
}

static void gl_rect(float x, float y, float w, float h)
{
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void gl_rect_outline(float x, float y, float w, float h)
{
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void gl_line(float x1, float y1, float x2, float y2)
{
    glBegin(GL_LINES);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
}

/* ── Text rendering ───────────────────────────────────────────── */

static void draw_text(float x, float y, const char *s, gui_color_t c,
                      float scale)
{
    gl_set_color(c);
    glBegin(GL_QUADS);
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = ' ';
        int idx = ch - 32;
        for (int row = 0; row < FONT_H; row++) {
            unsigned char bits = font_data[idx][row];
            for (int col = 0; col < FONT_W; col++) {
                if (bits & (1 << (FONT_W - 1 - col))) {
                    float px = x + col * scale;
                    float py = y + row * scale;
                    glVertex2f(px, py);
                    glVertex2f(px + scale, py);
                    glVertex2f(px + scale, py + scale);
                    glVertex2f(px, py + scale);
                }
            }
        }
        x += (FONT_W + 1) * scale;
    }
    glEnd();
}

/* ── Panel helper ─────────────────────────────────────────────── */

static void draw_panel(float x, float y, float w, float h, const char *title)
{
    gl_set_color(COL_PANEL);
    gl_rect(x, y, w, h);
    gl_set_color(COL_ACCENT);
    gl_rect(x, y, w, 2.0f); /* top accent bar */
    if (title) {
        draw_text(x + 8, y + 6, title, COL_TEXT, 1.5f);
    }
}

/* ── Progress bar ─────────────────────────────────────────────── */

static void draw_bar(float x, float y, float w, float h, float frac,
                     gui_color_t fill)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    gl_set_color(COL_DIM);
    gl_rect(x, y, w, h);
    if (frac > 0.0f) {
        gl_set_color(fill);
        gl_rect(x, y, w * frac, h);
    }
    gl_set_color(COL_TEXT);
    gl_rect_outline(x, y, w, h);
}

/* ── Line chart ───────────────────────────────────────────────── */

static void draw_chart(float x, float y, float w, float h,
                       const float *data, int count, gui_color_t c,
                       float max_val)
{
    /* Background */
    gl_set_color(COL_BG);
    gl_rect(x, y, w, h);
    gl_set_color(COL_DIM);
    gl_rect_outline(x, y, w, h);

    if (count < 2 || max_val <= 0.0f) return;

    /* Grid lines */
    gl_set_color((gui_color_t){0.25f, 0.26f, 0.30f, 1.0f});
    for (int i = 1; i < 4; i++) {
        float gy = y + h * (float)i / 4.0f;
        gl_line(x, gy, x + w, gy);
    }

    /* Data line */
    gl_set_color(c);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < count; i++) {
        float v = data[i];
        if (v > max_val) v = max_val;
        float px = x + (float)i / (float)(UFTA_GUI_MAX_SAMPLES - 1) * w;
        float py = y + h - (v / max_val) * h;
        glVertex2f(px, py);
    }
    glEnd();
}

/* ── Metric card ──────────────────────────────────────────────── */

static void draw_metric(float x, float y, float w, float h,
                        const char *label, const char *value,
                        gui_color_t value_color)
{
    gl_set_color(COL_PANEL);
    gl_rect(x, y, w, h);
    gl_set_color(COL_DIM);
    gl_rect_outline(x, y, w, h);
    draw_text(x + 8, y + 8, label, COL_DIM, 1.2f);
    draw_text(x + 8, y + 30, value, value_color, 2.0f);
}

/* ── Main render ──────────────────────────────────────────────── */

static void render_frame(gui_state_t *gs, const ufta_gui_snapshot_t *snap)
{
    int w = gs->w, h = gs->h;

    glClearColor(COL_BG.r, COL_BG.g, COL_BG.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* ── Header ── */
    draw_text(20, 12, "UFTA-VMM  Dashboard  —  Virtual Memory Manager",
              COL_TEXT, 2.0f);
    draw_text(20, 34, "Field-Driven Virtual Memory | Real-time telemetry",
              COL_DIM, 1.2f);

    /* ── Top metric cards ── */
    float card_w = (w - 80) / 4.0f;
    float card_h = 70;
    float cy = 60;

    /* VRAM recovered */
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f MB", snap->vram_recovered_mb);
    draw_metric(20, cy, card_w, card_h, "VRAM Recovered", buf, COL_GREEN);

    /* Latency per fault */
    snprintf(buf, sizeof(buf), "%.1f us", snap->latency_us_per_fault);
    draw_metric(30 + card_w, cy, card_w, card_h, "Latency / Fault", buf,
                snap->latency_us_per_fault < 2.0 ? COL_GREEN : COL_YELLOW);

    /* FPS estimate */
    snprintf(buf, sizeof(buf), "%.0f FPS", snap->fps_estimate);
    draw_metric(40 + 2 * card_w, cy, card_w, card_h, "FPS Stability", buf,
                snap->fps_estimate >= 55.0 ? COL_GREEN : COL_ORANGE);

    /* Pipeline cycle */
    snprintf(buf, sizeof(buf), "%.1f ms", snap->metrics ?
             snap->metrics->total_cycle_us / 1000.0 : 0.0);
    draw_metric(50 + 3 * card_w, cy, card_w, card_h, "Pipeline Cycle", buf,
                COL_ACCENT);

    /* ── Charts row ── */
    float chart_y = cy + card_h + 20;
    float chart_h = 160;
    float chart_w = (w - 60) / 2.0f;

    draw_panel(20, chart_y, chart_w, chart_h, "Cycle Time (µs)");
    draw_chart(30, chart_y + 30, chart_w - 20, chart_h - 40,
               gs->hist_cycle_us, gs->hist_count, COL_ACCENT, 2000.0f);

    draw_panel(40 + chart_w, chart_y, chart_w, chart_h, "Latency per Fault (µs)");
    draw_chart(50 + chart_w, chart_y + 30, chart_w - 20, chart_h - 40,
               gs->hist_latency_us, gs->hist_count, COL_GREEN, 10.0f);

    /* ── Tier utilization ── */
    float tier_y = chart_y + chart_h + 20;
    float tier_h = 120;
    draw_panel(20, tier_y, w - 40, tier_h, "Tier Utilization");

    if (snap->tiers) {
        int n = snap->tiers->num_tiers;
        float bar_w = (w - 80) / (float)(n > 0 ? n : 1);
        for (int i = 0; i < n; i++) {
            tier_t *t = &snap->tiers->tiers[i];
            float bx = 30 + i * bar_w;
            float frac = (t->capacity == UINT64_MAX) ? 0.0f :
                         (float)((double)t->used / (double)t->capacity);
            gui_color_t c = COL_ACCENT;
            if (frac > 0.8f) c = COL_RED;
            else if (frac > 0.5f) c = COL_YELLOW;
            draw_bar(bx, tier_y + 40, bar_w - 10, 20, frac, c);
            draw_text(bx, tier_y + 66, t->name, COL_TEXT, 1.2f);
            snprintf(buf, sizeof(buf), "%.0f%%", frac * 100.0f);
            draw_text(bx, tier_y + 84, buf, COL_DIM, 1.2f);
        }
    }

    /* ── Bottom stats ── */
    float stat_y = tier_y + tier_h + 20;
    float stat_h = h - stat_y - 20;

    draw_panel(20, stat_y, (w - 60) / 2.0f, stat_h, "Migration & Worker");

    if (snap->migrate) {
        snprintf(buf, sizeof(buf), "Migracoes: %llu",
                 (unsigned long long)snap->migrate->total_migrations);
        draw_text(30, stat_y + 30, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Bytes movidos: %.1f MB",
                 snap->migrate->total_bytes_moved / (1024.0 * 1024.0));
        draw_text(30, stat_y + 50, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Skipped (no gain): %llu",
                 (unsigned long long)snap->migrate->skipped_no_gain);
        draw_text(30, stat_y + 70, buf, COL_TEXT, 1.3f);
    }

    if (snap->worker) {
        snprintf(buf, sizeof(buf), "Batches: %llu",
                 (unsigned long long)snap->worker->total_batches);
        draw_text(30, stat_y + 100, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Paginas processadas: %llu",
                 (unsigned long long)snap->worker->total_pages_processed);
        draw_text(30, stat_y + 120, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Max batch: %llu paginas",
                 (unsigned long long)snap->worker->max_batch_pages);
        draw_text(30, stat_y + 140, buf, COL_TEXT, 1.3f);
    }

    draw_panel(40 + (w - 60) / 2.0f, stat_y, (w - 60) / 2.0f, stat_h,
               "Page Faults");

    if (snap->pf) {
        snprintf(buf, sizeof(buf), "Total faults: %llu",
                 (unsigned long long)snap->pf->total_faults);
        draw_text(50 + (w - 60) / 2.0f, stat_y + 30, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Bytes carregados: %.1f MB",
                 (double)snap->pf->bytes_loaded / (1024.0 * 1024.0));
        draw_text(50 + (w - 60) / 2.0f, stat_y + 50, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Evictions: %llu",
                 (unsigned long long)snap->pf->total_evictions);
        draw_text(50 + (w - 60) / 2.0f, stat_y + 70, buf, COL_TEXT, 1.3f);
        snprintf(buf, sizeof(buf), "Worker: %s",
                 snap->pf->worker_started ? "ATIVO" : "inativo");
        draw_text(50 + (w - 60) / 2.0f, stat_y + 100, buf,
                  snap->pf->worker_started ? COL_GREEN : COL_DIM, 1.3f);
    }

    /* Footer */
    draw_text(20, h - 18, "ESC para sair | UFTA-VMM v1.0.0", COL_DIM, 1.2f);
}

/* ── Main loop ────────────────────────────────────────────────── */

int ufta_gui_run(const ufta_gui_snapshot_t *snap)
{
    if (!snap) return -1;

    gui_state_t gs;
    memset(&gs, 0, sizeof(gs));
    gs.w = UFTA_GUI_WINDOW_W;
    gs.h = UFTA_GUI_WINDOW_H;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    gs.window = SDL_CreateWindow(UFTA_GUI_TITLE,
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 gs.w, gs.h,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!gs.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    gs.glctx = SDL_GL_CreateContext(gs.window);
    if (!gs.glctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(gs.window);
        SDL_Quit();
        return -1;
    }

    /* Set up orthographic projection (pixel coordinates) */
    glViewport(0, 0, gs.w, gs.h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, gs.w, gs.h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    bool running = true;
    Uint32 last_tick = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                }
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    gs.w = ev.window.data1;
                    gs.h = ev.window.data2;
                    glViewport(0, 0, gs.w, gs.h);
                    glMatrixMode(GL_PROJECTION);
                    glLoadIdentity();
                    glOrtho(0, gs.w, gs.h, 0, -1, 1);
                    glMatrixMode(GL_MODELVIEW);
                    glLoadIdentity();
                }
            }
        }

        /* Sample metrics at ~10 Hz */
        Uint32 now = SDL_GetTicks();
        if (now - last_tick >= 100) {
            last_tick = now;

            /* Push history samples */
            if (gs.hist_count < UFTA_GUI_MAX_SAMPLES) {
                gs.hist_cycle_us[gs.hist_count] =
                    snap->metrics ? (float)snap->metrics->total_cycle_us : 0.0f;
                gs.hist_latency_us[gs.hist_count] =
                    (float)snap->latency_us_per_fault;
                gs.hist_vram_mb[gs.hist_count] = (float)snap->vram_recovered_mb;
                gs.hist_count++;
            } else {
                /* Shift window */
                memmove(gs.hist_cycle_us, gs.hist_cycle_us + 1,
                        (UFTA_GUI_MAX_SAMPLES - 1) * sizeof(float));
                memmove(gs.hist_latency_us, gs.hist_latency_us + 1,
                        (UFTA_GUI_MAX_SAMPLES - 1) * sizeof(float));
                memmove(gs.hist_vram_mb, gs.hist_vram_mb + 1,
                        (UFTA_GUI_MAX_SAMPLES - 1) * sizeof(float));
                gs.hist_cycle_us[UFTA_GUI_MAX_SAMPLES - 1] =
                    snap->metrics ? (float)snap->metrics->total_cycle_us : 0.0f;
                gs.hist_latency_us[UFTA_GUI_MAX_SAMPLES - 1] =
                    (float)snap->latency_us_per_fault;
                gs.hist_vram_mb[UFTA_GUI_MAX_SAMPLES - 1] =
                    (float)snap->vram_recovered_mb;
            }
        }

        render_frame(&gs, snap);

        SDL_GL_SwapWindow(gs.window);
        SDL_Delay(16); /* ~60 FPS */
    }

    SDL_GL_DeleteContext(gs.glctx);
    SDL_DestroyWindow(gs.window);
    SDL_Quit();
    return 0;
}
