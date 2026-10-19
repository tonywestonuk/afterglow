/*
 * afterglow world API, for extensions (docs/apps.md).
 *
 * An extension is a C file, compiled to WebAssembly on the headset, that defines any of:
 *   void on_load(void)          once when it's loaded: make meshes, shaders and objects here
 *   void on_frame(float dt)     every frame; dt is the seconds since the last one
 *   void on_unload(void)        just before it's replaced or removed
 * Everything an extension creates goes when it's unloaded, and everything it changed on objects it
 * didn't create (such as the world's "floor") is put back, so each new version starts from a clean
 * world. A crash (a trap) or an endless loop unloads just that extension.
 *
 * The C library is there (malloc, math.h, printf to the world's log, ...).
 *
 * Space: metres, y up, the floor is the plane y = 0 (20 m across, centred on the origin). The wearer's
 * eyes are at (-0.0315, 1.6, 0) and (+0.0315, 1.6, 0), looking along -z. No head tracking yet.
 *
 * Shaders: OpenGL ES 3.0 GLSL, starting "#version 300 es". Vertex inputs: a_position (vec3),
 * a_normal (vec3), a_uv (vec2), a_colour (vec4), from the vertex struct below. Uniforms the world
 * sets each frame: u_mvp and u_model (mat4), u_time (float, seconds), u_eye (vec3, the eye being
 * drawn); plus any set with object_set_uniform. Rendering is 4x multisampled, depth tested.
 */
#ifndef AFTERGLOW_H
#define AFTERGLOW_H

#include <stdint.h>

#define AG_IMPORT(name) __attribute__((import_module("afterglow"), import_name(#name)))

enum { TRIANGLES = 0, LINES = 1, POINTS = 2 };

typedef struct {
    float x, y, z;      /* position */
    float nx, ny, nz;   /* normal */
    float u, v;         /* texture coordinates */
    float r, g, b, a;   /* colour, 0 to 1 */
} vertex;

/* Functions returning an int give an id (1 or more) or 0 for success; -1 means it failed, and
 * last_error() says why (for shaders, the compiler's messages). */

/* A mesh: vertices, and optionally indices into them (pass 0 and 0 for none) */
AG_IMPORT(mesh_create) int mesh_create(const vertex *vertices, int vertex_count, const uint32_t *indices,
                                       int index_count, int primitive);
/* A shader program from GLSL ES 3.0 source */
AG_IMPORT(shader_create) int shader_create(const char *vertex_glsl, const char *fragment_glsl);

/* Objects: a mesh drawn with a shader, at a transform. Names are unique; the world has "floor". */
AG_IMPORT(object_create) int object_create(const char *name);
AG_IMPORT(object_find) int object_find(const char *name);
AG_IMPORT(object_set_mesh) int object_set_mesh(int object, int mesh);
AG_IMPORT(object_set_shader) int object_set_shader(int object, int shader);
AG_IMPORT(object_set_transform) int object_set_transform(int object, const float matrix[16]);   /* column-major */
AG_IMPORT(object_set_visible) int object_set_visible(int object, int visible);
/* A uniform for this object's shader: 1-4 floats (float..vec4), 16 (mat4), or an array of floats */
AG_IMPORT(object_set_uniform) int object_set_uniform(int object, const char *name, const float *values, int count);

/* The colour behind everything */
AG_IMPORT(set_background) int set_background(float r, float g, float b);
/* Seconds since the world started */
AG_IMPORT(world_seconds) double world_seconds(void);
/* A line in the world's log */
AG_IMPORT(log_message) void log_message(const char *text);
/* Why the last call failed; returns its length */
AG_IMPORT(last_error) int last_error(char *buffer, int size);

#endif
