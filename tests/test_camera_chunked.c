#include "camera_chunked.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t payload[] = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 0xff, 0xd9,
    };
    const char encoded[] =
        "5;encoder=ustreamer\r\n"
        "\xff\xd8\xff\xe0\x00"
        "\r\nA\r\n"
        "\x10JFIF\x00" "abcd"
        "\r\n5\r\n"
        "efg\xff\xd9"
        "\r\n0\r\nX-UStreamer-Test: yes\r\n\r\n";
    const size_t encoded_len = sizeof(encoded) - 1;
    for (size_t chunk = 1; chunk <= encoded_len; chunk++) {
        camera_chunked_parser_t parser;
        camera_chunked_init(&parser);
        uint8_t output[64];
        size_t used = 0;
        bool done = false;
        for (size_t at = 0; at < encoded_len; at += chunk) {
            size_t len = encoded_len - at < chunk ? encoded_len - at : chunk;
            uint8_t decoded[64];
            size_t decoded_len = 0;
            bool ok = camera_chunked_feed(&parser, (const uint8_t *)encoded + at, len,
                                          decoded, len, &decoded_len, &done);
            if (!ok) fprintf(stderr, "chunk=%zu at=%zu state=%d remaining=%zu line=%zu out=%zu\n",
                             chunk, at, parser.state, parser.remaining, parser.line_len, decoded_len);
            assert(ok);
            assert(used + decoded_len <= sizeof(output));
            memcpy(output + used, decoded, decoded_len);
            used += decoded_len;
        }
        assert(done);
        assert(used == sizeof(payload));
        assert(memcmp(output, payload, sizeof(payload)) == 0);
    }

    camera_chunked_parser_t parser;
    camera_chunked_init(&parser);
    uint8_t out[8];
    size_t out_len;
    bool done;
    assert(!camera_chunked_feed(&parser, (const uint8_t *)"Z\r\n", 3,
                                out, sizeof(out), &out_len, &done));
    camera_chunked_init(&parser);
    assert(!camera_chunked_feed(&parser, (const uint8_t *)"4\r\nabcd", 7,
                                out, 2, &out_len, &done));
    puts("PASS: HTTP chunked decoding at arbitrary byte boundaries");
    return 0;
}
