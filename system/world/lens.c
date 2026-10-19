/*
 * The lenses: each eye's view is drawn undistorted into an eye buffer, then warped onto the panels so
 * that, seen through the lens, straight lines are straight and colours line up. The lens model is
 * Meta's for the Quest 1, as its own runtime has it (libvrapiimpl.so, device type 259; docs/hardware.md):
 * Oculus's Catmull-Rom model, in which a point d metres from the lens centre on the panel is seen at a
 * tangent angle t = (d / METERS_PER_TAN) * K(r^2), r = |d| / METERS_PER_TAN, with red and blue scaled a
 * little differently from green (chromatic aberration). The warp is a mesh over each panel whose
 * texture coordinates are those angles, per colour; it also turns the picture 180° for the panels.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "gles.h"
#include "world.h"

#define PANEL_W 1440
#define PANEL_H 1600
#define PANEL_METERS_X 0.0594f   /* the panel's width: 41.25 um a pixel */
#define METERS_PER_TAN 0.0389f   /* at the lens centre */
#define MAX_R 1.0f
static const float K[11] = { 1.0f, 1.0374f, 1.0810f, 1.1330f, 1.1970f, 1.2754f, 1.3771f, 1.5133f, 1.7018f, 1.9732f, 2.3f };
/* Red and blue against green: scale 1 + a + b r^2 */
static const float RED[2] = { -0.004f, -0.007f }, BLUE[2] = { 0.006f, 0.02f };
/* The lens centre's offset from the panel's centre (towards the nose, and down as the wearer sees it) */
#define LENS_IN_METERS 0.002986f
#define LENS_DOWN_METERS 0.00272f

/* How far each eye's buffer reaches, in degrees from straight ahead: Meta's field of view */
#define FOV_OUTER 52.0f
#define FOV_INNER 42.0f
#define FOV_UP 53.0f
#define FOV_DOWN 47.0f

#define MESH_COLUMNS 48
#define MESH_ROWS 54

/* The magnification at r^2 (Catmull-Rom through K, at r^2 = 0, 0.1 .. 1.0 of MAX_R^2) */
static float scale_at(float rsq)
{
    float x = 10 * rsq / (MAX_R * MAX_R);
    int k = (int)floorf(x);
    if (k < 0)
        k = 0;
    if (k > 9) {   /* beyond the last point: straight on */
        float slope = K[10] - K[9];
        return K[10] + slope * (x - 10);
    }
    float t = x - k;
#define AT(i) ((i) < 0 ? 2 * K[0] - K[1] : (i) > 10 ? 2 * K[10] - K[9] : K[i])
    float p0 = K[k], p1 = K[k + 1], m0 = (AT(k + 1) - AT(k - 1)) / 2, m1 = (AT(k + 2) - AT(k)) / 2;
#undef AT
    float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
}

/* Pixels a unit of tangent covers at the lens centre: how far the picture slides for a turn */
float lens_pixels_per_tan(void)
{
    return METERS_PER_TAN / (PANEL_METERS_X / PANEL_W);
}

/* The lens centre in panel pixels, as the wearer sees it (x across this eye's panel, y down) */
static void lens_centre(int eye, float *x, float *y)
{
    float pixel = PANEL_METERS_X / PANEL_W, in = LENS_IN_METERS / pixel;
    *x = PANEL_W / 2.0f + (eye == 0 ? in : -in);
    *y = PANEL_H / 2.0f + LENS_DOWN_METERS / pixel;
}

/* Each eye buffer's frustum, as tangents: left, right, bottom, top */
void lens_frustum(int eye, float tangents[4])
{
    float d = (float)M_PI / 180;
    float left = eye == 0 ? FOV_OUTER : FOV_INNER, right = eye == 0 ? FOV_INNER : FOV_OUTER;
    tangents[0] = -tanf(left * d), tangents[1] = tanf(right * d);
    tangents[2] = -tanf(FOV_DOWN * d), tangents[3] = tanf(FOV_UP * d);
}

static GLuint program, vao[2], vbo[2], ibo;
static GLint eye_range;
static int index_count;

int lens_init(void)
{
    const char *vs = "#version 300 es\n"
                     "in vec2 a_position;\n"
                     "in vec2 a_red;\n"
                     "in vec2 a_green;\n"
                     "in vec2 a_blue;\n"
                     "out vec2 red;\n"
                     "out vec2 green;\n"
                     "out vec2 blue;\n"
                     "void main() {\n"
                     "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
                     "    red = a_red; green = a_green; blue = a_blue;\n"
                     "}\n";
    const char *fs = "#version 300 es\n"
                     "precision mediump float;\n"
                     "uniform sampler2D u_eyes;\n"
                     "uniform vec2 u_range;\n"   /* this eye's half of the eye buffers, in u */
                     "in vec2 red;\n"
                     "in vec2 green;\n"
                     "in vec2 blue;\n"
                     "out vec4 colour;\n"
                     "float inside(vec2 uv) {\n"
                     "    return step(u_range.x, uv.x) * step(uv.x, u_range.y) * step(0.0, uv.y) * step(uv.y, 1.0);\n"
                     "}\n"
                     "void main() {\n"
                     "    colour = vec4(texture(u_eyes, red).r * inside(red), texture(u_eyes, green).g * inside(green),\n"
                     "                  texture(u_eyes, blue).b * inside(blue), 1.0);\n"
                     "}\n";
    int id = world_shader_create(0, vs, fs);
    if (id < 0) {
        wlog("the lens shader: %s", world_error());
        return -1;
    }
    program = world_shader_program(id);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_eyes"), 0);
    eye_range = glGetUniformLocation(program, "u_range");
    glUseProgram(0);

    /* The same grid for both eyes, as indices; the vertices differ */
    int n = (MESH_COLUMNS + 1) * (MESH_ROWS + 1);
    index_count = MESH_COLUMNS * MESH_ROWS * 6;
    unsigned *indices = malloc(index_count * sizeof(unsigned));
    float *v = malloc((size_t)n * 8 * sizeof(float));
    if (!indices || !v) {
        free(indices);
        free(v);
        return -1;
    }
    for (int r = 0, k = 0; r < MESH_ROWS; r++)
        for (int c = 0; c < MESH_COLUMNS; c++) {
            unsigned a = r * (MESH_COLUMNS + 1) + c, b = a + 1, d = a + MESH_COLUMNS + 1, e = d + 1;
            indices[k++] = a, indices[k++] = b, indices[k++] = e, indices[k++] = a, indices[k++] = e, indices[k++] = d;
        }
    glGenBuffers(1, &ibo);
    glGenVertexArrays(2, vao);
    glGenBuffers(2, vbo);
    float pixel = PANEL_METERS_X / PANEL_W;
    for (int eye = 0; eye < 2; eye++) {
        float cx, cy, f[4];
        lens_centre(eye, &cx, &cy);
        lens_frustum(eye, f);
        for (int r = 0; r <= MESH_ROWS; r++)
            for (int c = 0; c <= MESH_COLUMNS; c++) {
                float px = PANEL_W * (float)c / MESH_COLUMNS, py = PANEL_H * (float)r / MESH_ROWS;
                /* Where this point of the panel is seen: tangents, x right and y up */
                float tx = (px - cx) * pixel / METERS_PER_TAN, ty = -(py - cy) * pixel / METERS_PER_TAN;
                float rsq = tx * tx + ty * ty, s = scale_at(rsq);
                float colour[3] = { s * (1 + RED[0] + RED[1] * rsq), s, s * (1 + BLUE[0] + BLUE[1] * rsq) };
                float *out = &v[(r * (MESH_COLUMNS + 1) + c) * 8];
                /* On the framebuffer: the wearer's view turned 180° (their x is mirrored, OpenGL's rows
                 * already run bottom-up), this eye's panel being their left or right half */
                float wx = eye * PANEL_W + px;
                out[0] = 2 * (2 * PANEL_W - wx) / (2 * PANEL_W) - 1;
                out[1] = 2 * (PANEL_H - py) / PANEL_H - 1;
                for (int k = 0; k < 3; k++) {
                    float u = (tx * colour[k] - f[0]) / (f[1] - f[0]), w = (ty * colour[k] - f[2]) / (f[3] - f[2]);
                    out[2 + 2 * k] = (eye + u) / 2;   /* the eye buffers sit side by side */
                    out[3 + 2 * k] = w;
                }
            }
        glBindVertexArray(vao[eye]);
        glBindBuffer(GL_ARRAY_BUFFER, vbo[eye]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * 8 * sizeof(float), v, GL_STATIC_DRAW);
        static const char *const names[4] = { "a_position", "a_red", "a_green", "a_blue" };
        for (int a = 0; a < 4; a++) {
            GLint at = glGetAttribLocation(program, names[a]);
            glVertexAttribPointer(at, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(uintptr_t)(a * 2 * sizeof(float)));
            glEnableVertexAttribArray(at);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        if (eye == 0)
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)index_count * sizeof(unsigned), indices, GL_STATIC_DRAW);
    }
    glBindVertexArray(0);
    free(indices);
    free(v);
    return 0;
}

/* Warp the eye buffers (one texture, the eyes side by side) onto the bound framebuffer */
void lens_draw(GLuint eye_buffers)
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eye_buffers);
    for (int eye = 0; eye < 2; eye++) {
        glUniform2f(eye_range, eye * 0.5f, eye * 0.5f + 0.5f);
        glBindVertexArray(vao[eye]);
        glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}
