/*
 * Building blocks for afterglow world extensions: #include "shapes.h" (it includes afterglow.h).
 *
 *   shape("hut", BOX, v3(0, 1, -4), v3(2, 2, 2), 0xb08850);         a whole object in one line
 *   label("title", "Hello", v3(0, 2.5f, -4), 0.3f, 0, 0xffffff);    text you can read in the world
 *
 *   mesh_builder b = { 0 };                                            many shapes in one mesh, which
 *   mb_box(&b, v3(0, 0.5f, -3), v3(1, 1, 1), 0x808080);               draws far faster than many objects
 *   mb_pose(&b, v3(2, 0, -3), 45, v3(1, 1, 1));                       (later shapes: moved and turned)
 *   mb_cylinder(&b, v3(0, 1, 0), v3(0.3f, 2, 0.3f), 0x604020);
 *   int mesh = mb_mesh(&b);
 *
 * Solid shapes take a centre and a size (the box around them, in metres) and a colour 0xRRGGBB. They
 * come with normals, and uv in metres along the surface, so a texture repeats every metre (times the
 * object's u_uv_scale). Yaw is in degrees about y: positive turns -z (ahead) towards -x (left).
 */
#ifndef AFTERGLOW_SHAPES_H
#define AFTERGLOW_SHAPES_H

#include <math.h>
#include <stdlib.h>

#include "afterglow.h"

#define AG_PI 3.14159265358979f

/* ---- Vectors ---- */

typedef struct { float x, y, z; } vec3;

static inline vec3 v3(float x, float y, float z) { return (vec3){ x, y, z }; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline vec3 v3_mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3_length(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_lerp(vec3 a, vec3 b, float t) { return v3_add(a, v3_scale(v3_sub(b, a), t)); }
static inline vec3 v3_normalize(vec3 a)
{
    float n = v3_length(a);
    return n > 0 ? v3_scale(a, 1 / n) : a;
}

/* ---- Transforms: 4x4, column-major, as object_set_transform takes them ---- */

static inline void mat4_identity(float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = i % 5 == 0;
}

/* out = a * b (b happens first) */
static inline void mat4_multiply(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = 0;
            for (int k = 0; k < 4; k++)
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
        }
    for (int i = 0; i < 16; i++)
        out[i] = r[i];
}

/* Scale, then turn yaw degrees about y, then move to `at` */
static inline void mat4_pose(float m[16], vec3 at, float yaw_degrees, vec3 scale)
{
    float c = cosf(yaw_degrees * AG_PI / 180), s = sinf(yaw_degrees * AG_PI / 180);
    float r[16] = { c * scale.x, 0, -s * scale.x, 0, 0, scale.y, 0, 0, s * scale.z, 0, c * scale.z, 0, at.x, at.y, at.z, 1 };
    for (int i = 0; i < 16; i++)
        m[i] = r[i];
}

/* A turn of `degrees` about `axis` (anticlockwise looking down the axis towards the origin) */
static inline void mat4_rotation(float m[16], vec3 axis, float degrees)
{
    vec3 a = v3_normalize(axis);
    float c = cosf(degrees * AG_PI / 180), s = sinf(degrees * AG_PI / 180), t = 1 - c;
    float r[16] = { t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y, 0,
                    t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x, 0,
                    t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c, 0,
                    0, 0, 0, 1 };
    for (int i = 0; i < 16; i++)
        m[i] = r[i];
}

static inline vec3 mat4_apply(const float m[16], vec3 p)
{
    return v3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
              m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

/* Put an object somewhere: mat4_pose and object_set_transform in one */
static inline int place(int object, vec3 at, float yaw_degrees, vec3 scale)
{
    float m[16];
    mat4_pose(m, at, yaw_degrees, scale);
    return object_set_transform(object, m);
}

/* Colour an object drawn with the standard shader (its u_colour); alpha below 1 needs BLEND_ALPHA */
static inline int object_colour(int object, uint32_t rgb, float alpha)
{
    float c[4] = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, alpha };
    return object_set_uniform(object, "u_colour", c, 4);
}

/* ---- Meshes, built from shapes ---- */

typedef struct {
    vertex *v;
    int count, capacity, failed;
    int sides;          /* around spheres, cylinders and cones; 0 means 24 */
    int transformed;    /* if set (mb_pose, mb_transform), shapes are moved by m */
    float m[16], n[9];  /* n: how normals turn */
} mesh_builder;

/* Move and turn the shapes added after this (until the next call) */
static inline void mb_transform(mesh_builder *b, const float m[16])
{
    float a = m[0], bb = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float cof[9] = { e * i - f * h, -(d * i - f * g), d * h - e * g, -(bb * i - c * h), a * i - c * g,
                     -(a * h - bb * g), bb * f - c * e, -(a * f - c * d), a * e - bb * d };
    for (int k = 0; k < 16; k++)
        b->m[k] = m[k];
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            b->n[col * 3 + row] = cof[row * 3 + col];
    b->transformed = 1;
}

static inline void mb_pose(mesh_builder *b, vec3 at, float yaw_degrees, vec3 scale)
{
    float m[16];
    mat4_pose(m, at, yaw_degrees, scale);
    mb_transform(b, m);
}

static inline void mb_vertex(mesh_builder *b, vec3 p, vec3 normal, float u, float v, uint32_t rgb)
{
    if (b->count == b->capacity) {
        int capacity = b->capacity ? b->capacity * 2 : 1024;
        vertex *grown = realloc(b->v, capacity * sizeof(vertex));
        if (!grown) {
            b->failed = 1;
            return;
        }
        b->v = grown, b->capacity = capacity;
    }
    if (b->transformed) {
        p = mat4_apply(b->m, p);
        normal = v3_normalize(v3(b->n[0] * normal.x + b->n[3] * normal.y + b->n[6] * normal.z,
                                 b->n[1] * normal.x + b->n[4] * normal.y + b->n[7] * normal.z,
                                 b->n[2] * normal.x + b->n[5] * normal.y + b->n[8] * normal.z));
    }
    b->v[b->count++] = (vertex){ p.x, p.y, p.z, normal.x, normal.y, normal.z, u, v,
                                 ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1 };
}

static inline void mb_triangle(mesh_builder *b, vec3 p0, vec3 p1, vec3 p2, uint32_t rgb)
{
    vec3 e1 = v3_sub(p1, p0), n = v3_normalize(v3_cross(e1, v3_sub(p2, p0)));
    vec3 u = v3_normalize(e1), w = v3_cross(n, u);
    vec3 p[3] = { p0, p1, p2 };
    for (int i = 0; i < 3; i++)
        mb_vertex(b, p[i], n, v3_dot(v3_sub(p[i], p0), u), v3_dot(v3_sub(p[i], p0), w), rgb);
}

/* A flat four-sided piece, corners anticlockwise seen from its front (bottom left, bottom right, top
 * right, top left), with the texture from (u0, v0) at the top left to (u1, v1) at the bottom right */
static inline void mb_quad_uv(mesh_builder *b, vec3 p0, vec3 p1, vec3 p2, vec3 p3, float u0, float v0, float u1,
                              float v1, uint32_t rgb)
{
    vec3 n = v3_normalize(v3_cross(v3_sub(p1, p0), v3_sub(p3, p0)));
    mb_vertex(b, p0, n, u0, v1, rgb), mb_vertex(b, p1, n, u1, v1, rgb), mb_vertex(b, p2, n, u1, v0, rgb);
    mb_vertex(b, p0, n, u0, v1, rgb), mb_vertex(b, p2, n, u1, v0, rgb), mb_vertex(b, p3, n, u0, v0, rgb);
}

/* The same, with uv in metres */
static inline void mb_quad(mesh_builder *b, vec3 p0, vec3 p1, vec3 p2, vec3 p3, uint32_t rgb)
{
    vec3 n = v3_normalize(v3_cross(v3_sub(p1, p0), v3_sub(p3, p0))), u = v3_normalize(v3_sub(p1, p0));
    vec3 w = v3_cross(n, u), p[4] = { p0, p1, p2, p3 };
    float uv[4][2];
    for (int i = 0; i < 4; i++)
        uv[i][0] = v3_dot(v3_sub(p[i], p0), u), uv[i][1] = v3_dot(v3_sub(p[i], p0), w);
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        mb_vertex(b, p[order[i]], n, uv[order[i]][0], uv[order[i]][1], rgb);
}

/* A level rectangle facing up: a floor, a table top, a pond */
static inline void mb_plane(mesh_builder *b, vec3 centre, float width, float depth, uint32_t rgb)
{
    float x = width / 2, z = depth / 2;
    mb_quad(b, v3_add(centre, v3(-x, 0, z)), v3_add(centre, v3(x, 0, z)), v3_add(centre, v3(x, 0, -z)),
            v3_add(centre, v3(-x, 0, -z)), rgb);
}

static inline void mb_box(mesh_builder *b, vec3 centre, vec3 size, uint32_t rgb)
{
    static const signed char faces[6][4][3] = {
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },       /* +x */
        { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },   /* -x */
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },       /* top */
        { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },   /* bottom */
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },       /* front, +z */
        { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },   /* back */
    };
    vec3 h = v3_scale(size, 0.5f);
    for (int f = 0; f < 6; f++) {
        vec3 p[4];
        for (int i = 0; i < 4; i++)
            p[i] = v3_add(centre, v3(faces[f][i][0] * h.x, faces[f][i][1] * h.y, faces[f][i][2] * h.z));
        mb_quad(b, p[0], p[1], p[2], p[3], rgb);
    }
}

/* A roof shape: a ridge along x at the top, sloping to the bottom front and back edges */
static inline void mb_wedge(mesh_builder *b, vec3 centre, vec3 size, uint32_t rgb)
{
    vec3 h = v3_scale(size, 0.5f);
    vec3 bfl = v3_add(centre, v3(-h.x, -h.y, h.z)), bfr = v3_add(centre, v3(h.x, -h.y, h.z));
    vec3 bbl = v3_add(centre, v3(-h.x, -h.y, -h.z)), bbr = v3_add(centre, v3(h.x, -h.y, -h.z));
    vec3 rl = v3_add(centre, v3(-h.x, h.y, 0)), rr = v3_add(centre, v3(h.x, h.y, 0));
    mb_quad(b, bfl, bfr, rr, rl, rgb);
    mb_quad(b, bbr, bbl, rl, rr, rgb);
    mb_quad(b, bbl, bbr, bfr, bfl, rgb);
    mb_triangle(b, bbl, bfl, rl, rgb);
    mb_triangle(b, bfr, bbr, rr, rgb);
}

/* A sphere (or, with unequal sizes, an ellipsoid) */
static inline void mb_sphere(mesh_builder *b, vec3 centre, vec3 size, uint32_t rgb)
{
    int around = b->sides > 2 ? b->sides : 24, up = around / 2 > 2 ? around / 2 : 3;
    vec3 r = v3_scale(size, 0.5f);
    for (int i = 0; i < around; i++)
        for (int j = 0; j < up; j++) {
            static const int corners[6][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 0, 0 } };
            for (int k = 0; k < 6; k++) {
                float a = 2 * AG_PI * (i + corners[k][0]) / around, t = AG_PI * (j + corners[k][1]) / up;
                vec3 d = v3(sinf(t) * sinf(a), cosf(t), sinf(t) * cosf(a));
                vec3 n = v3_normalize(v3(d.x / (r.x ? r.x : 1), d.y / (r.y ? r.y : 1), d.z / (r.z ? r.z : 1)));
                mb_vertex(b, v3_add(centre, v3_mul(d, r)), n, a * r.x, t * r.y, rgb);
            }
        }
}

/* A solid of revolution about a vertical axis through `base`: profile is (radius, height) pairs from the
 * bottom up; start or end at radius 0 to close it (vases, bottles, columns, domes, trees) */
static inline void mb_lathe(mesh_builder *b, vec3 base, const float *profile, int points, uint32_t rgb)
{
    int around = b->sides > 2 ? b->sides : 24;
    for (int p = 0; p + 1 < points; p++) {
        float r0 = profile[2 * p], y0 = profile[2 * p + 1], r1 = profile[2 * p + 2], y1 = profile[2 * p + 3];
        float slope_v = (y1 - y0) * (y1 - y0) + (r1 - r0) * (r1 - r0);
        float along = 0;
        for (int q = 0; q < p; q++)
            along += hypotf(profile[2 * q + 2] - profile[2 * q], profile[2 * q + 3] - profile[2 * q + 1]);
        for (int i = 0; i < around; i++) {
            static const int corners[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
            for (int k = 0; k < 6; k++) {
                float a = 2 * AG_PI * (i + corners[k][0]) / around, r = corners[k][1] ? r1 : r0;
                float y = corners[k][1] ? y1 : y0;
                vec3 d = v3(sinf(a), 0, cosf(a));
                vec3 n = v3_normalize(v3_add(v3_scale(d, y1 - y0), v3(0, r0 - r1, 0)));
                mb_vertex(b, v3_add(base, v3(d.x * r, y, d.z * r)), n, a * fmaxf(r0, r1),
                          along + (corners[k][1] ? sqrtf(slope_v) : 0), rgb);
            }
        }
    }
}

/* A cylinder (top = 1) or cone (top = 0) with a flat bottom, and flat top if it has one */
static inline void mb_frustum(mesh_builder *b, vec3 centre, vec3 size, float top, uint32_t rgb)
{
    int around = b->sides > 2 ? b->sides : 24;
    float rx = size.x / 2, rz = size.z / 2, y0 = centre.y - size.y / 2, y1 = centre.y + size.y / 2;
    for (int i = 0; i < around; i++) {
        float a0 = 2 * AG_PI * i / around, a1 = 2 * AG_PI * (i + 1) / around;
        vec3 p0 = v3(centre.x + rx * sinf(a0), y0, centre.z + rz * cosf(a0));
        vec3 p1 = v3(centre.x + rx * sinf(a1), y0, centre.z + rz * cosf(a1));
        vec3 q0 = v3(centre.x + top * rx * sinf(a0), y1, centre.z + top * rz * cosf(a0));
        vec3 q1 = v3(centre.x + top * rx * sinf(a1), y1, centre.z + top * rz * cosf(a1));
        vec3 n0 = v3_normalize(v3(sinf(a0) * size.y / (rx ? rx : 1), (1 - top), cosf(a0) * size.y / (rz ? rz : 1)));
        vec3 n1 = v3_normalize(v3(sinf(a1) * size.y / (rx ? rx : 1), (1 - top), cosf(a1) * size.y / (rz ? rz : 1)));
        float u0 = a0 * rx, u1 = a1 * rx;
        mb_vertex(b, p0, n0, u0, 0, rgb), mb_vertex(b, p1, n1, u1, 0, rgb), mb_vertex(b, q1, n1, u1, size.y, rgb);
        mb_vertex(b, p0, n0, u0, 0, rgb), mb_vertex(b, q1, n1, u1, size.y, rgb), mb_vertex(b, q0, n0, u0, size.y, rgb);
        vec3 c0 = v3(centre.x, y0, centre.z), c1 = v3(centre.x, y1, centre.z);
        mb_vertex(b, c0, v3(0, -1, 0), c0.x, c0.z, rgb), mb_vertex(b, p1, v3(0, -1, 0), p1.x, p1.z, rgb);
        mb_vertex(b, p0, v3(0, -1, 0), p0.x, p0.z, rgb);
        if (top > 0) {
            mb_vertex(b, c1, v3(0, 1, 0), c1.x, c1.z, rgb), mb_vertex(b, q0, v3(0, 1, 0), q0.x, q0.z, rgb);
            mb_vertex(b, q1, v3(0, 1, 0), q1.x, q1.z, rgb);
        }
    }
}

static inline void mb_cylinder(mesh_builder *b, vec3 centre, vec3 size, uint32_t rgb) { mb_frustum(b, centre, size, 1, rgb); }
static inline void mb_cone(mesh_builder *b, vec3 centre, vec3 size, uint32_t rgb) { mb_frustum(b, centre, size, 0, rgb); }

/* Make the mesh (TRIANGLES) and empty the builder for reuse. The mesh id, or -1 */
static inline int mb_mesh(mesh_builder *b)
{
    int mesh = b->failed || b->count == 0 ? -1 : mesh_create(b->v, b->count, 0, 0, TRIANGLES);
    free(b->v);
    b->v = 0, b->count = b->capacity = b->failed = 0;
    return mesh;
}

/* ---- Whole objects ---- */

enum { BOX, SPHERE, CYLINDER, CONE, WEDGE, PLANE };

/* One object of one shape, `size` across, centred at `at`, in the standard shader coloured rgb: for
 * placeholders and quick scenery. Move it with place(); recolour it with object_colour(). The object,
 * or -1 */
static inline int shape(const char *name, int kind, vec3 at, vec3 size, uint32_t rgb)
{
    mesh_builder b = { 0 };
    vec3 o = v3(0, 0, 0);
    if (kind == BOX)
        mb_box(&b, o, size, 0xffffff);
    else if (kind == SPHERE)
        mb_sphere(&b, o, size, 0xffffff);
    else if (kind == CYLINDER)
        mb_cylinder(&b, o, size, 0xffffff);
    else if (kind == CONE)
        mb_cone(&b, o, size, 0xffffff);
    else if (kind == WEDGE)
        mb_wedge(&b, o, size, 0xffffff);
    else
        mb_plane(&b, o, size.x, size.z, 0xffffff);
    int mesh = mb_mesh(&b), object = mesh < 0 ? -1 : object_create(name);
    if (object < 0 || object_set_mesh(object, mesh) || object_set_shader(object, shader_standard()) ||
        object_colour(object, rgb, 1) || place(object, at, 0, v3(1, 1, 1)))
        return -1;
    return object;
}

/* Text standing in the world: `height` metres per line, centred at `at`, facing yaw degrees from +z (the
 * wearer, at first). Lines split at \n. The object, or -1 */
static inline int label(const char *name, const char *text, vec3 at, float height, float yaw_degrees, uint32_t ink)
{
    int lines = 1, w, h;
    for (const char *s = text; *s; s++)
        lines += *s == '\n';
    int texture = texture_text(text, 96, ink);
    if (texture < 0 || texture_size(texture, &w, &h))
        return -1;
    float y = height * lines * 1.1f / 2, x = y * w / h;
    mesh_builder b = { 0 };
    mb_quad_uv(&b, v3(-x, -y, 0), v3(x, -y, 0), v3(x, y, 0), v3(-x, y, 0), 0, 0, 1, 1, 0xffffff);
    int mesh = mb_mesh(&b), object = mesh < 0 ? -1 : object_create(name);
    if (object < 0 || object_set_mesh(object, mesh) || object_set_shader(object, shader_standard()) ||
        object_set_texture(object, 0, texture) || object_set_blend(object, BLEND_ALPHA) ||
        place(object, at, yaw_degrees, v3(1, 1, 1)))
        return -1;
    return object;
}

/* ---- Randomness ---- */

/* A pseudo-random number from lo to hi (the same sequence every load, so scenes come back the same) */
static inline float random_between(float lo, float hi)
{
    static uint32_t state = 2463534242u;
    state ^= state << 13, state ^= state >> 17, state ^= state << 5;
    return lo + (hi - lo) * (state >> 8) * (1.0f / 16777216.0f);
}

/* Smooth value noise, -1 to 1, with features about 1 unit across: terrain, patches, wobble */
static inline float noise2(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy, corner[4];
    for (int k = 0; k < 4; k++) {
        uint32_t h = (uint32_t)(ix + (k & 1)) * 374761393u + (uint32_t)(iy + (k >> 1)) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        corner[k] = (h ^ (h >> 16)) * (2.0f / 4294967295.0f) - 1;
    }
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    return (corner[0] + (corner[1] - corner[0]) * fx) * (1 - fy) + (corner[2] + (corner[3] - corner[2]) * fx) * fy;
}

#endif
