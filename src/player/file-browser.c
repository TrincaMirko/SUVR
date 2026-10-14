#define _XOPEN_SOURCE 700
#include "file-browser.h"
#include "jellyfin.h"
#include "webdav.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

struct file_browser {
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool worker_started;
    bool stopping;
    char *request_path;
    uint64_t request_generation;
    struct file_browser_listing ready;
    bool has_ready;
    /* Stream information: the entries asked for (ids) and the answer. */
    uint64_t streams_generation;
    size_t streams_count;
    size_t streams_index[FILE_BROWSER_STREAMS_MAX];
    char streams_id[FILE_BROWSER_STREAMS_MAX][JF_ID_SIZE];
    struct file_browser_streams streams_ready;
    bool has_streams;
    int sort;
    bool descending;
    int local_sort;
    bool local_descending;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

void file_browser_listing_clear(struct file_browser_listing *listing)
{
    if (!listing)
        return;
    for (size_t i = 0; i < listing->entry_count; ++i) {
        free(listing->entries[i].name);
        free(listing->entries[i].path);
        free(listing->entries[i].image);
    }
    free(listing->entries);
    free(listing->path);
    memset(listing, 0, sizeof(*listing));
}

static bool is_media_name(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension &&
           (strcasecmp(extension, ".mkv") == 0 ||
            strcasecmp(extension, ".mp4") == 0);
}

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    bool root = directory_length == 1 && directory[0] == '/';
    size_t size = directory_length + (root ? 0U : 1U) + strlen(name) + 1U;
    char *path = malloc(size);
    if (!path)
        return NULL;
    snprintf(path, size, root ? "/%s" : "%s/%s", directory, name);
    return path;
}

/* Order for compare_entries; only the browser worker sorts. */
static int compare_sort = FILE_BROWSER_SORT_NAME;
static bool compare_descending;

static int compare_entries(const void *left_pointer, const void *right_pointer)
{
    const struct file_browser_entry *left = left_pointer;
    const struct file_browser_entry *right = right_pointer;
    if (left->kind != right->kind)
        return left->kind < right->kind ? -1 : 1;
    int order = 0;
    if (compare_sort == FILE_BROWSER_SORT_MODIFIED)
        order = (left->modified > right->modified) -
                (left->modified < right->modified);
    else if (compare_sort == FILE_BROWSER_SORT_SIZE &&
             left->kind == FILE_BROWSER_MEDIA)
        order = (left->size > right->size) - (left->size < right->size);
    if (order)
        return compare_descending ? -order : order;
    int folded = strcasecmp(left->name, right->name);
    return folded ? folded : strcmp(left->name, right->name);
}

static int append_entry(struct file_browser_listing *listing,
                        enum file_browser_entry_kind kind,
                        const char *name, char *path)
{
    size_t next_count = listing->entry_count + 1U;
    struct file_browser_entry *entries =
        realloc(listing->entries, next_count * sizeof(*entries));
    if (!entries) {
        free(path);
        return -1;
    }
    listing->entries = entries;
    struct file_browser_entry *entry = &entries[listing->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->name = strdup(name);
    entry->path = path;
    if (!entry->name || !entry->path) {
        free(entry->name);
        free(entry->path);
        memset(entry, 0, sizeof(*entry));
        return -1;
    }
    listing->entry_count = next_count;
    return 0;
}

static void parent_path(const char *path, char *parent, size_t size)
{
    snprintf(parent, size, "%s", path);
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1U] == '/')
        parent[--length] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        snprintf(parent, size, "/");
    else
        *slash = '\0';
}

static struct file_browser_listing read_directory(const char *requested,
                                                   uint64_t generation,
                                                   int sort, bool descending)
{
    struct file_browser_listing listing = {.generation = generation};
    char *resolved = realpath(requested, NULL);
    if (!resolved) {
        snprintf(listing.error, sizeof(listing.error),
                 "Cannot open directory: %s", strerror(errno));
        listing.path = strdup(requested);
        return listing;
    }
    /* Display title "Local files / home / user / Videos". */
    size_t title_size = strlen(resolved) * 2U + 32U;
    listing.path = malloc(title_size);
    if (listing.path) {
        size_t used = (size_t)snprintf(listing.path, title_size,
                                       "Local files");
        for (const char *c = resolved; *c && used + 4U < title_size; ++c) {
            if (*c == '/') {
                if (c[1]) {
                    memcpy(listing.path + used, " / ", 3);
                    used += 3;
                }
            } else {
                listing.path[used++] = *c;
            }
        }
        listing.path[used] = '\0';
    }
    DIR *directory = opendir(resolved);
    if (!directory) {
        snprintf(listing.error, sizeof(listing.error),
                 "Cannot open directory: %s", strerror(errno));
        free(resolved);
        return listing;
    }
    if (strcmp(resolved, "/") != 0) {
        char parent[4096];
        parent_path(resolved, parent, sizeof(parent));
        if (append_entry(&listing, FILE_BROWSER_PARENT, "..",
                         strdup(parent)) < 0)
            goto allocation_failure;
    }
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(directory);
        if (!item) {
            if (errno)
                snprintf(listing.error, sizeof(listing.error),
                         "Directory could not be fully read: %s",
                         strerror(errno));
            break;
        }
        if (strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0)
            continue;
        char *path = join_path(resolved, item->d_name);
        if (!path)
            goto allocation_failure;
        struct stat status;
        if (stat(path, &status) < 0) {
            free(path);
            continue;
        }
        enum file_browser_entry_kind kind;
        if (S_ISDIR(status.st_mode))
            kind = FILE_BROWSER_DIRECTORY;
        else if (S_ISREG(status.st_mode) && is_media_name(item->d_name))
            kind = FILE_BROWSER_MEDIA;
        else {
            free(path);
            continue;
        }
        if (append_entry(&listing, kind, item->d_name, path) < 0)
            goto allocation_failure;
        if (kind == FILE_BROWSER_MEDIA) {
            size_t length = strlen(path) + 6U;
            char *image = malloc(length);
            if (image)
                snprintf(image, length, "file:%s", path);
            listing.entries[listing.entry_count - 1U].image = image;
        }
        listing.entries[listing.entry_count - 1U].modified =
            (int64_t)status.st_mtime;
        listing.entries[listing.entry_count - 1U].size =
            (int64_t)status.st_size;
    }
    closedir(directory);
    free(resolved);
    compare_sort = sort;
    compare_descending = descending;
    qsort(listing.entries, listing.entry_count, sizeof(*listing.entries),
          compare_entries);
    return listing;

allocation_failure:
    closedir(directory);
    free(resolved);
    file_browser_listing_clear(&listing);
    listing.generation = generation;
    listing.path = strdup(requested);
    snprintf(listing.error, sizeof(listing.error),
             "Out of memory for directory listing");
    return listing;
}

/* ---- virtual root and Jellyfin sources ---------------------------------- */

static char login_code[16];
static char login_secret[128];

static int add_entry(struct file_browser_listing *listing,
                     enum file_browser_entry_kind kind, const char *name,
                     const char *path)
{
    char *copy = strdup(path);
    return copy ? append_entry(listing, kind, name, copy) : -1;
}

/* Appends Jellyfin items; titles remember `folder_id` to return to after
 * playback ("" = the start page). */
static int add_jellyfin_items(struct file_browser_listing *listing,
                              const struct jf_item *items, size_t count,
                              const char *folder_id)
{
    for (size_t i = 0; i < count; ++i) {
        char path[4 * JF_ID_SIZE + 32];
        if (items[i].is_folder)
            snprintf(path, sizeof(path), "jf:view:%s", items[i].id);
        else
            snprintf(path, sizeof(path), "jf:item:%s:%s", items[i].id,
                     folder_id);
        if (add_entry(listing,
                      items[i].is_folder ? FILE_BROWSER_DIRECTORY
                                         : FILE_BROWSER_MEDIA,
                      items[i].name, path) < 0)
            return -1;
        struct file_browser_entry *entry =
            &listing->entries[listing->entry_count - 1U];
        if (items[i].image[0]) {
            size_t size = strlen(items[i].id) + strlen(items[i].image) + 2U;
            entry->image = malloc(size);
            if (entry->image)
                snprintf(entry->image, size, "%s|%s", items[i].id,
                         items[i].image);
        }
        entry->duration_us = items[i].runtime_ticks / 10;
        entry->year = items[i].year;
        snprintf(entry->codec, sizeof(entry->codec), "%s", items[i].codec);
        entry->width = items[i].width;
        entry->height = items[i].height;
        entry->resume_us = items[i].resume_ticks / 10;
        entry->played_percentage = items[i].played_percentage;
        entry->played = items[i].played;
    }
    return 0;
}

/* Start page: the two sources, then Jellyfin's "continue watching" titles
 * (skipped quietly if the server is not configured or not reachable). */
static struct file_browser_listing read_root(uint64_t generation)
{
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup("Start");
    const char *home = getenv("HOME");
    if (add_entry(&listing, FILE_BROWSER_DIRECTORY, "Local files",
                  home && home[0] ? home : "/") < 0 ||
        add_entry(&listing, FILE_BROWSER_DIRECTORY, "Jellyfin", "jf:") < 0 ||
        add_entry(&listing, FILE_BROWSER_DIRECTORY, "WebDAV", "webdav:") < 0) {
        file_browser_listing_clear(&listing);
        listing.generation = generation;
        snprintf(listing.error, sizeof(listing.error), "Out of memory");
        return listing;
    }
    struct jf_config config;
    struct jf_item *items = NULL;
    size_t count = 0;
    if (jf_config_load(&config, NULL, 0) == 0 &&
        jf_config_has_token(&config) &&
        jf_list_resume(&config, &items, &count, NULL, 0) == 0)
        add_jellyfin_items(&listing, items, count, "");
    free(items);
    return listing;
}

static struct file_browser_listing jellyfin_message(
    uint64_t generation, const char *title, const char *message)
{
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup(title);
    snprintf(listing.error, sizeof(listing.error), "%s", message);
    add_entry(&listing, FILE_BROWSER_PARENT, "..", "root:");
    return listing;
}

static struct file_browser_listing jellyfin_login(
    uint64_t generation, struct jf_config *config, bool poll,
    const char *note)
{
    char error[256] = {0};
    if (poll && login_secret[0]) {
        bool authenticated = false;
        if (jf_quickconnect_poll(config, login_secret, &authenticated, error,
                                 sizeof(error)) == 0 && authenticated) {
            login_secret[0] = '\0';
            return (struct file_browser_listing){.generation = generation};
        }
        if (!error[0])
            note = "Not yet confirmed";
        else
            login_secret[0] = '\0'; /* expired or rejected: request a new code */
    }
    if (!login_secret[0] &&
        jf_quickconnect_start(config, login_code, login_secret, error,
                              sizeof(error)) < 0)
        return jellyfin_message(generation, "Sign in to Jellyfin",
                                error[0] ? error : "Login failed");
    struct file_browser_listing listing = {.generation = generation};
    listing.path = strdup("Sign in to Jellyfin");
    snprintf(listing.error, sizeof(listing.error), "%s",
             note ? note : "Confirm the code in Jellyfin under Quick Connect");
    char label[64];
    add_entry(&listing, FILE_BROWSER_PARENT, "..", "root:");
    snprintf(label, sizeof(label), "Code %s", login_code);
    add_entry(&listing, FILE_BROWSER_DIRECTORY, label, "jf:login");
    add_entry(&listing, FILE_BROWSER_DIRECTORY, "Continue after confirmation",
              "jf:login");
    return listing;
}

static const char *const filter_keys[JF_FILTER_COUNT] = {
    "", "genre", "studio", "person",
};
static const char *const filter_names[JF_FILTER_COUNT] = {
    "", "Genre", "Studio", "Cast",
};
static const char *const filter_titles[JF_FILTER_COUNT] = {
    "", "Genres", "Studios", "Cast",
};

/* "genre=ID" (id copied when `id` is given) or "genre" alone. */
static enum jf_filter parse_filter(const char *text, char *id,
                                   size_t id_size)
{
    for (int filter = JF_FILTER_GENRE; filter < JF_FILTER_COUNT; ++filter) {
        size_t length = strlen(filter_keys[filter]);
        if (strncmp(text, filter_keys[filter], length) != 0)
            continue;
        if (!id && text[length] == '\0')
            return (enum jf_filter)filter;
        if (id && text[length] == '=' && text[length + 1] &&
            strlen(text + length + 1) < id_size) {
            snprintf(id, id_size, "%s", text + length + 1);
            return (enum jf_filter)filter;
        }
    }
    return JF_FILTER_NONE;
}

/* Genres, studios or persons as folders that open the filtered listing. */
static int add_filter_values(struct file_browser_listing *listing,
                             const struct jf_item *items, size_t count,
                             const char *folder_id, enum jf_filter filter)
{
    for (size_t i = 0; i < count; ++i) {
        char path[3 * JF_ID_SIZE + 32];
        snprintf(path, sizeof(path), "jf:view:%s/%s=%s", folder_id,
                 filter_keys[filter], items[i].id);
        if (add_entry(listing, FILE_BROWSER_DIRECTORY, items[i].name,
                      path) < 0)
            return -1;
        struct file_browser_entry *entry =
            &listing->entries[listing->entry_count - 1U];
        if (items[i].image[0]) {
            size_t size = strlen(items[i].id) + strlen(items[i].image) + 2U;
            entry->image = malloc(size);
            if (entry->image)
                snprintf(entry->image, size, "%s|%s", items[i].id,
                         items[i].image);
        }
    }
    return 0;
}

/* The libraries change rarely but every folder title needs them: kept for
 * a minute per server and user (worker thread only). Returns a copy. */
#define VIEWS_CACHE_SECONDS 60
static struct jf_item *views_cache;
static size_t views_cache_count;
static time_t views_cache_time;
static char views_cache_key[sizeof(((struct jf_config *)0)->server) +
                            sizeof(((struct jf_config *)0)->user_id)];

static int list_views_cached(const struct jf_config *config,
                             struct jf_item **items, size_t *count,
                             char *error, size_t error_size)
{
    char key[sizeof(views_cache_key)];
    snprintf(key, sizeof(key), "%s|%s", config->server, config->user_id);
    time_t now = time(NULL);
    if (!views_cache || strcmp(key, views_cache_key) != 0 ||
        now - views_cache_time > VIEWS_CACHE_SECONDS ||
        now < views_cache_time) {
        struct jf_item *fresh = NULL;
        size_t fresh_count = 0;
        if (jf_list_views(config, &fresh, &fresh_count, error, error_size) < 0)
            return -1;
        free(views_cache);
        views_cache = fresh;
        views_cache_count = fresh_count;
        views_cache_time = now;
        snprintf(views_cache_key, sizeof(views_cache_key), "%s", key);
    }
    *count = views_cache_count;
    *items = malloc((views_cache_count ? views_cache_count : 1U) *
                    sizeof(**items));
    if (!*items) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    memcpy(*items, views_cache, views_cache_count * sizeof(**items));
    return 0;
}

static struct file_browser_listing read_jellyfin(const char *request,
                                                  uint64_t generation,
                                                  enum jf_sort sort,
                                                  bool descending)
{
    char error[256] = {0};
    struct jf_config config;
    if (jf_config_load(&config, error, sizeof(error)) < 0)
        return jellyfin_message(generation, "Jellyfin", error);
    bool login_request = strcmp(request, "jf:login") == 0;
    if (login_request || !jf_config_has_token(&config)) {
        struct file_browser_listing login =
            jellyfin_login(generation, &config, login_request, NULL);
        if (login.path)
            return login;
        request = "jf:"; /* authenticated just now */
    }

    struct file_browser_listing listing = {.generation = generation};
    struct jf_item *items = NULL;
    size_t count = 0;
    char title[320] = "Jellyfin";
    char parent_path_text[JF_ID_SIZE + 16] = "root:";
    const char *folder_id = NULL;
    /* "jf:view:FOLDER/genre=ID" lists a folder's titles of one genre (or
     * studio, person); "jf:filters:FOLDER:genre" the genres to pick from. */
    char folder[JF_ID_SIZE] = "", filter_id[JF_ID_SIZE] = "";
    enum jf_filter filter = JF_FILTER_NONE;
    bool filter_values = false;
    if (strncmp(request, "jf:view:", 8) == 0 ||
        strncmp(request, "jf:filters:", 11) == 0) {
        filter_values = request[3] == 'f';
        const char *start = request + (filter_values ? 11 : 8);
        size_t length = strcspn(start, filter_values ? ":" : "/");
        if (length < sizeof(folder)) {
            memcpy(folder, start, length);
            folder[length] = '\0';
        }
        folder_id = folder;
        const char *rest = start + length;
        if (*rest)
            ++rest;
        filter = parse_filter(rest, filter_values ? NULL : filter_id,
                              sizeof(filter_id));
        if (filter_values && filter == JF_FILTER_NONE)
            filter_values = false;
    }
    /* "jf:search:QUERY": titles of all libraries matching QUERY. */
    const char *query = strncmp(request, "jf:search:", 10) == 0
                            ? request + 10
                            : NULL;
    int result;
    if (query) {
        snprintf(title, sizeof(title), "Suche / %s", query);
        result = jf_search(&config, query, &items, &count, error,
                           sizeof(error));
    } else if (!folder_id) {
        result = list_views_cached(&config, &items, &count, error,
                                   sizeof(error));
    } else {
        struct jf_item *views = NULL;
        size_t view_count = 0;
        bool is_view = false;
        if (list_views_cached(&config, &views, &view_count, error,
                              sizeof(error)) == 0) {
            for (size_t i = 0; i < view_count; ++i)
                if (strcmp(views[i].id, folder_id) == 0) {
                    is_view = true;
                    snprintf(title, sizeof(title), "Jellyfin / %s",
                             views[i].name);
                }
            free(views);
        }
        if (is_view) {
            snprintf(parent_path_text, sizeof(parent_path_text), "jf:");
        } else {
            char name[256] = {0}, parent[JF_ID_SIZE] = {0};
            if (jf_item_info(&config, folder_id, name, sizeof(name), parent,
                             sizeof(parent), error, sizeof(error)) == 0) {
                snprintf(title, sizeof(title), "Jellyfin / %s", name);
                if (parent[0])
                    snprintf(parent_path_text, sizeof(parent_path_text),
                             "jf:view:%s", parent);
                else
                    snprintf(parent_path_text, sizeof(parent_path_text), "jf:");
            }
        }
        error[0] = '\0';
        if (filter != JF_FILTER_NONE) {
            /* Filters belong to the folder: ".." leads back to it. */
            snprintf(parent_path_text, sizeof(parent_path_text),
                     "jf:view:%s", folder_id);
            size_t used = strlen(title);
            if (filter_values) {
                snprintf(title + used, sizeof(title) - used, " / %s",
                         filter_titles[filter]);
            } else {
                char name[256] = {0};
                jf_item_info(&config, filter_id, name, sizeof(name), NULL, 0,
                             NULL, 0);
                snprintf(title + used, sizeof(title) - used, " / %s: %s",
                         filter_names[filter], name[0] ? name : "?");
            }
        }
        if (filter_values)
            result = jf_list_filter_values(&config, folder_id, filter,
                                           &items, &count, error,
                                           sizeof(error));
        else
            result = jf_list_children(&config, folder_id, filter, filter_id,
                                      sort, descending, &items, &count,
                                      error, sizeof(error));
    }
    if (result == JF_AUTH_RETRY) {
        config.token[0] = '\0';
        login_secret[0] = '\0';
        return jellyfin_login(generation, &config, false,
                              "Login expired: confirm a new code");
    }
    if (result < 0)
        return jellyfin_message(generation, "Jellyfin",
                                error[0] ? error : "Jellyfin error");
    listing.path = strdup(title);
    if (add_entry(&listing, FILE_BROWSER_PARENT, "..", parent_path_text) < 0)
        goto allocation_failure;
    if (filter_values) {
        if (add_filter_values(&listing, items, count, folder_id, filter) < 0)
            goto allocation_failure;
    } else {
        /* Titles remember the filtered folder (or "?QUERY" for search
         * results) to come back to. */
        char back[2 * JF_ID_SIZE + 128];
        if (query)
            snprintf(back, sizeof(back), "?%.120s", query);
        else
            snprintf(back, sizeof(back), "%s", folder_id ? folder_id : "");
        if (filter != JF_FILTER_NONE) {
            size_t used = strlen(back);
            snprintf(back + used, sizeof(back) - used, "/%s=%s",
                     filter_keys[filter], filter_id);
        }
        if (add_jellyfin_items(&listing, items, count, back) < 0)
            goto allocation_failure;
        if (query && count == 0)
            snprintf(listing.error, sizeof(listing.error),
                     "No matches for “%.120s”", query);
    }
    free(items);
    return listing;

allocation_failure:
    free(items);
    file_browser_listing_clear(&listing);
    listing.generation = generation;
    listing.path = strdup("Jellyfin");
    snprintf(listing.error, sizeof(listing.error), "Out of memory");
    return listing;
}

/* "webdav:" lists the saved servers; "webdav:srv:<idx>:<dir>" PROPFINDs a
 * directory. Media become "webdav:play:<idx>:<path>" specs the session
 * resolves; the add-server form is a special "webdav:add" entry the UI
 * intercepts before it ever reaches here. */
static struct file_browser_listing read_webdav(const char *path,
                                               uint64_t generation)
{
    struct file_browser_listing listing = {.generation = generation};

    if (strcmp(path, "webdav:") == 0) {
        listing.path = strdup("WebDAV");
        add_entry(&listing, FILE_BROWSER_PARENT, "..", "root:");
        struct wd_server servers[WD_MAX_SERVERS];
        int count = wd_servers_load(servers, WD_MAX_SERVERS);
        for (int i = 0; i < count; ++i) {
            char base[640], spec[768];
            wd_base_path(&servers[i], base, sizeof(base));
            snprintf(spec, sizeof(spec), "webdav:srv:%d:%s", i, base);
            add_entry(&listing, FILE_BROWSER_DIRECTORY, servers[i].name, spec);
        }
        add_entry(&listing, FILE_BROWSER_DIRECTORY, "+ Add server",
                  "webdav:add");
        return listing;
    }

    /* "webdav:srv:<idx>:<dir>" */
    const char *rest = path + 11; /* strlen("webdav:srv:") */
    int index = atoi(rest);
    const char *colon = strchr(rest, ':');
    const char *dir = colon ? colon + 1 : "/";
    struct wd_server server;
    if (wd_server_get(index, &server) < 0) {
        listing.path = strdup("WebDAV");
        add_entry(&listing, FILE_BROWSER_PARENT, "..", "webdav:");
        snprintf(listing.error, sizeof(listing.error), "No such server");
        return listing;
    }
    listing.path = strdup(server.name);

    /* Parent: one directory up, or the server list at the base. */
    char base[640];
    wd_base_path(&server, base, sizeof(base));
    char parent[768];
    if (strcmp(dir, base) == 0) {
        snprintf(parent, sizeof(parent), "webdav:");
    } else {
        char up[640];
        snprintf(up, sizeof(up), "%s", dir);
        size_t len = strlen(up);
        while (len > 1 && up[len - 1] == '/')
            up[--len] = '\0';
        char *slash = strrchr(up, '/');
        if (slash)
            slash[1] = '\0';
        snprintf(parent, sizeof(parent), "webdav:srv:%d:%s", index, up);
    }
    add_entry(&listing, FILE_BROWSER_PARENT, "..", parent);

    struct wd_entry *entries = NULL;
    size_t count = 0;
    char error[256] = "";
    if (wd_list(&server, dir, &entries, &count, error, sizeof(error)) < 0) {
        snprintf(listing.error, sizeof(listing.error), "%s", error);
        return listing;
    }
    for (size_t i = 0; i < count; ++i) {
        char spec[1200];
        snprintf(spec, sizeof(spec), "webdav:%s:%d:%s",
                 entries[i].is_dir ? "srv" : "play", index, entries[i].path);
        /* ponytail: no WebDAV tile thumbnails. Software-decoding 8K HEVC
           keyframes in the renderer starved the hardware decoder's buffer
           pool and broke actual playback (REQBUFS ENOMEM). Icon only. */
        add_entry(&listing,
                  entries[i].is_dir ? FILE_BROWSER_DIRECTORY
                                    : FILE_BROWSER_MEDIA,
                  entries[i].name, spec);
        if (0) {
            char source[1200];
            snprintf(source, sizeof(source), "webdav:%d:%s", index,
                     entries[i].path);
            listing.entries[listing.entry_count - 1U].image = strdup(source);
        }
    }
    free(entries);
    return listing;
}

static struct file_browser_listing read_location(
    const char *path, uint64_t generation, int sort, bool descending,
    int local_sort, bool local_descending)
{
    if (strcmp(path, "root:") == 0)
        return read_root(generation);
    if (strncmp(path, "jf:", 3) == 0)
        return read_jellyfin(path, generation, (enum jf_sort)sort,
                             descending);
    if (strncmp(path, "webdav:", 7) == 0)
        return read_webdav(path, generation);
    return read_directory(path, generation, local_sort, local_descending);
}

/* Asks Jellyfin for the streams of the requested entries (worker thread,
 * mutex held on entry and exit). */
static void read_streams(struct file_browser *browser)
{
    uint64_t generation = browser->streams_generation;
    size_t count = browser->streams_count;
    size_t indices[FILE_BROWSER_STREAMS_MAX];
    char ids[FILE_BROWSER_STREAMS_MAX * JF_ID_SIZE] = "";
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        indices[i] = browser->streams_index[i];
        used += (size_t)snprintf(ids + used, sizeof(ids) - used, "%s%s",
                                 i ? "," : "", browser->streams_id[i]);
    }
    char id_list[FILE_BROWSER_STREAMS_MAX][JF_ID_SIZE];
    memcpy(id_list, browser->streams_id, count * JF_ID_SIZE);
    browser->streams_count = 0;
    pthread_mutex_unlock(&browser->mutex);

    struct file_browser_streams result = {.generation = generation};
    struct jf_config config;
    struct jf_item *items = NULL;
    size_t item_count = 0;
    char error[256] = "";
    if (jf_config_load(&config, error, sizeof(error)) == 0 &&
        jf_list_streams(&config, ids, &items, &item_count, error,
                        sizeof(error)) == 0) {
        for (size_t i = 0; i < count; ++i)
            for (size_t j = 0; j < item_count; ++j) {
                if (strcmp(items[j].id, id_list[i]) != 0)
                    continue;
                result.entries[result.count].index = indices[i];
                snprintf(result.entries[result.count].codec,
                         sizeof(result.entries[result.count].codec), "%s",
                         items[j].codec);
                result.entries[result.count].width = items[j].width;
                result.entries[result.count].height = items[j].height;
                ++result.count;
                break;
            }
    } else {
        fprintf(stderr, "browser_streams error=%s\n", error);
    }
    free(items);

    pthread_mutex_lock(&browser->mutex);
    if (generation != browser->request_generation)
        return;
    if (browser->has_streams &&
        browser->streams_ready.generation == generation) {
        /* Not taken yet: add to it. */
        struct file_browser_streams *ready = &browser->streams_ready;
        for (size_t i = 0; i < result.count &&
                           ready->count < FILE_BROWSER_STREAMS_MAX; ++i)
            ready->entries[ready->count++] = result.entries[i];
    } else {
        browser->streams_ready = result;
        browser->has_streams = true;
    }
}

static void *browser_worker(void *argument)
{
    struct file_browser *browser = argument;
    for (;;) {
        pthread_mutex_lock(&browser->mutex);
        while (!browser->request_path && !browser->streams_count &&
               !browser->stopping)
            pthread_cond_wait(&browser->changed, &browser->mutex);
        if (browser->stopping) {
            pthread_mutex_unlock(&browser->mutex);
            return NULL;
        }
        if (!browser->request_path) {
            read_streams(browser);
            pthread_mutex_unlock(&browser->mutex);
            continue;
        }
        char *path = browser->request_path;
        uint64_t generation = browser->request_generation;
        int sort = browser->sort;
        bool descending = browser->descending;
        int local_sort = browser->local_sort;
        bool local_descending = browser->local_descending;
        browser->request_path = NULL;
        pthread_mutex_unlock(&browser->mutex);

        struct file_browser_listing listing =
            read_location(path, generation, sort, descending, local_sort,
                          local_descending);
        free(path);

        pthread_mutex_lock(&browser->mutex);
        if (generation == browser->request_generation) {
            if (browser->has_ready)
                file_browser_listing_clear(&browser->ready);
            browser->ready = listing;
            browser->has_ready = true;
            memset(&listing, 0, sizeof(listing));
        }
        pthread_mutex_unlock(&browser->mutex);
        file_browser_listing_clear(&listing);
    }
}

void file_browser_set_sort(struct file_browser *browser, int sort,
                           bool descending)
{
    if (!browser)
        return;
    pthread_mutex_lock(&browser->mutex);
    browser->sort = sort;
    browser->descending = descending;
    pthread_mutex_unlock(&browser->mutex);
}

void file_browser_set_local_sort(struct file_browser *browser, int sort,
                                 bool descending)
{
    if (!browser)
        return;
    pthread_mutex_lock(&browser->mutex);
    browser->local_sort = sort >= 0 && sort < FILE_BROWSER_SORT_COUNT
                              ? sort
                              : FILE_BROWSER_SORT_NAME;
    browser->local_descending = descending;
    pthread_mutex_unlock(&browser->mutex);
}

size_t file_browser_request_streams(struct file_browser *browser,
                                    uint64_t generation,
                                    const struct file_browser_listing *listing,
                                    const size_t *indices, size_t count)
{
    if (!browser || !listing)
        return 0;
    pthread_mutex_lock(&browser->mutex);
    if (browser->streams_generation != generation) {
        browser->streams_generation = generation;
        browser->streams_count = 0;
    }
    size_t taken = 0;
    for (; taken < count; ++taken) {
        if (browser->streams_count >= FILE_BROWSER_STREAMS_MAX)
            break;
        size_t index = indices[taken];
        const char *path = index < listing->entry_count
                               ? listing->entries[index].path
                               : NULL;
        if (!path || strncmp(path, "jf:item:", 8) != 0)
            continue;
        const char *id = path + 8;
        size_t length = strcspn(id, ":");
        if (length == 0 || length >= JF_ID_SIZE)
            continue;
        size_t slot = browser->streams_count++;
        browser->streams_index[slot] = index;
        memcpy(browser->streams_id[slot], id, length);
        browser->streams_id[slot][length] = '\0';
    }
    if (browser->streams_count)
        pthread_cond_signal(&browser->changed);
    pthread_mutex_unlock(&browser->mutex);
    return taken;
}

bool file_browser_poll_streams(struct file_browser *browser,
                               struct file_browser_streams *streams)
{
    if (!browser || !streams)
        return false;
    pthread_mutex_lock(&browser->mutex);
    bool ready = browser->has_streams;
    if (ready) {
        *streams = browser->streams_ready;
        browser->has_streams = false;
    }
    pthread_mutex_unlock(&browser->mutex);
    return ready;
}

int file_browser_request(struct file_browser *browser, const char *path,
                         char *error, size_t error_size)
{
    if (!browser || !path || !path[0]) {
        set_error(error, error_size, "invalid directory path");
        return -1;
    }
    char *copy = strdup(path);
    if (!copy) {
        set_error(error, error_size, "could not copy directory path");
        return -1;
    }
    pthread_mutex_lock(&browser->mutex);
    free(browser->request_path);
    browser->request_path = copy;
    ++browser->request_generation;
    pthread_cond_signal(&browser->changed);
    pthread_mutex_unlock(&browser->mutex);
    return 0;
}

int file_browser_open(struct file_browser **result, const char *initial_path,
                      char *error, size_t error_size)
{
    if (!result) {
        set_error(error, error_size, "invalid browser pointer");
        return -1;
    }
    *result = NULL;
    struct file_browser *browser = calloc(1, sizeof(*browser));
    if (!browser) {
        set_error(error, error_size, "could not create browser");
        return -1;
    }
    if (pthread_mutex_init(&browser->mutex, NULL) != 0) {
        set_error(error, error_size, "browser synchronization failed");
        free(browser);
        return -1;
    }
    if (pthread_cond_init(&browser->changed, NULL) != 0) {
        set_error(error, error_size, "browser synchronization failed");
        pthread_mutex_destroy(&browser->mutex);
        free(browser);
        return -1;
    }
    if (pthread_create(&browser->worker, NULL, browser_worker, browser) != 0) {
        set_error(error, error_size, "could not start browser thread");
        pthread_cond_destroy(&browser->changed);
        pthread_mutex_destroy(&browser->mutex);
        free(browser);
        return -1;
    }
    browser->worker_started = true;
    *result = browser;
    if (file_browser_request(browser, initial_path, error, error_size) < 0) {
        file_browser_close(result);
        return -1;
    }
    return 0;
}

int file_browser_poll(struct file_browser *browser,
                      struct file_browser_listing *listing)
{
    if (!browser || !listing)
        return -1;
    pthread_mutex_lock(&browser->mutex);
    if (!browser->has_ready) {
        pthread_mutex_unlock(&browser->mutex);
        return 0;
    }
    *listing = browser->ready;
    memset(&browser->ready, 0, sizeof(browser->ready));
    browser->has_ready = false;
    pthread_mutex_unlock(&browser->mutex);
    return 1;
}

void file_browser_close(struct file_browser **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct file_browser *browser = *pointer;
    pthread_mutex_lock(&browser->mutex);
    browser->stopping = true;
    pthread_cond_broadcast(&browser->changed);
    pthread_mutex_unlock(&browser->mutex);
    if (browser->worker_started)
        pthread_join(browser->worker, NULL);
    free(browser->request_path);
    if (browser->has_ready)
        file_browser_listing_clear(&browser->ready);
    pthread_cond_destroy(&browser->changed);
    pthread_mutex_destroy(&browser->mutex);
    free(browser);
    *pointer = NULL;
}
