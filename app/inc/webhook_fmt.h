/**
 * @file    webhook_fmt.h
 * @brief   Webhook 请求组装（纯逻辑，无 SDK 依赖，可主机单元测试）：
 *          模板渲染 + 钉钉关键词注入 + 钉钉加签 URL 生成。
 *
 * 钉钉要求（docs/09-短信远程配置与钉钉转发-实现规格.md 第四节）：
 *   - 关键词：消息正文必须包含该关键词（本模块以【关键词】前缀形式注入）
 *   - 加签  ：sign = urlencode(base64(HMAC_SHA256(secret, ts + "\n" + secret)))
 *             URL 追加 &timestamp=<ms>&sign=<sign>
 */
#ifndef WEBHOOK_FMT_H
#define WEBHOOK_FMT_H

#include <stdint.h>
#include <stddef.h>

/** 模板渲染：支持 {from} {time} {text} {pdu} {idx}；未知占位符原样保留。 */
int webhook_render_template(const char *tmpl,
                            const char *from, const char *time_s,
                            const char *text, const char *pdu, unsigned idx,
                            char *out, size_t out_size);

/** 关键词注入：若 keyword 非空，输出为 "【kw】正文"。 */
int webhook_apply_keyword(const char *keyword, const char *body,
                          char *out, size_t out_size);

/**
 * @brief 生成最终请求 URL。
 *   secret 为空     -> 原样返回 base（不做加签）
 *   secret 非空     -> base + ("&" 或 "?") + "timestamp=<ms>&sign=<urlencoded>"
 * @return 写入长度，<0 失败
 */
int webhook_build_url(const char *base, const char *secret, uint64_t ts_ms,
                      char *out, size_t out_size);

#endif /* WEBHOOK_FMT_H */