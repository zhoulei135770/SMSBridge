/**
 * @file    traffic.c
 * @brief   流量统计与月度限额实现（应用侧基线差值法）。
 */
#include "traffic.h"
#include "sdk_port.h"
#include "app_log.h"
#include "webhook.h"

#include <string.h>
#include <stdio.h>

/* ---- 上电以来的基线 ---- */
static uint64_t s_last_total_rx;      /* 上次采样的平台累计接收 */
static uint64_t s_last_total_tx;      /* 上次采样的平台累计发送 */
static uint64_t s_session_rx;         /* 本次会话累计接收增量 */
static uint64_t s_session_tx;         /* 本次会话累计发送增量 */
static uint64_t s_month_bytes;        /* 本月累计（字节，从上电时的配置值继续） */
static uint32_t s_last_sample_ms;     /* 上次采样时刻 */
static uint32_t s_last_speed_bps;

static uint8_t  s_over_limit;
static uint8_t  s_alert_sent;
static uint8_t  s_cut_applied;
static uint8_t  s_month_rolled;
static uint8_t  s_counter_alive;

/* ================================================================== */
/* 内部工具                                                            */
/* ================================================================== */
static uint32_t mb_to_kb(uint32_t mb)
{
    if (mb > 0x003FFFFFu) return 0xFFFFFFFFu;   /* 防溢出：约 4TB */
    return mb * 1024u;
}

static void read_platform(uint64_t *rx, uint64_t *tx)
{
    uint64_t a = 0u, b = 0u;
    (void)sdk_traffic_get_total_bytes(&a, &b);
    if (rx != NULL) *rx = a;
    if (tx != NULL) *tx = b;
}

/** 检测跨月并清零本月累计。 */
static void check_month_roll(app_config_t *cfg)
{
    char tag[8];
    uint32_t now_tag;

    if (cfg == NULL) return;
    tag[0] = '\0';
    if (sdk_month_tag(tag, sizeof(tag)) != 0) return;   /* 时间未同步，不做跨月判断 */

    now_tag = (uint32_t)((tag[0] - '0') * 100000 +
                         (tag[1] - '0') * 10000 +
                         (tag[2] - '0') * 1000 +
                         (tag[3] - '0') * 100 +
                         (tag[4] - '0') * 10 +
                         (tag[5] - '0'));

    if (cfg->month_tag != 0u && cfg->month_tag != now_tag) {
        LOG_I("traffic", "跨月自动重置：%lu -> %lu，本月已用清零",
              (unsigned long)cfg->month_tag, (unsigned long)now_tag);
        s_month_bytes       = 0u;
        cfg->traffic_used_kb = 0u;
        s_month_rolled      = 1u;
        s_over_limit        = 0u;
        s_alert_sent        = 0u;
        s_cut_applied       = 0u;
    }
    cfg->month_tag = now_tag;
}

/* ================================================================== */
/* 对外接口                                                            */
/* ================================================================== */
void traffic_apply_limit(const app_config_t *cfg)
{
    if (cfg == NULL) return;
    /*
     * 本版底包未向应用导出"平台侧断网阈值"接口（lwip_nwst_set_cutoff_data 不在
     * 链接边界内），因此超限动作只能由本模块在采样时判断并主动断网
     * （APP_LIMIT_ACTION_ALERT_AND_CUT -> sdk_net_deactivate()）。
     */
    LOG_I("traffic", "限额配置已装载：%lu MB（超限动作：%s）",
          (unsigned long)cfg->traffic_limit_mb,
          (cfg->traffic_action == (uint8_t)APP_LIMIT_ACTION_ALERT_AND_CUT)
              ? "告警 + 断网" : "仅告警");
}

void traffic_init(app_config_t *cfg)
{
    if (cfg == NULL) return;

    read_platform(&s_last_total_rx, &s_last_total_tx);

    s_session_rx    = 0u;
    s_session_tx    = 0u;
    s_over_limit    = 0u;
    s_alert_sent    = 0u;
    s_cut_applied   = 0u;
    s_month_rolled  = 0u;
    s_counter_alive = 0u;

    /* 从配置里恢复本月累计（字节），实现"断电不丢失"（需求 4.5） */
    s_month_bytes = (uint64_t)cfg->traffic_used_kb * 1024u;

    check_month_roll(cfg);
    traffic_apply_limit(cfg);

    s_last_sample_ms = sdk_uptime_ms();

    LOG_I("traffic", "流量统计就绪：平台计数 rx=%lu tx=%lu 字节，本月累计 %lu KB",
          (unsigned long)s_last_total_rx, (unsigned long)s_last_total_tx,
          (unsigned long)(s_month_bytes / 1024u));
    LOG_I("traffic", "说明：使用平台导出计数 get_rx_byte_all/get_tx_byte_all + 应用侧基线差值；"
                     "无平台清零接口，重置仅复位应用侧累计值");
}

int traffic_sample(app_config_t *cfg)
{
    uint64_t rx, tx, drx, dtx, dtotal;
    uint32_t now_ms, dt_ms;
    int      need_save = 0;

    if (cfg == NULL) return 0;

    read_platform(&rx, &tx);
    now_ms = sdk_uptime_ms();

    /* 计数器回绕 / 模组重启后归零：重新建立基线，避免出现巨大负数 */
    if (rx < s_last_total_rx || tx < s_last_total_tx) {
        LOG_W("traffic", "平台计数器回退（rx %lu->%lu, tx %lu->%lu），重建基线",
              (unsigned long)s_last_total_rx, (unsigned long)rx,
              (unsigned long)s_last_total_tx, (unsigned long)tx);
        s_last_total_rx = rx;
        s_last_total_tx = tx;
    }

    drx = rx - s_last_total_rx;
    dtx = tx - s_last_total_tx;
    s_last_total_rx = rx;
    s_last_total_tx = tx;

    s_session_rx += drx;
    s_session_tx += dtx;
    s_month_bytes += drx + dtx;

    /* 自检：接口是否真的在工作（用于上板验收） */
    if ((drx + dtx) > 0u && !s_counter_alive) {
        s_counter_alive = 1u;
        LOG_I("traffic", "平台流量计数器已确认生效（本次增量 %lu 字节）",
              (unsigned long)(drx + dtx));
    }

    /* 瞬时速率 */
    dt_ms = now_ms - s_last_sample_ms;
    if (dt_ms > 0u) {
        s_last_speed_bps = (uint32_t)(((drx + dtx) * 1000u) / dt_ms);
    }
    s_last_sample_ms = now_ms;

    /* 需求 7.1：UART 输出流量采样日志 */
    LOG_D("traffic", "采样：本月 %lu KB / 会话 %lu KB（上 %lu / 下 %lu）/ 速率 %lu B/s",
          (unsigned long)(s_month_bytes / 1024u),
          (unsigned long)((s_session_rx + s_session_tx) / 1024u),
          (unsigned long)(s_session_tx / 1024u),
          (unsigned long)(s_session_rx / 1024u),
          (unsigned long)s_last_speed_bps);

    cfg->traffic_used_kb = (uint32_t)(s_month_bytes / 1024u);

    /* ---- 限额判断（需求 4.3） ---- */
    if (cfg->traffic_limit_mb == 0u) {
        s_over_limit = 0u;
        s_alert_sent = 0u;
        return need_save;
    }

    if (s_month_bytes >= (uint64_t)mb_to_kb(cfg->traffic_limit_mb) * 1024u) {
        if (!s_over_limit) {
            s_over_limit = 1u;
            need_save    = 1;      /* 触发阈值 -> 允许写一次 Flash（需求 6.3） */
            LOG_W("traffic", "月度流量已超限：%lu KB >= %lu MB",
                  (unsigned long)(s_month_bytes / 1024u),
                  (unsigned long)cfg->traffic_limit_mb);
        }
        if (!s_alert_sent) {
            char content[192];
            webhook_result_t r;

            (void)snprintf(content, sizeof(content),
                           "> **本月已用：** %.2f MB\n"
                           "> **月度限额：** %lu MB\n"
                           "> **超限动作：** %s",
                           (double)s_month_bytes / 1048576.0,
                           (unsigned long)cfg->traffic_limit_mb,
                           (cfg->traffic_action == (uint8_t)APP_LIMIT_ACTION_ALERT_AND_CUT)
                               ? "告警 + 断开 4G 网络" : "仅告警");

            r = webhook_send_note(cfg, "【流量超限告警】4G 短信网关", content);
            LOG_W("traffic", "超限告警推送结果：HTTP=%d %s", r.http_code, r.detail);
            s_alert_sent = 1u;
            need_save    = 1;
        }
        if (cfg->traffic_action == (uint8_t)APP_LIMIT_ACTION_ALERT_AND_CUT && !s_cut_applied) {
            LOG_W("traffic", "超限动作=断开 PDP，正在断网");
            if (sdk_net_deactivate() == 0) {
                s_cut_applied = 1u;
            }
        }
    } else if (s_over_limit) {
        LOG_I("traffic", "流量已回落到限额以内：%lu KB",
              (unsigned long)(s_month_bytes / 1024u));
        s_over_limit = 0u;
        s_alert_sent = 0u;
        need_save    = 1;
    }

    return need_save;
}

void traffic_get_status(const app_config_t *cfg, traffic_status_t *st)
{
    if (st == NULL) return;
    memset(st, 0, sizeof(*st));

    st->used_kb              = (uint32_t)(s_month_bytes / 1024u);
    st->limit_mb             = (cfg != NULL) ? cfg->traffic_limit_mb : 0u;
    st->session_kb           = (uint32_t)((s_session_rx + s_session_tx) / 1024u);
    st->up_kb                = (uint32_t)(s_session_tx / 1024u);
    st->down_kb              = (uint32_t)(s_session_rx / 1024u);
    st->speed_bps            = s_last_speed_bps;
    st->sample_sec           = (cfg != NULL) ? cfg->traffic_sample_sec : 60u;
    st->total_kb_since_boot  = (uint32_t)((s_session_rx + s_session_tx) / 1024u);
    st->over_limit           = s_over_limit;
    st->alert_sent           = s_alert_sent;
    st->cut_applied          = s_cut_applied;
    st->directional_ok       = 1u;      /* get_tx_byte_all / get_rx_byte_all 提供分方向 */
    st->counter_alive        = s_counter_alive;
    st->month_rolled         = s_month_rolled;
}

int traffic_reset_month(app_config_t *cfg)
{
    s_month_bytes = 0u;
    s_over_limit  = 0u;
    s_alert_sent  = 0u;
    s_cut_applied = 0u;

    if (cfg != NULL) {
        cfg->traffic_used_kb = 0u;
        cfg->reset_count++;
    }

    LOG_I("traffic", "本月流量统计已重置（应用侧累计值清零；平台计数器为只增计数，无需清零）");
    return 0;
}
