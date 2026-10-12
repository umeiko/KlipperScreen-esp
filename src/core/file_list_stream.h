#pragma once
#include "printer.h"
#include <stddef.h>

/* Moonraker files/list: array (RPC result), or {"result":[...]} (HTTP).
 * One bounded record at a time; only one requested page is retained. */
#define FILE_LIST_RECORD_MAX 1024
#define FILE_LIST_MAX_BYTES (8u * 1024u * 1024u)
typedef struct {
    printer_file_page_t page;
    unsigned offset, phase, prefix, depth;
    size_t bytes, used;
    bool envelope, in_string, escaped, need_item, failed;
    char record[FILE_LIST_RECORD_MAX + 1];
} file_list_stream_t;
void file_list_stream_init(file_list_stream_t *s, unsigned offset);
bool file_list_stream_feed(file_list_stream_t *s, const char *data, size_t len);
bool file_list_stream_finish(file_list_stream_t *s);
