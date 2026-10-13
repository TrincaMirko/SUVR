/*
 * Continuous Iris HEVC Main / H.264 -> NV12 DMA-BUF -> Vulkan transfer probe.
 *
 * Input is demuxed by the bounded media pipeline into Annex-B HEVC Main or
 * 8-bit H.264 access units with container timestamps. The session imports
 * every Iris CAPTURE allocation once, executes one two-plane Vulkan transfer
 * for every decoded frame, and requeues CAPTURE only after the Vulkan fence.
 * 8-bit AV1 up to 4K is decoded with dav1d instead if the viewer opted in
 * (play_decoded, decoded-video.h).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "nv12-playback-ipc.h"
#include "media-pipeline.h"
#include "jellyfin.h"
#include "playback-report.h"
#include "title-settings.h"
#include "format-detect.h"
#include "settings.h"
#include "decoded-video.h"
#include "webdav.h"

#define MAX_OUTPUT_BUFFERS 16U
#define MAX_CAPTURE_BUFFERS 32U
#define REQUESTED_OUTPUT_BUFFERS 6U
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define COMPRESSED_OUTPUT_CAPACITY (2U * 1024U * 1024U)
#define VIDEO_FEED_AHEAD_US INT64_C(500000)
#define VIDEO_LATE_DROP_US INT64_C(100000)

struct access_unit {
    size_t offset;
    size_t size;
};

struct input_stream {
    uint8_t *data;
    size_t size;
    struct access_unit *units;
    size_t unit_count;
    size_t largest_unit;
};

struct output_buffer {
    void *mapping;
    size_t length;
    bool free;
    int64_t pts_us;
    double queued_at;
};

struct capture_buffer {
    size_t length;
    int dma_buf_fd;
    VkImage image;
    VkDeviceMemory memory;
    VkCommandBuffer command;
};

struct timing_stats {
    uint64_t count;
    double sum_ms;
    double maximum_ms;
};

struct vulkan_context {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t timestamp_valid_bits;
    float timestamp_period;
    VkCommandPool command_pool;
    VkSemaphore producer_semaphore;
    VkFence fence;
    VkQueryPool query_pool;
    VkBuffer transfer_buffer;
    VkDeviceMemory transfer_memory;
    VkMemoryPropertyFlags transfer_memory_flags;
    VkDeviceSize transfer_size;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
};

struct video_layout {
    bool tp10;               /* Q10C instead of NV12 */
    bool p010;               /* PC hardware decoder, 10 bit */
    bool host_memory;        /* memfds instead of DMA-BUFs (PC) */
    uint32_t width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t stride;
    uint32_t sizeimage;
    uint64_t uv_offset;
};

struct panel_state {
    bool valid;
    bool visible;
    float x, y, z, yaw;
};

struct playback_options {
    uint32_t projection_mode;
    uint32_t fisheye_fov; /* degrees, NV12_PROJECTION_FISHEYE */
    uint32_t stereo_layout;
    bool swap_eyes;
    uint32_t video_levels;
    /* Set by explicit command-line flags; those win over the stored title
     * settings (a seek restart passes the current values explicitly). */
    bool projection_set, stereo_set, swap_eyes_set, levels_set;
    bool fisheye_fov_set;
    uint32_t frame_limit;
    int64_t start_us;
    struct panel_state panel;
    /* Renderer kept alive across a seek restart (inherited socket + pid). */
    int adopt_renderer_fd;
    pid_t adopt_renderer_pid;
    /* Seek restart of a Jellyfin title: its start is already reported. */
    bool jellyfin_resumed;
    /* Seek restart: where the format came from (enum nv12_format_source);
     * name and trickplay detection already ran. */
    uint32_t format_source;
    bool format_source_set;
    /* Seek restart while paused: show the target frame and stay paused. */
    bool start_paused;
    /* Iris refused to restart the queues after a seek in place (STREAMON
     * ENOMEM with a kernel warning, now and then with 8K): later seeks of
     * this title reopen the decoder straight away. */
    bool no_seek_in_place;
    /* Seek restart: the position the seek started from, which keeps the
     * resume keyframe in the seek's direction (media_demux_seek); -1 if
     * none. */
    int64_t seek_from_us;
    /* Diagnostics: after this many frames (counted from the start), seek
     * as if from the panel; up to four seeks. */
    uint64_t test_seek_frames[4];
    int64_t test_seek_us[4];
    unsigned test_seek_count, test_seek_next;
};

struct capture_contract {
    uint32_t pixel_format;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t memory_planes;
    uint32_t bytes_per_line;
    uint32_t sizeimage;
    struct v4l2_rect compose;
};

struct capture_minimum {
    bool available;
    unsigned value;
};

static int video_fd = -1;
static int renderer_fd = -1;
static pid_t renderer_pid = -1;
static bool renderer_outstanding[MAX_CAPTURE_BUFFERS];
static uint32_t renderer_action;
static int64_t renderer_action_position_us;
static struct panel_state panel_state;
/* Per-title viewing settings: the key identifies the title in titles.json;
 * the renderer reports changes made on the panel. */
static char title_key[TITLE_KEY_SIZE];
static struct title_settings view_state;
static bool view_state_valid;
static bool renderer_held;
/* Pipeline whose output volume the renderer sets. */
static struct media_pipeline *volume_pipeline;
/* Set when a seek within the session failed after stopping the V4L2
 * queues: the restart that follows closes the decoder, so buffers the
 * renderer releases are no longer queued. */
static bool capture_queue_closed;
/* Experimental AV1 decoder; replaces Iris for the whole session. */
static struct decoded_video *decoded_video;

/* Exit codes understood by the launcher. */
#define EXIT_UNSUPPORTED_FORMAT 3
#define EXIT_SOURCE_FAILED 4
#define EXIT_BROKEN_SOURCE 5

static struct jf_config jellyfin_config;
static struct jf_playback jellyfin_playback;
static bool jellyfin_active;
static struct playback_report *jellyfin_report;
/* Set before the seek re-exec: the playback continues in the new process. */
static bool jellyfin_restarting;

static void jellyfin_cleanup(void)
{
    if (!jellyfin_active)
        return;
    jellyfin_active = false;
    playback_report_finish(jellyfin_report, !jellyfin_restarting);
    jellyfin_report = NULL;
    jf_stop_encoding(&jellyfin_config, &jellyfin_playback);
}

/* "jf:item:<id>[:<parent>]" names a Jellyfin title. Resolve it to a stream
 * URL and hand the credentials to libavformat through the environment. */
static int resolve_jellyfin_input(const char *spec, bool resumed, char *url,
                                  size_t size)
{
    char id[JF_ID_SIZE];
    const char *start = spec + strlen("jf:item:");
    size_t length = strcspn(start, ":");
    if (length == 0 || length >= sizeof(id)) {
        fprintf(stderr, "invalid Jellyfin item: %s\n", spec);
        return -1;
    }
    memcpy(id, start, length);
    id[length] = '\0';
    char error[256] = {0};
    if (jf_config_load(&jellyfin_config, error, sizeof(error)) < 0 ||
        !jf_config_has_token(&jellyfin_config)) {
        fprintf(stderr, "Jellyfin: not signed in (%s)\n", error);
        return -1;
    }
    int resolved = jf_resolve_playback(&jellyfin_config, id,
                                       &jellyfin_playback, error,
                                       sizeof(error));
    if (resolved < 0) {
        fprintf(stderr, "Jellyfin: %s\n", error);
        return resolved == JF_BROKEN_ITEM ? JF_BROKEN_ITEM : -1;
    }
    char header[1024];
    jf_authorization_header(&jellyfin_config, header, sizeof(header));
    setenv("STEREONAUT_HTTP_HEADERS", header, 1);
    snprintf(url, size, "%s", jellyfin_playback.url);
    jellyfin_active = true;
    jellyfin_report = playback_report_create(&jellyfin_config,
                                             &jellyfin_playback, resumed);
    atexit(jellyfin_cleanup);
    printf("jellyfin_playback mode=%s\n",
           jellyfin_playback.transcoding ? "hls-transcode" : "direct-play");
    return 0;
}

/* "webdav:play:<idx>:<path>" names a file on a saved WebDAV server. Build
 * its URL and, if the server needs auth, hand Basic credentials to
 * libavformat through the environment (never through argv). */
static int resolve_webdav_input(const char *spec, char *url, size_t size)
{
    const char *rest = spec + strlen("webdav:play:");
    int index = atoi(rest);
    const char *colon = strchr(rest, ':');
    if (!colon || !colon[1]) {
        fprintf(stderr, "invalid WebDAV item: %s\n", spec);
        return -1;
    }
    struct wd_server server;
    if (wd_server_get(index, &server) < 0) {
        fprintf(stderr, "WebDAV: no such server (%d)\n", index);
        return -1;
    }
    char header[512];
    wd_authorization_header(&server, header, sizeof(header));
    if (header[0])
        setenv("STEREONAUT_HTTP_HEADERS", header, 1);
    char origin[512];
    wd_origin(&server, origin, sizeof(origin));
    snprintf(url, size, "%s%s", origin, colon + 1);
    return 0;
}

static void queue_capture(unsigned index, size_t length);

/* SIGTERM from the launcher: the viewer left while the title was still
 * loading. Failures caused by the aborted I/O are then a normal exit. */
static volatile sig_atomic_t stop_requested;

static void handle_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
    media_demux_request_abort();
}

static void fail_errno(const char *what)
{
    if (stop_requested)
        exit(0);
    fprintf(stderr, "%s: errno=%d (%s)\n", what, errno, strerror(errno));
    exit(2);
}

static void fail_message(const char *what)
{
    if (stop_requested)
        exit(0);
    fprintf(stderr, "%s\n", what);
    exit(2);
}

static void fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "%s: VkResult=%d\n", what, result);
    exit(2);
}

static int xioctl(unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(video_fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void must_ioctl(unsigned long request, void *argument, const char *what)
{
    if (xioctl(request, argument) < 0)
        fail_errno(what);
}

/* Orderly decoder teardown for every exit path (normal, seek, and error via
 * atexit). STREAMOFF + REQBUFS count=0 + close lets the Iris firmware drop
 * the HFI session (close_session -> iris_hfi_session_close). An abrupt close
 * can leave a ghost session that still counts against the core mbps/mbpf
 * budget (check_core_mbps_mbpf), which is what makes later 8K sessions fail
 * with -12 ("current session not supported") until the headset is reset.
 * Best-effort: the firmware may reject STREAMOFF after a refused STREAMON. */
static void release_decoder(void)
{
    if (video_fd < 0)
        return;
    enum v4l2_buf_type types[2] = {
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    for (int i = 0; i < 2; i++)
        xioctl(VIDIOC_STREAMOFF, &types[i]);
    for (int i = 0; i < 2; i++) {
        struct v4l2_requestbuffers zero = {
            .count = 0,
            .type = types[i],
            .memory = V4L2_MEMORY_MMAP,
        };
        xioctl(VIDIOC_REQBUFS, &zero);
    }
    close(video_fd);
    video_fd = -1;
}

static int fd_ioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static double monotonic_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        fail_errno("clock_gettime");
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

/* Seek restart: when the seek was requested (monotonic_seconds of the
 * session before the restart; CLOCK_MONOTONIC is system-wide), so the
 * restarted session can log how long each phase took. 0 otherwise. */
static double seek_requested_at;

static void log_seek_phase(const char *phase)
{
    if (seek_requested_at > 0.0)
        printf("seek_restart_phase=%s ms=%.0f\n", phase,
               (monotonic_seconds() - seek_requested_at) * 1000.0);
}

static void send_renderer_message(uint32_t type, uint32_t index,
                                  uint64_t frame_index)
{
    struct nv12_ipc_message message = {
        .type = type, .index = index, .frame_index = frame_index,
    };
    ssize_t written;
    do {
        written = send(renderer_fd, &message, sizeof(message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        fail_errno("send renderer message");
    if ((size_t)written != sizeof(message))
        fail_message("short renderer message");
}

static void send_renderer_status(bool paused, int64_t position_us,
                                 int64_t duration_us)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_STATUS,
        .player_state = paused ? NV12_PLAYER_PAUSED : NV12_PLAYER_PLAYING,
        .position_us = position_us,
        .duration_us = duration_us,
    };
    ssize_t written;
    do {
        written = send(renderer_fd, &message, sizeof(message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        fail_errno("send renderer status");
    if ((size_t)written != sizeof(message))
        fail_message("short renderer status");
}

/* Before the audio clock runs (decoder start, seek preroll) report the
 * requested start so the panel shows the seek target instead of 00:00. */
static int64_t status_position_us(struct media_pipeline *pipeline,
                                  int64_t start_us)
{
    int64_t clock = media_pipeline_clock_us(pipeline);
    return clock == AV_NOPTS_VALUE ? start_us : clock;
}

/* ---- format detection ------------------------------------------------- */

/* Where projection and stereo layout come from. While nothing is known
 * (DEFAULT) they are detected: file name tags first, then the server's
 * trickplay tiles (Jellyfin, waited for before the renderer starts), then
 * the first decoded frames (NV12 only; TP10 is UBWC-compressed). */
static uint32_t format_source = NV12_FORMAT_SOURCE_DEFAULT;
static struct format_guess format_name_guess = {FORMAT_UNKNOWN,
                                                FORMAT_UNKNOWN, 0};
static const char *const format_source_names[] = {"default", "name",
                                                  "image", "user"};

static struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool started, done, consumed;
    char item_id[JF_ID_SIZE];
    struct format_samples samples;
} trickplay_detection = {.mutex = PTHREAD_MUTEX_INITIALIZER};

/* Thumbnails at about 20 % and 55 % of the title, about eleven per tile
 * (tiles hold 5 x 5 or 10 x 10). */
static void *detect_from_trickplay(void *argument)
{
    (void)argument;
    struct format_samples samples = {0};
    struct jf_trickplay info;
    if (jf_trickplay_info(&jellyfin_config, trickplay_detection.item_id,
                          &info) == 0) {
        int per_tile = info.columns * info.rows;
        int tiles = (info.count + per_tile - 1) / per_tile;
        static const double positions[] = {0.2, 0.55};
        int previous = -1;
        for (size_t p = 0; p < 2; ++p) {
            int tile = (int)(info.count * positions[p]) / per_tile;
            if (tile >= tiles)
                tile = tiles - 1;
            if (tile == previous)
                continue;
            previous = tile;
            char *data = NULL;
            size_t size = 0;
            if (jf_fetch_trickplay_tile(&jellyfin_config,
                                        trickplay_detection.item_id, &info,
                                        tile, &data, &size) < 0)
                continue;
            uint8_t *luma = NULL;
            int width = 0, height = 0;
            if (format_decode_jpeg_luma((const unsigned char *)data, size,
                                        &luma, &width, &height) == 0) {
                int step = per_tile / 11 > 1 ? per_tile / 11 : 1;
                for (int k = 0; k < per_tile; k += step) {
                    int column = k % info.columns, row = k / info.columns;
                    if ((column + 1) * info.width > width ||
                        (row + 1) * info.height > height)
                        continue;
                    format_samples_add(
                        &samples,
                        luma + (size_t)row * (size_t)info.height *
                                   (size_t)width +
                            (size_t)column * (size_t)info.width,
                        info.width, info.height, width);
                }
                free(luma);
            }
            free(data);
        }
    }
    pthread_mutex_lock(&trickplay_detection.mutex);
    trickplay_detection.samples = samples;
    trickplay_detection.done = true;
    pthread_mutex_unlock(&trickplay_detection.mutex);
    return NULL;
}

static void start_trickplay_detection(const char *item_id)
{
    snprintf(trickplay_detection.item_id, sizeof(trickplay_detection.item_id),
             "%s", item_id);
    if (pthread_create(&trickplay_detection.thread, NULL,
                       detect_from_trickplay, NULL) == 0) {
        pthread_detach(trickplay_detection.thread);
        trickplay_detection.started = true;
    }
}

/* The analysed samples once, if the analysis has finished. */
static bool take_trickplay_samples(struct format_samples *samples)
{
    bool ready = false;
    pthread_mutex_lock(&trickplay_detection.mutex);
    if (trickplay_detection.done && !trickplay_detection.consumed) {
        *samples = trickplay_detection.samples;
        trickplay_detection.consumed = true;
        ready = true;
    }
    pthread_mutex_unlock(&trickplay_detection.mutex);
    return ready;
}

/* Adopts a complete detection result: stored for the title and, once the
 * renderer runs, sent to it. */
static void apply_format(struct playback_options *options,
                         const struct format_guess *guess, uint32_t source,
                         bool notify_renderer)
{
    options->projection_mode = (uint32_t)guess->projection;
    options->stereo_layout = (uint32_t)guess->stereo;
    if (guess->fisheye_fov > 0)
        options->fisheye_fov = (uint32_t)guess->fisheye_fov;
    format_source = source;
    if (!view_state_valid)
        view_state = (struct title_settings){
            .swap_eyes = options->swap_eyes,
            .video_levels = options->video_levels,
        };
    view_state.projection_mode = options->projection_mode;
    view_state.stereo_layout = options->stereo_layout;
    view_state.fisheye_fov = options->fisheye_fov;
    view_state.format_source = source;
    view_state_valid = true;
    if (title_settings_save(title_key, &view_state) < 0)
        fprintf(stderr, "title settings could not be saved\n");
    printf("format_detected projection=%u stereo=%u source=%s\n",
           options->projection_mode, options->stereo_layout,
           format_source_names[source]);
    if (notify_renderer) {
        struct nv12_ipc_message message = {
            .type = NV12_IPC_FORMAT,
            .projection_mode = options->projection_mode,
            .stereo_layout = options->stereo_layout,
            .fisheye_fov = options->fisheye_fov,
            .format_source = source,
        };
        ssize_t written;
        do {
            written = send(renderer_fd, &message, sizeof(message),
                           MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)sizeof(message))
            fail_errno("send renderer format");
    }
}

/* Name tags (with the picture shape) or the trickplay analysis, before the
 * renderer starts. Partial results replace the defaults but keep the
 * detection running. */
static void detect_format_before_start(struct playback_options *options,
                                       uint32_t width, uint32_t height)
{
    if (format_source != NV12_FORMAT_SOURCE_DEFAULT)
        return;
    struct format_samples none = {0};
    struct format_guess guess = format_name_guess;
    if (format_samples_decide(&none, (int)width, (int)height, &guess)) {
        apply_format(options, &guess, NV12_FORMAT_SOURCE_NAME, false);
        return;
    }
    if (trickplay_detection.started) {
        /* Started with the stream resolution; usually done by now. */
        double deadline = monotonic_seconds() + 3.0;
        struct format_samples samples;
        while (!take_trickplay_samples(&samples)) {
            if (monotonic_seconds() > deadline) {
                printf("format_trickplay pending\n");
                break;
            }
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
            nanosleep(&delay, NULL);
        }
        if (trickplay_detection.consumed) {
            printf("format_trickplay samples=%d\n", samples.count);
            guess = format_name_guess;
            if (format_samples_decide(&samples, (int)width, (int)height,
                                      &guess)) {
                apply_format(options, &guess, NV12_FORMAT_SOURCE_IMAGE,
                             false);
                return;
            }
        }
    }
    if (guess.projection != FORMAT_UNKNOWN)
        options->projection_mode = (uint32_t)guess.projection;
    if (guess.fisheye_fov > 0)
        options->fisheye_fov = (uint32_t)guess.fisheye_fov;
    if (guess.stereo != FORMAT_UNKNOWN)
        options->stereo_layout = (uint32_t)guess.stereo;
}

/* Analysis of the first decoded frames: one luma thumbnail per second of
 * video, read through a CPU mapping of the capture buffer. */
#define FRAME_DETECTION_TRIES 40U
#define FRAME_THUMBNAIL_WIDTH 256

static struct {
    bool active;
    unsigned tries;
    int64_t next_pts_us;
    struct format_samples samples;
    const uint8_t *mappings[MAX_CAPTURE_BUFFERS];
} frame_detection;

static bool sample_frame(const struct capture_buffer *buffer, unsigned index,
                         const struct video_layout *layout)
{
    if (!frame_detection.mappings[index]) {
        void *mapping = mmap(NULL, buffer->length, PROT_READ, MAP_SHARED,
                             buffer->dma_buf_fd, 0);
        if (mapping == MAP_FAILED) {
            printf("format_frames unavailable errno=%d\n", errno);
            frame_detection.active = false;
            return false;
        }
        frame_detection.mappings[index] = mapping;
    }
    uint32_t width = layout->visible_width, height = layout->visible_height;
    int thumb_w = FRAME_THUMBNAIL_WIDTH;
    int thumb_h = (int)((uint64_t)thumb_w * height / width);
    if (thumb_h < 32)
        thumb_h = 32;
    if (thumb_h > 512)
        thumb_h = 512;
    uint8_t thumbnail[FRAME_THUMBNAIL_WIDTH * 512];
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START |
                                         DMA_BUF_SYNC_READ};
    fd_ioctl(buffer->dma_buf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    const uint8_t *luma = frame_detection.mappings[index];
    /* P010: the upper byte of each little-endian 16-bit sample. */
    size_t sample_bytes = layout->p010 ? 2U : 1U;
    for (int y = 0; y < thumb_h; ++y) {
        size_t row = (size_t)((2U * (uint32_t)y + 1U) * height /
                              (2U * (uint32_t)thumb_h)) * layout->stride;
        for (int x = 0; x < thumb_w; ++x)
            thumbnail[y * thumb_w + x] =
                luma[row + sample_bytes * ((2U * (uint32_t)x + 1U) * width /
                                           (2U * (uint32_t)thumb_w)) +
                     sample_bytes - 1U];
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    fd_ioctl(buffer->dma_buf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    return format_samples_add(&frame_detection.samples, thumbnail, thumb_w,
                              thumb_h, thumb_w);
}

/* Called for every frame handed to the renderer. */
static void detect_format_from_frame(struct playback_options *options,
                                     const struct capture_buffer *buffers,
                                     unsigned index,
                                     const struct video_layout *layout,
                                     int64_t pts_us)
{
    if (format_source != NV12_FORMAT_SOURCE_DEFAULT)
        return;
    struct format_samples samples;
    struct format_guess guess;
    if (take_trickplay_samples(&samples)) { /* finished after the start */
        printf("format_trickplay samples=%d late=1\n", samples.count);
        guess = format_name_guess;
        if (format_samples_decide(&samples, (int)layout->visible_width,
                                  (int)layout->visible_height, &guess)) {
            apply_format(options, &guess, NV12_FORMAT_SOURCE_IMAGE, true);
            return;
        }
    }
    if (!frame_detection.active || pts_us < frame_detection.next_pts_us)
        return;
    frame_detection.next_pts_us = pts_us + 1000000;
    bool used = sample_frame(&buffers[index], index, layout);
    const struct format_samples *taken = &frame_detection.samples;
    int last = taken->count - 1;
    printf("format_frame try=%u used=%d samples=%d sbs=%.2f tb=%.2f\n",
           frame_detection.tries, used, taken->count,
           used ? taken->sbs[last] : 0.0f, used ? taken->tb[last] : 0.0f);
    if (++frame_detection.tries >= FRAME_DETECTION_TRIES)
        frame_detection.active = false;
    guess = format_name_guess;
    if (frame_detection.samples.count >= 8 &&
        format_samples_decide(&frame_detection.samples,
                              (int)layout->visible_width,
                              (int)layout->visible_height, &guess)) {
        frame_detection.active = false;
        apply_format(options, &guess, NV12_FORMAT_SOURCE_IMAGE, true);
    } else if (!frame_detection.active) {
        printf("format_frames undecided samples=%d\n",
               frame_detection.samples.count);
    }
}

static void start_renderer(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS], unsigned count,
    const struct video_layout *layout, const struct playback_options *options)
{
    bool adopted = options->adopt_renderer_fd >= 0;
    if (adopted) {
        /* The renderer survived the seek restart: it keeps showing the last
         * frame and re-imports the new buffers on INIT. */
        renderer_fd = options->adopt_renderer_fd;
        renderer_pid = options->adopt_renderer_pid;
        int fd_flags = fcntl(renderer_fd, F_GETFD);
        if (fd_flags >= 0)
            fcntl(renderer_fd, F_SETFD, fd_flags | FD_CLOEXEC);
        struct nv12_ipc_message stale;
        ssize_t drained;
        do {
            drained = recv(renderer_fd, &stale, sizeof(stale), MSG_DONTWAIT);
        } while (drained > 0 || (drained < 0 && errno == EINTR));
        if (drained == 0)
            fail_message("adopted OpenXR renderer is gone");
        printf("renderer_adopted pid=%ld fd=%d\n", (long)renderer_pid,
               renderer_fd);
    } else {
        fail_message("no renderer socket: start the session through"
                     " stereonaut-launcher (--renderer FD,PID)");
    }

    struct nv12_ipc_message message = {
        .type = NV12_IPC_INIT,
        .buffer_count = count,
        .width = layout->visible_width,
        .height = layout->visible_height,
        .stride = layout->stride,
        .size = layout->sizeimage,
        .uv_offset = layout->uv_offset,
        .projection_mode = options->projection_mode,
        .fisheye_fov = options->fisheye_fov,
        .stereo_layout = options->stereo_layout,
        .swap_eyes = options->swap_eyes,
        .video_levels = options->video_levels,
        .panel_x = options->panel.x,
        .panel_y = options->panel.y,
        .panel_z = options->panel.z,
        .panel_yaw = options->panel.yaw,
        .panel_visible = options->panel.visible,
        .panel_valid = options->panel.valid,
        .buffer_kind = layout->host_memory ? NV12_BUFFER_HOST
                                           : NV12_BUFFER_DMA_BUF,
        .pixel_format = layout->tp10   ? NV12_FORMAT_TP10
                        : layout->p010 ? NV12_FORMAT_P010
                                       : NV12_FORMAT_NV12,
        .coded_width = layout->width,
        .coded_height = layout->coded_height,
        .format_source = format_source,
    };
    int fds[MAX_CAPTURE_BUFFERS];
    for (unsigned i = 0; i < count; ++i)
        fds[i] = buffers[i].dma_buf_fd;
    char control[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
    struct msghdr sent = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = CMSG_SPACE(sizeof(int) * count),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&sent);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(header), fds, sizeof(int) * count);
    ssize_t bytes;
    do {
        bytes = sendmsg(renderer_fd, &sent, MSG_NOSIGNAL);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0)
        fail_errno("send renderer init");
    if ((size_t)bytes != sizeof(message))
        fail_message("short renderer init");
    int flags = fcntl(renderer_fd, F_GETFL);
    if (flags < 0 || fcntl(renderer_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        fail_errno("set renderer IPC nonblocking");
    printf("renderer_started pid=%ld buffers=%u ipc=seqpacket"
           " state=initializing\n",
           (long)renderer_pid, count);

    double deadline = monotonic_seconds() + 15.0;
    for (;;) {
        struct nv12_ipc_message response = {0};
        ssize_t received = recv(renderer_fd, &response, sizeof(response),
                                MSG_DONTWAIT);
        if (received == (ssize_t)sizeof(response)) {
            if (response.type != NV12_IPC_READY) {
                if (adopted)
                    continue;
                fail_message("unexpected renderer initialization response");
            }
            break;
        }
        if (received == 0)
            fail_message("OpenXR renderer exited during initialization");
        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR)
            fail_errno("receive renderer ready");
        if (monotonic_seconds() > deadline)
            fail_message("OpenXR renderer initialization timed out");
        struct pollfd ready_poll = {.fd = renderer_fd, .events = POLLIN};
        int ready = poll(&ready_poll, 1, 50);
        if (ready < 0 && errno != EINTR)
            fail_errno("poll renderer ready");
    }
    printf("renderer_ready pid=%ld buffers=%u projection=%u stereo=%u"
           " swap_eyes=%u\n",
           (long)renderer_pid, count, options->projection_mode,
           options->stereo_layout, options->swap_eyes);
    log_seek_phase("renderer_imported");
}

static bool process_renderer_messages(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS], unsigned count)
{
    bool flushed = false;
    for (;;) {
        struct nv12_ipc_message message = {0};
        ssize_t bytes = recv(renderer_fd, &message, sizeof(message),
                             MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes == 0)
            fail_message("OpenXR renderer disconnected unexpectedly");
        if (bytes < 0)
            fail_errno("receive renderer message");
        if ((size_t)bytes != sizeof(message))
            fail_message("short renderer response");
        if (message.type == NV12_IPC_RELEASE) {
            if (message.index >= count || !renderer_outstanding[message.index])
                fail_message("invalid renderer release");
            queue_capture(message.index, buffers[message.index].length);
            renderer_outstanding[message.index] = false;
        } else if (message.type == NV12_IPC_FLUSHED) {
            flushed = true;
        } else if (message.type == NV12_IPC_ACTION) {
            if (message.index < NV12_ACTION_TOGGLE_PAUSE ||
                message.index > NV12_ACTION_SEEK_TO)
                fail_message("invalid renderer player action");
            renderer_action = message.index;
            renderer_action_position_us = message.position_us;
        } else if (message.type == NV12_IPC_VIEW) {
            if (message.projection_mode > NV12_PROJECTION_FISHEYE ||
                message.fisheye_fov < 120 || message.fisheye_fov > 240 ||
                message.stereo_layout > NV12_STEREO_TB ||
                message.video_levels > NV12_LEVELS_FULL)
                fail_message("invalid renderer view settings");
            /* A changed projection or layout is the viewer's choice and
             * ends the detection; eye order and levels keep its source. */
            if (message.projection_mode != view_state.projection_mode ||
                message.stereo_layout != view_state.stereo_layout ||
                message.fisheye_fov != view_state.fisheye_fov) {
                format_source = NV12_FORMAT_SOURCE_USER;
                frame_detection.active = false;
            }
            view_state = (struct title_settings){
                .projection_mode = message.projection_mode,
                .stereo_layout = message.stereo_layout,
                .swap_eyes = message.swap_eyes != 0,
                .video_levels = message.video_levels,
                .fisheye_fov = message.fisheye_fov,
                .format_source = format_source,
            };
            view_state_valid = true;
            if (title_settings_save(title_key, &view_state) < 0)
                fprintf(stderr, "title settings could not be saved\n");
            printf("title_view projection=%u stereo=%u swap_eyes=%u"
                   " levels=%u key=%s\n",
                   view_state.projection_mode, view_state.stereo_layout,
                   view_state.swap_eyes, view_state.video_levels, title_key);
        } else if (message.type == NV12_IPC_VOLUME) {
            media_pipeline_set_volume(volume_pipeline, message.volume);
            printf("volume=%.2f\n", message.volume);
        } else if (message.type == NV12_IPC_HELD) {
            renderer_held = true;
        } else if (message.type == NV12_IPC_PANEL) {
            panel_state = (struct panel_state){
                .valid = true,
                .visible = message.panel_visible != 0,
                .x = message.panel_x, .y = message.panel_y,
                .z = message.panel_z, .yaw = message.panel_yaw,
            };
        } else {
            fail_message("unknown renderer response");
        }
    }
    return flushed;
}

static void flush_renderer(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS], unsigned count)
{
    send_renderer_message(NV12_IPC_FLUSH, 0, 0);
    double deadline = monotonic_seconds() + 5.0;
    bool flushed = false;
    while (!flushed) {
        flushed = process_renderer_messages(buffers, count);
        if (flushed)
            break;
        if (monotonic_seconds() > deadline)
            fail_message("OpenXR renderer flush timed out");
        struct pollfd pollfd = {.fd = renderer_fd, .events = POLLIN};
        int result = poll(&pollfd, 1, 50);
        if (result < 0 && errno != EINTR)
            fail_errno("poll renderer flush");
    }
    for (unsigned i = 0; i < count; ++i)
        if (renderer_outstanding[i])
            fail_message("renderer retained CAPTURE buffer after flush");
}

/* Seek restart: ask the renderer to keep its current frame and wait until it
 * dropped every other import, so this process may close the decoder. The
 * renderer stays alive and is adopted by the re-executed session. */
static void hold_renderer(struct capture_buffer buffers[MAX_CAPTURE_BUFFERS],
                          unsigned count)
{
    renderer_held = false;
    send_renderer_message(NV12_IPC_HOLD, 0, 0);
    double deadline = monotonic_seconds() + 5.0;
    while (!renderer_held) {
        process_renderer_messages(buffers, count);
        if (renderer_held)
            break;
        if (monotonic_seconds() > deadline)
            fail_message("OpenXR renderer did not acknowledge HOLD");
        struct pollfd pollfd = {.fd = renderer_fd, .events = POLLIN};
        int result = poll(&pollfd, 1, 20);
        if (result < 0 && errno != EINTR)
            fail_errno("poll renderer hold");
    }
    printf("renderer_held pid=%ld\n", (long)renderer_pid);
}

/* `finished`: the title played to its end (the renderer may continue with
 * the next one), otherwise it was stopped. */
static void wait_renderer(bool finished)
{
    send_renderer_message(NV12_IPC_EOS, finished ? 1U : 0U, 0);
    int flags = fcntl(renderer_fd, F_GETFL);
    if (flags >= 0)
        fcntl(renderer_fd, F_SETFL, flags & ~O_NONBLOCK);
    if (renderer_pid > 0) {
        int status = 0;
        while (waitpid(renderer_pid, &status, 0) < 0) {
            if (errno != EINTR)
                fail_errno("waitpid OpenXR renderer");
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fail_message("OpenXR playback renderer failed");
    }
    /* A host-owned renderer keeps running; closing the socket detaches it. */
    close(renderer_fd);
    renderer_fd = -1;
    renderer_pid = -1;
    printf("renderer_exit=success\n");
}

static void update_stats(struct timing_stats *stats, double milliseconds)
{
    stats->count++;
    stats->sum_ms += milliseconds;
    if (milliseconds > stats->maximum_ms)
        stats->maximum_ms = milliseconds;
}

static uint32_t parse_u32(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint32_t)value;
}

static int64_t parse_i64_nonnegative(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || !end || *end || value < 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (int64_t)value;
}

static struct playback_options parse_playback_options(int argc, char **argv)
{
    struct playback_options options = {0};
    options.adopt_renderer_fd = -1;
    options.seek_from_us = -1;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--swap-eyes") == 0) {
            options.swap_eyes = true;
            options.swap_eyes_set = true;
        } else if (strcmp(argv[i], "--no-swap-eyes") == 0) {
            options.swap_eyes = false;
            options.swap_eyes_set = true;
        } else if (strcmp(argv[i], "--projection") == 0) {
            if (++i >= argc)
                fail_message("--projection requires flat, 180, 360 or"
                             " fisheye");
            if (strcmp(argv[i], "flat") == 0)
                options.projection_mode = NV12_PROJECTION_FLAT;
            else if (strcmp(argv[i], "180") == 0)
                options.projection_mode = NV12_PROJECTION_EQUIRECT_180;
            else if (strcmp(argv[i], "360") == 0)
                options.projection_mode = NV12_PROJECTION_EQUIRECT_360;
            else if (strcmp(argv[i], "fisheye") == 0)
                options.projection_mode = NV12_PROJECTION_FISHEYE;
            else
                fail_message("invalid --projection value");
            options.projection_set = true;
        } else if (strcmp(argv[i], "--stereo") == 0) {
            if (++i >= argc)
                fail_message("--stereo requires mono, sbs, or tb");
            if (strcmp(argv[i], "mono") == 0)
                options.stereo_layout = NV12_STEREO_MONO;
            else if (strcmp(argv[i], "sbs") == 0)
                options.stereo_layout = NV12_STEREO_SBS;
            else if (strcmp(argv[i], "tb") == 0)
                options.stereo_layout = NV12_STEREO_TB;
            else
                fail_message("invalid --stereo value");
            options.stereo_set = true;
        } else if (strcmp(argv[i], "--levels") == 0) {
            if (++i >= argc)
                fail_message("--levels requires normal, deep, or full");
            if (strcmp(argv[i], "normal") == 0)
                options.video_levels = NV12_LEVELS_NORMAL;
            else if (strcmp(argv[i], "deep") == 0)
                options.video_levels = NV12_LEVELS_DEEP;
            else if (strcmp(argv[i], "full") == 0)
                options.video_levels = NV12_LEVELS_FULL;
            else
                fail_message("invalid --levels value");
            options.levels_set = true;
        } else if (strcmp(argv[i], "--fisheye-fov") == 0) {
            if (++i >= argc)
                fail_message("--fisheye-fov requires degrees");
            uint32_t degrees = parse_u32(argv[i], "fisheye field of view");
            if (degrees < 120 || degrees > 240)
                fail_message("--fisheye-fov must lie between 120 and 240");
            options.fisheye_fov = degrees;
            options.fisheye_fov_set = true;
        } else if (strcmp(argv[i], "--paused") == 0) {
            options.start_paused = true;
        } else if (strcmp(argv[i], "--no-seek-in-place") == 0) {
            options.no_seek_in_place = true;
        } else if (strcmp(argv[i], "--seek-requested-at") == 0) {
            if (++i >= argc)
                fail_message("--seek-requested-at requires seconds");
            seek_requested_at = strtod(argv[i], NULL);
        } else if (strcmp(argv[i], "--seek-from-us") == 0) {
            if (++i >= argc)
                fail_message("--seek-from-us requires a timestamp");
            options.seek_from_us =
                parse_i64_nonnegative(argv[i], "seek origin");
        } else if (strcmp(argv[i], "--format-source") == 0) {
            static const char *const sources[] = {"default", "name", "image",
                                                  "user"};
            if (++i >= argc)
                fail_message("--format-source requires a source name");
            options.format_source_set = false;
            for (uint32_t s = 0; s <= NV12_FORMAT_SOURCE_USER; ++s)
                if (strcmp(argv[i], sources[s]) == 0) {
                    options.format_source = s;
                    options.format_source_set = true;
                }
            if (!options.format_source_set)
                fail_message("invalid --format-source value");
        } else if (strcmp(argv[i], "--seek-test") == 0) {
            unsigned long long frames_before = 0;
            long long target = 0;
            if (++i >= argc ||
                sscanf(argv[i], "%llu:%lld", &frames_before, &target) != 2 ||
                frames_before == 0 || target < 0 ||
                options.test_seek_count == 4)
                fail_message("--seek-test requires FRAMES:TARGET_US"
                             " (at most four)");
            options.test_seek_frames[options.test_seek_count] = frames_before;
            options.test_seek_us[options.test_seek_count++] = target;
        } else if (strcmp(argv[i], "--frames") == 0) {
            if (++i >= argc)
                fail_message("--frames requires a positive frame count");
            options.frame_limit = parse_u32(argv[i], "frame limit");
        } else if (strcmp(argv[i], "--panel") == 0) {
            if (++i >= argc)
                fail_message("--panel requires x,y,z,yaw,visible");
            unsigned visible = 0;
            if (sscanf(argv[i], "%f,%f,%f,%f,%u", &options.panel.x,
                       &options.panel.y, &options.panel.z,
                       &options.panel.yaw, &visible) != 5 || visible > 1)
                fail_message("invalid --panel value");
            options.panel.visible = visible != 0;
            options.panel.valid = true;
        } else if (strcmp(argv[i], "--renderer") == 0) {
            if (++i >= argc)
                fail_message("--renderer requires fd,pid");
            int fd = -1;
            long pid = 0;
            if (sscanf(argv[i], "%d,%ld", &fd, &pid) != 2 || fd < 3 ||
                pid < 0)
                fail_message("invalid --renderer value");
            options.adopt_renderer_fd = fd;
            options.adopt_renderer_pid = (pid_t)pid;
        } else if (strcmp(argv[i], "--jellyfin-resumed") == 0) {
            options.jellyfin_resumed = true;
        } else if (strcmp(argv[i], "--start-us") == 0) {
            if (++i >= argc)
                fail_message("--start-us requires a nonnegative timestamp");
            options.start_us =
                parse_i64_nonnegative(argv[i], "start timestamp");
        } else {
            fail_message("unknown playback option");
        }
    }
    return options;
}

static struct input_stream read_input(const char *path)
{
    struct stat status;
    if (stat(path, &status) < 0)
        fail_errno("stat input");
    if (status.st_size <= 0)
        fail_message("input is empty");
    if ((uintmax_t)status.st_size > SIZE_MAX)
        fail_message("input exceeds addressable size");

    FILE *file = fopen(path, "rb");
    if (!file)
        fail_errno("open input");
    struct input_stream stream = {
        .size = (size_t)status.st_size,
    };
    stream.data = malloc(stream.size);
    if (!stream.data)
        fail_errno("allocate input");
    if (fread(stream.data, 1, stream.size, file) != stream.size)
        fail_errno("read input");
    if (fclose(file) != 0)
        fail_errno("close input");

    size_t capacity = 16384;
    size_t *starts = malloc(capacity * sizeof(*starts));
    if (!starts)
        fail_errno("allocate access-unit starts");
    for (size_t i = 0; i + 5 < stream.size;) {
        unsigned prefix = 0;
        if (stream.data[i] == 0 && stream.data[i + 1] == 0 &&
            stream.data[i + 2] == 1) {
            prefix = 3;
        } else if (stream.data[i] == 0 && stream.data[i + 1] == 0 &&
                   stream.data[i + 2] == 0 && stream.data[i + 3] == 1) {
            prefix = 4;
        }
        if (!prefix) {
            i++;
            continue;
        }
        uint8_t nal_type = (stream.data[i + prefix] >> 1) & 63U;
        if (nal_type == 35U) {
            if (stream.unit_count == capacity) {
                if (capacity > SIZE_MAX / 2 / sizeof(*starts))
                    fail_message("too many access units");
                capacity *= 2;
                size_t *grown = realloc(starts, capacity * sizeof(*starts));
                if (!grown)
                    fail_errno("grow access-unit starts");
                starts = grown;
            }
            starts[stream.unit_count++] = i;
        }
        i += prefix + 2;
    }
    if (!stream.unit_count)
        fail_message("input has no AUD-delimited access units");
    starts[0] = 0;
    stream.units = calloc(stream.unit_count, sizeof(*stream.units));
    if (!stream.units)
        fail_errno("allocate access units");
    for (size_t i = 0; i < stream.unit_count; i++) {
        size_t end = i + 1 < stream.unit_count ? starts[i + 1] : stream.size;
        if (end <= starts[i])
            fail_message("invalid access-unit boundaries");
        stream.units[i].offset = starts[i];
        stream.units[i].size = end - starts[i];
        if (stream.units[i].size > stream.largest_unit)
            stream.largest_unit = stream.units[i].size;
    }
    free(starts);
    printf("input_bytes=%zu access_units=%zu largest_access_unit=%zu\n",
           stream.size, stream.unit_count, stream.largest_unit);
    return stream;
}

static bool has_device_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(
        physical, NULL, &count, NULL);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumerateDeviceExtensionProperties(count)", result);
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (!properties)
        fail_errno("allocate extension properties");
    result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count,
                                                   properties);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumerateDeviceExtensionProperties(list)", result);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(properties[i].extensionName, name) == 0) {
            found = true;
            break;
        }
    }
    free(properties);
    return found;
}

static uint32_t choose_memory_type(VkPhysicalDevice physical,
                                   uint32_t allowed_bits,
                                   VkMemoryPropertyFlags required,
                                   VkMemoryPropertyFlags preferred,
                                   VkMemoryPropertyFlags *chosen_flags)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags flags =
                properties.memoryTypes[i].propertyFlags;
            if (!(allowed_bits & (1U << i)) ||
                (flags & required) != required)
                continue;
            if (pass == 0 && (flags & preferred) != preferred)
                continue;
            *chosen_flags = flags;
            return i;
        }
    }
    fail_message("no compatible Vulkan memory type");
    return 0;
}

static void check_nv12_modifier_capability(struct vulkan_context *vk)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(
        vk->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    if (!list.drmFormatModifierCount)
        fail_message("Turnip reports no NV12 DRM modifiers");
    list.pDrmFormatModifierProperties =
        calloc(list.drmFormatModifierCount,
               sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties)
        fail_errno("allocate modifier properties");
    uint32_t capacity = list.drmFormatModifierCount;
    vkGetPhysicalDeviceFormatProperties2(
        vk->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &properties);
    if (list.drmFormatModifierCount > capacity)
        fail_message("NV12 modifier list grew unexpectedly");
    bool linear = false;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        VkDrmFormatModifierPropertiesEXT *item =
            &list.pDrmFormatModifierProperties[i];
        if (item->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
            item->drmFormatModifierPlaneCount == 2 &&
            (item->drmFormatModifierTilingFeatures &
             (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
                (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            linear = true;
        }
    }
    free(list.pDrmFormatModifierProperties);
    if (!linear)
        fail_message("linear NV12 lacks two memory planes or required usage");

    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &external,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 format = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &modifier,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    VkExternalImageFormatProperties external_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 image_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        vk->physical, &format, &image_properties);
    if (result != VK_SUCCESS)
        fail_vk("NV12 linear DMA-BUF capability query", result);
    VkExternalMemoryFeatureFlags features =
        external_properties.externalMemoryProperties.externalMemoryFeatures;
    if (!(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        fail_message("linear NV12 DMA-BUF is not importable");
    printf("vulkan_preflight nv12_linear=1 memory_planes=2"
           " sampled=1 transfer_src=1 external_features=0x%x\n",
           features);
}

static void initialize_vulkan(struct vulkan_context *vk)
{
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-realvideo-001",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkResult result = vkCreateInstance(&instance_info, NULL, &vk->instance);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateInstance", result);

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(vk->instance, &physical_count, NULL);
    if (result != VK_SUCCESS || physical_count == 0)
        fail_vk("vkEnumeratePhysicalDevices(count)", result);
    VkPhysicalDevice *physicals = calloc(physical_count, sizeof(*physicals));
    if (!physicals)
        fail_errno("allocate physical devices");
    result = vkEnumeratePhysicalDevices(vk->instance, &physical_count,
                                         physicals);
    if (result != VK_SUCCESS)
        fail_vk("vkEnumeratePhysicalDevices(list)", result);
    vk->physical = physicals[0];
    free(physicals);

    VkPhysicalDeviceProperties physical_properties;
    vkGetPhysicalDeviceProperties(vk->physical, &physical_properties);
    if (physical_properties.vendorID != 0x5143)
        fail_message("unexpected Vulkan vendor; refusing non-Turnip path");
    vk->timestamp_period = physical_properties.limits.timestampPeriod;
    printf("vulkan_device=%s vendor=0x%x device=0x%x timestamp_period_ns=%f\n",
           physical_properties.deviceName, physical_properties.vendorID,
           physical_properties.deviceID, vk->timestamp_period);

    const char *extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        if (!has_device_extension(vk->physical, extensions[i])) {
            fprintf(stderr, "required Vulkan extension missing: %s\n",
                    extensions[i]);
            exit(2);
        }
    }

    check_nv12_modifier_capability(vk);

    VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties semaphore_properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(
        vk->physical, &semaphore_info, &semaphore_properties);
    if (!(semaphore_properties.externalSemaphoreFeatures &
          VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ||
        !(semaphore_properties.compatibleHandleTypes &
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT))
        fail_message("SYNC_FD semaphore import is unsupported");
    printf("sync_fd_preflight importable=1 features=0x%x\n",
           semaphore_properties.externalSemaphoreFeatures);

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &queue_count, NULL);
    VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
    if (!queues)
        fail_errno("allocate queue properties");
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &queue_count,
                                              queues);
    bool queue_found = false;
    for (uint32_t i = 0; i < queue_count; i++) {
        if ((queues[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            queues[i].timestampValidBits) {
            vk->queue_family = i;
            vk->timestamp_valid_bits = queues[i].timestampValidBits;
            queue_found = true;
            break;
        }
    }
    free(queues);
    if (!queue_found)
        fail_message("no transfer queue with timestamp support");

    float priority = 0.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount =
            (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
        .ppEnabledExtensionNames = extensions,
    };
    result = vkCreateDevice(vk->physical, &device_info, NULL, &vk->device);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateDevice", result);
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
    if (vk->queue == VK_NULL_HANDLE)
        fail_message("vkGetDeviceQueue returned null");

    vk->import_semaphore_fd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            vk->device, "vkImportSemaphoreFdKHR");
    vk->get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            vk->device, "vkGetMemoryFdPropertiesKHR");
    vk->get_modifier_properties =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            vk->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!vk->import_semaphore_fd || !vk->get_memory_fd_properties ||
        !vk->get_modifier_properties)
        fail_message("required Vulkan external-memory entry point missing");

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->queue_family,
    };
    result = vkCreateCommandPool(vk->device, &pool_info, NULL,
                                 &vk->command_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateCommandPool", result);
    VkSemaphoreCreateInfo create_semaphore = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    result = vkCreateSemaphore(vk->device, &create_semaphore, NULL,
                               &vk->producer_semaphore);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateSemaphore", result);
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    result = vkCreateFence(vk->device, &fence_info, NULL, &vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateFence", result);
    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2,
    };
    result = vkCreateQueryPool(vk->device, &query_info, NULL, &vk->query_pool);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateQueryPool", result);
    printf("vulkan_preflight=success queue_family=%u timestamp_bits=%u"
           " decoder_stream_not_started_yet\n",
           vk->queue_family, vk->timestamp_valid_bits);
}

static void create_transfer_buffer(struct vulkan_context *vk,
                                   const struct video_layout *layout)
{
    vk->transfer_size = (VkDeviceSize)layout->width * layout->coded_height *
                        3U / 2U;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vk->transfer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(vk->device, &buffer_info, NULL,
                                     &vk->transfer_buffer);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateBuffer(transfer)", result);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device, vk->transfer_buffer,
                                  &requirements);
    uint32_t memory_type = choose_memory_type(
        vk->physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &vk->transfer_memory_flags);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    result = vkAllocateMemory(vk->device, &allocation, NULL,
                              &vk->transfer_memory);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateMemory(transfer)", result);
    result = vkBindBufferMemory(vk->device, vk->transfer_buffer,
                                vk->transfer_memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindBufferMemory(transfer)", result);
    printf("transfer_buffer bytes=%" PRIu64 " memory_type=%u flags=0x%x\n",
           (uint64_t)vk->transfer_size, memory_type,
           vk->transfer_memory_flags);
}

static void record_transfer_command(struct vulkan_context *vk,
                                    struct capture_buffer *capture,
                                    const struct video_layout *layout)
{
    VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult result = vkAllocateCommandBuffers(vk->device, &allocation,
                                                &capture->command);
    if (result != VK_SUCCESS)
        fail_vk("vkAllocateCommandBuffers", result);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    result = vkBeginCommandBuffer(capture->command, &begin);
    if (result != VK_SUCCESS)
        fail_vk("vkBeginCommandBuffer", result);
    vkCmdResetQueryPool(capture->command, vk->query_pool, 0, 2);
    vkCmdWriteTimestamp(capture->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        vk->query_pool, 0);

    VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = vk->queue_family,
        .image = capture->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(capture->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &acquire);
    const VkBufferImageCopy copies[2] = {
        {
            .bufferOffset = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {layout->width, layout->coded_height, 1},
        },
        {
            .bufferOffset =
                (VkDeviceSize)layout->width * layout->coded_height,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageExtent = {layout->width / 2, layout->coded_height / 2, 1},
        },
    };
    vkCmdCopyImageToBuffer(capture->command, capture->image,
                           VK_IMAGE_LAYOUT_GENERAL, vk->transfer_buffer, 2,
                           copies);
    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask = 0;
    release.srcQueueFamilyIndex = vk->queue_family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(capture->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &release);
    vkCmdWriteTimestamp(capture->command,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk->query_pool,
                        1);
    result = vkEndCommandBuffer(capture->command);
    if (result != VK_SUCCESS)
        fail_vk("vkEndCommandBuffer", result);
}

static void import_capture_image(struct vulkan_context *vk,
                                 struct capture_buffer *capture,
                                 const struct video_layout *layout,
                                 unsigned index)
{
    const VkSubresourceLayout plane_layouts[2] = {
        {
            .offset = 0,
            .size = (VkDeviceSize)layout->stride * layout->coded_height,
            .rowPitch = layout->stride,
        },
        {
            .offset = layout->uv_offset,
            .size = (VkDeviceSize)layout->stride * layout->coded_height / 2U,
            .rowPitch = layout->stride,
        },
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .drmFormatModifierPlaneCount = 2,
        .pPlaneLayouts = plane_layouts,
    };
    VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .extent = {layout->width, layout->coded_height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(vk->device, &image_info, NULL,
                                    &capture->image);
    if (result != VK_SUCCESS)
        fail_vk("vkCreateImage(NV12 DMA-BUF)", result);

    VkImageDrmFormatModifierPropertiesEXT chosen_modifier = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    result = vk->get_modifier_properties(vk->device, capture->image,
                                          &chosen_modifier);
    if (result != VK_SUCCESS)
        fail_vk("vkGetImageDrmFormatModifierPropertiesEXT", result);
    if (chosen_modifier.drmFormatModifier != DRM_FORMAT_MOD_LINEAR)
        fail_message("Vulkan returned a non-linear NV12 modifier");

    for (uint32_t plane = 0; plane < 2; plane++) {
        VkImageSubresource subresource = {
            .aspectMask = plane == 0 ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT
                                     : VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        };
        VkSubresourceLayout returned;
        vkGetImageSubresourceLayout(vk->device, capture->image, &subresource,
                                    &returned);
        printf("vulkan_layout index=%u memory_plane=%u offset=%" PRIu64
               " row_pitch=%" PRIu64 " size=%" PRIu64 "\n",
               index, plane, (uint64_t)returned.offset,
               (uint64_t)returned.rowPitch, (uint64_t)returned.size);
        if (returned.offset != plane_layouts[plane].offset ||
            returned.rowPitch != plane_layouts[plane].rowPitch)
            fail_message("Turnip returned a different explicit NV12 layout");
    }

    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = capture->image,
    };
    vkGetImageMemoryRequirements2(vk->device, &requirements_info,
                                  &requirements);
    printf("image_memory index=%u size=%" PRIu64 " alignment=%" PRIu64
           " type_bits=0x%x dedicated_required=%u\n",
           index, (uint64_t)requirements.memoryRequirements.size,
           (uint64_t)requirements.memoryRequirements.alignment,
           requirements.memoryRequirements.memoryTypeBits,
           dedicated.requiresDedicatedAllocation);
    if (requirements.memoryRequirements.size > capture->length)
        fail_message("Vulkan NV12 image requirement exceeds Iris DMA-BUF");

    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    result = vk->get_memory_fd_properties(
        vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        capture->dma_buf_fd, &fd_properties);
    if (result != VK_SUCCESS)
        fail_vk("vkGetMemoryFdPropertiesKHR", result);
    uint32_t compatible = fd_properties.memoryTypeBits &
                          requirements.memoryRequirements.memoryTypeBits;
    if (!compatible)
        fail_message("DMA-BUF/image memory-type intersection is empty");
    VkMemoryPropertyFlags chosen_flags = 0;
    uint32_t memory_type = choose_memory_type(
        vk->physical, compatible, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &chosen_flags);

    int imported_fd = dup(capture->dma_buf_fd);
    if (imported_fd < 0)
        fail_errno("dup DMA-BUF for Vulkan import");
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = capture->image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = capture->length,
        .memoryTypeIndex = memory_type,
    };
    result = vkAllocateMemory(vk->device, &allocation, NULL,
                              &capture->memory);
    if (result != VK_SUCCESS) {
        close(imported_fd);
        fail_vk("vkAllocateMemory(import NV12 DMA-BUF)", result);
    }
    result = vkBindImageMemory(vk->device, capture->image, capture->memory, 0);
    if (result != VK_SUCCESS)
        fail_vk("vkBindImageMemory(NV12 DMA-BUF)", result);
    printf("dmabuf_import index=%u bytes=%zu fd_type_bits=0x%x"
           " intersection=0x%x memory_type=%u flags=0x%x dedicated=1\n",
           index, capture->length, fd_properties.memoryTypeBits, compatible,
           memory_type, chosen_flags);
    record_transfer_command(vk, capture, layout);
}

static void write_selected_frame(struct vulkan_context *vk,
                                 const char *prefix, uint64_t frame_index)
{
    void *mapped = NULL;
    VkResult result = vkMapMemory(vk->device, vk->transfer_memory, 0,
                                  vk->transfer_size, 0, &mapped);
    if (result != VK_SUCCESS)
        fail_vk("vkMapMemory(selected readback)", result);
    if (!(vk->transfer_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = vk->transfer_memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        result = vkInvalidateMappedMemoryRanges(vk->device, 1, &range);
        if (result != VK_SUCCESS)
            fail_vk("vkInvalidateMappedMemoryRanges(selected)", result);
    }
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s-frame-%08" PRIu64 ".nv12",
                          prefix, frame_index);
    if (length < 0 || (size_t)length >= sizeof(path))
        fail_message("selected-frame output path is too long");
    FILE *file = fopen(path, "wb");
    if (!file)
        fail_errno("open selected-frame output");
    if (fwrite(mapped, 1, (size_t)vk->transfer_size, file) !=
        (size_t)vk->transfer_size)
        fail_errno("write selected-frame output");
    if (fclose(file) != 0)
        fail_errno("close selected-frame output");
    vkUnmapMemory(vk->device, vk->transfer_memory);
    printf("selected_readback frame=%" PRIu64 " path=%s bytes=%" PRIu64
           "\n",
           frame_index, path, (uint64_t)vk->transfer_size);
}

static double execute_transfer(struct vulkan_context *vk,
                               struct capture_buffer *capture,
                               bool selected, const char *output_prefix,
                               uint64_t frame_index, double *wall_ms)
{
    struct dma_buf_export_sync_file sync_export = {
        .flags = DMA_BUF_SYNC_READ,
        .fd = -1,
    };
    if (fd_ioctl(capture->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                 &sync_export) < 0)
        fail_errno("DMA_BUF_IOCTL_EXPORT_SYNC_FILE READ");
    VkImportSemaphoreFdInfoKHR semaphore_import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = vk->producer_semaphore,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = sync_export.fd,
    };
    VkResult result = vk->import_semaphore_fd(vk->device, &semaphore_import);
    if (result != VK_SUCCESS) {
        if (sync_export.fd >= 0)
            close(sync_export.fd);
        fail_vk("vkImportSemaphoreFdKHR(producer SYNC_FD)", result);
    }
    result = vkResetFences(vk->device, 1, &vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkResetFences", result);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &vk->producer_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &capture->command,
    };
    double started = monotonic_seconds();
    result = vkQueueSubmit(vk->queue, 1, &submit, vk->fence);
    if (result != VK_SUCCESS)
        fail_vk("vkQueueSubmit(frame transfer)", result);
    result = vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE,
                             UINT64_C(5000000000));
    double finished = monotonic_seconds();
    if (result != VK_SUCCESS)
        fail_vk("vkWaitForFences(frame transfer)", result);
    *wall_ms = (finished - started) * 1000.0;

    uint64_t timestamps[2] = {0, 0};
    result = vkGetQueryPoolResults(
        vk->device, vk->query_pool, 0, 2, sizeof(timestamps), timestamps,
        sizeof(timestamps[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (result != VK_SUCCESS)
        fail_vk("vkGetQueryPoolResults", result);
    uint64_t mask = vk->timestamp_valid_bits == 64
                        ? UINT64_MAX
                        : (UINT64_C(1) << vk->timestamp_valid_bits) - 1U;
    uint64_t ticks = (timestamps[1] - timestamps[0]) & mask;
    double gpu_ms = (double)ticks * vk->timestamp_period / 1000000.0;
    if (selected)
        write_selected_frame(vk, output_prefix, frame_index);
    return gpu_ms;
}

static unsigned allocate_output_buffers(
    struct output_buffer buffers[MAX_OUTPUT_BUFFERS])
{
    struct v4l2_requestbuffers request = {
        .count = REQUESTED_OUTPUT_BUFFERS,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    must_ioctl(VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS OUTPUT");
    if (!request.count || request.count > MAX_OUTPUT_BUFFERS)
        fail_message("unexpected OUTPUT buffer count");
    for (unsigned i = 0; i < request.count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF OUTPUT");
        if (buffer.length != 1)
            fail_message("OUTPUT is not one memory plane");
        buffers[i].length = plane.length;
        buffers[i].mapping = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, video_fd, plane.m.mem_offset);
        if (buffers[i].mapping == MAP_FAILED)
            fail_errno("mmap OUTPUT");
        buffers[i].free = true;
    }
    printf("output_buffers=%u requested=%u\n", request.count,
           REQUESTED_OUTPUT_BUFFERS);
    return request.count;
}

static void queue_output(unsigned index, struct output_buffer *buffer,
                         struct media_packet *packet)
{
    if (!packet->packet || packet->packet->size <= 0 ||
        (size_t)packet->packet->size > buffer->length)
        fail_message("access unit exceeds OUTPUT allocation");
    memcpy(buffer->mapping, packet->packet->data,
           (size_t)packet->packet->size);
    struct v4l2_plane plane = {
        .bytesused = (uint32_t)packet->packet->size,
        .length = (uint32_t)buffer->length,
    };
    struct v4l2_buffer queued = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
        .length = 1,
        .m.planes = &plane,
    };
    int64_t packet_timestamp = packet->pts_us != AV_NOPTS_VALUE
                                   ? packet->pts_us : packet->dts_us;
    if (packet_timestamp < 0)
        packet_timestamp = 0;
    uint64_t timestamp_us = (uint64_t)packet_timestamp;
    queued.timestamp.tv_sec = (time_t)(timestamp_us / UINT64_C(1000000));
    queued.timestamp.tv_usec =
        (suseconds_t)(timestamp_us % UINT64_C(1000000));
    must_ioctl(VIDIOC_QBUF, &queued, "VIDIOC_QBUF OUTPUT");
    buffer->free = false;
    buffer->pts_us = packet_timestamp;
    buffer->queued_at = monotonic_seconds();
    media_packet_unref(packet);
}

static int export_capture(unsigned index, size_t expected_size)
{
    struct v4l2_exportbuffer exported = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .index = index,
        .plane = 0,
        .flags = O_CLOEXEC | O_RDONLY,
    };
    must_ioctl(VIDIOC_EXPBUF, &exported, "VIDIOC_EXPBUF CAPTURE");
    struct stat status;
    if (fstat(exported.fd, &status) < 0)
        fail_errno("fstat CAPTURE DMA-BUF");
    if (status.st_size < 0 || (uintmax_t)status.st_size != expected_size)
        fail_message("DMA-BUF size differs from V4L2 sizeimage");
    printf("dmabuf_export index=%u fd=%d size=%jd\n", index, exported.fd,
           (intmax_t)status.st_size);
    return exported.fd;
}

static void queue_capture(unsigned index, size_t length)
{
    if (decoded_video) { /* decoded outside Iris: no CAPTURE queue */
        decoded_video_release(decoded_video, index);
        return;
    }
    if (capture_queue_closed)
        return;
    struct v4l2_plane plane = {
        .length = (uint32_t)length,
    };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .index = index,
        .length = 1,
        .m.planes = &plane,
    };
    must_ioctl(VIDIOC_QBUF, &buffer, "VIDIOC_QBUF CAPTURE");
}

static void query_capture_contract(struct capture_contract *contract)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    must_ioctl(VIDIOC_G_FMT, &format, "VIDIOC_G_FMT after SOURCE_CHANGE");
    char fourcc[5] = {
        (char)(format.fmt.pix_mp.pixelformat & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 8) & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 16) & 0xff),
        (char)((format.fmt.pix_mp.pixelformat >> 24) & 0xff),
        0,
    };
    printf("capture_format fourcc=%s width=%u height=%u planes=%u"
           " stride=%u sizeimage=%u colorspace=%u xfer=%u ycbcr=%u quant=%u\n",
           fourcc, format.fmt.pix_mp.width, format.fmt.pix_mp.height,
           format.fmt.pix_mp.num_planes,
           format.fmt.pix_mp.plane_fmt[0].bytesperline,
           format.fmt.pix_mp.plane_fmt[0].sizeimage,
           format.fmt.pix_mp.colorspace, format.fmt.pix_mp.xfer_func,
           format.fmt.pix_mp.ycbcr_enc, format.fmt.pix_mp.quantization);
    struct v4l2_selection selection = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .target = V4L2_SEL_TGT_COMPOSE,
    };
    must_ioctl(VIDIOC_G_SELECTION, &selection, "VIDIOC_G_SELECTION COMPOSE");
    printf("visible_rect left=%d top=%d width=%u height=%u\n",
           selection.r.left, selection.r.top, selection.r.width,
           selection.r.height);
    contract->pixel_format = format.fmt.pix_mp.pixelformat;
    contract->coded_width = format.fmt.pix_mp.width;
    contract->coded_height = format.fmt.pix_mp.height;
    contract->memory_planes = format.fmt.pix_mp.num_planes;
    contract->bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    contract->sizeimage = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    contract->compose = selection.r;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

/* Q10C: Iris writes TP10 with UBWC as Y metadata, Y data, UV metadata and
 * UV data, each 4 KiB aligned (Venus/Iris msm_media_info layout). The
 * renderer imports both planes through the patched Turnip; validated bit
 * exact from 1920x1080 to 8192x4096 (docs/main10-linear-output.md). */
static void validate_tp10_contract(const struct capture_contract *contract,
                                   struct video_layout *layout)
{
    uint32_t width = layout->width, height = layout->coded_height;
    uint64_t meta_pitch = align_up((width + 47U) / 48U, 64U);
    uint64_t y_meta = align_up(meta_pitch * align_up((height + 3U) / 4U, 16U),
                               4096U);
    uint64_t y_data = align_up((uint64_t)layout->stride * align_up(height, 16U),
                               4096U);
    uint32_t uv_height = (height + 1U) / 2U;
    uint64_t uv_meta = align_up(meta_pitch *
                                    align_up((uv_height + 3U) / 4U, 16U),
                                4096U);
    uint64_t uv_data = align_up((uint64_t)layout->stride *
                                    align_up(uv_height, 16U),
                                4096U);
    layout->uv_offset = y_meta + y_data;
    uint64_t required = layout->uv_offset + uv_meta + uv_data;
    if (width % 3U || layout->stride != width / 3U * 4U ||
        required > contract->sizeimage ||
        contract->sizeimage - required >= 65536U)
        fail_message("Q10C sizeimage does not match the TP10 UBWC layout");
    printf("tp10_contract modifier=QCOM_COMPRESSED coded=%ux%u stride=%u"
           " uv_offset=%" PRIu64 " required_bytes=%" PRIu64 "\n",
           width, height, layout->stride, layout->uv_offset, required);
}

static void validate_capture_contract(const struct capture_contract *contract,
                                      struct video_layout *layout,
                                      uint32_t expected_width,
                                      uint32_t expected_visible_height)
{
    bool tp10 = contract->pixel_format == v4l2_fourcc('Q', '1', '0', 'C');
    if ((contract->pixel_format != V4L2_PIX_FMT_NV12 && !tp10) ||
        contract->memory_planes != 1)
        fail_message("Iris did not negotiate one-memory-plane NV12 or Q10C");
    if (contract->compose.left != 0 || contract->compose.top != 0 ||
        contract->compose.width != expected_width ||
        contract->compose.height != expected_visible_height)
        fail_message("Iris visible rectangle differs from input metadata");

    layout->tp10 = tp10;
    layout->width = contract->coded_width;
    layout->coded_height = contract->coded_height;
    layout->visible_width = contract->compose.width;
    layout->visible_height = contract->compose.height;
    layout->stride = contract->bytes_per_line;
    layout->sizeimage = contract->sizeimage;
    /* Iris pads the coded width to its alignment (e.g. 2880 -> 2944); the
     * renderer samples only the visible rectangle. */
    if (layout->width < layout->visible_width || (layout->width & 1U) ||
        layout->coded_height < layout->visible_height ||
        (layout->coded_height & 1U) ||
        (!tp10 && layout->stride < layout->width))
        fail_message("unsupported NV12 dimensions or stride");
    if (tp10) {
        validate_tp10_contract(contract, layout);
        return;
    }
    layout->uv_offset = (uint64_t)layout->stride * layout->coded_height;
    uint64_t required = layout->uv_offset +
                        (uint64_t)layout->stride * layout->coded_height / 2U;
    /* Iris rounds the buffer size up to whole pages (e.g. 2944x1440:
     * 6359040 bytes of picture in a 6361088 byte buffer). */
    if (required > layout->sizeimage ||
        layout->sizeimage - required >= 65536U)
        fail_message("NV12 sizeimage does not match contiguous linear layout");
    printf("nv12_contract modifier=DRM_FORMAT_MOD_LINEAR memory_planes=2"
           " v4l2_memory_planes=1 y_offset=0 uv_offset=%" PRIu64
           " row_pitch=%u required_bytes=%" PRIu64 "\n",
           layout->uv_offset, layout->stride, required);
}

static struct capture_minimum query_capture_minimum(void)
{
    struct v4l2_control minimum = {
        .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE,
    };
    if (xioctl(VIDIOC_G_CTRL, &minimum) < 0) {
        int control_errno = errno;
        if (control_errno == EINVAL || control_errno == ENOTTY) {
            printf("min_buffers_control=unavailable errno=%d\n",
                   control_errno);
            return (struct capture_minimum){0};
        }
        fail_errno("VIDIOC_G_CTRL MIN_BUFFERS_FOR_CAPTURE");
    }
    if (minimum.value < 0)
        fail_message("invalid V4L2_CID_MIN_BUFFERS_FOR_CAPTURE value");
    printf("min_buffers_control=available value=%d\n", minimum.value);
    return (struct capture_minimum){
        .available = true,
        .value = (unsigned)minimum.value,
    };
}

static unsigned setup_capture(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS],
    struct capture_contract *contract, struct video_layout *layout,
    uint32_t expected_width, uint32_t expected_visible_height)
{
    query_capture_contract(contract);
    validate_capture_contract(contract, layout, expected_width,
                              expected_visible_height);

    struct capture_minimum minimum = query_capture_minimum();
    unsigned wanted = 12;
    if (minimum.available) {
        if (minimum.value > MAX_CAPTURE_BUFFERS - 4U)
            fail_message("CAPTURE minimum buffer count exceeds bound");
        wanted = minimum.value + 4U;
    }
    if (wanted > MAX_CAPTURE_BUFFERS)
        fail_message("CAPTURE buffer request exceeds bound");
    struct v4l2_requestbuffers request = {
        .count = wanted,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    must_ioctl(VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS CAPTURE");
    if (!request.count || request.count > MAX_CAPTURE_BUFFERS)
        fail_message("unexpected CAPTURE buffer count");
    for (unsigned i = 0; i < request.count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF CAPTURE");
        if (buffer.length != 1 || plane.length != layout->sizeimage)
            fail_message("CAPTURE allocation differs from sizeimage");
        buffers[i].length = plane.length;
        printf("capture_allocation index=%u length=%zu\n", i,
               buffers[i].length);
        buffers[i].dma_buf_fd = export_capture(i, plane.length);
        queue_capture(i, plane.length);
    }
    printf("capture_buffers=%u exported_dmabufs=%u cpu_mapped=0\n",
           request.count, request.count);
    log_seek_phase("decoder_buffers");
    return request.count;
}

static bool capture_memory_compatible(const struct capture_contract *old,
                                      const struct capture_contract *updated)
{
    return old->pixel_format == updated->pixel_format &&
           old->coded_width == updated->coded_width &&
           old->coded_height == updated->coded_height &&
           old->memory_planes == updated->memory_planes &&
           old->bytes_per_line == updated->bytes_per_line &&
           old->sizeimage == updated->sizeimage;
}

static bool same_compose(const struct v4l2_rect *old,
                         const struct v4l2_rect *updated)
{
    return old->left == updated->left && old->top == updated->top &&
           old->width == updated->width && old->height == updated->height;
}

static void verify_capture_allocations(
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS],
    unsigned capture_count, uint32_t expected_size)
{
    for (unsigned i = 0; i < capture_count; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
            .length = 1,
            .m.planes = &plane,
        };
        must_ioctl(VIDIOC_QUERYBUF, &buffer,
                   "VIDIOC_QUERYBUF CAPTURE reuse");
        if (buffer.length != 1 || plane.length != expected_size ||
            buffers[i].length != plane.length)
            fail_message("existing CAPTURE allocation is incompatible");
        printf("capture_reuse_allocation index=%u length=%u\n", i,
               plane.length);
    }
}

static void release_capture_generation(struct vulkan_context *vk,
                                       struct capture_buffer *captures,
                                       unsigned capture_count)
{
    VkResult result = vkDeviceWaitIdle(vk->device);
    if (result != VK_SUCCESS)
        fail_vk("vkDeviceWaitIdle(release CAPTURE generation)", result);
    for (unsigned i = 0; i < capture_count; i++) {
        if (captures[i].command != VK_NULL_HANDLE)
            vkFreeCommandBuffers(vk->device, vk->command_pool, 1,
                                 &captures[i].command);
        if (captures[i].image != VK_NULL_HANDLE)
            vkDestroyImage(vk->device, captures[i].image, NULL);
        if (captures[i].memory != VK_NULL_HANDLE)
            vkFreeMemory(vk->device, captures[i].memory, NULL);
        if (captures[i].dma_buf_fd >= 0 && close(captures[i].dma_buf_fd) < 0)
            fail_errno("close CAPTURE DMA-BUF");
        memset(&captures[i], 0, sizeof(captures[i]));
        captures[i].dma_buf_fd = -1;
    }
    if (vk->transfer_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(vk->device, vk->transfer_buffer, NULL);
    if (vk->transfer_memory != VK_NULL_HANDLE)
        vkFreeMemory(vk->device, vk->transfer_memory, NULL);
    vk->transfer_buffer = VK_NULL_HANDLE;
    vk->transfer_memory = VK_NULL_HANDLE;
    vk->transfer_size = 0;
    vk->transfer_memory_flags = 0;
}

static void cleanup_vulkan(struct vulkan_context *vk,
                           struct capture_buffer *captures,
                           unsigned capture_count)
{
    if (vk->device != VK_NULL_HANDLE)
        release_capture_generation(vk, captures, capture_count);
    if (vk->query_pool != VK_NULL_HANDLE)
        vkDestroyQueryPool(vk->device, vk->query_pool, NULL);
    if (vk->fence != VK_NULL_HANDLE)
        vkDestroyFence(vk->device, vk->fence, NULL);
    if (vk->producer_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(vk->device, vk->producer_semaphore, NULL);
    if (vk->command_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    if (vk->device != VK_NULL_HANDLE)
        vkDestroyDevice(vk->device, NULL);
    if (vk->instance != VK_NULL_HANDLE)
        vkDestroyInstance(vk->instance, NULL);
}

/* Pause and seek state of the playing title. */
struct transport {
    bool paused;
    double pause_started;
    uint32_t requested;         /* NV12_ACTION_* that ends the playback loop */
    int64_t requested_start_us; /* its seek target */
    int64_t from_us;            /* and the position it was requested at */
    double requested_at;        /* monotonic_seconds */
    double last_status_sent;
};

/* --seek-test: the next seek once `frames` frames were shown. */
static void run_test_seek(struct playback_options *options, uint64_t frames)
{
    unsigned next = options->test_seek_next;
    if (next >= options->test_seek_count ||
        frames < options->test_seek_frames[next])
        return;
    renderer_action = NV12_ACTION_SEEK_TO;
    renderer_action_position_us = options->test_seek_us[next];
    options->test_seek_next++;
    printf("seek_test frames=%" PRIu64 " target_us=%" PRId64 "\n", frames,
           options->test_seek_us[next]);
}

/* Applies the renderer's last player action and reports the position to
 * the renderer and Jellyfin four times a second. Returns how long a pause
 * lasted that ended now (deadlines move by it), or -1 on failure. */
static double update_transport(struct transport *transport,
                               struct media_pipeline *pipeline,
                               const struct media_stream_info *info,
                               int64_t start_us, bool audio_started,
                               double now)
{
    double resumed_after = 0.0;
    if (renderer_action == NV12_ACTION_TOGGLE_PAUSE) {
        char error[512] = {0};
        transport->paused = !transport->paused;
        if (media_pipeline_pause(pipeline, transport->paused, error,
                                 sizeof(error)) < 0) {
            fprintf(stderr, "change playback pause state: %s\n", error);
            return -1.0;
        }
        if (transport->paused) {
            transport->pause_started = now;
        } else if (transport->pause_started > 0.0) {
            resumed_after = now - transport->pause_started;
            transport->pause_started = 0.0;
        }
        printf("player_state=%s position_us=%" PRId64 "\n",
               transport->paused ? "PAUSED" : "PLAYING",
               media_pipeline_clock_us(pipeline));
        send_renderer_status(transport->paused,
                             status_position_us(pipeline, start_us),
                             info->duration_us);
        playback_report_update(jellyfin_report,
                               status_position_us(pipeline, start_us),
                               transport->paused, audio_started);
        transport->last_status_sent = now;
    } else if (renderer_action == NV12_ACTION_SEEK_BACK ||
               renderer_action == NV12_ACTION_SEEK_FORWARD ||
               renderer_action == NV12_ACTION_SEEK_TO) {
        int64_t position = media_pipeline_clock_us(pipeline);
        if (position == AV_NOPTS_VALUE)
            position = start_us;
        int64_t target;
        if (renderer_action == NV12_ACTION_SEEK_TO) {
            target = renderer_action_position_us;
        } else {
            int64_t delta = renderer_action == NV12_ACTION_SEEK_BACK
                                ? -INT64_C(10000000) : INT64_C(10000000);
            target = position + delta;
        }
        if (target < 0)
            target = 0;
        if (info->duration_us > 0 && target >= info->duration_us)
            target = info->duration_us - 1;
        transport->requested_start_us = target;
        transport->from_us = position;
        transport->requested_at = now;
        transport->requested = renderer_action;
        printf("player_action=%u position_us=%" PRId64 " target_us=%" PRId64
               "\n",
               renderer_action, position, target);
    } else if (renderer_action == NV12_ACTION_BACK) {
        transport->requested = renderer_action;
        printf("player_action=back\n");
    }
    renderer_action = 0;
    if (!transport->requested && renderer_fd >= 0 &&
        now - transport->last_status_sent >= 0.25) {
        send_renderer_status(transport->paused,
                             status_position_us(pipeline, start_us),
                             info->duration_us);
        playback_report_update(jellyfin_report,
                               status_position_us(pipeline, start_us),
                               transport->paused, audio_started);
        transport->last_status_sent = now;
    }
    return resumed_after;
}

/* Before the renderer starts: projection and layout from the name or the
 * trickplay tiles, the title's view state, and whether the first frames
 * are analysed (NV12 only; TP10 is UBWC-compressed). */
static void prepare_view(struct playback_options *options,
                         const struct video_layout *layout)
{
    detect_format_before_start(options, layout->visible_width,
                               layout->visible_height);
    if (!view_state_valid) {
        view_state = (struct title_settings){
            .projection_mode = options->projection_mode,
            .fisheye_fov = options->fisheye_fov,
            .stereo_layout = options->stereo_layout,
            .swap_eyes = options->swap_eyes,
            .video_levels = options->video_levels,
            .format_source = format_source,
        };
        view_state_valid = true;
    }
    frame_detection.active =
        format_source == NV12_FORMAT_SOURCE_DEFAULT && !layout->tp10;
}

/* The view the viewer chose during playback, for the next start of the
 * decoder (seek restart or reopen). */
static void keep_view(struct playback_options *options)
{
    if (view_state_valid) {
        options->projection_mode = view_state.projection_mode;
        options->fisheye_fov = view_state.fisheye_fov;
        options->stereo_layout = view_state.stereo_layout;
        options->swap_eyes = view_state.swap_eyes;
        options->video_levels = view_state.video_levels;
    }
}

/* Seek restart: exec this program again at the new position with the
 * current view, panel and renderer, which the new process adopts. */
static void restart_session(char **argv, struct playback_options *options,
                            int64_t start_us, bool paused)
{
    char start_text[32];
    snprintf(start_text, sizeof(start_text), "%" PRId64, start_us);
    keep_view(options);
    const char *projection = options->projection_mode ==
                                     NV12_PROJECTION_EQUIRECT_180
                                 ? "180"
                             : options->projection_mode ==
                                       NV12_PROJECTION_EQUIRECT_360
                                 ? "360"
                             : options->projection_mode ==
                                       NV12_PROJECTION_FISHEYE
                                 ? "fisheye" : "flat";
    const char *stereo = options->stereo_layout == NV12_STEREO_SBS
                             ? "sbs"
                         : options->stereo_layout == NV12_STEREO_TB
                             ? "tb" : "mono";
    static const char *const levels_names[] = {"normal", "deep", "full"};
    const char *levels = options->video_levels <= NV12_LEVELS_FULL
                             ? levels_names[options->video_levels]
                             : "normal";
    printf("player_restart target_us=%s projection=%s stereo=%s"
           " levels=%s\n",
           start_text, projection, stereo, levels);
    char panel_text[96] = {0};
    if (panel_state.valid)
        snprintf(panel_text, sizeof(panel_text), "%.6f,%.6f,%.6f,%.6f,%u",
                 panel_state.x, panel_state.y, panel_state.z,
                 panel_state.yaw, panel_state.visible ? 1U : 0U);
    jellyfin_restarting = true;
    jellyfin_cleanup();
    char renderer_text[48] = {0};
    bool keep_renderer = renderer_fd >= 0;
    if (keep_renderer) {
        int fd_flags = fcntl(renderer_fd, F_GETFD);
        if (fd_flags < 0 ||
            fcntl(renderer_fd, F_SETFD, fd_flags & ~FD_CLOEXEC) < 0)
            fail_errno("keep renderer IPC across exec");
        snprintf(renderer_text, sizeof(renderer_text), "%d,%ld",
                 renderer_fd, (long)renderer_pid);
    }
    char *restart_argv[28];
    size_t restart_argc = 0;
    restart_argv[restart_argc++] = argv[0];
    restart_argv[restart_argc++] = argv[1];
    restart_argv[restart_argc++] = "--projection";
    restart_argv[restart_argc++] = (char *)projection;
    restart_argv[restart_argc++] = "--stereo";
    restart_argv[restart_argc++] = (char *)stereo;
    restart_argv[restart_argc++] =
        options->swap_eyes ? "--swap-eyes" : "--no-swap-eyes";
    restart_argv[restart_argc++] = "--levels";
    restart_argv[restart_argc++] = (char *)levels;
    restart_argv[restart_argc++] = "--start-us";
    restart_argv[restart_argc++] = start_text;
    char fisheye_text[16];
    snprintf(fisheye_text, sizeof(fisheye_text), "%u",
             options->fisheye_fov);
    restart_argv[restart_argc++] = "--fisheye-fov";
    restart_argv[restart_argc++] = fisheye_text;
    restart_argv[restart_argc++] = "--format-source";
    restart_argv[restart_argc++] =
        (char *)format_source_names[format_source];
    if (panel_state.valid) {
        restart_argv[restart_argc++] = "--panel";
        restart_argv[restart_argc++] = panel_text;
    }
    if (keep_renderer) {
        restart_argv[restart_argc++] = "--renderer";
        restart_argv[restart_argc++] = renderer_text;
    }
    if (strncmp(argv[1], "jf:item:", 8) == 0)
        restart_argv[restart_argc++] = "--jellyfin-resumed";
    if (paused)
        restart_argv[restart_argc++] = "--paused";
    if (options->no_seek_in_place)
        restart_argv[restart_argc++] = "--no-seek-in-place";
    char requested_text[32];
    if (seek_requested_at > 0.0) {
        snprintf(requested_text, sizeof(requested_text), "%.6f",
                 seek_requested_at);
        restart_argv[restart_argc++] = "--seek-requested-at";
        restart_argv[restart_argc++] = requested_text;
    }
    char from_text[32];
    if (options->seek_from_us >= 0) {
        snprintf(from_text, sizeof(from_text), "%" PRId64,
                 options->seek_from_us);
        restart_argv[restart_argc++] = "--seek-from-us";
        restart_argv[restart_argc++] = from_text;
    }
    restart_argv[restart_argc] = NULL;
    execv(argv[0], restart_argv);
    fail_errno("restart stereonaut-session after seek");
}

/* Playback of an AV1 title with dav1d (decoded-video.h): the renderer
 * protocol, pacing against the audio clock and seek restart of the Iris
 * loop, with frames from the decoder thread instead of CAPTURE buffers. */
static int play_decoded(char **argv, struct playback_options *options,
                        struct media_pipeline *pipeline,
                        double source_duration,
                        enum decoded_video_decoder decoder)
{
    const struct media_stream_info *info = media_pipeline_info(pipeline);
    char error[512] = {0};
    if (decoded_video_open(&decoded_video, pipeline, options->start_us,
                           decoder, error, sizeof(error)) < 0) {
        fprintf(stderr, "open video decoder: %s\n", error);
        return EXIT_UNSUPPORTED_FORMAT;
    }
    const struct decoded_video_layout *frames =
        decoded_video_layout(decoded_video);
    struct capture_buffer buffers[MAX_CAPTURE_BUFFERS];
    memset(buffers, 0, sizeof(buffers));
    for (unsigned i = 0; i < frames->count; ++i) {
        buffers[i].dma_buf_fd = frames->fds[i];
        buffers[i].length = (size_t)frames->size;
        frame_detection.mappings[i] = frames->mappings[i];
    }
    struct video_layout layout = {
        .width = frames->width,
        .coded_height = frames->height,
        .visible_width = frames->width,
        .visible_height = frames->height,
        .stride = frames->stride,
        .sizeimage = (uint32_t)frames->size,
        .uv_offset = frames->uv_offset,
        .p010 = frames->p010,
        .host_memory = frames->host_memory,
    };
    printf("decoded_video decoder=%s format=%s buffers=%u stride=%u"
           " uv_offset=%" PRIu64 " bytes=%" PRIu64 "\n",
           decoded_video_decoder_name(decoded_video),
           frames->p010 ? "p010" : "nv12", frames->count, frames->stride,
           frames->uv_offset, frames->size);
    prepare_view(options, &layout);
    start_renderer(buffers, frames->count, &layout, options);

    struct transport transport = {0};
    bool audio_started = false;
    double playback_start = monotonic_seconds();
    double last_progress = playback_start;
    double seek_started = seek_requested_at;
    uint64_t shown = 0, late_drops = 0;
    for (;;) {
        if (stop_requested) {
            printf("player_stop=signal\n");
            exit(0);
        }
        if (process_renderer_messages(buffers, frames->count))
            fail_message("unexpected renderer FLUSHED response");
        double now = monotonic_seconds();
        run_test_seek(options, shown);
        if (update_transport(&transport, pipeline, info, options->start_us,
                             audio_started, now) < 0.0)
            return 2;
        if (transport.requested && transport.requested != NV12_ACTION_BACK) {
            /* Seek within the session; a failure falls back to the
             * restart below. */
            send_renderer_message(NV12_IPC_SEEKING, 0, 0);
            decoded_video_stop(decoded_video);
            if (media_pipeline_seek(pipeline, transport.requested_start_us,
                                    transport.from_us,
                                    true, error, sizeof(error)) < 0) {
                fprintf(stderr, "seek: %s\n", error);
                break;
            }
            options->start_us = media_pipeline_start_us(pipeline);
            decoded_video_restart(decoded_video, options->start_us);
            printf("seek_in_place requested_us=%" PRId64 " start_us=%" PRId64
                   "\n", transport.requested_start_us, options->start_us);
            /* Paused: show the new position and stay paused. */
            options->start_paused = transport.paused;
            transport = (struct transport){
                .last_status_sent = transport.last_status_sent,
            };
            audio_started = false;
            seek_started = now;
            last_progress = now;
            continue;
        }
        if (transport.requested)
            break;
        if (transport.paused) {
            last_progress = now;
            struct pollfd pollfd = {.fd = renderer_fd, .events = POLLIN};
            if (poll(&pollfd, 1, 20) < 0 && errno != EINTR)
                fail_errno("poll renderer");
            continue;
        }
        unsigned index;
        int64_t pts_us;
        int next = decoded_video_next(decoded_video, &index, &pts_us, 20,
                                      error, sizeof(error));
        if (next < 0 && error[0]) {
            fprintf(stderr, "AV1 software decoding: %s\n", error);
            return 2;
        }
        if (next < 0)
            break;
        if (next == 0) {
            if (now - last_progress > 10.0)
                fail_message("no decoder progress for 10 seconds");
            continue;
        }
        last_progress = monotonic_seconds();
        if (seek_started > 0.0) {
            printf("seek_latency_ms=%.0f\n",
                   (last_progress - seek_started) * 1000.0);
            seek_started = 0.0;
            seek_requested_at = 0.0;
        }
        if (!audio_started && options->start_paused) {
            /* The audio clock starts with the next "play". */
            transport.paused = true;
            transport.pause_started = last_progress;
            audio_started = true;
            printf("player_state=PAUSED start_paused=1"
                   " first_video_pts_us=%" PRId64 "\n", pts_us);
        } else if (!audio_started) {
            if (media_pipeline_pause(pipeline, false, error,
                                     sizeof(error)) < 0) {
                fprintf(stderr, "start audio playback: %s\n", error);
                return 2;
            }
            audio_started = true;
            printf("audio_started first_video_pts_us=%" PRId64 "\n", pts_us);
        }
        int64_t audio_clock = media_pipeline_clock_us(pipeline);
        while (audio_clock != AV_NOPTS_VALUE &&
               pts_us > audio_clock + INT64_C(10000)) {
            int64_t wait_us = pts_us - audio_clock - 10000;
            if (wait_us > 5000)
                wait_us = 5000;
            struct timespec delay = {.tv_sec = 0, .tv_nsec = wait_us * 1000};
            nanosleep(&delay, NULL);
            if (process_renderer_messages(buffers, frames->count))
                fail_message("unexpected renderer FLUSHED response");
            audio_clock = media_pipeline_clock_us(pipeline);
        }
        bool late = audio_clock != AV_NOPTS_VALUE &&
                    pts_us + VIDEO_LATE_DROP_US < audio_clock;
        if (late) {
            late_drops++;
            decoded_video_release(decoded_video, index);
        } else {
            detect_format_from_frame(options, buffers, index, &layout,
                                     pts_us);
            send_renderer_message(NV12_IPC_FRAME, index, shown);
            renderer_outstanding[index] = true;
        }
        if (shown % 300U == 0)
            printf("frame=%" PRIu64 " buffer=%u timestamp_us=%" PRId64
                   " audio_clock_us=%" PRId64 " late_drops=%" PRIu64 "\n",
                   shown, index, pts_us, audio_clock, late_drops);
        if (++shown == options->frame_limit)
            break;
    }

    if (transport.requested) {
        if (media_pipeline_pause(pipeline, true, error, sizeof(error)) < 0) {
            fprintf(stderr, "pause before player control: %s\n", error);
            return 2;
        }
        if (transport.requested == NV12_ACTION_BACK)
            wait_renderer(false);
        else
            hold_renderer(buffers, frames->count);
        decoded_video_close(&decoded_video);
        volume_pipeline = NULL;
        media_pipeline_close(&pipeline);
        if (transport.requested == NV12_ACTION_BACK)
            return 0;
        options->seek_from_us = transport.from_us;
        seek_requested_at = transport.requested_at;
        restart_session(argv, options, transport.requested_start_us,
                        transport.paused);
    }

    double elapsed = monotonic_seconds() - playback_start;
    printf("summary decoder=%s frames=%" PRIu64
           " late_video_drops=%" PRIu64 " seek_preroll_drops=%" PRIu64
           " elapsed_s=%.3f source_duration_s=%.3f achieved_fps=%.3f\n",
           decoded_video_decoder_name(decoded_video), shown, late_drops,
           decoded_video_preroll_drops(decoded_video),
           elapsed, source_duration, elapsed > 0.0 ? shown / elapsed : 0.0);
    wait_renderer(options->frame_limit == 0); /* test runs stop after N */
    decoded_video_close(&decoded_video);
    volume_pipeline = NULL;
    media_pipeline_close(&pipeline);
    printf("result=success\n");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s INPUT.mkv|INPUT.mp4"
                " [--projection flat|180|360|fisheye] [--fisheye-fov DEG]"
                " [--stereo mono|sbs|tb] [--swap-eyes|--no-swap-eyes]"
                " [--levels normal|deep|full]"
                " [--format-source default|name|image|user] [--paused]"
                " [--frames N] [--seek-test FRAMES:TARGET_US]"
                " [--start-us TIMESTAMP]\n",
                argv[0]);
        return 2;
    }
    struct sigaction stop_action = {.sa_handler = handle_stop};
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGTERM, &stop_action, NULL);
    struct playback_options options = parse_playback_options(argc, argv);
    log_seek_phase("exec");
    panel_state = options.panel;
    title_settings_key(argv[1], title_key, sizeof(title_key));
    {
        struct title_settings stored;
        title_settings_defaults(&stored);
        if (title_settings_load(title_key, &stored) < 0)
            fprintf(stderr, "title settings unreadable; using defaults\n");
        if (!options.projection_set)
            options.projection_mode = stored.projection_mode;
        if (!options.fisheye_fov_set)
            options.fisheye_fov = stored.fisheye_fov;
        if (!options.stereo_set)
            options.stereo_layout = stored.stereo_layout;
        if (!options.swap_eyes_set)
            options.swap_eyes = stored.swap_eyes;
        if (!options.levels_set)
            options.video_levels = stored.video_levels;
        if (options.format_source_set)
            format_source = options.format_source;
        else if (options.projection_set && options.stereo_set)
            format_source = NV12_FORMAT_SOURCE_USER;
        else
            format_source = stored.format_source;
    }
    bool detect_format = !options.format_source_set &&
                         format_source == NV12_FORMAT_SOURCE_DEFAULT;
    /* Opt-in AV1 software decoding: the demuxer accepts AV1 and the
     * Jellyfin profile offers it (decoded-video.h). */
    if (settings_get_bool("av1_software", false))
        setenv(AV1_SOFTWARE_ENV, "1", 1);
    char pipeline_error[512] = {0};
    struct media_pipeline *pipeline = NULL;
    char input_url[2304];
    const char *input = argv[1];
    if (strncmp(argv[1], "jf:item:", 8) == 0) {
        int resolved = resolve_jellyfin_input(argv[1],
                                              options.jellyfin_resumed,
                                              input_url, sizeof(input_url));
        log_seek_phase("jellyfin");
        if (stop_requested)
            return 0;
        if (resolved == JF_BROKEN_ITEM)
            return EXIT_BROKEN_SOURCE;
        if (resolved < 0)
            return EXIT_SOURCE_FAILED;
        input = input_url;
        if (detect_format) {
            format_from_name(jellyfin_playback.file_name, &format_name_guess);
            format_from_3d_format(jellyfin_playback.video_3d_format,
                                  &format_name_guess);
            if (format_name_guess.projection == FORMAT_UNKNOWN ||
                format_name_guess.stereo == FORMAT_UNKNOWN)
                start_trickplay_detection(jellyfin_playback.item_id);
        }
    } else if (strncmp(argv[1], "webdav:play:", 12) == 0) {
        if (resolve_webdav_input(argv[1], input_url, sizeof(input_url)) < 0)
            return EXIT_SOURCE_FAILED;
        input = input_url;
        if (detect_format)
            format_from_name(input_url, &format_name_guess);
    } else if (detect_format) {
        format_from_name(argv[1], &format_name_guess);
    }
    if (media_pipeline_open(&pipeline, input, options.start_us,
                            options.seek_from_us, true, true,
                            pipeline_error, sizeof(pipeline_error)) < 0) {
        if (stop_requested)
            return 0;
        fprintf(stderr, "open media pipeline: %s\n", pipeline_error);
        if (strstr(pipeline_error, "unsupported video"))
            return EXIT_UNSUPPORTED_FORMAT;
        return strncmp(input, "http", 4) == 0 ? EXIT_SOURCE_FAILED : 2;
    }
    volume_pipeline = pipeline;
    log_seek_phase("pipeline_open");
    if (options.start_us > 0) {
        /* Playback starts at the keyframe nearest to the requested time. */
        printf("seek_keyframe requested_us=%" PRId64 " start_us=%" PRId64
               "\n", options.start_us, media_pipeline_start_us(pipeline));
        options.start_us = media_pipeline_start_us(pipeline);
    }
    const struct media_stream_info *info = media_pipeline_info(pipeline);
    uint32_t expected_width = (uint32_t)info->width;
    uint32_t expected_height = (uint32_t)info->height;
    uint32_t fps_numerator = (uint32_t)info->frame_rate.num;
    uint32_t fps_denominator = (uint32_t)info->frame_rate.den;
    if (!expected_width || !expected_height || !fps_numerator ||
        !fps_denominator)
        fail_message("container has invalid video dimensions or frame rate");
    if ((expected_width & 1U) || (expected_height & 1U))
        fail_message("NV12 requires even visible dimensions");

    double frame_interval =
        (double)fps_denominator / (double)fps_numerator;
    double source_duration = info->duration_us > 0
                                 ? (double)info->duration_us / 1000000.0
                                 : 0.0;
    if (options.frame_limit) {
        double limited_duration = options.frame_limit * frame_interval;
        if (source_duration <= 0.0 || limited_duration < source_duration)
            source_duration = limited_duration;
    }
    printf("source width=%u visible_height=%u fps=%u/%u duration_s=%.6f\n",
           expected_width, expected_height, fps_numerator, fps_denominator,
           source_duration);
    printf("playback_config projection=%u stereo=%u swap_eyes=%u levels=%u"
           " frame_limit=%u\n",
           options.projection_mode, options.stereo_layout,
           options.swap_eyes, options.video_levels, options.frame_limit);
    if (options.start_us)
        printf("playback_start_target_us=%" PRId64 "\n", options.start_us);
    /* PCVR: without the Frame's Iris the GPU's decoder takes every codec;
     * on the Frame AV1 goes to dav1d (opt-in, checked by the demuxer). */
    if (access(DECODED_VIDEO_IRIS_DEVICE, F_OK) < 0)
        return play_decoded(argv, &options, pipeline, source_duration,
                            DECODED_VIDEO_HARDWARE);
    if (info->video_codec == AV_CODEC_ID_AV1)
        return play_decoded(argv, &options, pipeline, source_duration,
                            DECODED_VIDEO_DAV1D);

    /* Keep the validated transfer implementation available as diagnostic
     * source, while Vulkan and OpenXR live entirely in the renderer process.
     * Forking after Vulkan device creation has no supported process model. */
    (void)read_input;
    (void)initialize_vulkan;
    (void)create_transfer_buffer;
    (void)import_capture_image;
    (void)execute_transfer;
    (void)cleanup_vulkan;

    /* A seek that reopens the decoder starts over here (see below). */
open_decoder:
    video_fd = open(DECODED_VIDEO_IRIS_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video_fd < 0)
        fail_errno("open /dev/video-dec0");
    /* Safety net: release the HFI session even if we abort before the normal
     * teardown (e.g. REQBUFS fails with -12 on an 8K session). */
    static bool decoder_atexit_registered;
    if (!decoder_atexit_registered) {
        atexit(release_decoder);
        decoder_atexit_registered = true;
    }
    struct v4l2_capability capability = {0};
    must_ioctl(VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP");
    printf("v4l2_device=/dev/video-dec0 driver=%s card=%s bus=%s\n",
           capability.driver, capability.card, capability.bus_info);
    if (strcmp((char *)capability.driver, "iris_driver") != 0)
        fail_message("/dev/video-dec0 is not iris_driver");
    struct v4l2_event_subscription source_subscription = {
        .type = V4L2_EVENT_SOURCE_CHANGE,
    };
    must_ioctl(VIDIOC_SUBSCRIBE_EVENT, &source_subscription,
               "VIDIOC_SUBSCRIBE_EVENT SOURCE_CHANGE");
    struct v4l2_event_subscription eos_subscription = {
        .type = V4L2_EVENT_EOS,
    };
    must_ioctl(VIDIOC_SUBSCRIBE_EVENT, &eos_subscription,
               "VIDIOC_SUBSCRIBE_EVENT EOS");

    struct v4l2_format output_format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    output_format.fmt.pix_mp.width = expected_width;
    output_format.fmt.pix_mp.height = expected_height;
    bool h264 = info->video_codec == AV_CODEC_ID_H264;
    uint32_t coded_format = h264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
    const char *coded_name = h264 ? "H264" : "HEVC";
    output_format.fmt.pix_mp.pixelformat = coded_format;
    output_format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    output_format.fmt.pix_mp.num_planes = 1;
    output_format.fmt.pix_mp.plane_fmt[0].sizeimage =
        COMPRESSED_OUTPUT_CAPACITY;
    must_ioctl(VIDIOC_S_FMT, &output_format, "VIDIOC_S_FMT OUTPUT");
    if (output_format.fmt.pix_mp.pixelformat != coded_format ||
        output_format.fmt.pix_mp.num_planes != 1 ||
        !output_format.fmt.pix_mp.plane_fmt[0].sizeimage)
        fail_message("Iris rejected the OUTPUT format");
    printf("output_format fourcc=%s width=%u height=%u sizeimage=%u\n",
           coded_name,
           output_format.fmt.pix_mp.width, output_format.fmt.pix_mp.height,
           output_format.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_format initial_capture = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    initial_capture.fmt.pix_mp.width = expected_width;
    initial_capture.fmt.pix_mp.height = expected_height;
    initial_capture.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    initial_capture.fmt.pix_mp.field = V4L2_FIELD_NONE;
    initial_capture.fmt.pix_mp.num_planes = 1;
    must_ioctl(VIDIOC_S_FMT, &initial_capture,
               "VIDIOC_S_FMT initial CAPTURE NV12");
    if (initial_capture.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)
        fail_message("Iris substituted the requested NV12 format");

    struct output_buffer outputs[MAX_OUTPUT_BUFFERS] = {0};
    unsigned output_count = allocate_output_buffers(outputs);
    struct capture_buffer captures[MAX_CAPTURE_BUFFERS];
    memset(captures, 0, sizeof(captures));
    for (unsigned i = 0; i < MAX_CAPTURE_BUFFERS; i++)
        captures[i].dma_buf_fd = -1;
    struct video_layout layout = {0};
    struct capture_contract capture_contract = {0};
    unsigned capture_count = 0;

    struct media_packet next_packet = {0};
    int packet_result = media_pipeline_pop_video(
        pipeline, &next_packet, -1, pipeline_error, sizeof(pipeline_error));
    if (packet_result != 1) {
        fprintf(stderr, "read first video packet: %s\n", pipeline_error);
        return 2;
    }
    double initial_start = monotonic_seconds();
    queue_output(0, &outputs[0], &next_packet);
    uint64_t queued_access_units = 1;
    bool have_next_packet = false;
    bool video_eof = options.frame_limit == 1U;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    must_ioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON OUTPUT");
    bool capture_started = false;
    bool source_change_pending = false;
    bool drain_sent = false;
    bool last_seen = false;
    bool eos_seen = false;
    bool audio_started = false;
    struct transport transport = {0};
    double playback_start = 0.0;
    double deadline = initial_start + 10.0;
    double last_progress = initial_start;
    uint64_t frames = 0;
    uint64_t sequence_drops = 0;
    uint64_t v4l2_errors = 0;
    /* CAPTURE buffers Iris flags as erroneous right after a seek (leading
     * pictures that reference frames before the entry point) are dropped. */
    uint64_t capture_error_drops = 0;
    uint64_t consecutive_capture_errors = 0;
    uint32_t previous_sequence = 0;
    bool have_sequence = false;
    uint64_t output_returned = 0;
    uint64_t source_change_events = 0;
    uint64_t capture_reconfigurations = 0;
    uint64_t late_video_drops = 0;
    uint64_t seek_preroll_drops = 0;
    struct timing_stats decode_stats = {0};
    double maximum_feed_lateness_ms = 0.0;
    double seek_started = seek_requested_at;

    while (!last_seen) {
        if (stop_requested) {
            printf("player_stop=signal\n");
            exit(0);
        }
        if (renderer_fd >= 0 &&
            process_renderer_messages(captures, capture_count))
            fail_message("unexpected renderer FLUSHED response");
        double now = monotonic_seconds();
        run_test_seek(&options, frames);
        bool was_paused = transport.paused;
        double resumed_after = update_transport(&transport, pipeline, info,
                                                options.start_us,
                                                audio_started, now);
        if (resumed_after < 0.0)
            return 2;
        deadline += resumed_after;
        if (transport.paused != was_paused)
            last_progress = now;
        if (transport.requested && transport.requested != NV12_ACTION_BACK &&
            capture_started && !source_change_pending &&
            !options.no_seek_in_place && !layout.tp10) {
            /* Seek within the session (stateful decoder seek: both queues
             * off, coded data from the new keyframe, queues on again); a
             * failure falls back to reopening the decoder below. Not with
             * Main10 (TP10): Iris refused STREAMON every time, also with
             * only the OUTPUT queue stopped and restarted. */
            send_renderer_message(NV12_IPC_SEEKING, 0, 0);
            enum v4l2_buf_type output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            enum v4l2_buf_type capture_type =
                V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            /* From here on a failure falls back to the restart, which
             * closes the decoder; the queues stay as they are. */
            capture_queue_closed = true;
            if (xioctl(VIDIOC_STREAMOFF, &output_type) < 0 ||
                xioctl(VIDIOC_STREAMOFF, &capture_type) < 0) {
                fprintf(stderr, "seek: STREAMOFF errno=%d\n", errno);
                break;
            }
            for (unsigned i = 0; i < output_count; ++i) {
                outputs[i].free = true;
                outputs[i].queued_at = 0.0;
            }
            if (have_next_packet) {
                media_packet_unref(&next_packet);
                have_next_packet = false;
            }
            if (media_pipeline_seek(pipeline, transport.requested_start_us,
                                    transport.from_us,
                                    true, pipeline_error,
                                    sizeof(pipeline_error)) < 0) {
                fprintf(stderr, "seek: %s\n", pipeline_error);
                break;
            }
            options.start_us = media_pipeline_start_us(pipeline);
            /* Buffers the renderer still shows are queued on RELEASE. */
            capture_queue_closed = false;
            for (unsigned i = 0; i < capture_count; ++i)
                if (!renderer_outstanding[i])
                    queue_capture(i, captures[i].length);
            if (xioctl(VIDIOC_STREAMON, &capture_type) < 0 ||
                xioctl(VIDIOC_STREAMON, &output_type) < 0) {
                fprintf(stderr, "seek: STREAMON errno=%d\n", errno);
                capture_queue_closed = true;
                options.no_seek_in_place = true;
                break;
            }
            printf("seek_in_place requested_us=%" PRId64 " start_us=%" PRId64
                   "\n", transport.requested_start_us, options.start_us);
            /* Access units still in the decoder were dropped. */
            queued_access_units = frames + capture_error_drops;
            video_eof = false;
            drain_sent = false;
            eos_seen = false;
            have_sequence = false;
            consecutive_capture_errors = 0;
            audio_started = false;
            /* Paused: show the new position and stay paused. */
            options.start_paused = transport.paused;
            transport = (struct transport){
                .last_status_sent = transport.last_status_sent,
            };
            seek_started = now;
            last_progress = now;
            continue;
        }
        if (transport.requested)
            break;
        if (transport.paused)
            last_progress = now;
        if (!transport.paused && now > deadline)
            fail_message("probe deadline exceeded");
        if (capture_started && !transport.paused &&
            now - last_progress > 10.0)
            fail_message("no decoder/GPU progress for 10 seconds");
        int timeout_ms = 20;
        struct pollfd descriptor = {
            .fd = video_fd,
            .events = POLLIN | POLLOUT | POLLPRI,
        };
        int poll_result = poll(&descriptor, 1, timeout_ms);
        if (poll_result < 0 && errno != EINTR)
            fail_errno("poll Iris");

        for (;;) {
            struct v4l2_event event = {0};
            if (xioctl(VIDIOC_DQEVENT, &event) < 0) {
                if (errno != EAGAIN && errno != ENOENT)
                    fail_errno("VIDIOC_DQEVENT");
                break;
            }
            if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
                if (!(event.u.src_change.changes &
                      V4L2_EVENT_SRC_CH_RESOLUTION))
                    fail_message("SOURCE_CHANGE lacks RESOLUTION flag");
                source_change_events++;
                if (!capture_started) {
                    capture_count = setup_capture(captures,
                                                  &capture_contract, &layout,
                                                  expected_width,
                                                  expected_height);
                    prepare_view(&options, &layout);
                    start_renderer(captures, capture_count, &layout,
                                   &options);
                    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    must_ioctl(VIDIOC_STREAMON, &type,
                               "VIDIOC_STREAMON CAPTURE");
                    capture_started = true;
                    playback_start = monotonic_seconds();
                    last_progress = playback_start;
                    deadline = playback_start +
                               (source_duration > 0.0
                                    ? source_duration * 3.0 + 30.0
                                    : 24.0 * 60.0 * 60.0);
                    printf("source_change event=%" PRIu64
                           " phase=initial changes=0x%x\n",
                           source_change_events,
                           event.u.src_change.changes);
                    printf("playback_started paced=1 maximum_runtime_factor=3"
                           " deadline_margin_s=30 stall_timeout_s=10\n");
                } else {
                    if (source_change_pending)
                        fail_message("overlapping SOURCE_CHANGE events");
                    source_change_pending = true;
                    last_progress = monotonic_seconds();
                    printf("source_change event=%" PRIu64
                           " phase=dynamic changes=0x%x frames=%" PRIu64
                           " action=drain-old-capture\n",
                           source_change_events,
                           event.u.src_change.changes, frames);
                }
            } else if (event.type == V4L2_EVENT_EOS) {
                eos_seen = true;
                last_progress = monotonic_seconds();
                printf("v4l2_event=EOS frames=%" PRIu64 "\n", frames);
            }
        }

        for (;;) {
            struct v4l2_plane plane = {0};
            struct v4l2_buffer dequeued = {
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                .memory = V4L2_MEMORY_MMAP,
                .length = 1,
                .m.planes = &plane,
            };
            if (xioctl(VIDIOC_DQBUF, &dequeued) < 0) {
                if (errno != EAGAIN)
                    fail_errno("VIDIOC_DQBUF OUTPUT");
                break;
            }
            if (dequeued.index >= output_count)
                fail_message("OUTPUT returned an invalid index");
            outputs[dequeued.index].free = true;
            output_returned++;
            last_progress = monotonic_seconds();
            if (dequeued.flags & V4L2_BUF_FLAG_ERROR) {
                v4l2_errors++;
                fail_message("Iris marked an OUTPUT buffer erroneous");
            }
        }

        if (capture_started && !transport.paused) {
            while (!video_eof) {
                if (!have_next_packet) {
                    pipeline_error[0] = '\0';
                    packet_result = media_pipeline_pop_video(
                        pipeline, &next_packet, 0, pipeline_error,
                        sizeof(pipeline_error));
                    if (packet_result < 0) {
                        if (pipeline_error[0]) {
                            fprintf(stderr, "read video packet: %s\n",
                                    pipeline_error);
                            return 2;
                        }
                        video_eof = true;
                        break;
                    }
                    if (packet_result == 0)
                        break;
                    have_next_packet = true;
                }
                unsigned free_index = output_count;
                for (unsigned i = 0; i < output_count; i++) {
                    if (outputs[i].free) {
                        free_index = i;
                        break;
                    }
                }
                if (free_index == output_count)
                    break;
                int64_t packet_time = next_packet.dts_us != AV_NOPTS_VALUE
                                          ? next_packet.dts_us
                                          : next_packet.pts_us;
                int64_t audio_clock = media_pipeline_clock_us(pipeline);
                if (packet_time != AV_NOPTS_VALUE &&
                    audio_clock != AV_NOPTS_VALUE &&
                    packet_time > audio_clock + VIDEO_FEED_AHEAD_US)
                    break;
                if (packet_time != AV_NOPTS_VALUE &&
                    audio_clock != AV_NOPTS_VALUE) {
                    double lateness_ms =
                        (double)(audio_clock - packet_time) / 1000.0;
                    if (lateness_ms > maximum_feed_lateness_ms)
                        maximum_feed_lateness_ms = lateness_ms;
                }
                queue_output(free_index, &outputs[free_index], &next_packet);
                have_next_packet = false;
                queued_access_units++;
                last_progress = monotonic_seconds();
                if (options.frame_limit &&
                    queued_access_units >= options.frame_limit)
                    video_eof = true;
            }
            if (video_eof && !have_next_packet && !drain_sent) {
                struct v4l2_decoder_cmd command = {
                    .cmd = V4L2_DEC_CMD_STOP,
                };
                must_ioctl(VIDIOC_DECODER_CMD, &command,
                           "VIDIOC_DECODER_CMD STOP");
                drain_sent = true;
                printf("decoder_drain_sent queued_access_units=%" PRIu64
                       "\n",
                       queued_access_units);
            }
        }

        if (capture_started && !transport.paused) {
            bool capture_generation_last = false;
            bool last_capture_had_payload = false;
            unsigned last_capture_index = capture_count;
            for (;;) {
                struct v4l2_plane plane = {0};
                struct v4l2_buffer dequeued = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    .memory = V4L2_MEMORY_MMAP,
                    .length = 1,
                    .m.planes = &plane,
                };
                if (xioctl(VIDIOC_DQBUF, &dequeued) < 0) {
                    if (errno != EAGAIN)
                        fail_errno("VIDIOC_DQBUF CAPTURE");
                    break;
                }
                if (dequeued.index >= capture_count)
                    fail_message("CAPTURE returned an invalid index");
                if (dequeued.flags & V4L2_BUF_FLAG_ERROR) {
                    capture_error_drops++;
                    previous_sequence = dequeued.sequence;
                    have_sequence = true;
                    if (++consecutive_capture_errors > 90)
                        fail_message("Iris marked too many consecutive"
                                     " CAPTURE buffers erroneous");
                    if (capture_error_drops <= 3 ||
                        capture_error_drops % 30 == 0)
                        printf("capture_error_drop count=%" PRIu64
                               " index=%u sequence=%u timestamp=%ld.%06ld\n",
                               capture_error_drops, dequeued.index,
                               dequeued.sequence,
                               (long)dequeued.timestamp.tv_sec,
                               (long)dequeued.timestamp.tv_usec);
                    if (!(dequeued.flags & V4L2_BUF_FLAG_LAST)) {
                        queue_capture(dequeued.index,
                                      captures[dequeued.index].length);
                        continue;
                    }
                    plane.bytesused = 0;
                } else if (plane.bytesused) {
                    consecutive_capture_errors = 0;
                }
                if (plane.bytesused) {
                    uint64_t frame_index = frames;
                    int64_t frame_pts_us =
                        (int64_t)dequeued.timestamp.tv_sec * INT64_C(1000000) +
                        dequeued.timestamp.tv_usec;
                    if (have_sequence &&
                        dequeued.sequence > previous_sequence + 1U)
                        sequence_drops +=
                            dequeued.sequence - previous_sequence - 1U;
                    previous_sequence = dequeued.sequence;
                    have_sequence = true;
                    for (unsigned i = 0; i < output_count; ++i) {
                        if (outputs[i].pts_us == frame_pts_us &&
                            outputs[i].queued_at > 0.0) {
                            update_stats(&decode_stats,
                                         (monotonic_seconds() -
                                          outputs[i].queued_at) * 1000.0);
                            break;
                        }
                    }
                    bool seek_preroll = options.start_us > 0 &&
                                        frame_pts_us < options.start_us;
                    if (seek_preroll) {
                        seek_preroll_drops++;
                        queue_capture(dequeued.index,
                                      captures[dequeued.index].length);
                    } else {
                    if (seek_started > 0.0) {
                        printf("seek_latency_ms=%.0f\n",
                               (monotonic_seconds() - seek_started) * 1000.0);
                        seek_started = 0.0;
                        seek_requested_at = 0.0;
                    }
                    if (!audio_started && options.start_paused) {
                        /* The audio clock starts with the next "play". */
                        transport.paused = true;
                        transport.pause_started = monotonic_seconds();
                        audio_started = true;
                        printf("player_state=PAUSED start_paused=1"
                               " first_video_pts_us=%" PRId64 "\n",
                               frame_pts_us);
                    } else if (!audio_started) {
                        if (media_pipeline_pause(
                                pipeline, false, pipeline_error,
                                sizeof(pipeline_error)) < 0) {
                            fprintf(stderr, "start audio playback: %s\n",
                                    pipeline_error);
                            return 2;
                        }
                        audio_started = true;
                        printf("audio_started first_video_pts_us=%" PRId64
                               "\n", frame_pts_us);
                    }
                    if (renderer_outstanding[dequeued.index])
                        fail_message("Iris returned a renderer-owned buffer");
                    int64_t audio_clock = media_pipeline_clock_us(pipeline);
                    while (audio_clock != AV_NOPTS_VALUE &&
                           frame_pts_us > audio_clock + INT64_C(10000)) {
                        int64_t wait_us = frame_pts_us - audio_clock - 10000;
                        if (wait_us > 5000)
                            wait_us = 5000;
                        struct timespec delay = {
                            .tv_sec = 0,
                            .tv_nsec = wait_us * 1000,
                        };
                        nanosleep(&delay, NULL);
                        if (process_renderer_messages(captures, capture_count))
                            fail_message("unexpected renderer FLUSHED response");
                        audio_clock = media_pipeline_clock_us(pipeline);
                    }
                    bool late = audio_clock != AV_NOPTS_VALUE &&
                                frame_pts_us + VIDEO_LATE_DROP_US < audio_clock;
                    if (late) {
                        late_video_drops++;
                        queue_capture(dequeued.index,
                                      captures[dequeued.index].length);
                    } else {
                        detect_format_from_frame(&options, captures,
                                                 dequeued.index, &layout,
                                                 frame_pts_us);
                        send_renderer_message(NV12_IPC_FRAME, dequeued.index,
                                              frame_index);
                        renderer_outstanding[dequeued.index] = true;
                    }
                    if (frame_index == 0 || frames % 300U == 0 ||
                        (options.frame_limit &&
                         frame_index + 1U == options.frame_limit)) {
                        printf("frame=%" PRIu64 " capture_index=%u"
                               " sequence=%u bytesused=%u timestamp=%ld.%06ld"
                               " audio_clock_us=%" PRId64 " action=%s\n",
                               frame_index, dequeued.index,
                               dequeued.sequence, plane.bytesused,
                               (long)dequeued.timestamp.tv_sec,
                               (long)dequeued.timestamp.tv_usec, audio_clock,
                               late ? "drop-late" : "handoff-renderer");
                    }
                    }
                    if (seek_preroll &&
                        (seek_preroll_drops == 1 ||
                         seek_preroll_drops % 60U == 0))
                        printf("frame=%" PRIu64 " timestamp_us=%" PRId64
                               " action=drop-seek-preroll target_us=%" PRId64
                               "\n", frame_index, frame_pts_us,
                               options.start_us);
                    frames++;
                    last_progress = monotonic_seconds();
                }
                if (dequeued.flags & V4L2_BUF_FLAG_LAST) {
                    capture_generation_last = true;
                    last_capture_had_payload = plane.bytesused != 0;
                    last_capture_index = dequeued.index;
                    last_seen = !source_change_pending;
                    printf("capture_flag=LAST frames=%" PRIu64
                           " reason=%s bytesused=%u\n",
                           frames,
                           source_change_pending ? "source-change"
                                                 : "end-of-stream",
                           plane.bytesused);
                    break;
                }
                if (!plane.bytesused)
                    queue_capture(dequeued.index,
                                  captures[dequeued.index].length);
                if (transport.paused)
                    break; /* started paused: one frame is shown */
            }
            if (capture_generation_last && source_change_pending) {
                if (last_capture_index >= capture_count)
                    fail_message("SOURCE_CHANGE LAST buffer has invalid index");
                flush_renderer(captures, capture_count);
                struct capture_contract updated_contract = {0};
                struct video_layout updated_layout = {0};
                query_capture_contract(&updated_contract);
                bool memory_compatible = capture_memory_compatible(
                    &capture_contract, &updated_contract);
                bool compose_unchanged = same_compose(
                    &capture_contract.compose, &updated_contract.compose);
                printf("capture_reuse_check memory_compatible=%u"
                       " compose_unchanged=%u existing_buffers=%u\n",
                       memory_compatible, compose_unchanged, capture_count);
                if (!memory_compatible)
                    fail_message("SOURCE_CHANGE CAPTURE memory contract changed");
                if (!compose_unchanged)
                    fail_message("SOURCE_CHANGE visible compose region changed");
                validate_capture_contract(&updated_contract, &updated_layout,
                                          expected_width, expected_height);
                struct capture_minimum minimum = query_capture_minimum();
                if (minimum.available && capture_count < minimum.value)
                    fail_message("existing CAPTURE count is below new minimum");
                verify_capture_allocations(captures, capture_count,
                                           updated_contract.sizeimage);
                if (!last_capture_had_payload)
                    queue_capture(last_capture_index,
                                  captures[last_capture_index].length);
                struct v4l2_decoder_cmd command = {
                    .cmd = V4L2_DEC_CMD_START,
                };
                must_ioctl(VIDIOC_DECODER_CMD, &command,
                           "VIDIOC_DECODER_CMD START CAPTURE reuse");
                capture_contract = updated_contract;
                layout = updated_layout;
                source_change_pending = false;
                capture_reconfigurations++;
                last_progress = monotonic_seconds();
                printf("capture_reused generation=%" PRIu64
                       " frames=%" PRIu64
                       " output_stream_kept=1 capture_stream_kept=1"
                       " vulkan_imports_kept=1 last_index=%u\n",
                       capture_reconfigurations + 1U, frames,
                       last_capture_index);
            }
        }
    }

    if (transport.requested) {
        if (media_pipeline_pause(pipeline, true, pipeline_error,
                                 sizeof(pipeline_error)) < 0) {
            fprintf(stderr, "pause before player control: %s\n",
                    pipeline_error);
            return 2;
        }
        if (transport.requested == NV12_ACTION_BACK || !capture_started)
            wait_renderer(false);
        else
            hold_renderer(captures, capture_count);
        /* After a failed seek in place the queues are already off, and
         * Iris may reject STREAMOFF (EINVAL after a refused STREAMON);
         * closing the device below cleans up either way. */
        if (capture_queue_closed)
            fprintf(stderr, "seek_in_place_failed restarting=1\n");
        for (unsigned i = 0; i < output_count; ++i)
            if (munmap(outputs[i].mapping, outputs[i].length) < 0)
                fail_errno("munmap OUTPUT control");
        release_decoder();
        for (unsigned i = 0; i < capture_count; ++i)
            if (captures[i].dma_buf_fd >= 0 &&
                close(captures[i].dma_buf_fd) < 0)
                fail_errno("close CAPTURE DMA-BUF control");
        media_packet_unref(&next_packet);
        if (transport.requested != NV12_ACTION_BACK && capture_started) {
            /* Seek by reopening the decoder: the source stays open (opening
             * it again for a restart took about a second for 8K over
             * Jellyfin), jumps to the new keyframe, and Iris is set up as
             * at the start. A freshly opened Iris accepts what it refused
             * after a seek in place. The renderer keeps the held frame
             * until the new buffers arrive. */
            if (media_pipeline_seek(pipeline, transport.requested_start_us,
                                    transport.from_us, true, pipeline_error,
                                    sizeof(pipeline_error)) == 0) {
                options.start_us = media_pipeline_start_us(pipeline);
                options.start_paused = transport.paused;
                options.adopt_renderer_fd = renderer_fd;
                options.adopt_renderer_pid = renderer_pid;
                if (panel_state.valid)
                    options.panel = panel_state;
                keep_view(&options);
                memset(renderer_outstanding, 0,
                       sizeof(renderer_outstanding));
                capture_queue_closed = false;
                seek_requested_at = transport.requested_at;
                printf("decoder_reopen requested_us=%" PRId64
                       " start_us=%" PRId64 "\n",
                       transport.requested_start_us, options.start_us);
                log_seek_phase("source_seek");
                goto open_decoder;
            }
            fprintf(stderr, "seek: %s\n", pipeline_error);
        }
        volume_pipeline = NULL;
        media_pipeline_close(&pipeline);
        if (transport.requested == NV12_ACTION_BACK)
            return 0;
        options.seek_from_us = transport.from_us;
        seek_requested_at = transport.requested_at;
        restart_session(argv, &options, transport.requested_start_us,
                        transport.paused);
    }

    double playback_end = monotonic_seconds();
    double elapsed = playback_end - playback_start;
    double achieved_fps = elapsed > 0.0 ? frames / elapsed : 0.0;
    double source_fps = (double)fps_numerator / fps_denominator;
    uint64_t accounted = frames + capture_error_drops;
    uint64_t count_difference =
        accounted < queued_access_units ? queued_access_units - accounted
                                        : accounted - queued_access_units;
    uint64_t drops = sequence_drops + count_difference + late_video_drops;
    printf("summary access_units=%" PRIu64 " output_returned=%" PRIu64
           " frames=%" PRIu64 " sequence_drops=%" PRIu64
           " late_video_drops=%" PRIu64
           " seek_preroll_drops=%" PRIu64
           " count_difference=%" PRIu64 " errors=%" PRIu64
           " source_changes=%" PRIu64 " capture_reconfigurations=%" PRIu64
           " eos_event=%u last_buffer=%u\n",
           queued_access_units, output_returned, frames, sequence_drops,
           late_video_drops, seek_preroll_drops, count_difference, v4l2_errors,
           source_change_events,
           capture_reconfigurations, eos_seen, last_seen);
    printf("timing source_fps=%.6f achieved_fps=%.6f elapsed_s=%.6f"
           " source_duration_s=%.6f realtime_ratio=%.6f"
           " maximum_feed_lateness_ms=%.6f\n",
           source_fps, achieved_fps, elapsed, source_duration,
           source_fps > 0.0 ? achieved_fps / source_fps : 0.0,
           maximum_feed_lateness_ms);
    printf("decode_timing samples=%" PRIu64 " mean_ms=%.6f max_ms=%.6f\n",
           decode_stats.count,
           decode_stats.count ? decode_stats.sum_ms / decode_stats.count : 0,
           decode_stats.maximum_ms);
    wait_renderer(options.frame_limit == 0); /* test runs stop after N */

    for (unsigned i = 0; i < output_count; i++) {
        if (munmap(outputs[i].mapping, outputs[i].length) < 0)
            fail_errno("munmap OUTPUT");
    }
    release_decoder();
    for (unsigned i = 0; i < capture_count; ++i) {
        if (captures[i].dma_buf_fd >= 0 && close(captures[i].dma_buf_fd) < 0)
            fail_errno("close CAPTURE DMA-BUF");
    }
    media_packet_unref(&next_packet);
    volume_pipeline = NULL;
    media_pipeline_close(&pipeline);
    bool success = frames + capture_error_drops == queued_access_units &&
                   sequence_drops == 0 &&
                   v4l2_errors == 0 && last_seen;
    printf("result=%s scheduler_drops=%" PRIu64 "\n",
           success ? "success" : "failure", drops);
    return success ? 0 : 1;
}
