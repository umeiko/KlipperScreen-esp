#include "camera.h"
#include "camera_mjpeg.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_list_and_urls(void)
{
    const char *json =
        "{\"result\":{\"webcams\":["
        "{\"name\":\"Main\\nCam\",\"enabled\":true,\"stream_url\":\"/webcam/webrtc\","
        "\"snapshot_url\":\"/webcam/?action=snapshot\",\"flip_horizontal\":true,"
        "\"flip_vertical\":false,\"rotation\":90},"
        "{\"name\":\"Disabled\",\"enabled\":false,\"stream_url\":\"/off\"},"
        "{\"enabled\":true,\"stream_url\":\"http://camera.local:8080/?action=stream\"}"
        "]}}";
    camera_list_t list;
    assert(camera_parse_list_json(json, &list));
    assert(list.count == 2);
    assert(strcmp(list.items[0].name, "Main Cam") == 0);
    assert(list.items[0].flip_horizontal && !list.items[0].flip_vertical);
    assert(list.items[0].rotation == 90);
    assert(strcmp(list.items[1].name, "Camera 2") == 0);
    assert(!camera_parse_list_json("{}", &list));
    assert(!camera_parse_list_json("not-json", &list));

    char url[512], host[128], path[384];
    uint16_t port;
    assert(camera_build_url("192.168.10.15", 7125, "/webcam/?action=stream", url, sizeof(url)));
    assert(strcmp(url, "http://192.168.10.15/webcam/?action=stream") == 0);
    assert(camera_parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "192.168.10.15") == 0 && port == 80);
    assert(strcmp(path, "/webcam/?action=stream") == 0);
    assert(camera_build_url("printer", 7125, "/webcam/webrtc", url, sizeof(url)));
    assert(strcmp(url, "http://printer/webcam/stream") == 0);
    assert(camera_build_url("printer", 7125, "webcam/?action=stream", url, sizeof(url)));
    assert(strcmp(url, "http://printer/webcam/?action=stream") == 0);
    assert(camera_build_url("printer", 7125, "http://127.0.0.1:8080/", url, sizeof(url)));
    assert(camera_parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "127.0.0.1") == 0 && port == 8080 && strcmp(path, "/") == 0);
    assert(camera_parse_http_url("http://camera.local/video", host, sizeof(host), &port, path, sizeof(path)));
    assert(port == 80 && strcmp(path, "/video") == 0);
    assert(!camera_parse_http_url("https://camera.local/video", host, sizeof(host), &port, path, sizeof(path)));
    assert(!camera_parse_http_url("http://camera.local:bad/video", host, sizeof(host), &port, path, sizeof(path)));
}

static void take_frames(camera_mjpeg_parser_t *parser, const uint8_t *data, size_t len,
                        const uint8_t *expected1, size_t expected1_len,
                        const uint8_t *expected2, size_t expected2_len, int *count)
{
    bool have;
    do {
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;
        have = camera_mjpeg_feed(parser, data, len, &jpeg, &jpeg_len);
        data = NULL;
        len = 0;
        if (!have) break;
        if (*count == 0) {
            assert(jpeg_len == expected1_len && memcmp(jpeg, expected1, expected1_len) == 0);
        } else {
            assert(*count == 1 && jpeg_len == expected2_len);
            assert(memcmp(jpeg, expected2, expected2_len) == 0);
        }
        (*count)++;
        free(jpeg);
    } while (have);
}

static void test_mjpeg(void)
{
    const uint8_t frame1[] = {0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 0xff, 0xd9};
    const uint8_t frame2[] = {0xff, 0xd8, 0xff, 0xdb, 9, 8, 7, 6, 5, 0xff, 0xd9};
    uint8_t stream[1024];
    size_t len = 0;
    const char *part1 = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 9\r\n\r\n";
    const char *part2 = "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n";
    memcpy(stream + len, "garbage", 7); len += 7;
    memcpy(stream + len, part1, strlen(part1)); len += strlen(part1);
    memcpy(stream + len, frame1, sizeof(frame1)); len += sizeof(frame1);
    memcpy(stream + len, part2, strlen(part2)); len += strlen(part2);
    memcpy(stream + len, frame2, sizeof(frame2)); len += sizeof(frame2);
    memcpy(stream + len, "\r\n--frame\r\n", 11); len += 11;

    for (size_t chunk = 1; chunk <= len; chunk++) {
        camera_mjpeg_parser_t parser;
        assert(camera_mjpeg_init(&parser, 1024));
        int count = 0;
        for (size_t at = 0; at < len; at += chunk) {
            size_t n = len - at < chunk ? len - at : chunk;
            take_frames(&parser, stream + at, n, frame1, sizeof(frame1),
                        frame2, sizeof(frame2), &count);
        }
        take_frames(&parser, NULL, 0, frame1, sizeof(frame1), frame2, sizeof(frame2), &count);
        assert(count == 2);
        camera_mjpeg_free(&parser);
    }

    camera_mjpeg_parser_t parser;
    assert(camera_mjpeg_init(&parser, 1024));
    uint8_t junk[1000];
    memset(junk, 0x55, sizeof(junk));
    junk[0] = 0xff; junk[1] = 0xd8; junk[2] = 0xff;
    int count = 0;
    take_frames(&parser, junk, sizeof(junk), frame1, sizeof(frame1), frame2, sizeof(frame2), &count);
    take_frames(&parser, junk, sizeof(junk), frame1, sizeof(frame1), frame2, sizeof(frame2), &count);
    take_frames(&parser, (const uint8_t *)part1, strlen(part1), frame1, sizeof(frame1), frame2, sizeof(frame2), &count);
    take_frames(&parser, frame1, sizeof(frame1), frame1, sizeof(frame1), frame2, sizeof(frame2), &count);
    assert(count == 1);
    camera_mjpeg_free(&parser);
}

int main(void)
{
    test_list_and_urls();
    test_mjpeg();
    puts("PASS: camera discovery, URL resolution and fragmented/oversized MJPEG parsing");
    return 0;
}
