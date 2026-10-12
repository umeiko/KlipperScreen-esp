#include "file_list_stream.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { size_t size; long double align; } block_t;
static size_t live, peak;
static int alloc_left = -1;
static void *tracked_alloc(size_t n)
{
    if (alloc_left == 0) return NULL;
    if (alloc_left > 0) alloc_left--;
    block_t *b = malloc(sizeof(*b) + n);
    assert(b); b->size = n;
    live += n; if (live > peak) peak = live;
    return b + 1;
}
static void tracked_free(void *p)
{
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    live -= b->size; free(b);
}
static bool parse(file_list_stream_t *s, const char *json, size_t chunk)
{
    for (size_t at = 0, len = strlen(json); at < len; at += chunk) {
        size_t n = len - at < chunk ? len - at : chunk;
        if (!file_list_stream_feed(s, json + at, n)) return false;
    }
    return file_list_stream_finish(s);
}
int main(void)
{
    cJSON_Hooks hooks = { tracked_alloc, tracked_free };
    cJSON_InitHooks(&hooks);
    file_list_stream_t s;
    const char *sample = "{\"result\":[{\"path\":\"dir/\\u4e2d}\\\"x.gcode\",\"size\":12345,\"modified\":1.5},"
        "{\"path\":\".hidden\",\"size\":1},{\"path\":\"directory\"}]}";
    for (size_t chunk = 1; chunk <= strlen(sample); chunk++) {
        file_list_stream_init(&s, 0);
        assert(parse(&s, sample, chunk));
        assert(s.page.count == 1 && s.page.total == 1);
        assert(strcmp(s.page.files[0].name, "dir/中}\"x.gcode") == 0);
        assert(s.page.files[0].size == 12345 && live == 0);
    }
    const char *bad[] = { "", "{}", "{\"error\":{}}", "[", "[{},]", "[{}", "[{}]x", "[null]", "[{\"path\":]", "[{}{}]" };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        file_list_stream_init(&s, 0);
        assert(!parse(&s, bad[i], 1));
        assert(live == 0);
    }
    file_list_stream_init(&s, 0);
    assert(parse(&s, "{\"result\":[]}", 1) && s.page.count == 0);
    char oversized[FILE_LIST_RECORD_MAX + 128];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "[{\"path\":\"", 10); oversized[sizeof(oversized)-1] = 0;
    file_list_stream_init(&s, 0);
    assert(!parse(&s, oversized, 31));
    char long_path[240];
    memset(long_path, 'a', sizeof(long_path));
    memcpy(long_path, "[{\"path\":\"", 10);
    strcpy(long_path + 130, "\",\"size\":1}]");
    file_list_stream_init(&s, 0);
    assert(parse(&s, long_path, 3) && s.page.skipped == 1 && s.page.count == 0);
    for (int fail_after = 0; fail_after < 9; fail_after++) {
        alloc_left = fail_after;
        file_list_stream_init(&s, 0);
        (void)parse(&s, sample, 1);
        assert(live == 0);
    }
    alloc_left = -1;
    for (unsigned total = 1000; total <= 10000; total *= 10) {
        peak = 0;
        file_list_stream_init(&s, total - 5);
        assert(file_list_stream_feed(&s, "{\"result\":[", 11));
        for (unsigned i = 0; i < total; i++) {
            char record[160];
            int n = snprintf(record, sizeof(record), "%s{\"path\":\"part_%05u.gcode\",\"size\":12345678,\"modified\":1756800000}", i ? "," : "", i);
            /* Feed real fragments, not an already-parsed array. */
            for (int at = 0; at < n; at += 7)
                assert(file_list_stream_feed(&s, record + at, n-at < 7 ? n-at : 7));
        }
        assert(file_list_stream_feed(&s, "]}", 2) && file_list_stream_finish(&s));
        assert(s.page.total == total && s.page.count == 5 && live == 0);
        char expected[48]; snprintf(expected, sizeof(expected), "part_%05u.gcode", total - 5);
        assert(strcmp(s.page.files[0].name, expected) == 0);
        assert(peak < 2048);
        printf("PASS: %u files, retained=%d parser_bytes=%zu cJSON_peak=%zu\n", total, s.page.count, sizeof(s), peak);
    }
    file_list_stream_init(&s, 0); s.bytes = FILE_LIST_MAX_BYTES;
    assert(!file_list_stream_feed(&s, " ", 1));
    cJSON_InitHooks(NULL);
    puts("PASS: arbitrary chunks, escapes, empty, invalid/truncated/oversized input, allocation failure, long-path safety");
    return 0;
}
