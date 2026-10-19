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
 * The C library is there (malloc, math.h, printf to the world's log, ...). shapes.h has ready-made
 * building blocks: vectors, transforms, meshes from boxes, spheres, cylinders and the like, whole
 * objects in one line, text labels. Most extensions want it: #include "shapes.h".
 *
 * Space: metres, y up, the floor is the plane y = 0 (20 m across, centred on the origin). The wearer's
 * eyes are at (-0.0315, 1.6, 0) and (+0.0315, 1.6, 0), looking along -z. No head tracking yet.
 * Each eye sees about 90 degrees across, centred on its lens; things nearer than 0.05 m or farther
 * than 500 m are clipped. The background starts as (0.02, 0.02, 0.04).
 *
 * Drawing: 4x multisampled, depth tested, both sides of every triangle (no culling). Solid objects
 * first, then blended ones (object_set_blend), farthest first. The standard shader (shader_standard)
 * lights things for you; write GLSL only for special effects.
 *
 * Shaders: OpenGL ES 3.0 GLSL, starting "#version 300 es". Vertex inputs: a_position (vec3),
 * a_normal (vec3), a_uv (vec2), a_colour (vec4), plain floats from the vertex struct below. Uniforms
 * the world sets each frame: u_mvp and u_model (mat4), u_normal (mat3, for normals), u_time (float,
 * seconds), u_eye (vec3, the eye being drawn), u_texture0..u_texture3 (sampler2D); plus any set with
 * object_set_uniform.
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

/*
 * The standard shader, so most things need no GLSL: lit by a sun (high, behind the wearer and a little
 * to the right) and the sky. An object's colour is the uniform u_colour (vec4, default white) x the vertex
 * colour x texture slot 0 (default white) at a_uv x u_uv_scale (vec2, default 1, 1), plus u_emission
 * (vec3, default black) to make it glow. Solid objects drop pixels whose alpha is under 0.5, for
 * cut-outs such as leaves.
 */
AG_IMPORT(shader_standard) int shader_standard(void);

/* Textures: they repeat, and are mipmapped. One from pixels: RGBA, 8 bits each, the top row first */
AG_IMPORT(texture_create) int texture_create(const uint8_t *rgba, int width, int height);
/* One from an image on the headset, /storage/world/textures/NAME.png or .jpg: afterglowd's
 * generate_texture and fetch_texture put them there */
AG_IMPORT(texture_load) int texture_load(const char *name);
/* Text: ink (0xRRGGBB) on transparent, pixel_height (8 to 512) per line, lines (\n) centred. Show it
 * on a blended object (label() in shapes.h does it all) */
AG_IMPORT(texture_text) int texture_text(const char *text, int pixel_height, uint32_t ink);
AG_IMPORT(texture_size) int texture_size(int texture, int *width, int *height);
/* The texture an object's shader reads as u_texture0..u_texture3 (slot 0 to 3); 0 for plain white */
AG_IMPORT(object_set_texture) int object_set_texture(int object, int slot, int texture);

/* BLEND_SOLID (the default); BLEND_ALPHA mixes by alpha with what's behind (glass, smoke, text);
 * BLEND_ADD adds light (glows, sparks, halos). Blended objects don't hide what's behind them */
enum { BLEND_SOLID = 0, BLEND_ALPHA = 1, BLEND_ADD = 2 };
AG_IMPORT(object_set_blend) int object_set_blend(int object, int mode);

/* The colour behind everything */
AG_IMPORT(set_background) int set_background(float r, float g, float b);
/* Seconds since the world started */
AG_IMPORT(world_seconds) double world_seconds(void);
/* A line in the world's log */
AG_IMPORT(log_message) void log_message(const char *text);
/* Why the last call failed; returns its length */
AG_IMPORT(last_error) int last_error(char *buffer, int size);

#endif
