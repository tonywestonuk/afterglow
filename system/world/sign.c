/*
 * The "Claude at work" sign: a road-works triangle over a plate saying CLAUDE AT WORK, an orange
 * asterisk spinning on top like a "working on it" spinner, and under them a message board showing what
 * Claude is doing (afterglowd passes on its status: tools/claude_status_hook.py).
 *
 * A placeholder (something quick an extension put up while the real thing is still being written)
 * gets one standing on the floor at the front right of everything it made, turned to the wearer; it
 * belongs to the placeholder, so it goes when the real version replaces it. While Claude is working
 * and there's no placeholder, the world stands one of its own a little ahead and to the right of where
 * the wearer looks, and takes it away once Claude is done.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

/* Made once and shared by every sign: the painted face and its outline (for the back), the message
 * board's picture (repainted in place when the status changes, so every sign shows it), and the meshes */
static int face_texture, back_texture, board_texture, face_mesh, back_mesh, star_mesh, board_mesh;

#define POST 0.025f       /* half the post's width */
#define POST_TOP 1.9f     /* just above the triangle's top, behind it */
#define STAR_Y 2.05f      /* the asterisk's centre */
#define STAR_RADIUS 0.13f
#define STAR_RAYS 11
#define BOARD_W 1.2f      /* the message board, under the plate */
#define BOARD_H 0.3f
#define BOARD_TOP 0.94f
#define BOARD_PIXELS_W 1024
#define BOARD_PIXELS_H 256

static char status_text[400] = "Claude at work";
static void paint_board(void);

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
    float v[72 * VERTEX_FLOATS], *p = v;
    p = panel(p, -0.4f, 0.4f, square, square + 0.8f, 0, 0, split, 0);
    p = panel(p, -0.375f, 0.375f, plate, plate + 0.1875f, 0, split, 1, 0);
    face_mesh = world_mesh_create(0, v, (int)(p - v) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);

    p = v;
    p = panel(p, -BOARD_W / 2, BOARD_W / 2, BOARD_TOP - BOARD_H, BOARD_TOP, 0, 0, 1, 0);
    board_mesh = world_mesh_create(0, v, (int)(p - v) / VERTEX_FLOATS, NULL, 0, PRIMITIVE_TRIANGLES);
    uint8_t *blank = calloc(BOARD_PIXELS_W * BOARD_PIXELS_H, 4);
    board_texture = blank ? world_texture_create(0, blank, BOARD_PIXELS_W, BOARD_PIXELS_H) : -1;
    free(blank);
    paint_board();

    p = v;
    p = panel(p, -0.4f, 0.4f, square, square + 0.8f, -0.004f, 0, split, 1);
    p = panel(p, -0.375f, 0.375f, plate, plate + 0.1875f, -0.004f, split, 1, 1);
    p = panel(p, -BOARD_W / 2, BOARD_W / 2, BOARD_TOP - BOARD_H, BOARD_TOP, -0.004f, split, 1, 1);   /* (opaque) */
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
    return face_texture > 0 && back_texture > 0 && board_texture > 0 && face_mesh > 0 && back_mesh > 0 &&
           star_mesh > 0 && board_mesh > 0 ? 0 : -1;
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

/* A whole sign, standing at `model` (on the floor, facing +z), its parts named NAME-sign... */
static int sign_build(int ext, const char *name, const float model[16], int parts[4])
{
    if (!face_mesh && make_sign() != 0)
        return -1;
    /* The face brighter than lighting alone would make it, as a reflective road sign looks; the board
     * lit from inside */
    static const float white[4] = { 1.35f, 1.35f, 1.35f, 1 }, grey[4] = { 0.5f, 0.52f, 0.55f, 1 };
    static const float terracotta[4] = { 0.85f, 0.47f, 0.34f, 1 }, glow[3] = { 0.3f, 0.12f, 0.07f };
    static const float lit[4] = { 2.2f, 2.2f, 2.2f, 1 };
    /* The asterisk sits above the triangle, a little in front of the post */
    float lift[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, STAR_Y, 0.01f, 1 }, star_model[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            star_model[col * 4 + row] = 0;
            for (int k = 0; k < 4; k++)
                star_model[col * 4 + row] += model[k * 4 + row] * lift[col * 4 + k];
        }
    if ((parts[0] = part(ext, name, "", face_mesh, face_texture, white, model)) < 0 ||
        (parts[1] = part(ext, name, "-back", back_mesh, back_texture, grey, model)) < 0 ||
        (parts[2] = part(ext, name, "-claude", star_mesh, 0, terracotta, star_model)) < 0 ||
        (parts[3] = part(ext, name, "-board", board_mesh, board_texture, lit, model)) < 0)
        return -1;
    world_object_set_uniform(ext, parts[2], "u_emission", glow, 3);
    return world_object_set_spin(parts[2], -1.2f);   /* clockwise as the wearer sees it, a turn every 5 s */
}

/* Standing at (x, z) on the floor, turned to face (towards_x, towards_z), sized up with distance */
static void stand(float model[16], float x, float z, float towards_x, float towards_z)
{
    float distance = hypotf(x - towards_x, z - towards_z), size = fmaxf(1, distance / 4);
    float yaw = atan2f(towards_x - x, towards_z - z), c = cosf(yaw) * size, s = sinf(yaw) * size;
    float m[16] = { c, 0, -s, 0, 0, size, 0, 0, s, 0, c, 0, x, 0, z, 1 };
    memcpy(model, m, sizeof(m));
}

/* The placeholders' signs, by name, to know whether any are standing */
#define MAX_SIGNS 32
static char placed[MAX_SIGNS][64];

int sign_place(int ext, const char *name)
{
    /* The wearer stands at the origin looking along -z, so the front right is the high x and z corner */
    float lo[3], hi[3], x = 0.9f, z = -2.5f, model[16];
    if (world_extension_bounds(ext, lo, hi) == 0) {
        x = hi[0] + 0.5f, z = hi[2] + 0.3f;
        if (z > -1.5f)   /* it reaches the wearer, or is behind: the sign goes in front of them */
            z = -2.5f;
        x = fminf(fmaxf(x, z * 0.6f), -z * 0.6f);   /* and where it can be seen */
    }
    stand(model, x, z, 0, 0);
    int parts[4];
    if (sign_build(ext, name, model, parts) != 0)
        return -1;
    for (int i = 0; i < MAX_SIGNS; i++)
        if (!placed[i][0] || world_object_find(placed[i]) < 0) {
            snprintf(placed[i], sizeof(placed[i]), "%s-sign", name);
            break;
        }
    return 0;
}

/* ---- What Claude is doing ---- */

#define STATUS_COLUMNS 34
#define BUSY_FOR 20.0     /* seconds after the last news that Claude counts as working */
#define DONE_FOR 6.0      /* and after "Done" */

static double status_updated;
static int status_changed, status_done;
static int own[4] = { -1, -1, -1, -1 };   /* the world's own sign, while Claude works without a placeholder */
static float own_yaw;                     /* the way the wearer faced when it was put up */

void status_set(const char *text)
{
    /* Up to three lines of about STATUS_COLUMNS, broken between words, "…" if it runs over */
    char out[sizeof(status_text)];
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
    if (strcmp(out, status_text) != 0) {
        snprintf(status_text, sizeof(status_text), "%s", out);
        status_changed = 1;
    }
    status_done = strncmp(text, "Done", 4) == 0;
    status_updated = world_time();
}

/* The board's picture: amber text, as large as fits, on black in a thin frame */
static void paint_board(void)
{
    static uint8_t board[BOARD_PIXELS_W * BOARD_PIXELS_H * 4];
    int tw = 0, th = 0;
    uint8_t *words = NULL;
    for (int px = 72; px >= 24; px -= 4) {
        free(words);
        words = paint_text(status_text, px, 0xffb02a, px * 0.03f, &tw, &th);
        if (!words || (tw <= BOARD_PIXELS_W - 48 && th <= BOARD_PIXELS_H - 40))
            break;
    }
    for (int y = 0; y < BOARD_PIXELS_H; y++)
        for (int x = 0; x < BOARD_PIXELS_W; x++) {
            uint8_t *p = &board[(y * BOARD_PIXELS_W + x) * 4];
            int frame = x < 6 || y < 6 || x >= BOARD_PIXELS_W - 6 || y >= BOARD_PIXELS_H - 6;
            int tx = x - (BOARD_PIXELS_W - tw) / 2, ty = y - (BOARD_PIXELS_H - th) / 2;
            float ink = words && tx >= 0 && tx < tw && ty >= 0 && ty < th ? words[(ty * tw + tx) * 4 + 3] / 255.0f : 0;
            uint8_t base = frame ? 70 : 8;
            p[0] = (uint8_t)(base + (255 - base) * ink);
            p[1] = (uint8_t)(base + (176 - base) * ink);
            p[2] = (uint8_t)(base + (42 - base) * ink);
            p[3] = 255;
        }
    free(words);
    world_texture_update(board_texture, board, BOARD_PIXELS_W, BOARD_PIXELS_H);
}

void status_frame(double now, const float r[9], const float centre[3])
{
    if (status_changed && board_texture > 0) {
        paint_board();
        status_changed = 0;
    }
    int working = status_updated > 0 && now - status_updated < (status_done ? DONE_FOR : BUSY_FOR);
    int placeholders = 0;
    for (int i = 0; i < MAX_SIGNS; i++)
        if (placed[i][0]) {
            if (world_object_find(placed[i]) > 0)
                placeholders = 1;
            else
                placed[i][0] = '\0';
        }
    int wanted = working && !placeholders;
    if (!wanted) {
        for (int k = 0; k < 4; k++)
            if (own[k] > 0)
                world_object_delete(own[k]), own[k] = -1;
        return;
    }
    /* Which way the wearer faces, level */
    float fx = -r[6], fz = -r[8], fn = hypotf(fx, fz);
    if (fn < 1e-3f)
        fx = 0, fz = -1, fn = 1;
    fx /= fn, fz /= fn;
    float facing = atan2f(-fx, -fz), off = facing - own_yaw;
    off = atan2f(sinf(off), cosf(off));
    if (own[0] > 0 && fabsf(off) < 1.2f)
        return;   /* standing, and still in view */
    /* Put up (or moved to) a little ahead and to the right of where the wearer looks */
    float x = centre[0] + fx * 2.4f - fz * 0.9f, z = centre[2] + fz * 2.4f + fx * 0.9f, model[16];
    stand(model, x, z, centre[0], centre[2]);
    own_yaw = facing;
    if (own[0] > 0)
        for (int k = 0; k < 4; k++) {
            if (k == 2) {   /* the asterisk sits higher */
                float lifted[16];
                memcpy(lifted, model, sizeof(lifted));
                for (int row = 0; row < 3; row++)
                    lifted[12 + row] += model[4 + row] * STAR_Y + model[8 + row] * 0.01f;
                world_object_set_transform(0, own[k], lifted);
            } else
                world_object_set_transform(0, own[k], model);
        }
    else if (sign_build(0, "claude", model, own) != 0)
        for (int k = 0; k < 4; k++)
            if (own[k] > 0)
                world_object_delete(own[k]), own[k] = -1;
}
