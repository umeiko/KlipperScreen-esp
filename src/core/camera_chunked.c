#include "camera_chunked.h"

#include <string.h>

void camera_chunked_init(camera_chunked_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

static bool parse_size(camera_chunked_parser_t *parser)
{
    size_t value = 0, digits = 0;
    for (size_t i = 0; i < parser->line_len; i++) {
        unsigned char c = (unsigned char)parser->line[i];
        if (c == ';' || c == ' ' || c == '\t') break;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (value > (SIZE_MAX >> 4)) return false;
        value = (value << 4) | digit;
        digits++;
    }
    if (!digits) return false;
    parser->remaining = value;
    parser->state = value ? CAMERA_CHUNK_DATA : CAMERA_CHUNK_TRAILER;
    parser->line_len = 0;
    return true;
}

bool camera_chunked_feed(camera_chunked_parser_t *parser,
                         const uint8_t *data, size_t len,
                         uint8_t *out, size_t out_cap, size_t *out_len,
                         bool *done)
{
    if (!parser || (!len && !data) || !out_len || !done) return false;
    *out_len = 0;
    *done = parser->state == CAMERA_CHUNK_DONE;
    if (parser->state == CAMERA_CHUNK_ERROR) return false;

    size_t at = 0;
    while (at < len && parser->state != CAMERA_CHUNK_DONE) {
        if (parser->state == CAMERA_CHUNK_DATA) {
            size_t left = parser->remaining;
            if (left > len - at) left = len - at;
            if (left > out_cap - *out_len) {
                parser->state = CAMERA_CHUNK_ERROR;
                return false;
            }
            memcpy(out + *out_len, data + at, left);
            *out_len += left;
            at += left;
            parser->remaining -= left;
            if (!parser->remaining) parser->state = CAMERA_CHUNK_DATA_CR;
            continue;
        }

        uint8_t c = data[at++];
        if (parser->state == CAMERA_CHUNK_DATA_CR) {
            if (c != '\r') goto error;
            parser->state = CAMERA_CHUNK_DATA_LF;
            continue;
        }
        if (parser->state == CAMERA_CHUNK_DATA_LF) {
            if (c != '\n') goto error;
            parser->state = CAMERA_CHUNK_SIZE;
            parser->line_len = 0;
            continue;
        }
        if (c == '\n') {
            if (parser->line_len && parser->line[parser->line_len - 1] == '\r')
                parser->line_len--;
            if (parser->state == CAMERA_CHUNK_SIZE) {
                if (!parse_size(parser)) goto error;
            } else if (parser->state == CAMERA_CHUNK_TRAILER) {
                if (!parser->line_len) {
                    parser->state = CAMERA_CHUNK_DONE;
                    *done = true;
                    return true;
                }
                parser->line_len = 0;
            } else {
                goto error;
            }
            continue;
        }
        if (parser->line_len >= sizeof(parser->line)) goto error;
        parser->line[parser->line_len++] = (char)c;
    }
    *done = parser->state == CAMERA_CHUNK_DONE;
    return true;
error:
    parser->state = CAMERA_CHUNK_ERROR;
    return false;
}
