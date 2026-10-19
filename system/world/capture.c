/*
 * Capture, for afterglowd's video stream (start_capture): while it's on, each new frame is also scaled
 * down on the GPU and read back without waiting, into a small shared file that afterglowd reads and sends
 * on as Motion JPEG. The file is a header and two frame slots, written in turn: the header's sequence
 * says which slot is newest, and changes only once a slot is whole.
 *   "eyes" is the eye buffers, undistorted; "panels" the frame warped for the lenses. EYE is left, right
 *   or both; rows are top first, as the wearer sees it. ZOOM crops each eye to 1/ZOOM of its field
 *   around straight ahead (each eye sees more outwards than in, about 94° across in all), so a video
 *   looks like a normal camera's; "capture zoom Z" changes it while capturing.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gles.h"
#include "world.h"

#define CAPTURE_PATH "/tmp/afterglow-capture"
#define FULL_W 2880
#define FULL_H 1600

struct capture_header {
    char magic[8];             /* "AGCAP1" */
    uint32_t width, height;    /* of each frame, RGBA */
    uint32_t sequence;         /* frames written: the newest is in slot (sequence - 1) % 2 */
    uint32_t reserved[11];
};

static struct {
    int on, panels, eye, width, height;   /* eye: 0 left, 1 right, 2 both */
    float zoom;
    GLuint fbo, texture, pbo;
    GLsync done;
    int pending;
    struct capture_header *shared;
    size_t shared_size;
} cap;

static void stop(void)
{
    if (cap.done)
        glDeleteSync(cap.done), cap.done = 0;
    if (cap.pbo)
        glDeleteBuffers(1, &cap.pbo), cap.pbo = 0;
    if (cap.fbo)
        glDeleteFramebuffers(1, &cap.fbo), cap.fbo = 0;
    if (cap.texture)
        glDeleteTextures(1, &cap.texture), cap.texture = 0;
    if (cap.shared)
        munmap(cap.shared, cap.shared_size), cap.shared = NULL;
    unlink(CAPTURE_PATH);
    cap.on = cap.pending = 0;
}

void capture_command(const char *args, char *reply, size_t size)
{
    char what[16] = "", view[16] = "", eye[16] = "";
    int width = 0, height = 0;
    float zoom = 1;
    if (sscanf(args, "zoom %f", &zoom) == 1) {
        if (!cap.on || zoom < 1 || zoom > 4) {
            snprintf(reply, size, "error: not capturing, or a zoom outside 1 to 4\n");
            return;
        }
        cap.zoom = zoom;
        snprintf(reply, size, "zoom %.2f\n", zoom);
        return;
    }
    sscanf(args, "%15s %15s %15s %d %d %f", what, view, eye, &width, &height, &zoom);
    if (strcmp(what, "stop") == 0) {
        stop();
        snprintf(reply, size, "capture stopped\n");
        return;
    }
    if (strcmp(what, "start") != 0 || width < 64 || height < 64 || width > FULL_W || height > FULL_H || zoom < 1 ||
        zoom > 4) {
        snprintf(reply, size, "error: capture start eyes|panels left|right|both WIDTH HEIGHT [ZOOM], capture zoom Z, or "
                              "capture stop\n");
        return;
    }
    stop();
    cap.panels = strcmp(view, "panels") == 0;
    cap.eye = strcmp(eye, "right") == 0 ? 1 : strcmp(eye, "both") == 0 ? 2 : 0;
    cap.width = width, cap.height = height, cap.zoom = zoom;
    cap.shared_size = sizeof(struct capture_header) + 2 * (size_t)width * height * 4;
    int fd = open(CAPTURE_PATH, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)cap.shared_size) != 0 ||
        (cap.shared = mmap(NULL, cap.shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        cap.shared = NULL;
        snprintf(reply, size, "error: %s: %s\n", CAPTURE_PATH, strerror(errno));
        if (fd >= 0)
            close(fd);
        stop();
        return;
    }
    close(fd);
    memset(cap.shared, 0, sizeof(*cap.shared));
    memcpy(cap.shared->magic, "AGCAP1", 6);
    cap.shared->width = width, cap.shared->height = height;
    if (gles_framebuffer(width, height, 0, 0, &cap.fbo, &cap.texture) != 0) {
        snprintf(reply, size, "error: no framebuffer for capture\n");
        stop();
        return;
    }
    glGenBuffers(1, &cap.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, cap.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)width * height * 4, NULL, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    cap.on = 1;
    wlog("capture: %dx%d, %s, %s, zoom %.2f", width, height, cap.panels ? "panels" : "eyes",
         cap.eye == 2 ? "both eyes" : cap.eye ? "right eye" : "left eye", zoom);
    snprintf(reply, size, "capturing %dx%d to %s\n", width, height, CAPTURE_PATH);
}

/* After a frame is drawn: hand on the last capture if the GPU has finished it, and start the next */
void capture_frame(GLuint eye_buffers, GLuint panel_fbo)
{
    if (!cap.on)
        return;
    /* The eyes are drawn multisampled (in tile memory), and a scaling blit from a multisampled framebuffer
     * isn't allowed: read their texture, which holds the resolved picture, through a plain framebuffer */
    static GLuint eyes_read;
    if (!eyes_read) {
        glGenFramebuffers(1, &eyes_read);
        glBindFramebuffer(GL_FRAMEBUFFER, eyes_read);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye_buffers, 0);
    }
    if (cap.pending) {
        GLenum state = glClientWaitSync(cap.done, 0, 0);
        if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
            return;   /* not yet: skip this frame rather than wait */
        glDeleteSync(cap.done), cap.done = 0;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, cap.pbo);
        const uint8_t *pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)cap.width * cap.height * 4,
                                                 GL_MAP_READ_BIT);
        if (pixels) {
            uint32_t sequence = __atomic_load_n(&cap.shared->sequence, __ATOMIC_RELAXED);
            uint8_t *slot = (uint8_t *)(cap.shared + 1) + (size_t)(sequence % 2) * cap.width * cap.height * 4;
            memcpy(slot, pixels, (size_t)cap.width * cap.height * 4);
            __atomic_store_n(&cap.shared->sequence, sequence + 1, __ATOMIC_RELEASE);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        cap.pending = 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cap.panels ? panel_fbo : eyes_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cap.fbo);
    int half = FULL_W / 2, first = cap.eye == 1 ? 1 : 0, last = cap.eye == 0 ? 0 : 1;
    for (int eye = first; eye <= last; eye++) {
        /* This eye's crop, w x h around straight ahead, kept within its half */
        float w = half / cap.zoom, h = FULL_H / cap.zoom, cx, cy, f[4];
        if (cap.panels) {   /* around the lens centre, on the panels as the wearer sees them (y down) */
            lens_centre_on_screen(eye, &cx, &cy);
            cy = FULL_H - cy;
        } else {            /* where straight ahead lands in the eye buffer (y up) */
            lens_frustum(eye, f);
            cx = eye * half + half * -f[0] / (f[1] - f[0]), cy = FULL_H * -f[2] / (f[3] - f[2]);
        }
        float lo = eye * half;
        cx = fminf(fmaxf(cx, lo + w / 2), lo + half - w / 2);
        cy = fminf(fmaxf(cy, h / 2), FULL_H - h / 2);
        int sx0 = (int)(cx - w / 2), sx1 = (int)(cx + w / 2), sy0 = (int)(cy - h / 2), sy1 = (int)(cy + h / 2);
        if (cap.panels) {   /* the panels' frame is mirrored left to right (lens.c): read it backwards */
            int m0 = FULL_W - sx0, m1 = FULL_W - sx1;
            sx0 = m0, sx1 = m1;
        }
        int dx0 = cap.eye == 2 ? eye * cap.width / 2 : 0, dx1 = cap.eye == 2 ? (eye + 1) * cap.width / 2 : cap.width;
        /* Upside down, so that reading it back (bottom row first) gives the top row first */
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, cap.height, dx1, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, cap.fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, cap.pbo);
    glReadPixels(0, 0, cap.width, cap.height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    cap.done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    cap.pending = 1;
}
