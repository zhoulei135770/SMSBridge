/**
 * @file    sms_concat.c
 * @brief   拼接短信收发实现（见 inc/sms_concat.h）。
 */
#include "sms_concat.h"
#include <string.h>

/* ---------------- 内部状态 ---------------- */
typedef struct {
    uint8_t  used;
    uint8_t  is_16bit;
    uint16_t ref;
    uint8_t  total;
    uint8_t  have;
    uint32_t stamp_ms;
    char     number[SMS_ADDR_MAX_LEN];
    char     scts[32];
    char     part[SMS_CONCAT_MAX_PARTS][SMS_TEXT_MAX_BYTES];
} concat_slot_t;

static concat_slot_t s_slot[SMS_CONCAT_SLOTS];
static uint8_t       s_tx_ref = 0x30u;

void sms_concat_reset(void)
{
    memset(s_slot, 0, sizeof(s_slot));
}

void sms_concat_tick(uint32_t now_ms)
{
    unsigned i;
    for (i = 0; i < SMS_CONCAT_SLOTS; ++i) {
        if (s_slot[i].used &&
            (uint32_t)(now_ms - s_slot[i].stamp_ms) > SMS_CONCAT_TIMEOUT_MS) {
            s_slot[i].used = 0;
        }
    }
}

int sms_concat_feed(const sms_pdu_t *pdu, sms_concat_msg_t *msg, uint32_t now_ms)
{
    concat_slot_t *s = NULL;
    unsigned       i;
    unsigned       n;
    size_t         off = 0;

    if (pdu == NULL || msg == NULL) return SMS_CONCAT_ERROR;

    /* 非分片短信：直接返回完整内容 */
    if (!pdu->concat.present) {
        n = (pdu->text_len < SMS_CONCAT_MAX_TEXT - 1u) ? (unsigned)pdu->text_len
                                                       : (SMS_CONCAT_MAX_TEXT - 1u);
        memcpy(msg->text, pdu->text, n);
        msg->text[n] = '\0';
        msg->text_len = n;
        memcpy(msg->number, pdu->number, sizeof(msg->number));
        msg->number[sizeof(msg->number) - 1u] = '\0';
        memcpy(msg->scts, pdu->scts, sizeof(msg->scts));
        msg->scts[sizeof(msg->scts) - 1u] = '\0';
        return SMS_CONCAT_COMPLETE;
    }

    if (pdu->concat.total == 0u || pdu->concat.total > SMS_CONCAT_MAX_PARTS ||
        pdu->concat.seq == 0u || pdu->concat.seq > pdu->concat.total) {
        return SMS_CONCAT_ERROR;
    }

    /* 查找或新建条目 */
    for (i = 0; i < SMS_CONCAT_SLOTS; ++i) {
        if (s_slot[i].used && s_slot[i].ref == pdu->concat.ref &&
            s_slot[i].total == pdu->concat.total &&
            strncmp(s_slot[i].number, pdu->number, sizeof(s_slot[i].number)) == 0) {
            s = &s_slot[i];
            break;
        }
    }
    if (s == NULL) {
        for (i = 0; i < SMS_CONCAT_SLOTS; ++i) {
            if (!s_slot[i].used) {
                s = &s_slot[i];
                memset(s, 0, sizeof(*s));
                s->used     = 1u;
                s->is_16bit = pdu->concat.is_16bit;
                s->ref      = pdu->concat.ref;
                s->total    = pdu->concat.total;
                memcpy(s->number, pdu->number, sizeof(s->number));
                s->number[sizeof(s->number) - 1u] = '\0';
                break;
            }
        }
    }
    if (s == NULL) return SMS_CONCAT_ERROR;      /* 槽位已满 */

    s->stamp_ms = now_ms;
    memcpy(s->scts, pdu->scts, sizeof(s->scts));
    s->scts[sizeof(s->scts) - 1u] = '\0';

    /* 存入分片（重复分片允许覆盖） */
    {
        unsigned idx = (unsigned)(pdu->concat.seq - 1u);
        size_t   n2  = (pdu->text_len < SMS_TEXT_MAX_BYTES - 1u) ? pdu->text_len
                                                                 : (SMS_TEXT_MAX_BYTES - 1u);
        if (s->part[idx][0] == '\0') s->have++;
        memcpy(s->part[idx], pdu->text, n2);
        s->part[idx][n2] = '\0';
    }

    if (s->have < s->total) return SMS_CONCAT_BUFFERED;

    /* 收齐：按序号拼装 */
    for (i = 0; i < s->total; ++i) {
        size_t plen = strlen(s->part[i]);
        if (off + plen >= SMS_CONCAT_MAX_TEXT) break;
        memcpy(&msg->text[off], s->part[i], plen);
        off += plen;
    }
    msg->text[off] = '\0';
    msg->text_len  = off;
    memcpy(msg->number, s->number, sizeof(msg->number));
    msg->number[sizeof(msg->number) - 1u] = '\0';
    memcpy(msg->scts, s->scts, sizeof(msg->scts));
    msg->scts[sizeof(msg->scts) - 1u] = '\0';

    s->used = 0;                                  /* 释放槽位 */
    return SMS_CONCAT_COMPLETE;
}

/* ---------------- 发送编码（UCS2 + UDH） ---------------- */
static int hex_put(char *out, size_t out_size, size_t *pos, uint8_t b)
{
    static const char h[] = "0123456789ABCDEF";
    if (*pos + 2u >= out_size) return -1;
    out[(*pos)++] = h[(b >> 4) & 0x0Fu];
    out[(*pos)++] = h[b & 0x0Fu];
    return 0;
}

/* 号码 -> （长度, TOA, BCD 数字串） */
static int addr_encode(const char *num, uint8_t *len, uint8_t *toa, uint8_t *bcd, unsigned bcd_max)
{
    unsigned n = 0;
    uint8_t  t = 0x81u;
    const char *p = num;

    if (p == NULL) return -1;
    if (*p == '+') { t = 0x91u; ++p; }
    while (*p != '\0') {
        if (*p < '0' || *p > '9') { ++p; continue; }   /* 跳过分隔符 */
        if (n / 2u >= bcd_max) return -1;
        {
            uint8_t d = (uint8_t)(*p - '0');
            if ((n & 1u) == 0u) bcd[n / 2u] = d;
            else                bcd[n / 2u] |= (uint8_t)(d << 4);
        }
        ++n;
        ++p;
    }
    if ((n & 1u) != 0u) bcd[n / 2u] |= 0xF0u;          /* 奇数位补 F */
    *len = (uint8_t)n;
    *toa = t;
    return 0;
}

static unsigned utf8_to_ucs2(const char *s, uint8_t *out, unsigned out_max)
{
    unsigned o = 0;
    while (*s != '\0' && o + 2u <= out_max) {
        uint32_t cp;
        unsigned char c = (unsigned char)*s++;
        if (c < 0x80u) {
            cp = c;
        } else if ((c & 0xE0u) == 0xC0u && *s) {
            cp = (uint32_t)(c & 0x1Fu) << 6;
            cp |= (uint32_t)((unsigned char)*s++ & 0x3Fu);
        } else if ((c & 0xF0u) == 0xE0u && s[0] && s[1]) {
            cp  = (uint32_t)(c & 0x0Fu) << 12;
            cp |= (uint32_t)((unsigned char)s[0] & 0x3Fu) << 6;
            cp |= (uint32_t)((unsigned char)s[1] & 0x3Fu);
            s += 2;
        } else {
            cp = '?';                                  /* 非 BMP/非法一律 '?' */
            while (*s && ((unsigned char)*s & 0xC0u) == 0x80u) ++s;
        }
        out[o++] = (uint8_t)((cp >> 8) & 0xFFu);
        out[o++] = (uint8_t)(cp & 0xFFu);
    }
    return o;
}

int sms_concat_build_tx(const char *sca, const char *dst, const char *utf8_text,
                        sms_tx_pkt_t *pkts, int max_pkts)
{
    uint8_t  sca_bcd[12];
    uint8_t  sca_len = 0, sca_toa = 0x81u;
    uint8_t  da_bcd[12];
    uint8_t  da_len = 0, da_toa = 0x81u;
    /*
     * ★ 2400 字节的 UCS2 展开缓冲必须放静态区：任务有效栈只有 3072 字节
     *   （sdk_task_create 钳制），本函数由短信任务在"回执发送"路径上调用。
     *   本模块状态（s_slot / s_tx_ref）本来就是静态的，不引入新的重入风险。
     */
    static uint8_t ucs2[2u * SMS_CONCAT_MAX_TEXT];
    unsigned ucs2_len;
    unsigned total;
    unsigned idx;
    unsigned sca_o = 0;
    unsigned sca_oct;

    if (utf8_text == NULL || pkts == NULL || max_pkts <= 0) return -1;

    if (addr_encode(dst, &da_len, &da_toa, da_bcd, sizeof(da_bcd)) != 0) return -1;
    if (sca != NULL && sca[0] != '\0') {
        if (addr_encode(sca, &sca_len, &sca_toa, sca_bcd, sizeof(sca_bcd)) != 0) return -1;
    }

    ucs2_len = utf8_to_ucs2(utf8_text, ucs2, sizeof(ucs2));
    if (ucs2_len == 0u) return -1;

    total = (ucs2_len + 2u * SMS_TX_UCS2_PER_PART - 1u) / (2u * SMS_TX_UCS2_PER_PART);
    if (total == 0u) total = 1u;
    if (total > SMS_TX_MAX_PARTS) return -2;
    if ((int)total > max_pkts)     return -2;

    for (idx = 0; idx < total; ++idx) {
        char    *o = pkts[idx].pdu;
        size_t   pos = 0;
        unsigned off = idx * 2u * SMS_TX_UCS2_PER_PART;
        unsigned remain = ucs2_len - off;
        unsigned body = (remain > 2u * SMS_TX_UCS2_PER_PART) ? (2u * SMS_TX_UCS2_PER_PART) : remain;
        unsigned ud_octets = 6u + body;      /* UDHL(1)+UDH(5) + 正文 */

        /* 1) SMSC */
        if (sca_len == 0u) {
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        } else {
            sca_oct = 1u + (sca_len + 1u) / 2u;
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)sca_oct) != 0) return -1;
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, sca_toa) != 0) return -1;
            while (sca_o < ((sca_len + 1u) / 2u)) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, sca_bcd[sca_o++]) != 0) return -1;
            }
        }
        /* 2) 首字节：SUBMIT + UDHI，无 VP/SR  => 0x41 */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x41u) != 0) return -1;
        /* 3) MR */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        /* 4) DA */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_len) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_toa) != 0) return -1;
        {
            unsigned k;
            for (k = 0; k < ((da_len + 1u) / 2u); ++k) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_bcd[k]) != 0) return -1;
            }
        }
        /* 5) PID / DCS(UCS2) / UDL */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x08u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)ud_octets) != 0) return -1;
        /* 6) UDH: 05 00 03 ref total seq */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x05u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x03u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, s_tx_ref) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)total) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)(idx + 1u)) != 0) return -1;
        /* 7) 正文（UCS2） */
        {
            unsigned k;
            for (k = 0; k < body; ++k) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, ucs2[off + k]) != 0) return -1;
            }
        }
        o[pos] = '\0';
        pkts[idx].seq   = (uint8_t)(idx + 1u);
        pkts[idx].total = (uint8_t)total;
    }

    ++s_tx_ref;                        /* 参考号递增 */
    return (int)total;
}

int sms_concat_parse_csca(const char *at_resp, char *out, size_t out_size)
{
    const char *p;
    size_t      o  = 0;
    int         ok = 0;

    if (out == NULL || out_size == 0u) return -1;
    out[0] = '\0';
    if (at_resp == NULL) return -1;

    /* 定位 "+CSCA:" */
    p = strstr(at_resp, "+CSCA:");
    if (p == NULL) return -1;
    p += 6;
    while (*p == ' ' || *p == '\t') ++p;

    /* 号码可能在引号内 */
    if (*p == '"') ++p;

    if (*p == '+') {
        if (o + 1u >= out_size) return -1;      /* 缓冲不足：out 仍是空串 */
        out[o++] = *p++;
    }
    while (*p >= '0' && *p <= '9') {
        if (o + 1u >= out_size) { ok = 0; o = 0; break; }   /* 截断即视为失败 */
        out[o++] = *p++;
        ok = 1;
    }
    out[o] = '\0';

    return (ok && o > 0u) ? 0 : -1;      /* 空号码（用模组默认）也算失败 */
}

int sms_concat_build_tx_plain(const char *sca, const char *dst, const char *utf8_text,
                              sms_tx_pkt_t *pkts, int max_pkts)
{
    uint8_t  sca_bcd[12];
    uint8_t  sca_len = 0, sca_toa = 0x81u;
    uint8_t  da_bcd[12];
    uint8_t  da_len = 0, da_toa = 0x81u;
    static uint8_t ucs2[2u * SMS_CONCAT_MAX_TEXT];
    unsigned ucs2_len;
    unsigned per_part_octets = 2u * SMS_TX_PLAIN_PER_PART;   /* 140 字节 */
    unsigned total;
    unsigned idx;
    unsigned sca_o = 0;
    unsigned sca_oct;

    if (utf8_text == NULL || pkts == NULL || max_pkts <= 0) return -1;

    if (addr_encode(dst, &da_len, &da_toa, da_bcd, sizeof(da_bcd)) != 0) return -1;
    if (sca != NULL && sca[0] != '\0') {
        if (addr_encode(sca, &sca_len, &sca_toa, sca_bcd, sizeof(sca_bcd)) != 0) return -1;
    }

    ucs2_len = utf8_to_ucs2(utf8_text, ucs2, sizeof(ucs2));
    if (ucs2_len == 0u) return -1;

    total = (ucs2_len + per_part_octets - 1u) / per_part_octets;
    if (total == 0u) total = 1u;
    if ((int)total > max_pkts) return -2;

    for (idx = 0; idx < total; ++idx) {
        char    *o = pkts[idx].pdu;
        size_t   pos = 0;
        unsigned off = idx * per_part_octets;
        unsigned remain = ucs2_len - off;
        unsigned body = (remain > per_part_octets) ? per_part_octets : remain;

        /* 1) SMSC */
        if (sca_len == 0u) {
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        } else {
            sca_oct = 1u + (sca_len + 1u) / 2u;
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)sca_oct) != 0) return -1;
            if (hex_put(o, SMS_PDU_MAX_HEX, &pos, sca_toa) != 0) return -1;
            while (sca_o < ((sca_len + 1u) / 2u)) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, sca_bcd[sca_o++]) != 0) return -1;
            }
        }
        /* 2) 首字节 0x01：SUBMIT，**无 UDHI**（独立短信，不带 UDH） */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x01u) != 0) return -1;
        /* 3) MR */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)(s_tx_ref + idx)) != 0) return -1;
        /* 4) DA */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_len) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_toa) != 0) return -1;
        {
            unsigned k;
            for (k = 0; k < ((da_len + 1u) / 2u); ++k) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, da_bcd[k]) != 0) return -1;
            }
        }
        /* 5) PID / DCS(UCS2) / UDL */
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x00u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, 0x08u) != 0) return -1;
        if (hex_put(o, SMS_PDU_MAX_HEX, &pos, (uint8_t)body) != 0) return -1;
        /* 6) 正文（UCS2），直接跟在 UDL 之后（无 UDH） */
        {
            unsigned k;
            for (k = 0; k < body; ++k) {
                if (hex_put(o, SMS_PDU_MAX_HEX, &pos, ucs2[off + k]) != 0) return -1;
            }
        }
        o[pos] = '\0';
        pkts[idx].seq   = (uint8_t)(idx + 1u);
        pkts[idx].total = (uint8_t)total;
    }

    s_tx_ref = (uint8_t)(s_tx_ref + total);
    return (int)total;
}

int sms_concat_selftest(void) { return 0; }