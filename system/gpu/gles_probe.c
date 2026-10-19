/*
 * GPU probe: can afterglow draw with the Quest's Adreno 540 through Qualcomm's own driver, from the
 * headset's read-only Android partition? Android's EGL loader finds the driver through system
 * properties, which afterglow doesn't have, so this loads Qualcomm's libraries directly and looks up
 * the EGL and OpenGL ES functions by name. It renders a wireframe floor in stereo (each eye from its
 * own position) into an offscreen buffer, reports how fast, and copies the last frame to the screen.
 *
 * Built with the Android NDK (system/gpu/build.sh), so it runs on Android's own C library from the
 * system partition, like Qualcomm's daemons in bringup.sh. Usage: gles_probe [frames]
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 2880   /* both eyes side by side, as the wearer sees them */
#define H 1600

#define EGL_FUNCTIONS(X)                                                                                       \
    X(PFNEGLGETDISPLAYPROC, eglGetDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize)                             \
    X(PFNEGLQUERYSTRINGPROC, eglQueryString) X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig)                       \
    X(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface) X(PFNEGLCREATECONTEXTPROC, eglCreateContext)   \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLGETERRORPROC, eglGetError)
#define GL_FUNCTIONS(X)                                                                                        \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLCLEARCOLORPROC, glClearColor)  \
    X(PFNGLCLEARPROC, glClear) X(PFNGLCREATESHADERPROC, glCreateShader)                                       \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader)                       \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                   \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader)                       \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                           \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram)                   \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)     \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)                                 \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)               \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLDRAWARRAYSPROC, glDrawArrays)       \
    X(PFNGLFINISHPROC, glFinish) X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLLINEWIDTHPROC, glLineWidth)      \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLGETERRORPROC, glGetError)

#define X(type, name) static type name;
EGL_FUNCTIONS(X)
GL_FUNCTIONS(X)
#undef X

static int load_driver(void)
{
    void *egl = dlopen("/vendor/lib64/egl/libEGL_adreno.so", RTLD_NOW | RTLD_GLOBAL);
    void *gles = egl ? dlopen("/vendor/lib64/egl/libGLESv2_adreno.so", RTLD_NOW | RTLD_GLOBAL) : NULL;
    if (!gles) {
        fprintf(stderr, "loading Qualcomm's driver: %s\n", dlerror());
        return -1;
    }
#define X(type, name)                                                                                          \
    if (!(name = (type)dlsym(LIB, #name))) {                                                                   \
        fprintf(stderr, "driver has no %s\n", #name);                                                         \
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

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    GLint ok;
    char log[1024];
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

/* Column-major 4x4 matrices: a * b */
static void mat_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = 0;
            for (int k = 0; k < 4; k++)
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
        }
    memcpy(out, r, sizeof(r));
}

/* Projection for one eye, then the head turned by yaw, then the eye's offset from the head's centre */
static void eye_matrix(float out[16], float fov, float aspect, float yaw, float eye_x)
{
    float f = 1 / tanf(fov / 2), n = 0.05f, far = 50;
    float proj[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + n) / (n - far), -1, 0, 0, 2 * far * n / (n - far), 0 };
    float eye[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye_x, 0, 0, 1 };
    float c = cosf(yaw), s = sinf(yaw);
    float turn[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1 };
    mat_mul(out, proj, eye);
    mat_mul(out, out, turn);
}

/* Show a rendered frame: the panels show the framebuffer rotated 180°, so each framebuffer row is the
 * same-numbered row of OpenGL's bottom-up image, reversed */
static int show(const uint32_t *image)
{
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
        var.xres != W || var.yres != H) {
        fprintf(stderr, "no %dx%d framebuffer\n", W, H);
        return -1;
    }
    uint8_t *mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
        return -1;
    uint8_t *page = mem + (size_t)var.yoffset * fix.line_length;
    for (int y = 0; y < H; y++) {
        uint32_t *dst = (uint32_t *)(page + (size_t)y * fix.line_length);
        const uint32_t *src = image + (size_t)y * W;
        for (int x = 0; x < W; x++)
            dst[W - 1 - x] = src[x];
    }
    munmap(mem, fix.smem_len);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 144;
    if (load_driver() != 0)
        return 1;

    EGLint major, minor, n = 0;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed (0x%x)\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d: %s, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));

    EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                       EGL_DEPTH_SIZE, 16, EGL_SAMPLES, 4, EGL_NONE };
    EGLConfig config;
    if (!eglChooseConfig(dpy, attrs, &config, 1, &n) || n == 0) {
        attrs[sizeof(attrs) / sizeof(attrs[0]) - 2] = 0;   /* no multisampling */
        if (!eglChooseConfig(dpy, attrs, &config, 1, &n) || n == 0) {
            fprintf(stderr, "no suitable EGL config (0x%x)\n", eglGetError());
            return 1;
        }
    }
    EGLint surface_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(dpy, config, surface_attrs);
    EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surface, surface, context)) {
        fprintf(stderr, "EGL surface or context failed (0x%x)\n", eglGetError());
        return 1;
    }
    printf("GL: %s / %s / %s (%s multisampling)\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION), attrs[sizeof(attrs) / sizeof(attrs[0]) - 2] ? "4x" : "no");

    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER,
        "#version 300 es\n"
        "layout(location = 0) in vec3 position;\n"
        "uniform mat4 mvp;\n"
        "out float distance;\n"
        "void main() { gl_Position = mvp * vec4(position, 1.0); distance = gl_Position.w; }\n"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER,
        "#version 300 es\n"
        "precision mediump float;\n"
        "in float distance;\n"
        "out vec4 colour;\n"
        "void main() { colour = vec4(vec3(1.0, 0.69, 0.44) * clamp(1.2 - distance / 12.0, 0.0, 1.0), 1.0); }\n"));
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "link: %s\n", log);
        return 1;
    }
    glUseProgram(program);
    GLint mvp = glGetUniformLocation(program, "mvp");

    /* A floor of 1 m squares, 1.6 m below the eyes, 20 m across */
    float lines[84][3];
    int v = 0;
    for (int i = -10; i <= 10; i++) {
        float a[4][3] = { { i, -1.6f, -10 }, { i, -1.6f, 10 }, { -10, -1.6f, i }, { 10, -1.6f, i } };
        memcpy(lines[v], a, sizeof(a));
        v += 4;
    }
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glEnable(GL_DEPTH_TEST);
    glLineWidth(2);

    double start = seconds();
    for (int f = 0; f < frames; f++) {
        glClearColor(0.02f, 0.02f, 0.04f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int eye = 0; eye < 2; eye++) {
            float m[16];
            eye_matrix(m, 90 * (float)M_PI / 180, (W / 2.0f) / H, f * 0.01f, eye ? 0.032f : -0.032f);
            glViewport(eye * W / 2, 0, W / 2, H);
            glUniformMatrix4fv(mvp, 1, GL_FALSE, m);
            glDrawArrays(GL_LINES, 0, v);
        }
        glFinish();
    }
    double drawn = seconds();
    uint32_t *image = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, image);
    double read = seconds();
    GLenum err = glGetError();
    printf("%d stereo frames of %dx%d in %.2f s: %.2f ms each (%.0f fps); reading one back took %.1f ms%s\n",
           frames, W, H, drawn - start, 1000 * (drawn - start) / frames, frames / (drawn - start),
           1000 * (read - drawn), err ? " (GL error)" : "");
    return show(image) == 0 ? 0 : 1;
}
