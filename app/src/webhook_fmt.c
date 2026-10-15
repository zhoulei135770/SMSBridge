/**
 * @file    webhook_fmt.c
 * @brief   Webhook 请求组装实现（见 inc/webhook_fmt.h）。
 */
#include "webhook_fmt.h"
#include "sha256.h"
#include <string.h>

/* 往输出缓冲追加，返回新的写入长度，溢出返回 -1 */
static int app(char *out, size_t out_size, size_t *pos, const char *s, size_t n)
{
    if (*pos + n + 1u > out_size) return -1;
    memcpy(out + *pos, s, n);
    *pos += n;
    out[*pos] = '\0';
    return 0;
}

static int app_str(char *out, size_t out_size, size_t *pos, const char *s)
{
    if (s == NULL) return 0;
    return app(out, out_size, pos, s, strlen(s));
}

/* 数字 -> 十进制 */
static int app_uint(char *out, size_t out_size, size_t *pos, unsigned v)
{
    char tmp[12];
    int  i = 0, j;
    if (v == 0u) tmp[i++] = '0';
    while (v > 0u && i < 11) { tmp[i++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    for (j = i - 1; j >= 0; --j) {
        if (app(out, out_size, pos, &tmp[j], 1u) != 0) return -1;
    }
    return 0;
}

int webhook_render_template(const char *tmpl,
                            const char *from, const char *time_s,
                            const char *text, const char *pdu, unsigned idx,
                            char *out, size_t out_size)
{
    size_t pos = 0;
    const char *p;

    if (out == NULL || out_size == 0u) return -1;
    out[0] = '\0';
    if (tmpl == NULL) return 0;

    p = tmpl;
    while (*p != '\0') {
        if (strncmp(p, "{from}", 6) == 0)      { if (app_str(out, out_size, &pos, from)   != 0) return -1; p += 6; continue; }
        if (strncmp(p, "{time}", 6) == 0)      { if (app_str(out, out_size, &pos, time_s) != 0) return -1; p += 6; continue; }
        if (strncmp(p, "{text}", 6) == 0)      { if (app_str(out, out_size, &pos, text)   != 0) return -1; p += 6; continue; }
        if (strncmp(p, "{pdu}", 5) == 0)       { if (app_str(out, out_size, &pos, pdu)    != 0) return -1; p += 5; continue; }
        if (strncmp(p, "{idx}", 5) == 0)       { if (app_uint(out, out_size, &pos, idx)   != 0) return -1; p += 5; continue; }
        if (app(out, out_size, &pos, p, 1u) != 0) return -1;
        ++p;
    }
    return (int)pos;
}

int webhook_apply_keyword(const char *keyword, const char *body,
                          char *out, size_t out_size)
{
    size_t pos = 0;

    if (out == NULL || out_size == 0u) return -1;
    out[0] = '\0';

    if (keyword != NULL && keyword[0] != '\0') {
        if (app_str(out, out_size, &pos, "【") != 0) return -1;
        if (app_str(out, out_size, &pos, keyword) != 0) return -1;
        if (app_str(out, out_size, &pos, "】") != 0) return -1;
    }
    if (app_str(out, out_size, &pos, body) != 0) return -1;
    return (int)pos;
}

int webhook_build_url(const char *base, const char *secret, uint64_t ts_ms,
                      char *out, size_t out_size)
{
    char   sign[160];
    size_t pos = 0;
    int    has_query;

    if (base == NULL || out == NULL || out_size == 0u) return -1;

    if (secret == NULL || secret[0] == '\0') {
        if (app_str(out, out_size, &pos, base) != 0) return -1;
        return (int)pos;
    }

    if (dingtalk_sign(secret, ts_ms, sign, sizeof(sign)) < 0) return -1;

    has_query = (strchr(base, '?') != NULL) ? 1 : 0;
    if (app_str(out, out_size, &pos, base) != 0) return -1;
    if (app_str(out, out_size, &pos, has_query ? "&timestamp=" : "?timestamp=") != 0) return -1;

    /* 毫秒时间戳十进制 */
    {
        char tmp[24];
        int  i = 0, j;
        uint64_t v = ts_ms;
        if (v == 0u) tmp[i++] = '0';
        while (v > 0u && i < 20) { tmp[i++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
        for (j = i - 1; j >= 0; --j) { if (app(out, out_size, &pos, &tmp[j], 1u) != 0) return -1; }
    }
    if (app_str(out, out_size, &pos, "&sign=") != 0) return -1;
    if (app_str(out, out_size, &pos, sign) != 0) return -1;
    return (int)pos;
}