/*
 * Bambu report 零分配流式解析器——逐字节状态机，分片边界无感。
 * 结构解析（容器栈 + 容器内期待状态）与标量捕获（键/字符串/数字/字面量）
 * 全部定长；捕获只在「根对象 print 成员的直接子字段」这一层开窗，
 * 其余层级只扫描不存储。merge 语义见头文件注释，对齐 bambu_status.c。
 */
#include "bambu_status_stream.h"

#include <string.h>

/* 容器内期待什么 */
enum {
    L_OBJ_KEY,      /* 期望键字符串或 '}'（仅 '{' 之后：空对象允许直接闭合） */
    L_OBJ_KEY2,     /* 逗号之后：只许键字符串（"[,}]" 尾逗号判畸形） */
    L_OBJ_COLON,    /* 期望 ':' */
    L_OBJ_VALUE,    /* 期望成员值 */
    L_OBJ_COMMA,    /* 期望 ',' 或 '}' */
    L_ARR_VALUE,    /* 期望元素值或 ']'（仅 '[' 之后：空数组允许直接闭合） */
    L_ARR_VALUE2,   /* 逗号之后：只许元素值（"[1,]" 尾逗号判畸形） */
    L_ARR_COMMA,    /* 期望 ',' 或 ']' */
};

/* 扫描模式 */
enum {
    M_ROOT,         /* 等根对象 '{' */
    M_STRUCT,       /* 结构字符/空白 */
    M_KEY,          /* 对象键字符串中 */
    M_STRING,       /* 标量字符串值中 */
    M_NUMBER,       /* 数字 token 中 */
    M_LIT,          /* true/false/null 中 */
    M_DONE,         /* 根对象已闭合，只许空白 */
};

/* 目标字段（print 直接子字段） */
enum {
    F_NONE = 0,
    F_GCODE_STATE,
    F_NOZZLE,
    F_NOZZLE_T,
    F_BED,
    F_BED_T,
    F_PERCENT,
    F_REMAIN,
    F_LAYER,
    F_LAYER_TOTAL,
    F_SUBTASK,
    F_GCODE_FILE,
    F_COUNT
};

static uint16_t fbit(int f) { return (uint16_t)(1u << (f - 1)); }

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ---------- 标量小工具 ---------- */

/* 键比对（大小写敏感，对齐 cJSON GetObjectItemCaseSensitive） */
static int match_field(const char *key)
{
    if (strcmp(key, "gcode_state") == 0) return F_GCODE_STATE;
    if (strcmp(key, "nozzle_temper") == 0) return F_NOZZLE;
    if (strcmp(key, "nozzle_target_temper") == 0) return F_NOZZLE_T;
    if (strcmp(key, "bed_temper") == 0) return F_BED;
    if (strcmp(key, "bed_target_temper") == 0) return F_BED_T;
    if (strcmp(key, "mc_percent") == 0) return F_PERCENT;
    if (strcmp(key, "mc_remaining_time") == 0) return F_REMAIN;
    if (strcmp(key, "layer_num") == 0) return F_LAYER;
    if (strcmp(key, "total_layer_num") == 0) return F_LAYER_TOTAL;
    if (strcmp(key, "subtask_name") == 0) return F_SUBTASK;
    if (strcmp(key, "gcode_file") == 0) return F_GCODE_FILE;
    return F_NONE;
}

static bambu_print_state_t parse_state(const char *v)
{
    if (strcmp(v, "RUNNING") == 0) return BAMBU_PRINT_RUNNING;
    if (strcmp(v, "PAUSE") == 0) return BAMBU_PRINT_PAUSED;
    if (strcmp(v, "PREPARE") == 0) return BAMBU_PRINT_PREPARE;
    if (strcmp(v, "FINISH") == 0) return BAMBU_PRINT_FINISHED;
    if (strcmp(v, "FAILED") == 0) return BAMBU_PRINT_FAILED;
    if (strcmp(v, "IDLE") == 0) return BAMBU_PRINT_IDLE;
    return BAMBU_PRINT_UNKNOWN;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 手工 JSON 数字 → double（避开 strtod 的 locale 依赖）。
   语法：-?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?，全长 ≤ BAMBUSTREAM_NUM_MAX。 */
static bool num_to_double(const char *s, size_t n, double *out)
{
    size_t i = 0;
    bool neg = false;
    if (i < n && s[i] == '-') { neg = true; i++; }

    size_t int_digits = 0;
    double v = 0.0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + (double)(s[i] - '0');
        i++;
        int_digits++;
    }
    if (!int_digits) return false;

    if (i < n && s[i] == '.') {
        i++;
        size_t frac_digits = 0;
        double scale = 1.0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10.0 + (double)(s[i] - '0');
            scale *= 10.0;
            i++;
            frac_digits++;
        }
        if (!frac_digits) return false;
        v /= scale;
    }

    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) { eneg = s[i] == '-'; i++; }
        size_t exp_digits = 0;
        int e = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (e < 10000) e = e * 10 + (s[i] - '0');
            i++;
            exp_digits++;
        }
        if (!exp_digits) return false;
        double m = 1.0;
        for (int k = 0; k < e && m < 1e300; k++) m *= 10.0;
        v = eneg ? v / m : v * m;
    }

    if (i != n) return false;
    *out = neg ? -v : v;
    return true;
}

static int dbl_to_int(double d)
{
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483647 - 1;
    return (int)d;
}

/* ---------- 捕获写入口 ---------- */

/* 字符串扫描时的一个解码后字节：按当前模式落键缓冲或标量缓冲 */
static void scan_put(bambu_status_stream_t *p, uint8_t c)
{
    if (p->mode == M_KEY) {
        if (p->key_len < BAMBUSTREAM_KEY_MAX) p->key_buf[p->key_len++] = (char)c;
        else p->key_overflow = 1;
        return;
    }
    if (!p->cap_field) return;   /* 跳过模式：只扫描不存储 */
    if (p->cap_len < p->cap_max) p->cap_buf[p->cap_len++] = (char)c;
    else p->cap_overflow = 1;    /* 超长字符串：截断但结构照常 */
}

static void scan_put_cp(bambu_status_stream_t *p, uint32_t cp)
{
    char b[4];
    size_t n = utf8_encode(cp, b);
    for (size_t i = 0; i < n; i++) scan_put(p, (uint8_t)b[i]);
}

/* 字符串内一个输入字节（不含收尾 '"'），false = 畸形 */
static bool str_char(bambu_status_stream_t *p, uint8_t c)
{
    switch (p->esc) {
    case 0:
        if (c == '\\') { p->esc = 1; return true; }
        if (c < 0x20) return false;          /* 未转义控制字符 */
        scan_put(p, c);                      /* ASCII / UTF-8 原样透传 */
        return true;
    case 1: {
        p->esc = 0;
        switch (c) {
        case '"': scan_put(p, '"'); return true;
        case '\\': scan_put(p, '\\'); return true;
        case '/': scan_put(p, '/'); return true;
        case 'b': scan_put(p, '\b'); return true;
        case 'f': scan_put(p, '\f'); return true;
        case 'n': scan_put(p, '\n'); return true;
        case 'r': scan_put(p, '\r'); return true;
        case 't': scan_put(p, '\t'); return true;
        case 'u': p->esc = 2; p->hex_pos = 0; p->code = 0; return true;
        default: return false;
        }
    }
    case 2: case 5: {                        /* 收 4 位 hex（2=普通，5=低代理） */
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        p->code = p->code * 16 + d;
        if (++p->hex_pos < 4) return true;
        uint32_t cp = p->code;
        if (p->esc == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF) { p->esc = 3; return true; }
            if (cp >= 0xDC00 && cp <= 0xDFFF) return false;   /* 裸低代理 */
            p->esc = 0;
            scan_put_cp(p, cp);
            return true;
        }
        /* 低代理收尾：code 存的是低代理，需要与高代理组合 */
        if (cp < 0xDC00 || cp > 0xDFFF) return false;
        p->esc = 0;
        scan_put_cp(p, 0x10000u + ((p->hex_hi - 0xD800u) << 10) + (cp - 0xDC00u));
        return true;
    }
    case 3:                                  /* 高代理后必须是 '\' */
        if (c != '\\') return false;
        p->hex_hi = p->code;
        p->esc = 4;
        return true;
    case 4:                                  /* 然后必须是 'u' */
        if (c != 'u') return false;
        p->esc = 5;
        p->hex_pos = 0;
        p->code = 0;
        return true;
    default:
        return false;
    }
}

/* ---------- 结构推进 ---------- */

/* 一个值（标量或容器）结束后推进父容器期待状态 */
static void after_value(bambu_status_stream_t *p)
{
    if (p->depth == 0) {          /* 根对象闭合 */
        p->root_closed = 1;
        p->mode = M_DONE;
        return;
    }
    bambustream_level_t *t = &p->lv[p->depth - 1];
    if (t->state == L_OBJ_VALUE) t->state = L_OBJ_COMMA;
    else if (t->state == L_ARR_VALUE || t->state == L_ARR_VALUE2)
        t->state = L_ARR_COMMA;
}

static void push_level(bambu_status_stream_t *p, bool obj)
{
    if (p->depth >= BAMBUSTREAM_MAX_DEPTH) { p->err = 1; return; }
    bambustream_level_t *t = &p->lv[p->depth];
    t->state = obj ? L_OBJ_KEY : L_ARR_VALUE;
    /* 只有根对象的 "print" 成员对象才是捕获窗口（first-wins 已在键处理保证） */
    t->is_print = (obj && p->depth == 1 && p->pending_print) ? 1 : 0;
    if (p->pending_print) {
        p->print_seen = t->is_print ? 1 : 2;
        p->pending_print = 0;
    }
    p->depth++;
}

/* print 直接子字段的值开始：决定是否开窗捕获 */
static void begin_scalar_capture(bambu_status_stream_t *p, bool is_string)
{
    p->cap_field = 0;
    p->cap_len = 0;
    p->cap_overflow = 0;
    if (p->depth != 2 || !p->lv[1].is_print || p->cur_key == F_NONE) return;
    if (p->present & fbit(p->cur_key)) return;   /* first-wins：重复键只取第一次 */

    bool want_string = (p->cur_key == F_GCODE_STATE ||
                        p->cur_key == F_SUBTASK || p->cur_key == F_GCODE_FILE);
    if (want_string != is_string) return;        /* 类型不符：忽略该字段（对齐 cJSON） */

    p->cap_field = p->cur_key;
    if (is_string)
        p->cap_max = p->cur_key == F_GCODE_STATE ? BAMBUSTREAM_STATE_MAX
                                                 : BAMBUSTREAM_TASK_MAX;
    else
        p->cap_max = BAMBUSTREAM_NUM_MAX;
}

/* 数字 token 结束：校验语法，合法且开窗则入 staging */
static void end_number(bambu_status_stream_t *p)
{
    if (p->cap_overflow) { p->err = 1; return; }   /* 数字原文超 31 字符，判畸形 */
    p->cap_buf[p->cap_len] = 0;
    double d;
    if (!num_to_double(p->cap_buf, p->cap_len, &d)) { p->err = 1; return; }
    switch (p->cap_field) {
    case F_NOZZLE:      p->nozzle = (float)d; break;
    case F_NOZZLE_T:    p->nozzle_t = (float)d; break;
    case F_BED:         p->bed = (float)d; break;
    case F_BED_T:       p->bed_t = (float)d; break;
    case F_PERCENT:     p->percent = dbl_to_int(d); break;
    case F_REMAIN:      p->remain = dbl_to_int(d); break;
    case F_LAYER:       p->layer = dbl_to_int(d); break;
    case F_LAYER_TOTAL: p->layer_total = dbl_to_int(d); break;
    default: return;                                /* 跳过模式：只校验不存储 */
    }
    p->present |= fbit(p->cap_field);
}

/* 字符串值结束：开窗字段入 staging */
static void end_string_value(bambu_status_stream_t *p)
{
    p->cap_buf[p->cap_len] = 0;
    switch (p->cap_field) {
    case F_GCODE_STATE:
        p->st = parse_state(p->cap_buf);
        p->present |= fbit(F_GCODE_STATE);   /* 未知值也算 changed（对齐 cJSON） */
        break;
    case F_SUBTASK:
        memcpy(p->task_buf, p->cap_buf, (size_t)p->cap_len + 1);
        p->task_src = 2;
        p->present |= fbit(F_SUBTASK);
        break;
    case F_GCODE_FILE:
        if (p->task_src < 2) {               /* subtask_name 优先 */
            memcpy(p->task_buf, p->cap_buf, (size_t)p->cap_len + 1);
            p->task_src = 1;
        }
        p->present |= fbit(F_GCODE_FILE);
        break;
    default:
        break;
    }
}

/* 键字符串结束：确定当前成员键 */
static void end_key(bambu_status_stream_t *p)
{
    p->cur_key = F_NONE;
    p->key_buf[p->key_len] = 0;
    if (p->key_overflow) return;
    if (p->depth == 1) {
        /* 根层只关心第一个 "print"（cJSON first-wins） */
        if (p->print_seen == 0 && !p->pending_print &&
            strcmp(p->key_buf, "print") == 0)
            p->pending_print = 1;
    } else if (p->depth == 2 && p->lv[1].is_print) {
        p->cur_key = match_field(p->key_buf);
    }
    p->lv[p->depth - 1].state = L_OBJ_COLON;
}

/* ---------- 逐字节喂入 ---------- */

static void feed_char(bambu_status_stream_t *p, uint8_t c)
{
    switch (p->mode) {
    case M_ROOT:
        if (is_ws(c)) return;
        if (c != '{') { p->err = 1; return; }
        p->lv[0].state = L_OBJ_KEY;
        p->lv[0].is_print = 0;
        p->depth = 1;
        p->mode = M_STRUCT;
        return;

    case M_DONE:
        if (!is_ws(c)) p->err = 1;
        return;

    case M_KEY:
    case M_STRING:
        if (p->esc == 0 && c == '"') {
            if (p->mode == M_KEY) end_key(p);
            else { end_string_value(p); after_value(p); }
            p->mode = M_STRUCT;
            return;
        }
        if (!str_char(p, c)) p->err = 1;
        return;

    case M_NUMBER:
        if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' ||
            (c >= '0' && c <= '9')) {
            /* 数字始终捕获原文（cap_max=BAMBUSTREAM_NUM_MAX），结束时要校验语法 */
            if (p->cap_len < BAMBUSTREAM_NUM_MAX) p->cap_buf[p->cap_len++] = (char)c;
            else p->cap_overflow = 1;
            return;
        }
        end_number(p);
        if (p->err) return;
        after_value(p);
        p->mode = M_STRUCT;
        feed_char(p, c);                     /* 终止符按结构字符重处理 */
        return;

    case M_LIT: {
        static const char *const lits[] = { "", "true", "false", "null" };
        const char *lit = lits[p->lit_kind];
        if (c != (uint8_t)lit[p->lit_pos]) { p->err = 1; return; }
        if (lit[++p->lit_pos] == 0) { after_value(p); p->mode = M_STRUCT; }
        return;
    }

    case M_STRUCT:
    default:
        break;
    }

    /* M_STRUCT */
    if (is_ws(c)) return;
    bambustream_level_t *t = &p->lv[p->depth - 1];
    switch (t->state) {
    case L_OBJ_KEY:
        if (c == '"') {
            p->key_len = 0;
            p->key_overflow = 0;
            p->esc = 0;
            p->mode = M_KEY;
        } else if (c == '}') {
            p->depth--;
            after_value(p);
        } else {
            p->err = 1;
        }
        return;
    case L_OBJ_KEY2:                     /* 逗号之后只许键，尾逗号判畸形 */
        if (c == '"') {
            p->key_len = 0;
            p->key_overflow = 0;
            p->esc = 0;
            p->mode = M_KEY;
        } else {
            p->err = 1;
        }
        return;
    case L_OBJ_COLON:
        if (c == ':') t->state = L_OBJ_VALUE;
        else p->err = 1;
        return;
    case L_OBJ_COMMA:
        if (c == ',') t->state = L_OBJ_KEY2;
        else if (c == '}') { p->depth--; after_value(p); }
        else p->err = 1;
        return;
    case L_ARR_COMMA:
        if (c == ',') t->state = L_ARR_VALUE2;
        else if (c == ']') { p->depth--; after_value(p); }
        else p->err = 1;
        return;
    case L_OBJ_VALUE:
    case L_ARR_VALUE:
    case L_ARR_VALUE2:
        break;
    }

    /* 值派发（L_OBJ_VALUE / L_ARR_VALUE / L_ARR_VALUE2） */
    bool is_obj_value = t->state == L_OBJ_VALUE;
    bool allow_arr_close = t->state == L_ARR_VALUE;   /* 仅 '[' 后第一个元素前 */
    if (c == '{') {
        push_level(p, true);
    } else if (c == '[') {
        push_level(p, false);
    } else if (c == '"') {
        if (is_obj_value) begin_scalar_capture(p, true);
        if (p->pending_print) { p->print_seen = 2; p->pending_print = 0; }
        p->esc = 0;
        p->mode = M_STRING;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        if (is_obj_value) begin_scalar_capture(p, false);
        if (p->pending_print) { p->print_seen = 2; p->pending_print = 0; }
        /* 数字原文捕获从首字符开始（跳过模式也捕获，用于语法校验） */
        p->cap_len = 0;
        p->cap_overflow = 0;
        p->cap_buf[p->cap_len++] = (char)c;
        p->mode = M_NUMBER;
    } else if (c == 't' || c == 'f' || c == 'n') {
        if (p->pending_print) { p->print_seen = 2; p->pending_print = 0; }
        p->lit_kind = c == 't' ? 1 : c == 'f' ? 2 : 3;
        p->lit_pos = 1;
        p->mode = M_LIT;
    } else if (c == ']' && allow_arr_close) {
        if (p->pending_print) { p->print_seen = 2; p->pending_print = 0; }
        p->depth--;                          /* 空数组 */
        after_value(p);
    } else {
        p->err = 1;
    }
}

/* ---------- 对外 ---------- */

void bambu_status_stream_begin(bambu_status_stream_t *p, size_t total_len)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->total = total_len;
    p->mode = M_ROOT;
    if (total_len == 0 || total_len > BAMBUSTREAM_MAX_TOTAL) p->err = 1;
}

bool bambu_status_stream_feed(bambu_status_stream_t *p,
                              const char *chunk, size_t len)
{
    if (!p || (!chunk && len)) return false;
    if (p->err) return false;
    if (len > p->total - p->fed) { p->err = 1; return false; }
    for (size_t i = 0; i < len && !p->err; i++)
        feed_char(p, (uint8_t)chunk[i]);
    p->fed += len;
    return !p->err;
}

bool bambu_status_stream_finish(bambu_status_stream_t *p,
                                bambu_status_t *in_out)
{
    if (!p || !in_out) return false;
    if (p->err || p->mode != M_DONE || p->fed != p->total) return false;
    if (p->print_seen != 1) return false;    /* print 缺失或非对象（对齐 cJSON） */
    if (!p->present) return false;           /* 没有任何合法目标字段 */

    if (p->present & fbit(F_GCODE_STATE)) {
        if (p->st != BAMBU_PRINT_UNKNOWN) in_out->state = p->st;
    }
    if (p->present & fbit(F_NOZZLE)) in_out->nozzle_temp = p->nozzle;
    if (p->present & fbit(F_NOZZLE_T)) in_out->nozzle_target = p->nozzle_t;
    if (p->present & fbit(F_BED)) in_out->bed_temp = p->bed;
    if (p->present & fbit(F_BED_T)) in_out->bed_target = p->bed_t;
    if (p->present & fbit(F_PERCENT)) in_out->progress_percent = p->percent;
    if (p->present & fbit(F_REMAIN)) in_out->remaining_minutes = p->remain;
    if (p->present & fbit(F_LAYER)) in_out->layer_current = p->layer;
    if (p->present & fbit(F_LAYER_TOTAL)) in_out->layer_total = p->layer_total;
    if (p->task_src) memcpy(in_out->task_name, p->task_buf, sizeof(p->task_buf));

    if (in_out->progress_percent < 0) in_out->progress_percent = 0;
    if (in_out->progress_percent > 100) in_out->progress_percent = 100;
    in_out->has_data = true;
    return true;
}
