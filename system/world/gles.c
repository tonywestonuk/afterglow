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
 * Frames are drawn into textures through framebuffer objects. With EXT_multisampled_render_to_texture
 * the 4x multisampling lives only in the GPU's tile memory and is resolved as each tile is written out,
 * so a frame costs memory traffic for one image rather than four samples a pixel plus a separate resolve.
 */
int gles_framebuffer(int width, int height, int depth, int multisample, GLuint *fbo, GLuint *colour)
{
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC texture_ms =
        (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC depth_ms =
        (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    int samples = multisample && texture_ms && depth_ms && strstr(extensions, "GL_EXT_multisampled_render_to_texture") ? 4 : 0;
    glGenTextures(1, colour);
    glBindTexture(GL_TEXTURE_2D, *colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    if (samples)
        texture_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *colour, 0, samples);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *colour, 0);
    if (depth) {
        GLuint rb;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        if (samples)
            depth_ms(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    }
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        wlog("a %dx%d framebuffer is incomplete (0x%x)", width, height, status);
        return -1;
    }
    if (multisample)
        wlog("eye buffers %dx%d, %s", width, height, samples ? "4x multisampled in tile memory" : "not multisampled");
    return 0;
}

void gles_frame_drawn(void)
{
    static const GLenum depth = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
}

int gles_init(void)
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
    wlog("GPU: %s, %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return 0;
}
