/**
 * @file    sms_pdu.h
 * @brief   短信 PDU 解码（GSM 03.40 / 03.38），纯 C99，无 SDK 依赖，可主机单元测试。
 *
 * 支持:
 *   - SMS-DELIVER 解析（+CMT URC 上报的 PDU 报文）
 *   - 7bit GSM 默认字母表（含 0x1B 扩展表：^ { } \ [ ] ~ | € 等）
 *   - UCS2 (UTF-16BE) -> UTF-8
 *   - 8bit 数据短信（原样保存，标记为 binary）
 *   - TP-UDHI 用户数据头解析，识别长短信分片信息（03.40 级联 IEI 0x00 / 0x08）
 *     ！仅识别与上报，不做分片重组（见需求文档 6.5：作为后续扩展）
 *
 * 设计约束（对应需求文档「五、非功能需求 5. Buffer 大小严格限制」）:
 *   - 所有输出缓冲区大小固定，任何越界都返回错误码而不是写穿
 *   - 不使用动态内存，不使用 SDK API，可在 PC 上用 gcc 直接编译测试
 */
#ifndef SMS_PDU_H
#define SMS_PDU_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 容量上限（可调，改动后需同步 05-resource-usage.md 的 RAM 估算）      */
/* ------------------------------------------------------------------ */
#define SMS_PDU_MAX_HEX        640   /**< PDU 十六进制串最大长度，含结尾 '\0' */
#define SMS_ADDR_MAX_LEN       24    /**< 号码字符串最大长度，含 '+' 与结尾 '\0' */
#define SMS_TEXT_MAX_BYTES     1024  /**< 解码后 UTF-8 文本上限，含结尾 '\0' */
#define SMS_UDH_MAX_OCTETS     64    /**< 用户数据头原始字节上限 */
#define SMS_RAW_UD_MAX_OCTETS  280   /**< TP-UD 原始字节上限 */

/* ------------------------------------------------------------------ */
/* 状态码                                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    SMS_PDU_OK           =  0,
    SMS_PDU_ERR_ARG      = -1,  /**< 入参为空或缓冲区为空 */
    SMS_PDU_ERR_HEX      = -2,  /**< 十六进制串非法（奇数长度或非法字符） */
    SMS_PDU_ERR_TRUNC    = -3,  /**< PDU 报文被截断 */
    SMS_PDU_ERR_MTI      = -4,  /**< 不是 SMS-DELIVER（如状态报告 / SUBMIT） */
    SMS_PDU_ERR_DCS      = -5,  /**< 不支持的 DCS 编码 */
    SMS_PDU_ERR_OVERFLOW = -6   /**< 输出缓冲区不足 */
} sms_pdu_status_t;

/** 用户数据编码方式（TP-DCS 的字母表位） */
typedef enum {
    SMS_ALPHA_7BIT = 0,   /**< GSM 7bit 默认字母表 */
    SMS_ALPHA_8BIT,       /**< 8bit 数据 */
    SMS_ALPHA_UCS2,       /**< UCS2 / UTF-16BE */
    SMS_ALPHA_RESERVED    /**< 保留 / 不支持 */
} sms_alpha_t;

/** 长短信级联信息（TP-UDH 中的 03.40 级联 IE） */
typedef struct {
    uint8_t  present;   /**< 1 = 本片属于长短信 */
    uint8_t  is_16bit;  /**< 1 = IEI 0x08（16bit 参考号），0 = IEI 0x00（8bit） */
    uint16_t ref;       /**< 级联参考号 */
    uint8_t  total;     /**< 分片总数 */
    uint8_t  seq;       /**< 当前分片序号，从 1 开始 */
} sms_concat_t;

/** 解码结果 */
typedef struct {
    char        sca[SMS_ADDR_MAX_LEN];      /**< 短信中心号码（可为空串） */
    char        number[SMS_ADDR_MAX_LEN];   /**< 对端号码，国际号码带 '+' */
    uint8_t     ton;                        /**< TP-OA 的 TON: 0=未知 1=国际 2=国内 5=字母数字 */
    uint8_t     dcs;                        /**< 原始 TP-DCS 字节 */
    sms_alpha_t alpha;                      /**< 由 DCS 推导出的编码方式 */

    char        scts[32];                   /**< 服务时间戳 "YYYY-MM-DD HH:MM:SS" */
    int16_t     tz_quarters;                /**< 时区，单位 15 分钟（+8 区 = 32） */
    int8_t      tz_valid;

    uint8_t     udhi;                       /**< TP-UDHI 位 */
    uint8_t     udh[SMS_UDH_MAX_OCTETS];    /**< 用户数据头原始字节（不含 UDHL 字节） */
    uint8_t     udh_len;                    /**< 用户数据头长度（不含 UDHL 字节） */
    sms_concat_t concat;

    uint8_t     raw_ud[SMS_RAW_UD_MAX_OCTETS]; /**< TP-UD 原始字节（含 UDH） */
    uint16_t    raw_ud_len;
    uint8_t     udl;                        /**< TP-UDL 原始值（7bit 时为 septet 数） */

    size_t      text_len;                   /**< 解码后文本字节数（不含结尾 '\0'） */
    char        text[SMS_TEXT_MAX_BYTES];   /**< 解码后的 UTF-8 文本 */
} sms_pdu_t;

/**
 * @brief  解码一条 SMS-DELIVER PDU 十六进制串。
 * @param  hex  以 '\0' 结尾的十六进制串，允许含空格/冒号（会自动跳过）。
 *              通常来自 +CMT URC 的第二行。
 * @param  out  输出结构体，成功时被完整填充；失败时内容未定义。
 * @return SMS_PDU_OK 或负错误码。
 * @note   线程安全：不使用任何全局状态。
 */
sms_pdu_status_t sms_pdu_decode(const char *hex, sms_pdu_t *out);

/** @brief 状态码转可读字符串（用于 UART 日志）。 */
const char *sms_pdu_strerror(sms_pdu_status_t st);

/** @brief 把 GSM 7bit 字母表 1 个 septet 转成 UTF-8，返回写入字节数；非法返回 0。 */
size_t sms_gsm7_to_utf8(uint8_t septet, char *dst, size_t dst_size);

#endif /* SMS_PDU_H */
