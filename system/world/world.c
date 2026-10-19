/*
 * afterglow's world: the render loop, the scene and the control socket (docs/apps.md).
 *
 * Built with the Android NDK (system/world/build.sh) so it runs on Android's C library and can use
 * Qualcomm's GPU driver. afterglowd starts it and talks to it over /run/afterglow/world.sock, one
 * command per connection, the reply ending when the connection closes:
 *   load NAME PATH    load extension NAME from a WebAssembly file, replacing any loaded version
 *   unload NAME
 *   list
 *   quit
 * While it runs it owns the display: /run/afterglow/display-owner holds its pid, and /init's status
 * screen keeps off the screen.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "gles.h"
#include "world.h"

#define W 2880   /* both eyes side by side, as the wearer sees them */
#define H 1600
#define EYE_W (W / 2)
#define SOCKET_PATH "/run/afterglow/world.sock"
#define OWNER_PATH "/run/afterglow/display-owner"

/* Optics, in screen pixels as the wearer sees them (docs/hardware.md). No lens distortion correction yet. */
static const float lens_x[2] = { 852, 2028 }, lens_y = 865;
#define FOCAL_PX 720.0f   /* pixels per unit of tan(angle) at the lens centre: about 90° across an eye */
#define EYE_HEIGHT 1.6f
#define IPD 0.063f
#define FRAME_RATE 72

static FILE *kmsg;
static double started;
static volatile sig_atomic_t running = 1;
static float roll[2];   /* each panel's roll correction, radians, counterclockwise as the wearer sees it */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double world_time(void)
{
    return now() - started;
}

void wlog(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[%9.3f] %s\n", world_time(), line);
    fflush(stdout);
    if (kmsg) {
        fprintf(kmsg, "<6>world: %s\n", line);
        fflush(kmsg);
    }
}

/* ---- The scene ---- */

#define MAX_MESHES 1024
#define MAX_SHADERS 256
#define MAX_OBJECTS 1024
#define MAX_UNIFORMS 8
#define MAX_CHANGES 4096

struct mesh {
    int used, owner;
    GLuint vao, vbo, ibo;
    GLsizei count;
    GLenum primitive;
};

struct shader {
    int used, owner;
    GLuint program;
    GLint mvp, model, time, eye;
};

struct uniform {
    char name[32];
    float values[MAX_UNIFORM_FLOATS];
    int count;
};

struct object {
    int used, owner, mesh, shader, visible;
    char name[48];
    float model[16];
    struct uniform uniforms[MAX_UNIFORMS];
    int uniform_count;
};

/* Something an extension changed but doesn't own, as it was before that extension's first change */
enum { CHANGE_MESH, CHANGE_SHADER, CHANGE_TRANSFORM, CHANGE_VISIBLE, CHANGE_UNIFORMS, CHANGE_BACKGROUND };
struct change {
    int ext, kind, object;
    struct object before;
    float background[3];
};

static struct mesh meshes[MAX_MESHES];   /* ids are index + 1; 0 means none */
static struct shader shaders[MAX_SHADERS];
static struct object objects[MAX_OBJECTS];
static struct change changes[MAX_CHANGES];
static int change_count;
static float background[3] = { 0.02f, 0.02f, 0.04f };
static char error[2048];

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, sizeof(error), fmt, ap);
    va_end(ap);
    return -1;
}

const char *world_error(void)
{
    return error;
}

static struct mesh *mesh_get(int id)
{
    return id >= 1 && id <= MAX_MESHES && meshes[id - 1].used ? &meshes[id - 1] : NULL;
}

static struct shader *shader_get(int id)
{
    return id >= 1 && id <= MAX_SHADERS && shaders[id - 1].used ? &shaders[id - 1] : NULL;
}

static struct object *object_get(int id)
{
    return id >= 1 && id <= MAX_OBJECTS && objects[id - 1].used ? &objects[id - 1] : NULL;
}

/* Note what an extension is about to change on something it doesn't own: once per thing and kind */
static int remember(int ext, int kind, int object)
{
    if (ext == 0 || (kind != CHANGE_BACKGROUND && object_get(object)->owner == ext))
        return 0;
    for (int i = 0; i < change_count; i++)
        if (changes[i].ext == ext && changes[i].kind == kind && changes[i].object == object)
            return 0;
    if (change_count == MAX_CHANGES)
        return fail("too many changes to things this extension doesn't own (%d)", MAX_CHANGES);
    struct change *c = &changes[change_count++];
    c->ext = ext, c->kind = kind, c->object = object;
    if (kind == CHANGE_BACKGROUND)
        memcpy(c->background, background, sizeof(background));
    else
        c->before = *object_get(object);
    return 0;
}

int world_mesh_create(int ext, const float *vertices, int vertex_count, const uint32_t *indices, int index_count,
                      int primitive)
{
    static const GLenum modes[] = { GL_TRIANGLES, GL_LINES, GL_POINTS };
    static const int sizes[] = { 3, 3, 2, 4 }, offsets[] = { 0, 3, 6, 8 };
    if (vertex_count <= 0 || index_count < 0 || primitive < 0 || primitive > 2)
        return fail("mesh_create: needs vertices, and primitive TRIANGLES, LINES or POINTS");
    for (int i = 0; i < index_count; i++)
        if (indices[i] >= (uint32_t)vertex_count)
            return fail("mesh_create: index %d is %u, but there are only %d vertices", i, indices[i], vertex_count);
    int id = 0;
    while (id < MAX_MESHES && meshes[id].used)
        id++;
    if (id == MAX_MESHES)
        return fail("mesh_create: no room for more than %d meshes", MAX_MESHES);
    struct mesh *m = &meshes[id];
    memset(m, 0, sizeof(*m));
    glGetError();
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertex_count * VERTEX_FLOATS * 4, vertices, GL_STATIC_DRAW);
    for (int a = 0; a < 4; a++) {
        glVertexAttribPointer(a, sizes[a], GL_FLOAT, GL_FALSE, VERTEX_FLOATS * 4, (void *)(uintptr_t)(offsets[a] * 4));
        glEnableVertexAttribArray(a);
    }
    if (index_count) {
        glGenBuffers(1, &m->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)index_count * 4, indices, GL_STATIC_DRAW);
    }
    glBindVertexArray(0);
    m->count = index_count ? index_count : vertex_count;
    m->primitive = modes[primitive];
    m->owner = ext;
    m->used = 1;
    if (glGetError() != GL_NO_ERROR) {
        glDeleteVertexArrays(1, &m->vao);
        glDeleteBuffers(1, &m->vbo);
        if (m->ibo)
            glDeleteBuffers(1, &m->ibo);
        m->used = 0;
        return fail("mesh_create: the GPU refused it (out of memory?)");
    }
    return id + 1;
}

static GLuint compile(GLenum kind, const char *src, const char *what)
{
    GLuint s = glCreateShader(kind);
    GLint ok;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1536];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fail("%s shader didn't compile: %s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

int world_shader_create(int ext, const char *vertex_glsl, const char *fragment_glsl)
{
    static const char *const attributes[] = { "a_position", "a_normal", "a_uv", "a_colour" };
    int id = 0;
    while (id < MAX_SHADERS && shaders[id].used)
        id++;
    if (id == MAX_SHADERS)
        return fail("shader_create: no room for more than %d shaders", MAX_SHADERS);
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_glsl, "vertex"), fs = vs ? compile(GL_FRAGMENT_SHADER, fragment_glsl, "fragment") : 0;
    if (!fs) {
        if (vs)
            glDeleteShader(vs);
        return -1;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    for (int a = 0; a < 4; a++)
        glBindAttribLocation(p, a, attributes[a]);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1536];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        glDeleteProgram(p);
        return fail("shader didn't link: %s", log);
    }
    shaders[id] = (struct shader){ .used = 1, .owner = ext, .program = p,
                                   .mvp = glGetUniformLocation(p, "u_mvp"), .model = glGetUniformLocation(p, "u_model"),
                                   .time = glGetUniformLocation(p, "u_time"), .eye = glGetUniformLocation(p, "u_eye") };
    return id + 1;
}

int world_object_find(const char *name)
{
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (objects[i].used && strcmp(objects[i].name, name) == 0)
            return i + 1;
    return fail("there's no object called \"%s\"", name);
}

int world_object_create(int ext, const char *name)
{
    if (!name[0] || strlen(name) >= sizeof(objects[0].name))
        return fail("object_create: the name must be 1 to %zu characters", sizeof(objects[0].name) - 1);
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (objects[i].used && strcmp(objects[i].name, name) == 0)
            return fail("object_create: there's already an object called \"%s\"", name);
    int id = 0;
    while (id < MAX_OBJECTS && objects[id].used)
        id++;
    if (id == MAX_OBJECTS)
        return fail("object_create: no room for more than %d objects", MAX_OBJECTS);
    struct object *o = &objects[id];
    memset(o, 0, sizeof(*o));
    o->used = 1, o->owner = ext, o->visible = 1;
    snprintf(o->name, sizeof(o->name), "%s", name);
    for (int i = 0; i < 4; i++)
        o->model[i * 5] = 1;
    return id + 1;
}

/* The object to change, with the change noted for undoing */
static struct object *changing(int ext, int object, int kind)
{
    struct object *o = object_get(object);
    if (!o) {
        fail("there's no object %d", object);
        return NULL;
    }
    return remember(ext, kind, object) == 0 ? o : NULL;
}

int world_object_set_mesh(int ext, int object, int mesh)
{
    if (mesh && !mesh_get(mesh))
        return fail("object_set_mesh: there's no mesh %d", mesh);
    struct object *o = changing(ext, object, CHANGE_MESH);
    return o ? (o->mesh = mesh, 0) : -1;
}

int world_object_set_shader(int ext, int object, int shader)
{
    if (shader && !shader_get(shader))
        return fail("object_set_shader: there's no shader %d", shader);
    struct object *o = changing(ext, object, CHANGE_SHADER);
    return o ? (o->shader = shader, 0) : -1;
}

int world_object_set_transform(int ext, int object, const float matrix[16])
{
    struct object *o = changing(ext, object, CHANGE_TRANSFORM);
    return o ? (memcpy(o->model, matrix, sizeof(o->model)), 0) : -1;
}

int world_object_set_visible(int ext, int object, int visible)
{
    struct object *o = changing(ext, object, CHANGE_VISIBLE);
    return o ? (o->visible = visible != 0, 0) : -1;
}

int world_object_set_uniform(int ext, int object, const char *name, const float *values, int count)
{
    if (!name[0] || strlen(name) >= sizeof(objects[0].uniforms[0].name) || count < 1 || count > MAX_UNIFORM_FLOATS)
        return fail("object_set_uniform: a name of 1 to 31 characters and 1 to %d values", MAX_UNIFORM_FLOATS);
    struct object *o = changing(ext, object, CHANGE_UNIFORMS);
    if (!o)
        return -1;
    int i = 0;
    while (i < o->uniform_count && strcmp(o->uniforms[i].name, name) != 0)
        i++;
    if (i == MAX_UNIFORMS)
        return fail("object_set_uniform: an object can have at most %d uniforms", MAX_UNIFORMS);
    if (i == o->uniform_count)
        o->uniform_count++;
    snprintf(o->uniforms[i].name, sizeof(o->uniforms[i].name), "%s", name);
    memcpy(o->uniforms[i].values, values, count * sizeof(float));
    o->uniforms[i].count = count;
    return 0;
}

int world_set_background(int ext, float r, float g, float b)
{
    if (remember(ext, CHANGE_BACKGROUND, 0) != 0)
        return -1;
    background[0] = r, background[1] = g, background[2] = b;
    return 0;
}

static void drop_changes(int object)
{
    for (int i = 0; i < change_count;)
        if (changes[i].kind != CHANGE_BACKGROUND && changes[i].object == object)
            memmove(&changes[i], &changes[i + 1], (--change_count - i) * sizeof(changes[0]));
        else
            i++;
}

void world_release(int ext)
{
    /* Put back what it changed, newest first */
    for (int i = change_count - 1; i >= 0; i--) {
        struct change *c = &changes[i];
        if (c->ext != ext)
            continue;
        struct object *o = c->kind == CHANGE_BACKGROUND ? NULL : object_get(c->object);
        if (c->kind == CHANGE_BACKGROUND)
            memcpy(background, c->background, sizeof(background));
        else if (o && c->kind == CHANGE_MESH)
            o->mesh = c->before.mesh;
        else if (o && c->kind == CHANGE_SHADER)
            o->shader = c->before.shader;
        else if (o && c->kind == CHANGE_TRANSFORM)
            memcpy(o->model, c->before.model, sizeof(o->model));
        else if (o && c->kind == CHANGE_VISIBLE)
            o->visible = c->before.visible;
        else if (o && c->kind == CHANGE_UNIFORMS) {
            memcpy(o->uniforms, c->before.uniforms, sizeof(o->uniforms));
            o->uniform_count = c->before.uniform_count;
        }
        memmove(&changes[i], &changes[i + 1], (--change_count - i) * sizeof(changes[0]));
    }
    /* What it created goes; anything still using it (other extensions' objects, notes of how things
     * were) lets go of it */
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (objects[i].used && objects[i].owner == ext) {
            objects[i].used = 0;
            drop_changes(i + 1);
        }
    for (int i = 0; i < MAX_MESHES; i++) {
        struct mesh *m = &meshes[i];
        if (!m->used || m->owner != ext)
            continue;
        glDeleteVertexArrays(1, &m->vao);
        glDeleteBuffers(1, &m->vbo);
        if (m->ibo)
            glDeleteBuffers(1, &m->ibo);
        m->used = 0;
        for (int k = 0; k < MAX_OBJECTS; k++)
            if (objects[k].mesh == i + 1)
                objects[k].mesh = 0;
        for (int k = 0; k < change_count; k++)
            if (changes[k].before.mesh == i + 1)
                changes[k].before.mesh = 0;
    }
    for (int i = 0; i < MAX_SHADERS; i++) {
        if (!shaders[i].used || shaders[i].owner != ext)
            continue;
        glDeleteProgram(shaders[i].program);
        shaders[i].used = 0;
        for (int k = 0; k < MAX_OBJECTS; k++)
            if (objects[k].shader == i + 1)
                objects[k].shader = 0;
        for (int k = 0; k < change_count; k++)
            if (changes[k].before.shader == i + 1)
                changes[k].before.shader = 0;
    }
}

/* The world starts with a floor of 1 m wireframe squares, 20 m across */
static int make_floor(void)
{
    static float v[84 * VERTEX_FLOATS];
    int n = 0;
    for (int i = -10; i <= 10; i++) {
        float ends[4][3] = { { i, 0, -10 }, { i, 0, 10 }, { -10, 0, i }, { 10, 0, i } };
        for (int k = 0; k < 4; k++, n++) {
            float vertex[VERTEX_FLOATS] = { ends[k][0], ends[k][1], ends[k][2], 0, 1, 0, 0, 0, 1.0f, 0.69f, 0.44f, 1 };
            memcpy(&v[n * VERTEX_FLOATS], vertex, sizeof(vertex));
        }
    }
    int mesh = world_mesh_create(0, v, n, NULL, 0, PRIMITIVE_LINES);
    int shader = world_shader_create(0,
        "#version 300 es\n"
        "in vec3 a_position;\n"
        "in vec4 a_colour;\n"
        "uniform mat4 u_mvp;\n"
        "uniform vec3 u_eye;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
        "    colour = a_colour * clamp(1.3 - distance(a_position, u_eye) / 11.0, 0.0, 1.0);\n"
        "}\n",
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec4 colour;\n"
        "out vec4 fragment;\n"
        "void main() { fragment = colour; }\n");
    int floor = world_object_create(0, "floor");
    if (mesh < 0 || shader < 0 || floor < 0 || world_object_set_mesh(0, floor, mesh) || world_object_set_shader(0, floor, shader)) {
        wlog("making the floor: %s", error);
        return -1;
    }
    return 0;
}

/* ---- Drawing ---- */

/* Column-major 4x4: out = a * b */
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

/*
 * One eye's view: a frustum centred on its lens (so straight ahead is where the lens is), turned by
 * that panel's roll correction, from the eye's position.
 */
static void eye_view(int eye, float view_projection[16], float position[3])
{
    float n = 0.05f, f = 500.0f;
    float cx = lens_x[eye] - eye * EYE_W, cy = H - lens_y;   /* OpenGL counts rows from the bottom */
    float l = -cx / FOCAL_PX * n, r = (EYE_W - cx) / FOCAL_PX * n;
    float b = -cy / FOCAL_PX * n, t = (H - cy) / FOCAL_PX * n;
    float projection[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                             (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
    position[0] = (eye ? 1 : -1) * IPD / 2, position[1] = EYE_HEIGHT, position[2] = 0;
    float c = cosf(roll[eye]), s = sinf(roll[eye]);
    float turn[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float move[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -position[0], -position[1], -position[2], 1 };
    mat_mul(view_projection, projection, turn);
    mat_mul(view_projection, view_projection, move);
}

static void draw_eye(int eye, float time)
{
    float vp[16], eye_position[3];
    eye_view(eye, vp, eye_position);
    glViewport(eye * EYE_W, 0, EYE_W, H);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        struct object *o = &objects[i];
        struct mesh *m = o->used && o->visible ? mesh_get(o->mesh) : NULL;
        struct shader *s = m ? shader_get(o->shader) : NULL;
        if (!s)
            continue;
        float mvp[16];
        mat_mul(mvp, vp, o->model);
        glUseProgram(s->program);
        glUniformMatrix4fv(s->mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(s->model, 1, GL_FALSE, o->model);
        glUniform1fv(s->time, 1, &time);
        glUniform3fv(s->eye, 1, eye_position);
        for (int u = 0; u < o->uniform_count; u++) {
            const struct uniform *un = &o->uniforms[u];
            GLint at = glGetUniformLocation(s->program, un->name);
            if (un->count == 16)
                glUniformMatrix4fv(at, 1, GL_FALSE, un->values);
            else if (un->count == 4)
                glUniform4fv(at, 1, un->values);
            else if (un->count == 3)
                glUniform3fv(at, 1, un->values);
            else if (un->count == 2)
                glUniform2fv(at, 1, un->values);
            else
                glUniform1fv(at, un->count, un->values);
        }
        glBindVertexArray(m->vao);
        if (m->ibo)
            glDrawElements(m->primitive, m->count, GL_UNSIGNED_INT, 0);
        else
            glDrawArrays(m->primitive, 0, m->count);
    }
    glBindVertexArray(0);
}

/* ---- The display ---- */

static struct {
    int fd, pages, page;
    uint8_t *mem;
    size_t size, stride;
    struct fb_var_screeninfo var;
} fb;
static GLuint pbo[2];

static int display_open(void)
{
    struct fb_fix_screeninfo fix;
    fb.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb.fd < 0 || ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &fix) ||
        fb.var.xres != W || fb.var.yres != H || fb.var.bits_per_pixel != 32) {
        wlog("no %dx%d 32-bit framebuffer at /dev/fb0", W, H);
        return -1;
    }
    fb.size = fix.smem_len, fb.stride = fix.line_length;
    fb.pages = fb.var.yres_virtual >= 2 * H && fb.size >= 2 * fb.stride * H ? 2 : 1;
    fb.page = fb.var.yoffset / H;
    fb.mem = mmap(NULL, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.mem == MAP_FAILED) {
        wlog("mapping the framebuffer: %s", strerror(errno));
        return -1;
    }
    glGenBuffers(2, pbo);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)W * H * 4, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return 0;
}

/*
 * Show the frame just drawn. For now through memory: the GPU copies each frame into a buffer while the
 * CPU puts the previous one on screen (the panels show the framebuffer rotated 180°, so each
 * framebuffer row is OpenGL's bottom-up row of the same number, reversed). Rendering straight into
 * what the display scans out is still to come (docs/apps.md).
 */
static double present(unsigned frame)
{
    double t0 = now();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[frame & 1]);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    if (frame > 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[(frame + 1) & 1]);
        const uint32_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)W * H * 4, GL_MAP_READ_BIT);
        if (src) {
            int back = fb.pages == 2 ? fb.page ^ 1 : 0;
            uint8_t *page = fb.mem + (size_t)back * fb.stride * H;
            for (int y = 0; y < H; y++) {
                uint32_t *dst = (uint32_t *)(page + (size_t)y * fb.stride);
                const uint32_t *row = src + (size_t)y * W;
                for (int x = 0; x < W; x++)
                    dst[W - 1 - x] = row[x];
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            fb.page = back;
            fb.var.yoffset = back * H;
            ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.var);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return now() - t0;
}

/* ---- Control ---- */

static int control_open(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
    mkdir("/run", 0755);
    mkdir("/run/afterglow", 0755);
    unlink(SOCKET_PATH);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || chmod(SOCKET_PATH, 0600) != 0 || listen(s, 4) != 0) {
        wlog("control socket %s: %s", SOCKET_PATH, strerror(errno));
        return -1;
    }
    return s;
}

/* Handle a command if one is waiting: a line, answered and then the connection closed */
static void control_poll(int listener)
{
    int c = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0)
        return;
    char line[1024], reply[16384] = "";
    size_t len = 0;
    while (len < sizeof(line) - 1 && !memchr(line, '\n', len)) {
        struct pollfd p = { c, POLLIN, 0 };
        ssize_t n = poll(&p, 1, 1000) == 1 ? read(c, line + len, sizeof(line) - 1 - len) : -1;
        if (n <= 0)
            break;
        len += n;
    }
    line[len] = '\0';
    line[strcspn(line, "\r\n")] = '\0';
    char verb[16] = "", name[64] = "", path[512] = "";
    sscanf(line, "%15s %63s %511s", verb, name, path);
    if (strcmp(verb, "load") == 0 && name[0] && path[0])
        ext_load(name, path, reply, sizeof(reply));
    else if (strcmp(verb, "unload") == 0 && name[0])
        ext_unload(name, reply, sizeof(reply));
    else if (strcmp(verb, "list") == 0)
        ext_list(reply, sizeof(reply));
    else if (strcmp(verb, "quit") == 0) {
        running = 0;
        snprintf(reply, sizeof(reply), "stopping\n");
    } else
        snprintf(reply, sizeof(reply), "error: commands are load NAME PATH, unload NAME, list, quit\n");
    for (size_t off = 0, total = strlen(reply); off < total;) {
        ssize_t n = write(c, reply + off, total - off);
        if (n <= 0)
            break;
        off += n;
    }
    close(c);
}

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

/* afterglow.roll_left_deg / roll_right_deg from the kernel command line, as /init uses them */
static void read_roll(void)
{
    char buf[4096] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    const char *keys[2] = { "afterglow.roll_left_deg=", "afterglow.roll_right_deg=" };
    for (int eye = 0; eye < 2; eye++) {
        const char *p = strstr(buf, keys[eye]);
        roll[eye] = p ? strtof(p + strlen(keys[eye]), NULL) * (float)M_PI / 180 : 0;
    }
}

int main(void)
{
    started = now();
    kmsg = fopen("/dev/kmsg", "we");
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    read_roll();
    if (gles_init(W, H) != 0 || display_open() != 0 || make_floor() != 0 || ext_init() != 0)
        return 1;
    int control = control_open();
    if (control < 0)
        return 1;
    FILE *owner = fopen(OWNER_PATH, "w");
    if (owner) {
        fprintf(owner, "%d\n", getpid());
        fclose(owner);
    }
    glEnable(GL_DEPTH_TEST);
    glLineWidth(2);
    wlog("running: %dx%d, lens-centred, roll %.1f° / %.1f°", W, H, roll[0] * 180 / M_PI, roll[1] * 180 / M_PI);

    unsigned frame = 0, frames = 0;
    double last = world_time(), report = last, busy = 0, copying = 0;
    while (running) {
        double t = world_time();
        control_poll(control);
        ext_frame((float)(t - last));
        last = t;
        glClearColor(background[0], background[1], background[2], 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int eye = 0; eye < 2; eye++)
            draw_eye(eye, (float)t);
        copying += present(frame++);
        frames++;
        busy += world_time() - t;
        double wait = t + 1.0 / FRAME_RATE - world_time();
        if (wait > 0)
            usleep((useconds_t)(wait * 1e6));
        if (world_time() - report >= 10) {
            wlog("%.1f fps; per frame %.1f ms busy, of which %.1f ms showing it", frames / (world_time() - report),
                 1000 * busy / frames, 1000 * copying / frames);
            report = world_time(), frames = 0, busy = copying = 0;
        }
    }
    unlink(OWNER_PATH);
    unlink(SOCKET_PATH);
    wlog("stopped");
    return 0;
}
