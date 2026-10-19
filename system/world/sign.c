/*
 * The road-works sign beside a placeholder: something quick an extension put up while the real thing is
 * still being written. It stands on the floor at the front right of everything the placeholder made,
 * turned to the wearer, with an amber lamp flashing on top; it belongs to the placeholder, so it goes
 * when the real version replaces it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

/* Made once and shared by every sign: the painted face and its outline (for the back), and three meshes */
static int face_texture, back_texture, face_mesh, back_mesh, lamp_mesh;

#define POST 0.025f       /* half the post's width */
#define LAMP_Y 1.95f
#define LAMP_RADIUS 0.06f

static float *put(float *v, float x, float y, float z, float nx, float ny, float nz, float u, float t)
{
    float vertex[VERTEX_FLOATS] = { x, y, z, nx, ny, nz, u, t, 1, 1, 1, 1 };
    memcpy(v, vertex, sizeof(vertex));
    return v + VERTEX_FLOATS;
}

/* A rectangle facing +z (or -z), x0..x1 by y0..y1 at depth z, showing texture rows v0 (top) to v1 */
static float *panel(float *v, float x0, float x1, float y0, float y1, float z, float v0, float v1, int back)
{
    float n = back ? -1 : 1;
    float corners[4][4] = { { x0, y0, 0, v1 }, { x1, y0, 1, v1 }, { x1, y1, 1, v0 }, { x0, y1, 0, v0 } };
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        const float *c = corners[order[i]];
        v = put(v, c[0], c[1], z, 0, 0, n, back ? 1 - c[2] : c[2], c[3]);
    }
    return v;
}

static int make_sign(void)
{
    int w, h;
    uint8_t *mask = NULL, *face = paint_sign(&w, &h, &mask);
    if (!face)
        return -1;
    face_texture = world_texture_create(0, face, w, h);
    back_texture = world_texture_create(0, mask, w, h);
    free(face);
    free(mask);

    /* Sizes in metres: a 0.75 m triangle (painted on a 0.8 m square) over a 0.75 x 0.19 m plate */
    const float plate = 1.0f, square = 1.13f, split = 512.0f / 640;
    float v[64 * VERTEX_FLOATS], *p = v;
    p = panel(p, -0.4f, 0.4f, square, square + 0.8f, 0, 0, split, 0);
    p = panel(p, -0.375f, 0.375f, plate, plate + 0.1875f, 0, split, 1, 0);
    face_mesh = world_mesh_create(0, v, (int)(p - v) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);

    p = v;
    p = panel(p, -0.4f, 0.4f, square, square + 0.8f, -0.004f, 0, split, 1);
    p = panel(p, -0.375f, 0.375f, plate, plate + 0.1875f, -0.004f, split, 1, 1);
    /* The post, behind: four sides, read from the opaque middle of the plate */
    const float x0 = -POST, x1 = POST, z0 = -0.03f - POST, z1 = -0.03f + POST;
    const float sides[4][6] = {   /* from x, z; to x, z; normal x, z */
        { x1, z1, x1, z0, 1, 0 }, { x0, z1, x1, z1, 0, 1 }, { x0, z0, x0, z1, -1, 0 }, { x1, z0, x0, z0, 0, -1 } };
    for (int i = 0; i < 4; i++) {
        const float *d = sides[i];
        float corners[4][3] = { { d[0], 0, d[1] }, { d[2], 0, d[3] }, { d[2], LAMP_Y, d[3] }, { d[0], LAMP_Y, d[1] } };
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++)
            p = put(p, corners[order[k]][0], corners[order[k]][1], corners[order[k]][2], d[4], 0, d[5], 0.5f, 0.9f);
    }
    back_mesh = world_mesh_create(0, v, (int)(p - v) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);

    /* The lamp: a small sphere on top of the post */
    enum { AROUND = 12, UP = 8 };
    float lamp[AROUND * UP * 6 * VERTEX_FLOATS], *q = lamp;
    for (int i = 0; i < AROUND; i++)
        for (int j = 0; j < UP; j++) {
            float a[2] = { 2 * (float)M_PI * i / AROUND, 2 * (float)M_PI * (i + 1) / AROUND };
            float b[2] = { (float)M_PI * j / UP, (float)M_PI * (j + 1) / UP };
            static const int corners[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
            for (int k = 0; k < 6; k++) {
                float s = sinf(b[corners[k][1]]), nx = s * cosf(a[corners[k][0]]), nz = s * sinf(a[corners[k][0]]);
                float ny = cosf(b[corners[k][1]]);
                q = put(q, nx * LAMP_RADIUS, LAMP_Y + LAMP_RADIUS * (1 + ny), -0.03f + nz * LAMP_RADIUS, nx, ny, nz, 0, 0);
            }
        }
    lamp_mesh = world_mesh_create(0, lamp, (int)(q - lamp) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);
    return face_texture > 0 && back_texture > 0 && face_mesh > 0 && back_mesh > 0 && lamp_mesh > 0 ? 0 : -1;
}

static int part(int ext, const char *name, const char *suffix, int mesh, int texture, const float colour[4],
                const float model[16])
{
    char full[64];
    snprintf(full, sizeof(full), "%s-sign%s", name, suffix);
    int o = world_object_create(ext, full);
    if (o < 0 || world_object_set_mesh(ext, o, mesh) || world_object_set_shader(ext, o, world_shader_standard()) ||
        world_object_set_transform(ext, o, model) || (texture && world_object_set_texture(ext, o, 0, texture)) ||
        world_object_set_uniform(ext, o, "u_colour", colour, 4))
        return -1;
    return o;
}

int sign_place(int ext, const char *name)
{
    if (!face_mesh && make_sign() != 0)
        return -1;
    /* The wearer stands at the origin looking along -z, so the front right is the high x and z corner */
    float lo[3], hi[3], x = 0.9f, z = -2.5f;
    if (world_extension_bounds(ext, lo, hi) == 0) {
        x = hi[0] + 0.5f, z = hi[2] + 0.3f;
        if (z > -1.5f)   /* it reaches the wearer, or is behind: the sign goes in front of them */
            z = -2.5f;
        x = fminf(fmaxf(x, z * 0.6f), -z * 0.6f);   /* and where it can be seen */
    }
    float distance = hypotf(x, z), size = fmaxf(1, distance / 4), yaw = atan2f(-x, -z);
    float c = cosf(yaw) * size, s = sinf(yaw) * size;
    float model[16] = { c, 0, -s, 0, 0, size, 0, 0, s, 0, c, 0, x, 0, z, 1 };
    /* The face brighter than lighting alone would make it, as a reflective road sign looks */
    static const float white[4] = { 1.35f, 1.35f, 1.35f, 1 }, grey[4] = { 0.5f, 0.52f, 0.55f, 1 }, amber[4] = { 0.5f, 0.3f, 0.05f, 1 };
    int lamp;
    if (part(ext, name, "", face_mesh, face_texture, white, model) < 0 ||
        part(ext, name, "-back", back_mesh, back_texture, grey, model) < 0 ||
        (lamp = part(ext, name, "-lamp", lamp_mesh, 0, amber, model)) < 0)
        return -1;
    return world_object_set_blink(lamp, 1);
}
