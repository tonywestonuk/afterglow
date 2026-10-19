/*
 * Boot animation player: shows a pre-rendered stereo animation on the framebuffer.
 *
 * File layout (little-endian, written by tools/pack_anim.py):
 *   header  "AGLANIM1", then u32 fps, frames, loop_start, width, height,
 *           s32 eye_x[2], eye_y[2]        top-left of each eye's image, as the viewer sees the
 *                                         screen (eye 0 is the left eye)
 *           s32 centre_x[2], centre_y[2]  each eye's lens centre, in the same coordinates
 *   index   frames x 2 eyes x { u32 offset, size, flags }   flags bit 0: keyframe
 *   data    one zstd frame per entry, decompressing to width x height RGB888. A keyframe holds the
 *           pixels; any other frame holds the bytewise difference (mod 256) from that eye's
 *           previous frame. Frames 0 and loop_start are keyframes, so the loop can restart there.
 *
 * Each eye's image is turned about its lens centre to cancel that panel's roll. The two eyes are
 * decoded on separate threads, then drawn in bands across more cores, so a frame takes less than
 * one 72 Hz refresh (13.9 ms) and the page flip that waits for it still lands on schedule.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zstd.h"
#include "afterglow.h"
#include "display.h"

struct header {
    char magic[8];
    uint32_t fps, frames, loop_start, width, height;
    int32_t eye_x[2], eye_y[2];
    int32_t centre_x[2], centre_y[2];
};

struct entry {
    uint32_t offset, size, flags;
};

struct eye {
    uint8_t *rgb;             /* the current frame, width x height RGB */
    uint8_t *delta;
    ZSTD_DCtx *dctx;
    struct view view;
    /* Per-frame job */
    const uint8_t *file;
    size_t file_size;
    const struct entry *entry;
    const struct fb *fb;
    uint8_t *page;
    int failed;
};

/* Rows of the screen drawn by one thread: each eye is split into BANDS horizontal bands */
#define BANDS 3

struct band {
    struct eye *eye;
    int row0, row1;
};

/* Thread body: decode this eye's next frame (each frame builds on the last, so one thread per eye) */
static void *eye_decode(void *arg)
{
    struct eye *e = arg;
    size_t frame_bytes = (size_t)e->view.width * e->view.height * 3;
    int key = e->entry->flags & 1;
    if ((size_t)e->entry->offset + e->entry->size > e->file_size) {
        e->failed = 1;
        return NULL;
    }
    size_t n = ZSTD_decompressDCtx(e->dctx, key ? e->rgb : e->delta, frame_bytes,
                                   e->file + e->entry->offset, e->entry->size);
    if (ZSTD_isError(n) || n != frame_bytes) {
        e->failed = 1;
        return NULL;
    }
    if (!key)
        for (size_t i = 0; i < frame_bytes; i++)
            e->rgb[i] += e->delta[i];
    return NULL;
}

/* Thread body: draw one band of an eye into the back page */
static void *draw_band(void *arg)
{
    struct band *b = arg;
    view_draw_rows(&b->eye->view, b->eye->fb, b->eye->page, b->eye->rgb, b->row0, b->row1);
    return NULL;
}

/* Run fn on each of n argument structs (size bytes apart) in parallel, and wait for all */
static void run_parallel(void *(*fn)(void *), void *args, size_t size, int n)
{
    pthread_t threads[2 * BANDS];
    int started[2 * BANDS];
    for (int i = 0; i < n; i++) {
        void *arg = (char *)args + i * size;
        started[i] = pthread_create(&threads[i], NULL, fn, arg) == 0;
        if (!started[i])
            fn(arg);   /* run inline if a thread can't be started */
    }
    for (int i = 0; i < n; i++)
        if (started[i])
            pthread_join(threads[i], NULL);
}

/* Decode both eyes' next frames, then draw them into a page in bands */
static int eye_frame_pair(struct eye eyes[2], const struct entry *entries, uint8_t *page)
{
    for (int i = 0; i < 2; i++)
        eyes[i].entry = &entries[i], eyes[i].page = page;
    run_parallel(eye_decode, eyes, sizeof(*eyes), 2);
    if (eyes[0].failed || eyes[1].failed)
        return -1;
    struct band bands[2 * BANDS];
    for (int i = 0; i < 2 * BANDS; i++) {
        struct eye *e = &eyes[i / BANDS];
        int b = i % BANDS;
        bands[i] = (struct band){ e, e->view.dh * b / BANDS, e->view.dh * (b + 1) / BANDS };
    }
    run_parallel(draw_band, bands, sizeof(*bands), 2 * BANDS);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        klog("open %s: %s", path, strerror(errno));
        return NULL;
    }
    uint8_t *buf = malloc(st.st_size);
    size_t done = 0;
    while (buf && done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done, st.st_size - done);
        if (n <= 0) {
            free(buf);
            buf = NULL;
            break;
        }
        done += n;
    }
    close(fd);
    *size = done;
    return buf;
}

/* Load an animation and set up both eyes (decoders and sampling maps) for a framebuffer */
static const struct header *anim_load(const char *path, size_t *size, struct eye eyes[2], const struct fb *fb,
                                      const double roll[2])
{
    uint8_t *file = read_file(path, size);
    if (!file)
        return NULL;
    const struct header *h = (const struct header *)file;
    if (*size < sizeof(*h) || memcmp(h->magic, "AGLANIM1", 8) != 0 || h->frames == 0 || h->fps == 0 ||
        h->loop_start >= h->frames || h->width < 2 || h->height < 2 ||
        *size < sizeof(*h) + (size_t)h->frames * 2 * sizeof(struct entry)) {
        klog("%s: not a valid animation", path);
        return NULL;
    }
    klog("animation: %u frames at %u fps, loop from %u, %ux%u per eye, %zu bytes",
         h->frames, h->fps, h->loop_start, h->width, h->height, *size);

    size_t frame_bytes = (size_t)h->width * h->height * 3;
    memset(eyes, 0, 2 * sizeof(*eyes));
    for (int i = 0; i < 2; i++) {
        struct eye *e = &eyes[i];
        e->rgb = malloc(frame_bytes);
        e->delta = malloc(frame_bytes);
        e->dctx = ZSTD_createDCtx();
        e->file = file, e->file_size = *size, e->fb = fb;
        e->view = (struct view){ .eye = i, .width = h->width, .height = h->height, .x = h->eye_x[i],
                                 .y = h->eye_y[i], .cx = h->centre_x[i], .cy = h->centre_y[i] };
        if (!e->rgb || !e->delta || !e->dctx || view_map(&e->view, roll[i], fb->var.xres, fb->var.yres) != 0) {
            klog("eye %d: out of memory or off screen", i);
            return NULL;
        }
        klog("eye %d: roll %.4f rad, drawn at %d,%d size %dx%d", i, roll[i], e->view.dx, e->view.dy,
             e->view.dw, e->view.dh);
    }
    return h;
}

int bootanim_play(const char *anim_path, struct fb *fb, const double roll[2], double deadline,
                  const volatile int *stop)
{
    size_t size;
    struct eye eyes[2];
    const struct header *h = anim_load(anim_path, &size, eyes, fb, roll);
    if (!h)
        return -1;
    const struct entry *index = (const struct entry *)(h + 1);

    double period = 1.0 / h->fps, start = now(), work = 0;
    unsigned shown = 0, late = 0;
    for (unsigned f = 0;; f = f + 1 < h->frames ? f + 1 : h->loop_start) {
        double t0 = now();
        if ((deadline > 0 && t0 >= deadline) || (*stop && shown >= h->loop_start))   /* intro always plays */
            break;

        /* Both eyes in parallel, into the hidden page */
        if (eye_frame_pair(eyes, &index[f * 2], fb_back(fb)) != 0) {
            klog("frame %u: couldn't decode", f);
            return -1;
        }
        work += now() - t0;
        fb_flip(fb);
        shown++;

        /* Pace to the animation's frame rate */
        double wait = start + shown * period - now();
        if (wait > 0) {
            struct timespec ts = { (time_t)wait, (long)((wait - (time_t)wait) * 1e9) };
            nanosleep(&ts, NULL);
        } else if (wait < -period) {
            late++;
            start = now() - shown * period;   /* fell behind: don't try to catch up */
        }
        if (f + 1 == h->frames)
            klog("shown %u frames, %.1f ms decode+draw per frame, %u late", shown, 1000 * work / shown, late);
    }
    klog("animation stopped after %u frames", shown);
    return 0;
}
