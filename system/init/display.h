#ifndef AFTERGLOW_DISPLAY_H
#define AFTERGLOW_DISPLAY_H

#include <stdint.h>
#include <linux/fb.h>

/*
 * Lens centres on the Quest 1's 2880x1600 screen as the viewer sees it (eye 0 is the left eye):
 * where Meta's splash logo appears.
 */
#define LENS_CENTRE_X(eye) ((eye) ? 2028 : 852)
#define LENS_CENTRE_Y 865

struct fb {
    int fd;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *mem;
    unsigned pages, page;
};

int fb_open(struct fb *fb, const char *path);
/* The page to draw into: the hidden one when double-buffered */
uint8_t *fb_back(struct fb *fb);
void fb_flip(struct fb *fb);

/*
 * One eye's image: width x height RGB, top-left at (x, y) as the viewer sees the screen, turned
 * about (cx, cy) and drawn through a sampling map (source pixel and bilinear weights for each
 * screen pixel in dx,dy,dw,dh, clipped to the eye's half of the screen).
 */
struct view {
    int eye, width, height, x, y;
    double cx, cy;
    int dx, dy, dw, dh;
    int32_t *src;
    uint8_t *fx, *fy;
};

/* (Re)compute the sampling map for a turn of `angle` radians, counterclockwise as the viewer sees it */
int view_map(struct view *v, double angle, int xres, int yres);
/* Draw `rgb` (the view's image) into a framebuffer page, compensating for the panels' 180° rotation */
void view_draw(const struct view *v, const struct fb *fb, uint8_t *page, const uint8_t *rgb);

#endif
