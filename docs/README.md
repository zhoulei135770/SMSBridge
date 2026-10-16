# ML307A-DSLN 4G 短信网关 · OpenCPU 固件交付说明

> 目标硬件：**ML307A-DSLN**（ASR1606，DS 系列）· 底板 DEMO-ML307X-V1.04
> 开发环境：中移 ML307A OpenCPU SDK 1.4.2（**已随本工程克隆到 `sdk/`**）
> 本文件是入口索引，读完这一页你就知道工程在哪、怎么编译、结论是什么。

---

## 一、最重要的三件事（先说结论）

### 1. 你其实已经有 SDK 了，而且这台机器已经成功编译出可烧录固件

在本次工作中已把 SDK 克隆到 `sdk/ML307A_OpenCPU_Standard_1.4.2/`（1.3 GB，4579 个文件），
并**完整跑通了官方编译链路**，产物为：

```
sdk/ML307A_OpenCPU_Standard_1.4.2/out/images/ML307A/DSLN/
├── ML307A_DSLN_APP.elf     应用 ELF（带调试符号）
├── ML307A_DSLN_APP.bin     应用镜像（供打包）
├── ML307A_DSLN_APP.map     链接 map（查符号/占用）
└── ML307A_DSLN_APP.zip     ★ 最终烧录包（含全部分区镜像 + download.json）
```

### 2. 你的需求文档里有 4 个前提是错的，必须改（否则会白花时间）

| 需求文档里的说法 | 实际情况（证据） |
|---|---|
| "QFlash 烧录 `.pac` 固件" | 本 SDK **根本不产生 `.pac`**。`pack.bat` 用 ASR 的 `arelease.exe` 生成 **`.zip`** 烧录包；`.pac` 是展锐(UNISOC)格式。你下载的 QFlash 是**移远 Quectel** 工具（主体是展锐下载器换皮），全工具内对 `ML307`/`CRANEL`/`ASR1606` **零命中**，能否烧本模组未知。正确路线见文档 01。 |
| "Web 访问 `http://192.168.4.1:8080`" | `192.168.4.1` 在底包里**完全不存在**（ASCII 与两种字节序均 0 命中）。SDK 里唯一的默认网关证据是 `192.168.0.1`（掩码 `255.255.0.0`）。固件会**在获取到 IP 后打印真实访问地址**，以实测为准。 |
| "Flash 1MB / RAM 3135KB" | 权威依据是 `prebuild/ld/ML307A/DSLN/app_dsln.ld`：**应用可用 flash 1024K / ram 512K**。"1MB" 指 APP 分区（DSLN 整片 NOR 是 8 MiB）；"3135KB RAM" 在 SDK 里**找不到依据**。 |
| "Keil 编译" | **完全不需要 Keil**。SDK 自带 `arm-none-eabi-gcc 8.3.1` + `make.exe` + `python3` + `arelease.exe`，官方流程是批处理 + make。本机从未安装 Keil，编译照样通过。 |

### 3. 关于 RNDIS 开关与短信接收：两个关键答案

- **RNDIS 运行时开关：应用层做不到**。这不是"待确认"，是链接器**实证**：
  用户 APP 的可调用符号边界是 `prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o`（326 个符号），
  其中 usb / rndis / netif 相关**一个都没有**。内核 map 里能看到 `rndis.o`、`netif_usb_set_status` 等，
  但那是内核镜像自己的，**没有导出给应用**，调用必然 undefined reference。
  → 固件把开关**保存为配置期望值**并如实提示"由底包/NV 决定，重启后生效"。
- **短信接收：SDK 没有收短信 API**，只有发送 `cm_virt_at_sms_send()`。
  接收必须 `cm_virt_at_urc_reg()` 注册 URC 回调 + `AT+CMGF=0` + `AT+CNMI=2,2,0,0,0`，
  由固件自行解析 PDU。本工程已按此实现（并保留 `+CMTI`→`AT+CMGR` 兜底路径）。

---

## 二、30 秒快速开始

```powershell
# 1) 主机单元测试 + 栈预算检查（不需要模组，844 项断言）
pwsh -File app/test/run_host_tests.ps1

# 1b) 只跑栈预算检查（-fstack-usage + 调用链求和，防大对象上栈）
pwsh -File app/test/check_stack_budget.ps1

# 2) 一键编译 + 打包（自动部署源码进 SDK）
pwsh -File tools/build_firmware.ps1

# 3) 只部署源码不编译
pwsh -File tools/deploy_to_sdk.ps1
```

---

## 三、交付物清单

| 路径 | 内容 |
|---|---|
| `app/` | **固件源码**（19 个文件，模块化 C99）。这是源码的唯一来源，编译前自动部署进 SDK |
| `app/inc/` `app/src/` | 头文件与实现 |
| `app/test/` | 主机单元测试：`host_tests.c`（844 断言，含短信远程配置模块 2/3/4/5/6 + 时间解析 + IPv4/IPv6 地址解析）+ `gen_pdus.py`（独立 PDU 编码器，48 用例）+ `check_stack_budget.ps1`（栈预算自动检查） |
| `tools/deploy_to_sdk.ps1` | 把 `app/` 部署进 SDK 并生成 `custom/custom.mk` |
| `tools/build_firmware.ps1` | 一键编译 + 打包（已修掉官方脚本在本机的两个坑） |
| `sdk/` | ML307A OpenCPU SDK 1.4.2（含预编译底包与工具链） |
| `docs/01-编译与烧录指南.md` | 编译环境、产物、烧录器选型、下载模式、日志串口 |
| `docs/02-设计与API映射.md` | 架构、模块职责、SDK API 映射、链接边界、内存占用 |
| `docs/03-需求对照与差异说明.md` | **逐条对照你的需求 + 差异与理由（优先看这份）** |
| `docs/04-上板验收清单.md` | 上板实测步骤与判定标准（含必须人工确认的 6 项） |
| `docs/10-短信远程配置-实现进度与交接.md` | 短信远程配置 + 钉钉转发的进度、实现细节与踩坑记录 |
| `docs/11-IPv6与公网IP-证据实现与上板验证.md` | **IPv6 实时开启 + 公网 IP**：能力证据（出厂实机回显/链接边界/反汇编）、实现、必须上板确认清单 |
| `docs/12-上板记录-首次成功烧录与IPv6实测.md` | **上板记录**：app-only 烧录流程、软下载入口（免短接）、烧录前后对比、IPv6 公网地址端到端实测、面板不可达的遗留问题 |
| `docs/research/` | 调研原始报告（SDK/AT/烧录/符号核实，含来源 URL） |
| `docs/sdk-reference/` | `api-cmiot.md`（cmiot API 速查）、`app-link-symbols.txt`（**应用可调用 326 符号全清单**） |

---

## 四、代码模块一览（对应需求"五、非功能需求 4. 代码结构模块化"）

| 需求要求的模块 | 对应文件 | 说明 |
|---|---|---|
| 主入口 | `main.c` | `cm_opencpu_entry()`，初始化编排 + 创建 4 个任务 |
| APN 网络管理 | `net_mgr.c/.h` | APN 轮询切换、TCP 心跳保活、状态维护（唯一发起 AT 的任务） |
| 短信 PDU 解析 | `sms_pdu.c/.h` | GSM 03.40/03.38 解码，纯 C，**48 用例独立编码器往返验证** |
| 短信服务 | `sms_svc.c/.h` | URC 注入→拼行→解码→黑白名单→环形缓存→Webhook，`+CMTI` 兜底 |
| Web 服务 | `web_server.c/.h` + `web_ui.c/.h` | 裸 TCP HTTP 服务 :8080，登录会话、路由、表单解析、页面渲染 |
| Flash 配置读写 | `app_config.c/.h` + `app_ctx.c/.h` | 结构体 + magic/version/CRC32，主/备/临时三文件原子写 |
| 流量统计模块 | `traffic.c/.h` | 平台字节计数 + 应用侧基线差值，月度限额、告警、可选断网 |
| USB RNDIS 控制模块 | `usb_rndis.c/.h` | 配置期望值 + 状态展示 + 降级提示（应用层无法切换） |
| Webhook 推送 | `webhook.c/.h` | 企业微信/钉钉/通用 JSON Markdown 组装 + 发送 |
| 短信远程配置（**新增功能**，见 `docs/09`、`docs/10`） | `sms_cmd.c/.h`、`sms_rcfg.c/.h`、`sms_admin.c/.h`、`sms_concat.c/.h`、`app_time.c/.h` | 管理员短信指令解析与路由 + `rcfg.bin` 持久化 + 长短信分包/重组 + `AT+CSCA?` 取短信中心回执；**已全量集成进 `sms_svc`**，转发侧支持模板渲染、`【关键词】`注入、钉钉加签（时钟不可信时自动降级）与 `AT+CCLK?`/NTP 对时 |
| IPv6 与公网地址（**新增功能**，见 `docs/11`） | `net_addr.c/.h`、`net_mgr.c/.h`、`sdk_port.c/.h` | **实时开启 IPv6 双栈**（`AT+CGDCONT=1,"IPV4V6"` 重新激活，Web 一键）；`AT+CGPADDR` 一次读 IPv4/IPv6；公网出口 IP 走 HTTP 回显（**强制 v4 优先**，避免 DNS 默认 v6 优先导致量错对象）；面板显示 IPv6/公网 IPv4/IPv6 出口 + PDP 类型 |
| SDK 适配层 | `sdk_port.c/.h` | **唯一** include SDK 头文件的文件；换 SDK 只改这里 |
| 其它 | `app_log.c/.h`、`sha256.c/.h` | UART 日志、口令摘要 |

---

## 五、当前状态与边界

**已完成并有证据**
- 官方编译链路跑通（`.elf`/`.bin`/`.zip` 全部产出），全文无错误
- 主机单元测试 **844 项断言全部通过**（含 PDU 往返、配置序列化、缓冲区越界哨兵、XSS 转义、
  短信远程配置的指令解析、`rcfg.bin` 坏文件回退、管理员/非管理员路由越权检查、
  转发组装（模板/关键词/加签/降级）、时间解析，以及 IPv4/IPv6 地址解析与 `AT+CGPADDR` 双栈形态）
- 测试过程中发现并修复了 **3 个真实缺陷**（见 `docs/02` 的"测试发现的问题"一节）
- **栈预算自查揪出并修掉一类"只会在板上崩"的问题**：`sdk_task_create()` 把任务栈钳制为 3072，
  而集成后 `handle_pdu`/`sms_admin_handle`/`sms_concat_build_tx`/`sms_svc_export` 的帧合计达 5~6KB。
  大对象已全部改静态区，并加了自动防线 `app/test/check_stack_budget.ps1`（详见 `docs/10` §五）
- 固件实测占用：text 170,608 / bss 166,088（APP 分区 1024K、RAM 512K），见 `docs/10` §5.1
- **IPv6 双栈与公网地址已实现**（`docs/11`）：设备出厂即为 `IPV4V6`（实机回显已证实），
  本工程支持 Web **一键实时开启/关闭**；公网 IPv6 由 PDP 全局地址直接给出，公网 IPv4 走 HTTP 回显
  （已修掉"DNS 默认 v6 优先导致量错对象"的缺陷）

**必须上板才能确认的 10 项**（清单与判定方法见 `docs/04`、`docs/11` §五）
1. `get_rx_byte_all()` / `get_tx_byte_all()` 的语义与单位（未文档化导出，链接可用）
2. `+CMT` URC 的实际投递形态（是否需要重发 `AT+CNMI`、URC 是否只在通道 init 期间送达）
3. RNDIS 网卡在电脑侧的真实网关 IP（决定 Web 面板地址）
4. 烧录器与下载模式入口（优先试 `AT$MYDOWNLOAD=1` 软入口）
5. `cm_log_printf` 在本底包上是否真的从 UART0 输出
6. 看门狗是否由底包自动喂狗（或需要应用层显式喂狗）
7. `cm_virt_at_sms_send()` 第一参 `atCmgs` 的真实语义（本工程按"PDU 八位组长"发送，见 `docs/10` §2.2）
8. NTP 对时能否连通：`cm_ntp_sync()` 依赖 asocket + async_dns + eloop（本工程已投递到 eloop 线程执行），
   失败时加签自动降级；同时可用 `AT+CCLK?`（模组时钟）作为兜底
9. ~~`AT+CGPADDR=1` 在 `IPV4V6` 下是否返回两个引号字段~~ → **2026-10-15 已实测证实**：
   `+CGPADDR: 1,"10.104.34.201","2409:xxxx:xxxx:xxxx::1"`（见 `docs/12`）
10. 公网 IP 回显服务在 CMCC 卡上的**可达性与返回地址族**（面板"刷新公网 IP"；
    若显示"公网 IPv6 出口"，说明该域名走的是 v6 出口，需换 URL）

> **2026-10-15 上板结论**（详见 `docs/12`）：新固件已用 app-only 包成功烧入并稳定运行；
> IPv6 公网地址端到端可用（PC 经 RNDIS 拿到同前缀公网 v6，可连通公网 IPv6）；
> **遗留问题**：Web 面板 8080 / 探针 8081 在 RNDIS 侧只建立 TCP、不返回数据（既有的"栈分离"问题，待修）。


