/*
 * Qualcomm's OpenGL ES driver, loaded by name. Android's EGL loader finds the vendor driver through
 * system properties, which afterglow doesn't have, so the libraries are opened directly and each
 * function looked up (docs/apps.md). Rendering is offscreen: no Android window system is involved.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "gles.h"
#include "world.h"

#include <GLES2/gl2ext.h>   /* after gles.h, which brings in the GL types */

#define X(type, name) type name;
EGL_FUNCTIONS(X)
GL_FUNCTIONS(X)
#undef X

static int load_driver(void)
{
    void *egl = dlopen("/vendor/lib64/egl/libEGL_adreno.so", RTLD_NOW | RTLD_GLOBAL);
    void *gles = egl ? dlopen("/vendor/lib64/egl/libGLESv2_adreno.so", RTLD_NOW | RTLD_GLOBAL) : NULL;
    if (!gles) {
        wlog("loading Qualcomm's driver: %s", dlerror());
        return -1;
    }
#define X(type, name)                                                                                          \
    if (!(name = (type)dlsym(LIB, #name))) {                                                                   \
        wlog("the driver has no %s", #name);                                                                   \
        return -1;                                                                                             \
    }
#define LIB egl
    EGL_FUNCTIONS(X)
#undef LIB
#define LIB gles
    GL_FUNCTIONS(X)
#undef LIB
#undef X
    return 0;
}

/*
 * The frame is drawn into a texture through a framebuffer object. With EXT_multisampled_render_to_texture
 * the 4x multisampling lives only in the GPU's tile memory and is resolved as each tile is written out,
 * so a frame costs memory traffic for one 18 MB image rather than four samples a pixel plus a separate
 * resolve when it's read back.
 */
static int make_framebuffer(int width, int height, int *samples)
{
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC texture_ms =
        (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC depth_ms =
        (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    *samples = texture_ms && depth_ms && strstr(extensions, "GL_EXT_multisampled_render_to_texture") ? 4 : 0;
    GLuint fbo, colour, depth;
    glGenTextures(1, &colour);
    glBindTexture(GL_TEXTURE_2D, colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (*samples) {
        depth_ms(GL_RENDERBUFFER, *samples, GL_DEPTH_COMPONENT24, width, height);
        texture_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0, *samples);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        wlog("the offscreen framebuffer is incomplete (0x%x)", status);
        return -1;
    }
    return 0;
}

void gles_frame_drawn(void)
{
    static const GLenum depth = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
}

int gles_init(int width, int height)
{
    if (load_driver() != 0)
        return -1;
    EGLint major, minor, n = 0;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        wlog("eglInitialize failed (0x%x): is bringup.sh's gpu step done?", eglGetError());
        return -1;
    }
    /* The context's own surface is only a stand-in: frames go to the framebuffer object */
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig config;
    if (!eglChooseConfig(dpy, attrs, &config, 1, &n) || n == 0) {
        wlog("no suitable EGL config (0x%x)", eglGetError());
        return -1;
    }
    EGLint surface_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(dpy, config, surface_attrs);
    EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surface, surface, context)) {
        wlog("EGL surface or context failed (0x%x)", eglGetError());
        return -1;
    }
    int samples;
    if (make_framebuffer(width, height, &samples) != 0)
        return -1;
    wlog("GPU: %s, %s (%s)", glGetString(GL_RENDERER), glGetString(GL_VERSION),
         samples ? "4x multisampling in tile memory" : "no multisampling");
    return 0;
}
