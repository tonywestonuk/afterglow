/*
 * Qualcomm's OpenGL ES driver, loaded by name from the headset's Android partition (see gles.c): every
 * EGL and GL function the world uses is a pointer filled in by gles_init().
 */
#ifndef WORLD_GLES_H
#define WORLD_GLES_H

#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define EGL_FUNCTIONS(X)                                                                                       \
    X(PFNEGLGETDISPLAYPROC, eglGetDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize)                             \
    X(PFNEGLQUERYSTRINGPROC, eglQueryString) X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig)                       \
    X(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface) X(PFNEGLCREATECONTEXTPROC, eglCreateContext)   \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLGETERRORPROC, eglGetError)

#define GL_FUNCTIONS(X)                                                                                        \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLCLEARCOLORPROC, glClearColor)  \
    X(PFNGLCLEARPROC, glClear) X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable)                    \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLLINEWIDTHPROC, glLineWidth)  \
    X(PFNGLGETERRORPROC, glGetError) X(PFNGLFINISHPROC, glFinish) X(PFNGLREADPIXELSPROC, glReadPixels)        \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                         \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                         \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLDELETESHADERPROC, glDeleteShader)                 \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader)                       \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLLINKPROGRAMPROC, glLinkProgram)               \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)               \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram)                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)     \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv) X(PFNGLUNIFORM2FVPROC, glUniform2fv)                                 \
    X(PFNGLUNIFORM3FVPROC, glUniform3fv) X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                 \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                           \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData)                                 \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)                       \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)       \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)     \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLDRAWARRAYSPROC, glDrawArrays)       \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)

#define X(type, name) extern type name;
EGL_FUNCTIONS(X)
GL_FUNCTIONS(X)
#undef X

/* Load the driver and make an offscreen OpenGL ES 3 context, width x height, 4x multisampled if it can be.
 * Returns 0, or -1 with the reason logged. */
int gles_init(int width, int height);

#endif
