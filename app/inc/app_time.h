/**
 * @file    app_time.h
 * @brief   时间解析与"时钟是否可信"判定（纯 C99，无 SDK 依赖，可主机单元测试）。
 *
 * 为什么单独一个模块：钉钉加签用的 `timestamp` 必须是**真实的毫秒级 UTC 时间**，
 * 时钟不对会被钉钉直接拒收（docs/09 第四节）。而时间来源有三种形态：
 *   - SDK 的 NTP 回调给的是 ISO-8601 字符串（`2026-10-15T10:20:31.868+08:00`）；
 *   - 模组自身的 `AT+CCLK?` 给的是 `"26/10/15,10:20:31+32"`（时区单位是 15 分钟）；
 *   - 还可能是 `Z` 结尾或无时区的形式。
 * 这三种解析 + 秒->毫秒换算全部是纯逻辑，放在这里先把边界（闰年、世纪闰年、
 * 跨时区、越界字段）测对，再上板。
 */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdint.h>
#include <stddef.h>

/**
 * 可信时钟下限：2025-10-09T00:00:00Z。
 * RTC 未对时时通常返回 0 或一个很小的值，用它来判定"能不能加签"。
 */
#define APP_TIME_MIN_VALID   1760000000LL

/** 可信时钟上限：2100-01-01T00:00:00Z（挡住明显异常的未来时间）。 */
#define APP_TIME_MAX_VALID   4102444800LL

/**
 * @brief 年月日时分秒（UTC）-> Unix 时间戳（秒）。
 * @note 采用 Howard Hinnant 的 civil-from-days 算法，无查表、无浮点，
 *       对 1970 前后与世纪闰年都正确。
 */
int64_t app_time_utc_from_fields(int year, int mon, int day, int hour, int min, int sec);

/** 时间戳是否落在可信区间 [APP_TIME_MIN_VALID, APP_TIME_MAX_VALID]。 */
int app_time_is_valid(int64_t utc_seconds);

/**
 * @brief 解析 ISO-8601 时间串。
 * 支持：`2026-10-15T10:20:31Z`、`2026-10-15T10:20:31.868+08:00`、
 *       `2026-10-15T10:20:31-05:30`、省略秒、用空格代替 'T'、无时区（按 UTC）。
 * @return 0 成功；<0 失败（格式非法或字段越界）
 */
int app_time_parse_iso8601(const char *s, int64_t *utc_out);

/**
 * @brief 解析 `AT+CCLK?` 应答里的本地时间。
 *
 * 形态：`+CCLK: "26/10/15,10:20:31+32"`（两位年 -> 20xx；`+32` = 32×15 分钟 = UTC+8），
 * 也兼容四位年与 `-` 时区、无时区。
 *
 * @return 0 成功（已换算成 UTC 秒）；<0 失败
 */
int app_time_parse_cclk(const char *at_resp, int64_t *utc_out);

/**
 * @brief 毫秒时间戳 = UTC 秒 × 1000 + 当前毫秒位。
 * @param uptime_ms sdk_uptime_ms()（单调毫秒）；只取低 3 位补足亚秒部分
 * @return 0 表示时钟无效（调用方应降级为不加签）
 */
uint64_t app_time_to_ts_ms(int64_t utc_seconds, uint32_t uptime_ms);

#endif /* APP_TIME_H */
