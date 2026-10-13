#define _POSIX_C_SOURCE 200809L
#include "webdav.h"

#include <curl/curl.h>
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* ponytail: PROPFIND XML is scanned, not DOM-parsed. One <href> per
 * <response>; each entry's segment runs to the next href. Namespaces vary
 * (D:, d:, lp1:) so local names are matched case-insensitively. A full XML
 * parser would be a new dependency for no gain on this well-shaped reply. */

static void set_error(char *error, size_t size, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
}

/* ---- configuration ------------------------------------------------- */

static int config_path(char *path, size_t size, bool directory_only)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    char fallback[512];
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return -1;
        snprintf(fallback, sizeof(fallback), "%s/.config", home);
        base = fallback;
    }
    int written = snprintf(path, size, "%s/stereonaut%s", base,
                           directory_only ? "" : "/webdav.json");
    return written > 0 && (size_t)written < size ? 0 : -1;
}

static const char *json_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !value ||
        !json_object_is_type(value, json_type_string))
        return NULL;
    return json_object_get_string(value);
}

static void copy_string(char *target, size_t size, const char *source)
{
    snprintf(target, size, "%.*s", size ? (int)(size - 1) : 0,
             source ? source : "");
}

int wd_servers_load(struct wd_server *out, int max)
{
    char path[640];
    if (config_path(path, sizeof(path), false) < 0)
        return 0;
    json_object *root = json_object_from_file(path);
    if (!root)
        return 0;
    int count = 0;
    if (json_object_is_type(root, json_type_array)) {
        int n = (int)json_object_array_length(root);
        for (int i = 0; i < n && count < max; ++i) {
            json_object *item = json_object_array_get_idx(root, i);
            if (!item || !json_object_is_type(item, json_type_object))
                continue;
            struct wd_server *s = &out[count];
            memset(s, 0, sizeof(*s));
            copy_string(s->name, sizeof(s->name), json_string(item, "name"));
            copy_string(s->url, sizeof(s->url), json_string(item, "url"));
            copy_string(s->user, sizeof(s->user), json_string(item, "user"));
            copy_string(s->pass, sizeof(s->pass), json_string(item, "password"));
            if (!s->url[0])
                continue;
            if (!s->name[0])
                copy_string(s->name, sizeof(s->name), s->url);
            ++count;
        }
    }
    json_object_put(root);
    return count;
}

static int servers_save(const struct wd_server *servers, int count,
                        char *error, size_t error_size)
{
    char directory[640], path[640];
    if (config_path(directory, sizeof(directory), true) < 0 ||
        config_path(path, sizeof(path), false) < 0) {
        set_error(error, error_size, "No configuration directory");
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        set_error(error, error_size, "Configuration is not writable");
        return -1;
    }
    json_object *array = json_object_new_array();
    if (!array) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        json_object *item = json_object_new_object();
        if (!item) {
            json_object_put(array);
            set_error(error, error_size, "Out of memory");
            return -1;
        }
        json_object_object_add(item, "name", json_object_new_string(servers[i].name));
        json_object_object_add(item, "url", json_object_new_string(servers[i].url));
        json_object_object_add(item, "user", json_object_new_string(servers[i].user));
        json_object_object_add(item, "password", json_object_new_string(servers[i].pass));
        json_object_array_add(array, item);
    }
    const char *text = json_object_to_json_string_ext(array, JSON_C_TO_STRING_PRETTY);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && text &&
             write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    if (fd >= 0)
        close(fd);
    json_object_put(array);
    if (!ok) {
        set_error(error, error_size, "Configuration is not writable");
        return -1;
    }
    return 0;
}

int wd_server_add(const struct wd_server *server, char *error, size_t error_size)
{
    if (!server->url[0]) {
        set_error(error, error_size, "Server address is required");
        return -1;
    }
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (count >= WD_MAX_SERVERS) {
        set_error(error, error_size, "Too many saved servers");
        return -1;
    }
    servers[count] = *server;
    /* A bare host like "192.168.1.102:8080" has no scheme; libcurl needs
     * one, so default to http. */
    if (!strstr(servers[count].url, "://")) {
        char full[sizeof(servers[count].url)];
        snprintf(full, sizeof(full), "http://%.*s",
                 (int)(sizeof(full) - 8), server->url);
        copy_string(servers[count].url, sizeof(servers[count].url), full);
    }
    if (!servers[count].name[0])
        copy_string(servers[count].name, sizeof(servers[count].name), server->url);
    return servers_save(servers, count + 1, error, error_size);
}

int wd_server_remove(int index, char *error, size_t error_size)
{
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (index < 0 || index >= count) {
        set_error(error, error_size, "No such server");
        return -1;
    }
    for (int i = index; i + 1 < count; ++i)
        servers[i] = servers[i + 1];
    return servers_save(servers, count - 1, error, error_size);
}

int wd_server_get(int index, struct wd_server *out)
{
    struct wd_server servers[WD_MAX_SERVERS];
    int count = wd_servers_load(servers, WD_MAX_SERVERS);
    if (index < 0 || index >= count)
        return -1;
    *out = servers[index];
    return 0;
}

/* ---- URL helpers --------------------------------------------------- */

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t len, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = in[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) v |= in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        char quad[4] = {
            base64_chars[(v >> 18) & 0x3F],
            base64_chars[(v >> 12) & 0x3F],
            rem > 1 ? base64_chars[(v >> 6) & 0x3F] : '=',
            rem > 2 ? base64_chars[v & 0x3F] : '=',
        };
        for (int k = 0; k < 4 && o + 1 < size; ++k)
            out[o++] = quad[k];
    }
    if (o < size)
        out[o] = '\0';
    else if (size)
        out[size - 1] = '\0';
}

void wd_authorization_header(const struct wd_server *server, char *out, size_t size)
{
    out[0] = '\0';
    if (!server->user[0])
        return;
    char pair[sizeof(server->user) + sizeof(server->pass) + 1];
    int len = snprintf(pair, sizeof(pair), "%s:%s", server->user, server->pass);
    if (len <= 0)
        return;
    char encoded[((sizeof(pair) + 2) / 3) * 4 + 1];
    base64_encode((const unsigned char *)pair, (size_t)len, encoded, sizeof(encoded));
    snprintf(out, size, "Authorization: Basic %s", encoded);
}

/* Copies scheme://host[:port] (everything before the first path '/'). */
void wd_origin(const struct wd_server *server, char *out, size_t size)
{
    const char *url = server->url;
    const char *after_scheme = strstr(url, "://");
    const char *host = after_scheme ? after_scheme + 3 : url;
    const char *slash = strchr(host, '/');
    size_t length = slash ? (size_t)(slash - url) : strlen(url);
    snprintf(out, size, "%.*s", (int)length, url);
}

void wd_base_path(const struct wd_server *server, char *out, size_t size)
{
    const char *url = server->url;
    const char *after_scheme = strstr(url, "://");
    const char *host = after_scheme ? after_scheme + 3 : url;
    const char *slash = strchr(host, '/');
    if (!slash || !slash[0]) {
        snprintf(out, size, "/");
        return;
    }
    size_t length = strlen(slash);
    if (length && slash[length - 1] == '/')
        snprintf(out, size, "%s", slash);
    else
        snprintf(out, size, "%s/", slash);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place-safe URL decode of `in` into `out`. */
static void url_decode(const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < size; ++i) {
        if (in[i] == '%' && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            out[o++] = (char)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* ---- PROPFIND ------------------------------------------------------ */

struct buffer {
    char *data;
    size_t size;
};

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *user)
{
    struct buffer *buffer = user;
    size_t total = size * nmemb;
    char *grown = realloc(buffer->data, buffer->size + total + 1);
    if (!grown)
        return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, ptr, total);
    buffer->size += total;
    buffer->data[buffer->size] = '\0';
    return total;
}

/* Local-name match: finds "needle>" where needle may carry a namespace
 * prefix ("...:needle>"). Returns the position just past '>' or NULL. */
static const char *find_tag_open(const char *from, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = from; (p = strchr(p, '<')); ++p) {
        const char *q = p + 1;
        if (*q == '/')
            continue;
        const char *colon = NULL;
        const char *r = q;
        while (*r && *r != '>' && *r != ' ' && *r != '/') {
            if (*r == ':')
                colon = r;
            ++r;
        }
        const char *local = colon ? colon + 1 : q;
        if ((size_t)(r - local) == n && strncasecmp(local, needle, n) == 0) {
            const char *close = strchr(r, '>');
            return close ? close + 1 : NULL;
        }
    }
    return NULL;
}

/* strcasestr is a GNU extension; a small portable version. */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return haystack;
    for (; *haystack; ++haystack)
        if (strncasecmp(haystack, needle, n) == 0)
            return haystack;
    return NULL;
}

static bool is_media_href(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mp4") == 0);
}

static void last_segment(const char *path, char *out, size_t size)
{
    char decoded[1024];
    url_decode(path, decoded, sizeof(decoded));
    size_t len = strlen(decoded);
    while (len > 1 && decoded[len - 1] == '/')
        decoded[--len] = '\0';
    const char *slash = strrchr(decoded, '/');
    snprintf(out, size, "%s", slash ? slash + 1 : decoded);
}

int wd_list(const struct wd_server *server, const char *dir_path,
            struct wd_entry **out, size_t *count, char *error, size_t error_size)
{
    *out = NULL;
    *count = 0;
    char origin[512];
    wd_origin(server, origin, sizeof(origin));
    char url[1600];
    snprintf(url, sizeof(url), "%s%s", origin, dir_path);

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    struct buffer response = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml");
    static const char *propfind_body =
        "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, propfind_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Stereonaut");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (server->user[0]) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERNAME, server->user);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, server->pass);
    }
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        set_error(error, error_size, curl_easy_strerror(result));
        free(response.data);
        return -1;
    }
    if (status == 401) {
        set_error(error, error_size, "Login rejected by server");
        free(response.data);
        return -1;
    }
    if (status < 200 || status >= 300) {
        char message[64];
        snprintf(message, sizeof(message), "WebDAV error (HTTP %ld)", status);
        set_error(error, error_size, message);
        free(response.data);
        return -1;
    }
    if (!response.data) {
        set_error(error, error_size, "Empty response from server");
        return -1;
    }

    /* Decoded requested path, to skip the collection's own entry. */
    char self[1024];
    url_decode(dir_path, self, sizeof(self));
    size_t self_len = strlen(self);
    while (self_len > 1 && self[self_len - 1] == '/')
        self[--self_len] = '\0';

    size_t capacity = 0;
    struct wd_entry *entries = NULL;
    const char *cursor = response.data;
    for (;;) {
        const char *href_val = find_tag_open(cursor, "href");
        if (!href_val)
            break;
        const char *href_end = strchr(href_val, '<');
        if (!href_end)
            break;
        /* Segment of this response runs to the next href (or end). */
        const char *next = find_tag_open(href_end, "href");
        const char *seg_end = next ? next : response.data + response.size;
        cursor = href_end;

        char raw[1024];
        snprintf(raw, sizeof(raw), "%.*s", (int)(href_end - href_val), href_val);
        /* Strip an absolute origin, keep the server-root-relative path. */
        const char *rel = raw;
        const char *scheme = strstr(raw, "://");
        if (scheme) {
            const char *host = scheme + 3;
            const char *slash = strchr(host, '/');
            rel = slash ? slash : "/";
        }
        char decoded[1024];
        url_decode(rel, decoded, sizeof(decoded));
        size_t dlen = strlen(decoded);
        char trimmed[1024];
        snprintf(trimmed, sizeof(trimmed), "%s", decoded);
        while (dlen > 1 && trimmed[dlen - 1] == '/')
            trimmed[--dlen] = '\0';
        if (strcmp(trimmed, self) == 0)
            continue; /* the directory itself */

        bool is_dir = false;
        {
            char segment[8192];
            size_t seg_len = (size_t)(seg_end - href_end);
            if (seg_len >= sizeof(segment))
                seg_len = sizeof(segment) - 1;
            memcpy(segment, href_end, seg_len);
            segment[seg_len] = '\0';
            is_dir = ci_strstr(segment, "collection") != NULL;
        }

        char name[256];
        last_segment(rel, name, sizeof(name));
        if (!name[0])
            continue;
        if (!is_dir && !is_media_href(name))
            continue;

        if (*count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            struct wd_entry *more = realloc(entries, grown * sizeof(*more));
            if (!more) {
                free(entries);
                free(response.data);
                set_error(error, error_size, "Out of memory");
                return -1;
            }
            entries = more;
            capacity = grown;
        }
        struct wd_entry *e = &entries[*count];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);
        /* Keep the (encoded) relative path for building child URLs. */
        snprintf(e->path, sizeof(e->path), "%s", rel);
        e->is_dir = is_dir;
        ++*count;
    }
    free(response.data);
    *out = entries;
    return 0;
}
