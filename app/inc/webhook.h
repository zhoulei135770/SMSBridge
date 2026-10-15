/**
 * @file    webhook.h
 * @brief   Webhook 推送：把短信/告警组装成企业微信或钉钉机器人的 Markdown 消息并 POST。
 *          消息体组装是纯 C（可主机单元测试），发送走 sdk_port。
 */
#ifndef WEBHOOK_H
#define WEBHOOK_H

#include <stdint.h>
#include <stddef.h>
#include "app_config.h"
#include "sms_rcfg.h"     /* 远程配置（钉钉 URL / 加签密钥 / 关键词 / 模板） */

/** 转发 URL 上限：远程配置 192 字节 + 加签参数（timestamp+sign）约 90 字节 */
#define WEBHOOK_URL_MAX    320
/** 请求体上限（与内部组装缓冲一致） */
#define WEBHOOK_BODY_MAX   2048

/** 推送结果 */
typedef struct {
    int  http_code;     /* 200/204 表示 HTTP 层成功；<0 表示连接层失败 */
    int  biz_ok;        /* 1 = 机器人返回 errcode==0（业务成功） */
    char detail[96];    /* 失败原因摘要，可直接打日志 */
} webhook_result_t;

/**
 * @brief 一个已组装好的转发请求（纯数据，无 I/O）。
 * @note 结构体约 2.4KB，调用方应放**静态区**而不是任务栈。
 */
typedef struct {
    char   url[WEBHOOK_URL_MAX];      /* 最终 URL（含 timestamp/sign，若已加签） */
    char   payload[WEBHOOK_BODY_MAX]; /* 最终 JSON 请求体 */
    size_t payload_len;
    int    signed_ok;                 /* 1 = 已按密钥加签 */
    int    sign_downgraded;           /* 1 = 配了密钥但时钟不可信，已降级为不加签 */
} webhook_req_t;

/**
 * @brief 组装一条短信转发请求（**纯函数**，无 I/O，可主机单元测试）。
 *
 * 组合顺序（docs/09 第四节）：
 *   模板渲染（{from}/{time}/{text}/{pdu}/{idx}）-> 【关键词】注入 -> 机器人 JSON 体 -> 加签 URL
 *
 * 取值优先级：远程配置（短信里设的）优先，为空时回落到 app_config 的对应字段。
 *
 * @param cfg         系统配置（决定机器人类型；提供 URL 兜底）
 * @param rcfg        远程配置（URL/密钥/关键词/模板），可为 NULL
 * @param from        发件号码      @param time_s 接收时间
 * @param text        正文          @param pdu    原始 PDU（可为 NULL）
 * @param idx         序号（模板 {idx}）
 * @param ts_ms       毫秒时间戳（clock_valid=1 时用于加签）
 * @param clock_valid 1 = 时钟可信；0 = 未对时（有密钥则降级）
 * @return 0 成功；-1 参数错误；-2 未配置 URL；-3 渲染/编码失败（含超长）
 */
int webhook_build_forward(const app_config_t *cfg, const sms_remote_cfg_t *rcfg,
                          const char *from, const char *time_s, const char *text,
                          const char *pdu, unsigned idx,
                          uint64_t ts_ms, int clock_valid,
                          webhook_req_t *req);

/** 发送一个已组装好的转发请求（I/O，走 sdk_port）。 */
webhook_result_t webhook_send_request(const webhook_req_t *req);

/**
 * @brief 组装一条短信转发消息（Markdown）。
 * @param cfg  配置
 * @param number 发件号码
 * @param text   短信正文（UTF-8）
 * @param time   服务时间戳字符串
 * @param out    输出缓冲
 * @param cap    缓冲大小
 * @return 写入字节数（不含结尾 '\0'）；0 表示失败。
 * @note 纯函数，无副作用，供单元测试与真机共用。
 */
size_t webhook_build_sms_markdown(const app_config_t *cfg,
                                  const char *number, const char *text,
                                  const char *time,
                                  char *out, size_t cap);

/** 组装一条自定义 Markdown 消息（用于流量告警、开机通知等）。 */
size_t webhook_build_note_markdown(const app_config_t *cfg,
                                   const char *title, const char *content,
                                   char *out, size_t cap);

/**
 * @brief 发送一条短信到 Webhook。
 * @return webhook_result_t，http_code==200/204 且 biz_ok 视为成功。
 */
webhook_result_t webhook_send_sms(const app_config_t *cfg,
                                 const char *number, const char *text,
                                 const char *time);

/** 发送一条纯文本 Markdown 通知（流量超限告警等）。 */
webhook_result_t webhook_send_note(const app_config_t *cfg,
                                  const char *title, const char *content);

/**
 * @brief 用**远程配置（短信下发）**里的 URL/密钥/关键词，组装一条自定义 Markdown 通知。
 *
 * 与 webhook_build_note_markdown() 的区别：后者的 URL/密钥/机器人类型都取自
 * `app_config`（出厂配置），而本工程的实际配置是短信写进 `rcfg.bin` 的，
 * 因此需要这个"以 rcfg 为准"的版本（同时保持机器人类型识别用生效 URL，
 * 关键词按 rcfg 注入，密钥按 rcfg 加签、时钟不可信时降级为不加签）。
 *
 * @return 0 成功；<0 失败（-2 = 没有可用 URL）
 */
int webhook_build_note_rcfg(const app_config_t *cfg, const sms_remote_cfg_t *rcfg,
                            const char *title, const char *content,
                            uint64_t ts_ms, int clock_valid, webhook_req_t *req);

/** 按 URL 自动识别机器人类型（cfg->webhook_type == APP_WEBHOOK_AUTO 时使用）。 */
app_webhook_type_t webhook_detect_type(const char *url);

#endif /* WEBHOOK_H */
