# 02 · 设计与 API 映射

---

## 一、架构分层

```
                    ┌──────────────────────────────────────────┐
   业务层（纯 C99，  │ main.c  net_mgr  sms_svc  traffic        │
   可在 PC 上单测）  │ webhook  web_server  web_ui  usb_rndis   │
                    │ app_config  app_ctx  sms_pdu  sha256     │
                    │ app_log                                   │
                    └───────────────────┬──────────────────────┘
                                        │  只依赖 sdk_port.h
                    ┌───────────────────▼──────────────────────┐
   SDK 适配层        │ sdk_port.c  ★唯一 include SDK 头文件的文件 │
                    └───────────────────┬──────────────────────┘
                                        │
        ┌───────────────────────────────┼───────────────────────────────┐
        │ cmiot OpenCPU API             │ lwip socket API               │
        │ cm_os / cm_fs / cm_http /     │ lwip_socket/bind/listen/      │
        │ cm_virt_at / cm_pm / cm_rtc / │ accept/select/recv/send/      │
        │ cm_sys / cm_modem_info        │ gethostbyname                 │
        └───────────────────────────────┴───────────────────────────────┘
                                        │
                    预编译底包（cm_stub_DSLN.o + src/cm_lib/*）
```

**设计意图**：把 SDK 依赖压缩到 `sdk_port.c` 一个文件。好处有二：
1. 其余模块是纯 C，可以在 PC 上用 gcc 直接跑单元测试（本工程 549 项断言就是这么来的）；
2. 换 SDK 版本时只改一个文件。

---

## 二、⚠ 应用可调用符号边界（本次工作最重要的技术结论）

用户 APP **不是**链接整个底包，而是只链接 `prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o`
（**326 个符号**）+ `src/cm_lib/*`。完整清单见 `docs/sdk-reference/app-link-symbols.txt`。

| 能力 | 边界内 | 证据 / 后果 |
|---|---|---|
| `cm_os` 任务/互斥/信号量/定时器（CMSIS-RTOS2 风格） | ✅ | 创建 4 个任务 |
| `cm_fs` 文件系统（open/read/write/close/move/exist/delete） | ✅ | 配置持久化 |
| `cm_httpclient_*`（含同步接口、SSL 开关） | ✅ | Webhook 推送 |
| `cm_virt_at_*`（init/send/get/deinit/urc_reg/sms_send） | ✅ | AT 通道 + 短信 |
| `cm_pm_reboot` / `cm_pm_get_power_on_reason` | ✅ | 重启 / 识别 WDT 复位 |
| `cm_rtc_get_current_time/timezone` | ✅ | 时间戳、跨月判断 |
| `cm_sys_get_imei/get_cm_ver/cm_log_printf` | ✅ | 状态展示 + 日志 |
| **BSD socket 全套**（`lwip_socket/accept/select/recv/send/…`） | ✅ | 内置 Web 服务 |
| `lwip_gethostbyname` | ✅ | 心跳目标域名解析 |
| **`get_rx_byte_all` / `get_tx_byte_all`** | ✅ | 流量统计（未文档化但对应用导出） |
| `lwip_nwst_get_used_data/clear/cutoff/…` | ❌ | 头文件里有，**链接时报 undefined reference** |
| `netif_usb_get_status` / `netif_usb_set_status` | ❌ | 同上 → RNDIS 无法控制 |
| `lwip_rndis_hotplug_link/_break` / `lwip_set_usb_forward_flag` | ❌ | 同上 |
| `lwip_iflist` / `lwip_ifexist` / `lwip_get_default_nw_ifname` | ❌ | 同上 → 改用 `AT+CGPADDR` 取 IP |
| `get_ip_flow_stats` / `netif_get_usb_packets` | ❌ | 同上 |
| `watchdogKick/Activate/Configure` | ❌ | 同上 → 依赖底包 WDT |

**踩过的两个编译坑（已固化在代码注释里）**
1. **不要 `#include "lwip_api.h"`**：它会拉进 `ip_nat.h → net_bridge.h → dhcpd_api.h`，
   后者 `typedef unsigned int time_t;` 与 newlib 冲突（`conflicting types for 'time_t'`），
   并且 `ip_nat.h` 还 `#include "lwip_customer.h"` —— **该文件未随 SDK 交付**。
2. `netif.h` 会被 `lwip/sockets.h` 传递包含，若自己重复声明
   `netif_usb_set_status` 必须**逐字同类型**（`err_t` = `s8_t` = `signed char`），
   否则报 `conflicting types`。

---

## 三、模块清单

| 文件 | 职责 | 关键点 |
|---|---|---|
| `main.c` | `cm_opencpu_entry()` 初始化编排；创建 `sms_svc/net_mgr/traffic/web` 4 个任务 | 申请 6144，**实际被 `sdk_task_create()` 钳制为 3072/任务** |
| `sdk_port.c/.h` | SDK 隔离层 | 唯一 SDK 依赖；所有返回码归一化 |
| `net_mgr.c/.h` | APN 轮询、心跳、状态机 | **唯一发起 AT 的任务**（SDK 明确虚拟 AT 不支持多线程） |
| `sms_pdu.c/.h` | PDU 解码（GSM 03.40/03.38） | 纯 C；7bit+扩展表、UCS2、UDH 分片识别 |
| `sms_svc.c/.h` | URC→拼行→解码→过滤→缓存→推送 | URC 回调只入队（≤µs 返回），解析在任务里 |
| `app_config.c/.h` | 配置结构体、默认值、校验、CRC32 序列化、黑白名单语义 | 纯 C |
| `app_ctx.c/.h` | 配置单例 + 互斥锁 + 保存入口 | 读多写少 |
| `traffic.c/.h` | 字节计数基线差值、限额、告警、断网 | 应用侧实现，无需平台清零接口 |
| `webhook.c/.h` | 企业微信/钉钉/通用 JSON Markdown 组装与发送 | 组装是纯函数，可单测 |
| `web_server.c/.h` | HTTP 解析、会话、路由、表单 | 单连接串行；延迟动作避免自断连接 |
| `web_ui.c/.h` | HTML/CSS 渲染 | 全部动态内容 HTML 转义；缓冲上限经实测校准 |
| `usb_rndis.c/.h` | RNDIS 期望值 + 状态展示 | 如实提示"由底包决定" |
| `app_log.c/.h` | 分级日志 + hexdump | `[秒] 级别 模块: 内容` |
| `sha256.c/.h` | 口令摘要 | FIPS 180-4 向量验证 |

---

## 四、数据流

### 4.1 短信接收与推送（需求 3.3）
```
+CMT URC（SDK URC 线程）
  └─ sdk_port 的 urc_trampoline → sms_svc_on_urc()
       └─ 按 \r\n 拆行 → 入队 s_q[12][352] + post 信号量   ← 必须极快返回
sms_svc_task（独立任务）
  ├─ 状态机：+CMT: → 期待下一行 PDU；+CMTI: → AT+CMGR 兜底
  ├─ sms_pdu_decode()  7bit/UCS2 → UTF-8
  ├─ app_config_sms_allowed()  黑名单优先 → 白名单（空=不启用）
  ├─ store_record()  16 条 RAM 环形缓存（供 Web 展示/导出）
  └─ webhook_send_sms()  cm_httpclient 同步 POST
```

### 4.2 心跳保活（需求 3.1）
```
net_mgr_task 每 1s：
  ├─ 每 5s：AT+CEREG? / AT+CSQ / AT+CGPADDR=1 → 刷新状态（首次拿到 IP 打印 Web 地址）
  ├─ 未注册：累计失败 → 达阈值切下一个 APN
  ├─ 已注册但无 IP：重新 AT+CGDCAP 激活
  └─ 到心跳周期：sdk_tcp_probe(目标,端口,5s)
       ├─ 成功 → fail_count=0，online=1
       └─ 连续失败达阈值 → 切下一个 APN（并落盘）
```

### 4.3 流量统计（需求 3.4）
```
每次采样（默认 60s）：
  rx = get_rx_byte_all(); tx = get_tx_byte_all();
  回绕/重启检测：若 rx/tx 变小 → 重建基线
  Δrx / Δtx → 会话与本月累计（本月累计来自配置中的持久值 + 增量）
  速率 = Δtotal / Δt
  超过限额 → 1 次 Webhook 告警 +（可选）AT 断网 → 落盘
一键重置 = 应用侧清零并落盘（平台计数是只增计数，无需清零）
```

### 4.4 Web 请求（需求 3.5）
```
web_server_task: listen(8080) → accept(超时 2s) → 一次请求一个连接
  ├─ 读到 \r\n\r\n → 解析请求行 + Content-Length → 读 body
  ├─ 未登录：只有 /login 可用，其余回登录页
  ├─ GET /            状态+配置页（web_ui_status_page 渲染，>16KB 缓冲）
  ├─ POST /config     表单解析 → sanitize → 落盘
  ├─ POST /action/apn_next | traffic_reset | reboot | ping
  └─ GET /export      短信日志 txt 附件
延迟动作：关闭 RNDIS / 重启 都是**先发响应再执行**，否则浏览器只看到连接被重置
```

---

## 五、Flash 存储结构（需求 3.6）

不写裸扇区（SDK 不暴露该接口），改用 `cm_fs` 文件系统，**三文件原子写**：

| 文件 | 作用 |
|---|---|
| `cfg.bin` | 当前有效配置 |
| `cfgbak.bin` | 上一份有效配置（回滚用） |
| `cfgtmp.bin` | 写入中转 |

写流程：
```
1) 序列化 → 写 cfgtmp.bin → close 落盘 → **回读比对前 32 字节**
2) 旧 cfg.bin 改名为 cfgbak.bin
3) cfgtmp.bin 改名为 cfg.bin
任一步失败：还原备份，旧配置继续有效（不会因为掉电写坏而无法启动）
```

blob 布局（`app_config_blob_t`）：

| 偏移 | 内容 |
|---|---|
| 0 | `magic = 'ML37'` (0x4D4C3337) |
| 4 | `version = 0x00010001` |
| 8 | `payload_size = sizeof(app_config_t)` |
| 12 | `crc32`（IEEE 802.3，与 zlib 一致，基准向量 `crc32("123456789")==0xCBF43926`） |
| 16 | `app_config_t`（Webhook / 心跳 / 口令摘要 / 黑白名单 / 流量限额 / RNDIS / APN 下标 / 月度累计 / 月份标记） |

写入时机（严格限制，需求 6.3）：**保存配置 / 重置流量 / 超限阈值变化** —— 仅此三种。

---

## 六、内存占用（实测）

链接器输出（最终版本）：`text 124568 / data 2612 / bss 100712`（字节）

| 项 | 占用 | 上限 | 占比 |
|---|---|---|---|
| APP Flash（text+data） | ≈124.2 KB | 1024 KB | 12.1% |
| APP RAM（data+bss） | ≈100.9 KB | 512 KB | 19.7% |

主要静态缓冲（全部在编译期确定，无动态分配）：

| 模块 | 缓冲 | 大小 |
|---|---|---|
| `web_server` | `s_export` 短信导出 | 32768 |
| `web_server` | `s_page` 状态页渲染 | 24576 |
| `web_server` | `s_req` 请求 + `s_small` 登录页 + `s_body` 表单 | 4096+4096+3072 |
| `sms_svc` | `s_q[12][352]` URC 行队列 | 4224 |
| `sms_svc` | `s_rec[16]` 短信环形缓存 | ≈5120 |
| `sms_svc` | `pdu` 解码结构（static，避免爆栈） | ≈1400 |
| `sdk_port` | `s_ca_buf` HTTPS 根证书缓冲 | 8192 |
| 任务栈 | 4 个任务 | 6144×3 + 3072 |

如需进一步压 RAM：把 `s_export` 改为流式分块发送、或把 `WEB_UI_STATUS_PAGE_MAX` 调小
（改一个宏即可，`web_server.c` 用宏分配）。

---

## 七、关键设计决策

| 决策 | 理由 |
|---|---|
| 心跳用 TCP 连通性探测而非 ICMP | SDK 无 PING API；`AT+MPING` 可用性未证实；TCP 探测验证的正是 Webhook 那条数据通路 |
| 全部 AT 集中在 `net_mgr` 一个任务 | SDK 明确"虚拟 AT 不支持多线程"，且每次交互后重新注册 URC 以防注册丢失 |
| URC 回调只入队，不在回调里解析 | URC 回调运行在 SDK 线程上下文，必须极快返回，否则影响协议栈 |
| 用 `get_rx_byte_all/get_tx_byte_all` + 应用侧基线 | 头文件中的 `lwip_nwst_*` 不在链接边界内；且这样**不依赖平台清零接口**就能实现月度/会话/重置 |
| 口令只存 SHA-256 摘要，比对用常量时间 | 避免 Flash 中明文；默认口令 `admin123` 的摘要已用测试向量锁定 |
| 关闭 RNDIS / 重启先发响应再执行 | 否则会把用户自己的这条 TCP 连接断在半路，浏览器只看到连接重置 |
| HTML 动态内容全部转义 | 短信正文与号码是不可信输入（测试里注入了 `<script>` 与 `onerror=` 验证） |

---

## 八、测试发现并修复的 3 个真实缺陷

> 这些是"先主机测试再上板"直接换回来的收益 —— 否则会在板子上表现为
> "短信全是乱码 / 登录页没有输入框"这类难查的现象。

| # | 缺陷 | 表现 | 根因 |
|---|---|---|---|
| 1 | `decode_address()` 返回的已消费字节数漏算 TOA 字节 | 全部短信解码失败，报 `unsupported dcs`；正文乱码 | 地址字段是 `[长度][TOA][号码]`，应前进 `2 + octets`，原来写成 `1 + octets`，导致 SCA 与 TP-OA 各少 1 字节、后续字段整体错位 2 字节 |
| 2 | TP-SCA 长度语义用错 | 同上（叠加放大） | **TP-SCA 长度是"八位组数"，TP-OA 长度是"半字节数"**，两者语义不同（3GPP 23.040 §9.1.2.1）；原先两者共用一个解析函数 |
| 3 | `decode_scts()` 对时区字节也做了 BCD 校验 | 负时区短信整体解码失败，报 `invalid hex string` | 第 7 个字节是时区，低半字节含**符号位**（合法值可达 0xF），不能按 BCD 数字校验（`-5 区 = 0x0A`） |

**另外**：`WEB_UI_LOGIN_PAGE_MAX` 原定 2048，而登录页含内联 CSS 实际需要 **3253 字节**，
会导致页面在 `<style>` 中被截断、`<form>` 根本不输出 —— 即**浏览器上只有样式没有输入框，永远无法登录**。
已把上限提到 4096 并加了回归断言（`app/test/host_tests.c`）。

**生成器（测试参照物）自身也修了 3 处规范错误**，否则会把正确的解码器判成错的：
- `encode_udh()` 把 UDHL 写成了"含自身字节的总长"（标准是**不含**自身；真实网络 concat 永远是 `05 00 03 …`）
- `encode_sca()` 长度用了半字节数（应为八位组数）
- `encode_scts()` 时区符号位加在了"个位"上（标准是加在**十位**，即低半字节的 bit3）

---

## 九、如何复现全部验证

```powershell
# 主机单元测试（549 项断言 + 48 条 PDU 往返用例）
pwsh -File app/test/run_host_tests.ps1

# 单独跑 PDU 编码器自检
python app/test/gen_pdus.py --self-check

# 重新编译 + 打包
pwsh -File tools/build_firmware.ps1
```
