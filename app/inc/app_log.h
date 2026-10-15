/**
 * @file    app_log.h
 * @brief   UART 调试日志（需求 7.1）。纯 C 封装，输出经由 sdk_port -> SDK 日志口。
 *
 * 日志格式： [模组运行秒数] [等级] 模块: 内容
 * 例： [000123] I net : APN 切换 cmnet -> 3gnet
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

typedef enum {
    APP_LOG_ERROR = 0,
    APP_LOG_WARN  = 1,
    APP_LOG_INFO  = 2,
    APP_LOG_DEBUG = 3
} app_log_level_t;

/** 设置日志等级（默认 APP_LOG_INFO；调试时可调到 DEBUG）。 */
void app_log_set_level(app_log_level_t level);
app_log_level_t app_log_get_level(void);

/** 初始化日志模块（记录启动时刻，打印开机横幅与上电原因）。 */
void app_log_init(void);

/** 通用日志输出。fmt 支持 %s %d %u %x %02x %lu %llu 等常见格式。 */
void app_log(app_log_level_t level, const char *module, const char *fmt, ...);

/** 便捷宏 */
#define LOG_E(mod, ...) app_log(APP_LOG_ERROR, mod, __VA_ARGS__)
#define LOG_W(mod, ...) app_log(APP_LOG_WARN,  mod, __VA_ARGS__)
#define LOG_I(mod, ...) app_log(APP_LOG_INFO,  mod, __VA_ARGS__)
#define LOG_D(mod, ...) app_log(APP_LOG_DEBUG, mod, __VA_ARGS__)

/** 十六进制转储（用于打印 PDU 原文，便于上板比对）。 */
void app_log_hexdump(const char *module, const char *tag, const void *data, uint32_t len);

#endif /* APP_LOG_H */
