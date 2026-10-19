/*
 * Pictures made on the CPU, as RGBA (8 bits a channel, rows top first), for textures: images from files
 * (stb_image), text in afterglow's font (stb_truetype, drawn from distance fields so it can be made
 * bolder), and the "Claude at work" sign put beside placeholders.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#pragma GCC diagnostic pop

#include "world.h"

static stbtt_fontinfo font;
static unsigned char *font_data;

int paint_init(const char *font_path)
{
    FILE *f = fopen(font_path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size > 0 && (font_data = malloc(size)) && fseek(f, 0, SEEK_SET) == 0 && fread(font_data, 1, size, f) == (size_t)size &&
        stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
        fclose(f);
        return 0;
    }
    if (f)
        fclose(f);
    free(font_data);
    font_data = NULL;
    return -1;
}

uint8_t *paint_load(const char *path, int *width, int *height)
{
    return stbi_load(path, width, height, NULL, 4);
}

const char *paint_load_error(void)
{
    return stbi_failure_reason();
}

/* Decode one UTF-8 character and step past it ('?' for anything malformed) */
static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (extra)
        c &= 0x3f >> extra;
    for (; extra > 0 && (*p & 0xc0) == 0x80; extra--)
        c = (c << 6) | (*p++ & 0x3f);
    *s = (const char *)p;
    return extra ? '?' : c;
}

/* One line's width in pixels, up to its end or a newline */
static float line_width(const char *s, float scale)
{
    float x = 0;
    int prev = 0;
    while (*s && *s != '\n') {
        int c = utf8_next(&s), advance, lsb;
        if (prev)
            x += scale * stbtt_GetCodepointKernAdvance(&font, prev, c);
        stbtt_GetCodepointHMetrics(&font, c, &advance, &lsb);
        x += scale * advance;
        prev = c;
    }
    return x;
}

/*
 * Text, each line centred, `pixel_height` from ascender to descender, in ink (0xRRGGBB) on transparent.
 * weight thickens every stroke by that many pixels each side (afterglow's font is a light one).
 */
uint8_t *paint_text(const char *text, int pixel_height, uint32_t ink, float weight, int *width, int *height)
{
    if (!font_data)
        return NULL;
    float scale = stbtt_ScaleForPixelHeight(&font, pixel_height);
    int ascent, descent, gap, pad = (int)ceilf(weight) + 2, lines = 1;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    float widest = 0, line_height = (ascent - descent + gap) * scale;
    for (const char *s = text;; s++) {
        widest = fmaxf(widest, line_width(s, scale));
        if (!(s = strchr(s, '\n')))
            break;
        lines++;
    }
    *width = (int)ceilf(widest) + 2 * pad;
    *height = (int)ceilf(lines * line_height - gap * scale) + 2 * pad;
    if (*width > 4096 || *height > 4096)
        return NULL;
    uint8_t *alpha = calloc((size_t)*width * *height, 1), *rgba = malloc((size_t)*width * *height * 4);
    if (!alpha || !rgba) {
        free(alpha);
        free(rgba);
        return NULL;
    }
    float baseline = pad + ascent * scale;
    for (const char *s = text; s; s = strchr(s, '\n') ? strchr(s, '\n') + 1 : NULL, baseline += line_height) {
        float x = pad + (widest - line_width(s, scale)) / 2;
        int prev = 0;
        while (*s && *s != '\n') {
            int c = utf8_next(&s), advance, lsb, gw, gh, gx, gy;
            if (prev)
                x += scale * stbtt_GetCodepointKernAdvance(&font, prev, c);
            stbtt_GetCodepointHMetrics(&font, c, &advance, &lsb);
            /* Distance field: 128 on the outline, rising 128/pad per pixel inwards */
            uint8_t *sdf = stbtt_GetCodepointSDF(&font, scale, c, pad, 128, 128.0f / pad, &gw, &gh, &gx, &gy);
            for (int y = 0; sdf && y < gh; y++)
                for (int k = 0; k < gw; k++) {
                    int px = (int)floorf(x) + gx + k, py = (int)floorf(baseline) + gy + y;
                    if (px < 0 || px >= *width || py < 0 || py >= *height)
                        continue;
                    float inside = (sdf[y * gw + k] - 128) / (128.0f / pad) + weight + 0.5f;
                    int a = (int)(fminf(fmaxf(inside, 0), 1) * 255 + 0.5f);
                    uint8_t *d = &alpha[(size_t)py * *width + px];
                    if (a > *d)
                        *d = (uint8_t)a;
                }
            stbtt_FreeSDF(sdf, NULL);
            x += scale * advance;
            prev = c;
        }
    }
    for (size_t i = 0; i < (size_t)*width * *height; i++) {
        rgba[i * 4] = ink >> 16, rgba[i * 4 + 1] = ink >> 8, rgba[i * 4 + 2] = ink;
        rgba[i * 4 + 3] = alpha[i];
    }
    free(alpha);
    return rgba;
}

/* ---- The "Claude at work" sign ---- */

/* Distances in pixels, negative inside */
static float segment(float px, float py, float ax, float ay, float bx, float by)
{
    float ex = bx - ax, ey = by - ay, wx = px - ax, wy = py - ay;
    float t = fminf(fmaxf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0), 1);
    return hypotf(wx - ex * t, wy - ey * t);
}

static float polygon(float px, float py, const float (*v)[2], int n)
{
    float d = INFINITY, sign = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        d = fminf(d, segment(px, py, v[i][0], v[i][1], v[j][0], v[j][1]));
        int a = py >= v[i][1], b = py < v[j][1];
        int c = (v[j][0] - v[i][0]) * (py - v[i][1]) > (v[j][1] - v[i][1]) * (px - v[i][0]);
        if ((a && b && c) || (!a && !b && !c))
            sign = -sign;
    }
    return sign * d;
}

static float coverage(float d)
{
    return fminf(fmaxf(0.5f - d, 0), 1);
}

/* The warning triangle, 512x512: apex up, red border, corners rounded */
static float triangle(float px, float py, float border)
{
    const float cx = 256, cy = 305.1f, inradius = 138.6f, round = 28;
    const float k = (inradius - round) / inradius;
    const float corners[3][2] = { { 256, 28 }, { 16, 443.7f }, { 496, 443.7f } };
    float v[3][2];
    for (int i = 0; i < 3; i++)
        v[i][0] = cx + (corners[i][0] - cx) * k, v[i][1] = cy + (corners[i][1] - cy) * k;
    return polygon(px, py, v, 3) - round + border;
}

/* A man digging, beside a heap of earth, facing right */
static float digger(float px, float py)
{
    static const float limbs[][5] = {
        /* from x, y; to x, y; radius */
        { 226, 254, 197, 312, 16 },   /* body */
        { 197, 312, 181, 344, 11 }, { 181, 344, 166, 375, 11 },   /* back leg */
        { 197, 312, 221, 342, 11 }, { 221, 342, 230, 375, 11 },   /* front leg */
        { 226, 258, 251, 281, 9 },                                /* arms */
        { 226, 258, 246, 300, 9 }, { 246, 300, 276, 316, 9 },
        { 236, 262, 302, 352, 5 },                                /* spade handle */
        { 298, 347, 310, 362, 12 },                               /* blade */
    };
    float d = hypotf(px - 242, py - 222) - 19;   /* head */
    for (size_t i = 0; i < sizeof(limbs) / sizeof(limbs[0]); i++)
        d = fminf(d, segment(px, py, limbs[i][0], limbs[i][1], limbs[i][2], limbs[i][3]) - limbs[i][4]);
    float heap[18][2];
    for (int i = 0; i < 17; i++) {
        float a = (float)M_PI * i / 16;
        heap[i][0] = 320 + 64 * cosf(a), heap[i][1] = 376 - 40 * sinf(a);
    }
    heap[17][0] = 320, heap[17][1] = 376;
    return fminf(d, polygon(px, py, (const float(*)[2])heap, 18));
}

/*
 * The sign's face, 512x640: the triangle above (rows 0-511) and a plate saying CLAUDE AT WORK below
 * (rows 512-639), transparent around both. *mask is the same shapes in plain white, for its back.
 */
uint8_t *paint_sign(int *width, int *height, uint8_t **mask)
{
    const int w = 512, h = 640;
    const uint8_t red[3] = { 193, 18, 31 }, white[3] = { 246, 246, 242 }, black[3] = { 18, 18, 18 };
    uint8_t *rgba = malloc(w * h * 4);
    *mask = malloc(w * h * 4);
    int tw = 0, th = 0, px_height = 76;
    uint8_t *words = NULL;
    while (px_height > 20) {   /* as large as fits on the plate */
        free(words);
        words = paint_text("CLAUDE AT WORK", px_height, 0, px_height * 0.05f, &tw, &th);
        if (!words || tw <= 440)
            break;
        px_height -= 4;
    }
    if (!rgba || !*mask || !words) {
        free(rgba);
        free(*mask);
        free(words);
        return NULL;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = x + 0.5f, fy = y + 0.5f, edge, inner, ink;
            if (y < 512) {
                edge = triangle(fx, fy, 0);
                inner = coverage(triangle(fx, fy, 50));
                ink = coverage(digger(fx, fy)) * inner;
            } else {
                /* rounded rectangle 504x120 with a black rim */
                float qx = fabsf(fx - 256) - 252 + 14, qy = fabsf(fy - 576) - 60 + 14;
                edge = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - 14;
                inner = coverage(edge + 7);
                int tx = x - (w - tw) / 2, ty = y - 512 - (128 - th) / 2;
                ink = tx >= 0 && tx < tw && ty >= 0 && ty < th ? words[(ty * tw + tx) * 4 + 3] / 255.0f : 0;
                ink = fmaxf(ink, 1 - inner);   /* the rim */
            }
            uint8_t *d = &rgba[(y * w + x) * 4], *m = &(*mask)[(y * w + x) * 4];
            for (int k = 0; k < 3; k++) {
                float c = y < 512 ? red[k] + (white[k] - red[k]) * inner : white[k];
                d[k] = (uint8_t)(c + (black[k] - c) * ink + 0.5f);
                m[k] = 255;
            }
            d[3] = m[3] = (uint8_t)(coverage(edge) * 255 + 0.5f);
        }
    free(words);
    *width = w, *height = h;
    return rgba;
}
