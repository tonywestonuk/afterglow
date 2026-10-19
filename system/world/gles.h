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
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLGETERRORPROC, eglGetError)                               \
    X(PFNEGLGETPROCADDRESSPROC, eglGetProcAddress)

#define GL_FUNCTIONS(X)                                                                                        \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLCLEARCOLORPROC, glClearColor)  \
    X(PFNGLCLEARPROC, glClear) X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable)                    \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLLINEWIDTHPROC, glLineWidth)  \
    X(PFNGLGETERRORPROC, glGetError) X(PFNGLFINISHPROC, glFinish) X(PFNGLFLUSHPROC, glFlush) X(PFNGLREADPIXELSPROC, glReadPixels)        \
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
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) X(PFNGLUNIFORM1IPROC, glUniform1i)                               \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv) X(PFNGLGENTEXTURESPROC, glGenTextures)                   \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture)                       \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                           \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLTEXPARAMETERFPROC, glTexParameterf)                     \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) X(PFNGLGETFLOATVPROC, glGetFloatv)                           \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                    \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)   \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)         \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                                    \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                            \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                              \
    X(PFNGLINVALIDATEFRAMEBUFFERPROC, glInvalidateFramebuffer) X(PFNGLFENCESYNCPROC, glFenceSync)                     \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) X(PFNGLDELETESYNCPROC, glDeleteSync)

#define X(type, name) extern type name;
EGL_FUNCTIONS(X)
GL_FUNCTIONS(X)
#undef X

/* Load the driver and make an OpenGL ES 3 context with an offscreen framebuffer, width x height, which
 * stays bound: 4x multisampled within the GPU's tile memory if it can be, so that only the resolved
 * image is written out. Returns 0, or -1 with the reason logged. */
int gles_init(int width, int height);
/* After drawing a frame: depth needn't be kept, so it never leaves the GPU */
void gles_frame_drawn(void);

#endif
