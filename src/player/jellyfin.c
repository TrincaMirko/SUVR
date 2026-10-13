#define _POSIX_C_SOURCE 200809L
#include "jellyfin.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "decoded-video.h"

#define RESPONSE_LIMIT (48U * 1024U * 1024U)
#define LIST_LIMIT 5000

/* Direct Play HEVC Main 8 bit (plus Main10 with the patched Turnip, see
 * STEREONAUT_TP10) and 8-bit 4:2:0 H.264 (Baseline, Main, High) in mkv/mp4,
 * plus 8-bit AV1 up to 4K if software decoding is enabled
 * (decoded-video.h); everything else becomes an HLS transcode to HEVC.
 * The codec list, the HEVC profiles and bit depth and the AV1 profile are
 * filled in by jf_resolve_playback(). */
static const char DEVICE_PROFILE_FORMAT[] =
    "{\"Name\":\"Stereonaut\",\"MaxStreamingBitrate\":200000000,"
    "\"DirectPlayProfiles\":[{\"Type\":\"Video\",\"Container\":\"mkv,mp4\","
    "\"VideoCodec\":\"%s\",\"AudioCodec\":\"aac,ac3,eac3,mp3,opus\"}],"
    "\"TranscodingProfiles\":[{\"Type\":\"Video\",\"Container\":\"ts\","
    "\"Protocol\":\"hls\",\"VideoCodec\":\"hevc\",\"AudioCodec\":\"aac\","
    "\"Context\":\"Streaming\",\"MaxAudioChannels\":\"2\","
    "\"BreakOnNonKeyFrames\":true}],"
    "\"CodecProfiles\":[{\"Type\":\"Video\",\"Codec\":\"hevc\","
    "\"Conditions\":[{\"Condition\":\"EqualsAny\","
    "\"Property\":\"VideoProfile\",\"Value\":\"%s\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\","
    "\"Value\":\"%s\",\"IsRequired\":true}]},"
    "{\"Type\":\"Video\",\"Codec\":\"h264\","
    "\"Conditions\":[{\"Condition\":\"EqualsAny\","
    "\"Property\":\"VideoProfile\","
    "\"Value\":\"high|main|baseline|constrained baseline\","
    "\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\","
    "\"Value\":\"8\",\"IsRequired\":true}]}%s]}";

static const char AV1_CODEC_PROFILE_FORMAT[] =
    ",{\"Type\":\"Video\",\"Codec\":\"av1\",\"Conditions\":["
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Width\","
    "\"Value\":\"%d\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"Height\","
    "\"Value\":\"%d\",\"IsRequired\":true},"
    "{\"Condition\":\"LessThanEqual\",\"Property\":\"VideoBitDepth\","
    "\"Value\":\"%d\",\"IsRequired\":true}]}";

struct buffer {
    char *data;
    size_t size;
};

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

static void curl_setup(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void copy_string(char *target, size_t size, const char *source)
{
    snprintf(target, size, "%s", source ? source : "");
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
                           directory_only ? "" : "/jellyfin.json");
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

static void ensure_device_id(struct jf_config *config);
static int config_save(const struct jf_config *config, char *error,
                       size_t error_size);

int jf_config_load(struct jf_config *config, char *error, size_t error_size)
{
    memset(config, 0, sizeof(*config));
    char path[640];
    if (config_path(path, sizeof(path), false) < 0) {
        set_error(error, error_size, "No configuration directory");
        return -1;
    }
    json_object *root = json_object_from_file(path);
    if (!root) {
        set_error(error, error_size, "Jellyfin is not set up");
        return -1;
    }
    copy_string(config->server, sizeof(config->server),
                json_string(root, "server"));
    copy_string(config->device_id, sizeof(config->device_id),
                json_string(root, "device_id"));
    copy_string(config->user_id, sizeof(config->user_id),
                json_string(root, "user_id"));
    copy_string(config->token, sizeof(config->token),
                json_string(root, "token"));
    json_object_put(root);
    size_t length = strlen(config->server);
    while (length && config->server[length - 1] == '/')
        config->server[--length] = '\0';
    if (!config->server[0]) {
        set_error(error, error_size, "Jellyfin server missing from configuration");
        return -1;
    }
    if (!config->device_id[0]) {
        /* Persist the id: Quick Connect start and poll run in separate
         * requests that each reload the configuration. */
        ensure_device_id(config);
        config_save(config, NULL, 0);
    }
    return 0;
}

bool jf_config_has_token(const struct jf_config *config)
{
    return config->token[0] && config->user_id[0] && config->device_id[0];
}

static int config_save(const struct jf_config *config, char *error,
                       size_t error_size)
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
    json_object *root = json_object_new_object();
    json_object_object_add(root, "server",
                           json_object_new_string(config->server));
    json_object_object_add(root, "device_id",
                           json_object_new_string(config->device_id));
    json_object_object_add(root, "user_id",
                           json_object_new_string(config->user_id));
    json_object_object_add(root, "token",
                           json_object_new_string(config->token));
    const char *text = json_object_to_json_string(root);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int result = -1;
    if (descriptor >= 0) {
        size_t length = strlen(text);
        result = write(descriptor, text, length) == (ssize_t)length ? 0 : -1;
        close(descriptor);
    }
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Configuration is not writable");
    return result;
}

static void ensure_device_id(struct jf_config *config)
{
    if (config->device_id[0])
        return;
    unsigned char random[8] = {0};
    int descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor >= 0) {
        if (read(descriptor, random, sizeof(random)) < 0)
            memset(random, 0, sizeof(random));
        close(descriptor);
    }
    snprintf(config->device_id, sizeof(config->device_id),
             "stereonaut-%02x%02x%02x%02x%02x%02x%02x%02x", random[0],
             random[1], random[2], random[3], random[4], random[5],
             random[6], random[7]);
}

void jf_authorization_header(const struct jf_config *config, char *out,
                             size_t out_size)
{
    if (config->token[0])
        snprintf(out, out_size,
                 "Authorization: MediaBrowser Client=\"Stereonaut\", "
                 "Device=\"Steam Frame\", DeviceId=\"%s\", Version=\"0.1\", "
                 "Token=\"%s\"",
                 config->device_id, config->token);
    else
        snprintf(out, out_size,
                 "Authorization: MediaBrowser Client=\"Stereonaut\", "
                 "Device=\"Steam Frame\", DeviceId=\"%s\", Version=\"0.1\"",
                 config->device_id);
}

/* ---- HTTP ---------------------------------------------------------- */

static size_t write_body(char *data, size_t size, size_t count, void *user)
{
    struct buffer *buffer = user;
    size_t length = size * count;
    if (buffer->size + length > RESPONSE_LIMIT)
        return 0;
    char *grown = realloc(buffer->data, buffer->size + length + 1U);
    if (!grown)
        return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return length;
}

/* Returns the HTTP status (>0) or -1 on a transport failure. The body is
 * always NUL-terminated and owned by the caller (free(body->data)). */
static long http_request_timed(const struct jf_config *config,
                               const char *method, const char *path,
                               const char *body, struct buffer *response,
                               long timeout_seconds)
{
    pthread_once(&curl_once, curl_setup);
    memset(response, 0, sizeof(*response));
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;
    char url[2304], header[1024];
    snprintf(url, sizeof(url), "%s%s", config->server, path);
    jf_authorization_header(config, header, sizeof(header));
    struct curl_slist *headers = curl_slist_append(NULL, header);
    if (body)
        headers = curl_slist_append(headers,
                                    "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     timeout_seconds < 10L ? timeout_seconds : 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Stereonaut");
    if (body)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    CURLcode result = curl_easy_perform(curl);
    long status = -1;
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

static long http_request(const struct jf_config *config, const char *method,
                         const char *path, const char *body,
                         struct buffer *response)
{
    return http_request_timed(config, method, path, body, response, 30L);
}

/* Parses a successful JSON response. 401 maps to JF_AUTH_RETRY. */
static int json_call_timed(const struct jf_config *config, const char *method,
                           const char *path, const char *body,
                           json_object **root, char *error,
                           size_t error_size, long timeout_seconds)
{
    struct buffer response;
    long status = http_request_timed(config, method, path, body, &response,
                                     timeout_seconds);
    int result = 0;
    if (status < 0) {
        set_error(error, error_size, "Jellyfin server not reachable");
        result = -1;
    } else if (status == 401 || status == 403) {
        set_error(error, error_size, "Jellyfin login expired");
        result = JF_AUTH_RETRY;
    } else if (status < 200 || status >= 300) {
        set_error(error, error_size, "Jellyfin error (HTTP %ld)", status);
        result = -1;
    } else {
        *root = response.data ? json_tokener_parse(response.data) : NULL;
        if (!*root) {
            set_error(error, error_size, "Invalid response from server");
            result = -1;
        }
    }
    free(response.data);
    return result;
}

static int json_call(const struct jf_config *config, const char *method,
                     const char *path, const char *body, json_object **root,
                     char *error, size_t error_size)
{
    return json_call_timed(config, method, path, body, root, error,
                           error_size, 30L);
}

/* ---- Quick Connect -------------------------------------------------- */

int jf_quickconnect_start(struct jf_config *config, char code[16],
                          char secret[128], char *error, size_t error_size)
{
    ensure_device_id(config);
    json_object *root = NULL;
    int result = json_call(config, "POST", "/QuickConnect/Initiate", "{}",
                           &root, error, error_size);
    if (result < 0)
        return result == JF_AUTH_RETRY ? -1 : result;
    const char *code_text = json_string(root, "Code");
    const char *secret_text = json_string(root, "Secret");
    if (!code_text || !secret_text) {
        set_error(error, error_size, "Quick Connect is disabled on the server");
        json_object_put(root);
        return -1;
    }
    copy_string(code, 16, code_text);
    copy_string(secret, 128, secret_text);
    json_object_put(root);
    return 0;
}

int jf_quickconnect_poll(struct jf_config *config, const char *secret,
                         bool *authenticated, char *error, size_t error_size)
{
    *authenticated = false;
    char path[256];
    snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", secret);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result == JF_AUTH_RETRY ? -1 : result;
    json_object *flag = NULL;
    bool done = json_object_object_get_ex(root, "Authenticated", &flag) &&
                json_object_get_boolean(flag);
    json_object_put(root);
    if (!done)
        return 0;
    char body[256];
    snprintf(body, sizeof(body), "{\"Secret\":\"%s\"}", secret);
    result = json_call(config, "POST", "/Users/AuthenticateWithQuickConnect",
                       body, &root, error, error_size);
    if (result < 0)
        return result == JF_AUTH_RETRY ? -1 : result;
    const char *token = json_string(root, "AccessToken");
    json_object *user = NULL;
    const char *user_id = NULL;
    if (json_object_object_get_ex(root, "User", &user))
        user_id = json_string(user, "Id");
    if (!token || !user_id) {
        set_error(error, error_size, "Login rejected by server");
        json_object_put(root);
        return -1;
    }
    copy_string(config->token, sizeof(config->token), token);
    copy_string(config->user_id, sizeof(config->user_id), user_id);
    json_object_put(root);
    if (config_save(config, error, error_size) < 0)
        return -1;
    *authenticated = true;
    return 0;
}

/* ---- browsing -------------------------------------------------------- */

static int compare_items(const void *left, const void *right)
{
    const struct jf_item *a = left, *b = right;
    if (a->is_folder != b->is_folder)
        return a->is_folder ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

/* Server-side order with a name tie-break; folders are moved to the front
 * afterwards (stable), so the chosen order holds within each group. */
static void folders_first(struct jf_item *list, size_t count)
{
    struct jf_item *ordered = malloc((count ? count : 1U) * sizeof(*ordered));
    if (!ordered)
        return;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
        if (list[i].is_folder)
            ordered[used++] = list[i];
    for (size_t i = 0; i < count; ++i)
        if (!list[i].is_folder)
            ordered[used++] = list[i];
    memcpy(list, ordered, count * sizeof(*list));
    free(ordered);
}

/* Codec, profile and size of the first video stream (needs
 * Fields=MediaStreams in the request). */
static void describe_video(json_object *entry, struct jf_item *item)
{
    json_object *streams = NULL;
    if (!json_object_object_get_ex(entry, "MediaStreams", &streams) ||
        !json_object_is_type(streams, json_type_array))
        return;
    for (size_t i = 0; i < json_object_array_length(streams); ++i) {
        json_object *stream = json_object_array_get_idx(streams, i);
        const char *type = json_string(stream, "Type");
        if (!type || strcmp(type, "Video") != 0)
            continue;
        const char *codec = json_string(stream, "Codec");
        const char *profile = json_string(stream, "Profile");
        json_object *value = NULL;
        char name[16] = "";
        if (codec) {
            if (strcmp(codec, "hevc") == 0)
                snprintf(name, sizeof(name), "HEVC");
            else if (strcmp(codec, "h264") == 0)
                snprintf(name, sizeof(name), "H.264");
            else if (strcmp(codec, "mpeg4") == 0)
                snprintf(name, sizeof(name), "MPEG-4");
            else
                for (size_t c = 0; codec[c] && c + 1U < sizeof(name); ++c)
                    name[c] = (char)(codec[c] >= 'a' && codec[c] <= 'z'
                                         ? codec[c] - 32
                                         : codec[c]);
        }
        int depth = 0;
        if (json_object_object_get_ex(stream, "BitDepth", &value))
            depth = json_object_get_int(value);
        snprintf(item->codec, sizeof(item->codec), "%s%s%.12s", name,
                 profile && profile[0] ? " " : "",
                 profile && profile[0] ? profile : "");
        if (depth >= 10 && !strstr(item->codec, "10")) {
            size_t used = strlen(item->codec);
            snprintf(item->codec + used, sizeof(item->codec) - used,
                     " (%d Bit)", depth);
        }
        if (json_object_object_get_ex(stream, "Width", &value))
            item->width = json_object_get_int(value);
        if (json_object_object_get_ex(stream, "Height", &value))
            item->height = json_object_get_int(value);
        return;
    }
}

static int collect_items(json_object *root, bool filter_views,
                         struct jf_item **items, size_t *count)
{
    json_object *array = NULL;
    *items = NULL;
    *count = 0;
    if (!json_object_object_get_ex(root, "Items", &array) ||
        !json_object_is_type(array, json_type_array))
        return 0;
    size_t length = json_object_array_length(array);
    struct jf_item *list = calloc(length ? length : 1U, sizeof(*list));
    if (!list)
        return -1;
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        json_object *entry = json_object_array_get_idx(array, i);
        const char *id = json_string(entry, "Id");
        const char *name = json_string(entry, "Name");
        if (!id || !name)
            continue;
        if (filter_views) {
            const char *type = json_string(entry, "CollectionType");
            if (type && (strcmp(type, "music") == 0 ||
                         strcmp(type, "books") == 0 ||
                         strcmp(type, "photos") == 0 ||
                         strcmp(type, "playlists") == 0 ||
                         strcmp(type, "livetv") == 0))
                continue;
        }
        json_object *folder = NULL, *value = NULL, *tags = NULL;
        copy_string(list[used].id, sizeof(list[used].id), id);
        const char *primary = NULL, *backdrop = NULL;
        if (json_object_object_get_ex(entry, "ImageTags", &tags))
            primary = json_string(tags, "Primary");
        if (json_object_object_get_ex(entry, "BackdropImageTags", &tags) &&
            json_object_is_type(tags, json_type_array) &&
            json_object_array_length(tags) > 0 &&
            json_object_is_type(json_object_array_get_idx(tags, 0),
                                json_type_string))
            backdrop = json_object_get_string(
                json_object_array_get_idx(tags, 0));
        if (backdrop && primary)
            snprintf(list[used].image, sizeof(list[used].image),
                     "Backdrop/0?tag=%.48s|Primary?tag=%.48s", backdrop,
                     primary);
        else if (backdrop)
            snprintf(list[used].image, sizeof(list[used].image),
                     "Backdrop/0?tag=%.48s", backdrop);
        else if (primary)
            snprintf(list[used].image, sizeof(list[used].image),
                     "Primary?tag=%.48s", primary);
        if (json_object_object_get_ex(entry, "RunTimeTicks", &value))
            list[used].runtime_ticks = json_object_get_int64(value);
        if (json_object_object_get_ex(entry, "ProductionYear", &value))
            list[used].year = json_object_get_int(value);
        describe_video(entry, &list[used]);
        json_object *user_data = NULL;
        if (json_object_object_get_ex(entry, "UserData", &user_data) &&
            json_object_is_type(user_data, json_type_object)) {
            if (json_object_object_get_ex(user_data, "PlaybackPositionTicks",
                                          &value))
                list[used].resume_ticks = json_object_get_int64(value);
            if (json_object_object_get_ex(user_data, "PlayedPercentage",
                                          &value))
                list[used].played_percentage =
                    (float)json_object_get_double(value);
            if (json_object_object_get_ex(user_data, "Played", &value))
                list[used].played = json_object_get_boolean(value);
        }
        copy_string(list[used].name, sizeof(list[used].name), name);
        list[used].is_folder =
            filter_views ||
            (json_object_object_get_ex(entry, "IsFolder", &folder) &&
             json_object_get_boolean(folder));
        ++used;
    }
    if (filter_views)
        qsort(list, used, sizeof(*list), compare_items);
    else
        folders_first(list, used);
    *items = list;
    *count = used;
    return 0;
}

int jf_list_views(const struct jf_config *config, struct jf_item **items,
                  size_t *count, char *error, size_t error_size)
{
    char path[256];
    snprintf(path, sizeof(path), "/UserViews?userId=%s", config->user_id);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result;
    result = collect_items(root, true, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_list_children(const struct jf_config *config, const char *parent_id,
                     enum jf_filter filter, const char *filter_id,
                     enum jf_sort sort, bool descending,
                     struct jf_item **items, size_t *count, char *error,
                     size_t error_size)
{
    static const char *const filter_fields[JF_FILTER_COUNT] = {
        NULL, "GenreIds", "StudioIds", "PersonIds",
    };
    char filter_text[160] = "";
    if (filter > JF_FILTER_NONE && filter < JF_FILTER_COUNT && filter_id &&
        filter_id[0])
        snprintf(filter_text, sizeof(filter_text),
                 "&Recursive=true&IncludeItemTypes=Movie,Video,Episode&%s=%.48s",
                 filter_fields[filter], filter_id);
    static const char *const fields[JF_SORT_COUNT] = {
        "SortName", "Random", "CommunityRating,SortName",
        "CriticRating,SortName", "DateCreated,SortName",
        "DatePlayed,SortName", "OfficialRating,SortName",
        "PlayCount,SortName", "PremiereDate,SortName", "Runtime,SortName",
    };
    if ((unsigned)sort >= JF_SORT_COUNT)
        sort = JF_SORT_NAME;
    char path[512];
    snprintf(path, sizeof(path),
             "/Items?userId=%s&ParentId=%s&SortBy=%s&"
             "SortOrder=%s&Limit=%d&EnableTotalRecordCount=false%s",
             config->user_id, parent_id, fields[sort],
             descending ? "Descending" : "Ascending", LIST_LIMIT,
             filter_text);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result;
    result = collect_items(root, false, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_list_filter_values(const struct jf_config *config,
                          const char *parent_id, enum jf_filter filter,
                          struct jf_item **items, size_t *count,
                          char *error, size_t error_size)
{
    static const char *const endpoints[JF_FILTER_COUNT] = {
        NULL, "/Genres", "/Studios", "/Persons",
    };
    *items = NULL;
    *count = 0;
    if (filter <= JF_FILTER_NONE || filter >= JF_FILTER_COUNT)
        return -1;
    char path[256];
    snprintf(path, sizeof(path),
             "%s?ParentId=%s&userId=%s&SortBy=SortName&Limit=%d&"
             "EnableTotalRecordCount=false",
             endpoints[filter], parent_id, config->user_id, LIST_LIMIT);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result;
    result = collect_items(root, false, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_search(const struct jf_config *config, const char *query,
              struct jf_item **items, size_t *count, char *error,
              size_t error_size)
{
    /* Percent-encode everything but unreserved characters (UTF-8 bytes
     * included). */
    char encoded[3 * 96 + 1];
    size_t used = 0;
    for (const unsigned char *c = (const unsigned char *)query;
         *c && used + 4 < sizeof(encoded); ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '-' || *c == '.' ||
            *c == '_' || *c == '~')
            encoded[used++] = (char)*c;
        else
            used += (size_t)snprintf(encoded + used, sizeof(encoded) - used,
                                     "%%%02X", *c);
    }
    encoded[used] = '\0';
    char path[512];
    snprintf(path, sizeof(path),
             "/Items?userId=%s&searchTerm=%s&Recursive=true&"
             "IncludeItemTypes=Movie,Video,Episode&Limit=200&"
             "EnableTotalRecordCount=false",
             config->user_id, encoded);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result;
    result = collect_items(root, false, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_list_streams(const struct jf_config *config, const char *ids,
                    struct jf_item **items, size_t *count, char *error,
                    size_t error_size)
{
    *items = NULL;
    *count = 0;
    size_t length = strlen(ids) + strlen(config->user_id) + 128;
    char *path = malloc(length);
    if (!path) {
        set_error(error, error_size, "Out of memory");
        return -1;
    }
    snprintf(path, length,
             "/Items?userId=%s&Ids=%s&Fields=MediaStreams&"
             "EnableTotalRecordCount=false",
             config->user_id, ids);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    free(path);
    if (result < 0)
        return result;
    result = collect_items(root, false, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_list_resume(const struct jf_config *config, struct jf_item **items,
                   size_t *count, char *error, size_t error_size)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/UserItems/Resume?userId=%s&MediaTypes=Video&Limit=12&"
             "Fields=MediaStreams&EnableTotalRecordCount=false",
             config->user_id);
    json_object *root = NULL;
    int result = json_call_timed(config, "GET", path, NULL, &root, error,
                                 error_size, 4L);
    if (result < 0)
        return result;
    result = collect_items(root, false, items, count);
    json_object_put(root);
    if (result < 0)
        set_error(error, error_size, "Out of memory");
    return result;
}

int jf_item_info(const struct jf_config *config, const char *id, char *name,
                 size_t name_size, char *parent, size_t parent_size,
                 char *error, size_t error_size)
{
    char path[256];
    snprintf(path, sizeof(path), "/Items/%s?userId=%s", id, config->user_id);
    json_object *root = NULL;
    int result = json_call(config, "GET", path, NULL, &root, error,
                           error_size);
    if (result < 0)
        return result;
    if (name)
        copy_string(name, name_size, json_string(root, "Name"));
    if (parent)
        copy_string(parent, parent_size, json_string(root, "ParentId"));
    json_object_put(root);
    return 0;
}

/* ---- playback -------------------------------------------------------- */

/* Copies a TranscodingUrl without the ApiKey parameter: the Authorization
 * header carries the credentials, so no secret ends up in a URL. */
static void strip_api_key(const char *source, char *target, size_t size)
{
    const char *query = strchr(source, '?');
    size_t base = query ? (size_t)(query - source) : strlen(source);
    size_t used = 0;
    for (size_t i = 0; i < base && used + 1U < size; ++i)
        target[used++] = source[i];
    bool first = true;
    if (query) {
        const char *cursor = query + 1;
        while (*cursor) {
            const char *end = strchr(cursor, '&');
            size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
            bool secret = (length >= 7 && strncmp(cursor, "ApiKey=", 7) == 0) ||
                          (length >= 8 && strncmp(cursor, "api_key=", 8) == 0);
            if (length && !secret && used + length + 2U < size) {
                target[used++] = first ? '?' : '&';
                memcpy(target + used, cursor, length);
                used += length;
                first = false;
            }
            cursor += length;
            if (*cursor == '&')
                ++cursor;
        }
    }
    target[used] = '\0';
}

int jf_resolve_playback(const struct jf_config *config, const char *item_id,
                        struct jf_playback *playback, char *error,
                        size_t error_size)
{
    memset(playback, 0, sizeof(*playback));
    char path[256], body[2304];
    snprintf(path, sizeof(path), "/Items/%s/PlaybackInfo?UserId=%s", item_id,
             config->user_id);
    /* A PC (no Iris) decodes Main10 and AV1 up to 10 bit on the GPU. */
    bool desktop = access(DECODED_VIDEO_IRIS_DEVICE, F_OK) < 0;
    bool main10 = desktop || getenv("STEREONAUT_TP10") != NULL;
    bool av1 = desktop || getenv(AV1_SOFTWARE_ENV) != NULL;
    char av1_profile[512] = "";
    char profile[2048];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    if (av1)
        snprintf(av1_profile, sizeof(av1_profile), AV1_CODEC_PROFILE_FORMAT,
                 desktop ? 8192 : AV1_SOFTWARE_MAX_WIDTH,
                 desktop ? 8192 : AV1_SOFTWARE_MAX_HEIGHT, desktop ? 10 : 8);
    snprintf(profile, sizeof(profile), DEVICE_PROFILE_FORMAT,
             av1 ? "hevc,h264,av1" : "hevc,h264",
             main10 ? "main|main 10" : "main", main10 ? "10" : "8",
             av1_profile);
#pragma GCC diagnostic pop
    snprintf(body, sizeof(body),
             "{\"DeviceProfile\":%s,\"AutoOpenLiveStream\":false}",
             profile);
    json_object *root = NULL;
    int result = json_call(config, "POST", path, body, &root, error,
                           error_size);
    if (result < 0)
        return result;
    json_object *sources = NULL, *source = NULL;
    if (!json_object_object_get_ex(root, "MediaSources", &sources) ||
        !json_object_is_type(sources, json_type_array) ||
        json_object_array_length(sources) == 0 ||
        !(source = json_object_array_get_idx(sources, 0))) {
        set_error(error, error_size, "No playback source from server");
        json_object_put(root);
        return -1;
    }
    json_object *streams = NULL;
    bool has_video = false;
    if (json_object_object_get_ex(source, "MediaStreams", &streams) &&
        json_object_is_type(streams, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(streams); ++i) {
            const char *type = json_string(
                json_object_array_get_idx(streams, i), "Type");
            if (type && strcmp(type, "Video") == 0)
                has_video = true;
        }
    }
    if (!has_video) {
        set_error(error, error_size, "Video file on the server is corrupt");
        json_object_put(root);
        return JF_BROKEN_ITEM;
    }
    json_object *flag = NULL;
    bool direct = json_object_object_get_ex(source, "SupportsDirectPlay",
                                            &flag) &&
                  json_object_get_boolean(flag);
    const char *source_id = json_string(source, "Id");
    const char *transcoding_url = json_string(source, "TranscodingUrl");
    if (direct && source_id) {
        snprintf(playback->url, sizeof(playback->url),
                 "%s/Videos/%s/stream?static=true&MediaSourceId=%s",
                 config->server, item_id, source_id);
    } else if (transcoding_url) {
        char stripped[1900];
        strip_api_key(transcoding_url, stripped, sizeof(stripped));
        int written = snprintf(playback->url, sizeof(playback->url), "%s%s",
                               config->server, stripped);
        if (written < 0 || (size_t)written >= sizeof(playback->url)) {
            set_error(error, error_size, "Stream address too long");
            json_object_put(root);
            return -1;
        }
        playback->transcoding = true;
    } else {
        set_error(error, error_size, "Title cannot be played");
        json_object_put(root);
        return -1;
    }
    copy_string(playback->item_id, sizeof(playback->item_id), item_id);
    copy_string(playback->media_source, sizeof(playback->media_source),
                source_id);
    copy_string(playback->play_session, sizeof(playback->play_session),
                json_string(root, "PlaySessionId"));
    const char *file = json_string(source, "Path");
    if (file && strrchr(file, '/'))
        file = strrchr(file, '/') + 1;
    copy_string(playback->file_name, sizeof(playback->file_name),
                file ? file : json_string(source, "Name"));
    copy_string(playback->video_3d_format, sizeof(playback->video_3d_format),
                json_string(source, "Video3DFormat"));
    json_object_put(root);
    return 0;
}

int jf_fetch_image(const struct jf_config *config, const char *item_id,
                   const char *image, int max_width, char **data,
                   size_t *size)
{
    *data = NULL;
    *size = 0;
    char path[256];
    snprintf(path, sizeof(path), "/Items/%s/Images/%s%cfillWidth=%d&quality=85",
             item_id, image, strchr(image, '?') ? '&' : '?', max_width);
    struct buffer response;
    long status = http_request(config, "GET", path, NULL, &response);
    if (status < 200 || status >= 300 || !response.size) {
        free(response.data);
        return -1;
    }
    *data = response.data;
    *size = response.size;
    return 0;
}

void jf_stop_encoding(const struct jf_config *config,
                      const struct jf_playback *playback)
{
    if (!playback->transcoding || !playback->play_session[0])
        return;
    char path[512];
    snprintf(path, sizeof(path),
             "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s",
             config->device_id, playback->play_session);
    struct buffer response;
    http_request(config, "DELETE", path, NULL, &response);
    free(response.data);
}

/* ---- playback reports ------------------------------------------------ */

int jf_report_playback(const struct jf_config *config,
                       const struct jf_playback *playback,
                       enum jf_report report, int64_t position_us,
                       bool paused, const char *event)
{
    static const char *const paths[] = {
        [JF_REPORT_START] = "/Sessions/Playing",
        [JF_REPORT_PROGRESS] = "/Sessions/Playing/Progress",
        [JF_REPORT_STOPPED] = "/Sessions/Playing/Stopped",
    };
    if ((unsigned)report > JF_REPORT_STOPPED || !playback->item_id[0])
        return -1;
    char body[768], event_field[64] = "";
    if (event && event[0])
        snprintf(event_field, sizeof(event_field), ",\"EventName\":\"%s\"",
                 event);
    snprintf(body, sizeof(body),
             "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\","
             "\"PlaySessionId\":\"%s\",\"PositionTicks\":%lld,"
             "\"IsPaused\":%s,\"CanSeek\":true,\"PlayMethod\":\"%s\"%s}",
             playback->item_id,
             playback->media_source[0] ? playback->media_source
                                       : playback->item_id,
             playback->play_session,
             (long long)(position_us > 0 ? position_us * 10 : 0),
             paused ? "true" : "false",
             playback->transcoding ? "Transcode" : "DirectPlay",
             event_field);
    struct buffer response;
    long status = http_request_timed(config, "POST", paths[report], body,
                                     &response, 5L);
    free(response.data);
    return status >= 200 && status < 300 ? 0 : -1;
}

/* ---- trickplay ------------------------------------------------------- */

static int json_int(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !value)
        return 0;
    return json_object_get_int(value);
}

int jf_trickplay_info(const struct jf_config *config, const char *item_id,
                      struct jf_trickplay *info)
{
    memset(info, 0, sizeof(*info));
    char path[256];
    snprintf(path, sizeof(path), "/Items?userId=%s&Ids=%s&Fields=Trickplay",
             config->user_id, item_id);
    json_object *root = NULL;
    if (json_call(config, "GET", path, NULL, &root, NULL, 0) < 0)
        return -1;
    json_object *items = NULL, *trickplay = NULL;
    if (json_object_object_get_ex(root, "Items", &items) &&
        json_object_is_type(items, json_type_array) &&
        json_object_array_length(items) > 0)
        json_object_object_get_ex(json_object_array_get_idx(items, 0),
                                  "Trickplay", &trickplay);
    int result = -1;
    if (trickplay && json_object_is_type(trickplay, json_type_object)) {
        /* Media source id -> width -> description; the first source is
         * the one PlaybackInfo picks for a single-version item. */
        json_object_object_foreach(trickplay, source, widths) {
            if (!json_object_is_type(widths, json_type_object))
                continue;
            int best = 0;
            json_object_object_foreach(widths, width_text, entry) {
                int width = atoi(width_text);
                bool better = best == 0 ||
                              (width <= 640 && (best > 640 || width > best)) ||
                              (width > 640 && best > 640 && width < best);
                if (width <= 0 || !better)
                    continue;
                best = width;
                copy_string(info->media_source, sizeof(info->media_source),
                            source);
                info->width = width; /* also the URL key */
                info->height = json_int(entry, "Height");
                info->columns = json_int(entry, "TileWidth");
                info->rows = json_int(entry, "TileHeight");
                info->count = json_int(entry, "ThumbnailCount");
                info->interval_ms = json_int(entry, "Interval");
            }
            break;
        }
        if (info->width > 0 && info->height > 0 && info->columns > 0 &&
            info->rows > 0 && info->count > 0 && info->interval_ms > 0)
            result = 0;
    }
    json_object_put(root);
    return result;
}

/* Appends the "Name" of each array element (or the strings themselves),
 * separated by ", ", as far as they fit. */
static void join_names(json_object *array, char *out, size_t size)
{
    out[0] = '\0';
    if (!array || !json_object_is_type(array, json_type_array))
        return;
    size_t used = 0;
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        json_object *entry = json_object_array_get_idx(array, i);
        const char *name = json_object_is_type(entry, json_type_string)
                               ? json_object_get_string(entry)
                               : json_string(entry, "Name");
        if (!name || !name[0])
            continue;
        size_t length = strlen(name);
        if (used + (used ? 2U : 0U) + length + 1U > size)
            break;
        if (used) {
            memcpy(out + used, ", ", 2);
            used += 2;
        }
        memcpy(out + used, name, length);
        used += length;
        out[used] = '\0';
    }
}

int jf_item_details(const struct jf_config *config, const char *item_id,
                    struct jf_details *details)
{
    memset(details, 0, sizeof(*details));
    char path[256];
    snprintf(path, sizeof(path),
             "/Items/%s?userId=%s&Fields=Chapters,Overview,People,Studios,"
             "Genres,Tags",
             item_id, config->user_id);
    json_object *root = NULL;
    if (json_call(config, "GET", path, NULL, &root, NULL, 0) < 0)
        return -1;
    copy_string(details->overview, sizeof(details->overview),
                json_string(root, "Overview"));
    json_object *value = NULL;
    if (json_object_object_get_ex(root, "People", &value))
        join_names(value, details->people, sizeof(details->people));
    if (json_object_object_get_ex(root, "Studios", &value))
        join_names(value, details->studios, sizeof(details->studios));
    if (json_object_object_get_ex(root, "Genres", &value))
        join_names(value, details->genres, sizeof(details->genres));
    if (json_object_object_get_ex(root, "Tags", &value))
        join_names(value, details->tags, sizeof(details->tags));
    if (json_object_object_get_ex(root, "CommunityRating", &value))
        details->community_rating = (float)json_object_get_double(value);
    details->year = json_int(root, "ProductionYear");
    json_object *chapters = NULL;
    if (json_object_object_get_ex(root, "Chapters", &chapters) &&
        json_object_is_type(chapters, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(chapters) &&
                           details->chapter_count < JF_MAX_CHAPTERS;
             ++i) {
            json_object *chapter = json_object_array_get_idx(chapters, i);
            json_object *start = NULL;
            if (!json_object_object_get_ex(chapter, "StartPositionTicks",
                                           &start))
                continue;
            int n = details->chapter_count++;
            details->chapter_us[n] = json_object_get_int64(start) / 10;
            copy_string(details->chapter_names[n],
                        sizeof(details->chapter_names[n]),
                        json_string(chapter, "Name"));
        }
    }
    json_object_put(root);
    return 0;
}

int jf_fetch_trickplay_tile(const struct jf_config *config,
                            const char *item_id,
                            const struct jf_trickplay *info, int tile,
                            char **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    char path[256];
    snprintf(path, sizeof(path),
             "/Videos/%s/Trickplay/%d/%d.jpg?MediaSourceId=%s", item_id,
             info->width, tile, info->media_source);
    struct buffer response;
    long status = http_request(config, "GET", path, NULL, &response);
    if (status < 200 || status >= 300 || !response.size) {
        free(response.data);
        return -1;
    }
    *data = response.data;
    *size = response.size;
    return 0;
}
