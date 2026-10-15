/**
 * @file    sms_concat.h
 * @brief   拼接短信（Concatenated SM）收发：接收重组 + 发送分包。
 *          纯 C99，无 SDK 依赖，可主机单元测试。
 *
 * 依据 docs/09-短信远程配置与钉钉转发-实现规格.md 第三节。
 * 发送侧固定使用 UCS2（DCS=0x08），每片正文上限 67 字，PDU 带 6 字节 UDH：
 *      05 00 03 <ref> <total> <seq>
 * 接收侧消费 sms_pdu_t.concat（解码器已解析级联 IE）。
 */
#ifndef SMS_CONCAT_H
#define SMS_CONCAT_H

#include <stdint.h>
#include <stddef.h>
#include "sms_pdu.h"

#define SMS_CONCAT_SLOTS      4u    /**< 并发重组条目上限 */
#define SMS_CONCAT_MAX_PARTS  8u    /**< 单条拼接短信最大分片数 */
#define SMS_CONCAT_MAX_TEXT   1200u /**< 重组后文本上限（UTF-8 字节） */
#define SMS_CONCAT_TIMEOUT_MS 30000u/**< 分片收齐超时 */

#define SMS_TX_MAX_PARTS      8u    /**< 发送最大分片数 */
#define SMS_TX_UCS2_PER_PART  67u   /**< UCS2 每片字符数（带 UDH 拼接时） */
#define SMS_TX_PLAIN_PER_PART 70u   /**< 独立短信（无 UDH）时每片 UCS2 字符数 = 140 字节 UDL 上限 */

/** 重组结果状态 */
#define SMS_CONCAT_BUFFERED  0
#define SMS_CONCAT_COMPLETE  1
#define SMS_CONCAT_ERROR    (-1)

/** 重组完成后的消息 */
typedef struct {
    char   number[SMS_ADDR_MAX_LEN];
    char   scts[32];
    char   text[SMS_CONCAT_MAX_TEXT];
    size_t text_len;
} sms_concat_msg_t;

/** 喂入一片已解码 PDU。返回 SMS_CONCAT_*。 */
int  sms_concat_feed(const sms_pdu_t *pdu, sms_concat_msg_t *msg, uint32_t now_ms);
/** 清空重组缓存 */
void sms_concat_reset(void);
/** 丢弃超时分片（now_ms 单调毫秒） */
void sms_concat_tick(uint32_t now_ms);

/** 发送分包结果 */
typedef struct {
    char    pdu[SMS_PDU_MAX_HEX];
    uint8_t seq;
    uint8_t total;
} sms_tx_pkt_t;

/**
 * @brief 把 UTF-8 文本编码为若干 SMS-SUBMIT PDU（UCS2 + UDH 拼接）。
 * @param sca  短信中心（可带 '+'，空串表示用默认）
 * @param dst  目标号码（可带 '+'）
 * @return 分片数，<0 失败（-1 参数 -2 超长 -3 单条过长）
 * @note **SMSC 已写进 PDU 首部**（SDK 明文要求），发送时不要重复携带。
 */
int  sms_concat_build_tx(const char *sca, const char *dst, const char *utf8_text,
                         sms_tx_pkt_t *pkts, int max_pkts);

/**
 * @brief 把 UTF-8 文本切成若干**彼此独立**的 SMS-SUBMIT PDU（UCS2，**不带 UDH**）。
 *
 * 为什么需要它（2026-10-15 上板教训）：
 *   UDH 拼接短信（上面那个函数）要求手机**收齐全部分片**才会显示；实测本底包在
 *   连续发送多片时，第 2 片起会被模组拒绝（上一片尚未提交完），结果用户**什么都看不到**
 *   ——因为手机手里只有第 1 片，永远等不到其余片。
 *   改成"每片独立成一条短信"后，成功一片就显示一片，不再有全有或全无的问题。
 *
 * @param sca 短信中心（可带 '+'）
 * @param dst 目标号码
 * @param utf8_text UTF-8 正文
 * @param pkts 输出分片（每片是完整可发送的 PDU）
 * @param max_pkts 分片上限
 * @return 分片数；<0 失败（-1 参数 -2 超过 max_pkts）
 */
int  sms_concat_build_tx_plain(const char *sca, const char *dst, const char *utf8_text,
                               sms_tx_pkt_t *pkts, int max_pkts);

/**
 * @brief 从 `AT+CSCA?` 的应答里取出短信中心号码。
 *
 * 应答形如：`+CSCA: "+8613800738500",145`
 *   - 去掉引号，保留 '+'（145 = 国际号码）；
 *   - 空号码（`+CSCA: "",145`，即用模组默认）返回 <0，调用方按"空串"处理；
 *   - 解析失败返回 <0。
 *
 * @param at_resp  AT 应答原文（可含多行）
 * @return 0 成功且 out 非空；<0 失败（out 被置为空串）
 */
int  sms_concat_parse_csca(const char *at_resp, char *out, size_t out_size);

/** 从 PDU 十六进制串里取 UD 起始位置（供调试/测试用） */
int  sms_concat_selftest(void);

#endif /* SMS_CONCAT_H */