/**
 * @file    sms_svc.h
 * @brief   短信接收服务（需求 3）：URC 注入 -> PDU 解码 -> 黑白名单 -> 环形缓存 -> Webhook 推送。
 *
 * 关键设计（来源：SDK 无收短信 API，已三重核实）：
 *   SDK 只提供 cm_virt_at_sms_send()（发送）。接收只能通过
 *   cm_virt_at_urc_reg() 注册的主动上报回调拿到 "+CMT:" 与随后的 PDU 行。
 *   URC 回调运行在 SDK 线程上下文，必须极快返回，因此：
 *     URC 回调 -> 只做"把一行文本塞进环形队列 + 发信号量"
 *     sms_svc_task -> 出队、状态机拼行、解码、过滤、推送
 */
#ifndef SMS_SVC_H
#define SMS_SVC_H

#include <stdint.h>
#include <stddef.h>
#include "app_config.h"
#include "sms_pdu.h"
#include "sms_rcfg.h"     /* sms_remote_cfg_t（快照接口用） */

/** 环形缓存中一条短信记录（RAM，断电丢失 —— 需求 6.4） */
#define SMS_RECORD_NUM          16
#define SMS_RECORD_TEXT_MAX     256
#define SMS_RECORD_NUM_MAX      24
#define SMS_RECORD_TIME_MAX     32

typedef struct {
    char     number[SMS_RECORD_NUM_MAX];
    char     time[SMS_RECORD_TIME_MAX];
    char     text[SMS_RECORD_TEXT_MAX];
    uint16_t text_len;
    uint8_t  blocked;        /* 1 = 被黑白名单拦截 */
    uint8_t  forwarded;      /* 1 = 已成功推送 Webhook */
    uint8_t  multipart;      /* 1 = 长短信分片（本版不重组，仅标记） */
    uint8_t  part_seq;
    uint8_t  part_total;
    uint16_t raw_len;        /* PDU 原始长度，便于日志导出 */
} sms_record_t;

/** 服务运行统计 */
typedef struct {
    uint32_t received;       /* 收到的短信总数（不含仅缓存的分片） */
    uint32_t blocked;        /* 被拦截数 */
    uint32_t forwarded;      /* 推送成功数 */
    uint32_t failed;         /* 推送失败数 */
    uint32_t decode_error;   /* 解码失败数 */
    /* ---- 长短信重组与远程配置（docs/09 / docs/10） ---- */
    uint32_t parts;          /* 收到的长短信分片数（未计入 received） */
    uint32_t reassembled;    /* 重组完成的长短信条数 */
    uint32_t concat_drop;    /* 重组失败/超时被丢弃的分片数 */
    uint32_t admin_cmd;      /* 管理员指令短信条数 */
    uint32_t replies;        /* 回执短信发送成功条数 */
    uint32_t reply_failed;   /* 回执发送失败条数 */
} sms_stat_t;

/**
 * @brief 初始化：初始化环形缓存与队列，并通过 sdk_port 注册 URC / 配置 +CMGF +CNMI。
 * @return 0 成功；>0 部分成功（URC 已注册但 AT 配置告警）；<0 失败。
 */
int sms_svc_init(void);

/** 短信服务任务主体（由 main 创建）。 */
void sms_svc_task(void *arg);

/**
 * @brief 供 sdk_port 的 URC 回调调用（**必须极快返回**）。
 * @note 线程安全：只做队列写入，内部用 sdk_port 的临界区保护。
 */
void sms_svc_on_urc(const char *line);

/** 取最近的短信（按时间倒序，最新在前）。返回实际条数。 */
size_t sms_svc_recent(sms_record_t *out, size_t max);

/** 导出短信日志为纯文本，返回写入字节数。 */
size_t sms_svc_export(char *buf, size_t cap);

/** 读取统计。 */
void sms_svc_stat(sms_stat_t *st);

/** 清空短信缓存。 */
void sms_svc_clear(void);

/**
 * @brief 立即尝试一次时钟同步（模组时钟 -> NTP）。
 * @note 服务任务每秒会自动按限速重试；启动流程在入网后也可主动调一次。
 */
void sms_svc_time_sync_try(void);

/**
 * @brief 读取远程配置与短信统计的快照（供 Web 面板只读展示）。
 * @param out    输出远程配置（不可为 NULL）
 * @param loaded 输出 1 = 来自 rcfg.bin，0 = 默认值（可为 NULL）
 * @param st     输出统计（可为 NULL）
 * @return 0 成功；<0 参数错误
 * @note 线程安全：内部用互斥锁拷贝，可与短信任务的写入并发。
 */
int sms_svc_rcfg_snapshot(sms_remote_cfg_t *out, int *loaded, sms_stat_t *st);

#endif /* SMS_SVC_H */
