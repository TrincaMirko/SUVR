#ifndef STEREONAUT_WEBDAV_H
#define STEREONAUT_WEBDAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Saved WebDAV servers (~/.config/stereonaut/webdav.json) and PROPFIND
 * directory listing over libcurl. Files play through the existing http(s)
 * pipeline; wd_authorization_header() hands Basic credentials to
 * libavformat via STEREONAUT_HTTP_HEADERS, never through argv. */

#define WD_MAX_SERVERS 16

struct wd_server {
    char name[64];
    char url[512]; /* base URL, e.g. https://host:8443/dav (no trailing / needed) */
    char user[128];
    char pass[256];
};

/* Fills `out` with the saved servers (up to `max`); returns the count. */
int wd_servers_load(struct wd_server *out, int max);
/* Appends `server` and persists. 0 on success, <0 on error. */
int wd_server_add(const struct wd_server *server, char *error, size_t error_size);
/* Removes the server at `index` and persists. 0 on success. */
int wd_server_remove(int index, char *error, size_t error_size);
/* Copies the server at `index` into `out`. 0 on success, <0 if out of range. */
int wd_server_get(int index, struct wd_server *out);

/* "Authorization: Basic <base64(user:pass)>" into `out`. Empty if no user. */
void wd_authorization_header(const struct wd_server *server, char *out, size_t size);
/* scheme://host[:port] of `server->url` into `out`. */
void wd_origin(const struct wd_server *server, char *out, size_t size);
/* Server-root-relative path of `server->url` ("/" if none), trailing slash
 * kept, into `out`. */
void wd_base_path(const struct wd_server *server, char *out, size_t size);

struct wd_entry {
    char name[256];  /* display name (URL-decoded last path segment) */
    char path[1024]; /* server-root-relative path, e.g. /dav/movies/ */
    bool is_dir;
    int64_t size; /* bytes, 0 if unknown */
};

/* PROPFIND Depth:1 of `dir_path` (server-root-relative). Returns a malloc'd
 * array in *out (caller frees) and *count, skipping the directory itself.
 * 0 on success, <0 on error with a message in `error`. */
int wd_list(const struct wd_server *server, const char *dir_path,
            struct wd_entry **out, size_t *count, char *error, size_t error_size);

#endif
