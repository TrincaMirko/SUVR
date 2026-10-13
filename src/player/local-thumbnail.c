#define _POSIX_C_SOURCE 200809L
#include "local-thumbnail.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <jpeglib.h> /* needs stdio.h first */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>

/* ---- sidecar pictures --------------------------------------------------- */

static char *read_whole(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    char *data = NULL;
    struct stat status;
    if (fstat(fileno(file), &status) == 0 && status.st_size > 0 &&
        status.st_size < 32L * 1024 * 1024 &&
        (data = malloc((size_t)status.st_size)) &&
        fread(data, 1, (size_t)status.st_size, file) !=
            (size_t)status.st_size) {
        free(data);
        data = NULL;
    }
    if (data)
        *size = (size_t)status.st_size;
    fclose(file);
    return data;
}

static char *read_sidecar(const char *path, size_t *size)
{
    static const char *const suffixes[] = {
        "-poster.jpg", ".jpg", "-thumb.jpg", "-fanart.jpg",
    };
    char base[4096];
    snprintf(base, sizeof(base), "%s", path);
    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        char candidate[4200];
        snprintf(candidate, sizeof(candidate), "%s%s", base, suffixes[i]);
        char *data = read_whole(candidate, size);
        if (data)
            return data;
    }
    return NULL;
}

/* ---- key frame -------------------------------------------------------- */

/* Sample of plane `p` at (x, y) in 8 bits. */
static int sample(const AVFrame *frame, int p, int x, int y, int bytes,
                  int shift)
{
    const uint8_t *row = frame->data[p] + (size_t)y * (size_t)frame->linesize[p];
    if (bytes == 1)
        return row[x];
    int value = row[2 * x] | row[2 * x + 1] << 8;
    return value >> shift;
}

static uint8_t clamp8(float value)
{
    return (uint8_t)(value < 0.0f ? 0 : value > 255.0f ? 255 : value + 0.5f);
}

/* RGB (3 bytes per pixel) scaled to `width` x `height` by point sampling;
 * BT.709, limited range. */
static uint8_t *frame_to_rgb(const AVFrame *frame, int width, int height)
{
    enum AVPixelFormat format = (enum AVPixelFormat)frame->format;
    bool semi = format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010LE;
    int bytes = format == AV_PIX_FMT_YUV420P10LE ||
                        format == AV_PIX_FMT_P010LE
                    ? 2
                    : 1;
    int shift = format == AV_PIX_FMT_YUV420P10LE ? 2
                : format == AV_PIX_FMT_P010LE    ? 8
                                                 : 0;
    if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P &&
        format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_YUV420P10LE &&
        format != AV_PIX_FMT_P010LE) {
        fprintf(stderr, "local_thumbnail unsupported pixel format %s\n",
                av_get_pix_fmt_name(format));
        return NULL;
    }
    uint8_t *rgb = malloc((size_t)width * (size_t)height * 3U);
    if (!rgb)
        return NULL;
    for (int y = 0; y < height; ++y) {
        int sy = (int)(((int64_t)y * 2 + 1) * frame->height / (2 * height));
        for (int x = 0; x < width; ++x) {
            int sx = (int)(((int64_t)x * 2 + 1) * frame->width / (2 * width));
            float luma = (sample(frame, 0, sx, sy, bytes, shift) - 16) *
                         (255.0f / 219.0f);
            int cb, cr;
            if (semi) {
                cb = sample(frame, 1, (sx / 2) * 2, sy / 2, bytes, shift);
                cr = sample(frame, 1, (sx / 2) * 2 + 1, sy / 2, bytes, shift);
            } else {
                cb = sample(frame, 1, sx / 2, sy / 2, bytes, shift);
                cr = sample(frame, 2, sx / 2, sy / 2, bytes, shift);
            }
            float u = (cb - 128) * (255.0f / 224.0f);
            float v = (cr - 128) * (255.0f / 224.0f);
            uint8_t *out = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3U;
            out[0] = clamp8(luma + 1.5748f * v);
            out[1] = clamp8(luma - 0.18733f * u - 0.46813f * v);
            out[2] = clamp8(luma + 1.8556f * u);
        }
    }
    return rgb;
}

struct jpeg_failure {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr info)
{
    longjmp(((struct jpeg_failure *)info->err)->jump, 1);
}

static int encode_jpeg(const uint8_t *rgb, int width, int height, char **data,
                       size_t *size)
{
    struct jpeg_compress_struct info;
    struct jpeg_failure failure;
    unsigned char *target = NULL;
    unsigned long length = 0;
    info.err = jpeg_std_error(&failure.manager);
    failure.manager.error_exit = jpeg_fail;
    if (setjmp(failure.jump)) {
        /* (A buffer libjpeg already allocated is lost; it does not fail
         * for in-memory output in practice.) */
        jpeg_destroy_compress(&info);
        return -1;
    }
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &target, &length);
    info.image_width = (JDIMENSION)width;
    info.image_height = (JDIMENSION)height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 85, TRUE);
    jpeg_start_compress(&info, TRUE);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)info.next_scanline *
                                            (size_t)width * 3U);
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    *data = (char *)target;
    *size = length;
    return 0;
}

static int key_frame_jpeg(const char *path, const char *headers,
                          int max_width, char **data, size_t *size)
{
    AVFormatContext *format = NULL;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVDictionary *options = NULL;
    int result = -1;
    if (headers && headers[0])
        av_dict_set(&options, "headers", headers, 0);
    if (!packet || !frame ||
        avformat_open_input(&format, path, NULL, &options) < 0 ||
        avformat_find_stream_info(format, NULL) < 0)
        goto done;
    const AVCodec *codec = NULL;
    int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                     &codec, 0);
    if (stream < 0 || !codec || !(decoder = avcodec_alloc_context3(codec)) ||
        avcodec_parameters_to_context(decoder,
                                      format->streams[stream]->codecpar) < 0)
        goto done;
    decoder->thread_count = 2;
    decoder->skip_frame = AVDISCARD_NONKEY; /* intra pictures only */
    if (avcodec_open2(decoder, codec, NULL) < 0)
        goto done;
    if (format->duration > 0)
        av_seek_frame(format, -1, format->duration / 10,
                      AVSEEK_FLAG_BACKWARD);
    bool decoded = false;
    for (int packets = 0; !decoded && packets < 600 &&
                          av_read_frame(format, packet) >= 0;
         ++packets) {
        if (packet->stream_index == stream &&
            avcodec_send_packet(decoder, packet) >= 0)
            decoded = avcodec_receive_frame(decoder, frame) == 0;
        av_packet_unref(packet);
    }
    if (!decoded) {
        avcodec_send_packet(decoder, NULL); /* drain */
        decoded = avcodec_receive_frame(decoder, frame) == 0;
    }
    if (!decoded || frame->width <= 0 || frame->height <= 0)
        goto done;
    int width = frame->width < max_width ? frame->width : max_width;
    int height = (int)((int64_t)frame->height * width / frame->width);
    if (height < 1)
        height = 1;
    uint8_t *rgb = frame_to_rgb(frame, width, height);
    if (rgb) {
        result = encode_jpeg(rgb, width, height, data, size);
        free(rgb);
    }
done:
    av_dict_free(&options);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return result;
}

int local_thumbnail_jpeg(const char *path, int max_width, char **data,
                         size_t *size)
{
    *data = NULL;
    *size = 0;
    *data = read_sidecar(path, size);
    if (*data)
        return 0;
    return key_frame_jpeg(path, NULL, max_width, data, size);
}

int local_thumbnail_url_jpeg(const char *url, const char *headers,
                             int max_width, char **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    return key_frame_jpeg(url, headers, max_width, data, size);
}
