/**
 * @file    usb_rndis.h
 * @brief   USB 流量共享（RNDIS）控制模块（需求 2）。
 *
 * 已核实的 SDK 支撑（详见 docs/sdk-reference/api-platform-net.md）：
 *   - lwip_rndis_hotplug_link()   : 通知 USB 主机侧"网卡插入"（enable）
 *   - lwip_rndis_hotplug_break()  : 通知 USB 主机侧"网卡拔出"（disable）
 *   - netif_usb_set_status(u8)    : USB netif 的 up/down 生命周期
 *   - lwip_set_usb_forward_flag() : L3 层"USB 流量转发到 4G"开关
 * 三个函数都在 ML307A-DSLN 底包中真实链接存在。
 */
#ifndef USB_RNDIS_H
#define USB_RNDIS_H

#include "app_config.h"

/** 模块状态 */
typedef struct {
    int  configured;    /* 配置里的期望值 0/1 */
    int  active;        /* 实网卡当前实际状态 1/0/-1 未知 */
    int  apply_now;     /* 1=支持即时生效 0=需重启生效 */
    int  changed;       /* 1=当前运行状态与配置不一致（需要重启或未生效） */
} rndis_status_t;

/** 上电时按配置应用一次（需求 2.2 默认启用）。 */
void usb_rndis_init(const app_config_t *cfg);

/**
 * @brief 运行时切换（Web 开关调用）。
 * @param enable 1 开 0 关
 * @param need_reboot 输出：1 表示需要重启才生效（调用方应提示用户）
 * @return 0 成功；<0 该底包不支持运行时切换（此时已把 need_reboot 置 1，
 *         并把期望值写进配置，重启后自动生效）。
 */
int usb_rndis_set(int enable, int *need_reboot);

/** 查询当前状态（供 Web 面板展示）。 */
void usb_rndis_status(rndis_status_t *st);

/** 生效方式的中文说明，用于 Web 面板。 */
const char *usb_rndis_note(void);

#endif /* USB_RNDIS_H */
