#include "file_list_stream.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>
#include <ctype.h>

enum { START, KEY, KEY_TEXT, KEY_END, COLON, ARRAY, ITEM, RECORD, COMMA, CLOSE, DONE };

void file_list_stream_init(file_list_stream_t *s, unsigned offset)
{
    memset(s, 0, sizeof(*s));
    s->offset = offset;
}

static bool consume_record(file_list_stream_t *s)
{
    s->record[s->used] = 0;
    cJSON *obj = cJSON_Parse(s->record);
    if (!obj) return false;
    cJSON *path = cJSON_GetObjectItemCaseSensitive(obj, "path");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(obj, "size");
    cJSON *modified = cJSON_GetObjectItemCaseSensitive(obj, "modified");
    bool valid = cJSON_IsString(path) && path->valuestring[0] &&
                 path->valuestring[0] != '.' && cJSON_IsNumber(size) &&
                 isfinite(size->valuedouble) && size->valuedouble >= 0 &&
                 size->valuedouble <= UINT32_MAX;
    if (valid && strlen(path->valuestring) >= sizeof(s->page.files[0].name)) {
        s->page.skipped++;  /* Never print/delete a silently truncated path. */
    } else if (valid) {
        if (s->page.total >= s->offset && s->page.count < PRINTER_FILES_PAGE_SIZE) {
            printer_file_t *f = &s->page.files[s->page.count++];
            strcpy(f->name, path->valuestring);
            f->size = (uint32_t)size->valuedouble;
            f->modified = cJSON_IsNumber(modified) && isfinite(modified->valuedouble)
                ? modified->valuedouble : 0;
        }
        s->page.total++;
    }
    cJSON_Delete(obj);
    return true;
}

bool file_list_stream_feed(file_list_stream_t *s, const char *data, size_t len)
{
    if (s->failed || len > FILE_LIST_MAX_BYTES - s->bytes) { s->failed = true; return false; }
    s->bytes += len;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (s->phase == RECORD) {
            if (s->used >= FILE_LIST_RECORD_MAX) goto fail;
            s->record[s->used++] = (char)c;
            if (s->in_string) {
                if (s->escaped) s->escaped = false;
                else if (c == '\\') s->escaped = true;
                else if (c == '"') s->in_string = false;
            } else if (c == '"') s->in_string = true;
            else if (c == '{' || c == '[') {
                if (++s->depth > 8) goto fail;
            } else if (c == '}' || c == ']') {
                if (--s->depth == 0) {
                    if (c != '}' || !consume_record(s)) goto fail;
                    s->phase = COMMA;
                }
            }
            continue;
        }
        if (s->phase != KEY_TEXT && s->phase != KEY_END && isspace(c)) continue;
        switch (s->phase) {
        case START:
            if (c == '[') s->phase = ITEM;
            else if (c == '{') { s->envelope = true; s->phase = KEY; }
            else goto fail;
            break;
        case KEY: if (c != '"') goto fail; s->phase = KEY_TEXT; break;
        case KEY_TEXT:
            if (c != (unsigned char)"result"[s->prefix++]) goto fail;
            if (s->prefix == 6) s->phase = KEY_END;
            break;
        case KEY_END: if (c != '"') goto fail; s->phase = COLON; break;
        case COLON: if (c != ':') goto fail; s->phase = ARRAY; break;
        case ARRAY: if (c != '[') goto fail; s->phase = ITEM; break;
        case ITEM:
            if (c == ']' && !s->need_item) { s->phase = s->envelope ? CLOSE : DONE; break; }
            if (c != '{') goto fail;
            s->record[0] = '{'; s->used = 1; s->depth = 1;
            s->in_string = s->escaped = s->need_item = false;
            s->phase = RECORD;
            break;
        case COMMA:
            if (c == ',') { s->need_item = true; s->phase = ITEM; }
            else if (c == ']') s->phase = s->envelope ? CLOSE : DONE;
            else goto fail;
            break;
        case CLOSE: if (c != '}') goto fail; s->phase = DONE; break;
        default: goto fail;
        }
    }
    return true;
fail:
    s->failed = true;
    return false;
}

bool file_list_stream_finish(file_list_stream_t *s)
{
    if (s->failed || s->phase != DONE) { s->page.count = -1; return false; }
    return true;
}
