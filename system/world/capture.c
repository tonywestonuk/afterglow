/*
 * Capture, for afterglowd's video stream (start_capture): while it's on, each new frame is also scaled
 * down on the GPU and read back without waiting, into a small shared file that afterglowd reads and sends
 * on as Motion JPEG. The file is a header and two frame slots, written in turn: the header's sequence
 * says which slot is newest, and changes only once a slot is whole.
 *   "eyes" is the eye buffers, undistorted; "panels" the frame warped for the lenses. EYE is left, right
 *   or both; rows are top first, as the wearer sees it.
 */
#include <errno.h>
#include <fcntl.h>
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
    sscanf(args, "%15s %15s %15s %d %d", what, view, eye, &width, &height);
    if (strcmp(what, "stop") == 0) {
        stop();
        snprintf(reply, size, "capture stopped\n");
        return;
    }
    if (strcmp(what, "start") != 0 || width < 64 || height < 64 || width > FULL_W || height > FULL_H) {
        snprintf(reply, size, "error: capture start eyes|panels left|right|both WIDTH HEIGHT, or capture stop\n");
        return;
    }
    stop();
    cap.panels = strcmp(view, "panels") == 0;
    cap.eye = strcmp(eye, "right") == 0 ? 1 : strcmp(eye, "both") == 0 ? 2 : 0;
    cap.width = width, cap.height = height;
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
    wlog("capture: %dx%d, %s, %s", width, height, cap.panels ? "panels" : "eyes",
         cap.eye == 2 ? "both eyes" : cap.eye ? "right eye" : "left eye");
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
    /* The source as the wearer sees it: the eye buffers are side by side, left eye first, upright; the
     * panels' frame is mirrored left to right (lens.c), so its left eye is on the right, read backwards */
    int half = FULL_W / 2, x0, x1;
    if (cap.panels)
        x0 = cap.eye == 1 ? half : FULL_W, x1 = cap.eye == 0 ? half : 0;
    else
        x0 = cap.eye == 1 ? half : 0, x1 = cap.eye == 0 ? half : FULL_W;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cap.panels ? panel_fbo : eyes_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cap.fbo);
    /* Upside down, so that reading it back (bottom row first) gives the top row first */
    glBlitFramebuffer(x0, 0, x1, FULL_H, 0, cap.height, cap.width, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, cap.fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, cap.pbo);
    glReadPixels(0, 0, cap.width, cap.height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    cap.done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    cap.pending = 1;
}
