#include "camera_mjpeg.h"

#include <stdlib.h>
#include <string.h>

bool camera_mjpeg_init(camera_mjpeg_parser_t *parser, size_t cap)
{
    if (!parser || cap < 1024) return false;
    memset(parser, 0, sizeof(*parser));
    parser->data = malloc(cap);
    if (!parser->data) return false;
    parser->cap = cap;
    return true;
}

void camera_mjpeg_free(camera_mjpeg_parser_t *parser)
{
    if (!parser) return;
    free(parser->data);
    memset(parser, 0, sizeof(*parser));
}

static size_t find_soi(const uint8_t *data, size_t len)
{
    if (len < 3) return SIZE_MAX;
    for (size_t i = 0; i + 3 <= len; i++)
        if (data[i] == 0xff && data[i + 1] == 0xd8 && data[i + 2] == 0xff) return i;
    return SIZE_MAX;
}

static void keep_prefix_fragment(camera_mjpeg_parser_t *parser)
{
    size_t keep = 0;
    if (parser->len >= 2 && parser->data[parser->len - 2] == 0xff &&
        parser->data[parser->len - 1] == 0xd8) {
        parser->data[0] = 0xff;
        parser->data[1] = 0xd8;
        keep = 2;
    } else if (parser->len && parser->data[parser->len - 1] == 0xff) {
        parser->data[0] = 0xff;
        keep = 1;
    }
    parser->len = keep;
}

bool camera_mjpeg_feed(camera_mjpeg_parser_t *parser, const uint8_t *data, size_t len,
                       uint8_t **jpeg, size_t *jpeg_len)
{
    if (!parser || !parser->data || !jpeg || !jpeg_len || (len && !data)) return false;
    *jpeg = NULL;
    *jpeg_len = 0;

    if (len > parser->cap) {
        data += len - parser->cap;
        len = parser->cap;
        parser->len = 0;
    }
    if (len > parser->cap - parser->len) parser->len = 0;
    if (len) memcpy(parser->data + parser->len, data, len);
    parser->len += len;

    size_t soi = find_soi(parser->data, parser->len);
    if (soi == SIZE_MAX) {
        keep_prefix_fragment(parser);
        return false;
    }
    if (soi) {
        memmove(parser->data, parser->data + soi, parser->len - soi);
        parser->len -= soi;
    }
    for (size_t i = 3; i + 1 < parser->len; i++) {
        if (parser->data[i] != 0xff || parser->data[i + 1] != 0xd9) continue;
        size_t frame_len = i + 2;
        uint8_t *frame = malloc(frame_len);
        if (!frame) return false;
        memcpy(frame, parser->data, frame_len);
        memmove(parser->data, parser->data + frame_len, parser->len - frame_len);
        parser->len -= frame_len;
        *jpeg = frame;
        *jpeg_len = frame_len;
        return true;
    }
    if (parser->len == parser->cap) keep_prefix_fragment(parser);
    return false;
}
