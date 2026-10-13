#ifndef STEREONAUT_SETTINGS_H
#define STEREONAUT_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

/* Global preferences in $XDG_CONFIG_HOME/stereonaut/settings.json (browser
 * sort order, scrub preview). Reads fall back when the file or key is
 * missing; writes keep the other keys and are not fatal on failure. */

/* Copies the string value of `key` into `out` (NUL-terminated, truncated to
 * `size`); returns false and leaves `out` untouched when absent. */
bool settings_get_string(const char *key, char *out, size_t size);
void settings_set_string(const char *key, const char *value);

int settings_get_int(const char *key, int fallback);
bool settings_get_bool(const char *key, bool fallback);
double settings_get_double(const char *key, double fallback);
void settings_set_int(const char *key, int value);
void settings_set_bool(const char *key, bool value);
void settings_set_double(const char *key, double value);

#endif
