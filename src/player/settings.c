#define _POSIX_C_SOURCE 200809L
#include "settings.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool settings_path(char *path, size_t size)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (base && base[0])
        return snprintf(path, size, "%s/stereonaut/settings.json", base) <
               (int)size;
    if (home && home[0])
        return snprintf(path, size, "%s/.config/stereonaut/settings.json",
                        home) < (int)size;
    return false;
}

/* The value of `key` if it has `type`, else NULL; *root must be released. */
static json_object *lookup(const char *key, json_type type, json_object **root)
{
    char path[4200];
    *root = settings_path(path, sizeof(path)) ? json_object_from_file(path)
                                              : NULL;
    json_object *value = NULL;
    if (!*root || !json_object_object_get_ex(*root, key, &value) ||
        !json_object_is_type(value, type))
        return NULL;
    return value;
}

static void store(const char *key, json_object *value);

bool settings_get_string(const char *key, char *out, size_t size)
{
    json_object *root = NULL;
    json_object *value = lookup(key, json_type_string, &root);
    bool found = false;
    if (value) {
        snprintf(out, size, "%s", json_object_get_string(value));
        found = true;
    }
    if (root)
        json_object_put(root);
    return found;
}

void settings_set_string(const char *key, const char *value)
{
    store(key, json_object_new_string(value));
}

int settings_get_int(const char *key, int fallback)
{
    json_object *root = NULL;
    json_object *value = lookup(key, json_type_int, &root);
    int result = value ? json_object_get_int(value) : fallback;
    if (root)
        json_object_put(root);
    return result;
}

bool settings_get_bool(const char *key, bool fallback)
{
    json_object *root = NULL;
    json_object *value = lookup(key, json_type_boolean, &root);
    bool result = value ? json_object_get_boolean(value) : fallback;
    if (root)
        json_object_put(root);
    return result;
}

double settings_get_double(const char *key, double fallback)
{
    json_object *root = NULL;
    json_object *value = lookup(key, json_type_double, &root);
    if (!value) {
        if (root)
            json_object_put(root);
        value = lookup(key, json_type_int, &root);
    }
    double result = value ? json_object_get_double(value) : fallback;
    if (root)
        json_object_put(root);
    return result;
}

/* Takes ownership of `value`. */
static void store(const char *key, json_object *value)
{
    char path[4200];
    if (!settings_path(path, sizeof(path))) {
        json_object_put(value);
        return;
    }
    json_object *root = access(path, F_OK) == 0
                            ? json_object_from_file(path) : NULL;
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root)
            json_object_put(root);
        if (access(path, F_OK) == 0) {
            json_object_put(value);
            return; /* unreadable: do not overwrite */
        }
        root = json_object_new_object();
    }
    json_object_object_add(root, key, value);
    char directory[4200];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        char parent[4200];
        snprintf(parent, sizeof(parent), "%s", directory);
        char *parent_slash = strrchr(parent, '/');
        if (parent_slash) {
            *parent_slash = '\0';
            mkdir(parent, 0700);
        }
        mkdir(directory, 0700);
    }
    char temporary[4300];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (json_object_to_file_ext(temporary, root, JSON_C_TO_STRING_PRETTY) == 0)
        chmod(temporary, 0600),
        rename(temporary, path);
    json_object_put(root);
}

void settings_set_int(const char *key, int value)
{
    store(key, json_object_new_int(value));
}

void settings_set_bool(const char *key, bool value)
{
    store(key, json_object_new_boolean(value));
}

void settings_set_double(const char *key, double value)
{
    store(key, json_object_new_double(value));
}
