#define _POSIX_C_SOURCE 200809L
#include "i18n.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ponytail: flat arrays + linear lookup. A few hundred keys looked up per
 * frame is noise next to the Vulkan work; a hash map would be more code for
 * no measurable gain. Add one if a profile ever says otherwise. */

#define MAX_ENTRIES 1024
#define LANG_DIR "lang"

struct entry {
    char *key;
    char *value;
};

struct catalog {
    struct entry entries[MAX_ENTRIES];
    int count;
    char *backing; /* whole file, keys/values point into it */
};

static struct catalog active;
static struct catalog fallback; /* English */
static char active_code[16] = "eng";

static void catalog_free(struct catalog *c)
{
    free(c->backing);
    c->backing = NULL;
    c->count = 0;
}

/* Parses "key=value" lines in place (backing owned by the catalog). Lines
 * starting with '#' and blank lines are ignored; the split is the first '='. */
static bool catalog_load(struct catalog *c, const char *code)
{
    catalog_free(c);
    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", LANG_DIR, code) >= (int)sizeof(path))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return false;
    }
    fclose(f);
    data[size] = '\0';
    c->backing = data;

    char *line = data;
    while (line && *line && c->count < MAX_ENTRIES) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        /* strip a trailing CR for CRLF files */
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        if (line[0] && line[0] != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                c->entries[c->count].key = line;
                c->entries[c->count].value = eq + 1;
                c->count++;
            }
        }
        line = next;
    }
    return true;
}

static const char *catalog_get(const struct catalog *c, const char *key)
{
    for (int i = 0; i < c->count; ++i)
        if (strcmp(c->entries[i].key, key) == 0)
            return c->entries[i].value;
    return NULL;
}

void i18n_load(const char *code)
{
    if (!code || !code[0])
        code = "eng";
    snprintf(active_code, sizeof(active_code), "%s", code);
    if (!catalog_load(&active, code)) {
        /* Unknown language: fall back to English as the active catalog. */
        snprintf(active_code, sizeof(active_code), "eng");
        catalog_load(&active, "eng");
    }
    if (strcmp(active_code, "eng") == 0)
        catalog_free(&fallback);
    else
        catalog_load(&fallback, "eng");
}

const char *i18n_current(void)
{
    return active_code;
}

const char *tr(const char *key)
{
    const char *v = catalog_get(&active, key);
    if (v)
        return v;
    v = catalog_get(&fallback, key);
    if (v)
        return v;
    return key;
}

int i18n_list(struct i18n_language *out, int max)
{
    DIR *dir = opendir(LANG_DIR);
    if (!dir)
        return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(dir))) {
        if (e->d_name[0] == '.' ||
            strlen(e->d_name) >= sizeof(out[n].code))
            continue;
        /* Skip obvious non-catalogs; keep extensionless names as-is. */
        char path[256];
        if (snprintf(path, sizeof(path), "%s/%s", LANG_DIR, e->d_name) >=
            (int)sizeof(path))
            continue;
        struct catalog c = {0};
        if (!catalog_load(&c, e->d_name))
            continue;
        snprintf(out[n].code, sizeof(out[n].code), "%s", e->d_name);
        const char *name = catalog_get(&c, "language.name");
        snprintf(out[n].name, sizeof(out[n].name), "%.*s",
                 (int)sizeof(out[n].name) - 1, name ? name : e->d_name);
        catalog_free(&c);
        n++;
    }
    closedir(dir);
    return n;
}
