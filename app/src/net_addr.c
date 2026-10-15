/**
 * @file    net_addr.c
 * @brief   IP 地址解析与判定实现（见 inc/net_addr.h）。
 */
#include "net_addr.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* 判定                                                                */
/* ------------------------------------------------------------------ */
int net_addr_is_ipv4(const char *s)
{
    int part = 0;
    int val  = 0;
    int digits = 0;

    if (s == NULL || s[0] == '\0') return 0;

    while (*s != '\0') {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3) return 0;
            if (val > 255) return 0;
        } else if (*s == '.') {
            if (digits == 0) return 0;          /* 空段 */
            ++part;
            if (part > 3) return 0;
            val = 0;
            digits = 0;
        } else {
            return 0;
        }
        ++s;
    }
    return (part == 3 && digits > 0) ? 1 : 0;
}

int net_addr_is_ipv6(const char *s)
{
    const char *p   = s;
    const char *grp = NULL;      /* 当前组的起始位置（用于内嵌 IPv4 校验） */
    int groups    = 0;           /* 已完成的组数 */
    int digits    = 0;           /* 当前组内十六进制位数 */
    int dcolon    = 0;           /* 是否已出现过 "::" */
    int total_hex = 0;

    if (s == NULL || s[0] == '\0') return 0;
    if (strchr(s, ':') == NULL) return 0;

    while (*p != '\0') {
        char c = *p;

        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            if (digits == 0) grp = p;
            if (++digits > 4) return 0;              /* 组内超过 4 位十六进制 */
            ++total_hex;
            ++p;
            continue;
        }
        if (c == '.') {
            /* 尾部内嵌 IPv4（如 ::ffff:192.168.0.1）：当前组必须整体是合法 IPv4 */
            return (grp != NULL && net_addr_is_ipv4(grp)) ? 1 : 0;
        }
        if (c != ':') return 0;

        if (digits > 0) {                            /* "xx:" —— 结束一组 */
            ++groups;
            digits = 0;
            ++p;
            continue;
        }

        /* 连续冒号：只允许出现一次 "::" */
        if (dcolon) return 0;
        dcolon = 1;
        ++p;
        if (*p == ':') ++p;                          /* 跳过 "::" 的第二个冒号 */
    }

    if (digits > 0) ++groups;
    if (total_hex == 0) return 0;
    return (groups <= 8) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* AT+CGPADDR 解析                                                     */
/* ------------------------------------------------------------------ */
static int next_quoted(const char **pp, char *out, size_t out_size)
{
    const char *p = *pp;
    const char *q;
    size_t      n;

    while (*p != '\0' && *p != '"') ++p;
    if (*p != '"') return 0;
    ++p;
    q = strchr(p, '"');
    if (q == NULL) return 0;

    n = (size_t)(q - p);
    if (n >= out_size) n = out_size - 1u;
    memcpy(out, p, n);
    out[n] = '\0';
    *pp = q + 1;
    return 1;
}

int net_addr_parse_cgpaddr(const char *resp, char *v4, size_t v4_size,
                           char *v6, size_t v6_size)
{
    const char *p;
    char        tok[NET_ADDR_MAX];
    int         found = 0;

    if (v4 != NULL && v4_size > 0u) v4[0] = '\0';
    if (v6 != NULL && v6_size > 0u) v6[0] = '\0';
    if (resp == NULL) return -1;

    p = strstr(resp, "+CGPADDR:");
    if (p == NULL) return -2;
    p += 9;                                     /* strlen("+CGPADDR:") */

    while (next_quoted(&p, tok, sizeof(tok))) {
        if (tok[0] == '\0') continue;

        if (net_addr_is_ipv6(tok)) {
            if (v6 != NULL && v6_size > 0u && v6[0] == '\0') {
                (void)snprintf(v6, v6_size, "%s", tok);
                found = 1;
            }
        } else if (net_addr_is_ipv4(tok)) {
            if (strcmp(tok, "0.0.0.0") == 0) continue;      /* 未获得地址 */
            if (v4 != NULL && v4_size > 0u && v4[0] == '\0') {
                (void)snprintf(v4, v4_size, "%s", tok);
                found = 1;
            }
        }
    }
    return found ? 0 : -3;
}

/* ------------------------------------------------------------------ */
/* 公网 IP 回显提取                                                    */
/* ------------------------------------------------------------------ */
int net_addr_extract_echo(const char *body, char *out, size_t out_size)
{
    const char *p;
    char        tok[NET_ADDR_MAX];

    if (body == NULL || out == NULL || out_size == 0u) return -1;
    out[0] = '\0';

    p = body;
    while (*p != '\0') {
        size_t n = 0;

        /* 跳过非地址字符 */
        while (*p != '\0' && !((*p >= '0' && *p <= '9') ||
                               (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') ||
                               *p == ':' || *p == '.')) {
            ++p;
        }
        /* 收集候选片段 */
        while (*p != '\0' && n + 1u < sizeof(tok) &&
               ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                (*p >= 'A' && *p <= 'F') || *p == ':' || *p == '.')) {
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        if (n == 0u) break;

        if (net_addr_is_ipv6(tok) || (net_addr_is_ipv4(tok) && strcmp(tok, "0.0.0.0") != 0)) {
            (void)snprintf(out, out_size, "%s", tok);
            return 0;
        }
    }
    return -2;
}
