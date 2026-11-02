#include "camera_stream.h"

#include "app_settings.h"
#include "bsp_caps.h"
#include "camera_chunked.h"
#include "camera_mjpeg.h"

#if BSP_HAS_CAMERA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET stream_socket_t;
#define STREAM_SOCKET_INVALID INVALID_SOCKET
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int stream_socket_t;
#define STREAM_SOCKET_INVALID (-1)
#endif

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#ifdef ESP_PLATFORM
#define CAMERA_JPEG_CAP (3 * 1024 * 1024)
#define CAMERA_FRAME_INTERVAL_MS 150
#define CAMERA_LARGE_FRAME_BYTES (1024 * 1024)
#elif defined(_WIN32)
#define CAMERA_JPEG_CAP (8 * 1024 * 1024)
#define CAMERA_FRAME_INTERVAL_MS 50
#else
#define CAMERA_JPEG_CAP (8 * 1024 * 1024)
#define CAMERA_FRAME_INTERVAL_MS 50
#endif

struct camera_stream {
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    int refs;
    bool stop;
    stream_socket_t fd;
    camera_info_t camera;
    int max_w, max_h;
    camera_frame_t *latest;
    camera_stream_status_t status;
};

static void lock_stream(camera_stream_t *s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}

static void unlock_stream(camera_stream_t *s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}

static void close_socket(stream_socket_t fd)
{
    if (fd == STREAM_SOCKET_INVALID) return;
    shutdown(fd, 2);
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool stream_stopped(camera_stream_t *s)
{
    lock_stream(s);
    bool stop = s->stop;
    unlock_stream(s);
    return stop;
}

static void free_stream(camera_stream_t *s)
{
    camera_frame_free(s->latest);
#ifdef _WIN32
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
#endif
    free(s);
}

static void release_ref(camera_stream_t *s)
{
    lock_stream(s);
    bool last = --s->refs == 0;
    unlock_stream(s);
    if (last) free_stream(s);
}

static void set_state(camera_stream_t *s, camera_stream_state_t state)
{
    lock_stream(s);
    s->status.state = state;
    if (state != CAMERA_STREAM_PLAYING) s->status.fps_x10 = 0;
    unlock_stream(s);
}

static void note_decode_error(camera_stream_t *s)
{
    lock_stream(s);
    s->status.decode_errors++;
    unlock_stream(s);
}

static void publish_frame(camera_stream_t *s, camera_frame_t *frame, unsigned fps_x10)
{
    lock_stream(s);
    if (s->stop) {
        unlock_stream(s);
        camera_frame_free(frame);
        return;
    }
    camera_frame_free(s->latest);
    frame->sequence = ++s->status.received_frames;
#ifdef ESP_PLATFORM
    if (frame->sequence == 1)
        ESP_LOGI("camera", "first frame %ux%u", (unsigned)frame->dsc.header.w,
                 (unsigned)frame->dsc.header.h);
#endif
    s->latest = frame;
    s->status.state = CAMERA_STREAM_PLAYING;
    s->status.fps_x10 = fps_x10;
    unlock_stream(s);
}

static void release_socket(camera_stream_t *s, stream_socket_t fd)
{
    lock_stream(s);
    if (s->fd == fd) s->fd = STREAM_SOCKET_INVALID;
    else fd = STREAM_SOCKET_INVALID;
    unlock_stream(s);
    close_socket(fd);
}

static bool claim_socket(camera_stream_t *s, stream_socket_t fd)
{
    lock_stream(s);
    bool ok = !s->stop;
    if (ok) s->fd = fd;
    unlock_stream(s);
    if (!ok) close_socket(fd);
    return ok;
}

static uint32_t mono_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#elif defined(_WIN32)
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void sleep_ms(uint32_t ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#elif defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void interruptible_sleep(camera_stream_t *s, uint32_t ms)
{
    while (ms && !stream_stopped(s)) {
        uint32_t step = ms > 100 ? 100 : ms;
        sleep_ms(step);
        ms -= step;
    }
}

static int set_nonblocking(stream_socket_t fd, bool enabled)
{
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

static bool connect_in_progress(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
}

static stream_socket_t connect_socket(camera_stream_t *s, const char *host, uint16_t port)
{
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0) return STREAM_SOCKET_INVALID;

    stream_socket_t connected = STREAM_SOCKET_INVALID;
    for (struct addrinfo *ai = addresses; ai && !stream_stopped(s); ai = ai->ai_next) {
        stream_socket_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == STREAM_SOCKET_INVALID || !claim_socket(s, fd)) continue;
        set_nonblocking(fd, true);
        int rc = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0 && connect_in_progress()) {
            fd_set writes, errors;
            FD_ZERO(&writes);
            FD_ZERO(&errors);
            FD_SET(fd, &writes);
            FD_SET(fd, &errors);
            struct timeval timeout = { 3, 0 };
            rc = select((int)fd + 1, NULL, &writes, &errors, &timeout);
            if (rc > 0 && FD_ISSET(fd, &writes)) {
                int error = 0;
                socklen_t error_len = sizeof(error);
                rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &error_len) == 0 && !error ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        set_nonblocking(fd, false);
        if (rc == 0 && !stream_stopped(s)) {
#ifdef _WIN32
            DWORD timeout = 2500;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
            struct timeval timeout = { 2, 500000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
            int enabled = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
            connected = fd;
            break;
        }
        release_socket(s, fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

static bool send_request(stream_socket_t fd, const char *host, uint16_t port,
                         const char *path, const char *api_key, bool snapshot)
{
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nAccept: image/jpeg, multipart/x-mixed-replace\r\n"
                     "Connection: %s\r\n%s%s%s\r\n",
                     path, host, (unsigned)port, snapshot ? "close" : "keep-alive",
                     api_key[0] ? "X-Api-Key: " : "", api_key, api_key[0] ? "\r\n" : "");
    if (n <= 0 || n >= (int)sizeof(request)) return false;
    int sent = 0;
    while (sent < n) {
#ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
        int wrote = send(fd, request + sent, n - sent, flags);
        if (wrote <= 0) return false;
        sent += wrote;
    }
    return true;
}

static bool header_has_token(const char *header, const char *token)
{
    size_t len = strlen(token);
    for (const char *p = header; *p; p++) {
        size_t i = 0;
        while (i < len && p[i]) {
            char a = p[i], b = token[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (a != b) break;
            i++;
        }
        if (i == len) return true;
    }
    return false;
}

static bool read_headers(stream_socket_t fd, bool *chunked)
{
    char header[4096];
    size_t used = 0;
    *chunked = false;
    while (used + 1 < sizeof(header)) {
        char c;
        int got = recv(fd, &c, 1, 0);
        if (got != 1) return false;
        header[used++] = c;
        if (used >= 4 && memcmp(header + used - 4, "\r\n\r\n", 4) == 0) {
            header[used] = 0;
            if (strncmp(header, "HTTP/1.1 200", 12) != 0 &&
                strncmp(header, "HTTP/1.0 200", 12) != 0) return false;
            *chunked = header_has_token(header, "transfer-encoding: chunked");
            return true;
        }
    }
    return false;
}

static bool receive_frames(camera_stream_t *s, stream_socket_t fd, bool snapshot, bool chunked)
{
    camera_mjpeg_parser_t parser;
    if (!camera_mjpeg_init(&parser, CAMERA_JPEG_CAP)) return false;
    uint8_t *buf = malloc(16384);
    uint8_t *decoded = chunked ? malloc(16384) : NULL;
    if (!buf || (chunked && !decoded)) {
        free(buf);
        free(decoded);
        camera_mjpeg_free(&parser);
        return false;
    }
    camera_chunked_parser_t chunker;
    camera_chunked_init(&chunker);
    bool received = false;
    uint32_t last_decode = 0, fps_start = mono_ms(), fps_elapsed = 0;
    unsigned fps_frames = 0, fps_x10 = 0;

    while (!stream_stopped(s)) {
        int got = recv(fd, (char *)buf, 16384, 0);
        if (got <= 0) break;
        const uint8_t *feed_data = buf;
        size_t feed_len = (size_t)got;
        bool stream_done = false;
        if (chunked) {
            if (!camera_chunked_feed(&chunker, buf, (size_t)got,
                                     decoded, (size_t)got, &feed_len, &stream_done)) break;
            feed_data = decoded;
        }
        bool have_frame;
        do {
            uint8_t *jpeg = NULL;
            size_t jpeg_len = 0;
            have_frame = camera_mjpeg_feed(&parser, feed_data, feed_len, &jpeg, &jpeg_len);
            feed_data = NULL;
            feed_len = 0;
            if (!have_frame) break;
            received = true;
            uint32_t now = mono_ms();
            if (last_decode && now - last_decode < CAMERA_FRAME_INTERVAL_MS) {
                free(jpeg);
                continue;
            }
            last_decode = now;
#ifdef ESP_PLATFORM
            vTaskPrioritySet(NULL, jpeg_len > CAMERA_LARGE_FRAME_BYTES ? 0 : 1);
#endif
            camera_frame_t *frame = NULL;
            if (camera_jpeg_decode(jpeg, jpeg_len, s->max_w, s->max_h,
                                   s->camera.flip_horizontal, s->camera.flip_vertical,
                                   s->camera.rotation, &frame)) {
                fps_frames++;
                fps_elapsed = mono_ms() - fps_start;
                if (fps_elapsed >= 1000) {
                    fps_x10 = (unsigned)((uint64_t)fps_frames * 10000 / fps_elapsed);
                    fps_frames = 0;
                    fps_start = mono_ms();
                }
                publish_frame(s, frame, fps_x10);
            } else {
                note_decode_error(s);
            }
            free(jpeg);
            if (snapshot) goto done;
        } while (have_frame && !stream_stopped(s));
        if (stream_done) break;
    }
done:
    free(buf);
    free(decoded);
    camera_mjpeg_free(&parser);
    return received;
}

static void stream_run(camera_stream_t *s)
{
    moonraker_conf_t conf;
    if (!settings_load_moonraker(&conf) || !conf.valid) {
        set_state(s, CAMERA_STREAM_FAILED);
        return;
    }
    bool snapshot = !s->camera.stream_url[0] ||
                    strstr(s->camera.stream_url, "action=snapshot") != NULL;
    const char *raw_url = snapshot && s->camera.snapshot_url[0]
                              ? s->camera.snapshot_url : s->camera.stream_url;
    if (!raw_url[0] && s->camera.snapshot_url[0]) {
        snapshot = true;
        raw_url = s->camera.snapshot_url;
    }
    char url[CAMERA_URL_CAP + 96], host[128], path[CAMERA_URL_CAP];
    uint16_t port;
    if (!camera_build_url(conf.host, conf.port, raw_url, url, sizeof(url)) ||
        !camera_parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        set_state(s, CAMERA_STREAM_FAILED);
        return;
    }

    bool relative = strncmp(raw_url, "http://", 7) != 0 &&
                    strncmp(raw_url, "https://", 8) != 0 && strncmp(raw_url, "//", 2) != 0;
    uint16_t fallback_port = relative && conf.port && conf.port != port ? conf.port : 0;
    unsigned attempt = 0;
    bool first = true;
    while (!stream_stopped(s)) {
        set_state(s, first ? CAMERA_STREAM_CONNECTING : CAMERA_STREAM_RECONNECTING);
        first = false;
        uint16_t active_port = fallback_port && (attempt++ & 1) ? fallback_port : port;
        stream_socket_t fd = connect_socket(s, host, active_port);
        bool received = false;
        if (fd != STREAM_SOCKET_INVALID) {
            bool chunked = false;
            if (send_request(fd, host, active_port, path, conf.api_key, snapshot) &&
                read_headers(fd, &chunked))
                received = receive_frames(s, fd, snapshot, chunked);
            release_socket(s, fd);
        }
        if (stream_stopped(s)) break;
        interruptible_sleep(s, snapshot && received ? 300 : 1500);
    }
}

#ifdef ESP_PLATFORM
static void stream_task(void *arg)
{
    camera_stream_t *s = arg;
    stream_run(s);
    release_ref(s);
    vTaskDelete(NULL);
}
#elif defined(_WIN32)
static DWORD WINAPI stream_thread(void *arg)
{
    camera_stream_t *s = arg;
    if (WSAStartup(MAKEWORD(2, 2), &(WSADATA){0}) == 0) {
        stream_run(s);
        WSACleanup();
    } else {
        set_state(s, CAMERA_STREAM_FAILED);
    }
    release_ref(s);
    return 0;
}
#else
static void *stream_thread(void *arg)
{
    camera_stream_t *s = arg;
    stream_run(s);
    release_ref(s);
    return NULL;
}
#endif

camera_stream_t *camera_stream_start(const camera_info_t *camera, int max_w, int max_h)
{
    if (!camera || max_w <= 0 || max_h <= 0) return NULL;
    camera_stream_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->refs = 2;
    s->fd = STREAM_SOCKET_INVALID;
    s->camera = *camera;
    s->max_w = max_w;
    s->max_h = max_h;
#ifdef _WIN32
    InitializeCriticalSection(&s->lock);
    HANDLE thread = CreateThread(NULL, 0, stream_thread, s, 0, NULL);
    if (!thread) {
        free_stream(s);
        return NULL;
    }
    CloseHandle(thread);
#else
    pthread_mutex_init(&s->lock, NULL);
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(stream_task, "camera", 12288, s, 1, NULL, 0) != pdPASS) {
        free_stream(s);
        return NULL;
    }
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, stream_thread, s) != 0) {
        free_stream(s);
        return NULL;
    }
    pthread_detach(thread);
#endif
#endif
    return s;
}

void camera_stream_stop(camera_stream_t *s)
{
    if (!s) return;
    lock_stream(s);
    s->stop = true;
    stream_socket_t fd = s->fd;
    s->fd = STREAM_SOCKET_INVALID;
    unlock_stream(s);
    close_socket(fd);
    release_ref(s);
}

bool camera_stream_poll(camera_stream_t *s, camera_frame_t **frame,
                        camera_stream_status_t *status)
{
    if (!s || !frame) return false;
    *frame = NULL;
    lock_stream(s);
    if (s->latest) {
        *frame = s->latest;
        s->latest = NULL;
    }
    if (status) *status = s->status;
    unlock_stream(s);
    return true;
}

#else

camera_stream_t *camera_stream_start(const camera_info_t *camera, int max_w, int max_h)
{
    (void)camera; (void)max_w; (void)max_h;
    return NULL;
}

void camera_stream_stop(camera_stream_t *stream) { (void)stream; }

bool camera_stream_poll(camera_stream_t *stream, camera_frame_t **frame,
                        camera_stream_status_t *status)
{
    (void)stream; (void)frame; (void)status;
    return false;
}

#endif
