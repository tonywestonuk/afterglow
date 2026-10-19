/*
 * afterglow's world: a long-running scene on the GPU that extensions (WebAssembly modules) add to while
 * it runs (docs/apps.md). The world holds all the state; extensions add behaviour and looks.
 */
#ifndef WORLD_H
#define WORLD_H

#include <stddef.h>
#include <stdint.h>

/* A line in the world's log (/tmp/world.log) and the kernel log */
void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Seconds since the world started */
double world_time(void);

/*
 * What extensions can do: the same API as sdk/afterglow.h, plus the calling extension's number. What
 * an extension creates is its own and goes when it's unloaded; what it changes on anything else is put
 * back. Functions return an id or 0, or -1 with the reason in world_error().
 */
#define PRIMITIVE_TRIANGLES 0
#define PRIMITIVE_LINES 1
#define PRIMITIVE_POINTS 2
#define VERTEX_FLOATS 12   /* position xyz, normal xyz, uv, colour rgba */
#define MAX_UNIFORM_FLOATS 16

int world_mesh_create(int ext, const float *vertices, int vertex_count, const uint32_t *indices, int index_count,
                      int primitive);
int world_shader_create(int ext, const char *vertex_glsl, const char *fragment_glsl);
int world_object_create(int ext, const char *name);
int world_object_find(const char *name);
int world_object_set_mesh(int ext, int object, int mesh);
int world_object_set_shader(int ext, int object, int shader);
int world_object_set_transform(int ext, int object, const float matrix[16]);
int world_object_set_visible(int ext, int object, int visible);
int world_object_set_uniform(int ext, int object, const char *name, const float *values, int count);
int world_set_background(int ext, float r, float g, float b);
const char *world_error(void);
/* Free what an extension created and put back what it changed */
void world_release(int ext);

/* Extensions (ext.c). Results go in reply, one or more lines. */
int ext_init(void);
int ext_load(const char *name, const char *path, char *reply, size_t size);
int ext_unload(const char *name, char *reply, size_t size);
void ext_list(char *reply, size_t size);
/* Every loaded extension's on_frame(dt); one that fails is unloaded */
void ext_frame(float dt);

#endif
