/**
 * @file    diag_stage.c
 * @brief   启动阶段自诊断实现（见 inc/diag_stage.h）。
 */
#include "diag_stage.h"
#include "sdk_port.h"

#include <stdio.h>
#include <string.h>

#define DIAG_FILE   "diagst.bin"
#define DIAG_MAGIC  0xD1u
#define DIAG_ATTEMPT_NONE 0xFFu

typedef struct {
    uint8_t  magic;
    uint8_t  attempted;     /* 正在执行/上次卡住的阶段；0xFF = 无 */
    uint8_t  crashes;       /* 崩溃次数（饱和） */
    uint8_t  reserved;
    uint32_t failed;        /* 崩溃过的阶段位图 */
    uint32_t ok;            /* 成功过的阶段位图 */
} diag_state_t;

static diag_state_t s_st;
static int          s_loaded;

/* 阶段名称表（下标 = 阶段号，1..DIAG_STAGE_MAX） */
static const char *const s_names[DIAG_STAGE_MAX + 1] = {
    "(none)",
    "log(cm_log_printf)",
    "fs write+read",
    "app_ctx_init(config)",
    "usb_rndis_init",
    "traffic_init(counters)",
    "AT+CMGF=0(virt_at)",
    "sms_svc_init(URC+CNMI)",
    "net_mgr_init",
    "web_server_init",
    "task sms_svc",
    "task net_mgr",
    "task web_server",
    "boot_probe(socket 8081)",
    "traffic_loop",
    "(reserved)"
};

static void diag_save(void)
{
    (void)sdk_file_write(DIAG_FILE, &s_st, (uint32_t)sizeof(s_st));
}

static int diag_valid(const diag_state_t *st)
{
    if (st->magic != DIAG_MAGIC) return 0;
    if (st->attempted != DIAG_ATTEMPT_NONE &&
        (st->attempted == 0u || st->attempted > DIAG_STAGE_MAX)) return 0;
    return 1;
}

void diag_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;

    memset(&s_st, 0, sizeof(s_st));
    if (sdk_file_read(DIAG_FILE, &s_st, (uint32_t)sizeof(s_st)) != (int32_t)sizeof(s_st) ||
        !diag_valid(&s_st)) {
        /* 首次运行或文件损坏：只在内存里初始化，不立刻写 Flash。
         * 真正的第一次落盘推迟到 diag_enter()，便于把"Flash 写入本身是否致命"
         * 与其它阶段区分开（第一次写入发生在阶段 1 之前 1.5 秒延时之后）。 */
        memset(&s_st, 0, sizeof(s_st));
        s_st.magic     = DIAG_MAGIC;
        s_st.attempted = DIAG_ATTEMPT_NONE;
        sdk_log_printf("[diag] no state, start fresh\r\n");
        return;
    }

    sdk_log_printf("[diag] loaded: attempted=%u crashes=%u failed=0x%04lX ok=0x%04lX\r\n",
                   (unsigned)s_st.attempted, (unsigned)s_st.crashes,
                   (unsigned long)s_st.failed, (unsigned long)s_st.ok);
}

void diag_note_boot(void)
{
    if (s_st.attempted == DIAG_ATTEMPT_NONE) return;

    /* 上次卡在第 N 步 => 第 N 步把模组搞崩了 */
    if (s_st.attempted <= DIAG_STAGE_MAX) {
        s_st.failed |= (1u << s_st.attempted);
    }
    if (s_st.crashes < 0xFFu) s_st.crashes++;
    sdk_log_printf("[diag] ### previous boot DIED at stage %u (%s) -> mark failed, skip it\r\n",
                   (unsigned)s_st.attempted,
                   diag_stage_name((int)s_st.attempted));
    s_st.attempted = DIAG_ATTEMPT_NONE;
    diag_save();
}

int diag_should_run(int stage)
{
    if (stage < 1 || stage > DIAG_STAGE_MAX) return 0;
    return ((s_st.failed & (1u << stage)) == 0u) ? 1 : 0;
}

void diag_enter(int stage)
{
    if (stage < 1 || stage > DIAG_STAGE_MAX) return;
    s_st.attempted = (uint8_t)stage;
    diag_save();
}

void diag_ok(int stage)
{
    if (stage < 1 || stage > DIAG_STAGE_MAX) return;
    s_st.ok |= (1u << stage);
    s_st.attempted = DIAG_ATTEMPT_NONE;
    diag_save();
}

void diag_finish(void)
{
    s_st.attempted = DIAG_ATTEMPT_NONE;
    diag_save();
}

void diag_clear(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.magic     = DIAG_MAGIC;
    s_st.attempted = DIAG_ATTEMPT_NONE;
    diag_save();
}

int diag_attempted(void)      { return (int)s_st.attempted; }
uint32_t diag_failed_mask(void){ return s_st.failed; }
uint32_t diag_ok_mask(void)   { return s_st.ok; }
int diag_crashes(void)        { return (int)s_st.crashes; }

const char *diag_stage_name(int stage)
{
    if (stage < 0 || stage > DIAG_STAGE_MAX) return "?";
    return s_names[stage];
}

int diag_report(char *buf, uint32_t buf_size)
{
    int   n = 0;
    int   i;

    if (buf == NULL || buf_size == 0u) return 0;
    buf[0] = '\0';

    n += (int)snprintf(&buf[n], (size_t)(buf_size - (uint32_t)n),
                       "crashes: %d\nfailed mask: 0x%04lX\nok mask: 0x%04lX\n",
                       diag_crashes(),
                       (unsigned long)diag_failed_mask(),
                       (unsigned long)diag_ok_mask());

    for (i = 1; i <= DIAG_STAGE_MAX && (uint32_t)n + 8u < buf_size; ++i) {
        if (diag_failed_mask() & (1u << i)) {
            n += (int)snprintf(&buf[n], (size_t)(buf_size - (uint32_t)n),
                               "FAILED stage %d: %s\n", i, diag_stage_name(i));
        }
    }
    for (i = 1; i <= DIAG_STAGE_MAX && (uint32_t)n + 8u < buf_size; ++i) {
        if (diag_ok_mask() & (1u << i)) {
            n += (int)snprintf(&buf[n], (size_t)(buf_size - (uint32_t)n),
                               "ok stage %d: %s\n", i, diag_stage_name(i));
        }
    }
    return n;
}
