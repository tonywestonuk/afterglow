/*
 * The "Claude at work" sign beside a placeholder: something quick an extension put up while the real
 * thing is still being written. A road-works triangle over a plate saying CLAUDE AT WORK, with an
 * orange asterisk spinning on top like a "working on it" spinner. It stands on the floor at the front
 * right of everything the placeholder made, turned to the wearer; it belongs to the placeholder, so it
 * goes when the real version replaces it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

/* Made once and shared by every sign: the painted face and its outline (for the back), and three meshes */
static int face_texture, back_texture, face_mesh, back_mesh, star_mesh;

#define POST 0.025f       /* half the post's width */
#define POST_TOP 1.9f     /* just above the triangle's top, behind it */
#define STAR_Y 2.05f      /* the asterisk's centre */
#define STAR_RADIUS 0.13f
#define STAR_RAYS 11

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
        float corners[4][3] = { { d[0], 0, d[1] }, { d[2], 0, d[3] }, { d[2], POST_TOP, d[3] }, { d[0], POST_TOP, d[1] } };
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++)
            p = put(p, corners[order[k]][0], corners[order[k]][1], corners[order[k]][2], d[4], 0, d[5], 0.5f, 0.9f);
    }
    back_mesh = world_mesh_create(0, v, (int)(p - v) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);

    /* The asterisk: tapered rays of uneven length at slightly uneven angles, drawn round its own centre
     * (the world spins it there), each a narrow kite with a rounded-off tip */
    float star[STAR_RAYS * 4 * 3 * VERTEX_FLOATS], *q = star;
    static const float length[STAR_RAYS] = { 1.0f, 0.86f, 0.95f, 0.82f, 0.99f, 0.88f, 0.93f, 0.84f, 0.97f, 0.9f, 0.85f };
    static const float wobble[STAR_RAYS] = { 0, 0.04f, -0.03f, 0.02f, -0.05f, 0.03f, 0, -0.02f, 0.04f, -0.03f, 0.02f };
    for (int i = 0; i < STAR_RAYS; i++) {
        float a = 2 * (float)M_PI * (i + wobble[i]) / STAR_RAYS, c = cosf(a), s = sinf(a);
        float r = STAR_RADIUS * length[i], base = 0.022f, tip = 0.012f;
        /* in the ray's own frame: along it x, across y */
        float pts[4][2] = { { 0, base }, { r - tip, tip }, { r, 0 }, { r - tip, -tip } };
        float w[4][2];
        for (int k = 0; k < 4; k++)
            w[k][0] = pts[k][0] * c - pts[k][1] * s, w[k][1] = pts[k][0] * s + pts[k][1] * c;
        float centre[2] = { 0, 0 }, other[2] = { base * s, -base * c };   /* the near corner on the far side */
        float fan[4][3][2] = { { { centre[0], centre[1] }, { w[0][0], w[0][1] }, { w[1][0], w[1][1] } },
                               { { centre[0], centre[1] }, { w[1][0], w[1][1] }, { w[2][0], w[2][1] } },
                               { { centre[0], centre[1] }, { w[2][0], w[2][1] }, { w[3][0], w[3][1] } },
                               { { centre[0], centre[1] }, { w[3][0], w[3][1] }, { other[0], other[1] } } };
        for (int t = 0; t < 4; t++)
            for (int k = 0; k < 3; k++)
                q = put(q, fan[t][k][0], fan[t][k][1], 0, 0, 0, 1, 0.5f, 0.9f);
    }
    star_mesh = world_mesh_create(0, star, (int)(q - star) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);
    return face_texture > 0 && back_texture > 0 && face_mesh > 0 && back_mesh > 0 && star_mesh > 0 ? 0 : -1;
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
    static const float white[4] = { 1.35f, 1.35f, 1.35f, 1 }, grey[4] = { 0.5f, 0.52f, 0.55f, 1 };
    static const float terracotta[4] = { 0.85f, 0.47f, 0.34f, 1 }, glow[3] = { 0.3f, 0.12f, 0.07f };
    /* The asterisk sits above the triangle, a little in front of the post */
    float lift[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, STAR_Y, 0.01f, 1 }, star_model[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            star_model[col * 4 + row] = 0;
            for (int k = 0; k < 4; k++)
                star_model[col * 4 + row] += model[k * 4 + row] * lift[col * 4 + k];
        }
    int star;
    if (part(ext, name, "", face_mesh, face_texture, white, model) < 0 ||
        part(ext, name, "-back", back_mesh, back_texture, grey, model) < 0 ||
        (star = part(ext, name, "-claude", star_mesh, 0, terracotta, star_model)) < 0)
        return -1;
    world_object_set_uniform(ext, star, "u_emission", glow, 3);
    return world_object_set_spin(star, -1.2f);   /* clockwise as the wearer sees it, a turn every 5 s */
}

/*
 * What Claude is doing: afterglowd passes on the status messages Claude writes between steps
 * (tools/claude_status_hook.py, from Claude Code's hooks), and they're shown on a dark panel two metres
 * in front of where the wearer was looking, a little below eye level. It stays put while updates come,
 * moves in front again if the wearer turns well away, and goes after a while with no news.
 */
#define STATUS_DISTANCE 2.0f
#define STATUS_DROP 0.45f       /* below eye level */
#define STATUS_LINE 0.07f       /* metres a line */
#define STATUS_COLUMNS 42
#define STATUS_SHOWN_FOR 20.0   /* seconds without news before it goes */

static struct {
    char text[400];
    int changed, object, mesh, texture;
    double updated;
    float at[3], yaw;           /* where the panel stands, and which way it faces */
    int placed;
} status;

void status_set(const char *text)
{
    /* Up to three lines of about STATUS_COLUMNS, broken between words, "…" if it runs over */
    char out[sizeof(status.text)];
    size_t n = 0, line_start = 0;
    int lines = 1;
    for (const char *p = text; *p && *p != '\n' && n < sizeof(out) - 4; p++) {
        out[n++] = *p;
        if (n - line_start > STATUS_COLUMNS) {
            size_t space = n;
            while (space > line_start && out[space - 1] != ' ')
                space--;
            if (lines == 3) {
                n = space > line_start ? space - 1 : n - 1;
                memcpy(out + n, "\xe2\x80\xa6", 3);   /* … */
                n += 3;
                break;
            }
            if (space > line_start)
                out[space - 1] = '\n', line_start = space;
            else
                out[n++] = '\n', line_start = n;
            lines++;
        }
    }
    out[n] = '\0';
    if (strcmp(out, status.text) != 0) {
        snprintf(status.text, sizeof(status.text), "%s", out);
        status.changed = 1;
    }
    status.updated = world_time();
}

/* The panel's picture: the text in white over a dark rounded panel, with a margin */
static int status_texture(float *aspect)
{
    int tw, th, pad = 28;
    uint8_t *words = paint_text(status.text[0] ? status.text : " ", 64, 0xffffff, 64 * 0.012f, &tw, &th);
    if (!words)
        return -1;
    int w = tw + 2 * pad, h = th + 2 * pad;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) {
        free(words);
        return -1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            /* rounded rectangle, corners of radius pad */
            float qx = fabsf(x + 0.5f - w / 2.0f) - (w / 2.0f - pad), qy = fabsf(y + 0.5f - h / 2.0f) - (h / 2.0f - pad);
            float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) - pad;
            float panel = fminf(fmaxf(0.5f - d, 0), 1) * 0.78f;
            int tx = x - pad, ty = y - pad;
            float ink = tx >= 0 && tx < tw && ty >= 0 && ty < th ? words[(ty * tw + tx) * 4 + 3] / 255.0f : 0;
            uint8_t *p = &rgba[(y * w + x) * 4];
            float a = ink + panel * (1 - ink);   /* text over the panel */
            for (int k = 0; k < 3; k++) {
                float bg = k == 0 ? 18 : k == 1 ? 18 : 24;
                p[k] = (uint8_t)(a > 0 ? (255 * ink + bg * panel * (1 - ink)) / a : 0);
            }
            p[3] = (uint8_t)(a * 255);
        }
    free(words);
    *aspect = (float)w / h;
    int id = world_texture_create(0, rgba, w, h);
    free(rgba);
    return id;
}

void status_frame(double now, const float r[9], const float centre[3])
{
    if (!status.object) {
        static const float white[4] = { 1, 1, 1, 1 };
        status.object = world_object_create(0, "claude-status");
        if (status.object < 0 || world_object_set_shader(0, status.object, world_shader_standard()) ||
            world_object_set_blend(0, status.object, BLEND_ALPHA) ||
            world_object_set_uniform(0, status.object, "u_colour", white, 4)) {
            status.object = -1;
            return;
        }
        static const float bright[3] = { 0.35f, 0.35f, 0.35f };   /* readable whatever the lighting */
        world_object_set_uniform(0, status.object, "u_emission", bright, 3);
    }
    if (status.object < 0)
        return;
    int showing = status.updated > 0 && now - status.updated < STATUS_SHOWN_FOR;
    world_object_set_visible(0, status.object, showing);
    if (!showing) {
        status.placed = 0;
        return;
    }
    /* Which way the wearer faces, level */
    float fx = -r[6], fz = -r[8], fn = hypotf(fx, fz);
    if (fn < 1e-3f)
        fx = 0, fz = -1, fn = 1;
    fx /= fn, fz /= fn;
    float facing = atan2f(-fx, -fz);
    float off = facing - status.yaw;
    off = atan2f(sinf(off), cosf(off));
    if (!status.placed || fabsf(off) > 1.0f) {   /* first shown, or the wearer turned more than ~60° away */
        status.at[0] = centre[0] + fx * STATUS_DISTANCE;
        status.at[1] = centre[1] - STATUS_DROP;
        status.at[2] = centre[2] + fz * STATUS_DISTANCE;
        status.yaw = facing;
        status.placed = 1;
        status.changed = 1;   /* (re)build where it is */
    }
    if (status.changed) {
        float aspect = 1;
        int texture = status_texture(&aspect);
        if (texture > 0) {
            int lines = 1;
            for (const char *p = status.text; *p; p++)
                lines += *p == '\n';
            float h = STATUS_LINE * (lines + 0.8f), w = h * aspect;
            float v[6 * VERTEX_FLOATS], *q = v;
            float corners[4][4] = { { -w / 2, -h / 2, 0, 1 }, { w / 2, -h / 2, 1, 1 }, { w / 2, h / 2, 1, 0 }, { -w / 2, h / 2, 0, 0 } };
            static const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; i++)
                q = put(q, corners[order[i]][0], corners[order[i]][1], 0, 0, 0, 1, corners[order[i]][2], corners[order[i]][3]);
            int mesh = world_mesh_create(0, v, 6, NULL, 0, PRIMITIVE_TRIANGLES);
            if (mesh > 0) {
                world_object_set_mesh(0, status.object, mesh);
                world_object_set_texture(0, status.object, 0, texture);
                world_mesh_delete(status.mesh);
                world_texture_delete(status.texture);
                status.mesh = mesh, status.texture = texture;
            } else
                world_texture_delete(texture);
        }
        float c = cosf(status.yaw), s = sinf(status.yaw);
        float model[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, status.at[0], status.at[1], status.at[2], 1 };
        world_object_set_transform(0, status.object, model);
        status.changed = 0;
    }
}
