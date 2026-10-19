/*
 * The status screen: what afterglow is doing, for whoever wears the headset. Each line comes from a
 * file in /run/afterglow/status, read in name order, holding "label<TAB>text": whatever knows writes
 * its own (the bring-up, the DHCP client, afterglowd). Redrawn when they change. Text is Montserrat,
 * rendered with stb_truetype; both eyes see the same image, so it appears far away. While another
 * program owns the display (the world), it keeps off it.
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "afterglow.h"
#include "display.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define W 1000
#define H 720
#define STATUS_DIR "/run/afterglow/status"

static const uint8_t white[3] = { 240, 240, 240 }, dim[3] = { 130, 130, 140 }, amber[3] = { 255, 176, 112 };

/* Next code point of a UTF-8 string ('?' for a malformed byte) */
static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && extra == 0)
        c = '?';
    else if (extra)
        c &= 0x3f >> extra;
    for (; extra > 0 && (*p & 0xc0) == 0x80; extra--)
        c = (c << 6) | (*p++ & 0x3f);
    *s = (const char *)p;
    return extra ? '?' : c;
}

/*
 * Draw text with its left end at x and its baseline at y, `px` pixels from ascender to descender,
 * blended over what's there. Returns where the next character would go (to measure, pass rgb NULL).
 */
static float draw_text(uint8_t *rgb, const stbtt_fontinfo *font, float px, float x, int y, const char *s,
                       const uint8_t colour[3])
{
    float scale = stbtt_ScaleForPixelHeight(font, px);
    int prev = 0;
    while (*s) {
        int c = utf8_next(&s), advance, lsb, x0, y0, x1, y1;
        if (prev)
            x += scale * stbtt_GetCodepointKernAdvance(font, prev, c);
        stbtt_GetCodepointHMetrics(font, c, &advance, &lsb);
        if (rgb) {
            stbtt_GetCodepointBitmapBoxSubpixel(font, c, scale, scale, x - floorf(x), 0, &x0, &y0, &x1, &y1);
            int w = x1 - x0, h = y1 - y0;
            uint8_t *glyph = w > 0 && h > 0 ? malloc((size_t)w * h) : NULL;
            if (glyph) {
                stbtt_MakeCodepointBitmapSubpixel(font, glyph, w, h, w, scale, scale, x - floorf(x), 0, c);
                for (int gy = 0; gy < h; gy++) {
                    int py = y + y0 + gy;
                    for (int gx = 0; gx < w && py >= 0 && py < H; gx++) {
                        int pxl = (int)floorf(x) + x0 + gx, a = glyph[gy * w + gx];
                        if (pxl < 0 || pxl >= W || a == 0)
                            continue;
                        uint8_t *d = rgb + ((size_t)py * W + pxl) * 3;
                        for (int k = 0; k < 3; k++)
                            d[k] = (uint8_t)((d[k] * (255 - a) + colour[k] * a) / 255);
                    }
                }
                free(glyph);
            }
        }
        x += scale * advance;
        prev = c;
    }
    return x;
}

/* All status lines, "label\ttext\n" each, in file name order */
static void read_status(char *out, size_t size)
{
    struct dirent **names;
    int n = scandir(STATUS_DIR, &names, NULL, alphasort);
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        char path[300], line[256];
        snprintf(path, sizeof(path), STATUS_DIR "/%s", names[i]->d_name);
        FILE *f = names[i]->d_name[0] != '.' ? fopen(path, "r") : NULL;
        if (f && fgets(line, sizeof(line), f) && strchr(line, '\t') && len < size - 1) {
            line[strcspn(line, "\n")] = '\0';
            len += snprintf(out + len, size - len, "%s\n", line);
        }
        if (f)
            fclose(f);
        free(names[i]);
    }
    if (n >= 0)
        free(names);
}

static void render(uint8_t *rgb, const stbtt_fontinfo *font, const char *status)
{
    memset(rgb, 0, (size_t)W * H * 3);
    float title = draw_text(NULL, font, 110, 0, 0, "afterglow", amber);
    draw_text(rgb, font, 110, (W - title) / 2, 190, "afterglow", amber);
    int y = 330;
    for (const char *line = status; *line && y < H - 20; y += 78) {
        const char *tab = strchr(line, '\t'), *end = strchr(line, '\n');
        char label[64], text[192];
        snprintf(label, sizeof(label), "%.*s", (int)(tab - line), line);
        snprintf(text, sizeof(text), "%.*s", (int)(end - tab - 1), tab + 1);
        draw_text(rgb, font, 52, 90, y, label, dim);
        draw_text(rgb, font, 52, 330, y, text, white);
        line = end + 1;
    }
}

/* Whether another program owns the display: the world writes its pid to /run/afterglow/display-owner */
static int display_taken(void)
{
    FILE *f = fopen("/run/afterglow/display-owner", "r");
    int pid = 0;
    if (!f)
        return 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    fclose(f);
    char proc[32];
    snprintf(proc, sizeof(proc), "/proc/%d", pid);
    return pid > 0 && access(proc, F_OK) == 0;
}

int status_run(struct fb *fb, const double roll[2], double deadline)
{
    size_t size;
    FILE *f = fopen("/afterglow/font.ttf", "rb");
    uint8_t *ttf = NULL;
    stbtt_fontinfo font;
    if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && (ttf = malloc(size)) &&
        fseek(f, 0, SEEK_SET) == 0 && fread(ttf, 1, size, f) == size)
        fclose(f), f = NULL;
    if (f)
        fclose(f);
    if (!ttf || !stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        klog("status screen: can't load /afterglow/font.ttf");
        return -1;
    }

    struct view views[2];
    uint8_t *rgb = malloc((size_t)W * H * 3);
    if (!rgb)
        return -1;
    for (int eye = 0; eye < 2; eye++) {
        views[eye] = (struct view){ .eye = eye, .width = W, .height = H, .x = LENS_CENTRE_X(eye) - W / 2,
                                    .y = LENS_CENTRE_Y - H / 2, .cx = LENS_CENTRE_X(eye), .cy = LENS_CENTRE_Y };
        if (view_map(&views[eye], roll[eye], fb->var.xres, fb->var.yres) != 0)
            return -1;
    }

    char shown[4096] = "\x01", status[4096];   /* never matches, so the first pass draws */
    while (deadline <= 0 || now() < deadline) {
        read_status(status, sizeof(status));
        if (display_taken()) {
            strcpy(shown, "\x01");   /* draw again once it's given back */
        } else if (strcmp(status, shown) != 0) {
            render(rgb, &font, status);
            uint8_t *page = fb_back(fb);
            memset(page, 0, (size_t)fb->fix.line_length * fb->var.yres);
            for (int eye = 0; eye < 2; eye++)
                view_draw(&views[eye], fb, page, rgb);
            fb_flip(fb);
            strcpy(shown, status);
        }
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
