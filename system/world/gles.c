/*
 * Qualcomm's OpenGL ES driver, loaded by name. Android's EGL loader finds the vendor driver through
 * system properties, which afterglow doesn't have, so the libraries are opened directly and each
 * function looked up (docs/apps.md). Rendering is offscreen: no Android window system is involved.
 */
#include <dlfcn.h>
#include <stdio.h>

#include "gles.h"
#include "world.h"

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
    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                       EGL_DEPTH_SIZE, 24, EGL_SAMPLES, 4, EGL_NONE };
    EGLConfig config;
    if (!eglChooseConfig(dpy, attrs, &config, 1, &n) || n == 0) {
        attrs[sizeof(attrs) / sizeof(attrs[0]) - 2] = 0;   /* no multisampling */
        if (!eglChooseConfig(dpy, attrs, &config, 1, &n) || n == 0) {
            wlog("no suitable EGL config (0x%x)", eglGetError());
            return -1;
        }
    }
    EGLint surface_attrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(dpy, config, surface_attrs);
    EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surface, surface, context)) {
        wlog("EGL surface or context failed (0x%x)", eglGetError());
        return -1;
    }
    wlog("GPU: %s, %s (%s multisampling)", glGetString(GL_RENDERER), glGetString(GL_VERSION),
         attrs[sizeof(attrs) / sizeof(attrs[0]) - 2] ? "4x" : "no");
    return 0;
}
