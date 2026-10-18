/*
 * Off-device test for afterglow's drawing code: runs the real display.c, bootanim.c and calibrate.c
 * against an in-memory framebuffer laid out like the Quest 1's (2880x1600, 32 bpp, shown rotated
 * 180°) and writes what the viewer would see as a PPM image.
 * Usage: display_test anim <anim> <frame> <deg left> <deg right> <out.ppm>
 *        display_test calib <deg left> <deg right> <selected: 0 both, 1 left, 2 right> <out.ppm>
 */
#include <stdarg.h>
#include <stdio.h>

#include "../init/display.c"
#include "../init/bootanim.c"
#include "../init/calibrate.c"

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int input_open(void) { return 0; }
int input_key(int timeout_ms) { (void)timeout_ms; return 0; }

static void fake_fb(struct fb *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->var.xres = 2880, fb->var.yres = 1600, fb->var.bits_per_pixel = 32;
    fb->var.red.offset = 16, fb->var.green.offset = 8, fb->var.blue.offset = 0;
    fb->var.red.length = fb->var.green.length = fb->var.blue.length = 8;
    fb->fix.line_length = 2880 * 4;
    fb->mem = calloc(fb->fix.line_length, fb->var.yres);
    fb->pages = 1;
}

/* What the viewer sees: the framebuffer rotated 180° */
static void write_view(const struct fb *fb, const char *path)
{
    FILE *out = fopen(path, "wb");
    fprintf(out, "P6 %u %u 255\n", fb->var.xres, fb->var.yres);
    for (int y = 0; y < (int)fb->var.yres; y++) {
        const uint32_t *row = (const uint32_t *)(fb->mem + (size_t)(fb->var.yres - 1 - y) * fb->fix.line_length);
        for (int x = 0; x < (int)fb->var.xres; x++) {
            uint32_t p = row[fb->var.xres - 1 - x];
            uint8_t rgb[3] = { p >> 16, p >> 8, p };
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
}

int main(int argc, char **argv)
{
    struct fb fb;
    fake_fb(&fb);
    if (argc == 7 && strcmp(argv[1], "anim") == 0) {
        double roll[2] = { atof(argv[4]) * M_PI / 180, atof(argv[5]) * M_PI / 180 };
        size_t size;
        struct eye eyes[2];
        const struct header *h = anim_load(argv[2], &size, eyes, &fb, roll);
        if (!h)
            return 1;
        const struct entry *index = (const struct entry *)(h + 1);
        for (unsigned f = 0; f <= (unsigned)atoi(argv[3]); f++) {
            for (int i = 0; i < 2; i++) {
                eyes[i].entry = &index[f * 2 + i];
                eyes[i].page = fb.mem;
                eye_frame(&eyes[i]);
                if (eyes[i].failed)
                    return 1;
            }
        }
        write_view(&fb, argv[6]);
        return 0;
    }
    if (argc == 6 && strcmp(argv[1], "calib") == 0) {
        double degrees[2] = { atof(argv[2]), atof(argv[3]) };
        int mode = atoi(argv[4]);
        for (int eye = 0; eye < 2; eye++) {
            struct view v = { .eye = eye, .width = W, .height = H, .x = LENS_CENTRE_X(eye) - W / 2,
                              .y = LENS_CENTRE_Y - H / 2, .cx = LENS_CENTRE_X(eye), .cy = LENS_CENTRE_Y };
            uint8_t *rgb = malloc((size_t)W * H * 3);
            render(rgb, eye, degrees[eye], mode == 0 || mode == eye + 1);
            if (view_map(&v, degrees[eye] * M_PI / 180, fb.var.xres, fb.var.yres) != 0)
                return 1;
            view_draw(&v, &fb, fb.mem, rgb);
        }
        write_view(&fb, argv[5]);
        return 0;
    }
    fprintf(stderr, "usage: see the comment at the top of %s\n", __FILE__);
    return 2;
}
