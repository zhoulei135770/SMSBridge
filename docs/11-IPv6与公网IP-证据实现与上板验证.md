# 11 · IPv6 实时开启 + 获取公网 IP：证据、实现与上板验证

> 需求（2026-10-15）：**把设备的 IPv6 实时打开，并获取公网 IP 地址**。
> 本文 = 能力证据（静态取证：SDK 头文件 / 应用链接边界 / 出厂实机回显 / 底包反汇编）
> ＋ 本工程实现 ＋ **必须上板确认的清单**。所有"已证实/未证实"标注沿用本工程惯例。

---

## 一、结论摘要（先看这 6 条）

1. **能不能做：能，而且设备出厂就是双栈。** 出厂实机回显（`out/flash/module_backup_before_flash.txt:33,37`）：
   ```
   --- AT+CGDCONT?
   +CGDCONT: 1,"IPV4V6","cmnet",,0,0,,,,
   +CGDCONT: 8,"IPV4V6","IMS",,0,0,0,2,1,1
   --- AT+CGACT?
   +CGACT: 1,1        ← PDP 上下文 1 已激活
   ```
   即 NV 里 cmnet/IMS 的 PDP type 字面量就是 `IPV4V6`。烧录的厂商底包 `apn.bin` 对中国移动/联通/电信
   的 APN 也**全部预置 `IPV4V6`**（与 `pkg_vendor_baseline/apn.bin` SHA256 一致）。**【已证实】**

2. **`AT+CGDCONT` 的 PDP type 支持 `IPV6` 与 `IPV4V6`**：① 出厂实机回显；② 固件 APN 库里
   `{208,20,"Bouygues Telecom",...,"IPV6"}` 说明纯 `IPV6` 也合法；③ DSLN 底包 diag 库枚举
   `_CiPsPdpType: PPP=0, IP=1, IPV6=2, IPV4V6=3`。另有 `SM_IPV4V6_CHANGE_IPV4_RETRY/IPV6_RETRY`
   ⇒ **IPV4V6 模式下底包自带 v4/v6 回退重试**。**【已证实】**

3. **"实时打开"是可行的**：改 PDP type = 重新激活上下文，`AT+CGDCONT` 属 3GPP 指令，虚拟 AT
   官方声明支持；本工程把它做成运行时开关（Web 一键），由唯一发起 AT 的 `net_mgr` 任务执行。

4. **"公网 IP"要分两种，别混为一谈**：
   - **IPv6**：CMCC 分配的是 `2409::/` 全球单播地址（GUA），**本身就是公网地址** ⇒ `AT+CGPADDR` 直接可得；
   - **IPv4**：`cmnet` 下通常是 `10.x.x.x`（运营商 NAT/CGNAT 之后）⇒ **`AT+CGPADDR` 给不出公网 IPv4**，
     只能用 HTTP 回显服务（本工程 `sdk_http_get_text()`）读出口地址。

5. ⚠ **一个会"量错对象"的真缺陷（已修）**：`cm_async_dns.h:100,106` 原文"**开机默认v6优先**"，
   而原实现的 `cm_httpclient_cfg_t.dns_priority` 填的是 `0`（= 用全局优先级）。后果：回显服务的域名
   若有 AAAA 记录，请求会走 **IPv6 出口**，拿回来的是 IPv6 地址却写进"公网 IPv4"字段。
   现已在 `sdk_http_get_text()` 增加 `dns_priority` 形参，查公网 IPv4 时显式传 `1`；
   若仍返回 IPv6，则记入独立的 `pub_ip6` 字段并告警，**不冒充 IPv4**。

6. **`AT+CGPADDR` 的双栈返回形态已实测证实（2026-10-15 上板）**：
   ```
   AT+CGPADDR=1  ->  +CGPADDR: 1,"10.0.0.100","2409:xxxx:xxxx:xxxx::1"
   AT+CGPADDR=8  ->  +CGPADDR: 8,"","2409:xxxx:xxxx:xxxx::1"
   ```
   两个引号字段依次是 IPv4 与 **IPv6 全局地址**，纯 v6 时 IPv4 字段为空串 —— 与
   `net_addr_parse_cgpaddr()` 的四种形态假设完全一致。**该项从未证实转为已证实**（详见 `docs/12`）。

7. **IPv6 公网可达性也已实测**：PC 经模组 RNDIS 自动配置到同一 `/64` 的公网 IPv6，并能连通
   公网 IPv6 目标（阿里 DNS v6，~70 ms）。详见 `docs/12` §四。

---

## 二、能力边界（硬约束，来自链接器实测）

`docs/sdk-reference/app-link-symbols.txt`（应用可调用 326 符号）里与 IPv6/网络地址相关的**全部**命中：

| 符号 | 用途 |
|---|---|
| `cm_get_ipv4v6_local_addr` | 唯一的 cm 级地址 API，**但无任何头文件声明**，见 §四 |
| `inet_ntop` / `inet_pton` / `ip6addr_ntoa` / `ip6addr_aton` / `ipaddr_ntoa` | 地址文本↔二进制 |
| `lwip_get_sock_isipv6` / `lwip_get_sock_localipXaddr` / `lwip_get_sock_remoteipXaddr` | 取 socket 的本地/远端地址（可证明"真的走了 IPv6"） |
| `lwip_getaddrinfo` / `lwip_gethostbyname` / `lwip_freeaddrinfo` | 域名解析（可解析 AAAA） |
| `cm_async_dns_init/eloop/request_ex/set_priority` | 原生 IPv6 异步 DNS |
| `cm_httpclient_*`（含 `set_cfg`） | HTTP(S) 同步客户端，`dns_priority` 就在这里 |
| `cm_ntp_set_cfg` / `cm_ntp_sync` | NTP（`CM_NTP_CFG_SERVER` 支持 IPv6 地址） |
| `cm_virt_at_*` | 虚拟 AT（本工程主路径） |

**负面清单（清单里没有 ⇒ 调用必然 undefined reference）**：`netif*`、`cm_net*`、`cm_pdp*`、
`cm_get_pdp_type`、`cm_get_pdp_state`、`lwip_iflist`、`lwip_ifexist`、`lwip_get_netif_*`、
`lwip_get_default_nw_ifname`、`cm_get_ipv4v6_local_addr_to_ip`。
⇒ **不能枚举网卡、不能查当前 PDP 类型**（所以本工程自己用 `pdp_type[8]` 记录当前类型）。

`cm_virt_at.h:41-57` 原文约束（决定了实现路径）：
> 1、虚拟AT不完全支持中移开发的AT手册中的指令，**仅支持3GPP的AT指令**……
> 2、**暂不支持使用多线程操作虚拟AT**，同时在使用虚拟AT时每次都需要初始化和去初始化。

⇒ `AT+CGDCONT`/`AT+CGPADDR`/`AT+CGACT` 可用；且这些 AT **只能由 net_mgr 单个任务串行发起**。

---

## 三、本工程实现（2026-10-15）

| 文件 | 改动 |
|---|---|
| `app/inc/net_addr.h`<br>`app/src/net_addr.c` | **新增纯逻辑模块**：IPv4/IPv6 字面量判定、`AT+CGPADDR` 双栈解析、HTTP 回显正文提取（裸文本 / 带引号 / JSON 都能取）。已并入正式主机测试 `[11]` 段 |
| `app/src/sdk_port.c` | ① `sdk_net_set_pdp(apn, pdp_type)`（原 `sdk_net_set_apn_and_activate` 硬编码 `"IP"`，已替换）② `sdk_net_pdp_addrs()` 一次读 v4+v6（解析失败打原始应答）③ `sdk_http_get_text(url, dns_priority, ...)`，`http_sync_request()` 抽出 POST/GET 公共路径 |
| `app/src/net_mgr.c`<br>`app/inc/net_mgr.h` | 状态新增 `ip6/pub_ip/pub_ip6/pdp_type/ipv6_on/ipv6_enable/pubip_*`；新增 `net_mgr_set_ipv6()`（**实时开关**）、`net_mgr_refresh_pubip()`；5 秒轮询同时取双栈地址；公网 IPv4 按 10 分钟周期（或手动）查询，**强制 v4 优先**；切换限速 15 秒；重新激活后**轮询等地址出现**（最多 15 秒）而不是固定 sleep |
| `app/src/app_config.c`<br>`app/inc/app_config.h` | **版本升到 v1.2**，末尾追加 `ipv6_enable`（默认 1）、`pubip_enable`（默认 1）、`pubip_url`（默认 `http://ip.3322.net`）；sanitize 校验 scheme |
| `app/src/web_ui.c`<br>`app/src/web_server.c` | 状态页新增：模组 IPv6、PDP 类型、**公网 IPv4 出口 / 公网 IPv6 出口**、以及"**短信远程配置**"只读卡片（含优先级提示）；快捷操作新增 **开启 IPv6 双栈 / 关闭 IPv6 / 刷新公网 IP**；配置表单新增 IPv6 与公网地址一组 |
| `app/test/host_tests.c` | 新增 `[11] net_addr` 段（判定 + 4 种 CGPADDR 形态 + 回显提取），web_ui 段补 IPv6/公网/远程配置的渲染断言 |

**两处实时生效路径**：
- Web 一键开关 → `POST /action/ipv6_on|ipv6_off` → 延迟到响应发出后调 `net_mgr_set_ipv6()` →
  `net_mgr` 任务写配置并落盘 → `AT+CGDCONT=1,"IPV4V6"|"IP","<apn>"` → 重新激活 → 轮询地址 → 面板显示。
- 保存配置里的 IPv6 开关同样会触发一次立即重拨（`s_defer_ipv6`）。

---

## 四、为什么不采用 `cm_get_ipv4v6_local_addr`

它虽然在链接边界内，但对底包 `.axf` 反汇编（`0x8030a8f2`，82 字节）显示：
读数后调 `ipaddr_ntoa` / `ip6addr_ntoa`，字符串**只交给 `RTI_LOG` 打印**，
调用者缓冲区只收到**原始地址字节**（结构 +12 = IPv4、+64 = IPv6），并不回传文本；
且反汇编显示 `arg1 == 2` 走 **IPv4 分支**，而平台宏 `IP_TYPE_IPV6` 也是 `2`（语义冲突）。
⇒ 收益低于 `AT+CGPADDR`，**不采用**（若要"证明数据面真的走 v6"，正确做法是在 eloop 线程用
`cm_asocket_open(AF_INET6,...)` + `lwip_get_sock_localipXaddr()`，见 §五 第 4 项）。

---

## 五、必须上板确认的清单（按优先级）

| # | 待确认 | 为什么静态定不了 | 怎么验 |
|---|---|---|---|
| 1 | ~~`AT+CGPADDR=1` 在 IPV4V6 下是否返回两个引号字段~~ | — | **已实测证实（2026-10-15）**：`+CGPADDR: 1,"10.0.0.100","2409:xxxx:xxxx:xxxx::1"`，见 `docs/12` §三 |
| 2 | **实网是否给 `2409::/` 全局 IPv6**、前缀是否稳定 | 依赖签约与网络 | 连续观察面板"模组 IPv6 (PDP)" |
| 3 | **回显服务可达性与返回族**（含是否走了 v6） | 外部网络 | 面板"刷新公网 IP"：正常应显示**公网 IPv4 出口**；若显示"公网 IPv6 出口"，说明该域名只有 AAAA 或 v4 出口不可用 → 换 URL 或接受 v6 结果 |
| 4 | **IPv6 数据面是否真的通**（不只是"分配了地址"） | 需要真实连接 | 后续可加：eloop 线程内 `cm_asocket_open(AF_INET6,...)` 连 v6 目标 + `lwip_get_sock_localipXaddr()` + `inet_ntop()` 打印实际本地 v6 |
| 5 | **`AT+MDIALUPCFG` / `AT+MIPCALL` 经虚拟 AT 能否成功** | `cm_virt_at.h` 只保证 3GPP；`docs/07:42-43` 记录真 AT 口上 `AT+MDIALUPCFG?` → `+CME ERROR: 4` | 看重新激活时的 AT 应答日志 |
| 6 | **改 PDP type 后多久分配地址** | 运营商行为 | 已改为轮询等地址（最多 15 秒），观察实际耗时 |
| 7 | **`gethostbyname`（心跳/域名解析）在非 eloop 任务里是否安全** | 属内核 socket/DNS 路径，`docs/08` 教训"能链接≠能调用" | 跑一段时间看是否复位 |
| 8 | `AT+CGAUTH` 是否存在（影响需要鉴权的 APN） | 两套证据都是"未出现" | 直接发 `AT+CGAUTH?` |

---

## 六、配置与默认值（v1.2）

| 字段 | 默认 | 说明 |
|---|---|---|
| `ipv6_enable` | 1 | PDP 类型用 `IPV4V6`（双栈）；关闭则用 `IP` |
| `pubip_enable` | 1 | 是否定期查询公网出口 IP |
| `pubip_url` | `http://ip.3322.net` | 回显服务（HTTP 明文，避免缺 `ca.pem` 时 HTTPS 校验被降级）；返回纯文本或 JSON 均可解析 |

> ⚠ **版本升级的影响**：`APP_CFG_VERSION` 由 `0x00010001` 升到 `0x00010002`。
> 已烧录设备上旧的 `cfg.bin` 会因版本不符被拒 → 首次启动按默认值重建并写回
> （配置回到出厂默认：口令 admin123、黑白名单清空等）。这是"只追加字段"策略的既有约定，
> 见 `docs/09` 第五节。
