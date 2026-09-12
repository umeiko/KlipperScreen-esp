/*
 * bambu_status_stream host 端单元测试（独立构建运行，见 tools/test-stream-parser.sh）。
 * 链接时 --wrap malloc/calloc/realloc/free：断言解析器全程零堆调用。
 * 覆盖：全字段、单字段增量保留、逐字节边界切包、同名诱饵、转义任务名、
 * 截断/畸形包、40KiB 未知 AMS 数组、超长 task_name 截断、first-wins、类型不符。
 */
#include "bambu_status_stream.h"

#include <stdio.h>
#include <string.h>

/* ---------- 堆计数（链接器 --wrap） ---------- */
static int heap_calls;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) { heap_calls++; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { heap_calls++; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { heap_calls++; return __real_realloc(p, n); }
void __wrap_free(void *p) { heap_calls++; __real_free(p); }

/* ---------- 断言框架 ---------- */
static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static int g_test;
#define TEST(name) do { g_test++; printf("test %d: %s\n", g_test, name); } while (0)

static void status_reset(bambu_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->state = BAMBU_PRINT_UNKNOWN;
    s->remaining_minutes = -1;
}

static bool feq(float a, float b)
{
    float d = a - b;
    return d > -0.001f && d < 0.001f;
}

/* 一次性喂完整消息 */
static bool run_msg(const char *msg, size_t len, bambu_status_t *st)
{
    bambu_status_stream_t p;
    bambu_status_stream_begin(&p, len);
    if (!bambu_status_stream_feed(&p, msg, len)) return false;
    return bambu_status_stream_finish(&p, st);
}

/* ---------- 测试消息 ---------- */

static const char FULL_MSG[] =
    "{\"print\":{\"gcode_state\":\"RUNNING\",\"nozzle_temper\":220.5,"
    "\"nozzle_target_temper\":225,\"bed_temper\":60.25,\"bed_target_temper\":65,"
    "\"mc_percent\":47,\"mc_remaining_time\":83,\"layer_num\":12,"
    "\"total_layer_num\":340,\"subtask_name\":\"benchy.3mf\","
    "\"gcode_file\":\"benchy.gcode\"},\"sequence_id\":\"12\"}";

static void expect_full(const bambu_status_t *st)
{
    CHECK(st->has_data);
    CHECK(st->state == BAMBU_PRINT_RUNNING);
    CHECK(feq(st->nozzle_temp, 220.5f));
    CHECK(feq(st->nozzle_target, 225.0f));
    CHECK(feq(st->bed_temp, 60.25f));
    CHECK(feq(st->bed_target, 65.0f));
    CHECK(st->progress_percent == 47);
    CHECK(st->remaining_minutes == 83);
    CHECK(st->layer_current == 12);
    CHECK(st->layer_total == 340);
    CHECK(strcmp(st->task_name, "benchy.3mf") == 0);   /* subtask 优先于 gcode_file */
}

/* 转义任务名：a"b\c<LF>dé😀（é=U+00E9 两位 UTF-8，😀=U+1F600 代理对） */
static const char ESC_MSG[] =
    "{\"print\":{\"subtask_name\":\"a\\\"b\\\\c\\nd\\u00e9\\ud83d\\ude00\"}}";
static const char ESC_EXPECT[] = "a\"b\\c\nd\xC3\xA9\xF0\x9F\x98\x80";

int main(void)
{
    printf("sizeof(bambu_status_stream_t) = %u bytes\n",
           (unsigned)sizeof(bambu_status_stream_t));

    /* 1. 全字段单发 */
    TEST("full fields single-shot");
    {
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(FULL_MSG, sizeof(FULL_MSG) - 1, &st));
        expect_full(&st);
    }

    /* 2. 单字段增量：其余字段保持旧值 */
    TEST("single-field patch keeps old values");
    {
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(FULL_MSG, sizeof(FULL_MSG) - 1, &st));
        static const char patch[] = "{\"print\":{\"bed_temper\":61}}";
        CHECK(run_msg(patch, sizeof(patch) - 1, &st));
        CHECK(st.has_data);
        CHECK(st.state == BAMBU_PRINT_RUNNING);      /* 以下为保持不变的旧值 */
        CHECK(feq(st.nozzle_temp, 220.5f));
        CHECK(feq(st.nozzle_target, 225.0f));
        CHECK(feq(st.bed_target, 65.0f));
        CHECK(st.progress_percent == 47);
        CHECK(st.remaining_minutes == 83);
        CHECK(st.layer_current == 12);
        CHECK(st.layer_total == 340);
        CHECK(strcmp(st.task_name, "benchy.3mf") == 0);
        CHECK(feq(st.bed_temp, 61.0f));              /* 唯一被 patch 的字段 */
    }

    /* 3. 每一个字节边界切包（全字段 + 转义串两条消息） */
    TEST("every byte-boundary split");
    {
        size_t len = sizeof(FULL_MSG) - 1;
        for (size_t i = 0; i <= len; i++) {
            bambu_status_stream_t p;
            bambu_status_t st;
            status_reset(&st);
            bambu_status_stream_begin(&p, len);
            if (!bambu_status_stream_feed(&p, FULL_MSG, i) ||
                !bambu_status_stream_feed(&p, FULL_MSG + i, len - i) ||
                !bambu_status_stream_finish(&p, &st)) {
                CHECK(0 && "split failed");
                printf("  at split %zu\n", i);
                break;
            }
            if (st.progress_percent != 47 || !feq(st.nozzle_temp, 220.5f) ||
                strcmp(st.task_name, "benchy.3mf") != 0 ||
                st.state != BAMBU_PRINT_RUNNING) {
                CHECK(0 && "split result mismatch");
                printf("  at split %zu\n", i);
                break;
            }
        }
        size_t elen = sizeof(ESC_MSG) - 1;
        for (size_t i = 0; i <= elen; i++) {
            bambu_status_stream_t p;
            bambu_status_t st;
            status_reset(&st);
            bambu_status_stream_begin(&p, elen);
            if (!bambu_status_stream_feed(&p, ESC_MSG, i) ||
                !bambu_status_stream_feed(&p, ESC_MSG + i, elen - i) ||
                !bambu_status_stream_finish(&p, &st)) {
                CHECK(0 && "escape split failed");
                printf("  at split %zu\n", i);
                break;
            }
            if (strcmp(st.task_name, ESC_EXPECT) != 0) {
                CHECK(0 && "escape split mismatch");
                printf("  at split %zu\n", i);
                break;
            }
        }
    }

    /* 4. 外层/嵌套同名诱饵：只有 print 直接子字段生效 */
    TEST("decoy same-name fields ignored");
    {
        static const char msg[] =
            "{\"mc_percent\":99,\"nozzle_temper\":999,"
            "\"device\":{\"print\":{\"mc_percent\":88,\"nozzle_temper\":111}},"
            "\"print\":{\"mc_percent\":5,"
            "\"ams\":{\"ams\":[{\"nozzle_temper\":777,\"subtask_name\":\"decoy\"}],"
            "\"layer_num\":42},\"nozzle_temper\":200.5},"
            "\"subtask_name\":\"root_decoy\"}";
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(msg, sizeof(msg) - 1, &st));
        CHECK(st.progress_percent == 5);
        CHECK(feq(st.nozzle_temp, 200.5f));
        CHECK(st.layer_current == 0);
        CHECK(st.task_name[0] == 0);
    }

    /* 5. 转义任务名（引号/反斜杠/换行/\uXXXX/代理对） */
    TEST("escaped task name decoding");
    {
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(ESC_MSG, sizeof(ESC_MSG) - 1, &st));
        CHECK(strcmp(st.task_name, ESC_EXPECT) == 0);
    }

    /* 6. 截断/畸形包：finish false 且旧状态一字节不动 */
    TEST("truncated/malformed packets rejected, state untouched");
    {
        static const char *bad[] = {
            "{\"print\":{\"mc_percent\":47",              /* 截断 */
            "{\"print\":{\"mc_percent\":47,",             /* 截断在键前 */
            "{\"print\":{\"mc_percent\":12abc}}",         /* 数字后垃圾 */
            "{\"print\":{\"gcode_state\":\"RUNNING}",     /* 字符串未闭合 */
            "{\"print\":null}",                           /* print 非对象 */
            "{\"print\":[1,2]}",                          /* print 是数组 */
            "[1,2]",                                      /* 根不是对象 */
            "{\"print\":{\"mc_percent\":47}}garbage",     /* 根后垃圾 */
            "{\"print\":{\"mc_percent\":1.2.3}}",         /* 非法数字 */
            "{\"print\":{\"mc_percent\":47},}",           /* 多余逗号 */
            "{\"print\":{\"mc_percent\":47,}}",           /* print 内尾逗号 */
            "{\"print\":{\"ams\":[1,],\"mc_percent\":7}}", /* 数组尾逗号 */
            "{\"print\":{\"subtask_name\":\"a\\ud83dX\"}}", /* 裸高代理 */
            "{\"print\":{\"subtask_name\":\"a\\x\"}}",    /* 非法转义 */
            "{\"print\":{\"subtask_name\":\"a\nb\"}}",    /* 未转义控制字符 */
            "",                                           /* 空 */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            bambu_status_t st, before;
            status_reset(&st);
            st.progress_percent = 33;
            strcpy(st.task_name, "keep");
            st.nozzle_temp = 199.5f;
            before = st;
            size_t len = strlen(bad[i]);
            CHECK(!run_msg(bad[i], len, &st));
            CHECK(memcmp(&st, &before, sizeof(st)) == 0);
        }
        /* 声明 total 但喂不够：finish false */
        bambu_status_stream_t p;
        bambu_status_t st;
        status_reset(&st);
        bambu_status_stream_begin(&p, sizeof(FULL_MSG) - 1);
        CHECK(bambu_status_stream_feed(&p, FULL_MSG, 20));
        CHECK(!bambu_status_stream_finish(&p, &st));
        /* 喂超过声明 total：feed false */
        bambu_status_stream_begin(&p, 10);
        CHECK(!bambu_status_stream_feed(&p, FULL_MSG, 20));
    }

    /* 7. 40KiB 未知 AMS 数组之后仍有目标字段（奇数块长分片） */
    TEST("40KiB unknown AMS array then target fields");
    {
        static char big[44000];
        size_t n = 0;
        n += (size_t)sprintf(big + n, "{\"print\":{\"ams\":{\"tray\":[");
        /* 填 ~40KiB 含嵌套对象/数组/字符串/诱饵键的垃圾 */
        while (n < 40000) {
            n += (size_t)sprintf(big + n,
                "{\"id\":123,\"name\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\","
                "\"mc_percent\":99,\"nested\":[[1,2,3],{\"k\":\"v\"}]},");
        }
        n += (size_t)sprintf(big + n,
            "{}],\"extra\":[true,false,null,-12.5e2]},"
            "\"nozzle_temper\":201.5,\"layer_num\":7}}");
        CHECK(n < sizeof(big));
        CHECK(n < BAMBUSTREAM_MAX_TOTAL);

        bambu_status_stream_t p;
        bambu_status_t st;
        status_reset(&st);
        bambu_status_stream_begin(&p, n);
        size_t off = 0;
        bool feed_ok = true;
        while (off < n) {                      /* 997 字节奇数块分片 */
            size_t chunk = n - off > 997 ? 997 : n - off;
            if (!bambu_status_stream_feed(&p, big + off, chunk)) {
                feed_ok = false;
                break;
            }
            off += chunk;
        }
        CHECK(feed_ok);
        CHECK(bambu_status_stream_finish(&p, &st));
        CHECK(feq(st.nozzle_temp, 201.5f));
        CHECK(st.layer_current == 7);
    }

    /* 8. 超长 task_name 安全截断到 95 字符 + NUL */
    TEST("overlong task_name truncated safely");
    {
        static char msg[512];
        int n = sprintf(msg, "{\"print\":{\"subtask_name\":\"");
        for (int i = 0; i < 300; i++) msg[n++] = 'x';
        n += sprintf(msg + n, "\"}}");
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(msg, (size_t)n, &st));
        CHECK(strlen(st.task_name) == BAMBUSTREAM_TASK_MAX);
        for (int i = 0; i < (int)BAMBUSTREAM_TASK_MAX; i++)
            CHECK(st.task_name[i] == 'x');
        CHECK(st.task_name[BAMBUSTREAM_TASK_MAX] == 0);
    }

    /* 9. 总长度超限：begin 即拒绝 */
    TEST("total length cap enforced");
    {
        bambu_status_stream_t p;
        bambu_status_t st;
        status_reset(&st);
        bambu_status_stream_begin(&p, BAMBUSTREAM_MAX_TOTAL + 1);
        CHECK(!bambu_status_stream_feed(&p, FULL_MSG, 10));
        CHECK(!bambu_status_stream_finish(&p, &st));
        bambu_status_stream_begin(&p, 0);
        CHECK(!bambu_status_stream_feed(&p, "", 0));
        CHECK(!bambu_status_stream_finish(&p, &st));
    }

    /* 10. 未知 gcode_state：算 changed 但不覆盖旧状态 */
    TEST("unknown gcode_state keeps old state");
    {
        static const char msg[] = "{\"print\":{\"gcode_state\":\"SLICING\"}}";
        bambu_status_t st;
        status_reset(&st);
        st.state = BAMBU_PRINT_RUNNING;
        CHECK(run_msg(msg, sizeof(msg) - 1, &st));
        CHECK(st.state == BAMBU_PRINT_RUNNING);
        CHECK(st.has_data);
        static const char pause[] = "{\"print\":{\"gcode_state\":\"PAUSE\"}}";
        CHECK(run_msg(pause, sizeof(pause) - 1, &st));
        CHECK(st.state == BAMBU_PRINT_PAUSED);
    }

    /* 11. 类型不符的字段被忽略 */
    TEST("type-mismatched fields ignored");
    {
        static const char msg[] =
            "{\"print\":{\"mc_percent\":\"47\",\"bed_temper\":61}}";
        bambu_status_t st;
        status_reset(&st);
        st.progress_percent = 11;
        CHECK(run_msg(msg, sizeof(msg) - 1, &st));
        CHECK(st.progress_percent == 11);
        CHECK(feq(st.bed_temp, 61.0f));
        static const char only_bad[] = "{\"print\":{\"mc_percent\":\"47\"}}";
        CHECK(!run_msg(only_bad, sizeof(only_bad) - 1, &st));   /* 无合法字段 */
    }

    /* 12. 重复键 first-wins（对齐 cJSON） */
    TEST("duplicate keys first-wins");
    {
        static const char msg[] =
            "{\"print\":{\"mc_percent\":1},\"print\":{\"mc_percent\":2}}";
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(msg, sizeof(msg) - 1, &st));
        CHECK(st.progress_percent == 1);
        static const char msg2[] = "{\"print\":{\"mc_percent\":3,\"mc_percent\":4}}";
        CHECK(run_msg(msg2, sizeof(msg2) - 1, &st));
        CHECK(st.progress_percent == 3);
        /* 非法的第一个 print（非对象）也按 first-wins 判负 */
        static const char msg3[] =
            "{\"print\":null,\"print\":{\"mc_percent\":9}}";
        CHECK(!run_msg(msg3, sizeof(msg3) - 1, &st));
    }

    /* 13. 数字形式覆盖：负数/小数/指数/progress 钳位 */
    TEST("number forms and progress clamp");
    {
        static const char msg[] =
            "{\"print\":{\"nozzle_temper\":-3.5e1,\"mc_remaining_time\":1e2,"
            "\"layer_num\":0,\"mc_percent\":140}}";
        bambu_status_t st;
        status_reset(&st);
        CHECK(run_msg(msg, sizeof(msg) - 1, &st));
        CHECK(feq(st.nozzle_temp, -35.0f));
        CHECK(st.remaining_minutes == 100);
        CHECK(st.layer_current == 0);
        CHECK(st.progress_percent == 100);
        static const char neg[] = "{\"print\":{\"mc_percent\":-5}}";
        CHECK(run_msg(neg, sizeof(neg) - 1, &st));
        CHECK(st.progress_percent == 0);
    }

    /* 堆纯净窗口：reset 计数后只做解析器调用（stdio 预热malloc 不进入窗口） */
    TEST("zero heap during parsing");
    {
        heap_calls = 0;
        size_t len = sizeof(FULL_MSG) - 1;
        bool ok = true;
        for (size_t i = 0; i <= len && ok; i++) {
            bambu_status_stream_t p;
            bambu_status_t st;
            status_reset(&st);
            bambu_status_stream_begin(&p, len);
            ok = bambu_status_stream_feed(&p, FULL_MSG, i) &&
                 bambu_status_stream_feed(&p, FULL_MSG + i, len - i) &&
                 bambu_status_stream_finish(&p, &st);
        }
        CHECK(ok);
        CHECK(heap_calls == 0);
    }

    printf("heap calls inside parser-only window: %d (expected 0)\n", heap_calls);
    if (failures) {
        printf("RESULT: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("RESULT: all %d tests passed\n", g_test);
    return 0;
}
