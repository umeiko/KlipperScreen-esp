#include "gcode_thumbnail.h"
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 缩略图注释块一般在文件头（PrusaSlicer/Orca/Cura）；个别切片器放尾部。 */
#define SCAN_CHUNK (512 * 1024)

bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap)
{
    if (!gcode_name || !gcode_name[0]) return false;
    const char *dir = getenv("KLIPPER_GCODES_DIR");
    char fallback[512];
    if (!dir || !dir[0]) {
        const char *home = getenv("HOME");
        if (!home) return false;
        snprintf(fallback, sizeof(fallback), "%s/printer_data/gcodes", home);
        dir = fallback;
    }
    int n = snprintf(out, cap, "%s/%s", dir, gcode_name);
    return n > 0 && (size_t)n < cap;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 就地解码 base64 段（已去掉 "; " 前缀与空白），返回解码后长度（<=srclen）。 */
static size_t b64_decode(const char *src, size_t srclen, unsigned char *dst)
{
    size_t out = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < srclen; i++) {
        if (src[i] == '=') break;
        int v = b64val((unsigned char)src[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

/* 在一块文本缓冲里找最大缩略图；命中返回 malloc 的 PNG（调用方 free）。 */
static bool scan_buf(const char *buf, size_t len, unsigned char **png_out, size_t *png_len)
{
    const char *p = buf, *end = buf + len;
    unsigned char *best = NULL;
    size_t best_len = 0;
    long best_area = 0;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        size_t ll = (size_t)(line_end - p);

        int w = 0, h = 0;
        if (ll > 18 && memcmp(p, "; thumbnail begin", 17) == 0 &&
            sscanf(p + 17, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            /* 收集 base64 行直到 "; thumbnail end"；base64 行以 "; " 开头 */
            size_t b64_cap = (size_t)w * h * 3 + 1024;   /* PNG 通常远小于 raw RGB */
            char *b64 = malloc(b64_cap);
            size_t b64_len = 0;
            const char *q = line_end < end ? line_end + 1 : end;
            bool closed = false;
            while (b64 && q < end) {
                const char *qe = memchr(q, '\n', (size_t)(end - q));
                if (!qe) qe = end;
                size_t ql = (size_t)(qe - q);
                if (ql >= 15 && memcmp(q, "; thumbnail end", 15) == 0) { closed = true; break; }
                if (ql > 2 && q[0] == ';' && q[1] == ' ') {
                    size_t payload = ql - 2;
                    if (payload > 0 && (q[2] == ';' || payload > 1024 * 10)) break;  /* 注释跑偏 */
                    if (b64_len + payload + 1 > b64_cap) {
                        b64_cap = (b64_len + payload + 1) * 2;
                        char *nb = realloc(b64, b64_cap);
                        if (!nb) { free(b64); b64 = NULL; break; }
                        b64 = nb;
                    }
                    memcpy(b64 + b64_len, q + 2, payload);
                    b64_len += payload;
                } else break;   /* 非缩略图行：块损坏，放弃本块 */
                q = qe < end ? qe + 1 : end;
            }
            if (b64 && closed && b64_len > 8) {
                unsigned char *png = malloc(b64_len * 3 / 4 + 4);
                if (png) {
                    size_t plen = b64_decode(b64, b64_len, png);
                    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
                    if (plen > sizeof(magic) && memcmp(png, magic, sizeof(magic)) == 0) {
                        long area = (long)w * h;
                        if (area > best_area) {
                            free(best);
                            best = png; best_len = plen; best_area = area;
                        } else free(png);
                    } else free(png);
                }
            }
            free(b64);
            p = q;
            continue;
        }
        p = line_end < end ? line_end + 1 : end;
    }

    if (!best) return false;
    *png_out = best;
    *png_len = best_len;
    return true;
}

bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len)
{
    FILE *f = fopen(gcode_path, "rb");
    if (!f) return false;
    char *buf = malloc(SCAN_CHUNK);
    if (!buf) { fclose(f); return false; }

    bool ok = false;
    size_t n = fread(buf, 1, SCAN_CHUNK, f);
    if (n > 0) ok = scan_buf(buf, n, png_out, png_len);
    if (!ok) {
        /* 头部没找到：扫尾部一段（SuperSlicer 等放在文件尾） */
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            long off = sz > SCAN_CHUNK ? sz - SCAN_CHUNK : 0;
            if (off > 0 && fseek(f, off, SEEK_SET) == 0) {
                n = fread(buf, 1, SCAN_CHUNK, f);
                if (n > 0) ok = scan_buf(buf, n, png_out, png_len);
            }
        }
    }
    free(buf);
    fclose(f);
    return ok;
}

#else /* !BSP_HAS_LINUX_HOST */

bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap)
{
    (void)gcode_name; (void)out; (void)cap;
    return false;
}

bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len)
{
    (void)gcode_path; (void)png_out; (void)png_len;
    return false;
}

#endif
