/**
 * @file    traffic.h
 * @brief   流量统计与月度限额管理（需求 4）。
 *
 * 数据来源（重要，已用链接器实证，见 docs/sdk-reference/app-link-symbols.txt）：
 *   - 用户 APP 的可调用边界是 prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o（326 符号）。
 *     SDK 头文件里声明的 lwip_nwst_get_used_data / lwip_nwst_clear_used_data
 *     **不在该边界内**，调用会 undefined reference，无法使用。
 *   - 边界内可用的是平台导出的两个累计计数器：
 *         get_rx_byte_all()  累计接收字节
 *         get_tx_byte_all()  累计发送字节
 *     它们未写入任何 SDK 头文件（属未文档化导出），语义需上板实测确认。
 *   - 由于计数器只增不减且**没有清零接口**，本模块用"应用侧基线差值"实现
 *     月度统计 / 本次会话统计 / 一键重置，完全不需要平台清零能力：
 *        本月已用 = 持久化的月度累计值（每次采样累加增量）
 *        本次会话 = 上电以来累计增量
 *        上下行   = 分别累加 tx / rx 增量
 */
#ifndef TRAFFIC_H
#define TRAFFIC_H

#include <stdint.h>
#include "app_config.h"

typedef struct {
    uint32_t used_kb;            /* 本月已用（KB，持久化累计） */
    uint32_t limit_mb;           /* 月度限额（MB），0=不限 */
    uint32_t session_kb;         /* 本次会话（上电以来）合计（KB） */
    uint32_t up_kb;              /* 本次会话上行（KB） */
    uint32_t down_kb;            /* 本次会话下行（KB） */
    uint32_t speed_bps;          /* 瞬时速率（B/s） */
    uint32_t sample_sec;         /* 采样周期（秒） */
    uint32_t total_kb_since_boot;/* 上电以来平台计数增量（KB），用于校验接口是否生效 */
    uint8_t  over_limit;         /* 1=已超限 */
    uint8_t  alert_sent;         /* 1=本次超限已推送过告警 */
    uint8_t  cut_applied;        /* 1=已执行断网动作 */
    uint8_t  directional_ok;     /* 1=支持上下行分方向统计（本版为 1） */
    uint8_t  counter_alive;      /* 1=平台计数器确实在增长（接口生效） */
    uint8_t  month_rolled;       /* 1=本次上电检测到跨月并自动清零 */
} traffic_status_t;

/** 上电初始化：读平台计数器建立基线、处理跨月。 */
void traffic_init(app_config_t *cfg);

/**
 * @brief 采样一次（由 main 的 traffic 任务按 sample_sec 周期调用）。
 * @return 1 表示本次采样触发了 Flash 写入（超限阈值/跨月），否则 0。
 */
int traffic_sample(app_config_t *cfg);

/** 供 Web 面板查询状态。 */
void traffic_get_status(const app_config_t *cfg, traffic_status_t *st);

/** 一键重置本月流量统计（需求 4.4）。返回 0 成功。 */
int traffic_reset_month(app_config_t *cfg);

/** 应用限额配置（阈值告警/断网由本模块按采样周期自行判断）。 */
void traffic_apply_limit(const app_config_t *cfg);

#endif /* TRAFFIC_H */
