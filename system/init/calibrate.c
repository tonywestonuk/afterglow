/*
 * Interactive display roll calibration. Each eye sees level lines and its current correction,
 * all turned together by that correction, so nothing in view stays level as a false cue.
 *   Volume Up / Down   turn by +/- 0.5° (counterclockwise is positive)
 *   Power              cycle what's adjusted: both eyes, left only, right only
 * The selected eye's label is bright, the other dim. Whoever wears the headset reads the values
 * out once both eyes look level and line up.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>

#include "afterglow.h"
#include "display.h"

#define W 900
#define H 640
#define STEP 0.5

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column */
static const struct {
    char c;
    uint8_t rows[7];
} font[] = {
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } }, { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } }, { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } }, { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } }, { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } }, { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } }, { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
};

static void fill(uint8_t *rgb, int x0, int y0, int w, int h, const uint8_t colour[3])
{
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < H; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < W; x++)
            memcpy(rgb + ((size_t)y * W + x) * 3, colour, 3);
}

/* Text centred on (cx, cy), each glyph pixel `scale` pixels square */
static void text(uint8_t *rgb, int cx, int cy, int scale, const char *s, const uint8_t colour[3])
{
    int n = strlen(s), x = cx - (n * 6 - 1) * scale / 2, y = cy - 7 * scale / 2;
    for (; *s; s++, x += 6 * scale) {
        for (size_t g = 0; g < sizeof(font) / sizeof(font[0]); g++) {
            if (font[g].c != *s)
                continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (font[g].rows[row] & (0x10 >> col))
                        fill(rgb, x + col * scale, y + row * scale, scale, scale, colour);
        }
    }
}

static void render(uint8_t *rgb, int eye, double degrees, int selected)
{
    static const uint8_t white[3] = { 255, 255, 255 }, amber[3] = { 255, 176, 112 },
                         violet[3] = { 184, 92, 255 }, dim[3] = { 90, 90, 90 };
    memset(rgb, 0, (size_t)W * H * 3);
    /* Level lines through and parallel to the lens centre (the middle of the image) */
    fill(rgb, W / 2 - 380, H / 2 - 3, 760, 6, white);
    for (int k = -2; k <= 2; k += 4)
        fill(rgb, W / 2 - 220, H / 2 + k * 70 - 2, 440, 3, violet);
    for (int k = -1; k <= 1; k += 2)
        fill(rgb, W / 2 - 300, H / 2 + k * 70 - 2, 600, 3, amber);
    char label[16];
    snprintf(label, sizeof(label), "%c%+.1f", eye ? 'R' : 'L', degrees);
    text(rgb, W / 2, H / 2 - 230, 10, label, selected ? white : dim);
}

int calibrate_run(const char *fb_path, double degrees[2], double deadline)
{
    struct fb fb;
    if (fb_open(&fb, fb_path) != 0)
        return -1;
    if (input_open() == 0)
        klog("calibration: no input devices");

    struct view views[2];
    uint8_t *rgb[2];
    for (int eye = 0; eye < 2; eye++) {
        views[eye] = (struct view){ .eye = eye, .width = W, .height = H, .x = LENS_CENTRE_X(eye) - W / 2,
                                    .y = LENS_CENTRE_Y - H / 2, .cx = LENS_CENTRE_X(eye), .cy = LENS_CENTRE_Y };
        if (!(rgb[eye] = malloc((size_t)W * H * 3)))
            return -1;
    }

    int mode = 0;   /* 0: both eyes, 1: left, 2: right */
    int dirty = 1;
    while (deadline <= 0 || now() < deadline) {
        if (dirty) {
            uint8_t *page = fb_back(&fb);
            memset(page, 0, (size_t)fb.fix.line_length * fb.var.yres);   /* the turned area changes */
            for (int eye = 0; eye < 2; eye++) {
                render(rgb[eye], eye, degrees[eye], mode == 0 || mode == eye + 1);
                if (view_map(&views[eye], degrees[eye] * M_PI / 180, fb.var.xres, fb.var.yres) != 0)
                    return -1;
                view_draw(&views[eye], &fb, page, rgb[eye]);
            }
            fb_flip(&fb);
            klog("calibration: left %+.1f, right %+.1f degrees (adjusting %s)", degrees[0], degrees[1],
                 mode == 0 ? "both" : mode == 1 ? "left" : "right");
            dirty = 0;
        }
        int timeout = deadline > 0 ? (int)fmin(1000, (deadline - now()) * 1000) : 1000;
        int key = input_key(timeout > 0 ? timeout : 0);
        double step = key == KEY_VOLUMEUP ? STEP : key == KEY_VOLUMEDOWN ? -STEP : 0;
        if (step != 0) {
            if (mode != 2)
                degrees[0] += step;
            if (mode != 1)
                degrees[1] += step;
            dirty = 1;
        } else if (key == KEY_POWER) {
            mode = (mode + 1) % 3;
            dirty = 1;
        }
    }
    return 0;
}
