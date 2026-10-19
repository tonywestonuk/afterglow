/*
 * Extensions: WebAssembly modules run by WAMR (its fast interpreter for now). Each may export on_load(),
 * on_frame(float dt) and on_unload(), and imports the world's API from the module "afterglow"
 * (sdk/afterglow.h). A trap, or running past its instruction budget, unloads that extension and puts
 * back everything it did; the world carries on. Loading a new version replaces the old one, and if the
 * new one fails, the old one is loaded again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"
#include "world.h"

#define MAX_EXTENSIONS 32
#define LOAD_BUDGET 2000000000   /* instructions for on_load: several seconds in the interpreter */
#define FRAME_BUDGET 100000000   /* and for each on_frame */
#define STACK_SIZE (256 * 1024)

struct extension {
    int used, failed, id;   /* id: whose things are whose in the world; never reused */
    int placeholder;        /* stands in for something still being made: it gets a road-works sign */
    char name[64], path[512], problem[1024];
    uint8_t *bytes;
    wasm_module_t module;
    wasm_module_inst_t inst;
    wasm_exec_env_t env;
    wasm_function_inst_t on_frame;
};

static struct extension extensions[MAX_EXTENSIONS];
static int next_id = 1;
static struct extension *current;   /* whose code is running */

/* ---- The world's API, as extensions see it ---- */

/* A world call that failed: the extension gets -1, and the reason is kept for whoever loaded it */
static int noted(int result)
{
    if (result < 0 && current) {
        snprintf(current->problem, sizeof(current->problem), "%s", world_error());
        size_t n = strlen(current->problem);   /* shader compilers end with blank lines */
        while (n > 0 && (current->problem[n - 1] == '\n' || current->problem[n - 1] == ' '))
            current->problem[--n] = '\0';
    }
    return result;
}

/* An array in the extension's memory, checked to lie wholly inside it (if not, the call traps) */
static void *array(wasm_exec_env_t env, uint32_t offset, uint64_t size)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    return wasm_runtime_validate_app_addr(inst, offset, size) ? wasm_runtime_addr_app_to_native(inst, offset) : NULL;
}

static int32_t x_mesh_create(wasm_exec_env_t env, uint32_t vertices, int32_t vertex_count, uint32_t indices,
                             int32_t index_count, int32_t primitive)
{
    if (vertex_count <= 0 || vertex_count > 4000000 || index_count < 0 || index_count > 12000000) {
        snprintf(current->problem, sizeof(current->problem), "mesh_create: %d vertices and %d indices: too many or too few",
                 vertex_count, index_count);
        return -1;
    }
    const float *v = array(env, vertices, (uint64_t)vertex_count * VERTEX_FLOATS * sizeof(float));
    const uint32_t *ix = index_count ? array(env, indices, (uint64_t)index_count * sizeof(uint32_t)) : NULL;
    if (!v || (index_count && !ix))
        return -1;
    return noted(world_mesh_create(current->id, v, vertex_count, ix, index_count, primitive));
}

static int32_t x_shader_create(wasm_exec_env_t env, const char *vertex_glsl, const char *fragment_glsl)
{
    (void)env;
    return noted(world_shader_create(current->id, vertex_glsl, fragment_glsl));
}

static int32_t x_object_create(wasm_exec_env_t env, const char *name)
{
    (void)env;
    return noted(world_object_create(current->id, name));
}

static int32_t x_object_find(wasm_exec_env_t env, const char *name)
{
    (void)env;
    return noted(world_object_find(name));
}

static int32_t x_object_set_mesh(wasm_exec_env_t env, int32_t object, int32_t mesh)
{
    (void)env;
    return noted(world_object_set_mesh(current->id, object, mesh));
}

static int32_t x_object_set_shader(wasm_exec_env_t env, int32_t object, int32_t shader)
{
    (void)env;
    return noted(world_object_set_shader(current->id, object, shader));
}

static int32_t x_object_set_transform(wasm_exec_env_t env, int32_t object, uint32_t matrix)
{
    const float *m = array(env, matrix, 16 * sizeof(float));
    return m ? noted(world_object_set_transform(current->id, object, m)) : -1;
}

static int32_t x_object_set_visible(wasm_exec_env_t env, int32_t object, int32_t visible)
{
    (void)env;
    return noted(world_object_set_visible(current->id, object, visible));
}

static int32_t x_object_set_uniform(wasm_exec_env_t env, int32_t object, const char *name, uint32_t values, int32_t count)
{
    if (count < 1 || count > MAX_UNIFORM_FLOATS) {
        snprintf(current->problem, sizeof(current->problem), "object_set_uniform: 1 to %d values", MAX_UNIFORM_FLOATS);
        return -1;
    }
    const float *v = array(env, values, (uint64_t)count * sizeof(float));
    return v ? noted(world_object_set_uniform(current->id, object, name, v, count)) : -1;
}

static int32_t x_shader_standard(wasm_exec_env_t env)
{
    (void)env;
    return world_shader_standard();
}

static int32_t x_texture_create(wasm_exec_env_t env, uint32_t rgba, int32_t width, int32_t height)
{
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        snprintf(current->problem, sizeof(current->problem), "texture_create: 1 to 4096 pixels each way, not %dx%d",
                 width, height);
        return -1;
    }
    const uint8_t *pixels = array(env, rgba, (uint64_t)width * height * 4);
    return pixels ? noted(world_texture_create(current->id, pixels, width, height)) : -1;
}

static int32_t x_texture_load(wasm_exec_env_t env, const char *name)
{
    (void)env;
    return noted(world_texture_load(current->id, name));
}

static int32_t x_texture_text(wasm_exec_env_t env, const char *text, int32_t pixel_height, int32_t ink)
{
    (void)env;
    return noted(world_texture_text(current->id, text, pixel_height, (uint32_t)ink));
}

static int32_t x_texture_size(wasm_exec_env_t env, int32_t texture, uint32_t width, uint32_t height)
{
    int32_t *w = array(env, width, 4), *h = array(env, height, 4);
    int tw, th;
    if (!w || !h || noted(world_texture_size(texture, &tw, &th)) != 0)
        return -1;
    *w = tw, *h = th;
    return 0;
}

static int32_t x_object_set_texture(wasm_exec_env_t env, int32_t object, int32_t slot, int32_t texture)
{
    (void)env;
    return noted(world_object_set_texture(current->id, object, slot, texture));
}

static int32_t x_object_set_blend(wasm_exec_env_t env, int32_t object, int32_t mode)
{
    (void)env;
    return noted(world_object_set_blend(current->id, object, mode));
}

static int32_t x_head_pose(wasm_exec_env_t env, uint32_t position, uint32_t orientation)
{
    float *p = array(env, position, 3 * sizeof(float)), *q = array(env, orientation, 4 * sizeof(float));
    float r[9];
    if (!p || !q)
        return -1;
    int tracked = head_pose(0, r, p);
    head_quaternion(0, q);
    return tracked;
}

static int32_t x_remember(wasm_exec_env_t env, const char *key, const uint8_t *data, uint32_t size)
{
    (void)env;
    return noted(world_remember(key, data, (int)size));
}

static int32_t x_recall(wasm_exec_env_t env, const char *key, uint8_t *data, uint32_t size)
{
    (void)env;
    return world_recall(key, data, (int)size);
}

static int32_t x_object_get_transform(wasm_exec_env_t env, int32_t object, uint32_t matrix)
{
    float *m = array(env, matrix, 16 * sizeof(float));
    return m ? noted(world_object_get_transform(object, m)) : -1;
}

static int32_t x_set_background(wasm_exec_env_t env, float r, float g, float b)
{
    (void)env;
    return noted(world_set_background(current->id, r, g, b));
}

static double x_world_seconds(wasm_exec_env_t env)
{
    (void)env;
    return world_time();
}

static void x_log_message(wasm_exec_env_t env, const char *text)
{
    (void)env;
    wlog("%s: %s", current->name, text);
}

static int32_t x_last_error(wasm_exec_env_t env, char *buffer, uint32_t size)
{
    (void)env;
    if (size == 0)
        return 0;
    snprintf(buffer, size, "%s", current->problem);
    return (int32_t)strlen(buffer);
}

int ext_init(void)
{
    static NativeSymbol natives[] = {
        { "mesh_create", (void *)x_mesh_create, "(iiiii)i", NULL },
        { "shader_create", (void *)x_shader_create, "($$)i", NULL },
        { "object_create", (void *)x_object_create, "($)i", NULL },
        { "object_find", (void *)x_object_find, "($)i", NULL },
        { "object_set_mesh", (void *)x_object_set_mesh, "(ii)i", NULL },
        { "object_set_shader", (void *)x_object_set_shader, "(ii)i", NULL },
        { "object_set_transform", (void *)x_object_set_transform, "(ii)i", NULL },
        { "object_set_visible", (void *)x_object_set_visible, "(ii)i", NULL },
        { "object_set_uniform", (void *)x_object_set_uniform, "(i$ii)i", NULL },
        { "shader_standard", (void *)x_shader_standard, "()i", NULL },
        { "texture_create", (void *)x_texture_create, "(iii)i", NULL },
        { "texture_load", (void *)x_texture_load, "($)i", NULL },
        { "texture_text", (void *)x_texture_text, "($ii)i", NULL },
        { "texture_size", (void *)x_texture_size, "(iii)i", NULL },
        { "object_set_texture", (void *)x_object_set_texture, "(iii)i", NULL },
        { "object_set_blend", (void *)x_object_set_blend, "(ii)i", NULL },
        { "head_pose", (void *)x_head_pose, "(ii)i", NULL },
        { "remember", (void *)x_remember, "($*~)i", NULL },
        { "recall", (void *)x_recall, "($*~)i", NULL },
        { "object_get_transform", (void *)x_object_get_transform, "(ii)i", NULL },
        { "set_background", (void *)x_set_background, "(fff)i", NULL },
        { "world_seconds", (void *)x_world_seconds, "()F", NULL },
        { "log_message", (void *)x_log_message, "($)", NULL },
        { "last_error", (void *)x_last_error, "(*~)i", NULL },
    };
    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_System_Allocator;
    args.native_module_name = "afterglow";
    args.native_symbols = natives;
    args.n_native_symbols = sizeof(natives) / sizeof(natives[0]);
    if (!wasm_runtime_full_init(&args)) {
        wlog("the WebAssembly runtime didn't start");
        return -1;
    }
    return 0;
}

/* ---- Running extensions ---- */

/* Call one of an extension's functions within an instruction budget. 0, or -1 with the reason kept. */
static int call(struct extension *e, const char *function, uint32_t argc, uint32_t *argv, int budget)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(e->inst, function);
    if (!f)
        return 0;   /* every export is optional */
    current = e;
    wasm_runtime_set_instruction_count_limit(e->env, budget);
    int ok = wasm_runtime_call_wasm(e->env, f, argc, argv);
    current = NULL;
    if (!ok) {
        const char *exception = wasm_runtime_get_exception(e->inst);
        snprintf(e->problem, sizeof(e->problem), "%s trapped: %s", function, exception ? exception : "unknown");
        wasm_runtime_clear_exception(e->inst);
        return -1;
    }
    return 0;
}

static void destroy(struct extension *e)
{
    if (e->env)
        wasm_runtime_destroy_exec_env(e->env);
    if (e->inst)
        wasm_runtime_deinstantiate(e->inst);
    if (e->module)
        wasm_runtime_unload(e->module);
    free(e->bytes);
    e->env = NULL, e->inst = NULL, e->module = NULL, e->bytes = NULL, e->on_frame = NULL;
    world_release(e->id);
}

/* Read, instantiate and start (on_load) an extension. 0, or -1 with the reason in e->problem. */
static int start(struct extension *e)
{
    FILE *f = fopen(e->path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size <= 0 || !(e->bytes = malloc(size)) || fseek(f, 0, SEEK_SET) != 0 || fread(e->bytes, 1, size, f) != (size_t)size) {
        snprintf(e->problem, sizeof(e->problem), "can't read %s", e->path);
        if (f)
            fclose(f);
        return -1;
    }
    fclose(f);
    char message[256];
    e->id = next_id++;
    e->problem[0] = '\0';
    e->module = wasm_runtime_load(e->bytes, (uint32_t)size, message, sizeof(message));
    if (!e->module) {
        snprintf(e->problem, sizeof(e->problem), "not a usable WebAssembly module: %s", message);
        return -1;
    }
    wasm_runtime_set_wasi_args(e->module, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    e->inst = wasm_runtime_instantiate(e->module, STACK_SIZE, 0, message, sizeof(message));
    if (!e->inst || !(e->env = wasm_runtime_create_exec_env(e->inst, STACK_SIZE))) {
        snprintf(e->problem, sizeof(e->problem), "couldn't start it: %s", e->inst ? "no execution environment" : message);
        return -1;
    }
    e->on_frame = wasm_runtime_lookup_function(e->inst, "on_frame");
    /* (Instantiating has already run _initialize, the C library's setup; wasi-libc traps if it runs twice) */
    if (call(e, "on_load", 0, NULL, LOAD_BUDGET) != 0)
        return -1;
    if (e->placeholder && sign_place(e->id, e->name) != 0)
        wlog("%s: no sign: %s", e->name, world_error());
    return 0;
}

static struct extension *find(const char *name)
{
    for (int i = 0; i < MAX_EXTENSIONS; i++)
        if (extensions[i].used && strcmp(extensions[i].name, name) == 0)
            return &extensions[i];
    return NULL;
}

static void stop_extension(struct extension *e)
{
    if (e->inst && !e->failed)
        call(e, "on_unload", 0, NULL, FRAME_BUDGET);
    destroy(e);
}

int ext_load(const char *name, const char *path, int placeholder, char *reply, size_t size)
{
    struct extension *slot = find(name), previous = { 0 };
    if (!slot) {
        for (int i = 0; i < MAX_EXTENSIONS && !slot; i++)
            if (!extensions[i].used)
                slot = &extensions[i];
        if (!slot) {
            snprintf(reply, size, "error: no room for more than %d extensions\n", MAX_EXTENSIONS);
            return -1;
        }
    } else {
        /* The old version goes first, so the new one starts from the world as it was */
        previous = *slot;
        stop_extension(slot);
    }
    struct extension *e = slot;
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->placeholder = placeholder;
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "%s", path);
    double t0 = world_time();
    if (start(e) != 0) {
        char problem[sizeof(e->problem)];
        snprintf(problem, sizeof(problem), "%s", e->problem);
        destroy(e);
        wlog("%s didn't load: %s", name, problem);
        if (previous.used && !previous.failed) {
            snprintf(e->path, sizeof(e->path), "%s", previous.path);
            e->placeholder = previous.placeholder;
            if (start(e) == 0) {
                snprintf(reply, size, "error: %s\nthe previous version is running again\n", problem);
                return -1;
            }
            destroy(e);
        }
        memset(e, 0, sizeof(*e));
        snprintf(reply, size, "error: %s\n", problem);
        return -1;
    }
    wlog("%s loaded from %s (on_load %.0f ms)%s", name, path, 1000 * (world_time() - t0), placeholder ? ", a placeholder" : "");
    snprintf(reply, size, "loaded %s%s (on_load took %.0f ms)\n", name, placeholder ? " as a placeholder, with a road-works sign" : "",
             1000 * (world_time() - t0));
    if (e->problem[0]) {   /* a world call failed, though nothing trapped: likely the reason it looks wrong */
        size_t used = strlen(reply);
        snprintf(reply + used, size - used, "but a call failed: %s\n", e->problem);
    }
    return 0;
}

int ext_unload(const char *name, char *reply, size_t size)
{
    struct extension *e = find(name);
    if (!e) {
        snprintf(reply, size, "error: no extension called %s\n", name);
        return -1;
    }
    stop_extension(e);
    memset(e, 0, sizeof(*e));
    snprintf(reply, size, "unloaded %s\n", name);
    wlog("%s unloaded", name);
    return 0;
}

void ext_list(char *reply, size_t size)
{
    size_t used = 0;
    reply[0] = '\0';
    for (int i = 0; i < MAX_EXTENSIONS && used < size; i++) {
        struct extension *e = &extensions[i];
        if (e->used)
            used += snprintf(reply + used, size - used, "%s: %s%s%s%s\n", e->name, e->failed ? "failed" : "running",
                             e->placeholder ? " (placeholder)" : "", e->problem[0] ? ": " : "", e->problem);
    }
    if (used == 0)
        snprintf(reply, size, "no extensions loaded\n");
}

void ext_unload_all(void)
{
    for (int i = 0; i < MAX_EXTENSIONS; i++)
        if (extensions[i].used) {
            stop_extension(&extensions[i]);
            memset(&extensions[i], 0, sizeof(extensions[i]));
        }
}

void ext_frame(float dt)
{
    for (int i = 0; i < MAX_EXTENSIONS; i++) {
        struct extension *e = &extensions[i];
        if (!e->used || e->failed || !e->on_frame)
            continue;
        uint32_t argv[1];
        memcpy(&argv[0], &dt, sizeof(dt));
        if (call(e, "on_frame", 1, argv, FRAME_BUDGET) != 0) {
            wlog("%s stopped: %s", e->name, e->problem);
            destroy(e);
            e->failed = 1;   /* kept in the list, with the reason, until it's loaded again or unloaded */
        }
    }
}
