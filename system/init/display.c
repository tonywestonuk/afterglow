/*
 * Framebuffer access and per-eye drawing. The Quest 1's panels show the framebuffer rotated 180°:
 * viewer (x, y) is framebuffer (xres-1-x, yres-1-y), which also puts the left half of the
 * framebuffer in front of the right eye. Everything here takes viewer coordinates.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "afterglow.h"
#include "display.h"

int fb_open(struct fb *fb, const char *path)
{
    fb->fd = open(path, O_RDWR | O_CLOEXEC);
    if (fb->fd < 0) {
        klog("open %s: %s", path, strerror(errno));
        return -1;
    }
    if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0)
        klog("FBIOBLANK unblank: %s", strerror(errno));
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) != 0 || ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) != 0) {
        klog("framebuffer info: %s", strerror(errno));
        return -1;
    }
    klog("fb '%s': %ux%u (virtual %ux%u), %u bpp, R%u/%u G%u/%u B%u/%u, line %u, mem %u",
         fb->fix.id, fb->var.xres, fb->var.yres, fb->var.xres_virtual, fb->var.yres_virtual,
         fb->var.bits_per_pixel, fb->var.red.offset, fb->var.red.length, fb->var.green.offset,
         fb->var.green.length, fb->var.blue.offset, fb->var.blue.length, fb->fix.line_length,
         fb->fix.smem_len);
    if (fb->var.bits_per_pixel != 32) {
        klog("unsupported %u bpp", fb->var.bits_per_pixel);
        return -1;
    }
    fb->mem = mmap(NULL, fb->fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        klog("mmap framebuffer: %s", strerror(errno));
        return -1;
    }
    /* Double-buffer when there's room for a second screen */
    size_t screen = (size_t)fb->fix.line_length * fb->var.yres;
    fb->pages = fb->var.yres_virtual >= 2 * fb->var.yres && fb->fix.smem_len >= 2 * screen ? 2 : 1;
    fb->page = 0;
    memset(fb->mem, 0, fb->pages * screen);
    klog("framebuffer ready, %u page(s)", fb->pages);
    return 0;
}

uint8_t *fb_back(struct fb *fb)
{
    unsigned page = fb->pages == 2 ? fb->page ^ 1 : 0;
    return fb->mem + (size_t)page * fb->fix.line_length * fb->var.yres;
}

void fb_flip(struct fb *fb)
{
    static int warned;
    if (fb->pages == 2)
        fb->page ^= 1;
    fb->var.yoffset = fb->page * fb->var.yres;
    if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var) != 0 && !warned++)
        klog("FBIOPAN_DISPLAY: %s", strerror(errno));
}

int view_map(struct view *v, double angle, int xres, int yres)
{
    free(v->src), free(v->fx), free(v->fy);
    v->src = NULL, v->fx = v->fy = NULL;

    double c = cos(angle), s = sin(angle);
    double x0 = v->x, y0 = v->y, x1 = x0 + v->width, y1 = y0 + v->height;
    double corners[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
    double lo_x = 1e9, lo_y = 1e9, hi_x = -1e9, hi_y = -1e9;
    for (int i = 0; i < 4; i++) {
        /* Counterclockwise on screen, where y points down */
        double x = v->cx + c * (corners[i][0] - v->cx) + s * (corners[i][1] - v->cy);
        double y = v->cy - s * (corners[i][0] - v->cx) + c * (corners[i][1] - v->cy);
        lo_x = fmin(lo_x, x), hi_x = fmax(hi_x, x), lo_y = fmin(lo_y, y), hi_y = fmax(hi_y, y);
    }
    int half = xres / 2;
    v->dx = (int)fmax(floor(lo_x), v->eye * half);
    v->dy = (int)fmax(floor(lo_y), 0);
    v->dw = (int)fmin(ceil(hi_x), (v->eye + 1) * half) - v->dx;
    v->dh = (int)fmin(ceil(hi_y), yres) - v->dy;
    if (v->dw <= 0 || v->dh <= 0)
        return -1;

    size_t n = (size_t)v->dw * v->dh;
    v->src = malloc(n * sizeof(*v->src));
    v->fx = malloc(n);
    v->fy = malloc(n);
    if (!v->src || !v->fx || !v->fy)
        return -1;
    for (int y = 0; y < v->dh; y++) {
        for (int x = 0; x < v->dw; x++) {
            /* Inverse rotation of this screen pixel's centre into the image */
            double px = v->dx + x + 0.5 - v->cx, py = v->dy + y + 0.5 - v->cy;
            double u = v->cx + c * px - s * py - x0 - 0.5;
            double w = v->cy + s * px + c * py - y0 - 0.5;
            int iu = (int)floor(u), iw = (int)floor(w);
            size_t i = (size_t)y * v->dw + x;
            if (iu < 0 || iw < 0 || iu >= v->width - 1 || iw >= v->height - 1) {
                v->src[i] = -1;
                continue;
            }
            v->src[i] = iw * v->width + iu;
            v->fx[i] = (uint8_t)((u - iu) * 256);
            v->fy[i] = (uint8_t)((w - iw) * 256);
        }
    }
    return 0;
}

void view_draw(const struct view *v, const struct fb *fb, uint8_t *page, const uint8_t *rgb)
{
    view_draw_rows(v, fb, page, rgb, 0, v->dh);
}

void view_draw_rows(const struct view *v, const struct fb *fb, uint8_t *page, const uint8_t *rgb, int row0, int row1)
{
    const struct fb_var_screeninfo *var = &fb->var;
    size_t stride = (size_t)v->width * 3;
    uint32_t black = var->transp.length ? 0xffu << var->transp.offset : 0;
    for (int y = row0 < 0 ? 0 : row0; y < row1 && y < v->dh; y++) {
        uint32_t *row = (uint32_t *)(page + (size_t)(var->yres - 1 - (v->dy + y)) * fb->fix.line_length);
        uint32_t *d = row + (var->xres - 1 - v->dx);
        const int32_t *src = v->src + (size_t)y * v->dw;
        const uint8_t *fx = v->fx + (size_t)y * v->dw, *fy = v->fy + (size_t)y * v->dw;
        for (int x = 0; x < v->dw; x++) {
            uint32_t pixel = black;
            if (src[x] >= 0) {
                const uint8_t *p = rgb + (size_t)src[x] * 3, *q = p + stride;
                /* Most of an eye's area is black: skip the blend when all four samples are */
                if ((p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | q[0] | q[1] | q[2] | q[3] | q[4] | q[5]) == 0) {
                    d[-x] = black;
                    continue;
                }
                unsigned a = fx[x], b = fy[x], c[3];
                for (int ch = 0; ch < 3; ch++) {
                    unsigned top = p[ch] * (256 - a) + p[3 + ch] * a;
                    unsigned bottom = q[ch] * (256 - a) + q[3 + ch] * a;
                    c[ch] = (top * (256 - b) + bottom * b) >> 16;
                }
                pixel = (c[0] >> (8 - var->red.length)) << var->red.offset |
                        (c[1] >> (8 - var->green.length)) << var->green.offset |
                        (c[2] >> (8 - var->blue.length)) << var->blue.offset |
                        (var->transp.length ? 0xffu << var->transp.offset : 0);
            }
            d[-x] = pixel;
        }
    }
}
