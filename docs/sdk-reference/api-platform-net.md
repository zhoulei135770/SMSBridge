# 平台网络 / RNDIS / 流量统计 API 核实

**核实对象**：ML307A-DSLN 底包（预编译平台库）
**底包版本标识**（从 `.axf` 内构建字符串读出）：
`ML307A_OpenCPU_Standard_1.4.2.2306201630_release` / `ML307A-DSLN-MBRH1S00` / `-DML302A_TIMESTAMP=2306201630` / `-DML302A_GIT_ID=a086d37a`
**权威符号表**：`prebuild/baseline/ML307A/DSLN/ML307A-DSLN-MBRH1S00.map`（7,628,105 B）
**含调试串的 ELF**：`prebuild/baseline/ML307A/DSLN/ML307A-DSLN-MBRH1S00.axf`（71,065,204 B）
**涉及库归档**（map 中归属，L69692–L73836 区段）：`pcaconfigs-lwipv4v6.lib` → `lwip_api.o` / `lwip_minis.o` / `lwip_stats.o` / `netif.o`

> 判定口径：**"存在" = map 中作为已链接符号出现**（非 veneer、非调试串）。
> map 中形如 `Long ARM to Thumb Veneer to X` / `Section` / `Data` 的非代码条目已单独标注。
> 仅出现在 `.debug_info` 或 `.rodata` 字符串表里的名字**不算存在**（见 §1 说明）。

---

## 结论速览

| # | 结论 | 置信度 |
|---|---|---|
| 1 | `lwip_nwst_*` 中**只有 5 个真正链接进底包**：`get_current_speed` / `get_used_data` / `clear_used_data` / `set_cutoff_data` / `set_reminder_data`。**`lwip_nwst_set_callback` 与 `lwip_nwst_get_cutoff_data` 没有链接进底包**（map 中无该符号）。 | 高 |
| 2 | `LWIP_NO_STATIC` 在本底包 **被定义为 `0`**（`lwipopts_crane.h:218`），因此 `lwip_api.h:414` 的 `#if !LWIP_NO_STATIC` 恒为真，**头文件层面全部 nwst 声明可见**；但符号层面第 1 条为准 —— 声明可见 ≠ 可链接。 | 高 |
| 3 | `nwst_callback_fn` **在 SDK 中找不到任何 typedef/定义**，仅在 `lwip_api.h:428` 被使用一次；其定义所在的 `Dialer_Task.h` **不存在于 include 树**（仅作为 `-IW:\tavor\Arbel\obj_PMD2NONE\inc` 的调试路径出现在 ELF 中）。**结论：未证实**。 | 高（"找不到"这一事实） |
| 4 | **底包是 CRANE 平台，不是 NBIOT**：构建 flag 含 `-DCRANE_MCU_DONGLE -DML307A_SUPPORT`，`lwipopts.h:53` 默认 `LWIP_PLATFORM_DEFINE = CRANE`，故 `IS_LWIP_PLATFORM_NBIOT == 0`。`lwip_api.h:148-155` 的 `#if IS_LWIP_PLATFORM_NBIOT` 块因此被编译掉；**但同一批 RNDIS 函数在 `lwip_api.h:387-390` 处还有一份无任何 `#if` 保护的裸声明，所以编译期仍然可见、可直接调用。** | 高 |
| 5 | **`192.168.0.1` 在底包中完全不存在**（ASCII 扫描 0 次；大端 `C0 A8 04 01` 与字序 `01 04 A8 C0` 原始字节扫描也均为 0 次）。二进制里唯一成体系的默认网段是 **`192.168.0.x`**（`192.168.0.1` / `.0.100` / `.0.200`）。**需求文档假设的 192.168.0.1 被证伪（就底包内可见证据而言）**。 | 高 |
| 6 | 唯一可佐证的 dhcpd 默认网关是 **`192.168.0.1`**，且仅以 **AT 命令帮助文本**形式出现（`dhcpd serverip` 示例），同时 `dhcpd netmask` 示例为 **`255.255.0.0`**（不是 255.255.255.0）。**这是帮助文本，不是从运行期配置结构里读出的真实默认值** —— 真实运行期默认仍需实测。 | 中（网关地址）/ 高（无 4.1） |
| 7 | 流量统计另有一条**未在 map 中**的路径：`ip_flow_stats_rx/tx`（`lwip_minis.o`，存在）与 `lte_data_statistic_*` 系列（`lwip_stats.o`，存在）；而 `get_ip_flow_stats`、`netif_get_usb_packets`、`netif_get_uap_packets`、`netif_get_all_packets` **均未链接进底包**（尽管 `lwip_stats.h:43/49-51` 有声明）。 | 高 |
| 8 | `cm_http_client_new` **不存在于本 SDK**；本版本公开 HTTP API 是 `cm_httpclient_create/delete/...`（`cm_http_client.o`，已链接）。属于**命名差异**，不是能力缺失。 | 高 |

---

## 1. 符号存在性核实表

### 1.1 判定依据说明（必读）

map 里同一个名字可能以多种形态出现，本次实际观察到的形态有 4 类，含义不同：

| map 中的形态 | 含义 | 是否算"存在" |
|---|---|---|
| `符号名 0xADDR Thumb Code N xxx.o(.text)` | 已链接的实际函数体 | ✅ 存在，可直接调用 |
| `Long ARM to Thumb Veneer to 符号名 ... anon$$obj.o(Veneer$$Code)` | 为跨状态（ARM↔Thumb）跳转生成的 veneer；**说明该符号被真实引用** | ✅ 存在（并额外证明有调用者） |
| `符号名 0xADDR Data N xxx.o(.data/.bss)` | 同名**全局变量**，不是函数 | ⚠️ 存在但同名的是变量 |
| 仅出现在 `.axf` 字符串（`.debug_info` / `.rodata`）中 | 编译期调试信息或名字表，**未链接** | ❌ 不存在 |

另外发现一个易误判点：`.axf` 的 `DDR_RO_EXEC` 段 0x489200–0x489400 处有一张**连续函数名表**：

```
...lwip_ifconfig_value.lwip_ifconfig.lwip_ifconfig_dongle.lwip_iflist_value.lwip_iflist.
lwip_iflist_dongle.lwip_manul_adjust_pp.lwip_manual_set_pmod....0.......1.......2.......
lwip_nwst_get_current_speed.lwip_nwst_get_used_data.lwip_nwst_clear_used_data.
lwip_nwst_set_cutoff_data.lwip_nwst_set_reminder_data.lwip_nwst_set_callback....
```

该表**包含 `lwip_nwst_set_callback`，但 map 中并无此符号**，且它**不含** `lwip_nwst_get_cutoff_data`。可见这张表来自头文件声明/文档生成流程，**不能作为符号存在的证据**。本表一律以 map 为准。

### 1.2 流量统计（lwip_nwst_*）

| 符号 | 存在? | 位置（map 行号 / 归属） |
|---|---|---|
| `lwip_nwst_get_used_data` | ✅ 存在 | **L58608** `0x803446b5 Thumb Code 84 lwip_api.o(.text)` |
| `lwip_nwst_clear_used_data` | ✅ 存在 | **L58609** `0x80344709 Thumb Code 76 lwip_api.o(.text)` |
| `lwip_nwst_get_current_speed` | ✅ 存在 | **L58607** `0x80344631 Thumb Code 132 lwip_api.o(.text)` |
| `lwip_nwst_set_cutoff_data` | ✅ 存在 | **L58610** `0x80344755 Thumb Code 114 lwip_api.o(.text)` |
| `lwip_nwst_set_reminder_data` | ✅ 存在 | **L58611** `0x803447c7 Thumb Code 114 lwip_api.o(.text)` |
| `lwip_nwst_set_callback` | ❌ **不存在** | map 0 匹配。仅出现在：`lwip_api.h:428` 声明；`.axf` 0x48938c 名字表；`.debug_info` 0x25411b0 的 `nwst_callback_fn`。 |
| `lwip_nwst_get_cutoff_data` | ❌ **不存在** | map 0 匹配。`lwip_api.h:429` 有声明，但**连名字表里都没有**。 |

**地址空缺佐证**：L58607–L58611 在 map 中是连续 5 条，`set_reminder_data`(0x803447c7, 114B → 结束于 0x80344839) 之后直接跳到 `lwip_get_dnsr_hijack_flag`(L58612, 0x80344861)，中间**只有 0x28 字节空隙**，放不下 `set_callback`；而 0x80344839–0x80344861 是段对齐填充。即底包里这一段**确实没有**这两个函数。

### 1.3 USB netif 状态

| 符号 | 存在? | 位置 |
|---|---|---|
| `netif_usb_get_status` | ✅ 存在 | **L34228** `0x7e6a8ac3 Thumb Code 26 netif.o(.text)` |
| `netif_usb_set_status` | ✅ 存在 | **L34229** `0x7e6a8add Thumb Code 148 netif.o(.text)` |
| `netif_get_usb_if` | ✅ 存在 | **L34208** `0x7e6a788d Thumb Code 36 netif.o(.text)`；另有 veneer **L60714** `Long ARM to Thumb Veneer to netif_get_usb_if 0x803cf988 anon$$obj.o(Veneer$$Code)`（→ 有真实调用者） |

### 1.4 RNDIS 热插拔

| 符号 | 存在? | 位置 |
|---|---|---|
| `lwip_rndis_hotplug_link` | ✅ 存在 | **L58579** `0x80342797 Thumb Code 80 lwip_api.o(.text)` |
| `lwip_rndis_hotplug_break` | ✅ 存在 | **L58580** `0x803427e7 Thumb Code 80 lwip_api.o(.text)` |
| `lwip_do_rndis_hotplug_now` | ✅ 存在 | **L58597** `0x803441ab Thumb Code 148 lwip_api.o(.text)` |
| `lwip_set_rndis_hotplug_flag` | ❌ **不存在**（函数） | map 中无函数形态。**但**存在同名变量：**L29720** `lwip_rndis_hotplug_flag 0x7e41cb04 Data 4 lwip_api.o(.data)`（注意变量名无 `set_`）。头文件声明在 `lwip_api.h:234`。 |
| `lwip_get_rndis_hotplug_flag` | ❌ **不存在** | map 0 匹配。仅 `.debug_info` 0x2acb057 有名字串。声明在 `lwip_api.h:235`。 |
| （附）`lwip_do_rndis_hotplug` | ✅ 存在 | **L58598** `0x8034423f Thumb Code 12 lwip_api.o(.text)` |

> 注意：`lwip_set/get_rndis_hotplug_flag` 声明位于 `lwip_api.h:234-235`，**不在**任何 `#if` 保护内，却仍未链接 —— 即头文件里有、底包里没有，"编译能过、链接会失败"。

### 1.5 转发开关

| 符号 | 存在? | 位置 |
|---|---|---|
| `lwip_set_usb_forward_flag` | ✅ 存在 | **L47499** `0x800cae6f Thumb Code 10 lwip_minis.o(.text)` |
| `lwip_get_usb_forward_flag` | ✅ 存在 | **L47500** `0x800cae79 Thumb Code 6 lwip_minis.o(.text)`；+ veneer **L33657** `Long Thumb to Thumb Veneer to lwip_get_usb_forward_flag 0x7e68a785`（→ 有调用者） |
| `lwip_get_ip_forward_flag` | ✅ 存在 | **L47504** `0x800cae99 Thumb Code 6 lwip_minis.o(.text)`；+ veneer **L33479** `Long ARM to Thumb Veneer to lwip_get_ip_forward_flag 0x7e68a1ac` |
| `lwip_set_ip_forward_flag` | ✅ 存在 | **L47503** `0x800cae8f Thumb Code 10 lwip_minis.o(.text)` |
| （附）同名变量 | ⚠️ | **L29731** `lwip_ip_forward_flag 0x7e41cb7c Data 4 lwip_minis.o(.data)`；**L29733** `lwip_usb_forward_flag 0x7e41cb84 Data 4 lwip_minis.o(.data)` |

### 1.6 netif 查询 / 配置

| 符号 | 存在? | 位置 |
|---|---|---|
| `lwip_get_nw_status` | ❌ **不存在** | map 0 匹配；`lwip_api.h:94` 有声明。名字串亦不在 0x489xxx 名字表中。 |
| `lwip_iflist` | ✅ 存在 | **L58596** `0x80343e65 Thumb Code 838 lwip_api.o(.text)` |
| `lwip_ifconfig` | ✅ 存在 | **L58594** `0x80343645 Thumb Code 1622 lwip_api.o(.text)` |
| `lwip_get_netif_pdp_if` | ❌ **不存在** | map 0 匹配；`lwip_api.h:90` 有声明（`struct netif * lwip_get_netif_pdp_if(char simid, char cid)`）。 |
| `lwip_get_netif_by_hostname` | ❌ **不存在** | map 0 匹配；`lwip_api.h:91` 有声明。 |
| `lwip_ifexist` | ✅ 存在 | **L58591** `0x80342dc5 Thumb Code 96 lwip_api.o(.text)` |
| `lwip_select_netif_ready` | ✅ 存在 | **L47521** `0x800cafd9 Thumb Code 304 lwip_minis.o(.text)` |
| `lwip_select_default_nw_ready` | ❌ **不存在** | map 0 匹配；`lwip_api.h:412` 有声明。仅 `.axf` 0x28db33（`DDR_SYS_RO_EXEC` 名字表）有名字串。 |
| `lwip_get_netif_usb_ifname` | ✅ 存在 | **L58588** `0x80342d89 Thumb Code 22 lwip_api.o(.text)` |
| `lwip_get_default_nw_ifname` | ✅ 存在 | **L58614** `0x80344879 Thumb Code 36 lwip_api.o(.text)` |

### 1.7 cm_* / HTTP

| 符号 | 存在? | 位置 |
|---|---|---|
| `cm_virt_at_init` | ✅ 存在 | **L59843** `0x8039e9db Thumb Code 44 cm_virt_at.o(.text)` |
| `cm_virt_at_send` | ✅ 存在 | **L59845** `0x8039ea6b Thumb Code 78 cm_virt_at.o(.text)` |
| `cm_virt_at_get` | ✅ 存在 | **L59844** `0x8039ea07 Thumb Code 100 cm_virt_at.o(.text)` |
| `cm_virt_at_deinit` | ✅ 存在 | **L59846** `0x8039eab9 Thumb Code 22 cm_virt_at.o(.text)` |
| `cm_virt_at_urc_reg` | ✅ 存在 | **L46508** `0x800a9a55 Thumb Code 6`，**归属 `cimodem_usb.o(.text)`**（不是 `cm_virt_at.o`，注意归属不同） |
| `cm_virt_at_sms_send` | ✅ 存在 | **L59847** `0x8039eacf Thumb Code 210 cm_virt_at.o(.text)` |
| `cm_virt_at_msg_memory` | ✅ 存在 | **L59848** `0x8039eba1 Thumb Code 202 cm_virt_at.o(.text)` |
| `cm_modem_info_cell` | ✅ 存在 | **L59849** `0x8039ef1d Thumb Code 98 cm_modem_info.o(.text)` |
| `cm_modem_info_radio` | ✅ 存在 | **L59850** `0x8039ef7f Thumb Code 72 cm_modem_info.o(.text)` |
| `cm_fs_open` | ✅ 存在 | **L58836** `0x8034e829 Thumb Code 54 cm_fs.o(.text)` |
| `cm_http_client_new` | ❌ **不存在** | map 0 匹配，源码树中亦 0 匹配。**本版本的对应 API 是 `cm_httpclient_create`**（**L59679** `0x803971a7 Thumb Code 284 cm_http_client.o(.text)`）。见 §1.8。 |

### 1.8 HTTP 命名差异补充（`cm_http_client_new` 的"正名"）

`cm_http_client_new` 在本 SDK 中不存在（既无声明也无符号）。`include/cmiot/cm_http.h` 公开的是 `cm_httpclient_*` 系列，全部已链接：

| 符号 | map 行号 | 地址 |
|---|---|---|
| `cm_httpclient_create` | L59679 | `0x803971a7` |
| `cm_httpclient_delete` | L59678 | `0x8039717f` |
| `cm_httpclient_set_cfg` | L59680 | `0x803972c3` |
| `cm_httpclient_terminate` | L59681 | `0x80397313` |
| `cm_httpclient_request_start` | L59686 | `0x803973db` |
| `cm_httpclient_request_send` | L59687 | `0x8039748b` |
| `cm_httpclient_request_end` | L59688 | `0x80397597` |
| `cm_httpclient_sync_request` | L59691 | `0x80397641` |

→ 这是**命名差异**（`_client_new` vs `cm_httpclient_create`），不是功能缺失。

### 1.9 流量统计补充符号

| 符号 | 存在? | 位置 |
|---|---|---|
| `get_ip_flow_stats` | ❌ **不存在** | map 0 匹配；`lwip_stats.h:43` 有声明 `ip_flow_stats *get_ip_flow_stats(void);` |
| `ip_flow_stats_rx` | ✅ 存在 | **L47458** `0x800ca7eb Thumb Code 28 lwip_minis.o(.text)`；+ veneer **L33466** `Long ARM to Thumb Veneer to ip_flow_stats_rx 0x7e68a144` |
| `ip_flow_stats_tx` | ✅ 存在 | **L47459** `0x800ca807 Thumb Code 28 lwip_minis.o(.text)`；+ veneer **L33467** `Long ARM to Thumb Veneer to ip_flow_stats_tx 0x7e68a14c` |
| （内部）`_ip_flow_stats_rx` | ✅ 存在 | **L59627** `0x8039278b Thumb Code 20 lwip_stats.o(.text)` |
| （内部）`_ip_flow_stats_tx` | ✅ 存在 | **L59628** `0x8039279f Thumb Code 20 lwip_stats.o(.text)` |
| `netif_get_usb_packets` | ❌ **不存在** | map 0 匹配；`lwip_stats.h:49` 有声明。仅 `.debug_info` 0x2a5e8ab 有名字串（含局部变量 `packets_cnt`、`netif`） |

**替代路径（已核实存在，`lwip_stats.o`）**：`lte_data_statistic_get_used`(L59633 `0x8039283b`) / `lte_data_statistic_process`(L59634) / `lte_data_statistic_reset`(L59635) / `lte_data_statistic_query_used`(L59636) / `lte_data_statistic_set_event`(L59637) / `lte_data_statistic_clear_event`(L59638)。
另注意 `ip_flow_stats` / `net_flow_stats` / `hw_flow_stats` 三个结构体在 `lwip_stats.h:13-38` 定义，且 `lwip_nwst_data 0x7e5373e0 Data 80 lwip_stats.o(.bss)`（**L5817**）存在。

### 1.10 全表不存在符号汇总

| 符号 | 声明位置 | map |
|---|---|---|
| `lwip_nwst_set_callback` | `lwip_api.h:428` | ❌ |
| `lwip_nwst_get_cutoff_data` | `lwip_api.h:429` | ❌ |
| `lwip_set_rndis_hotplug_flag` | `lwip_api.h:234` | ❌ |
| `lwip_get_rndis_hotplug_flag` | `lwip_api.h:235` | ❌ |
| `lwip_get_nw_status` | `lwip_api.h:94` | ❌ |
| `lwip_get_netif_pdp_if` | `lwip_api.h:90` | ❌ |
| `lwip_get_netif_by_hostname` | `lwip_api.h:91` | ❌ |
| `lwip_select_default_nw_ready` | `lwip_api.h:412` | ❌ |
| `cm_http_client_new` | 无声明 | ❌ |
| `get_ip_flow_stats` | `lwip_stats.h:43` | ❌ |
| `netif_get_usb_packets` | `lwip_stats.h:49` | ❌ |

（`lwip_stats.h:50-51` 的 `netif_get_uap_packets` / `netif_get_all_packets` 同样 map 0 匹配，亦不存在。）

---

## 2. RNDIS 网卡默认 IP（原始字符串 + 上下文）

### 2.1 扫描方法与范围

对 `ML307A-DSLN-MBRH1S00.axf`（71,065,204 B）全文件 mmap 扫描，三类模式：

1. ASCII `\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}` 全量枚举；
2. ASCII `192\.168` 裸前缀 + `255\.255\.x\.x` 掩码；
3. **原始字节**（绕过 ASCII 限制）：`192.168.0.1` 的网络序 `C0 A8 04 01` 与主机序 `01 04 A8 C0`；以及 `.0.1/.0.100/.0.200/.2.2/.1.1` 两种字节序。

### 2.2 全部 `192.168.x.x` 结果（4 个不同值，共 11 处）

| 值 | 出现次数 | 首次文件偏移 | 首次所在段 |
|---|---|---|---|
| `192.168.0.1` | 2 | `0x4a5920` | DDR_RO_EXEC（AT 帮助文本） |
| `192.168.0.100` | 7 | `0x37d1a0` | DDR_SYS_RO_EXEC（含 `lwip_sttest_control` 附近） |
| `192.168.0.100` | 1 | `0x4a59ae` | DDR_RO_EXEC（AT 帮助文本） |
| `192.168.2.2` | 1 | `0x2edbcc` | DDR_SYS_RO_EXEC（`create sink IP frame` 测试代码附近） |

**`192.168.0.1`：0 处（ASCII）＋ 0 处（网络序字节）＋ 0 处（主机序字节）。**
**`10.x.x.x`：0 处。`172.16-31.x.x`：0 处。`192.168.1.1`：0 处。**（`254.128.0.0` 是 IPv6 前缀常量，非 IPv4 网关。）

### 2.3 ±80 字节上下文（原始形态，`.` = 不可打印字节）

**`192.168.0.1` @ `0x4a5920`** — dhcpd 配置帮助块：
```
=system, dhcpflag|1 //config dhcpd dhcpflag..    at*lwipctrl=system, dhcpserver|192.168.0.1 //config dhcpd serverip..    at*lwipctrl=system, dhcpstart|192.168.0.100 //conf
```

**`192.168.0.100` @ `0x4a5967`**（同块，作 ip start 示例）：
```
cpserver|192.168.0.1 //config dhcpd serverip..    at*lwipctrl=system, dhcpstart|192.168.0.100 //config dhcpd ip start..    at*lwipctrl=system, dhcpend|192.168.0.100  //confi
```

**`192.168.0.100` @ `0x4a59ae`**（同块，作 ip end 示例）：
```
hcpstart|192.168.0.100 //config dhcpd ip start..    at*lwipctrl=system, dhcpend|192.168.0.100  //config dhcpd ip end..    at*lwipctrl=system, dhcpmask|255.255.0.0  //config
```

**同一帮助块的完整原文**（`0x4a5800`–`0x4a5c00` 窗口）：
```
/query dhcpd serverip..    at*lwipctrl=system, dhcpmask  //query dhcpd ip start..
at*lwipctrl=system, dhcpstart //query dhcpd ip end..    at*lwipctrl=system, dhcpend  //query dhcpd netmask..
at*lwipctrl=system, dhcpflag|1 //config dhcpd dhcpflag..
at*lwipctrl=system, dhcpserver|192.168.0.1 //config dhcpd serverip..
at*lwipctrl=system, dhcpstart|192.168.0.100 //config dhcpd ip start..
at*lwipctrl=system, dhcpend|192.168.0.100  //config dhcpd ip end..
at*lwipctrl=system, dhcpmask|255.255.0.0  //config dhcpd netmask....
```
→ 帮助文本给出的默认网段三角是 **server `192.168.0.1` / start `.0.100` / end `.0.200` / mask `255.255.0.0`**。**注意掩码是 /16，不是 /24。**

**`192.168.0.1` @ `0x4a8de7`** — 另一处帮助文本（`stconnect` 示例，非网关语义）：
```
s: task, ip4, ip6, port..    example:..    at*lwipctrl=stconnect,task|task1|ip4|192.168.0.1|port|9090,0......help for stgettcpopt:..    format: at*lwipctrl=stgettcpopt,opt
```

**`192.168.0.100` @ `0x37d1a0`** — 带 `Hello World!` 与断言串，疑似 sttest：
```
.h....`i..-....i.(..%.0...\.........d.........|....... F..>@8.9.Hello World!....192.168.0.100....l@~P=V~..A~.k@~..K.lwip_sttest_control.conn_tablelist_last != 0....l.@~.S@~p
```

**`192.168.2.2` @ `0x2edbcc`** — 测试代码 sink IP：
```
NK = [NSAPI 5-6],[packetSize 0-%d],[PacketCount 1-20]...create sink IP frame....192.168.2.2.m..(..(x.(..(Fa....s..m..(..(x.(...  t`t.t.t..<...4...0 R...0 'u.1gu..@....u.u..
```

### 2.4 `255.255.255.0`（题面要求）实际出现位置

| 偏移 | 上下文 | 语义 |
|---|---|---|
| `0x117f04` | `...255.255.255.0.0.0.0.0.0.0.0.0.0.0.0.0...255.255.255.0...%d.%d...+CGTFTRDP:` | **AT 解析模块的 IPv4 netmask 字符串常量域**（与 `+CGTFTRDP`/`+CGEQOSRDP` 相邻），非 RNDIS 网关 |
| `0x3e5cfc` | `...:M.%s%s,Unknown ipsec mode.....255.255.255.255` | ipsec 通配掩码 `255.255.255.255`（不是 .255.0） |
| `0x4a59f5` | `dhcpmask\|255.255.0.0` | 见上，**这是 dhcpd 帮助里的掩码，值为 255.255.0.0** |
| `0x3e6a27e` | `MAX_ADDR_PRESENTATION_BUF_LEN (sizeof "ffff:...:255.255.255.255")` | 编译期 sizeof 常量 |

### 2.5 原始字节命中的甄别（避免误判）

用两种字节序搜 `192.168.0.1` 时在 `DDR_RW_DATA` 段命中 2 处（`0x4d5848`、`0x4d6da4`），但上下文为 `0123456789abcdef..-.L.M.$.M.4.M.@.M.\.M...M...` 之类的**表/查找数据结构**，是字节巧合，**不构成 IP 配置**。已排除。

`DDR_RO_EXEC` 段 0x318a0c / 0x31a594 的 `C0 A8 00 01` 亦嵌在字符串表/指针数组中，同样不作为配置证据。

### 2.6 网卡名（"pc" / "usb" / netif 名）核查

- `lwip_api.h:81-89` 声明 `lwip_get_netif_pc_ifname / wifi / usb / uap / eth / lte / lte6 / default_nw / default_nw6`；`lwip_api.h:104/115/132` 注释列出 netif 名为 **`cid_4`、`pc`、`wlan`、`local`**。
- map 中实际存在的 ifname 取值函数只有：`lwip_get_netif_usb_ifname`(**L58588**)、`lwip_get_netif_uap_ifname`(**L58589**)、`lwip_get_netif_lte_ifname`(**L58590**)、`lwip_get_default_nw_ifname`(**L58614**)。`lwip_get_netif_pc_ifname` / `lwip_get_netif_wifi_ifname` / `lwip_get_netif_eth_ifname` / `lwip_get_netif_lte6_ifname` / `lwip_get_default_nw6_ifname` **未在 map 中出现**。
- ASCII 扫描**未找到**独立的 `"pc\0"` / `"usb\0"` 网卡名字符串常量。`.axf` 中 `usb` 共 2094 次、`RNDIS` 136 次、`rndis` 188 次，但均为日志/描述串（如 `strMobileRNDIS 0x7e7cc550 Data 58 usb_descriptor.o(UsbDesc)`(map **L6450**)、`RndisOnlyUpdateDescriptor 0x8005c20d usb_descriptor.o(.text)`(**L6914**)、`netifapi_pc_rndis_setup 0x7e6a9969 netif_pc.o(.text)`(**L34249**)）。
- → **"usb" netif 的字符串名未在底包中以字面量形式定位到；其名称由 `lwip_get_netif_usb_ifname()` 运行期返回，字面值未证实。**

---

## 3. `lwip_nwst_*` 流量统计可用性（含 `LWIP_NO_STATIC` 结论）

### 3.1 `LWIP_NO_STATIC` 是否被定义、在哪定义

**是，被定义为 `0`。** 全部 6 个平台配置头都显式定义：

| 文件 | 行号 | 原文 |
|---|---|---|
| `include/platform/lwip/arch/lwipopts_crane.h` | **218** | `#define LWIP_NO_STATIC                  0 //close lwip static flag, default set is disable; open static` |
| `include/platform/lwip/arch/lwipopts_c4m.h` | 234 | 同上 |
| `include/platform/lwip/arch/lwipopts_minis.h` | 225 | 同上 |
| `include/platform/lwip/arch/lwipopts_nb.h` | 221 | 同上 |
| `include/platform/lwip/arch/lwipopts_alios.h` | 218 | 同上 |
| `include/platform/lwip/arch/lwipopts_1802.h` | 202 | 同上 |

**本底包实际走哪一支**：`lwip_api.h:11-15` → `#include "opt.h"` → `opt.h:45` → `#include "lwipopts.h"` → `lwipopts.h:203-217` 的分派链：

```
#if IS_LWIP_PLATFORM_C4M        → lwipopts_c4m.h
#elif IS_LWIP_PLATFORM_MINIS    → lwipopts_minis.h
#elif IS_LWIP_PLATFORM_USE_ALIOS→ lwipopts_alios.h
#elif IS_LWIP_PLATFORM_NBIOT    → lwipopts_nb.h
#elif IS_LWIP_PLATFORM_CRANE    → lwipopts_crane.h   ← 命中
#elif IS_LWIP_PLATFORM_1802_1802S → lwipopts_1802.h
#else                           → lwipopts_crane.h
```

`IS_LWIP_PLATFORM_*` 由 `lwipopts.h:95-102` 依据 `LWIP_PLATFORM_DEFINE`（`lwipopts.h:44-93`）计算。判定 **CRANE** 的证据链：

1. `lwipopts.h:53`：`#define LWIP_PLATFORM_DEFINE (LWIP_PLATFORM_DEFINE_CRANE)` 为默认值（`0x0004`）。
2. `.axf` 内构建命令行含 `-DCRANE_MCU_DONGLE`、`-DML307A_SUPPORT`、`-DCRANE_CUST_BUILD`、`-DCRANE_Z2`。
3. `lwipopts.h:67-70` 的 NBIOT 分支条件是 `#ifdef LWIP_NBIOT`；**构建命令行中不存在 `-DLWIP_NBIOT`**（已逐字检索）。
4. 版本串自述 `CRANE_CAT1_CP_`、`CRANE_PLT`、`CRANE_GIT_LTEONLY_CP_`（Cat.1，符合 ML307A）。

⇒ `IS_LWIP_PLATFORM_CRANE == 1`、`IS_LWIP_PLATFORM_NBIOT == 0`、`IS_LWIP_PLATFORM_C4M == 0`（命令行无 `-DC4M`/`-DLTEONLY_THIN`）⇒ 采用 `lwipopts_crane.h` ⇒ `LWIP_NO_STATIC == 0`。

### 3.2 因此 `#if !LWIP_NO_STATIC` 的编译结果

`lwip_api.h:414` `#if !LWIP_NO_STATIC` → `!0` → **真**。所以 `lwip_api.h:414-443` 整块**被保留**：

| 声明 | 头文件可见? | 底包符号存在? | 综合可用性 |
|---|---|---|---|
| `lwip_nwst_get_current_speed(void)` | ✅ | ✅ L58607 | **可用** |
| `lwip_nwst_get_used_data(simid, *MB, *B)` | ✅ | ✅ L58608 | **可用** |
| `lwip_nwst_clear_used_data(simid)` | ✅ | ✅ L58609 | **可用** |
| `lwip_nwst_set_cutoff_data(simid, MByte)` | ✅ | ✅ L58610 | **可用** |
| `lwip_nwst_set_reminder_data(simid, MByte)` | ✅ | ✅ L58611 | **可用** |
| `lwip_nwst_set_callback(simid, cb)` | ✅ | ❌ | **不可用**（链接期 `undefined symbol`） |
| `lwip_nwst_get_cutoff_data(simid, *out)` | ✅ | ❌ | **不可用**（链接期 `undefined symbol`） |
| `lwip_nwst_get_reminder_data(simid, *out)` | ✅ | ❌（连名字表都没有） | **不可用** |
| `lwip_nwst_set/get_update_flash_*` | ❌（被 `#if 0` 包裹，`lwip_api.h:432-441`） | — | **不可用（源码已注释禁用）** |

**关键结论**：`LWIP_NO_STATIC=0` 只决定**头文件是否声明**，不决定**底包是否导出**。本次实测出现"声明在、符号不在"的组合，因此**不能用头文件条件编译结论推断链接可用性**，必须以 map 为准。

### 3.3 `nwst_callback_fn` 回调类型定义位置与原型

**未证实。** 检索事实（穷尽性检索 `sdk/` 及 `docs/` 全树 `*.h`/`*.c`/`*.html`）：

1. **SDK 全树中 `nwst_callback_fn` 仅出现 1 次**：
   `include/platform/lwip/lwip/lwip_api.h:428:  void lwip_nwst_set_callback(u8_t simid, nwst_callback_fn cb);`
   —— 只有**使用**，没有任何 `typedef ... nwst_callback_fn`。
2. `lwip_api.h:15` 有 `#include "Dialer_Task.h"`，但 **`Dialer_Task.h` 不存在于 SDK 的 `include/` 树**（全树按文件名检索无结果）。
3. `.axf` 的 `.debug_info`（`0x25411b0` 附近）中可见该头文件路径与类型名，证明编译底包时确实存在过该头文件：
   ```
   W:\tavor\Arbel\obj_PMD2NONE\inc\Dialer_Task.h.Component: ARM Compiler 5.05 (build 41) Tool: armcc [4d0eb9]....
   ...int....unsigned long....char....unsigned int.uvoid."...Pnwst_callback_fn...........E_Dialer_Cmd_Msg...DIALER_AT_COMMAND....DIALER_AT_IND
   ```
   → 该类型定义在**编译底包时的 `Dialer_Task.h`** 里（内部构建路径 `W:\tavor\Arbel\obj_PMD2NONE\inc`），**本 SDK 交付物中没有**。
4. `Dialer_Task.h` 同时声明了 `lwip_nwst_event` 相关宏/类型；`lwip_stats.h:9-11` 只给出**事件位**（非回调类型）：
   ```c
   /** define lwip_nwst_event, using hotbit format, for bit using also */
   #define LTE_ST_EVENT_CUTOFF_DATA        (0x1)
   #define LTE_ST_EVENT_REMINDER_DATA      (0x2)
   ```

**能说的**：回调语义是"流量阈值事件通知"，事件位就是 `LTE_ST_EVENT_CUTOFF_DATA(0x1)` / `LTE_ST_EVENT_REMINDER_DATA(0x2)`。
**不能说的**：`nwst_callback_fn` 的**确切原型（参数表/返回值）**。**未证实** —— 且即使查到原型，`lwip_nwst_set_callback` 本身也**没链接进底包**，所以该 API 在本底包上**根本不可用**。

---

## 4. RNDIS 与 USB 流量共享的推荐调用序列

> 本节只使用 §1 中**已核实存在**的函数。所有"推断"已显式标注。

### 4.1 include 清单

```c
/* 1) 必需：netif 层 USB 网卡状态与句柄（netif_usb_get_status/set_status/netif_get_usb_if） */
#include "netif.h"          /* <sdk>/include/platform/lwip/lwip/netif.h */

/* 2) 必需：转发开关 + ifconfig/iflist（lwip_* 公共 API 的主头文件） */
#include "lwip_api.h"       /* <sdk>/include/platform/lwip/lwip/lwip_api.h */

/* 3) 建议：网络就绪等待（lwip_select_netif_ready 在 minis.o，但声明在 lwip_api.h） */
/*    已含于 lwip_api.h —— L411 */
```

**编译期需要添加的 include 路径**（按仓库内既有分区，`#include "netif.h"` / `#include "lwip_api.h"` 是**扁平名**，所以要让两个目录同时在搜索路径上）：

```
<sdk>/include/platform/lwip/lwip
<sdk>/include/platform/lwip/arch
<sdk>/include/platform/lwip/ipv4
```

**关于这 3 个 RNDIS 函数的声明可见性 —— 有一个陷阱，必须说明：**

`lwip_api.h` 中它们**出现了两次，两处的条件编译结论相反**：

| 位置 | 原文所在保护 | 本底包（NBIOT=0）下是否可见 |
|---|---|---|
| `lwip_api.h:148-155` | `#if IS_LWIP_PLATFORM_NBIOT` | ❌ **不可见**（条件为 0） |
| `lwip_api.h:387-390` | **无任何 `#if` 保护**（裸声明） | ✅ **可见** |

```c
/* lwip_api.h:387-390 —— 无保护，编译期一定可见 */
void lwip_do_rndis_hotplug_now(void);
void lwip_do_rndis_hotplug(void);
void lwip_rndis_hotplug_break(void);
void lwip_rndis_hotplug_link(void);
```

⇒ **结论：只要 `#include "lwip_api.h"`，这 3 个函数在编译期就是有声明的，无需自写 `extern`。**
（§1.4 表中"头文件不可见"的表述仅适用于 148-155 那一份重复声明。）
若某些构建配置下 `lwip_api.h:387-390` 因其他原因未生效，可用下面的备份声明，符号本身已确认链接进底包，不会产生链接错误：

```c
/* 备份声明：符号已核实存在（L58579 / L58580 / L58597） */
extern void lwip_rndis_hotplug_link(void);      /* map L58579 0x80342797 */
extern void lwip_rndis_hotplug_break(void);     /* map L58580 0x803427e7 */
extern void lwip_do_rndis_hotplug_now(void);    /* map L58597 0x803441ab */
```

**不要 include / 不要调用**（已核实不存在，会链接失败）：`lwip_set_rndis_hotplug_flag`、`lwip_get_rndis_hotplug_flag`、`lwip_get_nw_status`、`lwip_select_default_nw_ready`、`lwip_get_netif_pdp_if`、`lwip_get_netif_by_hostname`、`netif_get_usb_packets`、`get_ip_flow_stats`。

### 4.2 推荐调用序列（开启 RNDIS / USB 流量共享）

```c
#include "netif.h"
#include "lwip_api.h"

extern void lwip_rndis_hotplug_link(void);
extern void lwip_do_rndis_hotplug_now(void);

int rndis_share_enable(void)
{
    struct netif *usb = netif_get_usb_if();          /* L34208 存在 */
    if (usb == NULL) {
        return -1;                                   /* USB/RNDIS netif 未注册 */
    }

    /* 步骤 1：允许把包转发到 USB 网卡（开路由/转发总开关） */
    lwip_set_usb_forward_flag(1);                    /* L47499 存在；lwip_api.h:189 */
    lwip_set_ip_forward_flag(1);                     /* L47503 存在；lwip_api.h:188 */

    /* 步骤 2：把 USB 网卡置为可用（netif up） */
    netif_usb_set_status(1);                         /* L34229 存在；netif.h:570 */

    /* 步骤 3：给电脑侧网卡配地址/掩码（配套 dhcpd，见 §2.3） */
    lwip_ifconfig("pc", "ip",      "192.168.0.1");   /* L58594 存在；lwip_api.h:123 */
    lwip_ifconfig("pc", "netmask", "255.255.0.0");   /* 取值依据 §2.3 帮助文本 */
    lwip_ifconfig("pc", "gateway", "192.168.0.1");

    /* 步骤 4：通知主机侧重新枚举/插入（RNDIS 热插拔） */
    lwip_rndis_hotplug_link();                       /* L58579 存在 */
    lwip_do_rndis_hotplug_now();                     /* L58597 存在 */
    return 0;
}
```

关闭：

```c
extern void lwip_rndis_hotplug_break(void);

void rndis_share_disable(void)
{
    lwip_rndis_hotplug_break();                      /* L58580 存在 */
    netif_usb_set_status(0);                         /* L34229 存在 */
    lwip_set_usb_forward_flag(0);                    /* L47499 存在 */
    lwip_set_ip_forward_flag(0);                     /* L47503 存在 */
}
```

### 4.3 读取状态 / 等待就绪（均已核实存在）

```c
int  st = netif_usb_get_status();                    /* L34228；netif.h:569，返回 int */
int  fw = lwip_get_usb_forward_flag();               /* L47500；lwip_api.h:190 */
int  ip = lwip_get_ip_forward_flag();                /* L47504；lwip_api.h:187 */

char name[64];
lwip_iflist("pc", "ip", name);                       /* L58596；lwip_api.h:143，caller 缓冲建议 ≥64B */
char *usbname = (char *)lwip_get_netif_usb_ifname(); /* L58588；lwip_api.h:83 */
int  ok      = lwip_ifexist("pc");                   /* L58591；lwip_api.h:110，1=存在 */

/* 等 USB/指定 netif 就绪；返回值语义见 lwip_api.h:93 注释：0 = ready, <0 = not ready */
int r = lwip_select_netif_ready(0, "pc", 5000);      /* L47521，单位 ms */
```

### 4.4 三者区别：`lwip_set_usb_forward_flag` vs `netif_usb_set_status` vs `lwip_rndis_hotplug_link/break`

| 维度 | `lwip_set_usb_forward_flag(int)` | `netif_usb_set_status(u8_t)` | `lwip_rndis_hotplug_link()/break()` |
|---|---|---|---|
| 头文件 | `lwip_api.h:189` | `netif.h:570` | `lwip_api.h:151/153`（**在 `#if IS_LWIP_PLATFORM_NBIOT` 内，本底包不可见**） |
| 归属 .o | `lwip_minis.o`（**L47499**，代码仅 10 B） | `netif.o`（**L34229**，148 B） | `lwip_api.o`（**L58579/L58580**，各 80 B） |
| 返回值 | `void` | `err_t` | `void` |
| 同名变量 | `lwip_usb_forward_flag` **L29733** `Data 4` | — | `lwip_rndis_hotplug_flag` **L29720** `Data 4`（**无 setter/getter 函数**） |
| 作用层级（**推断**） | lwIP 转发/路由**策略位**：是否允许把报文转发给 USB netif | **netif 生命周期/链路状态**：USB netif 的 up/down | **USB 主机侧重枚举事件**：通知电脑端"网卡插入/拔出" |
| 依据 | 命名 + 10 B 体量（典型 flag setter）+ 存在同名 4 B 全局变量 | `netif.h` 命名族规律：`netif_usb_set_status` / `netif_uap_set_status`(L572) / `netif_ppp_set_status`(L574) 与 `netif_set_up/down`(L549/550) 同族 | 命名 `hotplug` + `link`/`break` 对应"接入/断开"；`lwip_do_rndis_hotplug_now`(L58597, 148 B) 是实际执行体 |

**明确标注为推断的部分**（未能从底包内证据证明）：
1. `lwip_set_usb_forward_flag` 的**参数取值语义**（0/1 分别代表什么）—— 头文件**无任何注释**。**推断** 1=启用转发、0=停用。
2. `netif_usb_set_status` 的**入参取值范围** —— `netif.h:570` 只写 `u8_t status`，无注释。**推断** 1=up、0=down（与同族 `netif_set_up/down` 一致）。
3. 三者**是否需要同时调用、调用顺序是否强约束** —— 底包内无调用样例（`examples/` 28 个 `.c`、`src/` 3 个 `.c` 中，`lwip_set_usb_forward_flag` / `netif_usb_set_status` / `lwip_rndis_hotplug*` **全部 0 次引用**）。§4.2 的顺序是**推断**，需实测验证。
4. `lwip_rndis_hotplug_link/break` 与 `lwip_do_rndis_hotplug_now` 的**关系**：`link/break` 只是置位/清位，`..._now` 是立刻执行 —— 依据是 `lwip_do_rndis_hotplug`(L58598, 仅 12 B，近似 thunk) 与 `..._now`(148 B) 的并存。**推断**。
5. `lwip_rndis_hotplug_flag` 变量（L29720）**无对应 setter/getter**，因此**无法通过公开 API 读写它** —— 这是**事实**（map 检索结果），不是推断。

### 4.5 流量统计的可用调用序列（替代不可用的 `nwst_set_callback`）

由于 `lwip_nwst_set_callback` / `lwip_nwst_get_cutoff_data` 不存在，**轮询式**统计是唯一稳妥路径：

```c
#include "lwip_api.h"      /* lwip_nwst_* 声明在此 (L414-430) */

void traffic_poll(uint8_t simid)
{
    uint32_t speed = lwip_nwst_get_current_speed();      /* L58607 存在，单位 B/s */
    uint32_t mb = 0, b = 0;
    lwip_nwst_get_used_data(simid, &mb, &b);             /* L58608 存在 */

    lwip_nwst_set_cutoff_data(simid, 100);               /* L58610 存在，单位 MByte；0=禁用 */
    lwip_nwst_set_reminder_data(simid, 80);              /* L58611 存在，单位 MByte；0=禁用 */

    lwip_nwst_clear_used_data(simid);                    /* L58609 存在 */
}
```

单位与语义（**来自 `lwip_api.h` 注释，可直接引用**）：
- `lwip_nwst_get_current_speed` → "return speed in Bytes per second"（L418）
- `lwip_nwst_get_used_data` → "return used data in MBytes + Bytes"（L420）
- `lwip_nwst_set_cutoff_data` → "cutoff_data, unit is Mbyte, if config 0, denote disable cutoff_data check"（L423）
- `lwip_nwst_set_reminder_data` → "reminder_data, unit is Mbyte, if config 0, denote disable reminder_data check"（L425）

---

## 5. 未证实 / 需实测确认清单

| # | 待确认项 | 现状 | 建议确认方式 |
|---|---|---|---|
| 1 | **RNDIS 电脑侧默认网关到底是多少** | 底包内**无 `192.168.0.1`**；只有 AT 帮助文本给出的 `192.168.0.1/255.255.0.0`。运行期真实默认值**未证实** | 实机插 USB，在 PC 上 `ipconfig` 读默认网关；或 `at*lwipctrl=system, dhcpserver` 查询；或 `lwip_iflist("pc","gateway",buf)` 打印 |
| 2 | 底包 NVRAM 里是否存有另一套 IP | 未证实（NVRAM 内容不在 `.axf` 中） | 实机查询 dhcpd 当前配置 |
| 3 | `lwip_get_netif_usb_ifname()` 实际返回的字符串 | **未证实**（`.axf` 中未定位到独立 `"usb\0"` 字面量） | 实机打印返回值 |
| 4 | `lwip_set_usb_forward_flag` 参数语义（0/1） | **推断**（头文件无注释） | 实测：置 0/1 观察 PC 能否上网 |
| 5 | `netif_usb_set_status` 入参范围 | **推断**（`u8_t status` 无注释） | 实测 0/1 |
| 6 | §4.2 调用顺序是否必须、是否需额外调用 | **推断**（底包内 0 处调用样例） | 实测；也可开 `__trace_MIFI__LWIP__*` 日志观察 |
| 7 | `lwip_rndis_hotplug_link/break` 与 `..._now` 的分工 | **推断** | 实测；反汇编 `0x80342797`/`0x803427e7` 看是否仅置位 |
| 8 | `lwip_nwst_set_callback` 是否在**其他底包变体**中存在 | 本底包**确认不存在**；其他变体未核实 | 换底包时重新 grep map |
| 9 | `nwst_callback_fn` 原型 | **未证实**（类型头文件 `Dialer_Task.h` 未随 SDK 交付） | 向原厂索取 `Dialer_Task.h`；但该 API 在本底包不可用，优先级低 |
| 10 | `get_ip_flow_stats` / `netif_get_usb_packets` 是否有替代 API | 二者**均不存在**；可用替代见 §1.9（`ip_flow_stats_rx/tx`、`lte_data_statistic_*`） | 若需按 IP 分流统计，需自建表（可参考 `lwip_stats.h:13-38` 的结构体定义） |
| 11 | `lwip_get_nw_status` / `lwip_get_netif_pdp_if` / `lwip_get_netif_by_hostname` / `lwip_select_default_nw_ready` 的替代 | 均**未链接**。`lwip_select_netif_ready`(L47521) 与 `lwip_ifexist`(L58591) 可用 | 用 `lwip_iflist` 读 gateway/ip 自行判断就绪 |
| 12 | RNDIS 功能是否受 NVRAM/GPIO/AT 开关前置约束 | **未证实**（未在底包中找到开关） | 实测；必要时查原厂《ML307A RNDIS 应用指南》 |

---

## 附：本次核实所用命令与可复现要点

```powershell
$map = "...\prebuild\baseline\ML307A\DSLN\ML307A-DSLN-MBRH1S00.map"
# 单符号存在性（注意 map 中同名的 veneer/Section/Data 条目）
Select-String -Path $map -Pattern "\blwip_nwst_set_callback\b"
# 归属库归档
Select-String -Path $map -Pattern "pcaconfigs-lwipv4v6\.lib\(lwip_api\.o\)"
```

`.axf` 字符串扫描：Python `mmap` 全文件，正则 `\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}` + `192\.168` + 掩码，并对关键 IP 追加**原始字节两种字节序**扫描（避免 ASCII 扫描漏掉二进制常量）。
ELF 段归属：手工解析 `.shoff/.shentsize/.shnum/.shstrndx`，把字符串文件偏移映射到段名（用于区分 `.debug_info` / `DDR_RO_EXEC` 名字表 / 真实代码段）。

**未联网**：全部结论来自本地 map / axf / include 树。
