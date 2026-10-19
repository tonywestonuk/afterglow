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
#define TEXTURE_SLOTS 4
#define BLEND_SOLID 0
#define BLEND_ALPHA 1
#define BLEND_ADD 2

int world_mesh_create(int ext, const float *vertices, int vertex_count, const uint32_t *indices, int index_count,
                      int primitive);
int world_shader_create(int ext, const char *vertex_glsl, const char *fragment_glsl);
int world_shader_standard(void);
unsigned world_shader_program(int shader);   /* its OpenGL program */
int world_texture_create(int ext, const uint8_t *rgba, int width, int height);
int world_texture_load(int ext, const char *name);
int world_texture_text(int ext, const char *text, int pixel_height, uint32_t ink);
int world_texture_size(int texture, int *width, int *height);
int world_object_create(int ext, const char *name);
int world_object_find(const char *name);
int world_object_set_mesh(int ext, int object, int mesh);
int world_object_set_shader(int ext, int object, int shader);
int world_object_set_transform(int ext, int object, const float matrix[16]);
int world_object_set_visible(int ext, int object, int visible);
int world_object_set_uniform(int ext, int object, const char *name, const float *values, int count);
int world_object_set_texture(int ext, int object, int slot, int texture);
int world_object_set_blend(int ext, int object, int mode);
int world_set_background(int ext, float r, float g, float b);
const char *world_error(void);
/* Set the reason world_error() gives and return -1 */
int world_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Any object's transform, whoever made it */
int world_object_get_transform(int object, float matrix[16]);

/* The world's memory (memory.c): named values that outlive extensions' versions and the world */
int world_remember(const char *key, const void *data, int size);
int world_recall(const char *key, void *data, int size);   /* the size kept, or -1 if none */
void memory_load(const char *path);
void memory_save(const char *path);
/* Free what an extension created and put back what it changed */
void world_release(int ext);

/* For the world's own things (sign.c): an object that blinks amber, and the box around everything an
 * extension made (0, or -1 if it made nothing visible) */
int world_object_set_blink(int object, int blink);
int world_extension_bounds(int ext, float lo[3], float hi[3]);
/* Put a road-works sign beside everything an extension made, to say it's a placeholder */
int sign_place(int ext, const char *name);

/* Painting on the CPU (paint.c): text and the road-works sign, as RGBA images (free them) */
int paint_init(const char *font_path);
uint8_t *paint_text(const char *text, int pixel_height, uint32_t ink, float weight, int *width, int *height);
uint8_t *paint_sign(int *width, int *height, uint8_t **mask);
uint8_t *paint_load(const char *path, int *width, int *height);
const char *paint_load_error(void);

/* Head tracking, turning only (head.c). Eyes are EYE_HEIGHT above the floor when the head is upright */
#define EYE_HEIGHT 1.6f
int head_start(void);
void head_recentre(void);
/* The head `ahead` seconds from now: rotation (head to world, column-major 3x3), the point between the
 * eyes; 1 if tracked */
int head_pose(double ahead, float rotation[9], float centre[3]);
void head_quaternion(double ahead, float xyzw[4]);
double head_sample_age(void);
/* Run this thread on cores first..last (4-7 are the fast ones) at a real-time priority */
void pin_thread(int first_core, int last_core, int priority);

/* Extensions (ext.c). Results go in reply, one or more lines. */
int ext_init(void);
/* A placeholder gets a road-works sign beside what it makes (sign.c) */
int ext_load(const char *name, const char *path, int placeholder, char *reply, size_t size);
int ext_unload(const char *name, char *reply, size_t size);
void ext_list(char *reply, size_t size);
/* As the world stops: every extension's on_unload(), so they can keep their state */
void ext_unload_all(void);
/* Every loaded extension's on_frame(dt); one that fails is unloaded */
void ext_frame(float dt);

#endif
