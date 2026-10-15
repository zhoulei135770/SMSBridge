/**
 * @file    usb_rndis.c
 * @brief   USB 流量共享（RNDIS）控制实现。
 *
 * 结论（链接器实证，见 docs/sdk-reference/app-link-symbols.txt）：
 *   用户 APP 的可调用符号边界是 cm_stub_DSLN.o（326 个），其中 **没有任何**
 *   usb / rndis / netif / lwip_rndis_hotplug_* / lwip_set_usb_forward_flag 符号。
 *   内核 map 中可见的 rndis.o、netif_usb_set_status 等属于内核镜像自身，
 *   **未对应用导出**，应用调用会 undefined reference。
 *   ⇒ 应用层无法运行时开关 RNDIS。RNDIS 的实际状态由底包 / NV 配置决定。
 *
 * 本模块的职责因此收敛为：
 *   1. 把用户在 Web 面板上的选择持久化为"期望配置"（掉电记忆，需求 2.5）；
 *   2. 启动时尝试应用一次，失败则明确告知"重启后生效"（需求 2.6 的降级分支）；
 *   3. 在 Web 面板如实展示"实际状态未知（底包未导出查询接口）"，
 *      不伪造状态（需求 2.7）。
 */
#include "usb_rndis.h"
#include "sdk_port.h"
#include "app_log.h"

#include <string.h>

static int s_configured  = 1;   /* 配置里的期望值 */
static int s_active      = -1;  /* -1 = 未知（本底包无法查询） */
static int s_runtime_ok  = 0;   /* 运行时切换是否可用 */

void usb_rndis_init(const app_config_t *cfg)
{
    int want;
    int rc;

    if (cfg == NULL) return;

    want = (cfg->rndis_enable != 0u) ? 1 : 0;
    s_configured = want;

    rc = sdk_rndis_apply(want);
    if (rc == 0) {
        s_runtime_ok = 1;
        s_active     = want;
        LOG_I("rndis", "USB 流量共享已%s（运行时接口可用，即时生效）",
              want ? "开启" : "关闭");
    } else {
        s_runtime_ok = 0;
        s_active     = -1;
        LOG_W("rndis", "本底包未向应用导出 RNDIS 控制接口：USB 流量共享无法由固件切换。");
        LOG_W("rndis", "已把「%s」记录为配置期望值（掉电记忆）；"
                       "RNDIS 实际状态由底包/NV 决定，重启后请用电脑侧 ipconfig 核对。",
              want ? "开启" : "关闭");
    }
}

int usb_rndis_set(int enable, int *need_reboot)
{
    int want = enable ? 1 : 0;

    s_configured = want;

    if (s_runtime_ok && sdk_rndis_apply(want) == 0) {
        s_active = want;
        LOG_I("rndis", "USB 流量共享切换为%s（即时生效）", want ? "开启" : "关闭");
        if (need_reboot != NULL) *need_reboot = 0;
        return 0;
    }

    /* 降级分支：本底包不支持运行时切换，只能提示用户重启（需求 2.6） */
    if (need_reboot != NULL) *need_reboot = 1;
    LOG_W("rndis", "USB 流量共享无法即时切换，已保存为期望值「%s」，需重启后由底包生效",
          want ? "开启" : "关闭");
    return -1;
}

void usb_rndis_status(rndis_status_t *st)
{
    if (st == NULL) return;

    st->configured = s_configured;
    st->apply_now  = s_runtime_ok;

    /* 主动查询一次；本底包返回 -1（未知） */
    {
        int real = sdk_rndis_is_active();
        if (real >= 0) {
            s_active = real;
        }
    }
    st->active  = s_active;                     /* -1 表示未知 */
    st->changed = (s_active >= 0 && s_active != s_configured) ? 1 : 0;
}

const char *usb_rndis_note(void)
{
    return s_runtime_ok ? "即时生效" : "由底包/NV 决定，固件无法运行时切换";
}
