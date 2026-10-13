#include "media-demux.h"

#include <errno.h>
#include <signal.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>

#include "decoded-video.h"

/* Jellyfin's HLS segments are about 3 s long and start with a keyframe;
 * see media_demux_seek. */
#define HLS_SEGMENT_HALF_US INT64_C(1500000)
/* Packets read ahead after a seek while looking for the first video
 * packet (audio interleaved before it). */
#define PENDING_CAPACITY 256U

struct media_demux {
    AVFormatContext *format;
    AVBSFContext *video_bsf;
    /* Copied at open: the demux thread may update the stream's own. */
    AVCodecParameters *video_parameters;
    AVPacket *input;
    AVPacket *filtered;
    struct media_stream_info info;
    bool input_eof;
    bool bsf_eof_sent;
    /* HLS timelines (Jellyfin) start at 10 s; all packet times are shifted
     * so playback positions and seek targets start at zero. */
    int64_t time_offset_us;
    /* Read ahead by the last seek, returned before reading on. */
    struct media_packet pending[PENDING_CAPACITY];
    size_t pending_first, pending_count;
    /* Time of the first video packet after the last seek. */
    int64_t start_us;
};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || size == 0)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void set_av_error(char *error, size_t size, const char *operation,
                         int result)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, detail, sizeof(detail));
    set_error(error, size, "%s: %s", operation, detail);
}

static int64_t packet_time_us(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return av_rescale_q(value, time_base, AV_TIME_BASE_Q);
}

/* PCVR: GPU decoders (decoded-video.h) take 4:2:0 with 8 or 10 bits of
 * H.264 (8-bit profiles only; NVDEC and Vulkan Video lack High 10), HEVC
 * and AV1. */
static bool is_supported_on_desktop(const AVCodecParameters *parameters)
{
    bool eight = parameters->format == AV_PIX_FMT_NONE ||
                 parameters->format == AV_PIX_FMT_YUV420P;
    bool ten = parameters->format == AV_PIX_FMT_YUV420P10LE;
    if (parameters->codec_id == AV_CODEC_ID_H264) {
        int profile = parameters->profile & ~AV_PROFILE_H264_CONSTRAINED;
        return eight && parameters->bits_per_raw_sample <= 8 &&
               (profile == AV_PROFILE_H264_BASELINE ||
                profile == AV_PROFILE_H264_MAIN ||
                profile == AV_PROFILE_H264_HIGH);
    }
    if (parameters->codec_id == AV_CODEC_ID_HEVC)
        return (eight || ten) &&
               (parameters->profile == AV_PROFILE_HEVC_MAIN ||
                parameters->profile == AV_PROFILE_HEVC_MAIN_10);
    if (parameters->codec_id == AV_CODEC_ID_AV1)
        return eight || ten;
    return false;
}

/* Iris decodes HEVC Main and the 8-bit 4:2:0 H.264 profiles to NV12 and
 * HEVC Main10 to Q10C. 8-bit AV1 up to 4K is decoded in software if the
 * viewer opted in (decoded-video.h). Without Iris (a PC) the GPU decodes. */
static bool is_supported_video(const AVCodecParameters *parameters)
{
    if (access(DECODED_VIDEO_IRIS_DEVICE, F_OK) < 0)
        return is_supported_on_desktop(parameters);
    if (parameters->codec_id == AV_CODEC_ID_AV1) {
        if (!getenv(AV1_SOFTWARE_ENV) ||
            parameters->width > AV1_SOFTWARE_MAX_WIDTH ||
            parameters->height > AV1_SOFTWARE_MAX_HEIGHT)
            return false;
        return (parameters->format == AV_PIX_FMT_NONE ||
                parameters->format == AV_PIX_FMT_YUV420P) &&
               parameters->bits_per_raw_sample <= 8;
    }
    if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        /* Main10 arrives as Q10C, which only the patched Turnip can sample;
         * the launcher sets STEREONAUT_TP10 when that driver is installed. */
        if (parameters->profile == AV_PROFILE_HEVC_MAIN_10 &&
            getenv("STEREONAUT_TP10"))
            return (parameters->format == AV_PIX_FMT_NONE ||
                     parameters->format == AV_PIX_FMT_YUV420P10LE) &&
                   parameters->bits_per_raw_sample <= 10;
        if (parameters->profile != AV_PROFILE_HEVC_MAIN)
            return false;
    } else if (parameters->codec_id == AV_CODEC_ID_H264) {
        int profile = parameters->profile & ~AV_PROFILE_H264_CONSTRAINED;
        if (profile != AV_PROFILE_H264_BASELINE &&
            profile != AV_PROFILE_H264_MAIN &&
            profile != AV_PROFILE_H264_HIGH)
            return false;
    } else {
        return false;
    }
    if (parameters->format != AV_PIX_FMT_NONE &&
        parameters->format != AV_PIX_FMT_YUV420P)
        return false;
    return parameters->bits_per_raw_sample <= 8;
}

static volatile sig_atomic_t abort_requested;

void media_demux_request_abort(void)
{
    abort_requested = 1;
}

/* libavformat polls this during blocking I/O (HTTP, HLS segment waits). */
static int interrupt_requested(void *opaque)
{
    (void)opaque;
    return abort_requested != 0;
}

static int select_audio_stream(AVFormatContext *format)
{
    int best = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1,
                                   NULL, 0);
    if (best >= 0 && avcodec_find_decoder(
                         format->streams[best]->codecpar->codec_id))
        return best;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVCodecParameters *parameters = format->streams[i]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(parameters->codec_id))
            return (int)i;
    }
    return -1;
}

static int initialize_video_filter(struct media_demux *demux,
                                   char *error, size_t error_size)
{
    /* dav1d takes AV1 packets as stored in the container. */
    const char *name = demux->info.video_codec == AV_CODEC_ID_H264
                           ? "h264_mp4toannexb"
                       : demux->info.video_codec == AV_CODEC_ID_AV1
                           ? "null"
                           : "hevc_mp4toannexb";
    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter) {
        set_error(error, error_size, "FFmpeg provides no %s filter", name);
        return -1;
    }
    int result = av_bsf_alloc(filter, &demux->video_bsf);
    if (result < 0) {
        set_av_error(error, error_size, "av_bsf_alloc", result);
        return -1;
    }
    AVStream *stream =
        demux->format->streams[demux->info.video_stream_index];
    result = avcodec_parameters_copy(demux->video_bsf->par_in,
                                     stream->codecpar);
    if (result < 0) {
        set_av_error(error, error_size, "avcodec_parameters_copy", result);
        return -1;
    }
    demux->video_bsf->time_base_in = stream->time_base;
    result = av_bsf_init(demux->video_bsf);
    if (result < 0) {
        set_av_error(error, error_size, "av_bsf_init", result);
        return -1;
    }
    return 0;
}

int media_demux_open(struct media_demux **result, const char *path,
                     char *error, size_t error_size)
{
    if (!result || !path) {
        set_error(error, error_size, "invalid media_demux_open arguments");
        return -1;
    }
    *result = NULL;
    struct media_demux *demux = calloc(1, sizeof(*demux));
    if (!demux) {
        set_error(error, error_size, "allocate demux context: %s",
                  strerror(errno));
        return -1;
    }
    demux->info.video_stream_index = -1;
    demux->info.audio_stream_index = -1;
    demux->start_us = AV_NOPTS_VALUE;
    demux->format = avformat_alloc_context();
    if (!demux->format) {
        set_error(error, error_size, "allocate format context");
        free(demux);
        return -1;
    }
    demux->format->interrupt_callback =
        (AVIOInterruptCB){.callback = interrupt_requested};
    AVDictionary *options = NULL;
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        /* Credentials come from the environment, never from argv, so they
         * stay out of process listings and logs. */
        const char *headers = getenv("STEREONAUT_HTTP_HEADERS");
        if (headers && headers[0]) {
            /* libavformat requires CRLF-terminated header lines. */
            size_t length = strlen(headers);
            char *terminated = malloc(length + 3);
            if (terminated) {
                memcpy(terminated, headers, length);
                while (length && (terminated[length - 1] == '\n' ||
                                  terminated[length - 1] == '\r'))
                    --length;
                memcpy(terminated + length, "\r\n", 3);
                av_dict_set(&options, "headers", terminated, 0);
                free(terminated);
            }
        }
        av_dict_set(&options, "user_agent", "Stereonaut", 0);
        av_dict_set(&options, "rw_timeout", "20000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "2", 0);
        /* Reconnect mid-stream, not only on timeout, and keep the TCP
         * connection alive so byte-range seeks reuse it instead of
         * reopening. A larger avio buffer absorbs WiFi jitter on the
         * high-bitrate 8K60 10-bit path. */
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "multiple_requests", "1", 0);
        av_dict_set(&options, "buffer_size", "4194304", 0);
    }
    int av_result = avformat_open_input(&demux->format, path, NULL, &options);
    av_dict_free(&options);
    if (av_result < 0) {
        set_av_error(error, error_size, "avformat_open_input", av_result);
        media_demux_close(&demux);
        return -1;
    }
    av_result = avformat_find_stream_info(demux->format, NULL);
    if (av_result < 0) {
        set_av_error(error, error_size, "avformat_find_stream_info",
                     av_result);
        media_demux_close(&demux);
        return -1;
    }
    int video = av_find_best_stream(demux->format, AVMEDIA_TYPE_VIDEO,
                                    -1, -1, NULL, 0);
    if (video < 0) {
        set_error(error, error_size, "container has no video stream");
        media_demux_close(&demux);
        return -1;
    }
    AVCodecParameters *video_parameters =
        demux->format->streams[video]->codecpar;
    if (!is_supported_video(video_parameters)) {
        const char *pixel = av_get_pix_fmt_name(video_parameters->format);
        set_error(error, error_size,
                  "unsupported video: codec=%s profile=%d pixel_format=%s",
                  avcodec_get_name(video_parameters->codec_id),
                  video_parameters->profile, pixel ? pixel : "unknown");
        media_demux_close(&demux);
        return -1;
    }
    demux->info.video_stream_index = video;
    demux->info.video_codec = video_parameters->codec_id;
    demux->info.video_profile = video_parameters->profile;
    demux->info.pixel_format = video_parameters->format;
    demux->info.width = video_parameters->width;
    demux->info.height = video_parameters->height;
    AVStream *video_stream = demux->format->streams[video];
    demux->info.frame_rate = av_guess_frame_rate(demux->format,
                                                 video_stream, NULL);

    int audio = select_audio_stream(demux->format);
    if (audio >= 0) {
        AVCodecParameters *audio_parameters =
            demux->format->streams[audio]->codecpar;
        demux->info.audio_stream_index = audio;
        demux->info.audio_codec = audio_parameters->codec_id;
        demux->info.sample_rate = audio_parameters->sample_rate;
        demux->info.audio_time_base = demux->format->streams[audio]->time_base;
        if (av_channel_layout_copy(&demux->info.channel_layout,
                                   &audio_parameters->ch_layout) < 0) {
            set_error(error, error_size, "copy audio channel layout failed");
            media_demux_close(&demux);
            return -1;
        }
    }
    demux->info.start_us = demux->format->start_time == AV_NOPTS_VALUE
                                ? 0 : demux->format->start_time;
    if (demux->format->iformat && demux->format->iformat->name &&
        strstr(demux->format->iformat->name, "hls"))
        demux->time_offset_us = demux->info.start_us;
    demux->info.duration_us = demux->format->duration;
    if (initialize_video_filter(demux, error, error_size) < 0) {
        media_demux_close(&demux);
        return -1;
    }
    /* The packets leave the filter as Annex B; a decoder (dav1d, the PC's
     * hardware decoders) needs the matching extradata from its output. */
    demux->video_parameters = avcodec_parameters_alloc();
    if (!demux->video_parameters ||
        avcodec_parameters_copy(demux->video_parameters,
                                demux->video_bsf->par_out) < 0) {
        set_error(error, error_size, "copy video parameters failed");
        media_demux_close(&demux);
        return -1;
    }
    demux->input = av_packet_alloc();
    demux->filtered = av_packet_alloc();
    if (!demux->input || !demux->filtered) {
        set_error(error, error_size, "allocate FFmpeg packets failed");
        media_demux_close(&demux);
        return -1;
    }
    *result = demux;
    return 0;
}

const struct media_stream_info *media_demux_info(const struct media_demux *demux)
{
    return demux ? &demux->info : NULL;
}

int media_demux_copy_audio_parameters(const struct media_demux *demux,
                                      AVCodecParameters *parameters)
{
    if (!demux || !parameters || demux->info.audio_stream_index < 0)
        return AVERROR(EINVAL);
    return avcodec_parameters_copy(
        parameters,
        demux->format->streams[demux->info.audio_stream_index]->codecpar);
}

int media_demux_copy_video_parameters(const struct media_demux *demux,
                                      AVCodecParameters *parameters)
{
    if (!demux || !parameters || !demux->video_parameters)
        return AVERROR(EINVAL);
    return avcodec_parameters_copy(parameters, demux->video_parameters);
}

static int return_packet(struct media_packet *output, AVPacket *packet,
                         enum media_packet_kind kind, AVRational time_base,
                         int64_t time_offset_us,
                         char *error, size_t error_size)
{
    if (time_offset_us) {
        int64_t ticks = av_rescale_q(time_offset_us, AV_TIME_BASE_Q,
                                     time_base);
        if (packet->pts != AV_NOPTS_VALUE)
            packet->pts -= ticks;
        if (packet->dts != AV_NOPTS_VALUE)
            packet->dts -= ticks;
    }
    output->packet = av_packet_alloc();
    if (!output->packet) {
        set_error(error, error_size, "allocate output packet failed");
        return -1;
    }
    av_packet_move_ref(output->packet, packet);
    output->kind = kind;
    output->pts_us = packet_time_us(output->packet->pts, time_base);
    output->dts_us = packet_time_us(output->packet->dts, time_base);
    output->duration_us = output->packet->duration > 0
                              ? av_rescale_q(output->packet->duration,
                                             time_base, AV_TIME_BASE_Q)
                              : 0;
    output->keyframe = (output->packet->flags & AV_PKT_FLAG_KEY) != 0;
    return 1;
}

static int read_packet(struct media_demux *demux, struct media_packet *packet,
                       char *error, size_t error_size)
{
    memset(packet, 0, sizeof(*packet));
    for (;;) {
        int result = av_bsf_receive_packet(demux->video_bsf,
                                           demux->filtered);
        if (result >= 0)
            return return_packet(packet, demux->filtered,
                                 MEDIA_PACKET_VIDEO,
                                 demux->video_bsf->time_base_out,
                                 demux->time_offset_us, error, error_size);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            set_av_error(error, error_size, "av_bsf_receive_packet", result);
            return -1;
        }
        if (demux->input_eof) {
            if (!demux->bsf_eof_sent) {
                result = av_bsf_send_packet(demux->video_bsf, NULL);
                if (result < 0 && result != AVERROR_EOF) {
                    set_av_error(error, error_size,
                                 "av_bsf_send_packet(eof)", result);
                    return -1;
                }
                demux->bsf_eof_sent = true;
                continue;
            }
            return 0;
        }

        av_packet_unref(demux->input);
        result = av_read_frame(demux->format, demux->input);
        if (result == AVERROR_EOF) {
            demux->input_eof = true;
            continue;
        }
        if (result < 0) {
            set_av_error(error, error_size, "av_read_frame", result);
            return -1;
        }
        if (demux->input->stream_index == demux->info.video_stream_index) {
            result = av_bsf_send_packet(demux->video_bsf, demux->input);
            if (result < 0) {
                set_av_error(error, error_size, "av_bsf_send_packet",
                             result);
                return -1;
            }
            continue;
        }
        if (demux->input->stream_index == demux->info.audio_stream_index) {
            AVStream *audio_stream = demux->format->streams[
                demux->info.audio_stream_index];
            return return_packet(packet, demux->input,
                                 MEDIA_PACKET_AUDIO,
                                 audio_stream->time_base,
                                 demux->time_offset_us, error, error_size);
        }
    }
}

int media_demux_read(struct media_demux *demux, struct media_packet *packet,
                     char *error, size_t error_size)
{
    if (!demux || !packet) {
        set_error(error, error_size, "invalid media_demux_read arguments");
        return -1;
    }
    if (demux->pending_count) {
        *packet = demux->pending[demux->pending_first];
        memset(&demux->pending[demux->pending_first], 0, sizeof(*packet));
        demux->pending_first = (demux->pending_first + 1U) % PENDING_CAPACITY;
        demux->pending_count--;
        return 1;
    }
    return read_packet(demux, packet, error, error_size);
}

static void drop_pending(struct media_demux *demux)
{
    while (demux->pending_count) {
        media_packet_unref(&demux->pending[demux->pending_first]);
        demux->pending_first = (demux->pending_first + 1U) % PENDING_CAPACITY;
        demux->pending_count--;
    }
    demux->pending_first = 0;
}

/* Reads up to the first video packet, which starts the playback after a
 * seek, and keeps everything read for media_demux_read. Audio before it
 * beyond the read-ahead capacity is dropped (it precedes the start). */
static int find_start(struct media_demux *demux, char *error,
                      size_t error_size)
{
    for (;;) {
        struct media_packet packet;
        int result = read_packet(demux, &packet, error, error_size);
        if (result <= 0) {
            demux->start_us = AV_NOPTS_VALUE;
            return result;
        }
        if (demux->pending_count == PENDING_CAPACITY) {
            media_packet_unref(&demux->pending[demux->pending_first]);
            demux->pending_first = (demux->pending_first + 1U) %
                                   PENDING_CAPACITY;
            demux->pending_count--;
        }
        bool video = packet.kind == MEDIA_PACKET_VIDEO;
        int64_t time = packet.pts_us != AV_NOPTS_VALUE ? packet.pts_us
                                                       : packet.dts_us;
        demux->pending[(demux->pending_first + demux->pending_count) %
                       PENDING_CAPACITY] = packet;
        demux->pending_count++;
        if (video) {
            demux->start_us = time;
            return 1;
        }
    }
}

/* The keyframe nearest to `target` (stream time base) from the container
 * index, or `target` if the index is empty. */
static int64_t nearest_keyframe(AVStream *stream, int64_t target)
{
    const AVIndexEntry *before = avformat_index_get_entry_from_timestamp(
        stream, target, AVSEEK_FLAG_BACKWARD);
    const AVIndexEntry *after =
        avformat_index_get_entry_from_timestamp(stream, target, 0);
    if (!before && !after)
        return target;
    if (!before)
        return after->timestamp;
    if (!after)
        return before->timestamp;
    return target - before->timestamp <= after->timestamp - target
               ? before->timestamp
               : after->timestamp;
}

/* The keyframe to resume at for a seek from `from` to `target` (stream
 * time base; `from` negative when there is no current position): the
 * nearest to the target, unless that covers less than half the way in the
 * seek's direction. Then the first keyframe beyond the halfway point, so a
 * short skip over a long GOP (8K titles with one keyframe in 30 s) does
 * not land where it started or move the wrong way. */
static int64_t choose_keyframe(AVStream *stream, int64_t target,
                               int64_t from)
{
    int64_t chosen = nearest_keyframe(stream, target);
    if (from < 0 || target == from)
        return chosen;
    int64_t half = from + (target - from) / 2;
    const AVIndexEntry *beyond = NULL;
    if (target > from && chosen <= half)
        beyond = avformat_index_get_entry_from_timestamp(stream, half + 1, 0);
    else if (target < from && chosen >= half)
        beyond = avformat_index_get_entry_from_timestamp(
            stream, half - 1, AVSEEK_FLAG_BACKWARD);
    return beyond ? beyond->timestamp : chosen;
}

/* To the first keyframe at or after `minimum` (stream time base), the one
 * nearest `target` if the index knows several. Without an index entry
 * there libavformat reads forward to find it. */
static int seek_forward(struct media_demux *demux, int64_t minimum,
                        int64_t target, char *error, size_t error_size)
{
    int result = avformat_seek_file(demux->format,
                                    demux->info.video_stream_index,
                                    minimum, target, INT64_MAX, 0);
    if (result < 0) {
        set_av_error(error, error_size, "avformat_seek_file", result);
        return -1;
    }
    return 0;
}

/* To the index keyframe at `keyframe` (stream time base). Searched
 * forwards: MP4 index entries carry decoding times, and a backward search
 * for one of them lands a keyframe early (libavformat shifts the time by
 * the B-frame delay first). */
static int seek_keyframe(struct media_demux *demux, int64_t keyframe,
                         char *error, size_t error_size)
{
    int result = av_seek_frame(demux->format, demux->info.video_stream_index,
                               keyframe, 0);
    if (result < 0) {
        set_av_error(error, error_size, "av_seek_frame", result);
        return -1;
    }
    return 0;
}

/* To the last keyframe at or before `target` (stream time base). */
static int seek_backward(struct media_demux *demux, int64_t target,
                         char *error, size_t error_size)
{
    int result = avformat_seek_file(demux->format,
                                    demux->info.video_stream_index,
                                    INT64_MIN, target, target,
                                    AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        set_av_error(error, error_size, "avformat_seek_file", result);
        return -1;
    }
    return 0;
}

int64_t media_demux_start_us(const struct media_demux *demux)
{
    return demux ? demux->start_us : AV_NOPTS_VALUE;
}

/* Playback resumes at the keyframe nearest to the target, before or after
 * it, so no frames have to be decoded and discarded up to the target (see
 * choose_keyframe for from_us). */
int media_demux_seek(struct media_demux *demux, int64_t target_us,
                     int64_t from_us, char *error, size_t error_size)
{
    if (!demux || target_us < 0) {
        set_error(error, error_size, "invalid seek target");
        return -1;
    }
    AVStream *stream =
        demux->format->streams[demux->info.video_stream_index];
    if (demux->time_offset_us) {
        /* libavformat's HLS demuxer starts at the first keyframe at or after
         * the requested time. Segments begin with a keyframe, so asking half
         * a segment early yields the segment start nearest to the target. */
        target_us -= HLS_SEGMENT_HALF_US;
        if (target_us < 0)
            target_us = 0;
    }
    int64_t target = av_rescale_q(target_us + demux->time_offset_us,
                                  AV_TIME_BASE_Q, stream->time_base);
    int64_t from = from_us < 0 ? -1
                               : av_rescale_q(from_us + demux->time_offset_us,
                                              AV_TIME_BASE_Q,
                                              stream->time_base);
    int64_t chosen = target;
    /* If the index already reaches past the target (MP4; Matroska after
     * its first seek, which reads the cues), one seek goes straight to the
     * chosen keyframe. Otherwise seek back first, which completes the
     * index, and on to the chosen keyframe if that is another one. */
    bool indexed = !demux->time_offset_us &&
                   avformat_index_get_entry_from_timestamp(stream, target, 0);
    if (indexed) {
        chosen = choose_keyframe(stream, target, from);
        if (seek_keyframe(demux, chosen, error, error_size) < 0)
            return -1;
    } else if (seek_backward(demux, chosen, error, error_size) < 0) {
        return -1;
    }
    if (!indexed && !demux->time_offset_us) {
        const AVIndexEntry *reached = avformat_index_get_entry_from_timestamp(
            stream, target, AVSEEK_FLAG_BACKWARD);
        int64_t keyframe = choose_keyframe(stream, target, from);
        int64_t half = from + (target - from) / 2;
        if (from >= 0 && target > from && keyframe <= half) {
            /* The index ends before the halfway point (Matroska without
             * cues lists only keyframes read so far). */
            if (seek_forward(demux, half + 1, target, error,
                             error_size) < 0)
                return -1;
        } else if ((!reached || keyframe != reached->timestamp) &&
                   seek_keyframe(demux, keyframe, error, error_size) < 0) {
            return -1;
        }
    }
    avformat_flush(demux->format);
    av_bsf_flush(demux->video_bsf);
    av_packet_unref(demux->input);
    av_packet_unref(demux->filtered);
    demux->input_eof = false;
    demux->bsf_eof_sent = false;
    drop_pending(demux);
    return find_start(demux, error, error_size) < 0 ? -1 : 0;
}

void media_packet_unref(struct media_packet *packet)
{
    if (!packet)
        return;
    av_packet_free(&packet->packet);
    memset(packet, 0, sizeof(*packet));
}

void media_demux_close(struct media_demux **pointer)
{
    if (!pointer || !*pointer)
        return;
    struct media_demux *demux = *pointer;
    drop_pending(demux);
    av_packet_free(&demux->input);
    av_packet_free(&demux->filtered);
    av_bsf_free(&demux->video_bsf);
    avcodec_parameters_free(&demux->video_parameters);
    avformat_close_input(&demux->format);
    av_channel_layout_uninit(&demux->info.channel_layout);
    free(demux);
    *pointer = NULL;
}
