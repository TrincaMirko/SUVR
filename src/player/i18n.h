#ifndef STEREONAUT_I18N_H
#define STEREONAUT_I18N_H

/* Text catalog. User-facing strings live in plain "key=value" files under a
 * `lang/` directory next to the executable (resolved relative to the working
 * directory, like the shaders): lang/eng, lang/deu, lang/ita, ... Adding a
 * language is just adding a file. English (lang/eng) is the default and the
 * fallback for any key a translation is missing.
 *
 * tr("key") returns the translation for the active language, falling back to
 * English and finally to the key itself. */

#define I18N_MAX_LANGUAGES 32

struct i18n_language {
    char code[16];  /* file name, e.g. "eng" */
    char name[48];  /* display name from the file's "language.name" key */
};

/* Loads lang/<code> as the active catalog and lang/eng as the fallback. A
 * NULL or missing code selects "eng". Safe to call again to switch. */
void i18n_load(const char *code);

/* The active language code ("eng" when nothing was loaded). */
const char *i18n_current(void);

/* Translation for `key`: active language, then English, then `key`. */
const char *tr(const char *key);

/* Lists the language files found under lang/ (for the options menu). Returns
 * the count written to `out` (at most `max`). */
int i18n_list(struct i18n_language *out, int max);

#endif
