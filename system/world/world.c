/*
 * afterglow's world: the render loop, the scene and the control socket (docs/apps.md).
 *
 * Built with the Android NDK (system/world/build.sh) so it runs on Android's C library and can use
 * Qualcomm's GPU driver. afterglowd starts it and talks to it over /run/afterglow/world.sock, one
 * command per connection, the reply ending when the connection closes:
 *   load NAME PATH [placeholder]
 *                     load extension NAME from a WebAssembly file, replacing any loaded version; a
 *                     placeholder gets a "Claude at work" sign beside it until the real thing replaces it
 *   unload NAME
 *   list
 *   recentre          make the way the wearer faces now "ahead" (-z)
 *   status TEXT       what Claude is doing, shown on a panel in front of the wearer (sign.c)
 *   capture start eyes|panels left|right|both WIDTH HEIGHT, capture stop
 *                     frames for afterglowd's video stream, scaled down, in /tmp/afterglow-capture (capture.c)
 *   snapshot PATH eyes|panels
 *                     write the newest frame to PATH, raw RGBA 2880x1600 rows top first, as the wearer
 *                     sees it: "eyes" undistorted (the eye buffers), "panels" warped for the lenses
 *   quit
 * While it runs it owns the display: /run/afterglow/display-owner holds its pid, and /init's status
 * screen keeps off the screen.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
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

#include <GLES2/gl2ext.h>   /* after gles.h, which brings in the GL types */

#define W 2880   /* both eyes side by side, as the wearer sees them */
#define H 1600
#define EYE_W (W / 2)
#define SOCKET_PATH "/run/afterglow/world.sock"
#define OWNER_PATH "/run/afterglow/display-owner"
#define FONT_PATH "/afterglow/font.ttf"

/* Optics, in screen pixels as the wearer sees them (docs/hardware.md). No lens distortion correction yet. */
#define IPD 0.063f
/* Frames are drawn for the head as it will be when they're first seen (a frame's drawing time, then the
 * refresh: frame_show reprojects later refreshes), at most this far ahead */
#define PREDICT_MAX 0.1
#define FRAME_RATE 72

static FILE *kmsg;
static double started;
static volatile sig_atomic_t running = 1;
static float roll[2];   /* each panel's roll correction, radians, counterclockwise as the wearer sees it */
static float head_rotation[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, head_centre[3] = { 0, EYE_HEIGHT, 0 };   /* this frame's */

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
#define MAX_TEXTURES 256
#define MAX_OBJECTS 1024
#define MAX_UNIFORMS 8
#define MAX_CHANGES 4096

struct mesh {
    int used, owner;
    GLuint vao, vbo, ibo;
    GLsizei count;
    GLenum primitive;
    float lo[3], hi[3];   /* the box around it */
};

struct shader {
    int used, owner, standard;
    GLuint program;
    GLint mvp, model, normal, time, eye, colour, uv_scale, emission, alpha_cut;
};

struct texture {
    int used, owner, width, height;
    GLuint id;
};

struct uniform {
    char name[32];
    float values[MAX_UNIFORM_FLOATS];
    int count;
};

struct object {
    int used, owner, mesh, shader, visible, blend;
    float spin;   /* radians a second about its own z axis: the world turns it (sign.c's asterisk) */
    int textures[TEXTURE_SLOTS];
    char name[48];
    float model[16];
    struct uniform uniforms[MAX_UNIFORMS];
    int uniform_count;
};

/* Something an extension changed but doesn't own, as it was before that extension's first change */
enum { CHANGE_MESH, CHANGE_SHADER, CHANGE_TRANSFORM, CHANGE_VISIBLE, CHANGE_UNIFORMS, CHANGE_TEXTURES, CHANGE_BLEND,
       CHANGE_BACKGROUND };
struct change {
    int ext, kind, object;
    struct object before;
    float background[3];
};

static struct mesh meshes[MAX_MESHES];   /* ids are index + 1; 0 means none */
static struct shader shaders[MAX_SHADERS];
static struct texture textures[MAX_TEXTURES];
static struct object objects[MAX_OBJECTS];
static struct change changes[MAX_CHANGES];
static int change_count;
static float background[3] = { 0.02f, 0.02f, 0.04f };
static char error[2048];
static int standard_shader;
static GLuint white;          /* what an empty texture slot reads: plain white */
static float anisotropy = 1;  /* the sharpest texture filtering the GPU offers */
static char texture_dir[256] = "/storage/world/textures";   /* $AFTERGLOW_WORLD_DIR/textures */
static char memory_path[256] = "/storage/world/memory";       /* $AFTERGLOW_WORLD_DIR/memory */

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

int world_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, sizeof(error), fmt, ap);
    va_end(ap);
    return -1;
}

unsigned world_shader_program(int id)
{
    return id >= 1 && id <= MAX_SHADERS && shaders[id - 1].used ? shaders[id - 1].program : 0;
}

static struct mesh *mesh_get(int id)
{
    return id >= 1 && id <= MAX_MESHES && meshes[id - 1].used ? &meshes[id - 1] : NULL;
}

static struct shader *shader_get(int id)
{
    return id >= 1 && id <= MAX_SHADERS && shaders[id - 1].used ? &shaders[id - 1] : NULL;
}

static struct texture *texture_get(int id)
{
    return id >= 1 && id <= MAX_TEXTURES && textures[id - 1].used ? &textures[id - 1] : NULL;
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
    for (int k = 0; k < 3; k++)
        m->lo[k] = INFINITY, m->hi[k] = -INFINITY;
    for (int i = 0; i < vertex_count; i++)
        for (int k = 0; k < 3; k++) {
            m->lo[k] = fminf(m->lo[k], vertices[i * VERTEX_FLOATS + k]);
            m->hi[k] = fmaxf(m->hi[k], vertices[i * VERTEX_FLOATS + k]);
        }
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
                                   .normal = glGetUniformLocation(p, "u_normal"), .time = glGetUniformLocation(p, "u_time"),
                                   .eye = glGetUniformLocation(p, "u_eye"), .colour = glGetUniformLocation(p, "u_colour"),
                                   .uv_scale = glGetUniformLocation(p, "u_uv_scale"),
                                   .emission = glGetUniformLocation(p, "u_emission"),
                                   .alpha_cut = glGetUniformLocation(p, "u_alpha_cut") };
    glUseProgram(p);
    for (int t = 0; t < TEXTURE_SLOTS; t++) {
        char name[16];
        snprintf(name, sizeof(name), "u_texture%d", t);
        glUniform1i(glGetUniformLocation(p, name), t);
    }
    glUseProgram(0);
    return id + 1;
}

/*
 * The standard shader, so most things need no GLSL: lit by a sun (high, behind the wearer and a little
 * to the right) and the sky. Colour: u_colour (default white) x the vertex colour x texture 0 (default
 * white) at a_uv x u_uv_scale (default 1, 1); plus u_emission (default black) for things that glow.
 * Solid objects drop pixels whose alpha is under a half (cut-outs such as leaves); normals are turned
 * to face the eye, so both sides of a surface are lit alike.
 */
static int make_standard_shader(void)
{
    int id = world_shader_create(0,
        "#version 300 es\n"
        "in vec3 a_position;\n"
        "in vec3 a_normal;\n"
        "in vec2 a_uv;\n"
        "in vec4 a_colour;\n"
        "uniform mat4 u_mvp;\n"
        "uniform mat4 u_model;\n"
        "uniform mat3 u_normal;\n"
        "uniform vec2 u_uv_scale;\n"
        "out vec3 normal;\n"
        "out vec3 position;\n"
        "out vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
        "    position = (u_model * vec4(a_position, 1.0)).xyz;\n"
        "    normal = u_normal * a_normal;\n"
        "    uv = a_uv * u_uv_scale;\n"
        "    colour = a_colour;\n"
        "}\n",
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform vec4 u_colour;\n"
        "uniform vec3 u_emission;\n"
        "uniform vec3 u_eye;\n"
        "uniform float u_alpha_cut;\n"
        "uniform sampler2D u_texture0;\n"
        "in vec3 normal;\n"
        "in vec3 position;\n"
        "in vec2 uv;\n"
        "in vec4 colour;\n"
        "out vec4 fragment;\n"
        "void main() {\n"
        "    vec4 base = u_colour * colour * texture(u_texture0, uv);\n"
        "    if (base.a < u_alpha_cut) discard;\n"
        "    vec3 n = normalize(normal);\n"
        "    if (dot(n, u_eye - position) < 0.0) n = -n;\n"
        "    float sun = max(dot(n, normalize(vec3(0.35, 0.7, 0.62))), 0.0);\n"
        "    vec3 sky = mix(vec3(0.24, 0.22, 0.20), vec3(0.36, 0.40, 0.47), 0.5 + 0.5 * n.y);\n"
        "    fragment = vec4(base.rgb * (sky + vec3(1.0, 0.95, 0.86) * 0.78 * sun) + u_emission, base.a);\n"
        "}\n");
    if (id < 0) {
        wlog("the standard shader: %s", error);
        return -1;
    }
    shaders[id - 1].standard = 1;
    standard_shader = id;
    return 0;
}

int world_shader_standard(void)
{
    return standard_shader;
}

/* ---- Textures ---- */

int world_texture_create(int ext, const uint8_t *rgba, int width, int height)
{
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        return fail("texture_create: 1 to 4096 pixels each way, not %dx%d", width, height);
    int id = 0;
    while (id < MAX_TEXTURES && textures[id].used)
        id++;
    if (id == MAX_TEXTURES)
        return fail("no room for more than %d textures", MAX_TEXTURES);
    GLuint t;
    glGetError();
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (anisotropy > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &t);
        return fail("the GPU refused a %dx%d texture (out of memory?)", width, height);
    }
    textures[id] = (struct texture){ .used = 1, .owner = ext, .width = width, .height = height, .id = t };
    return id + 1;
}

/* An image from texture_dir/NAME.png or .jpg (afterglowd's fetch_texture and generate_texture put them there) */
int world_texture_load(int ext, const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 64 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") != n)
        return fail("texture_load: \"%s\" isn't a texture name (lower case, digits, - and _)", name);
    char path[256];
    int width, height;
    uint8_t *rgba = NULL;
    for (int i = 0; i < 3 && !rgba; i++) {
        snprintf(path, sizeof(path), "%s/%s.%s", texture_dir, name, (const char *[]){ "png", "jpg", "jpeg" }[i]);
        rgba = access(path, R_OK) == 0 ? paint_load(path, &width, &height) : NULL;
        if (!rgba && access(path, R_OK) == 0)
            return fail("texture_load: %s: %s", path, paint_load_error());
    }
    if (!rgba)
        return fail("texture_load: there's no texture called \"%s\" in %s", name, texture_dir);
    int id = world_texture_create(ext, rgba, width, height);
    free(rgba);
    return id;
}

int world_texture_text(int ext, const char *text, int pixel_height, uint32_t ink)
{
    if (pixel_height < 8 || pixel_height > 512)
        return fail("texture_text: a height of 8 to 512 pixels");
    int width, height;
    uint8_t *rgba = paint_text(text, pixel_height, ink, pixel_height * 0.012f, &width, &height);
    if (!rgba)
        return fail("texture_text: too big (at most 4096 pixels each way) or no font");
    int id = world_texture_create(ext, rgba, width, height);
    free(rgba);
    return id;
}

/* For the world's own things: free one texture or mesh it made (extensions' go with world_release) */
void world_texture_delete(int texture)
{
    struct texture *t = texture_get(texture);
    if (!t)
        return;
    glDeleteTextures(1, &t->id);
    t->used = 0;
    for (int k = 0; k < MAX_OBJECTS; k++)
        for (int s = 0; s < TEXTURE_SLOTS; s++)
            if (objects[k].textures[s] == texture)
                objects[k].textures[s] = 0;
}

/* Repaint a texture in place (anything showing it shows the new picture) */
int world_texture_update(int texture, const uint8_t *rgba, int width, int height)
{
    struct texture *t = texture_get(texture);
    if (!t)
        return fail("there's no texture %d", texture);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    t->width = width, t->height = height;
    return 0;
}

void world_mesh_delete(int mesh)
{
    struct mesh *m = mesh_get(mesh);
    if (!m)
        return;
    glDeleteVertexArrays(1, &m->vao);
    glDeleteBuffers(1, &m->vbo);
    if (m->ibo)
        glDeleteBuffers(1, &m->ibo);
    m->used = 0;
    for (int k = 0; k < MAX_OBJECTS; k++)
        if (objects[k].mesh == mesh)
            objects[k].mesh = 0;
}

int world_texture_size(int texture, int *width, int *height)
{
    struct texture *t = texture_get(texture);
    if (!t)
        return fail("texture_size: there's no texture %d", texture);
    *width = t->width, *height = t->height;
    return 0;
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

int world_object_get_transform(int object, float matrix[16])
{
    struct object *o = object_get(object);
    if (!o)
        return fail("object_get_transform: there's no object %d", object);
    memcpy(matrix, o->model, sizeof(o->model));
    return 0;
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

int world_object_set_texture(int ext, int object, int slot, int texture)
{
    if (slot < 0 || slot >= TEXTURE_SLOTS)
        return fail("object_set_texture: slot 0 to %d", TEXTURE_SLOTS - 1);
    if (texture && !texture_get(texture))
        return fail("object_set_texture: there's no texture %d", texture);
    struct object *o = changing(ext, object, CHANGE_TEXTURES);
    return o ? (o->textures[slot] = texture, 0) : -1;
}

int world_object_set_blend(int ext, int object, int mode)
{
    if (mode < BLEND_SOLID || mode > BLEND_ADD)
        return fail("object_set_blend: SOLID, BLEND or ADD");
    struct object *o = changing(ext, object, CHANGE_BLEND);
    return o ? (o->blend = mode, 0) : -1;
}

int world_object_set_spin(int object, float radians_a_second)
{
    struct object *o = object_get(object);
    return o ? (o->spin = radians_a_second, 0) : fail("there's no object %d", object);
}

/* Where a mesh's box corner lands, through an object's transform */
static void corner(const struct object *o, const struct mesh *m, int which, float out[3])
{
    float p[3] = { which & 1 ? m->hi[0] : m->lo[0], which & 2 ? m->hi[1] : m->lo[1], which & 4 ? m->hi[2] : m->lo[2] };
    for (int r = 0; r < 3; r++)
        out[r] = o->model[r] * p[0] + o->model[4 + r] * p[1] + o->model[8 + r] * p[2] + o->model[12 + r];
}

int world_extension_bounds(int ext, float lo[3], float hi[3])
{
    int found = 0;
    for (int k = 0; k < 3; k++)
        lo[k] = INFINITY, hi[k] = -INFINITY;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        struct object *o = &objects[i];
        struct mesh *m = o->used && o->owner == ext && o->visible ? mesh_get(o->mesh) : NULL;
        for (int c = 0; m && c < 8; c++, found = 1) {
            float p[3];
            corner(o, m, c, p);
            for (int k = 0; k < 3; k++)
                lo[k] = fminf(lo[k], p[k]), hi[k] = fmaxf(hi[k], p[k]);
        }
    }
    return found ? 0 : -1;
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

/* Take away one of the world's own objects */
void world_object_delete(int object)
{
    struct object *o = object_get(object);
    if (o) {
        o->used = 0;
        drop_changes(object);
    }
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
        } else if (o && c->kind == CHANGE_TEXTURES)
            memcpy(o->textures, c->before.textures, sizeof(o->textures));
        else if (o && c->kind == CHANGE_BLEND)
            o->blend = c->before.blend;
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
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (!textures[i].used || textures[i].owner != ext)
            continue;
        glDeleteTextures(1, &textures[i].id);
        textures[i].used = 0;
        for (int t = 0; t < TEXTURE_SLOTS; t++) {
            for (int k = 0; k < MAX_OBJECTS; k++)
                if (objects[k].textures[t] == i + 1)
                    objects[k].textures[t] = 0;
            for (int k = 0; k < change_count; k++)
                if (changes[k].before.textures[t] == i + 1)
                    changes[k].before.textures[t] = 0;
        }
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
 * that panel's roll correction, from the eye's position, which turns with the head.
 */
static void eye_view(int eye, float view_projection[16], float position[3])
{
    float n = 0.05f, f = 500.0f, tangents[4];
    lens_frustum(eye, tangents);   /* the eye buffer covers the lens's field of view; lens.c warps it */
    float l = tangents[0] * n, r = tangents[1] * n, b = tangents[2] * n, t = tangents[3] * n;
    float projection[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                             (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1, 0, 0, -2 * f * n / (f - n), 0 };
    const float *h = head_rotation;   /* columns: the head's right, up and back, in the world */
    for (int k = 0; k < 3; k++)
        position[k] = head_centre[k] + (eye ? 1 : -1) * IPD / 2 * h[k];
    float c = cosf(roll[eye]), s = sinf(roll[eye]);
    float turn[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float unturn_head[16] = { h[0], h[3], h[6], 0, h[1], h[4], h[7], 0, h[2], h[5], h[8], 0, 0, 0, 0, 1 };
    float move[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -position[0], -position[1], -position[2], 1 };
    mat_mul(view_projection, projection, turn);
    mat_mul(view_projection, view_projection, unturn_head);
    mat_mul(view_projection, view_projection, move);
}

/* The inverse transpose of a transform's 3x3 part, which turns normals the way it turns surfaces */
static void normal_matrix(float n[9], const float m[16])
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float cof[9] = { e * i - f * h, -(d * i - f * g), d * h - e * g, -(b * i - c * h), a * i - c * g,
                     -(a * h - b * g), b * f - c * e, -(a * f - c * d), a * e - b * d };
    float det = a * cof[0] + b * cof[1] + c * cof[2];
    if (fabsf(det) < 1e-12f)
        det = 1;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            n[col * 3 + row] = cof[row * 3 + col] / det;
}

static void draw_object(const struct object *o, const float vp[16], const float eye_position[3], float time)
{
    static const float ones[4] = { 1, 1, 1, 1 }, zeros[3] = { 0, 0, 0 };
    struct mesh *m = mesh_get(o->mesh);
    struct shader *s = shader_get(o->shader);
    float mvp[16], normal[9], spun[16];
    const float *model = o->model;
    if (o->spin != 0) {   /* turned about its own z axis as time goes by */
        float a = fmodf(time * o->spin, 2 * (float)M_PI), c = cosf(a), s = sinf(a);
        float turn[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        mat_mul(spun, o->model, turn);
        model = spun;
    }
    mat_mul(mvp, vp, model);
    normal_matrix(normal, model);
    glUseProgram(s->program);
    glUniformMatrix4fv(s->mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s->model, 1, GL_FALSE, model);
    glUniformMatrix3fv(s->normal, 1, GL_FALSE, normal);
    glUniform1fv(s->time, 1, &time);
    glUniform3fv(s->eye, 1, eye_position);
    if (s->standard) {
        float cut = o->blend == BLEND_SOLID ? 0.5f : 0.004f;
        glUniform4fv(s->colour, 1, ones);
        glUniform2fv(s->uv_scale, 1, ones);
        glUniform3fv(s->emission, 1, zeros);
        glUniform1fv(s->alpha_cut, 1, &cut);
    }
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
    for (int t = 0; t < TEXTURE_SLOTS; t++) {
        struct texture *tx = texture_get(o->textures[t]);
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, tx ? tx->id : white);
    }
    glBindVertexArray(m->vao);
    if (m->ibo)
        glDrawElements(m->primitive, m->count, GL_UNSIGNED_INT, 0);
    else
        glDrawArrays(m->primitive, 0, m->count);
}

/* Solid things first; then see-through and glowing ones, farthest first, without hiding what's behind them */
static void draw_eye(int eye, float time)
{
    float vp[16], eye_position[3], distance[MAX_OBJECTS];
    int order[MAX_OBJECTS], later = 0;
    eye_view(eye, vp, eye_position);
    glViewport(eye * EYE_W, 0, EYE_W, H);   /* the eye buffers, side by side */
    for (int i = 0; i < MAX_OBJECTS; i++) {
        struct object *o = &objects[i];
        struct mesh *m = o->used && o->visible ? mesh_get(o->mesh) : NULL;
        if (!m || !shader_get(o->shader))
            continue;
        if (o->blend == BLEND_SOLID) {
            draw_object(o, vp, eye_position, time);
            continue;
        }
        float a[3], b[3];
        corner(o, m, 0, a);
        corner(o, m, 7, b);
        distance[i] = hypotf(hypotf((a[0] + b[0]) / 2 - eye_position[0], (a[1] + b[1]) / 2 - eye_position[1]),
                             (a[2] + b[2]) / 2 - eye_position[2]);
        int k = later++;
        while (k > 0 && distance[order[k - 1]] < distance[i]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }
    if (later) {
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        for (int k = 0; k < later; k++) {
            const struct object *o = &objects[order[k]];
            glBlendFunc(GL_SRC_ALPHA, o->blend == BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            draw_object(o, vp, eye_position, time);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
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
 * Frames reach the panels through memory: the GPU reads each finished frame into a buffer, and the CPU
 * copies it to the framebuffer's hidden page and flips to it at the next refresh (frames are drawn
 * mirrored, eye_view, so the copy runs row for row). Drawing and showing run on their own threads: the
 * main one draws frames as fast as the scene allows, and the display thread shows the newest finished
 * frame at every refresh (72 Hz: the flip waits for it).
 *
 * Reprojection: a frame is drawn for one pose of the head and shown at later refreshes, by when the head
 * has turned on. For a small turn the picture mostly slides, so each copy slides it by the turn since it
 * was drawn (yaw sideways, pitch up and down; roll is ignored), and every refresh shows the world where
 * it now is: no double image from showing one frame twice, and prediction only has to cover a refresh.
 * Rendering straight into what the display scans out is still to come (docs/apps.md).
 */
#define DISPLAY_AHEAD 0.019   /* from the copy to the middle of the next refresh's scan-out */

static GLuint pbo[2];
static GLuint eye_fbo, panel_fbo;          /* the eyes, drawn; and warped for the lenses, as the panels show them */
static GLsync drawing;                     /* done when the frame on the GPU is */
static int drawing_pbo = -1, next_pbo;
static float drawn_rotation[9];
/* Finished frames, copied out of the readback buffers (which the CPU sees uncached: fine to stream
 * through once, far too slow to sample) into ordinary memory, two so one can be filled while the other
 * is shown */
static uint8_t *frames[2];
static int next_frame;

static int frames_open(void)
{
    for (int i = 0; i < 2; i++)
        if (!(frames[i] = malloc((size_t)W * H * 4))) {
            wlog("no memory for frames");
            return -1;
        }
    return 0;
}
/* Shared with the display thread, under shown_lock: the newest finished frame and its pose */
static pthread_mutex_t shown_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t *shown;
static float shown_rotation[9];
static unsigned refreshes;                 /* for the report */
static double copying;

/* After drawing: read the frame into a free buffer, with a fence to say when it's done */
static void frame_submit(void)
{
    int k = next_pbo;
    next_pbo ^= 1;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[k]);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    drawing = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();   /* to the GPU now, so it draws while frames are shown */
    drawing_pbo = k;
    memcpy(drawn_rotation, head_rotation, sizeof(drawn_rotation));
}

/* Wait up to `wait_ns` for the frame on the GPU; once it's finished it becomes the one shown. 1 if it did */
static int frame_collect(GLuint64 wait_ns)
{
    if (drawing_pbo < 0)
        return 0;
    GLenum state = glClientWaitSync(drawing, 0, wait_ns);
    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
        return 0;
    glDeleteSync(drawing);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[drawing_pbo]);
    const uint8_t *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)W * H * 4, GL_MAP_READ_BIT);
    if (mapped) {
        uint8_t *frame = frames[next_frame];   /* not the one being shown */
        memcpy(frame, mapped, (size_t)W * H * 4);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        pthread_mutex_lock(&shown_lock);   /* waits if the display thread is mid-copy */
        shown = frame;
        memcpy(shown_rotation, drawn_rotation, sizeof(shown_rotation));
        pthread_mutex_unlock(&shown_lock);
        next_frame ^= 1;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    drawing_pbo = -1;
    return 1;
}

/*
 * Reprojection with a tilt: each eye's picture is also turned about its lens centre (exact, as the lens
 * is symmetric about it). For the small turns between frames, along a row of the framebuffer the picture
 * comes from one row of the drawn frame at a fixed offset for 1/angle pixels at a time, then the next row:
 * so a row is a few dozen straight copies, nearly as cheap as sliding it (a pixel-by-pixel turn took
 * 15-50 ms a refresh). Accurate to about a pixel up to 3°; rows are shared among the display thread
 * and two helpers.
 */
#define TURN_HELPERS 2
#define TILT_REPROJECTION 0   /* off until it fits in a refresh (docs/apps.md) */
#define TILT_MAX 0.05f   /* radians: beyond about 3° the picture is turned no further */

struct turn_job {
    uint8_t *page;
    const uint32_t *src;
    float angle;          /* counterclockwise as the wearer sees it */
    float right, up;      /* the slide, in pixels as the wearer sees it */
    int first_row, end_row;
    double started, finished;   /* for the report */
};
static double turn_wake_max, turn_rows_max;

static struct turn_job turn_jobs[TURN_HELPERS + 1];
static pthread_mutex_t turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_start = PTHREAD_COND_INITIALIZER, turn_done = PTHREAD_COND_INITIALIZER;
static unsigned turn_generation, turn_finished;
static unsigned turned;          /* refreshes that needed turning, for the report */
static double slowest_copy;
static float test_tilt;          /* added to every refresh's tilt by the "test-tilt" command, to measure */

static void turn_rows(struct turn_job *j)
{
    j->started = now();
    /* In framebuffer coordinates (the wearer's view turned 180°, which leaves turns turning the same way),
     * the pixel at offset p from the lens centre shows the drawn frame at offset R^-1 (p + slide); for a
     * small angle a, R^-1 (x, y) is (x - a y, y + a x) */
    float a = j->angle;
    for (int half = 0; half < 2; half++) {
        float cx, cy;
        lens_centre_on_screen(1 - half, &cx, &cy);   /* the framebuffer's left half is the right eye */
        cx = W - 1 - cx, cy = H - 1 - cy;
        int lo = half * EYE_W, hi = lo + EYE_W;
        for (int y = j->first_row; y < j->end_row; y++) {
            uint32_t line[EYE_W];   /* built in ordinary memory, then copied to the framebuffer whole */
            float vy = y - cy - j->up;
            int shift = (int)lrintf(j->right - vy * a);   /* source x = x + shift, along the whole row */
            int x = lo;
            while (x < hi) {
                float source_y = cy + vy + (x - cx + j->right) * a;
                int row = (int)floorf(source_y + 0.5f), end = hi;
                if (a > 1e-6f)
                    end = x + (int)ceilf((row + 0.5f - source_y) / a);
                else if (a < -1e-6f)
                    end = x + (int)ceilf((source_y - (row - 0.5f)) / -a);
                if (end <= x)
                    end = x + 1;
                if (end > hi)
                    end = hi;
                /* The part of [x, end) whose source lies in this eye's half of a real row */
                int from = x, to = end;
                if (from + shift < lo)
                    from = lo - shift;
                if (to + shift > hi)
                    to = hi - shift;
                if (row < 0 || row >= H || from >= to)
                    memset(&line[x - lo], 0, (size_t)(end - x) * 4);
                else {
                    if (from > x)
                        memset(&line[x - lo], 0, (size_t)(from - x) * 4);
                    const uint32_t *piece = j->src + (size_t)row * W + from + shift;
                    /* This source row's next piece is wanted by the next output row: fetch it now, as the
                     * hardware can't follow the dozens of rows read in turn */
                    __builtin_prefetch(piece + (to - from) + 16);
                    __builtin_prefetch(piece + (to - from) + 32);
                    __builtin_prefetch(piece + (to - from) + 48);
                    memcpy(&line[from - lo], piece, (size_t)(to - from) * 4);
                    if (to < end)
                        memset(&line[to - lo], 0, (size_t)(end - to) * 4);
                }
                x = end;
            }
            memcpy((uint32_t *)(j->page + (size_t)y * fb.stride) + lo, line, sizeof(line));
        }
    }
    j->finished = now();
}

static void *turn_helper(void *arg)
{
    int k = (int)(intptr_t)arg;
    pin_thread(4 + k, 4 + k, 50);   /* cores 5 and 6, beside the display thread on 7 */
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&turn_lock);
        while (turn_generation == seen)
            pthread_cond_wait(&turn_start, &turn_lock);
        seen = turn_generation;
        pthread_mutex_unlock(&turn_lock);
        turn_rows(&turn_jobs[k]);
        pthread_mutex_lock(&turn_lock);
        turn_finished++;
        pthread_cond_signal(&turn_done);
        pthread_mutex_unlock(&turn_lock);
    }
    return NULL;
}

static void turn_copy(uint8_t *page, float angle, float right, float up)
{
    for (int k = 0; k <= TURN_HELPERS; k++)
        turn_jobs[k] = (struct turn_job){ page, (const uint32_t *)shown, angle, right, up,
                                          H * k / (TURN_HELPERS + 1), H * (k + 1) / (TURN_HELPERS + 1), 0, 0 };
    pthread_mutex_lock(&turn_lock);
    turn_finished = 0;
    turn_generation++;
    pthread_cond_broadcast(&turn_start);
    pthread_mutex_unlock(&turn_lock);
    double t0 = now();
    turn_rows(&turn_jobs[0]);
    pthread_mutex_lock(&turn_lock);
    while (turn_finished < TURN_HELPERS)
        pthread_cond_wait(&turn_done, &turn_lock);
    pthread_mutex_unlock(&turn_lock);
    for (int k = 0; k <= TURN_HELPERS; k++) {
        turn_wake_max = fmax(turn_wake_max, turn_jobs[k].started - t0);
        turn_rows_max = fmax(turn_rows_max, turn_jobs[k].finished - turn_jobs[k].started);
    }
}

/* Copy the newest frame to the hidden page, moved for the head's turn since it was drawn (under shown_lock) */
static void frame_copy(const float now_rotation[9])
{
    /* Where the drawn frame's straight ahead and up lie now, in the head's present axes (its columns) */
    const float *r = shown_rotation, *d = now_rotation;
    float ahead[3] = { -r[6], -r[7], -r[8] }, f[3], u[3];
    for (int k = 0; k < 3; k++) {
        f[k] = d[k * 3] * ahead[0] + d[k * 3 + 1] * ahead[1] + d[k * 3 + 2] * ahead[2];
        u[k] = d[k * 3] * r[3] + d[k * 3 + 1] * r[4] + d[k * 3 + 2] * r[5];
    }
    float tilt = atan2f(-u[0], u[1]) + test_tilt;   /* how far to turn the picture, counterclockwise as seen */
    tilt = tilt > TILT_MAX ? TILT_MAX : tilt < -TILT_MAX ? -TILT_MAX : tilt;
    int right = 0, up = 0;   /* pixels, as the wearer sees it */
    if (f[2] < -0.5f) {
        right = (int)lrintf(lens_pixels_per_tan() * f[0] / -f[2]);
        up = (int)lrintf(lens_pixels_per_tan() * f[1] / -f[2]);
        right = right > EYE_W / 2 ? EYE_W / 2 : right < -EYE_W / 2 ? -EYE_W / 2 : right;
        up = up > H / 2 ? H / 2 : up < -H / 2 ? -H / 2 : up;
    }
    int back = fb.pages == 2 ? fb.page ^ 1 : 0;
    uint8_t *page = fb.mem + (size_t)back * fb.stride * H;
    const size_t row = (size_t)W * 4, half = (size_t)EYE_W * 4;
    if (TILT_REPROJECTION && fabsf(tilt) > 0.0005f) {   /* a tilt worth turning for: 0.03°, half a pixel at the edge */
        turn_copy(page, tilt, right, up);
        turned++;
    }
    else if (right == 0 && up == 0 && fb.stride == row)
        memcpy(page, shown, row * H);
    else
        /* The framebuffer is the wearer's view turned 180°: their right is to the left here, their up is
         * down, and the eyes' halves are swapped; nothing crosses from one eye's half to the other's */
        for (int y = 0; y < H; y++) {
            uint8_t *dst = page + (size_t)y * fb.stride;
            int from = y - up;
            if (from < 0 || from >= H) {
                memset(dst, 0, row);
                continue;
            }
            const uint8_t *src = shown + (size_t)from * row;
            for (int e = 0; e < 2; e++) {
                uint8_t *d0 = dst + e * half;
                const uint8_t *s0 = src + e * half;
                size_t shift = (size_t)abs(right) * 4;
                if (right >= 0) {
                    memcpy(d0, s0 + shift, half - shift);
                    memset(d0 + half - shift, 0, shift);
                } else {
                    memset(d0, 0, shift);
                    memcpy(d0 + shift, s0, half - shift);
                }
            }
        }
    fb.page = back;
}

/* The display thread: at every refresh, the newest frame for the head as it is then */
static unsigned missed;   /* refreshes that came and went without a new picture */

/* A thread on the given cores, at a real-time priority (higher runs first), so that it isn't moved to a
 * slow core or kept waiting at the wrong moment */
void pin_thread(int first_core, int last_core, int priority)
{
    cpu_set_t cores;
    CPU_ZERO(&cores);
    for (int c = first_core; c <= last_core; c++)
        CPU_SET(c, &cores);
    struct sched_param param = { .sched_priority = priority };
    if (sched_setaffinity(0, sizeof(cores), &cores) != 0 || pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
        wlog("pinning a thread: %s", strerror(errno));
}

static void *display_thread(void *arg)
{
    (void)arg;
    /* The copy must be done within a refresh: on a fast core (4-7 are the 2.3 GHz ones), ahead of all else */
    pin_thread(7, 7, 50);
    double last = now(), flipped = 0, logged = 0;
    const uint8_t *previous = NULL;
    while (running) {
        float now_rotation[9], centre[3];
        double imu_age = head_sample_age();
        head_pose(DISPLAY_AHEAD, now_rotation, centre);
        double c0 = now();
        pthread_mutex_lock(&shown_lock);
        double c1 = now();
        int have = shown != NULL, fresh = shown != previous;
        previous = shown;
        if (have)
            frame_copy(now_rotation);
        pthread_mutex_unlock(&shown_lock);
        if (have) {
            double c2 = now();
            if (c2 - c1 > slowest_copy)
                slowest_copy = c2 - c1;
            fb.var.yoffset = fb.page * H;
            ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.var);   /* waits for the refresh */
            double c3 = now();
            copying += c3 - c0;
            refreshes++;
            if (flipped > 0 && c3 - flipped > 1.5 / FRAME_RATE) {
                missed++;
                if (c3 - logged > 1) {   /* at most one a second */
                    logged = c3;
                    wlog("missed a refresh: %.1f ms since the last flip; waited %.1f ms for the frame, copied in %.1f "
                         "(%s), flipped in %.1f; IMU sample %.1f ms old", 1000 * (c3 - flipped), 1000 * (c1 - c0),
                         1000 * (c2 - c1), fresh ? "its first copy" : "copied before", 1000 * (c3 - c2), 1000 * imu_age);
                }
            }
            flipped = c3;
        }
        double wait = last + 1.0 / FRAME_RATE - 0.002 - now();   /* in case flipping doesn't wait */
        if (!have || wait > 0)
            usleep(have ? (useconds_t)(wait * 1e6) : 5000);
        last = now();
    }
    return NULL;
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

/*
 * A snapshot for afterglowd's screenshot: the newest frame whole (reading the framebuffer itself, while
 * pages flip 72 times a second, mixes refreshes), as the wearer sees it, rows top first. "eyes" is
 * the eye buffers, undistorted (easiest to judge a scene by); "panels" is the frame warped for the
 * lenses, as it goes to the panels (before the refresh's slide for the head).
 */
static void snapshot(const char *file, int panels, char *reply, size_t size)
{
    uint32_t *image = malloc((size_t)W * H * 4);
    if (!image) {
        snprintf(reply, size, "error: no memory\n");
        return;
    }
    if (panels) {
        pthread_mutex_lock(&shown_lock);
        const uint32_t *frame = (const uint32_t *)shown;
        for (int i = 0; frame && i < W * H; i++)   /* the framebuffer is the wearer's view turned 180° */
            image[i] = frame[W * H - 1 - i];
        pthread_mutex_unlock(&shown_lock);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, eye_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, image);   /* rows bottom first */
        for (int y = 0; y < H / 2; y++)
            for (int x = 0; x < W; x++) {
                uint32_t t = image[(size_t)y * W + x];
                image[(size_t)y * W + x] = image[(size_t)(H - 1 - y) * W + x];
                image[(size_t)(H - 1 - y) * W + x] = t;
            }
    }
    FILE *f = fopen(file, "wb");
    int ok = f && fwrite(image, 4, (size_t)W * H, f) == (size_t)W * H;
    if (f && fclose(f) != 0)
        ok = 0;
    free(image);
    snprintf(reply, size, ok ? "%dx%d %s\n" : "error: can't write the snapshot\n", W, H, panels ? "panels" : "eyes");
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
    if (strncmp(line, "capture ", 8) == 0) {
        capture_command(line + 8, reply, sizeof(reply));
        line[0] = '\0';
    } else if (strncmp(line, "status ", 7) == 0) {   /* the rest of the line is the text */
        status_set(line + 7);
        snprintf(reply, sizeof(reply), "shown\n");
        line[0] = '\0';
    }
    char verb[16] = "", name[64] = "", path[512] = "", flag[16] = "";
    sscanf(line, "%15s %63s %511s %15s", verb, name, path, flag);
    if (strcmp(verb, "snapshot") == 0 && name[0]) {   /* (name is the file, path is which view) */
        snapshot(name, strcmp(path, "panels") == 0, reply, sizeof(reply));
        verb[0] = '\0';
    }
    if (strcmp(verb, "load") == 0 && name[0] && path[0])
        ext_load(name, path, strcmp(flag, "placeholder") == 0, reply, sizeof(reply));
    else if (strcmp(verb, "unload") == 0 && name[0])
        ext_unload(name, reply, sizeof(reply));
    else if (strcmp(verb, "list") == 0)
        ext_list(reply, sizeof(reply));
    else if (!verb[0] && reply[0])
        ;   /* answered above */
    else if (strcmp(verb, "test-tilt") == 0) {   /* development: turn every refresh by NAME degrees more */
        test_tilt = strtof(name, NULL) * (float)M_PI / 180;
        snprintf(reply, sizeof(reply), "every refresh now turned %.2f degrees more\n", test_tilt * 180 / M_PI);
    } else if (strcmp(verb, "recentre") == 0) {
        head_recentre();
        snprintf(reply, sizeof(reply), "the way the wearer faces is now ahead\n");
    }
    else if (strcmp(verb, "quit") == 0) {
        running = 0;
        snprintf(reply, sizeof(reply), "stopping\n");
    } else
        snprintf(reply, sizeof(reply), "error: commands are load NAME PATH [placeholder], unload NAME, list, recentre, quit\n");
    for (size_t off = 0, total = strlen(reply); off < total;) {
        ssize_t n = write(c, reply + off, total - off);
        if (n <= 0)
            break;
        off += n;
    }
    close(c);
}

/*
 * The GPU's clock. Its governor (msm-adreno-tz) judges by how busy the GPU is, and while the CPU copies
 * each frame the GPU sits idle often enough to stay at its lowest clock (257 MHz), well short of what
 * scenes need. So the world holds it at full clock while it runs, unless the chip gets hot, and gives
 * it back to the governor when it stops.
 */
#define GPU_LEVELS "/sys/class/kgsl/kgsl-3d0/min_pwrlevel"
#define GPU_FAST "0"      /* 710 MHz */
#define GPU_GOVERNED "6"  /* the lowest: the governor chooses */
#define HOT_C 70.0        /* the hottest SoC sensor above which the governor gets it back */
#define COOL_C 62.0

static void gpu_floor(const char *level)
{
    int fd = open(GPU_LEVELS, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, level, strlen(level)) < 0)
            wlog("setting the GPU's clock: %s", strerror(errno));
        close(fd);
    }
}

/* The hottest of the SoC's temperature sensors (tsens), in C */
static double soc_temperature(void)
{
    double hottest = 0;
    for (int i = 0; i < 64; i++) {
        char path[64], type[32] = "";
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int is_tsens = fgets(type, sizeof(type), f) && strncmp(type, "tsens_tz_sensor", 15) == 0;
        fclose(f);
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        long t;
        if (is_tsens && (f = fopen(path, "r"))) {
            if (fscanf(f, "%ld", &t) == 1 && t / 10.0 > hottest)
                hottest = t / 10.0;   /* tenths of a degree */
            fclose(f);
        }
    }
    return hottest;
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
    if (getenv("AFTERGLOW_WORLD_DIR")) {
        snprintf(texture_dir, sizeof(texture_dir), "%s/textures", getenv("AFTERGLOW_WORLD_DIR"));
        snprintf(memory_path, sizeof(memory_path), "%s/memory", getenv("AFTERGLOW_WORLD_DIR"));
    }
    memory_load(memory_path);
    GLuint eye_buffers, panel_image;
    if (gles_init() != 0 || gles_framebuffer(W, H, 0, 0, &panel_fbo, &panel_image) != 0 ||
        gles_framebuffer(2 * EYE_W, H, 1, 1, &eye_fbo, &eye_buffers) != 0 || lens_init() != 0 ||
        display_open() != 0 || frames_open() != 0 || make_floor() != 0 || make_standard_shader() != 0 ||
        ext_init() != 0)
        return 1;
    static const uint8_t white_pixel[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &white);
    glBindTexture(GL_TEXTURE_2D, white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &anisotropy);
    anisotropy = fminf(anisotropy, 8);
    if (paint_init(FONT_PATH) != 0)
        wlog("no font at %s: no text or signs", FONT_PATH);
    if (head_start() != 0)
        wlog("head tracking didn't start");
    int control = control_open();
    if (control < 0)
        return 1;
    FILE *owner = fopen(OWNER_PATH, "w");
    if (owner) {
        fprintf(owner, "%d\n", getpid());
        fclose(owner);
    }
    gpu_floor(GPU_FAST);
    int gpu_fast = 1;
    glEnable(GL_DEPTH_TEST);
    glLineWidth(2);
    wlog("running: %dx%d, lens-centred, roll %.1f° / %.1f°", W, H, roll[0] * 180 / M_PI, roll[1] * 180 / M_PI);

    pin_thread(4, 6, 20);   /* drawing: the fast cores, but never ahead of showing */
    for (int k = 1; k <= TURN_HELPERS; k++) {
        pthread_t helper;
        if (pthread_create(&helper, NULL, turn_helper, (void *)(intptr_t)k) == 0)
            pthread_detach(helper);
    }
    pthread_t display;
    if (pthread_create(&display, NULL, display_thread, NULL) != 0) {
        wlog("the display thread didn't start");
        return 1;
    }
    unsigned drawn = 0;
    double last = world_time(), report = last, drawing_time = 0, interval = 1.0 / FRAME_RATE;
    while (running) {
        control_poll(control);
        if (drawing_pbo >= 0) {   /* a frame is on the GPU: wait a little for it, then look for commands again */
            drawn += frame_collect(4000000);
            continue;
        }
        double t = world_time();   /* the GPU is free: draw the next frame, for when it will be seen */
        ext_frame((float)(t - last));
        interval += (fmin(t - last, 0.2) - interval) * 0.1;
        last = t;
        head_pose(fmin(interval + DISPLAY_AHEAD, PREDICT_MAX), head_rotation, head_centre);
        status_frame(t, head_rotation, head_centre);
        glBindFramebuffer(GL_FRAMEBUFFER, eye_fbo);
        glClearColor(background[0], background[1], background[2], 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int eye = 0; eye < 2; eye++)
            draw_eye(eye, (float)t);
        gles_frame_drawn();
        glBindFramebuffer(GL_FRAMEBUFFER, panel_fbo);   /* warped for the lenses onto what the panels show */
        glViewport(0, 0, W, H);
        lens_draw(eye_buffers);
        frame_submit();
        capture_frame(eye_buffers, panel_fbo);
        drawing_time += world_time() - t;
        if (world_time() - report >= 10) {
            int gpu_busy = -1;
            FILE *g = fopen("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "r");
            if (g) {
                if (fscanf(g, "%d", &gpu_busy) != 1)
                    gpu_busy = -1;
                fclose(g);
            }
            double span = world_time() - report;
            unsigned shown_count = refreshes;
            wlog("%.1f frames drawn and %.1f shown a second (reprojected in between, %u turned for a tilt; %u refreshes "
                 "missed; slowest copy %.1f ms; turning: a third of the rows took up to %.1f ms, helpers woke within "
                 "%.1f ms); %.1f ms a frame to submit; GPU %d%% busy", drawn / span, shown_count / span, turned, missed,
                 1000 * slowest_copy, 1000 * turn_rows_max, 1000 * turn_wake_max, drawn ? 1000 * drawing_time / drawn : 0,
                 gpu_busy);
            missed = turned = 0, slowest_copy = turn_rows_max = turn_wake_max = 0;
            ext_keep_all();   /* so a crash or power-off loses at most 10 s */
            memory_save(memory_path);   /* if anything changed */
            report = world_time(), drawn = 0, drawing_time = 0;
            refreshes = 0, copying = 0;
            double hot = soc_temperature();
            if (gpu_fast && hot > HOT_C) {
                gpu_floor(GPU_GOVERNED);
                gpu_fast = 0;
                wlog("the SoC is at %.0f C: the GPU's clock goes back to its governor", hot);
            } else if (!gpu_fast && hot < COOL_C) {
                gpu_floor(GPU_FAST);
                gpu_fast = 1;
                wlog("the SoC is down to %.0f C: the GPU runs at full clock again", hot);
            }
        }
    }
    pthread_join(display, NULL);
    ext_unload_all();   /* each keeps its state, for when the world comes back */
    memory_save(memory_path);
    gpu_floor(GPU_GOVERNED);
    unlink(OWNER_PATH);
    unlink(SOCKET_PATH);
    wlog("stopped");
    return 0;
}
