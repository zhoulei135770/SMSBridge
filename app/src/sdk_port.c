/**
 * @file    sdk_port.c
 * @brief   SDK 适配层实现。**唯一** include ML307A OpenCPU SDK 头文件的文件。
 *
 * 所有 SDK 用法都有据可查，来源见文件内注释与 docs/sdk-reference/。
 * 本文件不做业务判断，只做：参数校验 -> 调用 SDK -> 归一化返回值/错误码。
 */
#include "sdk_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ---------------- ML307A OpenCPU SDK (cmiot) ---------------- */
#include "cm_os.h"          /* osThreadNew / osMutexNew / osDelay / osTimerNew   */
#include "cm_sys.h"         /* cm_sys_get_imei / cm_sys_get_cm_ver / cm_log_printf */
#include "cm_pm.h"          /* cm_pm_reboot / cm_pm_get_power_on_reason           */
#include "cm_rtc.h"         /* cm_rtc_get_current_time / cm_rtc_get_timezone      */
#include "cm_fs.h"          /* cm_fs_open/read/write/close/move/exist/delete      */
#include "cm_http.h"        /* cm_httpclient_* 同步接口                            */
#include "cm_virt_at.h"     /* cm_virt_at_init/send/get/deinit/urc_reg/sms_send   */
#include "cm_asocket.h"     /* ★ 应用侧 socket 必须走异步接口（eloop 线程内操作）  */
#include "cm_eloop.h"       /* cm_eloop_register_event / post_event               */
#include "cm_modem_info.h"  /* cm_modem_info_radio / cm_modem_info_cell           */
#include "cm_ssl.h"         /* cm_ssl_setopt / CM_SSL_PARAM_CA_CERT / _VERIFY     */
#include "cm_uart.h"        /* 备选日志通道（默认不用）                            */
#include "cm_ntp.h"         /* cm_ntp_set_cfg / cm_ntp_sync（依赖 eloop 线程）     */
#include "app_time.h"       /* ISO-8601 / CCLK 解析（纯逻辑，已主机测试）           */
#include "net_addr.h"       /* CGPADDR 双栈解析 / 公网回显提取（纯逻辑）            */

/* ---------------- 平台 lwip ---------------- */
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/*
 * 可用符号边界（链接器实测，完整清单见 docs/sdk-reference/app-link-symbols.txt）：
 *   用户 APP 只链接 prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o + src/cm_lib/*，
 *   共 326 个可调用符号。内核 map 里存在但**不在该边界内**的符号（lwip_nwst_*、
 *   netif_usb_*、lwip_rndis_hotplug_*、lwip_iflist/ifexist、get_ip_flow_stats）
 *   调用会直接 undefined reference —— 已用实际编译验证过。
 *   本文件因此只使用：cm_* OpenCPU API + 标准 lwip socket API + 下列两个
 *   平台导出的流量计数函数。
 */
extern unsigned int get_rx_byte_all(void);   /* 累计接收字节（未文档化，链接可用） */
extern unsigned int get_tx_byte_all(void);   /* 累计发送字节（未文档化，链接可用） */

/* ================================================================== */
/* 常量                                                                */
/* ================================================================== */
#define OS_TICK_MS              5u          /* 由 app.c 的 osDelayMs(ms/5) 反推：1 tick = 5ms */
/*
 * ★ 诊断上报开关（2026-10-15 加，为"拔掉电脑长期运行"场景）。
 *   诊断上报 = 每约 10 秒往 PC（192.168.0.100:9090）POST 一行。
 *   离线（没插电脑）时该请求会走 HTTP 超时，**拖慢短信任务**，因此：
 *     - 长期运行版：默认 0（完全不发上报，零开销）；
 *     - 现场排查时：改成 1 重新编译烧录即可（开启后连续 3 次失败会自动闭嘴）。
 */
#ifndef SDK_DIAG_ENABLE
#define SDK_DIAG_ENABLE 0
#endif

#define AT_RESP_MAX             512u   /* 单条短信 CMGR 的 PDU 可到 ~180 字节，192 太小 */
#define AT_POLL_INTERVAL_MS     20u
#define CFG_FILE_MAIN           "cfg.bin"
#define CFG_FILE_TMP            "cfgtmp.bin"
#define CFG_FILE_BAK            "cfgbak.bin"
#define RTC_VALID_EPOCH         1600000000LL   /* 2020-09-13，早于此认为时间未同步 */

/* ================================================================== */
/* 内部状态                                                            */
/* ================================================================== */
static sdk_mutex_t      s_at_mutex;
static int              s_at_ready;
static int              s_sms_inited;
static sdk_urc_cb_t     s_urc_cb;      /* 上层注册的 URC 回调 */
static sdk_urc_cb_t     s_urc_shadow;  /* URC 回调影子变量（多线程安全） */

/* ================================================================== */
/* 日志                                                                */
/* ================================================================== */
/*
 * 日志走 SDK 自带输出 cm_log_printf()（cm_sys.h），不需要应用自己去配 UART：
 * DEMO 底板上的调试口是 UART0（IOMUX PIN_17/PIN_18，FUNCTION1），SDK 示例
 * 在 cm_opencpu_entry() 开头用 uart_open(CM_UART_DEV_0, 921600, u0_callback)
 * 打开它，日志同时会出现在 USB CDC 调试串口上。
 *
 * 如果某个底包上 cm_log_printf() 不输出，有两条已验证的替代路径：
 *   (a) 复用 SDK 自带的串口驱动：在 main.c 里 #include "drv_uart.h"，
 *       调用 uart_open(CM_UART_DEV_0, 921600, u0_callback)，再用 u0_printf()；
 *       （drv_uart.c 是 SDK 工程自带文件，其 baudrate 参数是 uint32_t，支持 921600）
 *   (b) 直接用 cm_uart_open()：注意 cm_uart_cfg_t 的 baudrate 是**枚举**，
 *       cm_uart.h 提供的最大值为 256000，**没有 921600**，字段名为
 *       byte_size / parity / stop_bit / flow_ctrl / baudrate / is_lpuart。
 * 因此这里不擅自调用 cm_uart_open()，避免给出无法验证的枚举名。
 */
void sdk_log_puts(const char *s)
{
    if (s == NULL) return;
    cm_log_printf(0, "%s", s);
}

void sdk_log_printf(const char *fmt, ...)
{
    char    buf[192];
    va_list ap;

    if (fmt == NULL) return;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sdk_log_puts(buf);
}

/* ================================================================== */
/* 任务 / 时间 / 同步                                                  */
/* ================================================================== */
uint32_t sdk_uptime_ms(void)
{
    uint32_t freq = osKernelGetTickFreq();
    uint32_t tick = osKernelGetTickCount();
    if (freq == 0u) freq = 1000u / OS_TICK_MS;
    /* 先除后乘避免 32 位溢出（tick / freq * 1000 的精度损失在 1s 内，可接受） */
    return (tick / freq) * 1000u + ((tick % freq) * 1000u) / freq;
}

void sdk_task_sleep_ms(uint32_t ms)
{
    /* SDK 心跳 5ms；osDelay 单位是 tick。向上取整以免变成忙等 */
    uint32_t ticks = (ms + OS_TICK_MS - 1u) / OS_TICK_MS;
    if (ticks == 0u) ticks = 1u;
    (void)osDelay(ticks);
}

static void sdk_beacon_task(void *arg);   /* 前置声明（诊断任务，见下方定义） */

int sdk_task_create(const char *name, sdk_task_fn_t fn, void *arg,
                    uint32_t stack_bytes, int prio)
{
    osThreadAttr_t attr;
    static int     s_diag_task_made;

    if (fn == NULL) return -1;

    /*
     * ★ 栈上限收紧到 3072：官方示例的入口只创建一个 8192 的任务、eloop 线程
     *   仅 1024；而本应用原先要创建 7 个任务（6144×4 + 4096×2 + 3072 ≈ 36KB），
     *   栈由内核堆分配，总量过大时后续 osThreadNew 会失败 —— 表现为
     *   web / 探针任务建不出来（8080/8081 完全无监听）而模组本身稳定不崩。
     */
    if (stack_bytes > 3072u) stack_bytes = 3072u;
    if (stack_bytes < 1024u) stack_bytes = 1024u;

    /*
     * 首次创建任务时顺带创建诊断任务：保证"即使后续任务创建失败"也有诊断上报，
     * 不再出现"零重启、拿不到诊断码"的观测盲区。（幂等，递归前已置标志）
     */
    if (s_diag_task_made == 0) {
        s_diag_task_made = 1;
        (void)sdk_task_create("diag", sdk_beacon_task, NULL, 2048u, 1);
    }

    memset(&attr, 0, sizeof(attr));
    attr.name       = (name != NULL) ? name : "app";
    attr.stack_size = stack_bytes;
    /*
     * SDK 约定（见 custom/applaction/app.c）：用户线程优先级不得高于 osPriorityNormal。
     * prio 参数：0 = Normal，1 = 更高一级（会被 SDK 钳制），保持简单只用到 Normal。
     */
    (void)prio;
    attr.priority   = osPriorityNormal;

    return (osThreadNew((osThreadFunc_t)fn, arg, &attr) != NULL) ? 0 : -1;
}

sdk_mutex_t sdk_mutex_new(void)
{
    return (sdk_mutex_t)osMutexNew(NULL);
}

int sdk_mutex_lock(sdk_mutex_t m, uint32_t timeout_ms)
{
    uint32_t ticks;
    if (m == NULL) return -1;
    if (timeout_ms == 0xFFFFFFFFu) {
        ticks = osWaitForever;
    } else {
        ticks = (timeout_ms + OS_TICK_MS - 1u) / OS_TICK_MS;
        if (ticks == 0u) ticks = 1u;
    }
    return (osMutexAcquire((osMutexId_t)m, ticks) == osOK) ? 0 : -1;
}

int sdk_mutex_unlock(sdk_mutex_t m)
{
    if (m == NULL) return -1;
    return (osMutexRelease((osMutexId_t)m) == osOK) ? 0 : -1;
}

sdk_sem_t sdk_sem_new(uint32_t initial_count)
{
    /* 最大计数固定为 1（二值信号量），够用于"有事件到达"的通知语义 */
    return (sdk_sem_t)osSemaphoreNew(1u, initial_count, NULL);
}

void sdk_sem_post(sdk_sem_t s)
{
    if (s != NULL) (void)osSemaphoreRelease((osSemaphoreId_t)s);
}

int sdk_sem_wait(sdk_sem_t s, uint32_t timeout_ms)
{
    uint32_t ticks;

    if (s == NULL) return -1;
    if (timeout_ms == 0xFFFFFFFFu) {
        ticks = osWaitForever;
    } else {
        ticks = (timeout_ms + OS_TICK_MS - 1u) / OS_TICK_MS;
        if (ticks == 0u) ticks = 1u;
    }
    return (osSemaphoreAcquire((osSemaphoreId_t)s, ticks) == osOK) ? 0 : -1;
}

/* ---- 日历时间 ---- */
int64_t sdk_utc_seconds(void)
{
    return (int64_t)cm_rtc_get_current_time();
}

int sdk_local_time_str(char *buf, size_t buf_size)
{
    uint64_t utc;
    int32_t  tz_h;
    time_t   t;
    struct tm tmv;

    if (buf == NULL || buf_size < 20u) return -1;

    utc = cm_rtc_get_current_time();
    if ((int64_t)utc < RTC_VALID_EPOCH) {
        buf[0] = '\0';
        return -1;
    }
    tz_h = cm_rtc_get_timezone();          /* -12..12（小时） */
    if (tz_h < -12 || tz_h > 12) tz_h = 0;

    t = (time_t)(utc + (uint64_t)(tz_h * 3600));
    if (gmtime_r(&t, &tmv) == NULL) {
        buf[0] = '\0';
        return -1;
    }
    (void)snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return 0;
}

int sdk_month_tag(char *buf, size_t buf_size)
{
    uint64_t utc;
    int32_t  tz_h;
    time_t   t;
    struct tm tmv;

    if (buf == NULL || buf_size < 7u) return -1;
    utc = cm_rtc_get_current_time();
    if ((int64_t)utc < RTC_VALID_EPOCH) {
        buf[0] = '\0';
        return -1;
    }
    tz_h = cm_rtc_get_timezone();
    if (tz_h < -12 || tz_h > 12) tz_h = 0;
    t = (time_t)(utc + (uint64_t)(tz_h * 3600));
    if (gmtime_r(&t, &tmv) == NULL) {
        buf[0] = '\0';
        return -1;
    }
    (void)snprintf(buf, buf_size, "%04d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
    return 0;
}

void sdk_uptime_str(char *buf, size_t buf_size)
{
    uint32_t ms, sec, min, hour, day;

    if (buf == NULL || buf_size < 4u) return;
    ms   = sdk_uptime_ms();
    sec  = ms / 1000u;
    day  = sec / 86400u;
    hour = (sec % 86400u) / 3600u;
    min  = (sec % 3600u) / 60u;
    sec  = sec % 60u;

    if (day > 0u) {
        (void)snprintf(buf, buf_size, "%lu天 %02lu:%02lu:%02lu",
                       (unsigned long)day, (unsigned long)hour,
                       (unsigned long)min, (unsigned long)sec);
    } else {
        (void)snprintf(buf, buf_size, "%02lu:%02lu:%02lu",
                       (unsigned long)hour, (unsigned long)min, (unsigned long)sec);
    }
}

/* ================================================================== */
/* 时钟同步（NTP / 模组时钟）                                          */
/* ================================================================== */
/*
 * ★ 为什么 NTP 要投递到 eloop 线程：
 *   cm_ntp.h 明说 "NTP模块底层依赖asocket、async_dns和eloop模块"，
 *   而 docs/08 已证实应用侧内核 socket 只能在 eloop 线程内操作（否则模组复位）。
 *   这里沿用 sdk_srv_start() 的同一套做法：register_event + post_event。
 */
static volatile int s_ntp_state;          /* 0 进行中/未开始, 1 成功, -1 失败 */
static char         s_ntp_text[48];
static char         s_ntp_server[128];

int sdk_rtc_set_utc(int64_t utc_seconds)
{
    if (utc_seconds <= 0) return -1;
    return (cm_rtc_set_current_time((uint64_t)utc_seconds) == 0) ? 0 : -1;
}

static void sdk_ntp_cb(cm_ntp_event_e event, void *event_param, void *cb_param)
{
    const char *iso = (const char *)event_param;

    (void)cb_param;

    if (event != CM_NTP_EVENT_SYNC_OK && event != CM_NTP_EVENT_SETTIME_FAIL) {
        s_ntp_state = -1;
        return;
    }

    s_ntp_text[0] = '\0';
    if (iso != NULL) {
        size_t n = strlen(iso);
        if (n >= sizeof(s_ntp_text)) n = sizeof(s_ntp_text) - 1u;
        memcpy(s_ntp_text, iso, n);
        s_ntp_text[n] = '\0';
    }

    /* SYNC_OK：SDK 已按默认配置写入 RTC；SETTIME_FAIL：自己再写一次兜底 */
    if (event == CM_NTP_EVENT_SETTIME_FAIL) {
        int64_t utc = 0;
        if (iso == NULL || app_time_parse_iso8601(iso, &utc) != 0 || !app_time_is_valid(utc)) {
            s_ntp_state = -1;
            return;
        }
        if (sdk_rtc_set_utc(utc) != 0) {
            s_ntp_state = -1;
            return;
        }
    }
    s_ntp_state = 1;
}

/** eloop 线程内真正发起 NTP 同步 */
static void sdk_ntp_start_ev(cm_eloop_event_handle_t ev, void *param)
{
    (void)param;
    (void)cm_eloop_unregister_event(ev);

    if (s_ntp_server[0] != '\0') {
        /* 形参是 128 字节字符串指针；把参数带进 eloop 线程再设置 */
        (void)cm_ntp_set_cfg(CM_NTP_CFG_SERVER, s_ntp_server);
    }
    (void)cm_ntp_set_cfg(CM_NTP_CFG_CB, (void *)sdk_ntp_cb);
    if (cm_ntp_sync() < 0) {
        s_ntp_state = -1;
    }
}

int sdk_time_sync_start(const char *server)
{
    cm_eloop_event_handle_t ev;
    cm_eloop_handle_t       eh;

    s_ntp_server[0] = '\0';
    if (server != NULL && server[0] != '\0') {
        size_t n = strlen(server);
        if (n >= sizeof(s_ntp_server)) return -1;
        memcpy(s_ntp_server, server, n + 1u);
    }

    s_ntp_state   = 0;
    s_ntp_text[0] = '\0';

    if (sdk_eloop_start() != 0) {
        s_ntp_state = -1;
        return -1;
    }
    /* 与 sdk_srv_start() 同一约定：pump 的是 asocket 真正使用的那个 eloop */
    eh = cm_asocket_eloop();
    if (eh == NULL) {
        s_ntp_state = -1;
        return -1;
    }

    ev = cm_eloop_register_event(eh, sdk_ntp_start_ev, NULL);
    if (ev == NULL) {
        s_ntp_state = -1;
        return -2;
    }
    if (cm_eloop_post_event(ev) != 0) {
        s_ntp_state = -1;
        return -3;
    }
    return 0;
}

int sdk_time_sync_state(void)
{
    return s_ntp_state;
}

const char *sdk_time_sync_text(void)
{
    return s_ntp_text;
}

int sdk_time_sync_from_modem(void)
{
    static char resp[128];
    int64_t     utc = 0;

    resp[0] = '\0';
    if (sdk_at_cmd("AT+CCLK?", resp, sizeof(resp), 3000u) < 0) return -1;
    if (app_time_parse_cclk(resp, &utc) != 0) return -2;
    if (!app_time_is_valid(utc)) return -3;
    if (sdk_rtc_set_utc(utc) != 0) return -4;
    return 0;
}

/* ================================================================== */
/* 配置持久化（cm_fs）                                                 */
/* ================================================================== */
/** 读整个文件到缓冲，返回字节数；<0 失败。 */
static int32_t fs_read_file(const char *name, void *out, uint32_t out_size)
{
    int32_t fd;
    int32_t n;

    if (name == NULL || out == NULL || out_size == 0u) return -1;
    if (cm_fs_exist(name) != 1) return -2;

    fd = cm_fs_open(name, CM_FS_RB);
    if (fd < 0) return -3;

    n = cm_fs_read(fd, out, out_size);
    (void)cm_fs_close(fd);
    return (n < 0) ? -4 : n;
}

/** 写整个文件（wb 会截断重建），并回读校验。 */
static int32_t fs_write_file_verified(const char *name, const void *in, uint32_t size)
{
    int32_t fd;
    int32_t n;
    uint8_t verify[32];
    int32_t got;

    if (name == NULL || in == NULL || size == 0u) return -1;

    fd = cm_fs_open(name, CM_FS_WB);
    if (fd < 0) return -2;

    n = cm_fs_write(fd, in, size);
    /* 注意：cm_fs_sync 在 SDK 中被 #if 0 禁用，落盘依赖 close() */
    (void)cm_fs_close(fd);
    if (n != (int32_t)size) return -3;

    /* 回读前 32 字节做一致性抽查（CRC 校验由上层 app_config 完成） */
    got = fs_read_file(name, verify, (size < sizeof(verify)) ? size : sizeof(verify));
    if (got <= 0) return -4;
    if (memcmp(verify, in, (size_t)got) != 0) return -5;
    return (int32_t)size;
}

int32_t sdk_cfg_load(void *out, uint32_t out_size)
{
    int32_t n;

    n = fs_read_file(CFG_FILE_MAIN, out, out_size);
    if (n > 0) return n;

    /* 主文件坏了就退回备份 */
    n = fs_read_file(CFG_FILE_BAK, out, out_size);
    return n;
}

/*
 * 通用文件读写（供启动自诊断留下"面包屑"使用）：
 * 诊断需要把"当前执行到哪个初始化阶段"落盘，崩溃重启后还能读出来，
 * 这是现场没有 UART 日志时唯一能跨复位保留的证据。
 */
int32_t sdk_file_read(const char *name, void *out, uint32_t out_size)
{
    return fs_read_file(name, out, out_size);
}

int32_t sdk_file_write(const char *name, const void *in, uint32_t size)
{
    return fs_write_file_verified(name, in, size);
}

int32_t sdk_cfg_save(const void *in, uint32_t size)
{
    int32_t r;

    if (in == NULL || size == 0u) return -1;

    /* 1) 先写临时文件并回读校验，避免把好配置写坏 */
    r = fs_write_file_verified(CFG_FILE_TMP, in, size);
    if (r < 0) return -10 + r;

    /* 2) 现有主文件降级为备份（保留上一份可用配置） */
    if (cm_fs_exist(CFG_FILE_MAIN) == 1) {
        if (cm_fs_exist(CFG_FILE_BAK) == 1) {
            (void)cm_fs_delete(CFG_FILE_BAK);
        }
        (void)cm_fs_move(CFG_FILE_MAIN, CFG_FILE_BAK);
    }

    /* 3) 临时文件转正 */
    r = cm_fs_move(CFG_FILE_TMP, CFG_FILE_MAIN);
    if (r < 0) {
        /* 转正失败：把备份还原回主文件，保证设备仍能用旧配置启动 */
        if (cm_fs_exist(CFG_FILE_BAK) == 1) {
            (void)cm_fs_move(CFG_FILE_BAK, CFG_FILE_MAIN);
        }
        return -20;
    }
    return (int32_t)size;
}

/* ================================================================== */
/* 系统                                                                */
/* ================================================================== */
void sdk_reboot(void)
{
    sdk_log_puts("[sdk] rebooting...\r\n");
    sdk_task_sleep_ms(200);     /* 留时间把日志吐完 */
    cm_pm_reboot();
}

int sdk_power_on_reason(void)
{
    switch (cm_pm_get_power_on_reason()) {
    case CM_PM_POWER_ON:
    case CM_PM_CHARG_POW_OFF:
    case CM_PM_RTC_ALARM:
        return 1;
    case CM_PM_ERROR_RESET:     /* WDT 复位 */
        return 2;
    default:
        return 0;
    }
}

int sdk_get_imei(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 16u) return -1;
    buf[0] = '\0';
    return (cm_sys_get_imei(buf) == 0) ? 0 : -1;
}

int sdk_get_version(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 8u) return -1;
    buf[0] = '\0';
    return (cm_sys_get_cm_ver((char *)buf, (uint32_t)buf_size) == 0) ? 0 : -1;
}

/* ================================================================== */
/* AT 通道                                                             */
/* ================================================================== */
/* ------------------------------------------------------------------ */
/* 虚拟 AT 响应回调                                                     */
/*                                                                     */
/* 严格依据 SDK 官方**完整应用示例** examples/demo_main/src/cm_demo_main.c */
/* 的 iccid_resp_cb() / cm_demo_iccid() 写法：                          */
/*   1) cm_virt_at_init() 必须传回调，回调参数是响应码字符串            */
/*      （"0"=OK "1"=CME ERROR ...）；传 NULL 会因内核回调空指针而硬故障；*/
/*   2) 响应数据在回调内用 cm_virt_at_get() 取出（官方用 256B 缓冲）；   */
/*   3) ★ 回调内【不能】调用 cm_virt_at_deinit()：                       */
/*      官方在回调里只取数据，deinit 由调用线程在等待结束后执行一次      */
/*      （cm_demo_iccid: send -> osDelay -> cm_virt_at_deinit）。        */
/*      在回调（内核上下文）里销毁 virt_at 内部结构会导致内核崩溃，      */
/*      表现为应用开始发 AT 后模组反复重启。                            */
/*   4) URC 回调 cm_virt_at_urc_reg() 全程**只注册一次**；deinit 不会     */
/*      清掉 URC 注册（官方示例 deinit 后从不重新注册即可证明）。        */
/* ------------------------------------------------------------------ */
#define VIRT_AT_RX_MAX 512u

static volatile int s_virtat_active;    /* 1 = 通道已 init 且未 deinit */
static volatile int s_virtat_done;
static volatile int s_virtat_result;
static char         s_virtat_rx[VIRT_AT_RX_MAX];
static int          s_virtat_len;
static int          s_virtat_cb_count;      /* 诊断：响应回调次数 */
static int          s_virtat_drain_hits;    /* 诊断：等待结束后还能读出数据的次数 */

/** 不依赖 stdlib 的十进制解析（响应码只可能是 0~4）。 */
static int virt_at_atoi(const char *s)
{
    int v = 0;

    if (s == NULL) return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (int)(*s - '0');
        ++s;
    }
    return v;
}

static void virt_at_resp_cb(void *param)
{
    int code;
    int n;

    /* 通道已 deinit 时丢弃迟到的回调，避免对已释放的通道调用 get() */
    if (!s_virtat_active) return;

    ++s_virtat_cb_count;
    code = virt_at_atoi((const char *)param);

    /*
     * ★ 累积读取（2026-10-15 上板后改）：
     *   原实现每次回调都把缓冲**覆盖**成最新一段，且任何回调都判定"事务结束"。
     *   但 AT+CMGS 这类交互命令会先回一个中间提示（"> "），最后才是 "+CMGS: n / OK"；
     *   若中间提示就结束事务，就永远看不到真正的发送结果（表现为 resp 为空、rc=-2）。
     *   现在：数据追加到同一缓冲，且**只有 OK/ERROR 才算事务完成**。
     */
    if (s_virtat_len + 1 < (int)sizeof(s_virtat_rx)) {
        n = cm_virt_at_get((unsigned char *)&s_virtat_rx[s_virtat_len],
                           (int)(sizeof(s_virtat_rx) - 1u - (size_t)s_virtat_len));
        if (n > 0) {
            s_virtat_len += n;
            s_virtat_rx[s_virtat_len] = '\0';
        }
    }

    if (code == 0) {
        s_virtat_result = 0;            /* OK */
    } else if (code == 1) {
        s_virtat_result = -2;           /* CME ERROR */
    } else {
        /* 中间态（如 CMGS 的 "> " 提示）：继续等最终结果，由调用方超时兜底 */
        return;
    }

    s_virtat_done = 1;

    /* 注意：这里不调用 cm_virt_at_deinit()，由 sdk_at_cmd() 在调用线程里收尾 */
}

/** URC 回调转发（本体定义见下方"短信"一节）。 */
static void urc_trampoline(char *urc);

int sdk_at_cmd(const char *cmd, char *resp, size_t resp_size, uint32_t timeout_ms)
{
    /* tx 改静态：AT_RESP_MAX 提到 512 后不该再吃 512 字节任务栈（本函数被多任务调用，有互斥保护） */
    static char tx[AT_RESP_MAX];
    uint32_t waited = 0;
    int      result;
    int      n;

    if (cmd == NULL || cmd[0] == '\0') return -3;
    if (resp != NULL && resp_size > 0u) resp[0] = '\0';

    if (s_at_mutex == NULL) s_at_mutex = sdk_mutex_new();
    if (sdk_mutex_lock(s_at_mutex, 30000u) != 0) return -3;

    n = snprintf(tx, sizeof(tx), "%s\r\n", cmd);
    if (n <= 0 || (size_t)n >= sizeof(tx)) {
        (void)sdk_mutex_unlock(s_at_mutex);
        return -3;
    }

    s_virtat_done   = 0;
    s_virtat_result = -3;
    s_virtat_len    = 0;
    s_virtat_cb_count   = 0;
    s_virtat_drain_hits = 0;
    s_virtat_rx[0]  = '\0';
    s_virtat_active = 1;                /* 必须在 init 前置位：响应可能瞬间到达 */

    if (cm_virt_at_init(virt_at_resp_cb) != 0) {
        s_virtat_active = 0;
        (void)sdk_mutex_unlock(s_at_mutex);
        return -3;
    }
    s_at_ready = 1;

    if (cm_virt_at_send((uint8_t *)tx, (int32_t)strlen(tx)) < 0) {
        s_virtat_active = 0;
        cm_virt_at_deinit();
        (void)sdk_mutex_unlock(s_at_mutex);
        return -3;
    }

    timeout_ms = (timeout_ms == 0u) ? 3000u : timeout_ms;

    while (!s_virtat_done && waited < timeout_ms) {
        sdk_task_sleep_ms(AT_POLL_INTERVAL_MS);
        waited += AT_POLL_INTERVAL_MS;
    }

    /*
     * ★ 把队列里剩余的**分块数据**读干净（2026-10-15 上板定位的真 bug）。
     *
     * SDK 的虚拟 AT 按固定长度分块投递应答，长应答（如 AT+CMGR 读一条长短信，
     * 模组侧应答可达 360 字节）只靠响应回调里读一次就会丢尾巴 —— 实测应用只拿到
     * 82 个十六进制字符（UDL 声称 140 字节、实际正文只剩 6 字节），而同一个索引从
     * **外口**读同一份存储却能得到完整 338 字符。可见截断在虚拟 AT 通道这一侧。
     * 这里在等待结束后继续 get()，并容忍短暂的空返回（数据可能稍后才入队）。
     */
    if (resp != NULL && resp_size > 1u) {
        int idle = 0;
        int iter = 0;
        while (iter++ < 80 && idle < 10) {
            if (s_virtat_len + 1 >= (int)sizeof(s_virtat_rx)) break;
            n = cm_virt_at_get((unsigned char *)&s_virtat_rx[s_virtat_len],
                               (int)(sizeof(s_virtat_rx) - 1u - (size_t)s_virtat_len));
            if (n > 0) {
                s_virtat_len += n;
                s_virtat_rx[s_virtat_len] = '\0';
                ++s_virtat_drain_hits;
                idle = 0;
                continue;
            }
            ++idle;
            sdk_task_sleep_ms(50u);         /* 容忍 10×50ms = 500ms 的空档 */
        }
    }

    /* 诊断：短信读取这类长应答，把读取过程的细节上报（现场定位用） */
    /* 只在"异常"（超时/拿不到数据）时上报，正常轮询不再每 10 秒发一条 */
    if (strncmp(cmd, "AT+CMG", 6) == 0 && (result < 0 || s_virtat_len == 0)) {
        sdk_diag_post("atread FAIL %s rc=%d cb=%d drain=%d len=%d",
                      cmd, result, s_virtat_cb_count, s_virtat_drain_hits, s_virtat_len);
    }

    if (s_virtat_done) {
        result = s_virtat_result;
        if (resp != NULL && resp_size > 1u && s_virtat_len > 0) {
            size_t cp = ((size_t)s_virtat_len < (resp_size - 1u))
                            ? (size_t)s_virtat_len : (resp_size - 1u);
            memcpy(resp, s_virtat_rx, cp);
            resp[cp] = '\0';
        }
    } else {
        result = -3;                    /* 超时 */
    }

    /* ★ deinit 在本线程调用且只调用一次（与官方 cm_demo_iccid 一致）；
     *   先清 active 标志，之后迟到的回调会直接返回。 */
    s_virtat_active = 0;
    cm_virt_at_deinit();

    (void)sdk_mutex_unlock(s_at_mutex);
    return result;
}

/* ================================================================== */
/* 短信（cm_virt_at）                                                  */
/* ================================================================== */
static void urc_trampoline(char *urc)
{
    /* 该回调运行在 SDK 的 URC 线程上下文，必须快速返回。
     * 上层 sms_ctrl 内部只做入队，不做解析与网络操作。 */
    if (s_urc_shadow != NULL && urc != NULL) {
        s_urc_shadow(urc);
    }
}

int sdk_sms_channel_init(sdk_urc_cb_t cb)
{
    int rc1, rc2;

    s_urc_cb     = cb;
    s_urc_shadow = cb;

    /* 1) 注册主动上报（+CMT 新短信等） */
    cm_virt_at_urc_reg(urc_trampoline);

    /* 2) AT 通道由 sdk_at_cmd() 内部按官方契约 init/deinit，这里无需预初始化 */
    s_sms_inited = 1;

    /* 3) PDU 模式（中文必须用 PDU） */
    rc1 = sdk_at_cmd("AT+CMGF=0", NULL, 0u, 2000u);
    /*
     * 4) ★ 新短信**直投**：<mt>=2（新短信以 +CMT 形式由 URC 直接推上来，携带 PDU）。
     *
     * 为什么改（2026-10-15 上板实测）：mt=1（存储 + +CMTI）虽然能收到通知，但
     * **读不回来** —— `AT+CMGR` 的应答在 SDK 虚拟 AT 的"响应"通路上只投递
     * **第一个数据突发**：
     *     模组侧应答 363 字节（PDU 338 字符；外口读同一份存储可验证完整），
     *     应用只拿到 96 字节（PDU 82 字符）⇒ 长短信必然残缺；
     *     `AT+CMGL=4`（应答约 800 字节）只拿到 24 字节，回调仅 1 次、补读 0 字节。
     * `+CMT` 走的是 **URC 通路**（cm_virt_at_urc_reg，逐行回调），不受该限制，
     * 整条 PDU 能完整到达 ⇒ 含长 Webhook URL 的配置短信才有救。
     *
     * 代价与兜底：mt=2 按标准不落存储，URC 丢了就没了。因此
     *   ① 仍保留 `AT+CMGL=0` 轮询（若本底包 mt=2 仍落存储即可兜底）；
     *   ② 收到截断 PDU 时明确上报 `urc pdu trunc`，绝不静默失败。
     */
    rc2 = sdk_at_cmd("AT+CNMI=2,2,0,0,0", NULL, 0u, 2000u);

    /* 两条都失败才算失败；单条失败返回正数告警码供上层打日志 */
    if (rc1 < 0 && rc2 < 0) return -2;
    if (rc1 < 0 || rc2 < 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PDU 发送                                                             */
/* ------------------------------------------------------------------ */
/*
 * 2026-10-15 上板把发送路径试了个遍，最终结论（每条都有实测证据）：
 *   ① `AT+CMGS=<len>`（**只 CR**）→ 等 ">" → PDU → Ctrl-Z 是正确形态
 *      （外口实测 `+CMGS: 27 / OK`，手机收到短信；CRLF 会多一个 LF → `+CMS ERROR: 304`）
 *   ② 这条路**在应用里只能靠 SDK 封装 `cm_virt_at_sms_send()`**：
 *      - `cm_virt_at_send()` 受 SDK 流控约束：一段发完没等到**终结响应**（OK/ERROR）
 *        之前不允许再发。CMGS 的 ">" 提示不是终结响应 ⇒ 自实现分段必然卡死
 *        （实测 `txA r=12/-1/-1`：第 1 段 12 字节成功，第 2 段起一律 -1）
 *      - 内核里该封装就是 `AT+CMGS=` + 长度 → ">" → 数据（cp.bin 0x374E00 字符串）
 *   ③ 早期"第 1 条成功、后续失败"的现象，一条是我误加的 `len > 320` 守卫
 *      （第 1 片 PDU 326 字符被自己挡掉），另一条是**条间隔太短**：
 *      模组还在把上一条提交给短信中心，紧接着再发就会被拒（ret=-1）。
 * 因此：只用封装 + 每条独立 init/deinit + 长间隔 + 多次重试。
 */
/**
 * @brief 发送时用的空回调：**故意不调用 cm_virt_at_get()**。
 *
 * 为什么：`cm_virt_at_sms_send()` 是内核自己完成 "AT+CMGS= → 等 > → 发数据" 全流程的，
 * 它需要读走模组的应答；如果我们在 init 时注册一个会 `cm_virt_at_get()` 的回调，
 * 就会**抢走内核要用的数据**。实测现象正是"每次开机只有第一次发送成功、
 * 之后 ret 恒为 -1"（第一次的时序侥幸没抢到）。这里注册空回调，只让内核自己处理。
 */
static volatile int s_tx_cb_hits;

static void sms_send_cb(void *param)
{
    (void)param;
    ++s_tx_cb_hits;         /* 只计数，不取数据 */
}

/**
 * @brief 两条短信之间的最小间隔（毫秒）。
 *
 * 2026-10-15 上板逐步逼近出来的数字：
 *   - 间隔 1 秒  ：第 2 条起全部 `ret=-1`（v6/v10 的 4 条回执只出第 1 条）
 *   - 间隔 8~11 秒：仍然 `ret=-1`（v12 的 3 条回执只出第 1 条）
 *   - 间隔 19 秒 ：**两条都 `ret=0`**（v11：菜单 147 字节 + 状态 127 字节）
 * 说明模组/网络需要一段时间把上一条真正提交给短信中心，期间再发一律被拒。
 * 因此这里做**全局冷却**：任何两次发送之间至少间隔 20 秒（含重试）。
 */
#define SMS_TX_MIN_GAP_MS  20000u

static uint32_t s_last_tx_ms;

int sdk_sms_send_pdu(const char *pdu_hex)
{
    char     cmgs[16];
    int      ri = -99;
    int      ret = -99;
    size_t   len;
    uint32_t now;

    if (pdu_hex == NULL) return -1;
    len = strlen(pdu_hex);
    if (len == 0u || (len % 2u) != 0u) return -1;
    /* 上限放宽：第 1 片 PDU 就有 326 字符（SCA 9 + 头 14 + 140 字节正文）×2，
       早期写成 320 会把最长的那一片直接挡掉，导致"第一条永远失败"。 */
    if (len > 400u) return -1;

    (void)snprintf(cmgs, sizeof(cmgs), "%u", (unsigned)(len / 2u));

    /* 冷却：在**取互斥锁之前**等待，避免把别的任务的 AT 轮询一起堵住 */
    if (s_last_tx_ms != 0u) {
        uint32_t elapsed = sdk_uptime_ms() - s_last_tx_ms;
        if (elapsed < SMS_TX_MIN_GAP_MS) {
            sdk_task_sleep_ms(SMS_TX_MIN_GAP_MS - elapsed);
        }
    }

    if (s_at_mutex == NULL) s_at_mutex = sdk_mutex_new();
    if (sdk_mutex_lock(s_at_mutex, 30000u) != 0) return -3;

    s_tx_cb_hits = 0;
    ri = cm_virt_at_init(sms_send_cb);
    if (ri == 0) {
        ret = cm_virt_at_sms_send(cmgs, (char *)pdu_hex, 0);
        sdk_task_sleep_ms(2000u);       /* 给内核把报文发完并等模组确认 */
        cm_virt_at_deinit();
    }

    (void)sdk_mutex_unlock(s_at_mutex);

    now = sdk_uptime_ms();
    s_last_tx_ms = (now == 0u) ? 1u : now;

    sdk_diag_post("tx init=%d ret=%d len=%u cb=%d", ri, ret, (unsigned)(len / 2u), s_tx_cb_hits);
    return (ret == 0) ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* 未读短信兜底读取（URC 丢失时的补救路径）                             */
/* ------------------------------------------------------------------ */
int sdk_sms_list_unread(char *resp, size_t resp_size)
{
    if (resp == NULL || resp_size < 16u) return -1;
    resp[0] = '\0';
    /* AT+CMGL=0 = 仅列未读（PDU 模式） */
    return sdk_at_cmd("AT+CMGL=0", resp, resp_size, 5000u);
}

int sdk_sms_read_index(int index, char *resp, size_t resp_size)
{
    char cmd[24];

    if (resp == NULL || resp_size < 16u || index < 0) return -1;
    resp[0] = '\0';
    (void)snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    return sdk_at_cmd(cmd, resp, resp_size, 5000u);
}

int sdk_sms_delete_index(int index)
{
    char cmd[24];

    if (index < 0) return -1;
    (void)snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    return sdk_at_cmd(cmd, NULL, 0u, 5000u);
}

int sdk_sms_storage_used(void)
{
    static char resp[AT_RESP_MAX];
    const char *p;
    int         v = 0;

    resp[0] = '\0';
    if (sdk_at_cmd("AT+CPMS?", resp, sizeof(resp), 3000u) < 0) return -1;
    /* 形如: +CPMS: "ME",3,180,"ME",3,180,"ME",3,180 —— 取第一个已用数 */
    p = strstr(resp, "+CPMS:");
    if (p == NULL) return -1;
    p = strchr(p, ',');
    if (p == NULL) return -1;
    ++p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (int)(*p - '0');
        ++p;
    }
    return v;
}

/* ================================================================== */
/* 网络 / 信号 / PDP                                                   */
/* ================================================================== */
int sdk_net_registered(void)
{
    char resp[AT_RESP_MAX];
    const char *p;

    if (sdk_at_cmd("AT+CEREG?", resp, sizeof(resp), 3000u) < 0) {
        /* 退回 2G/3G 注册查询 */
        if (sdk_at_cmd("AT+CREG?", resp, sizeof(resp), 3000u) < 0) return -1;
    }
    p = strstr(resp, ",");
    if (p == NULL) return 0;
    /* +CEREG: <n>,<stat>   stat: 1=已注册本地 5=已注册漫游 */
    ++p;
    while (*p == ' ') ++p;
    if (*p == '1' || *p == '5') return 1;
    return 0;
}

int sdk_net_csq(void)
{
    char resp[AT_RESP_MAX];
    const char *p;
    int v = 0;

    if (sdk_at_cmd("AT+CSQ", resp, sizeof(resp), 3000u) < 0) return -1;
    p = strstr(resp, "+CSQ:");
    if (p == NULL) return -1;
    p += 5;
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
    }
    return (v > 31) ? 99 : v;
}

int sdk_net_local_ip(char *ip, size_t ip_size)
{
    return sdk_net_pdp_addrs(ip, ip_size, NULL, 0u);
}

int sdk_net_pdp_addrs(char *v4, size_t v4_size, char *v6, size_t v6_size)
{
    /*
     * lwip_iflist / lwip_ifexist / lwip_get_default_nw_ifname 都**不在应用链接
     * 边界内**（调用会 undefined reference），因此改用 3GPP 标准 AT 指令
     * AT+CGPADDR 查询 PDP 地址。虚拟 AT 官方声明支持 3GPP 指令。
     *
     * ⚠ 双栈返回形态属**未上板证实**项（docs/11 §3.3）：本仓库只有第三方社区证据
     *   表明 IPV4V6 下会返回两个引号字段：
     *       +CGPADDR: 1,"10.20.30.40","2409:xxxx:xxxx:xxxx::1"
     *   因此解析交给纯逻辑模块 net_addr（四种形态都覆盖，已主机测试），并且
     *   **解析失败时把原始应答打进日志**，方便上板第一时间确认真实形态。
     */
    static char resp[AT_RESP_MAX];
    int         r;

    if (v4 != NULL && v4_size > 0u) v4[0] = '\0';
    if (v6 != NULL && v6_size > 0u) v6[0] = '\0';

    resp[0] = '\0';
    if (sdk_at_cmd("AT+CGPADDR=1", resp, sizeof(resp), 4000u) < 0 && resp[0] == '\0') {
        return -2;
    }

    r = net_addr_parse_cgpaddr(resp, v4, v4_size, v6, v6_size);
    if (r != 0) {
        /* 诊断用：现场没有 USB-TTL 时，这是确认 CGPADDR 实际形态的唯一线索 */
        sdk_log_printf("[net] CGPADDR 解析失败(%d)，原始应答: %.140s\r\n", r, resp);
    }
    return r;
}

int sdk_net_set_pdp(const char *apn, const char *pdp_type)
{
    char cmd[112];
    const char *type;

    if (apn == NULL || apn[0] == '\0') return -1;
    type = (pdp_type != NULL && pdp_type[0] != '\0') ? pdp_type : SDK_PDP_IPV4V6;

    /*
     * 1) 配置 PDP 上下文：AT+CGDCONT=<cid>,"<PDP type>","<APN>"
     *    PDP type 取 IP / IPV6 / IPV4V6（3GPP 标准），双栈才能同时拿到 v4 与 v6。
     */
    (void)snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"", type, apn);
    (void)sdk_at_cmd(cmd, NULL, 0u, 3000u);

    /* 2) 按 cm_virt_at.h 头文件注释：AT+MDIALUPCFG="auto",1 开启拨号上网 */
    (void)sdk_at_cmd("AT+MDIALUPCFG=\"auto\",1", NULL, 0u, 3000u);

    /* 3) 触发一次激活（部分底包需要显式拨号；失败不致命，auto 会兜底） */
    (void)sdk_at_cmd("AT+MIPCALL=1,1", NULL, 0u, 5000u);

    return 0;
}

int sdk_net_deactivate(void)
{
    /* 头文件注释：AT+MDIALUPCFG="auto",0 关闭拨号上网 */
    if (sdk_at_cmd("AT+MDIALUPCFG=\"auto\",0", NULL, 0u, 5000u) < 0) {
        return -1;
    }
    return 0;
}

int sdk_host_resolve(const char *host, char *ip, size_t ip_size)
{
    struct hostent *he;
    int i;

    if (host == NULL || ip == NULL || ip_size < 8u) return -1;

    /* 纯点分十进制直接返回 */
    {
        int dots = 0, digits = 0, ok = 1;
        const char *p = host;
        for (; *p != '\0'; ++p) {
            if (*p == '.') { ++dots; }
            else if (*p >= '0' && *p <= '9') { ++digits; }
            else { ok = 0; break; }
        }
        if (ok && dots == 3 && digits >= 4) {
            (void)snprintf(ip, ip_size, "%s", host);
            return 0;
        }
    }

    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
        return -2;
    }
    /* h_addr_list[0] 是 4 字节网络序地址 */
    for (i = 0; i < 4; ++i) {
        unsigned v = (unsigned)((uint8_t *)he->h_addr_list[0])[i];
        (void)snprintf(&ip[i * 4], ip_size - (size_t)i * 4u, "%u%s",
                       v, (i == 3) ? "" : ".");
    }
    return 0;
}

int sdk_tcp_probe(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char url[160];
    int  rc;

    (void)timeout_ms;   /* 超时由 cm_httpclient 内部处理 */

    if (host == NULL || host[0] == '\0' || port == 0u) return -1;

    /*
     * ★ 关键修正：不能用裸 lwip 从应用任务里做 socket。
     *   内核的异步 socket 模型要求 socket 的创建与收发都发生在 eloop 线程内
     *   （见 SDK 官方示例 examples/asocket 中反复强调的注释），否则会导致模组复位。
     *   因此心跳探测改用**文档化的同步 HTTP 客户端**：只要能拿到 HTTP 状态码
     *   （即使是 404）就说明 TCP 链路可达。
     */
    (void)snprintf(url, sizeof(url), "http://%s:%u/", host, (unsigned)port);
    rc = sdk_http_post_json(url, "text/plain", "", 0u, NULL, 0u);
    return (rc >= 0) ? 0 : -1;
}

/* ================================================================== */
/* 流量统计                                                            */
/* ================================================================== */
int sdk_traffic_get_total_bytes(uint64_t *total_rx, uint64_t *total_tx)
{
    uint32_t rx;
    uint32_t tx;

    /*
     * 这两个函数是 ML307A-DSLN 底包向应用导出的流量计数器（未写入任何 SDK
     * 头文件，但链接可用 —— 见 docs/sdk-reference/app-link-symbols.txt）。
     * 返回 u32 累计字节，模组计数溢出/清零由平台管理。应用侧只做差值统计，
     * 因此**不需要任何"清零"接口**即可实现月度/会话统计与一键重置。
     */
    rx = get_rx_byte_all();
    tx = get_tx_byte_all();

    if (total_rx != NULL) *total_rx = (uint64_t)rx;
    if (total_tx != NULL) *total_tx = (uint64_t)tx;

    /* 两个计数都为 0：可能是真的没流量，也可能是接口在本底包上不生效。
     * 这里返回 0（可用），由 traffic 模块结合"本应用自己发过数据"来判定，
     * 并在日志中提示需实测确认。 */
    return 0;
}

/* ================================================================== */
/* USB 流量共享（RNDIS）                                               */
/* ================================================================== */
int sdk_rndis_apply(int enable)
{
    /*
     * 链接器实测：cm_stub_DSLN.o 的 326 个可调用符号里，usb / rndis / netif /
     * lwip_rndis_hotplug_* / lwip_set_usb_forward_flag **全部不存在**。
     * 内核 map 里虽然能看到 rndis.o、netif_usb_set_status 等符号，但它们属于
     * 内核镜像自身，**没有对用户 APP 导出**，应用调用会 undefined reference。
     *
     * 因此这里如实返回 -1：应用层无法切换 RNDIS。usb_rndis 模块会据此把策略
     * 降级为"保存配置 + 重启后生效"，并在 Web 面板与日志中说明。
     * RNDIS 由底包/NV 决定；候选替代路径见 docs/04-需求实现对照与差异说明.md。
     */
    (void)enable;
    return -1;
}

int sdk_rndis_is_active(void)
{
    /* 无查询接口（netif_usb_get_status 不在应用边界内） */
    return -1;
}

/* ================================================================== */
/* HTTP(S) 客户端                                                      */
/* ================================================================== */
/**
 * @brief 把完整 URL 拆成 "scheme://host[:port]" 与 "/path" 两段。
 * @return 0 成功。
 */
static int split_url(const char *url, char *base, size_t base_size,
                     char *path, size_t path_size, int *is_https)
{
    const char *p;
    const char *host_start;
    const char *path_start;
    size_t      base_len;

    if (url == NULL || base == NULL || path == NULL) return -1;

    if (strncmp(url, "https://", 8u) == 0) {
        *is_https  = 1;
        host_start = url + 8;
    } else if (strncmp(url, "http://", 7u) == 0) {
        *is_https  = 0;
        host_start = url + 7;
    } else {
        return -1;
    }

    path_start = strchr(host_start, '/');
    if (path_start == NULL) {
        base_len = strlen(host_start);
        (void)snprintf(path, path_size, "/");
    } else {
        base_len = (size_t)(path_start - host_start);
        (void)snprintf(path, path_size, "%s", path_start);
    }
    if (base_len == 0u) return -2;

    p = url;
    {
        size_t prefix = (size_t)(host_start - p);
        size_t total  = prefix + base_len;
        if (total + 1u > base_size) return -3;
        memcpy(base, url, total);
        base[total] = '\0';
    }
    return 0;
}

/**
 * @brief 为 HTTPS 请求准备证书策略（企业微信/钉钉的 Webhook 都是 https）。
 *
 * 依据 include/cmiot/cm_ssl.h：
 *   CM_SSL_PARAM_CA_CERT : 值类型 char*，装入 CA 证书字符串（会写入文件系统，
 *                          故需保证文件系统有空间）
 *   CM_SSL_PARAM_VERIFY  : 值类型 uint8_t，0=不做身份验证 1=单向验证 2=双向验证
 *
 * 策略：
 *   1) 优先使用文件系统里的根证书 `ca.pem`，装入后开启单向校验（安全）；
 *   2) 找不到就关闭证书校验并**每次上电告警一次**（可用性优先），
 *      生产环境建议把根证书写入 ca.pem 后重启。
 * 这样做的原因：不开校验 vs 直接不可用 —— 对"只往固定机器人地址推消息"的网关，
 * 可用性优先是更合理的第一步，且把安全级别显式记录在日志和文档里。
 *
 * @note 该函数只在本底包上生效；语义与是否需要先于 set_cfg 调用，需上板确认。
 */
static void https_prepare_verify(int32_t ssl_id)
{
    static int s_state = 0;          /* 0=未尝试 1=已装 CA 2=已关闭校验 */
    static char s_ca_buf[8192];      /* CA PEM 缓冲（静态，避免占栈） */

    if (s_state != 0) return;

    if (cm_fs_exist("ca.pem") == 1) {
        int32_t n = fs_read_file("ca.pem", s_ca_buf, sizeof(s_ca_buf) - 1u);
        if (n > 0) {
            uint8_t verify = 1u;
            s_ca_buf[n] = '\0';
            if (cm_ssl_setopt(ssl_id, CM_SSL_PARAM_CA_CERT, (void *)s_ca_buf) == 0) {
                (void)cm_ssl_setopt(ssl_id, CM_SSL_PARAM_VERIFY, &verify);
                s_state = 1;
                /* 注意：本文件是最底层，不能 include app_log.h（会循环依赖），
                 * 因此这里直接用 sdk_log_puts 输出。 */
                sdk_log_printf("[webhook] 已装入根证书 ca.pem（%d 字节），HTTPS 开启单向校验\r\n",
                               (int)n);
                return;
            }
            sdk_log_puts("[webhook] ca.pem 装入失败，将退回不校验证书\r\n");
        }
    }

    {
        uint8_t verify = 0u;
        (void)cm_ssl_setopt(ssl_id, CM_SSL_PARAM_VERIFY, &verify);
        s_state = 2;
        sdk_log_puts("[webhook] 未找到根证书（文件系统里的 ca.pem），HTTPS 已关闭证书校验。"
                     "生产环境请把根证书写入 ca.pem 后重启以开启校验\r\n");
    }
}

/** 统一的同步 HTTP 请求（POST/GET 共用；定义在本节末尾） */
static int http_sync_request(int method, const char *url, const char *content_type,
                             const char *body, uint32_t body_len, uint8_t dns_priority,
                             char *rsp, size_t rsp_size);

int sdk_http_post_json(const char *url, const char *content_type,
                       const char *body, uint32_t body_len,
                       char *rsp, size_t rsp_size)
{
    return http_sync_request((int)HTTPCLIENT_REQUEST_POST, url, content_type,
                             body, body_len, 0u, rsp, rsp_size);
}

int sdk_http_get_text(const char *url, uint8_t dns_priority, char *rsp, size_t rsp_size)
{
    return http_sync_request((int)HTTPCLIENT_REQUEST_GET, url, NULL, NULL, 0u,
                             dns_priority, rsp, rsp_size);
}

static int http_sync_request(int method, const char *url, const char *content_type,
                             const char *body, uint32_t body_len, uint8_t dns_priority,
                             char *rsp, size_t rsp_size)
{
    char                        base[160];
    char                        path[128];
    char                        hdr[96];
    int                         is_https = 0;
    cm_httpclient_handle_t      handle = NULL;
    cm_httpclient_cfg_t         cfg;
    cm_httpclient_sync_param_t  param;
    cm_httpclient_sync_response_t resp;
    cm_httpclient_ret_code_e    rc;
    int                         code;

    if (url == NULL) return -1;
    if (method == (int)HTTPCLIENT_REQUEST_POST && (body == NULL || body_len == 0u)) return -1;
    if (rsp != NULL && rsp_size > 0u) rsp[0] = '\0';

    if (split_url(url, base, sizeof(base), path, sizeof(path), &is_https) != 0) {
        return -3;
    }

    rc = cm_httpclient_create((const uint8_t *)base, NULL, &handle);
    if (rc != CM_HTTP_RET_CODE_OK || handle == NULL) {
        return -2;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.ssl_enable  = (uint8_t)(is_https ? 1 : 0);
    cfg.ssl_id      = 0;
    cfg.cid         = 0;
    cfg.conn_timeout = 30u;     /* 秒 */
    cfg.rsp_timeout  = 30u;     /* 秒 */
    /*
     * DNS 优先级：★ 不能一律填 0（= 用全局优先级）。cm_async_dns.h 原文：
     *   "@param priority 0：v4优先，1：v6优先，默认v6优先"
     * 也就是说本底包**开机默认 v6 优先**，查"公网 IP"这类回显服务时若域名有 AAAA 记录，
     * 请求会走 IPv6 出口，量回来的是 IPv6 地址而不是公网 IPv4 → 字段语义就错了。
     * 因此由调用方显式指定（1 = v4 优先 / 2 = v6 优先）。
     */
    cfg.dns_priority = (dns_priority <= 2u) ? dns_priority : 0u;
    (void)cm_httpclient_set_cfg(handle, cfg);

    /* HTTPS：先确定证书策略，否则握手会因缺少根证书而失败 */
    if (is_https) {
        https_prepare_verify(cfg.ssl_id);
    }

    if (content_type != NULL) {
        (void)snprintf(hdr, sizeof(hdr), "Content-Type: %s\r\n", content_type);
        (void)cm_httpclient_custom_header_set(handle, (uint8_t *)hdr,
                                             (uint16_t)strlen(hdr));
    }

    memset(&param, 0, sizeof(param));
    param.method         = (cm_httpclient_request_type_e)method;
    param.path           = (const uint8_t *)path;
    param.content        = (body != NULL) ? (uint8_t *)body : NULL;   /* 同步接口期间不会被修改 */
    param.content_length = body_len;

    memset(&resp, 0, sizeof(resp));
    rc = cm_httpclient_sync_request(handle, param, &resp);

    if (rc == CM_HTTP_RET_CODE_OK) {
        code = (int)resp.response_code;
        if (rsp != NULL && rsp_size > 1u && resp.response_content != NULL) {
            uint32_t n = resp.response_content_len;
            if (n > (uint32_t)(rsp_size - 1u)) n = (uint32_t)(rsp_size - 1u);
            memcpy(rsp, resp.response_content, n);
            rsp[n] = '\0';
        }
    } else if (rc == CM_HTTP_RET_CODE_CONNECT_FAIL) {
        code = -4;
    } else {
        code = -4;
    }

    cm_httpclient_sync_free_data(handle);
    (void)cm_httpclient_delete(handle);
    /* 头文件提示：连续两次 delete 之间建议 100ms 延时 */
    sdk_task_sleep_ms(100u);

    return code;
}

/* ================================================================== */
/* TCP 服务端（Web 管理面板）                                          */
/* ================================================================== */
int sdk_tcp_listen(uint16_t port)
{
    int                 fd;
    int                 on = 1;
    struct sockaddr_in  addr;
    int                 nb = 0;

    fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = lwip_htons(port);
    addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);

    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)lwip_close(fd);
        return -2;
    }
    if (lwip_listen(fd, 2) != 0) {
        (void)lwip_close(fd);
        return -3;
    }
    /* 阻塞模式 + select 超时控制 */
    (void)lwip_ioctl(fd, FIONBIO, &nb);
    return fd;
}

int sdk_tcp_accept(int listen_fd, uint32_t timeout_ms)
{
    fd_set          rset;
    struct timeval  tv;
    int             sel;

    if (listen_fd < 0) return -2;

    FD_ZERO(&rset);
    FD_SET(listen_fd, &rset);
    tv.tv_sec  = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

    sel = lwip_select(listen_fd + 1, &rset, NULL, NULL, &tv);
    if (sel == 0)  return -1;      /* 超时，无连接 */
    if (sel < 0)   return -2;

    return lwip_accept(listen_fd, NULL, NULL);
}

int sdk_tcp_recv(int fd, void *buf, size_t len, uint32_t timeout_ms)
{
    fd_set          rset;
    struct timeval  tv;
    int             sel;

    if (fd < 0 || buf == NULL || len == 0u) return -2;

    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    tv.tv_sec  = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

    sel = lwip_select(fd + 1, &rset, NULL, NULL, &tv);
    if (sel == 0) return -1;
    if (sel < 0)  return -2;

    {
        int n = lwip_recv(fd, buf, len, 0);
        if (n == 0) return 0;            /* 对端关闭 */
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) return -1;
            return -2;
        }
        return n;
    }
}

int sdk_tcp_send(int fd, const void *buf, size_t len)
{
    const uint8_t *p   = (const uint8_t *)buf;
    size_t         off = 0;

    if (fd < 0 || buf == NULL) return -1;

    while (off < len) {
        int n = lwip_send(fd, &p[off], len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOBUFS)) {
            sdk_task_sleep_ms(20u);
            continue;
        }
        return (int)off;
    }
    return (int)off;
}

void sdk_tcp_close(int fd)
{
    if (fd >= 0) {
        (void)lwip_close(fd);
    }
}

/* ================================================================== */
/* 异步 socket 服务端（★ 正确做法：cm_asocket + eloop）                 */
/*                                                                     */
/* 依据 SDK 官方示例 examples/asocket/src/cm_demo_asocket.c：           */
/*   - 创建 socket 必须用 cm_asocket_open(..., event_cb, param)；        */
/*   - 所有 socket 操作（bind/listen/accept/recv/send/close）都必须在    */
/*     **eloop 线程**内执行 —— 官方注释原文：                            */
/*     "执行环境为eloop线程(和异步socket在同一个线程,可以安全地处理      */
/*      socket创建和数据收发等操作)"；                                   */
/*   - 主动动作（例如开始监听）通过 cm_eloop_register_event +           */
/*     cm_eloop_post_event 投递到 eloop 线程执行。                      */
/*                                                                     */
/* 之前的实现用裸 lwip_socket/bind/listen/accept 在应用任务里同步操作，  */
/* 属于跨线程使用内核 socket，会导致模组复位（本次故障的根因）。         */
/* ================================================================== */
#define SDK_SRV_MAX     4
#define SDK_USB_GW_ADDR "192.168.0.1"   /* USB/RNDIS 侧模组地址（PC 默认网关） */

typedef struct {
    uint16_t            port;
    sdk_srv_client_cb_t on_client;
    int                 fd;
    int                 family;      /* AF_INET / AF_INET6 */
    int                 used;
} sdk_srv_ctx_t;

/*
 * ★ eloop：asocket 与"投递到 eloop 执行的动作"都依赖它，而内核不会替你跑。
 *   官方示例 examples/asocket 的 __test_eloop_init() 明确要求：
 *     1) cm_eloop_create(N) 创建事件循环；
 *     2) 建一个线程循环调用 cm_eloop_wait_event(eloop, timeout) 来执行事件；
 *     3) 用 cm_asocket_init(eloop) 把该 eloop 交给 asocket 模块使用
 *        （cm_asocket_init 未在头文件声明，但 stub 已导出，是应用侧必需的初始化）。
 */
extern int cm_asocket_init(cm_eloop_handle_t eloop);

/*
 * lwip_socket_init：stub 导出的 socket 层初始化入口（此前从未调用）。
 * 二进制分析显示它是应用侧唯一的 socket 初始化原语之一，可能决定应用
 * socket 挂在哪个 lwIP 实例上（是否就是服务 USB/RNDIS 的那个）。
 */
extern void lwip_socket_init(void);

/*
 * 诊断上报（独立通道）：把关键内部返回值 POST 到 PC 侧监听端口。
 * 之所以独立：被诊断的对象正是"本机监听是否建立"，若用本机 socket 做上报通道
 * 会形成循环依赖。cm_httpclient 是内核侧同步客户端，不依赖应用自己的 eloop。
 * PC 侧只需监听 9090 端口即可收到，例如：
 *     python -m http.server 9090        （或用本工程的 tools/diag_listen.ps1）
 */
#define SDK_DIAG_HOST "192.168.0.100"      /* RNDIS 侧 PC 地址 */
#define SDK_DIAG_PORT 9090

static int s_diag_fail;

void sdk_diag_post(const char *fmt, ...)
{
    char    body[192];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

#if SDK_DIAG_ENABLE
    if (s_diag_fail < 3) {                 /* 连续失败 3 次就闭嘴：离线时不再拖慢短信任务 */
        int rc = sdk_http_post_json("http://" SDK_DIAG_HOST ":9090/diag",
                                    "text/plain", body, (uint32_t)strlen(body), NULL, 0u);
        if (rc >= 200 && rc < 300) { s_diag_fail = 0; } else { ++s_diag_fail; }
    }
#endif
    sdk_log_printf("[diag] %s\r\n", body);
}

static cm_eloop_handle_t s_eloop;
static int               s_eloop_started;

static void sdk_eloop_task(void *arg)
{
    (void)arg;
    for (;;) {
        /*
         * 关键：必须 pump **asocket 真正使用的那个 eloop**。
         * cm_asocket.h 对 cm_asocket_eloop() 的说明是"初始化传入或者系统默认"，
         * 因此优先取它返回的句柄；若为空才退回自己创建的。
         * （此前固定 pump 自己创建的 eloop，若 asocket 实际用另一个，
         *   投递的监听事件就永远不会被执行 —— 表现为"稳定但无监听"。）
         */
        cm_eloop_handle_t ae = cm_asocket_eloop();

        if (ae == NULL) ae = s_eloop;
        if (ae == NULL) {
            sdk_task_sleep_ms(1000u);
            continue;
        }
        (void)cm_eloop_wait_event(ae, 500u);
    }
}

/* 延迟诊断任务：无条件重启测试，验证 cm_pm_reboot() 是否真的复位模组 */
static void sdk_beacon_task(void *arg)
{
    (void)arg;

    sdk_task_sleep_ms(12000u);

    /*
     * ★ 无条件重启测试：
     *   本任务在"首次创建任务"时就被创建（即入口处），因此它一定存在。
     *   若烧录后模组完全不重启 -> 说明 cm_pm_reboot() 在此基线无效，
     *   那么此前基于"复位周期编码诊断码"的整套诊断通道都是无效的。
     *   若模组每 ~15 秒重启一次 -> 复位有效，回到用诊断码解释观测结果。
     */
    /*
     * 重启测试已完成，结论：cm_pm_reboot() 有效（模组确实会复位）。
     * 因此此前"零重启"= 诊断码 0 = 应用侧监听 socket 已成功建立，
     * 而 PC 从 USB RNDIS 侧访问 8080 仍被拒绝
     * => 应用网络栈与 USB RNDIS 栈分离（架构限制）。
     * 这里改回惰性任务，不再重启模组。
     */
    for (;;) {
        sdk_task_sleep_ms(600000u);
    }
}

int sdk_eloop_start(void)
{
    cm_eloop_handle_t ae;

    if (s_eloop_started) return 0;

    /* 1) 优先复用 asocket 已有的（通常是系统默认）eloop */
    ae = cm_asocket_eloop();
    sdk_diag_post("eloop pre : asocket_eloop=%p", (void *)ae);

    if (ae == NULL) {
        /* 2) 没有默认 eloop：自己创建并用 cm_asocket_init 绑定给 asocket 模块 */
        s_eloop = cm_eloop_create(32u);
        sdk_diag_post("eloop created=%p", (void *)s_eloop);
        if (s_eloop == NULL) {
            sdk_log_printf("[eloop] cm_eloop_create failed\r\n");
            return -1;
        }
        (void)lwip_socket_init();      /* ★ 二进制发现的初始化入口：先初始化 lwip socket 层 */
        (void)cm_asocket_init(s_eloop);
        ae = cm_asocket_eloop();
        sdk_diag_post("eloop post : asocket_eloop=%p", (void *)ae);
        if (ae == NULL) ae = s_eloop;
    }

    s_eloop = ae;
    sdk_diag_post("eloop pumping=%p", (void *)s_eloop);
    sdk_log_printf("[eloop] pumping eloop=%p\r\n", (void *)s_eloop);

    if (sdk_task_create("eloop", sdk_eloop_task, NULL, 4096u, 1) != 0) {
        sdk_log_printf("[eloop] create thread failed\r\n");
        return -2;
    }

    /* 诊断任务：12 秒后检查监听是否建立，未建立则用复位时序上报诊断码 */
    (void)sdk_task_create("diag", sdk_beacon_task, NULL, 3072u, 1);

    s_eloop_started = 1;
    sdk_log_printf("[eloop] eloop thread running\r\n");
    return 0;
}

static sdk_srv_ctx_t s_srv[SDK_SRV_MAX];

/** eloop 线程：有新连接 -> accept -> 交给应用回调（仍在 eloop 线程内）-> 关闭 */
static void sdk_srv_on_event(int sock, cm_asocket_event_e event, void *param)
{
    sdk_srv_ctx_t *c = (sdk_srv_ctx_t *)param;

    if (c == NULL) return;

    if (event == CM_ASOCKET_EV_ACCEPT_IND) {
        struct sockaddr_in peer;
        socklen_t          plen = sizeof(peer);
        struct timeval     tv;
        int                cli;

        cli = accept(sock, (struct sockaddr *)&peer, &plen);
        /* ★ 诊断：连接是否真的到达应用（PC 侧 TCP 能连上但收不到数据时，这一行是分水岭） */
        sdk_diag_post("srv%u ACCEPT cli=%d peer=%s:%u", (unsigned)c->port, cli,
                      (cli >= 0) ? inet_ntoa(peer.sin_addr) : "-",
                      (unsigned)((cli >= 0) ? lwip_ntohs(peer.sin_port) : 0));
        if (cli < 0) return;

        /* 给客户端设接收超时，避免慢连接长期占用 eloop 线程 */
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        (void)setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (c->on_client != NULL) {
            c->on_client(cli);
        }
        (void)close(cli);
        sdk_diag_post("srv%u closed cli=%d", (unsigned)c->port, cli);
    } else if (event == CM_ASOCKET_EV_ERROR_IND) {
        sdk_log_printf("[srv] port %u socket error\r\n", (unsigned)c->port);
    }
}

/** eloop 线程：创建监听 socket（cm_asocket_open + bind + listen） */
/* 诊断码：记录监听建立过程中最先失败的环节（用于复位时序上报） */
static volatile int s_diag_code;

static void sdk_srv_start_ev(cm_eloop_event_handle_t ev, void *param)
{
    sdk_srv_ctx_t     *c = (sdk_srv_ctx_t *)param;
    struct sockaddr_in addr;
    int                rb;
    int                rl;

    cm_eloop_unregister_event(ev);
    if (c == NULL) return;

    c->fd = cm_asocket_open((c->family == AF_INET6) ? AF_INET6 : AF_INET,
                            SOCK_STREAM, IPPROTO_TCP, sdk_srv_on_event, c);
    sdk_diag_post("srv%u asocket_open=%d fam=%d", (unsigned)c->port, c->fd, c->family);
    if (c->fd < 0) {
        s_diag_code = 21;                       /* asocket_open 失败 */
        sdk_log_printf("[srv] asocket_open(%u) failed\r\n", (unsigned)c->port);
        return;
    }

    /*
     * ★ IPv6 监听（2026-10-15 追加）：此前监听 socket 只有 AF_INET，
     *   所以"从 IPv6 进面板"在原理上就不可能成功——应用根本没有 v6 监听。
     *   这里绑 `::`（全零，不依赖 lwip 的 in6addr_any 导出符号）。
     */
    if (c->family == AF_INET6) {
        struct sockaddr_in6 a6;
        int                 rb6;
        int                 rl6;

        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port   = lwip_htons(c->port);
        rb6 = bind(c->fd, (struct sockaddr *)&a6, sizeof(a6));
        rl6 = (rb6 == 0) ? listen(c->fd, 4) : -99;
        sdk_diag_post("srv%u[v6] bind=%d listen=%d", (unsigned)c->port, rb6, rl6);
        if (rb6 != 0 || rl6 != 0) {
            s_diag_code = 25;
            (void)cm_asocket_close(c->fd);
            c->fd = -1;
            return;
        }
        sdk_log_printf("[srv] listening v6 on %u (eloop thread)\r\n", (unsigned)c->port);
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = lwip_htons(c->port);
    addr.sin_addr.s_addr = inet_addr(SDK_USB_GW_ADDR);

    /*
     * 决定性实验：先显式绑定 USB/RNDIS 侧网关地址（PC 所在网段）。
     *   - bind 成功 => 应用栈里确实有 USB 接口 => 8080 应当可被 PC 访问；
     *   - bind 失败 => 应用栈与 USB RNDIS 栈分离（返回 EADDRNOTAVAIL），
     *     此时退回 INADDR_ANY 保证不劣于现状，并用诊断码 24 报出结论。
     */
    rb = bind(c->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rb != 0) {
        sdk_log_printf("[srv] bind(" SDK_USB_GW_ADDR ") failed -> fallback ANY\r\n");
        addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
        rb = bind(c->fd, (struct sockaddr *)&addr, sizeof(addr));
        s_diag_code = 24;                       /* USB 地址不可用，已退回 ANY */
    }

    rl = (rb == 0) ? listen(c->fd, 4) : -99;
    sdk_diag_post("srv%u bind=%d listen=%d", (unsigned)c->port, rb, rl);

    if (rb != 0) {
        s_diag_code = 22;                       /* bind 失败 */
    } else if (rl != 0) {
        s_diag_code = 23;                       /* listen 失败 */
    } else if (s_diag_code == 0) {
        s_diag_code = 30;                       /* 监听成功（未发生 USB 地址回退） */
    }

    if (rb != 0 || rl != 0) {
        sdk_log_printf("[srv] bind=%d listen=%d on port %u FAILED\r\n",
                       rb, rl, (unsigned)c->port);
        (void)cm_asocket_close(c->fd);
        c->fd = -1;
        return;
    }
    sdk_log_printf("[srv] listening on %u (eloop thread)\r\n", (unsigned)c->port);
}

/* ------------------------------------------------------------------ */
/* 复位时序上报：不依赖网络与 UART 的诊断通道                            */
/*                                                                     */
/* 现场既没有 USB-TTL，模组又未注册上网（cm_httpclient 走不通）时，       */
/* 唯一可读的信号是"模组复位的时间特征"。做法：                          */
/*   1) 初始化结束时取诊断码 code；                                      */
/*   2) 若 code != 0，则延时 (3 + code) 秒后主动重启；                    */
/*   3) 用 Flash 计数限制重启次数（默认 6 次），之后正常进入运行；         */
/*   4) PC 侧只需测量 USB 上下线周期 ≈ 启动时间 + 3 + code，即可反推 code。*/
/* 代码含义：1=asocket 无 eloop  2=srv_start 未成功提交  21=asocket_open  */
/*          22=bind 失败  23=listen 失败  30=监听成功  0=全正常          */
/* ------------------------------------------------------------------ */
#define DIAG_BEACON_FILE   "beacon.bin"
#define DIAG_BEACON_MAX    6u

int sdk_diag_code(void)
{
    if (cm_asocket_eloop() == NULL) return 1;       /* asocket 拿不到 eloop */
    if (s_diag_code == 0) {
        if (s_srv[0].fd < 0 && s_srv[1].fd < 0) return 2;   /* 事件没被执行 */
        return 0;
    }
    return s_diag_code;
}

void sdk_diag_beacon(void)
{
    int code = sdk_diag_code();

    sdk_log_printf("[beacon] diag code=%d\r\n", code);
    if (code == 0) return;      /* 一切正常：不重启，正常运行 */

    /*
     * 不做次数限制，每次启动都上报，便于在任意时刻测量。
     * （此前用 Flash 计数器限制 6 次，结果计数器被上一次烧录用尽，
     *   诊断通道被自己堵死 —— 表现为"零重启、拿不到诊断码"。）
     * 延时长度编码诊断码：PC 侧测出的复位周期 ≈ 启动时间 + 3 + code。
     */
    sdk_log_printf("[beacon] report code=%d -> reboot after %d s\r\n", code, 3 + code);
    sdk_task_sleep_ms((uint32_t)(3 + code) * 1000u);
    sdk_reboot();
}

int sdk_srv_start(uint16_t port, sdk_srv_client_cb_t on_client)
{
    return sdk_srv_start_family(port, on_client, 2 /* AF_INET */);
}

/** 通用版本：family 传 AF_INET / AF_INET6（IPv6 监听用于"从 v6 访问面板"） */
int sdk_srv_start_family(uint16_t port, sdk_srv_client_cb_t on_client, int family)
{
    cm_eloop_event_handle_t ev;
    cm_eloop_handle_t       eh;
    int                     i;

    if (on_client == NULL) return -1;

    for (i = 0; i < SDK_SRV_MAX; ++i) {
        if (!s_srv[i].used) break;
    }
    if (i >= SDK_SRV_MAX) return -1;

    memset(&s_srv[i], 0, sizeof(s_srv[i]));
    s_srv[i].port      = port;
    s_srv[i].on_client = on_client;
    s_srv[i].fd        = -1;
    s_srv[i].family    = family;

    (void)sdk_eloop_start();     /* 确保 eloop 已创建并在运行（asocket 依赖它） */
    eh = cm_asocket_eloop();
    if (eh == NULL) {
        sdk_log_printf("[srv] cm_asocket_eloop() returned NULL\r\n");
        return -2;
    }

    ev = cm_eloop_register_event(eh, sdk_srv_start_ev, &s_srv[i]);
    if (ev == NULL) {
        sdk_log_printf("[srv] register_event failed\r\n");
        return -3;
    }

    s_srv[i].used = 1;
    if (cm_eloop_post_event(ev) != 0) {
        s_srv[i].used = 0;
        return -4;
    }

    sdk_log_printf("[srv] start requested on port %u\r\n", (unsigned)port);
    return 0;
}

int sdk_srv_listening(uint16_t port)
{
    int i;

    for (i = 0; i < SDK_SRV_MAX; ++i) {
        if (s_srv[i].used && s_srv[i].port == port && s_srv[i].fd >= 0) return 1;
    }
    return 0;
}




