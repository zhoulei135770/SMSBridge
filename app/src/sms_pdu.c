/**
 * @file    sms_pdu.c
 * @brief   短信 PDU 解码实现（GSM 03.40 / 03.38）。纯 C99，无 SDK 依赖。
 */
#include "sms_pdu.h"

#include <string.h>

/* ================================================================== */
/* GSM 03.38 默认字母表：septet -> Unicode 码点                         */
/* 0 表示该位置无定义                                                   */
/* ================================================================== */
static const uint16_t s_gsm7_basic[128] = {
/*0x00*/ 0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
/*0x08*/ 0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
/*0x10*/ 0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
/*0x18*/ 0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
/*0x20*/ 0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
/*0x28*/ 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
/*0x30*/ 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
/*0x38*/ 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
/*0x40*/ 0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
/*0x48*/ 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
/*0x50*/ 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
/*0x58*/ 0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
/*0x60*/ 0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
/*0x68*/ 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
/*0x70*/ 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
/*0x78*/ 0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0
};

/* GSM 03.38 扩展表：0x1B 之后紧跟的 septet -> Unicode 码点 */
typedef struct { uint8_t septet; uint16_t code; } gsm7_ext_entry_t;

static const gsm7_ext_entry_t s_gsm7_ext[] = {
    { 0x0A, 0x000C },  /* 换页            */
    { 0x14, 0x005E },  /* ^               */
    { 0x28, 0x007B },  /* {               */
    { 0x29, 0x007D },  /* }               */
    { 0x2F, 0x005C },  /* \               */
    { 0x3C, 0x005B },  /* [               */
    { 0x3D, 0x007E },  /* ~               */
    { 0x3E, 0x005D },  /* ]               */
    { 0x40, 0x007C },  /* |               */
    { 0x65, 0x20AC }   /* €               */
};

#define GSM7_EXT_COUNT (sizeof(s_gsm7_ext) / sizeof(s_gsm7_ext[0]))

/* ================================================================== */
/* 小工具                                                              */
/* ================================================================== */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief 把十六进制串（允许空格/冒号/换行分隔）转成字节数组。
 * @return 字节数，-1 表示非法。
 */
static int hex_to_bin(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t n = 0;
    int    hi = -1;

    if (hex == NULL || out == NULL) return -1;

    for (; *hex != '\0'; ++hex) {
        int v;
        char c = *hex;
        if (c == ' ' || c == ':' || c == '\r' || c == '\n' || c == '\t' || c == ',') continue;
        v = hex_val(c);
        if (v < 0) return -1;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= out_cap) return -1;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return -1; /* 奇数个半字节 */
    return (int)n;
}

/** 把一个 Unicode 码点以 UTF-8 写入 dst，返回写入字节数；空间不足返回 0。 */
static size_t utf8_put(uint32_t cp, char *dst, size_t dst_size)
{
    if (cp == 0) return 0;
    if (dst_size < 5) return 0;

    if (cp < 0x80u) {
        dst[0] = (char)cp;
        dst[1] = '\0';
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        dst[2] = '\0';
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        dst[3] = '\0';
        return 3;
    }
    if (cp < 0x110000u) {
        dst[0] = (char)(0xF0u | (cp >> 18));
        dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        dst[4] = '\0';
        return 4;
    }
    return 0; /* 非法码点（UCS2 代理区在调用方处理） */
}

/**
 * @brief 从字节流的 bitoff 位置取出 7 个 bit（GSM 7bit 解包）。
 * @return 0 成功，-1 越界。
 */
static int get_septet(const uint8_t *d, size_t len, size_t bitoff, uint8_t *out)
{
    size_t   byte  = bitoff / 8u;
    unsigned shift = (unsigned)(bitoff % 8u);
    uint32_t v;

    if (byte >= len) return -1;
    v = d[byte];
    if (shift > 1u) {                 /* 需要跨字节 */
        if (byte + 1u >= len) return -1;
        v |= ((uint32_t)d[byte + 1u]) << 8;
    }
    *out = (uint8_t)((v >> shift) & 0x7Fu);
    return 0;
}

/** 把 1 个 septet（含 0x1B 转义）转 UTF-8，返回写入字节数；非法返回 0。 */
size_t sms_gsm7_to_utf8(uint8_t septet, char *dst, size_t dst_size)
{
    if (septet >= 128u) return 0;
    return utf8_put(s_gsm7_basic[septet], dst, dst_size);
}

/** 查扩展表，返回码点，0 表示无定义。 */
static uint16_t gsm7_ext_lookup(uint8_t septet)
{
    size_t i;
    for (i = 0; i < GSM7_EXT_COUNT; ++i) {
        if (s_gsm7_ext[i].septet == septet) return s_gsm7_ext[i].code;
    }
    return 0;
}

/* ================================================================== */
/* 字段解析                                                            */
/* ================================================================== */

/**
 * @brief 解析 TP-地址字段（TP-OA / TP-SCA 共用）。
 * @param p        指向长度字节
 * @param avail    可用字节数
 * @param consumed 输出：本字段占用字节数
 */
static sms_pdu_status_t decode_address(const uint8_t *p, size_t avail,
                                       size_t *consumed,
                                       char *out, size_t out_size,
                                       uint8_t *ton_out)
{
    uint8_t addr_len;      /* 半字节数（字母数字号码时为 septet 数） */
    uint8_t toa;
    size_t  octets, i, n = 0;
    uint8_t ton;

    if (avail < 1u) return SMS_PDU_ERR_TRUNC;

    addr_len = p[0];
    if (addr_len == 0u) {
        if (out != NULL && out_size > 0u) out[0] = '\0';
        if (ton_out != NULL) *ton_out = 0u;
        *consumed = 1u;
        return SMS_PDU_OK;
    }
    if (avail < 2u) return SMS_PDU_ERR_TRUNC;

    toa = p[1];
    ton = (uint8_t)((toa >> 4) & 0x07u);
    if (ton_out != NULL) *ton_out = ton;

    if (ton == 5u) {
        /* 字母数字号码：号码本身也是 7bit 打包的 */
        size_t septets = addr_len;
        size_t bitoff  = 0;
        octets = (septets * 7u + 7u) / 8u;
        if (2u + octets > avail) return SMS_PDU_ERR_TRUNC;
        while (septets-- > 0u) {
            uint8_t s;
            char    tmp[8];
            size_t  w;
            if (n + 8u >= out_size) return SMS_PDU_ERR_OVERFLOW;
            if (get_septet(&p[2], octets, bitoff, &s) != 0) break;
            bitoff += 7u;
            w = sms_gsm7_to_utf8(s, tmp, sizeof(tmp));
            if (w == 0u) continue;
            memcpy(&out[n], tmp, w);
            n += w;
        }
        out[n] = '\0';
        /* 字段布局：[长度][TON/NPI][号码字节]，共 2 + octets */
        *consumed = 2u + octets;
        return SMS_PDU_OK;
    }

    octets = (size_t)((addr_len + 1u) / 2u);
    if (2u + octets > avail) return SMS_PDU_ERR_TRUNC;

    if (ton == 1u && out_size > 1u) out[n++] = '+';

    for (i = 0; i < octets; ++i) {
        uint8_t b = p[2 + i];
        uint8_t lo = (uint8_t)(b & 0x0Fu);
        uint8_t hi = (uint8_t)((b >> 4) & 0x0Fu);

        if (n + 1u >= out_size) return SMS_PDU_ERR_OVERFLOW;
        if (lo < 10u) out[n++] = (char)('0' + lo);
        else if (lo != 0x0Fu) out[n++] = '?';   /* 非 BCD 数字，占位 */
        if ((i * 2u + 1u) >= addr_len) break;   /* 处理奇数长度 */
        if (n + 1u >= out_size) return SMS_PDU_ERR_OVERFLOW;
        if (hi < 10u) out[n++] = (char)('0' + hi);
        else if (hi != 0x0Fu) out[n++] = '?';
    }
    out[n] = '\0';
    /* 关键：字段布局是 [长度字节][TON/NPI 字节][号码字节...]，
     * 所以总共占用 2 + octets 字节。这里曾经漏算 TON/NPI 字节，
     * 导致 SCA 与 TP-OA 各少前进 1 字节、后续全体字段错位 2 字节
     * （表现为 DCS 读到垃圾值报 "unsupported dcs"）。主机单元测试抓出。 */
    *consumed = 2u + octets;
    return SMS_PDU_OK;
}

/** 解析 TP-SCTS 7 字节时间戳。 */
static sms_pdu_status_t decode_scts(const uint8_t *p, size_t avail,
                                    char *out, size_t out_size,
                                    int16_t *tz_quarters_out, int8_t *tz_valid_out)
{
    uint8_t  d[7];
    unsigned i;
    int      year, mon, day, hour, min, sec, tz;
    int      tz_neg = 0;

    if (avail < 7u) return SMS_PDU_ERR_TRUNC;

    /*
     * 前 6 个字节（YY MM DD HH MM SS）必须是 BCD 数字；
     * 第 7 个字节是时区，其**低半字节含符号位**（合法值可为 8~F），
     * 不能按 BCD 数字校验 —— 早期版本把 7 个字节一起校验，导致负时区
     * （例如 -5 区 = 0x0A）被误报为 "invalid hex string"。
     * 主机单元测试 scts_tz_negative 抓出此缺陷。
     */
    for (i = 0; i < 6u; ++i) {
        uint8_t b = p[i];
        unsigned lo = (unsigned)(b & 0x0Fu);
        unsigned hi = (unsigned)((b >> 4) & 0x0Fu);
        if (lo > 9u || hi > 9u) return SMS_PDU_ERR_HEX;
        d[i] = (uint8_t)(lo * 10u + hi);
    }

    year = d[0]; mon = d[1]; day = d[2];
    hour = d[3]; min = d[4]; sec = d[5];

    /* 时区：bit3 of 低半字节为符号位 */
    {
        uint8_t tzb = p[6];
        unsigned lo = (unsigned)(tzb & 0x0Fu);
        unsigned hi = (unsigned)((tzb >> 4) & 0x0Fu);
        tz_neg = (lo & 0x08u) ? 1 : 0;
        tz = (int)((lo & 0x07u) * 10u + hi);
    }

    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        /* 基站下发异常时不整体失败，只标记时间无效 */
        if (out != NULL && out_size > 0u) out[0] = '\0';
        if (tz_quarters_out != NULL) *tz_quarters_out = 0;
        if (tz_valid_out != NULL) *tz_valid_out = 0;
        return SMS_PDU_OK;
    }

    year += (year >= 70) ? 1900 : 2000;

    if (out != NULL && out_size > 0u) {
        /* 手工格式化，避免依赖 snprintf 的 C99 库实现差异 */
        char *w = out;
        size_t left = out_size;
        const int vals[6] = { year, mon, day, hour, min, sec };
        const char *sep[6] = { "-", "-", " ", ":", ":", "" };
        int k;
        for (k = 0; k < 6; ++k) {
            unsigned v = (unsigned)vals[k];
            if (left < 6u) { out[out_size - 1u] = '\0'; return SMS_PDU_ERR_OVERFLOW; }
            if (k == 0) {
                *w++ = (char)('0' + (v / 1000u) % 10u);
                *w++ = (char)('0' + (v / 100u) % 10u);
            }
            *w++ = (char)('0' + (v / 10u) % 10u);
            *w++ = (char)('0' + v % 10u);
            *w++ = *sep[k];
            left -= 4u;
        }
        *w = '\0';
    }
    if (tz_quarters_out != NULL) *tz_quarters_out = (int16_t)(tz_neg ? -tz : tz);
    if (tz_valid_out != NULL) *tz_valid_out = 1;
    return SMS_PDU_OK;
}

/** 解析 TP-UDH，识别长短信级联 IE。 */
static void parse_udh(sms_pdu_t *o, const uint8_t *udh, uint8_t udh_len)
{
    uint8_t i = 0;

    if (udh_len > SMS_UDH_MAX_OCTETS) udh_len = SMS_UDH_MAX_OCTETS;
    memcpy(o->udh, udh, udh_len);
    o->udh_len = udh_len;

    while (i + 1u < udh_len) {
        uint8_t iei = udh[i];
        uint8_t iedl = udh[i + 1u];
        const uint8_t *ied = &udh[i + 2u];

        if ((size_t)i + 2u + iedl > (size_t)udh_len) break;   /* 头部不完整 */

        if (iei == 0x00u && iedl == 3u) {          /* 8bit 级联参考号 */
            o->concat.present  = 1u;
            o->concat.is_16bit = 0u;
            o->concat.ref      = ied[0];
            o->concat.total    = ied[1];
            o->concat.seq      = ied[2];
        } else if (iei == 0x08u && iedl == 4u) {   /* 16bit 级联参考号 */
            o->concat.present  = 1u;
            o->concat.is_16bit = 1u;
            o->concat.ref      = (uint16_t)(((uint16_t)ied[0] << 8) | ied[1]);
            o->concat.total    = ied[2];
            o->concat.seq      = ied[3];
        }
        i = (uint8_t)(i + 2u + iedl);
    }
}

/** UCS2(UTF-16BE) -> UTF-8，含代理对处理。 */
static sms_pdu_status_t ucs2_to_utf8(const uint8_t *d, size_t len, sms_pdu_t *o)
{
    size_t i = 0;
    size_t n = 0;

    if ((len & 1u) != 0u) len -= 1u;   /* 丢弃半个码元 */

    while (i + 1u < len) {
        uint32_t cp = ((uint32_t)d[i] << 8) | d[i + 1u];
        size_t   w;
        char     tmp[8];

        i += 2u;

        if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1u) < len) {
            uint32_t lo = ((uint32_t)d[i] << 8) | d[i + 1u];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2u;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            continue;   /* 孤立低代理，丢弃 */
        }

        w = utf8_put(cp, tmp, sizeof(tmp));
        if (w == 0u) continue;
        if (n + w + 1u > sizeof(o->text)) return SMS_PDU_ERR_OVERFLOW;
        memcpy(&o->text[n], tmp, w);
        n += w;
    }
    o->text[n] = '\0';
    o->text_len = n;
    return SMS_PDU_OK;
}

/** 7bit 字母表 -> UTF-8。 */
static sms_pdu_status_t gsm7_to_utf8(const uint8_t *d, size_t byte_len,
                                     size_t skip_septets, size_t septets,
                                     sms_pdu_t *o)
{
    size_t n = 0;
    size_t k;
    size_t max_bits = byte_len * 8u;

    for (k = 0; k < septets; ++k) {
        size_t  bitoff = (skip_septets + k) * 7u;
        uint8_t s;
        char    tmp[8];
        size_t  w;

        if (bitoff + 7u > max_bits) break;                 /* 数据不足，停止 */
        if (get_septet(d, byte_len, bitoff, &s) != 0) break;

        if (s == 0x1Bu) {                                  /* 扩展转义 */
            uint16_t code;
            if (k + 1u >= septets) break;
            ++k;
            bitoff += 7u;
            if (bitoff + 7u > max_bits) break;
            if (get_septet(d, byte_len, bitoff, &s) != 0) break;
            code = gsm7_ext_lookup(s);
            if (code == 0u) code = s_gsm7_basic[s];        /* 未定义扩展：退回基本表 */
            w = utf8_put(code, tmp, sizeof(tmp));
        } else {
            w = sms_gsm7_to_utf8(s, tmp, sizeof(tmp));
        }
        if (w == 0u) continue;
        if (n + w + 1u > sizeof(o->text)) return SMS_PDU_ERR_OVERFLOW;
        memcpy(&o->text[n], tmp, w);
        n += w;
    }
    o->text[n] = '\0';
    o->text_len = n;
    return SMS_PDU_OK;
}

/** 由 TP-DCS 推导字母表。 */
static sms_alpha_t dcs_to_alpha(uint8_t dcs, int *ok)
{
    *ok = 1;
    if ((dcs & 0xC0u) == 0x00u) {
        switch ((dcs >> 2) & 0x03u) {
        case 0: return SMS_ALPHA_7BIT;
        case 1: return SMS_ALPHA_8BIT;
        case 2: return SMS_ALPHA_UCS2;
        default: *ok = 0; return SMS_ALPHA_RESERVED;
        }
    }
    if ((dcs & 0xF0u) == 0xF0u) {          /* data coding / message class */
        switch ((dcs >> 2) & 0x03u) {
        case 0: return SMS_ALPHA_7BIT;
        case 1: return SMS_ALPHA_8BIT;
        case 2: return SMS_ALPHA_UCS2;
        default: *ok = 0; return SMS_ALPHA_RESERVED;
        }
    }
    if ((dcs & 0xF0u) == 0xC0u || (dcs & 0xF0u) == 0xD0u) return SMS_ALPHA_7BIT;  /* MWI */
    if ((dcs & 0xF0u) == 0xE0u) return SMS_ALPHA_UCS2;                            /* MWI UCS2 */
    *ok = 0;
    return SMS_ALPHA_RESERVED;
}

/**
 * @brief 解析 TP-SCA。
 *
 * ⚠ 关键规范点：TP-SCA 的长度字节是**八位组数（octets，含 TOA 字节）**，
 * 与 TP-OA 的"半字节数（数字位数）"语义**不同**。混用会让后续所有字段
 * 错位，最终表现为"DCS 不支持 / 正文乱码"。
 * 依据：3GPP TS 23.040 §9.1.2.1，以及真实现网 PDU 例
 *   07917283010010F5...  → 07 = 7 个八位组（TOA + 6 字节 BCD = 12 位号码）
 */
static sms_pdu_status_t decode_sca(const uint8_t *p, size_t avail,
                                   size_t *consumed,
                                   char *out, size_t out_size)
{
    uint8_t addr_octets;
    uint8_t toa;
    uint8_t ton;
    size_t  digit_octets, i, n = 0;

    if (avail < 1u) return SMS_PDU_ERR_TRUNC;

    addr_octets = p[0];
    if (out != NULL && out_size > 0u) out[0] = '\0';

    if (addr_octets == 0u) {
        *consumed = 1u;
        return SMS_PDU_OK;
    }
    if ((size_t)addr_octets + 1u > avail) return SMS_PDU_ERR_TRUNC;

    toa = p[1];
    ton = (uint8_t)((toa >> 4) & 0x07u);
    digit_octets = (size_t)addr_octets - 1u;   /* 去掉 TOA 字节 */

    if (out != NULL) {
        if (ton == 1u && out_size > 1u) out[n++] = '+';
        for (i = 0; i < digit_octets; ++i) {
            uint8_t b  = p[2 + i];
            uint8_t lo = (uint8_t)(b & 0x0Fu);
            uint8_t hi = (uint8_t)((b >> 4) & 0x0Fu);

            if (n + 1u >= out_size) return SMS_PDU_ERR_OVERFLOW;
            if (lo < 10u) out[n++] = (char)('0' + lo);
            else if (lo != 0x0Fu) out[n++] = '?';

            if (n + 1u >= out_size) return SMS_PDU_ERR_OVERFLOW;
            if (hi < 10u) out[n++] = (char)('0' + hi);
            else if (hi != 0x0Fu) out[n++] = '?';
        }
        out[n] = '\0';
    }

    *consumed = (size_t)addr_octets + 1u;
    return SMS_PDU_OK;
}

/* ================================================================== */
/* 主入口                                                              */
/* ================================================================== */

const char *sms_pdu_strerror(sms_pdu_status_t st)
{
    switch (st) {
    case SMS_PDU_OK:           return "ok";
    case SMS_PDU_ERR_ARG:      return "invalid argument";
    case SMS_PDU_ERR_HEX:      return "invalid hex string";
    case SMS_PDU_ERR_TRUNC:    return "truncated pdu";
    case SMS_PDU_ERR_MTI:      return "not sms-deliver";
    case SMS_PDU_ERR_DCS:      return "unsupported dcs";
    case SMS_PDU_ERR_OVERFLOW: return "output buffer overflow";
    default:                   return "unknown error";
    }
}

sms_pdu_status_t sms_pdu_decode(const char *hex, sms_pdu_t *out)
{
    uint8_t  buf[SMS_PDU_MAX_HEX / 2];
    int      len;
    size_t   pos = 0;
    size_t   consumed = 0;
    uint8_t  first_octet;
    uint8_t  udl;
    size_t   ud_off;
    sms_pdu_status_t st;
    int      dcs_ok = 0;

    if (hex == NULL || out == NULL) return SMS_PDU_ERR_ARG;

    memset(out, 0, sizeof(*out));

    len = hex_to_bin(hex, buf, sizeof(buf));
    if (len < 0) return SMS_PDU_ERR_HEX;
    if (len < 2) return SMS_PDU_ERR_TRUNC;

    /* --- SCA：注意长度语义与 TP-OA 不同（八位组数 vs 半字节数） --- */
    st = decode_sca(&buf[pos], (size_t)len - pos, &consumed,
                    out->sca, sizeof(out->sca));
    if (st != SMS_PDU_OK) return st;
    pos += consumed;

    /* --- 首字节：TP-MTI 等 --- */
    if (pos >= (size_t)len) return SMS_PDU_ERR_TRUNC;
    first_octet = buf[pos++];

    if ((first_octet & 0x03u) != 0x00u) {
        /* 0b00 = SMS-DELIVER；0b10 = 状态报告；0b01 = SMS-SUBMIT(本机发出) */
        return SMS_PDU_ERR_MTI;
    }
    out->udhi = (uint8_t)((first_octet >> 6) & 0x01u);

    /* --- TP-OA --- */
    st = decode_address(&buf[pos], (size_t)len - pos, &consumed,
                        out->number, sizeof(out->number), &out->ton);
    if (st != SMS_PDU_OK) return st;
    pos += consumed;

    /* --- TP-PID --- */
    if (pos >= (size_t)len) return SMS_PDU_ERR_TRUNC;
    pos += 1u;

    /* --- TP-DCS --- */
    if (pos >= (size_t)len) return SMS_PDU_ERR_TRUNC;
    out->dcs   = buf[pos++];
    out->alpha = dcs_to_alpha(out->dcs, &dcs_ok);
    if (!dcs_ok) return SMS_PDU_ERR_DCS;

    /* --- TP-SCTS --- */
    st = decode_scts(&buf[pos], (size_t)len - pos,
                     out->scts, sizeof(out->scts),
                     &out->tz_quarters, &out->tz_valid);
    if (st != SMS_PDU_OK) return st;
    pos += 7u;

    /* --- TP-UDL --- */
    if (pos >= (size_t)len) return SMS_PDU_ERR_TRUNC;
    udl = buf[pos++];
    out->udl = udl;

    /* --- TP-UD --- */
    ud_off = pos;
    out->raw_ud_len = (uint16_t)((size_t)len - ud_off);
    if (out->raw_ud_len > SMS_RAW_UD_MAX_OCTETS) {
        out->raw_ud_len = SMS_RAW_UD_MAX_OCTETS;
    }
    if (out->raw_ud_len > 0u) {
        memcpy(out->raw_ud, &buf[ud_off], out->raw_ud_len);
    }

    {
        const uint8_t *ud = &buf[ud_off];
        size_t         ud_bytes = (size_t)len - ud_off;
        size_t         skip_septets = 0;
        size_t         data_off = 0;

        if (out->udhi) {
            uint8_t udhl;
            if (ud_bytes < 1u) return SMS_PDU_ERR_TRUNC;
            udhl = ud[0];
            if ((size_t)udhl + 1u > ud_bytes) return SMS_PDU_ERR_TRUNC;
            parse_udh(out, &ud[1], udhl);
            data_off = (size_t)udhl + 1u;
            /* UDH 占用整 septet 数（含填充位） */
            skip_septets = (data_off * 8u + 6u) / 7u;
        }

        switch (out->alpha) {
        case SMS_ALPHA_7BIT: {
            size_t septets = (size_t)udl;
            if (skip_septets > septets) return SMS_PDU_ERR_TRUNC;
            /* 同上：按 UDL 声明的 septet 数换算所需比特，数据不够就是被截断 */
            if (septets * 7u > ud_bytes * 8u) return SMS_PDU_ERR_TRUNC;
            st = gsm7_to_utf8(ud, ud_bytes, skip_septets,
                              septets - skip_septets, out);
            if (st != SMS_PDU_OK) return st;
            break;
        }
        case SMS_ALPHA_UCS2: {
            size_t octets;
            size_t avail  = (data_off <= ud_bytes) ? (ud_bytes - data_off) : 0u;

            /*
             * ★ TP-UDL 对 UCS2/8bit 是**整个 TP-UD 的字节数（含 UDH）**，
             *   所以判据是 `UDL > 实际到手的 UD 字节数` ⇒ PDU 被截断。
             *   2026-10-15 上板事故：虚拟 AT 分块投递只读回 82 个十六进制字符
             *   （UDL=140、正文只剩 6 字节），旧代码把正文长度"夹到"实际字节数，
             *   于是把碎片当成合法短信 "U h" 执行（还把 Webhook URL 改成了 "h1"）。
             */
            if ((size_t)udl > ud_bytes) return SMS_PDU_ERR_TRUNC;
            if (data_off > (size_t)udl)  return SMS_PDU_ERR_TRUNC;
            octets = (size_t)udl - data_off;
            (void)avail;
            st = ucs2_to_utf8(&ud[data_off], octets, out);
            if (st != SMS_PDU_OK) return st;
            break;
        }
        case SMS_ALPHA_8BIT:
        default: {
            /* 数据短信：不作文本转换，只保留 raw_ud，text 置空 */
            out->text[0] = '\0';
            out->text_len = 0u;
            break;
        }
        }
    }

    return SMS_PDU_OK;
}
