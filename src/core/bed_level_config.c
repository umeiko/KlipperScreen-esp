#include "bed_level_config.h"
#if BSP_HAS_BED_LEVEL
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { OTHER, ROOT, STATUS, CONFIGFILE, CONFIG };
enum { KEY, COLON, VALUE, COMMA };
static bool append(bed_level_config_stream_t *s, char c)
{
    if (s->used + 2 >= sizeof(s->json)) return !(s->failed = true);
    s->json[s->used++] = c;
    return true;
}
void bed_level_config_begin(bed_level_config_stream_t *s)
{
    memset(s, 0, sizeof(*s)); s->json[s->used++] = '{';
}
static bool wanted(const char *key)
{
    return !strcmp(key, "bed_screws") || !strcmp(key, "screws_tilt_adjust") ||
           !strcmp(key, "probe") || !strcmp(key, "bltouch") ||
           !strcmp(key, "smart_effector") || !strncmp(key, "probe_eddy_current ", 19);
}
bool bed_level_config_feed(bed_level_config_stream_t *s, const char *data, size_t size)
{
    if (s->failed || size > 1024 * 1024 - s->bytes) return !(s->failed = true);
    s->bytes += size;
    for (size_t i = 0; i < size; i++) {
        char c = data[i];
        if (s->capture && !append(s, c)) return false;
        if (s->string) {
            if (s->escape) { s->escape = false; s->key_overflow = true; }
            else if (c == '\\') s->escape = true;
            else if (c == '"') {
                s->string = false;
                if (s->is_key) {
                    s->key[s->key_len] = 0;
                    if (s->key_overflow) s->key[0] = 0;
                    s->expect[s->depth - 1] = COLON;
                } else s->expect[s->depth - 1] = COMMA;
            } else if (s->is_key) {
                if (s->key_len + 1 < sizeof(s->key)) s->key[s->key_len++] = c;
                else s->key_overflow = true;
            }
            continue;
        }
        if (isspace((unsigned char)c)) continue;
        if (s->closed) return !(s->failed = true);
        if (c == '"') {
            if (!s->depth) return !(s->failed = true);
            s->string = true; s->escape = false;
            s->is_key = s->expect[s->depth - 1] == KEY;
            if (s->is_key) { s->key_len = 0; s->key_overflow = false; }
        } else if (c == '{' || c == '[') {
            if (s->depth >= 24) return !(s->failed = true);
            unsigned role = OTHER;
            if (!s->depth) { if (c != '{') return !(s->failed = true); role = ROOT; }
            else if (c == '{' && s->expect[s->depth - 1] == VALUE) {
                unsigned parent = s->role[s->depth - 1];
                if (parent == ROOT && !strcmp(s->key, "result")) role = ROOT;
                else if (parent == ROOT && !strcmp(s->key, "status")) role = STATUS;
                else if (parent == STATUS && !strcmp(s->key, "configfile")) role = CONFIGFILE;
                else if (parent == CONFIGFILE && !strcmp(s->key, "config")) { role = CONFIG; s->found = true; }
                else if (parent == CONFIG && wanted(s->key) && !s->capture) {
                    if (s->used > 1 && !append(s, ',')) return false;
                    if (!append(s, '"')) return false;
                    for (const char *p = s->key; *p; p++) if (!append(s, *p)) return false;
                    if (!append(s, '"') || !append(s, ':') || !append(s, c)) return false;
                    s->capture = true; s->capture_depth = s->depth;
                }
            }
            if (s->depth) s->expect[s->depth - 1] = COMMA;
            s->role[s->depth] = role; s->container[s->depth] = c;
            s->expect[s->depth++] = c == '{' ? KEY : VALUE;
        } else if (c == '}' || c == ']') {
            if (!s->depth || s->container[s->depth - 1] != (c == '}' ? '{' : '[')) return !(s->failed = true);
            s->depth--;
            if (s->capture && s->depth == s->capture_depth) s->capture = false;
            if (!s->depth) s->closed = true;
        } else if (c == ':') {
            if (!s->depth || s->expect[s->depth - 1] != COLON) return !(s->failed = true);
            s->expect[s->depth - 1] = VALUE;
        } else if (c == ',') {
            if (!s->depth) return !(s->failed = true);
            s->expect[s->depth - 1] = s->container[s->depth - 1] == '{' ? KEY : VALUE;
        } else if (!s->depth || s->expect[s->depth - 1] != VALUE) return !(s->failed = true);
        /* Primitive values remain VALUE until their separating comma/brace. */
    }
    return !s->failed;
}
static bool number(cJSON *o, const char *key, float fallback, float *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) { *out = fallback; return true; }
    char *end = NULL;
    if (cJSON_IsNumber(v)) *out = (float)v->valuedouble;
    else if (cJSON_IsString(v)) {
        *out = strtof(v->valuestring, &end);
        if (end == v->valuestring) return false;
        while (isspace((unsigned char)*end)) end++;
        if (*end) return false;
    } else return false;
    return isfinite(*out);
}
bool bed_level_config_finish(bed_level_config_stream_t *s, bed_level_data_t *out)
{
    memset(out, 0, sizeof(*out)); out->state = BED_LEVEL_FAILED;
    if (s->failed || s->string || s->depth || !s->closed || !s->found || !append(s, '}')) return false;
    s->json[s->used] = 0;
    cJSON *root = cJSON_Parse(s->json);
    if (!root) return false;
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(root, "screws_tilt_adjust");
    out->has_manual = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "bed_screws"));
    out->probe = cJSON_IsObject(cfg);
    if (!out->probe) cfg = cJSON_GetObjectItemCaseSensitive(root, "bed_screws");
    out->state = cfg ? BED_LEVEL_INVALID : BED_LEVEL_MISSING;
    bool ok = !cfg;
    if (cJSON_IsObject(cfg)) {
        ok = number(cfg, "horizontal_move_z", 5, &out->travel_z) &&
             number(cfg, "probe_height", 0, &out->paper_z) &&
             number(cfg, "speed", 50, &out->speed) && number(cfg, "probe_speed", 5, &out->lift_speed);
        for (int i = 0; ok && i < 5; i++) {
            char key[32]; snprintf(key, sizeof(key), "screw%d", i + 1);
            cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, key);
            if (!v) break;
            if (i == 4 || !cJSON_IsString(v)) { ok = false; break; }
            bed_level_point_t *p = &out->points[i]; char extra;
            if (sscanf(v->valuestring, " %f , %f %c", &p->x, &p->y, &extra) != 2 ||
                !isfinite(p->x) || !isfinite(p->y)) { ok = false; break; }
            snprintf(key, sizeof(key), "screw%d_name", i + 1);
            cJSON *name = cJSON_GetObjectItemCaseSensitive(cfg, key);
            if (cJSON_IsString(name)) {
                size_t n = strlen(name->valuestring);
                if (n >= sizeof(p->name)) {
                    n = sizeof(p->name)-1;
                    while (n && ((unsigned char)name->valuestring[n] & 0xc0) == 0x80) n--;
                }
                memcpy(p->name, name->valuestring, n); p->name[n] = 0;
            }
            if (!p->name[0]) snprintf(p->name, sizeof(p->name), "%d", i + 1);
            out->count++;
        }
        /* Tilt coordinates command the nozzle with the probe above the screw.
           Add the probe offset to put the nozzle itself over that screw. */
        if (out->probe) {
            cJSON *probe = NULL, *entry;
            cJSON_ArrayForEach(entry, root) if (entry->string &&
                (!strcmp(entry->string, "probe") || !strcmp(entry->string, "bltouch") ||
                 !strcmp(entry->string, "smart_effector") || !strncmp(entry->string, "probe_eddy_current ", 19))) { probe = entry; break; }
            float x = 0, y = 0;
            ok = ok && number(probe, "x_offset", 0, &x) && number(probe, "y_offset", 0, &y);
            for (int i = 0; i < out->count; i++) { out->points[i].x += x; out->points[i].y += y; }
        }
        ok = ok && (out->count == 3 || out->count == 4) && out->speed > 0 && out->speed <= 10000 &&
             out->lift_speed > 0 && out->lift_speed <= 10000 &&
             out->travel_z >= out->paper_z && out->travel_z >= 0;
        for (int i = 0; i < out->count; i++) for (int j = 0; j < i; j++)
            if (hypotf(out->points[i].x - out->points[j].x, out->points[i].y - out->points[j].y) < 1) ok = false;
        /* A 2D bed diagram cannot represent a collapsed/collinear screw set. */
        if (out->count >= 3 && fabsf((out->points[1].x-out->points[0].x)*(out->points[2].y-out->points[0].y) -
            (out->points[2].x-out->points[0].x)*(out->points[1].y-out->points[0].y)) < 1) ok = false;
        if (ok) out->state = BED_LEVEL_READY;
    }
    cJSON_Delete(root);
    return true;
}
#endif
