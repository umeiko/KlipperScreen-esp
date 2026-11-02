#include "camera.h"

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src, bool single_line)
{
    size_t n = 0;
    if (!cap) return;
    if (src) {
        for (; src[n] && n + 1 < cap; n++) {
            unsigned char c = (unsigned char)src[n];
            dst[n] = single_line && (c == '\r' || c == '\n') ? ' ' : (char)c;
        }
    }
    dst[n] = 0;
}

bool camera_parse_list_json(const char *json, camera_list_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *webcams = cJSON_GetObjectItemCaseSensitive(result ? result : root, "webcams");
    if (!cJSON_IsArray(webcams)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *item;
    cJSON_ArrayForEach(item, webcams) {
        if (out->count >= CAMERA_MAX_CAMERAS) break;
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) continue;
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *stream = cJSON_GetObjectItemCaseSensitive(item, "stream_url");
        cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(item, "snapshot_url");
        camera_info_t *cam = &out->items[out->count];
        copy_text(cam->name, sizeof(cam->name), cJSON_IsString(name) ? name->valuestring : NULL, true);
        copy_text(cam->stream_url, sizeof(cam->stream_url),
                  cJSON_IsString(stream) ? stream->valuestring : NULL, false);
        copy_text(cam->snapshot_url, sizeof(cam->snapshot_url),
                  cJSON_IsString(snapshot) ? snapshot->valuestring : NULL, false);
        cam->enabled = true;
        cam->flip_horizontal = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "flip_horizontal"));
        cam->flip_vertical = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "flip_vertical"));
        cJSON *rotation = cJSON_GetObjectItemCaseSensitive(item, "rotation");
        if (cJSON_IsNumber(rotation)) {
            int angle = ((int)rotation->valuedouble % 360 + 360) % 360;
            cam->rotation = angle == 90 || angle == 180 || angle == 270 ? angle : 0;
        }
        if (!cam->name[0]) snprintf(cam->name, sizeof(cam->name), "Camera %d", out->count + 1);
        out->count++;
    }
    cJSON_Delete(root);
    return true;
}

static bool replace_webrtc(char *url, size_t cap)
{
    char *at = strstr(url, "/webrtc");
    if (!at) return true;
    size_t tail = strlen(at + 7);
    if ((size_t)(at - url) + 7 + tail + 1 > cap) return false;
    memcpy(at, "/stream", 7);
    return true;
}

bool camera_build_url(const char *moonraker_host, uint16_t moonraker_port,
                      const char *raw_url, char *out, size_t out_cap)
{
    if (!moonraker_host || !moonraker_host[0] || !raw_url || !raw_url[0] || !out || !out_cap)
        return false;
    (void)moonraker_port;
    int n;
    if (strncmp(raw_url, "http://", 7) == 0 || strncmp(raw_url, "https://", 8) == 0) {
        n = snprintf(out, out_cap, "%s", raw_url);
    } else if (strncmp(raw_url, "//", 2) == 0) {
        n = snprintf(out, out_cap, "http:%s", raw_url);
    } else {
        n = snprintf(out, out_cap, "http://%s%s%s", moonraker_host,
                     raw_url[0] == '/' ? "" : "/", raw_url);
    }
    return n > 0 && (size_t)n < out_cap && replace_webrtc(out, out_cap);
}

bool camera_parse_http_url(const char *url, char *host, size_t host_cap,
                           uint16_t *port, char *path, size_t path_cap)
{
    if (!url || strncmp(url, "http://", 7) != 0 || !host || !port || !path) return false;
    const char *authority = url + 7;
    const char *end = authority + strcspn(authority, "/?#");
    const char *host_end = end;
    const char *colon = NULL;
    for (const char *p = authority; p < end; p++) {
        if (*p == '@') return false;
        if (*p == ':') colon = p;
    }
    *port = 80;
    if (colon) {
        char *num_end = NULL;
        long value = strtol(colon + 1, &num_end, 10);
        if (num_end != end || value <= 0 || value > 65535) return false;
        *port = (uint16_t)value;
        host_end = colon;
    }
    size_t host_len = (size_t)(host_end - authority);
    if (!host_len || host_len >= host_cap) return false;
    memcpy(host, authority, host_len);
    host[host_len] = 0;

    int n;
    if (*end == '/') {
        const char *fragment = strchr(end, '#');
        size_t len = fragment ? (size_t)(fragment - end) : strlen(end);
        if (len >= path_cap) return false;
        memcpy(path, end, len);
        path[len] = 0;
        n = (int)len;
    } else if (*end == '?') {
        n = snprintf(path, path_cap, "/%s", end);
    } else {
        n = snprintf(path, path_cap, "/");
    }
    return n > 0 && (size_t)n < path_cap;
}
