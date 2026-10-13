#ifndef STEREONAUT_LOCAL_THUMBNAIL_H
#define STEREONAUT_LOCAL_THUMBNAIL_H

#include <stddef.h>

/* Tile picture of a local video as JPEG: a picture next to it the way
 * Jellyfin and Kodi store them ("NAME-poster.jpg", "NAME.jpg",
 * "NAME-thumb.jpg", "NAME-fanart.jpg"), else a key frame at about 10 % of
 * the title decoded in software (only 4:2:0 in 8 or 10 bit). Slow for 8K
 * (seconds): run it on a worker and cache the result. *data is malloc'd. */
int local_thumbnail_jpeg(const char *path, int max_width, char **data,
                         size_t *size);

/* Same key frame, but from an http(s) URL (no sidecar lookup). `headers`,
 * if set, are extra request headers (e.g. "Authorization: Basic ...\r\n")
 * passed to libavformat for WebDAV Basic auth. */
int local_thumbnail_url_jpeg(const char *url, const char *headers,
                             int max_width, char **data, size_t *size);

#endif
