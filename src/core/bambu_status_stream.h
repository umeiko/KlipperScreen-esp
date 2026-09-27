#pragma once
/*
 * Bambu report 零分配流式解析器（ESP-MQTT 分片直喂用）。
 *
 * 语义对齐 bambu_status.c 的 cJSON 版 merge 规则（缺字段保持旧值、
 * 只认根对象 "print" 成员的直接子字段、subtask_name 优先于 gcode_file、
 * 非法 gcode_state 不覆盖旧状态但算 changed、progress 钳到 0..100），但：
 *  - parser context 定长，全程不 malloc/calloc/realloc/free，不依赖 cJSON；
 *  - 支持任意字节位置分片喂入（MQTT total/offset 分片直接 feed）；
 *  - 合法且完整的消息在 finish 时一次性原子 patch；畸形、截断、超限
 *    或 print 非对象的消息不改变旧状态（finish 返回 false）；
 *  - 总长度上限 BAMBUSTREAM_MAX_TOTAL（begin 即拒绝），嵌套上限
 *    BAMBUSTREAM_MAX_DEPTH（超过判畸形）；
 *  - 重复键按 cJSON first-wins：第一个 "print" 决定结果，print 内同名字段
 *    只取第一次出现。
 *
 * 已知的刻意严格（Bambu 不会发送，cJSON 却会放行的边角）：
 *  数字必须合乎 -?(0|[0-9]+)(\.[0-9]+)?([eE][+-]?[0-9]+)?（"1." / "+1" 判畸形），
 *  字符串内不允许未转义控制字符，不支持 BOM，lone surrogate 判畸形。
 */
#include "bambu_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAMBUSTREAM_MAX_TOTAL   49152u   /* 单条 report 上限（与 MQTT 层一致） */
#define BAMBUSTREAM_MAX_DEPTH   32u      /* JSON 嵌套上限，超过判畸形 */
#define BAMBUSTREAM_KEY_MAX     23u      /* 最长目标键 "nozzle_target_temper"=20，留余量 */
#define BAMBUSTREAM_NUM_MAX     31u      /* JSON 数字原文上限，超过判畸形 */
#define BAMBUSTREAM_STATE_MAX   15u      /* gcode_state 捕获上限（已知最长 "RUNNING"=7） */
#define BAMBUSTREAM_TASK_MAX    95u      /* = sizeof(bambu_status_t.task_name) - 1 */

typedef struct {
    uint8_t state;      /* 容器内期待什么（L_*） */
    uint8_t is_print;   /* 该层是根对象的 "print" 成员对象 */
} bambustream_level_t;

typedef struct {
    /* 输入进度与总控 */
    size_t total;               /* begin 宣告的总字节数 */
    size_t fed;                 /* 已喂字节数 */
    uint8_t mode;               /* 扫描模式（M_*） */
    uint8_t err;                /* 置位后 feed/finish 恒失败 */
    uint8_t root_closed;        /* 根对象已闭合（收尾只允许空白） */
    uint8_t depth;              /* 容器栈深度（根对象 = 1） */
    bambustream_level_t lv[BAMBUSTREAM_MAX_DEPTH];

    /* "print" 成员追踪（cJSON first-wins）：0 未见 / 1 是对象 / 2 非对象 */
    uint8_t print_seen;
    uint8_t pending_print;      /* 根层刚读完 "print" 键，等其值开始 */

    /* 键捕获（解码转义后比对） */
    uint8_t key_len;
    uint8_t key_overflow;
    char key_buf[BAMBUSTREAM_KEY_MAX + 1];

    /* 字符串扫描子状态：0 常规 / 1 反斜杠后 / 2 收 \u hex /
       3 高代理后等 '\' / 4 等 'u' / 5 收低 \u hex */
    uint8_t esc;
    uint8_t hex_pos;
    uint32_t code;              /* 当前 \u 累积码点 */
    uint32_t hex_hi;            /* 代理对中暂存的高代理值 */

    /* true/false/null 字面量扫描 */
    uint8_t lit_kind;           /* 1 true / 2 false / 3 null */
    uint8_t lit_pos;

    /* 当前标量捕获：cap_field 非 0 时才落 cap_buf（数字始终捕获用于语法校验） */
    uint8_t cur_key;            /* print 内当前成员键（F_*），值开始时消费 */
    uint8_t cap_field;          /* 当前在捕获的字段（F_*），0 = 跳过 */
    uint8_t cap_len;
    uint8_t cap_max;
    uint8_t cap_overflow;
    char cap_buf[BAMBUSTREAM_TASK_MAX + 1];

    /* 已确认字段的 staging（finish 时才原子合并到调用方状态） */
    uint16_t present;           /* 每个 F_* 一个 bit */
    float nozzle, nozzle_t, bed, bed_t;
    int percent, remain, layer, layer_total;
    bambu_print_state_t st;
    uint8_t task_src;           /* 0 无 / 1 gcode_file / 2 subtask_name（高优先） */
    char task_buf[BAMBUSTREAM_TASK_MAX + 1];
} bambu_status_stream_t;

/* 开始一条新消息。total_len 为 MQTT total_data_len；为 0 或超过
 * BAMBUSTREAM_MAX_TOTAL 时进入错误态（后续 feed/finish 恒 false）。 */
void bambu_status_stream_begin(bambu_status_stream_t *p, size_t total_len);

/* 喂一个分片（任意字节边界）。返回 false 表示消息已判畸形/超限，
 * 之后应丢弃整条，不得再 feed/finish。 */
bool bambu_status_stream_feed(bambu_status_stream_t *p,
                              const char *chunk, size_t len);

/* 收尾：结构完整、喂够 total、print 是对象且至少一个目标字段合法时，
 * 把 staging 原子合并进 *in_out（缺字段保持旧值）并返回 true；
 * 否则 *in_out 原样不动，返回 false。 */
bool bambu_status_stream_finish(bambu_status_stream_t *p,
                                bambu_status_t *in_out);

#ifdef __cplusplus
}
#endif
