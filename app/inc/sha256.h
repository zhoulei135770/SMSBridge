/**
 * @file    sha256.h
 * @brief   极简 SHA-256（用于 Web 登录口令摘要，避免在 Flash 中明文存储口令）。
 *          纯 C99，无 SDK 依赖，可用已知测试向量在主机上验证。
 */
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE    65   /* 64 hex + '\0' */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/** 一次性计算并输出小写十六进制摘要（out 至少 SHA256_HEX_SIZE 字节）。 */
void sha256_hex(const void *data, size_t len, char *out);

/** 常量时间比较两个十六进制摘要（防时序侧信道，返回 1 相等）。 */
int  sha256_hex_equal(const char *a, const char *b);

/* ---- 以下为钉钉加签所需：HMAC-SHA256 / Base64 / URL 编码 ---- */
#define HMAC_SHA256_SIZE 32
/** Base64 编码后的最大长度（含 '\0'） */
#define BASE64_MAX_LEN(len) ((((len) + 2u) / 3u) * 4u + 1u)

/** RFC2104 HMAC-SHA256，out 需 32 字节 */
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[HMAC_SHA256_SIZE]);

/** 标准 Base64（带 '=' 填充），返回写入长度，<0 失败 */
int base64_encode(const void *data, size_t len, char *out, size_t out_size);

/** RFC3986 百分号编码（非 unreserved 全部编码），返回写入长度，<0 失败 */
int url_encode(const char *src, char *out, size_t out_size);

/**
 * @brief 钉钉"加签"串：urlencode(base64(HMAC_SHA256(secret, ts_ms + "\n" + secret)))
 * @return 写入长度，<0 失败；out 建议 >= 128 字节
 */
int dingtalk_sign(const char *secret, uint64_t ts_ms, char *out, size_t out_size);
#endif /* SHA256_H */
