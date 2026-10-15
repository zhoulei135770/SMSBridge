/**
 * @file    app_log.c
 * @brief   UART 调试日志实现。
 */
#include "app_log.h"
#include "sdk_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static app_log_level_t s_level = APP_LOG_INFO;
static uint32_t        s_boot_ms;

void app_log_set_level(app_log_level_t level)
{
    if (level <= APP_LOG_DEBUG) s_level = level;
}

app_log_level_t app_log_get_level(void)
{
    return s_level;
}

void app_log_init(void)
{
    int reason;

    s_boot_ms = sdk_uptime_ms();

    sdk_log_puts("\r\n");
    sdk_log_puts("========================================\r\n");
    sdk_log_puts(" ML307A-DSLN 4G SMS Gateway  (OpenCPU)\r\n");
    sdk_log_puts(" firmware v1.1  build " __DATE__ " " __TIME__ "\r\n");
    sdk_log_puts("========================================\r\n");

    reason = sdk_power_on_reason();
    if (reason == 2) {
        /* 需求 7.2：看门狗 60s 超时复位后要能看出来 */
        LOG_W("boot", "上次复位原因: 看门狗/异常复位 (WDT)");
    } else if (reason == 1) {
        LOG_I("boot", "上次复位原因: 正常上电");
    } else {
        LOG_I("boot", "上次复位原因: 未知/首次上电");
    }
}

void app_log(app_log_level_t level, const char *module, const char *fmt, ...)
{
    char     line[224];
    char     body[176];
    va_list  ap;
    uint32_t t;
    const char *tag;

    if (level > s_level) return;

    switch (level) {
    case APP_LOG_ERROR: tag = "E"; break;
    case APP_LOG_WARN:  tag = "W"; break;
    case APP_LOG_INFO:  tag = "I"; break;
    default:            tag = "D"; break;
    }

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    t = (sdk_uptime_ms() - s_boot_ms) / 1000u;

    /* 手工拼接，避免 vsnprintf 在嵌入式 libc 上的 %lld 支持问题 */
    (void)snprintf(line, sizeof(line), "[%06lu] %s %-6s: %s\r\n",
                   (unsigned long)t, tag,
                   (module != NULL) ? module : "app",
                   body);

    sdk_log_puts(line);
}

void app_log_hexdump(const char *module, const char *tag, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    char           line[100];
    uint32_t       i;
    int            n;

    if (p == NULL) return;

    /* 逐行 32 字节，避免一次分配大缓冲 */
    for (i = 0; i < len; i += 32u) {
        uint32_t j;
        n = snprintf(line, sizeof(line), "[%s] %s +%03lu: ",
                     (module != NULL) ? module : "app",
                     (tag != NULL) ? tag : "hex",
                     (unsigned long)i);
        if (n < 0) return;
        for (j = i; j < i + 32u && j < len && (size_t)n + 3u < sizeof(line); ++j) {
            n += snprintf(&line[n], sizeof(line) - (size_t)n, "%02X ", p[j]);
            if (n < 0) return;
        }
        if ((size_t)n + 2u < sizeof(line)) {
            line[n++] = '\r';
            line[n++] = '\n';
            line[n]   = '\0';
        }
        sdk_log_puts(line);
    }
}
