/*
 * The world's memory: small named values that outlive an extension's versions and the world itself (it's
 * kept on storage). An extension keeps its state here in on_unload() and gets it back in on_load(), so a
 * new version carries on where the old one was; and as keys are shared, extensions also pass things to
 * each other ("dog.position").
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

#define MAX_KEYS 512
#define MAX_KEY 64
#define MAX_VALUE 65536

static struct entry {
    char key[MAX_KEY];
    uint32_t size;
    uint8_t *value;
} entries[MAX_KEYS];
static int count, changed;

static struct entry *find(const char *key)
{
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].key, key) == 0)
            return &entries[i];
    return NULL;
}

static int store(const char *key, const void *data, uint32_t size)
{
    struct entry *e = find(key);
    if (!e) {
        if (count == MAX_KEYS)
            return -1;
        e = &entries[count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->value = NULL, e->size = 0;
    }
    if (e->size != size || !e->value) {
        uint8_t *value = realloc(e->value, size ? size : 1);
        if (!value)
            return -1;
        e->value = value;
    }
    memcpy(e->value, data, size);
    e->size = size;
    return 0;
}

int world_remember(const char *key, const void *data, int size)
{
    if (!key[0] || strlen(key) >= MAX_KEY || size < 0 || size > MAX_VALUE)
        return world_fail("remember: a key of 1 to %d characters and up to %d bytes", MAX_KEY - 1, MAX_VALUE);
    struct entry *e = find(key);
    if (e && e->size == (uint32_t)size && memcmp(e->value, data, size) == 0)
        return 0;   /* unchanged: nothing to save */
    if (store(key, data, size) != 0)
        return world_fail("remember: no room (at most %d keys)", MAX_KEYS);
    changed = 1;
    return 0;
}

int world_recall(const char *key, void *data, int size)
{
    struct entry *e = find(key);
    if (!e)
        return -1;   /* nothing kept: not an error worth a message */
    memcpy(data, e->value, (size_t)size < e->size ? (size_t)size : e->size);
    return (int)e->size;
}

/* On storage: for each value, a 2-byte key length, the key, a 4-byte size and the bytes */
void memory_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    uint16_t key_length;
    while (fread(&key_length, 2, 1, f) == 1 && key_length > 0 && key_length < MAX_KEY) {
        char key[MAX_KEY];
        uint32_t size;
        if (fread(key, 1, key_length, f) != key_length || fread(&size, 4, 1, f) != 1 || size > MAX_VALUE)
            break;
        key[key_length] = '\0';
        uint8_t *value = malloc(size ? size : 1);
        if (!value || fread(value, 1, size, f) != size) {
            free(value);
            break;
        }
        store(key, value, size);
        free(value);
    }
    fclose(f);
    wlog("memory: %d values from %s", count, path);
}

void memory_save(const char *path)
{
    if (!changed)
        return;
    char part[512];
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) {
        wlog("memory: can't write %s: %s", part, strerror(errno));
        return;
    }
    for (int i = 0; i < count; i++) {
        uint16_t key_length = (uint16_t)strlen(entries[i].key);
        fwrite(&key_length, 2, 1, f);
        fwrite(entries[i].key, 1, key_length, f);
        fwrite(&entries[i].size, 4, 1, f);
        fwrite(entries[i].value, 1, entries[i].size, f);
    }
    if (fclose(f) == 0 && rename(part, path) == 0)
        changed = 0;
}
