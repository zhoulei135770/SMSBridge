/**
 * @file    net_addr.h
 * @brief   IP 地址解析与判定（纯 C99，无 SDK 依赖，可主机单元测试）。
 *
 * 用途（IPv6 实时开启 + 获取公网地址）：
 *   - `net_addr_parse_cgpaddr()`：解析 `AT+CGPADDR` 应答，一次取出 IPv4 与 IPv6
 *     （双栈时底包会用两个引号字段返回）；
 *   - `net_addr_extract_echo()`：从"公网 IP 回显服务"的响应正文里提取地址
 *     （裸文本、带引号、或 `{"ip":"1.2.3.4"}` 这类 JSON 包装都能取）；
 *   - `net_addr_is_ipv4()` / `net_addr_is_ipv6()`：地址分类校验，避免把
 *     `+CGPADDR` 的上下文编号、"0.0.0.0" 之类的占位值当成地址。
 */
#ifndef NET_ADDR_H
#define NET_ADDR_H

#include <stdint.h>
#include <stddef.h>

#define NET_ADDR_V4_MAX   16u   /**< "255.255.255.255" + '\0' */
#define NET_ADDR_V6_MAX   46u   /**< 完整 IPv6（39）+ 余量 + '\0' */
#define NET_ADDR_MAX      46u

/** 合法 IPv4 点分十进制（4 段、每段 0..255）。 */
int net_addr_is_ipv4(const char *s);

/** 合法 IPv6（支持 `::` 压缩、组数 ≤8、可含内嵌 IPv4 尾部；不校验压缩位置的最严格规则）。 */
int net_addr_is_ipv6(const char *s);

/**
 * @brief 解析 `AT+CGPADDR` 应答，抽取 IPv4 与 IPv6。
 *
 * 已覆盖的应答形态：
 *   `+CGPADDR: 1,"10.20.30.40"`                纯 IPv4
 *   `+CGPADDR: 1,"10.20.30.40","2409:8a00::1"` 双栈
 *   `+CGPADDR: 1,"","2409:8a00::1"`            纯 IPv6（IPv4 字段为空）
 *   `+CGPADDR: 1,"2409:8a00::1"`               单字段但为 IPv6
 *
 * @param resp     应答原文（可含多行、OK 等）
 * @param v4       输出 IPv4；无则写空串（可为 NULL）
 * @param v6       输出 IPv6；无则写空串（可为 NULL）
 * @return 0 至少解析出一个地址；<0 失败（-1 参数、-2 无 +CGPADDR 行、-3 无有效地址）
 * @note `0.0.0.0` 视为"未获得地址"，不计入。
 */
int net_addr_parse_cgpaddr(const char *resp, char *v4, size_t v4_size,
                           char *v6, size_t v6_size);

/**
 * @brief 从公网 IP 回显服务的响应正文里提取第一个合法地址。
 * @return 0 成功；<0 未找到
 */
int net_addr_extract_echo(const char *body, char *out, size_t out_size);

#endif /* NET_ADDR_H */
