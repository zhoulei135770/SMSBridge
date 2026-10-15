# ML307A-DSLN 4G 短信网关

> 基于**中国移动 ML307A-DSLN**（ASR1606，OpenCPU 模组）的 4G 短信网关固件。
> 纯 C99 模块化实现，**短信远程配置** + **长短信重组** + **钉钉/企业微信机器人转发** +
> **IPv6/公网地址查询** + **Web 面板与出向状态推送**。
>
> 本仓库包含：**应用源码**、**构建/诊断/打包脚本**、**完整中文设计文档与踩坑记录**、
> 以及**编译好的应用镜像** `firmware/user_app.bin`。
>
> ⚠ 本仓库**不包含**中国移动 OpenCPU SDK、vendor 底包镜像与 `aboot`/`adownload` 刷写工具，
> 请按 [依赖获取](#四依赖获取) 自行下载。详见 [免责声明](#十四免责声明)。

---

## 目录

1. [项目简介](#一项目简介)
2. [功能清单](#二功能清单)
3. [仓库结构](#三仓库结构)
4. [依赖获取](#四依赖获取)
5. [硬件与环境要求](#五硬件与环境要求)
6. [编译步骤](#六编译步骤)
7. [主机单元测试与栈预算检查](#七主机单元测试与栈预算检查)
8. [刷写步骤](#八刷写步骤)
9. [短信指令完整参考](#九短信指令完整参考)
10. [钉钉/企业微信机器人配置](#十钉钉企业微信机器人配置)
11. [架构说明](#十一架构说明)
12. [平台实测陷阱合集](#十二平台实测陷阱合集最重要)
13. [常见问题排查](#十三常见问题排查)
14. [免责声明](#十四免责声明)
15. [License](#十五license)

---

## 一、项目简介

一块插着 SIM 卡的 ML307A-DSLN 模组，上电后变成一个能长期无人值守运行的 4G 短信网关：

- 收到短信 → 解码 PDU（自动重组长短信）→ 判断黑白名单 → 转发到钉钉/企业微信机器人 → 留存记录；
- 管理员用**手机发短信**就能改配置（转发地址、加签密钥、关键词、消息模板、管理员号码），无需连电脑；
- 管理员用短信就能问出**当前状态**（网络、信号、对时、配置摘要、收发计数）与**IP 地址**（IPv4/IPv6）；
- 双栈 PDP（`IPV4V6`），能拿到运营商分配的**公网 IPv6 全局地址**，并对下游（USB RNDIS）设备提供 IPv6 出网；
- 模组侧还有一个内置的 Web 配置面板（对 RNDIS/USB 侧开放）。

设计上刻意把**所有业务逻辑做成不依赖 SDK 的纯 C**，因此绝大部分代码可以在 PC 上跑单元测试，
只有极少数文件（`sdk_port.c`）直接 include SDK 头文件。这让"上板才发现"的 bug 少了很多。

---

## 二、功能清单

### 2.1 短信远程配置（核心功能）

管理员手机号发短信即可配置，**全部指令都在一条短信里装得下**（≤70 个 UCS2 字符），
也因此**每条回执只发一条短信**（原因见[陷阱 8](#陷阱-8udh-拼接短信缺片会导致手机整体不显示)）。

| 能力 | 说明 |
|---|---|
| 指令集 | `U` / `T` / `K` / `M` / `SAVE` / `STATUS` / `IP` / `面板` / `PANEL` / `A` / `RESET` / `我要成为管理员` / `我要配置` |
| 多指令回填 | 一条短信可以写多行，每行一条指令（最多 8 条） |
| **管理员首次认领机制** | 出厂管理员号**为空**。第一部发送 `我要成为管理员` 的号码会被认领为管理员；认领后其它号码再发该指令会被拒绝。这避免了"出厂默认管理员号被陌生人猜中"的问题 |
| 权限校验 | 非管理员号码发来的指令**静默忽略**（不回执、不泄露配置），且写入前会再校验一次 |
| 配置持久化 | 独立文件 `rcfg.bin`（magic/version/校验和），坏文件自动回退默认值；与原有 `cfg.bin` 完全隔离，零回归风险 |
| 回执 | `OK U` / `OK T` / `OK K` / `OK M` / `OK SAVED` / `OK RESET` / `OK ADMIN <号码>` / `ERR 无法识别的指令`；`STATUS` 回状态文本 |
| 模板占位符 | `{from}` `{time}` `{text}` `{pdu}` `{idx}`，未知占位符原样保留 |

### 2.2 长短信（Concatenated SMS / UDH）重组

- 接收侧：按 `UDH` 里的 `reference / total / seq` 重组多片长短信；超时自动清理半截会话。
- 发送侧：超过单条容量的文本自动分包（`sms_concat_build_tx`）。
- **但回执走的是"每条独立成一条短信"**（`sms_concat_build_tx_plain`，不带 UDH）——
  原因见[陷阱 8](#陷阱-8udh-拼接短信缺片会导致手机整体不显示)。
- UCS2 解码严格按 3GPP 03.38/03.40：`TP-UDL` 是**含 UDH 的总字节数**，
  一旦 `UDL > 实际到手字节` 立即报 `SMS_PDU_ERR_TRUNC`，**绝不静默截断执行**。

### 2.3 钉钉 / 企业微信转发

- 自动识别机器人类型（URL 含 `oapi.dingtalk.com` → 钉钉；含 `qyapi.weixin.qq.com` → 企业微信；否则通用 JSON `markdown`）。
- Markdown 报文组装；`{from}{time}{text}{pdu}{idx}` 模板渲染。
- 钉钉**自定义关键词**：正文前注入 `【关键词】`，保证钉钉的关键词校验通过。
- 钉钉**加签**：`sign = urlencode(base64(HMAC_SHA256(secret, timestamp + "\n" + secret)))`，
  以 `&timestamp=<ms>&sign=<sign>` 追加到 URL。
- **时钟与 NTP 的关系**：加签用毫秒级时间戳，钉钉要求与服务器时间偏差在 **1 小时**以内。
  固件开机后经 NTP（默认 `ntp.aliyun.com`）对时，并以 `AT+CCLK?` 的模组时钟作为兜底。
  **时钟不可信时自动降级为"仅关键词"发送**，并在状态短信里标注 `加签:已降级(时钟未同步)`。
- HTTPS：企业微信与钉钉本身就是 `https://`，固件支持 TLS（优先 `ca.pem` 校验，否则关闭校验并告警）。

### 2.4 IPv6 与公网地址

- 双栈：`AT+CGDCONT=1,"IPV4V6",...` + 重新激活 PDP，Web 面板可一键开启/关闭。
- 一次 `AT+CGPADDR=1` 读出 IPv4 与 IPv6（同时兼容纯 v4 / 纯 v6 / 双栈三种应答形态）。
- 公网 IPv4 出口地址走 HTTP 回显服务查询（**强制 v4 优先**，避免 DNS 默认 v6 优先导致量错对象）。
- 公网 IPv6 由 PDP 的全局地址直接给出。
- 获取到的地址会进入 `STATUS` 回执与面板。

### 2.5 Web 面板与出向状态推送

- 内置裸 TCP HTTP 服务（默认 `:8080`），带会话登录、路由、表单解析、HTML 转义。
- 面板展示：网络注册、信号、流量统计、黑白名单、Webhook 配置、IPv6/公网地址、短信记录。
- **⚠ 面板不可从外部访问**（原因见[陷阱 9](#陷阱-9应用内-http-服务收不到入向连接)）。
  因此增加了**出向推送**：把面板状态渲染成文本，主动 POST 到机器人（`面板` / `PANEL` 指令触发），
  在面板不可达时仍然能远程观测设备状态。

### 2.6 其它

| 功能 | 说明 |
|---|---|
| APN 管理 | `cmnet` / `3gnet` / `ctnet` 自动轮换 + TCP 连通性心跳保活 |
| 流量统计 | 平台字节计数 + 应用侧基线差值，月度限额、超限告警、可选断网 |
| 黑白名单 | 白名单后缀匹配（填 `13800000000` 也能匹配 `+8613800000000`） |
| 时钟 | NTP（`ntp.aliyun.com`）+ `AT+CCLK?` 模组时钟兜底 + 模板 `{time}` |
| 诊断上报 | `sdk_diag_post()` 把内部状态 HTTP POST 到 PC（配合 `tools/diag_listen2.ps1`），无 UART 日志时的救命通道 |
| 启动探针 | `boot_probe.c`，可判定"卡在哪一步"（:8081） |
| 栈预算检查 | `app/test/check_stack_budget.ps1`，防止大对象上任务栈（**很有必要**，见[陷阱 7](#陷阱-7任务实际栈只有-3072-字节)） |

---

## 三、仓库结构

```
ML307A-DSLN-SMS-Gateway/
├── README.md                    本文档
├── LICENSE                      MIT
├── PRIVACY.md                   脱敏说明（仓库内不含真实号码/密钥/设备标识）
├── .gitignore
├── app/                         ★ 固件源码（唯一源码来源）
│   ├── Makefile                 主机端测试用 Makefile
│   ├── build_host_tests.bat     旧版单模块测试入口（历史遗留，建议用 test/run_host_tests.ps1）
│   ├── inc/                     20 个头文件
│   ├── src/                     19 个实现文件 + main.c.normal（无诊断模式的 main 变体）
│   └── test/
│       ├── host_tests.c         ★ 主机单元测试（932 条断言）
│       ├── pdu_vectors.h        48 组 PDU 测试向量
│       ├── gen_pdus.py          独立 Python PDU 编码器（生成上面的向量）
│       ├── run_host_tests.ps1   ★ 一键：生成向量 → 编译 → 运行 → 栈预算检查
│       └── check_stack_budget.ps1  ★ -fstack-usage 调用链栈预算检查
├── tools/                       构建 / 部署 / 诊断脚本
│   ├── build_firmware.ps1       一键编译 + 打包（含官方脚本的两个坑的修补）
│   ├── deploy_to_sdk.ps1        把 app/ 部署进 SDK 并生成 custom/custom.mk
│   ├── diag_listen2.ps1         诊断上报接收端（HTTP，支持分段/多连接）
│   ├── verify_sdk_api.py        核对用到的 SDK 符号是否真有头文件声明
│   ├── extract_manual_text.py   从官方 PDF 手册提取文本（需自备 PDF）
│   ├── dump_sdk_manual_flash.py 提取手册里与烧录相关的原文
│   └── dump_bootmode_doc.py     提取手册里 BOOT_MODE/下载控制相关原文
├── docs/                        ★ 详细中文文档（含上板实测与踩坑清单）
│   ├── README.md                文档索引
│   ├── 01-编译与烧录指南.md
│   ├── 02-设计与API映射.md
│   ├── 03-需求对照与差异说明.md
│   ├── 04-上板验收清单.md
│   ├── 05-烧录排查记录与后续方案.md
│   ├── 06-首次上电重启循环定位与修复.md
│   ├── 07-今日收尾-设备状态与后续步骤.md
│   ├── 08-根因确认-socket线程模型与eloop.md
│   ├── 09-短信远程配置与钉钉转发-实现规格.md
│   ├── 10-短信远程配置-实现进度与交接.md
│   ├── 11-IPv6与公网IP-证据实现与上板验证.md
│   ├── 12-上板记录-首次成功烧录与IPv6实测.md
│   ├── research/                调研原始报告（SDK/AT/烧录/符号核实）
│   └── sdk-reference/           cmiot API 速查、平台网络 API、应用可调用符号清单
└── firmware/                    ★ 应用镜像与刷写脚本
    ├── user_app.bin             应用镜像（app-only，185,408 字节）
    ├── user_app.bin.sha256      SHA256 校验值
    ├── flash_apponly.ps1        ★ 可复用的 app-only 刷写脚本
    └── README.md                镜像说明与刷写详解
```

---

## 四、依赖获取

> **本仓库不包含**下列 vendor 内容。请自行从合法渠道获取，并遵守各自的许可协议。

| 依赖 | 用途 | 获取方式 |
|---|---|---|
| **中国移动 ML307A OpenCPU SDK 1.4.2** | 编译固件（含 `arm-none-eabi-gcc`、`make.exe`、`python3`、`arelease.exe`、预编译底包） | 中国移动物联网 OpenCPU 开发者平台 / OneMO 官方渠道，包名形如 `ML307A_OpenCPU_Standard_1.4.2`。解压后放到本仓库根目录的 `sdk/ML307A_OpenCPU_Standard_1.4.2/`（该目录已在 `.gitignore` 中，不会被提交） |
| **arm-none-eabi-gcc** | 交叉编译 | **SDK 自带**（`sdk/.../tools/toolchain/gcc-arm-none-eabi/bin`），无需单独安装。若单独安装请用官方 SDK 配套版本（本工程验证过的是 8.3.1） |
| **`aboot` 工具包（含 `adownload.exe`）** | 刷写 | 与 SDK 同渠道的 `aboot-tools-*-win-x64` 包。中移 OpenCPU 资料下载页 / 官方 Gitee 仓库（如 `YZRDEG/ML307A` 的 `aboot-tools-2022.03.01-win-x64.rar`） |
| **PC 端工具** | 主机测试 | `gcc`（MinGW-w64，需支持 `-fstack-usage`）、`python`（3.x，`gen_pdus.py` 无第三方依赖；`tools/*.py` 若要读 PDF 需额外 `pypdf`）、`pwsh`（PowerShell 7） |

**关于 `.pac` 的澄清**：本 SDK 的打包链**不产生 `.pac`**。`.pac` 是展锐（UNISOC）格式；
本平台的产物是 ASR `aboot` 体系的 **`.zip` release package**。不要去找 `.pac`。

---

## 五、硬件与环境要求

### 5.1 硬件

| 项 | 要求 |
|---|---|
| 模组 | **ML307A-DSLN**（ASR1606，DS 系列，8 MB NOR）。底板实测为 `DEMO-ML307X-V1.04` |
| SIM 卡 | 已激活的中国移动 4G SIM/Nano 卡；**必须关闭 PIN 锁**（`AT+CPIN?` 应为 `+CPIN: READY`）。开了 PIN 锁的卡固件无法解锁，会一直卡在初始化 |
| 天线 | LTE 主天线必须接（`AT+CSQ` 建议 ≥ 15；实测 31） |
| 供电 | **USB 5V / 2A 直供**。不要用劣质 USB HUB 或仅 0.5A 的充电头——发射瞬间电流冲击会导致模组掉电重启 |
| 串口 | 正常模式 AT 口 **`COMx @ 115200`**（实测为 `COM3`）；刷写时另出现 ASR 下载口（实测为 `COM5`，由 `adownload -a` 自动枚举） |
| 数据线 | 数据线（非纯充电线）；跑 RNDIS 时还需要 Windows 能识别 USB 网卡 |
| 手机（可选） | 用来给模组发配置短信、验证转发效果 |

### 5.2 软件环境

| 项 | 要求 |
|---|---|
| OS | Windows 10/11（SDK 的 `make.exe`/`arelease.exe`/`pack.bat` 均为 Windows 可执行文件） |
| Shell | PowerShell 7（`pwsh`）。**不要**让 MSYS/Git 的 `sh.exe` 出现在 `PATH` 最前面（见[编译步骤](#六编译步骤)） |
| 编译 | `make` 一定要带 `SHELL=cmd.exe`（`tools/build_firmware.ps1` 已经代劳） |
| 无需 | **不需要 Keil**，**不需要 Linux**，**不需要 WSL** |

---

## 六、编译步骤

### 6.1 准备

```powershell
# 1) 把 SDK 放到仓库根目录（目录名必须与默认值一致，或用 -SdkDir 覆盖）
#    <仓库根>\sdk\ML307A_OpenCPU_Standard_1.4.2\

# 2) 确认能拿到 pwsh / gcc / python
pwsh -v ; gcc --version ; python --version
```

### 6.2 一键编译 + 打包

```powershell
# 完整编译 + 打包（自动先把 app/ 部署进 SDK）
pwsh -File tools/build_firmware.ps1

# 先 clean 再编译
pwsh -File tools/build_firmware.ps1 -Clean

# 只编译不打包
pwsh -File tools/build_firmware.ps1 -SkipPack

# 换 SDK 路径 / 换子型号（DSLN / DCLN / GSLN / GCLN）
pwsh -File tools/build_firmware.ps1 -SdkDir D:\sdk\ML307A_OpenCPU_Standard_1.4.2 -Model DSLN

# 只把 app/ 源码部署进 SDK（生成 custom/custom.mk），不编译
pwsh -File tools/deploy_to_sdk.ps1
```

产物在 `sdk/ML307A_OpenCPU_Standard_1.4.2/out/images/ML307A/DSLN/`：

| 文件 | 说明 |
|---|---|
| `ML307A_DSLN_APP.bin` | **应用镜像**（就是被写进 `user_app` 分区的东西；本仓库的 `firmware/user_app.bin` 即此文件） |
| `ML307A_DSLN_APP.elf` | 带调试符号的 ELF |
| `ML307A_DSLN_APP.map` | 链接 map（查符号与占用） |
| `ML307A_DSLN_APP.zip` | **整包**（约 10 MB，含全部分区镜像 + `download.json`），供刷整包用 |

### 6.3 手动编译（等价于脚本做的事）

```powershell
cd sdk\ML307A_OpenCPU_Standard_1.4.2
$env:PROJECT_ROOT = $PWD.Path
$env:MODEL_NAME   = 'ML307A'
$env:MODEL_TARGET = 'DSLN'
$env:HW_REVISION  = 'H0'
$env:GNSS_OPT     = 'NO'
$env:FLASH_OPT    = '8M'
$env:SW_REVISION  = (Get-Date -Format 'yyyyMMddHHmm')

# ★ 关键是 SHELL=cmd.exe
make -f tools\scripts\Makefile all -j4 -e `
    MODEL_NAME=ML307A MODEL_TARGET=DSLN `
    HW_REVISION=H0 SW_REVISION=$env:SW_REVISION DEMO_SUPPORT=n `
    SHELL=cmd.exe

# 打包
cmd /c "tools\scripts\pack.bat"
```

### 6.4 编译要点与两个真实的坑

1. **`SHELL=cmd.exe` 不能省。** SDK 自带的 `make.exe` 需要一个 POSIX shell 才能执行
   `echo`/`mkdir` 一类的配方，而 SDK 里没有带 `sh.exe`。若不指定，make 会在 `PATH` 里找
   `sh.exe`，找到 Git/MSYS 的那个就可能报 `couldn't create signal pipe`
   （受限环境下无法创建命名管道）。指定 `SHELL=cmd.exe` 让 make 直接用 cmd，问题消失。
   **`tools/build_firmware.ps1` 已内置这一点。**

2. **`pack.bat` 依赖 `build.bat` 设置的环境变量**（`PROJECT_ROOT` / `MODEL_TARGET` /
   `FLASH_OPT` 等）。直接单独跑 `pack.bat` 会因为变量缺失而失败或打出错误的包。
   `tools/build_firmware.ps1` 里做了显式补齐。

3. `SW_REVISION` 用 `yyyyMMddHHmm`，会体现在 `ATI` 里，便于确认设备上跑的是哪一版。

---

## 七、主机单元测试与栈预算检查

**所有纯逻辑模块都能在 PC 上测**，不需要模组、不需要 ARM 工具链：

```powershell
# 一键：生成 PDU 向量 → 编译 → 运行 → 栈预算检查
pwsh -File app/test/run_host_tests.ps1

# 跳过 AddressSanitizer/UBSan（本机 MinGW 缺 sanitizer 运行时的时候用）
pwsh -File app/test/run_host_tests.ps1 -NoSanitizer

# 只跑栈预算检查
pwsh -File app/test/check_stack_budget.ps1
```

| 项 | 说明 |
|---|---|
| 断言数 | **932 条**（含 PDU 编解码往返、配置序列化、缓冲区越界哨兵、HTML 转义、XSS、指令解析、`rcfg.bin` 坏文件回退、管理员/非管理员路由越权检查、转发组装（模板/关键词/加签/降级）、时间解析、IPv4/IPv6 解析与 `AT+CGPADDR` 双栈形态） |
| PDU 向量 | `app/test/pdu_vectors.h` 里的 **48 组**向量，由 `gen_pdus.py`（独立 Python 编码器）生成，与被测 C 解码器互为对照 |
| 消毒器 | 自动探测 MinGW 是否有 libasan/libubsan：有就用 AddressSanitizer + UBSan，没有就退回"哨兵字节"越界检测 |
| 退出码 | 全部通过 = `0`；有用例失败或栈预算超限 = `1` |
| 栈预算 | 单函数 ≤ **1280 字节**，已知调用链合计 ≤ **2048 字节**（给 3072 的真实任务栈留 1 KB 余量）。主机是 x86-64（指针 8 字节），同一份代码的帧通常**大于** ARM32，所以主机上通过 = 对固件偏保守 |

> 单元测试**测不出栈溢出**（PC 栈有 8 MB，固件任务栈只有 3072 字节），
> 所以栈预算检查是独立的一步，必须跑。详见[陷阱 7](#陷阱-7任务实际栈只有-3072-字节)。

---

## 八、刷写步骤

### 8.1 概念：整包 vs app-only

| | **整包**（`ML307A_DSLN_APP.zip`，~10 MB） | **app-only**（本仓库方案，~130 KB） |
|---|---|---|
| 内容 | `cp.bin`（内核）、`system.img`、`bootloader.ubi`、`rf.bin`、`user_app.bin` 等全部分区 + `download.json` | 只含 `user_app.bin` + 刷写器必需的 `preboot.img`/`flasher.img`/`flashinfo.bin`/`partition.bin` |
| 会动内核吗 | **会** | 不会 |
| `download.json` 里的动作 | 含 `erase nvm` / `erase fota_param` / `erase fota_pkg`，production 下还有 `erase factory` | **只有 `flash user_app`，没有任何 erase** |
| 风险 | 擦 `nvm` / `factory` 可能丢 IMEI 与产测数据；换内核必须与 app 同源 | 低 |
| 什么时候用 | 设备内核与 SDK 基线**不同源**；或设备已成砖需要恢复 | 设备内核与 SDK 基线**同源**，只升级应用 |
| 判定同源 | 用 `ATI` 读内核（形如 `ML307A-DSLN-MBRH1S00`），与 SDK 基线版本对照 | 同 |

**结论**：只要能确认同源，优先用 **app-only**。本仓库的 `firmware/flash_apponly.ps1` 就是干这个的。

### 8.2 前置检查（强烈建议）

```powershell
# 1) 确认设备能通信、SIM 正常、已注册
AT          -> OK
ATI         -> ML307A-DSLN-<内核版本>     ← 记下来，与 SDK 基线对照
AT+CPIN?    -> +CPIN: READY
AT+CSQ      -> +CSQ: 31,99
AT+CEREG?   -> +CEREG: 0,1               ← 已注册

# 2) 备份关键标识（换内核/整包前必做）
AT+CGSN=1            -> IMEI
AT*MRD_IMEI?
AT*MRD_SN?           -> SN
```

### 8.3 软入口：`AT$MYDOWNLOAD=1`

ML307A-DSLN 支持一条 AT 指令让模组自己重启进 aboot 下载模式，**不需要短接 BOOT_MODE 触点**：

```
AT$MYDOWNLOAD=1        ->  OK        （AT 表里的取值域：$MYDOWNLOAD: (0,1)）
# 随后 AT 口断开，Windows 出现 "ASR Serial Download Device"（本次实测 COM5）
```

> **前提**：内核必须是 **MBR 基线**。出厂 `MTSH1S00` 内核上这条指令返回 `ERROR`
> （实测 494 条受支持的 AT 命令里没有它）。这种情况下只能用 BOOT_MODE / KEYOUT1
> （管脚 82）短接 + 上电的方式进下载模式，**具体触点请查你手上底板的原理图，不要凭猜测短接**。

### 8.4 用脚本刷写（推荐）

```powershell
# 0) 先干跑：只抽包组包、打印命令行，不碰设备
pwsh -File firmware\flash_apponly.ps1 `
    -AdownloadExe C:\aboot-tools\adownload.exe `
    -ReleaseZip sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip `
    -DryRun

# 1) 正式刷写（默认 COM3 @115200）
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe

# 2) 换串口
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe -AtPort COM7

# 3) 已经用 BOOT_MODE 短接进了下载模式：跳过发软入口那一步
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe -SkipEnterDownload
```

脚本做的事：抽 4 个刷写器镜像 → 换上 `user_app.bin` → 生成**无 erase**的 `download.json`
（并自检拒绝破坏性包）→ 发 `AT$MYDOWNLOAD=1` → 调 `adownload.exe` → 轮询等设备回正常模式 →
打印 `ATI` / `AT+CPIN?` / `AT+CMGF?` / `AT+CNMI?` 做快速自检。详见 `firmware/README.md`。

### 8.5 手动刷写（等同脚本内部）

```powershell
# 1) 正常模式下（COM3 @115200）发软下载入口
AT$MYDOWNLOAD=1

# 2) 模组重启进下载模式：AT 口断开，出现 ASR 下载口

# 3) 刷写
adownload.exe -u -a -r -q -s 115200 <app-only 包.zip>
```

参数含义：`-u` 只用 Arom USB 下载口 ｜ `-a` 自动枚举串口 ｜ `-r` 完成后重启设备
｜ `-q` 完成即退出 ｜ `-s 115200` 串口速率。

### 8.6 ⛔ 绝对不要加 `-m`

`-m` / `--production` 是**生产模式**。它会让 `download.json` 里标了
`"productionOnly": true` 的动作真正执行——也就是 **`erase factory`** 与 **`fuse`**。
这会**抹掉产测数据甚至 IMEI**，且不可逆。本仓库的脚本刻意不提供这个开关，
手动刷写时也请**永远不要**加 `-m`。

### 8.7 刷写成功的判据

```
<COMx> processing command [partition partition.bin]
<COMx> #=> [INFO: Aboot ] published 20 partition information table.
<COMx> processing command [flash user_app user_app.bin]
<COMx> (flasher) programming partition 'user_app' with datap = 0x7e300000, size = 0x...
<COMx> (flasher) program "user_app" done.
<COMx> processing command [reboot]
... "status": "SUCCEEDED" ... all finished. total time: 28.108s
```

回到正常模式后，用 AT 口确认**新应用真的在跑**：

```
AT+CMGF?   ->   +CMGF: 0            ← PDU 模式，本固件已接管（旧固件或是 1）
AT+CNMI?   ->   +CNMI: 2,2,0,0,0    ← +CMT 直投已开启，这是本固件的招牌配置
```

然后给模组发一条 `STATUS`，应收到状态回执短信。

---

## 九、短信指令完整参考

**触发**：用管理员号码发短信，内容为指令（一条短信可写多行，每行一条，最多 8 条）。
**首次使用**：出厂管理员号为空，先发 `我要成为管理员` 认领。

### 9.1 指令表

| 指令 | 含义 | 示例 | 回执 |
|---|---|---|---|
| `我要成为管理员` | **首次认领**管理员（仅在未认领时有效；认领后其它号码使用会被拒绝） | `我要成为管理员` | 认领成功则回状态；已被认领则不响应 |
| `我要配置` | 返回配置菜单 + 当前状态 | `我要配置` | `配置:U网址 T密钥 K词 M模板` / `SAVE STATUS IP 面板` / `A管理员号 RESET` / `我要成为管理员` |
| `U <url>` | 设置 Webhook 转发地址（钉钉/企业微信/通用） | `U https://oapi.dingtalk.com/robot/send?access_token=YOUR_ACCESS_TOKEN` | `OK U` |
| `T <secret>` | 设置钉钉**加签密钥**；`T -` 清空 | `T SECxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx` | `OK T` |
| `K <keyword>` | 设置钉钉**自定义关键词**；`K -` 清空；转发时以 `【关键词】` 注入正文 | `K 报警` | `OK K` |
| `M <template>` | 设置消息模板，支持 `{from}` `{time}` `{text}` `{pdu}` `{idx}`；`M -` 恢复默认 | `M {from}\|{time}\|{text}` | `OK M` |
| `SAVE` | 立刻把当前配置落盘到 `rcfg.bin` | `SAVE` | `OK SAVED` |
| `STATUS` | 查询状态（网络/信号/对时/配置摘要/收发计数/管理员认领状态） | `STATUS` | 见 §9.2 |
| `IP` | 查询地址：`IP4:<IPv4>` + `IP6:<IPv6>`（**只发地址，一条短信装完**） | `IP` | `IP4:10.0.0.100` / `IP6:2409:xxxx:xxxx:xxxx::1`（空间够时附 `详情已发机器人`） |
| `面板` / `PANEL` | 把 Web 面板状态页渲染成文本，**出向推送**到机器人 | `面板` | 回执 + 机器人收到面板快照 |
| `A <号码>` / `ADMIN <号码>` | 更换管理员号码（旧号立即失去权限） | `A 13900000000` | `OK ADMIN 13900000000` |
| `RESET` | 恢复远程配置默认值（模板、NTP 等；不影响 `cfg.bin` 与已认领的管理员） | `RESET` | `OK RESET` |
| 其它 | 无法识别 | `HELLO WORLD` | `ERR 无法识别的指令` |

> 号码比较允许带/不带 `+` 与 `86` 前缀：`13800000000`、`+8613800000000`、`8613800000000` 视为同一号码。

### 9.2 `STATUS` 回执形态（单条短信，≤70 个 UCS2 字符）

```
网:<注册状态> 信:<信号> 时:<对时状态>
U:已设/未设 T:已设/未设 K:<关键词前8字符/未设>
M:已设/未设 收<n>/转<n>
管:<管理员号码> 已认领/未认领
```

### 9.3 典型使用流程

```
# 第一步：认领管理员（新设备）
我要成为管理员

# 第二步：看菜单
我要配置

# 第三步：一次性回填全部配置（多行，一条短信）
U https://oapi.dingtalk.com/robot/send?access_token=YOUR_ACCESS_TOKEN
T SEC0000000000000000000000000000000000000000000000000000
K 报警
M 【报警】{from} @{time}
SAVE

# 第四步：查状态与地址
STATUS
IP
```

> **注意**：回填用的长 URL 短信会被手机按长短信（2 片）发出。
> 本固件通过 `+CMT` URC 通路接收完整 PDU，可以正确重组；
> 详见[陷阱 5](#陷阱-5应答通路只投递第一个数据突发读长短信必须靠-cmt-urc-通路)。

---

## 十、钉钉 / 企业微信机器人配置

### 10.1 钉钉：自定义关键词（最简单）

1. 钉钉群 → 群设置 → 智能群助手 → 添加机器人 → **自定义**（通过 Webhook 接入）；
2. 安全设置勾选 **自定义关键词**，例如填 `报警`；
3. 复制 Webhook 地址（形如 `https://oapi.dingtalk.com/robot/send?access_token=YOUR_ACCESS_TOKEN`）；
4. 短信设置：
   ```
   U https://oapi.dingtalk.com/robot/send?access_token=YOUR_ACCESS_TOKEN
   K 报警
   SAVE
   ```
5. 之后任何转发消息的正文都会以 `【报警】...` 开头，满足钉钉的关键词校验。

### 10.2 钉钉：加签（更安全）

1. 安全设置勾选 **加签**，复制形如 `SECxxxxxxxx...` 的密钥；
2. 短信设置：
   ```
   U https://oapi.dingtalk.com/robot/send?access_token=YOUR_ACCESS_TOKEN
   T SECxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
   SAVE
   ```
3. 固件会计算 `sign = urlencode(base64(HMAC_SHA256(secret, timestamp + "\n" + secret)))`，
   并以 `&timestamp=<ms>&sign=<sign>` 追加到 URL。

**加签与时钟的关系**：时间戳是毫秒级，钉钉要求与服务端时间偏差在 **1 小时**以内。

- 固件开机后经 **NTP**（默认 `ntp.aliyun.com`）对时，`AT+CCLK?` 的模组时钟作为兜底；
- **时钟不可信时自动降级**为"仅关键词"发送（如果同时配了 `K`），
  并在 `STATUS` 里标注 `加签:已降级(时钟未同步)`；
- 如果只配了 `T` 没配 `K`，降级后钉钉会拒收（缺关键词又缺有效签名）→ 建议 `K` 和 `T` 都配。

### 10.3 企业微信

1. 企业微信群 → 群机器人 → 添加 → 复制 Webhook（`https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=...`）；
2. 短信设置 `U <该地址>` + `SAVE`。企业微信不需要加签，也**不要**配 `T`。

### 10.4 通用 JSON（自建服务）

URL 既不是 `oapi.dingtalk.com` 也不是 `qyapi.weixin.qq.com` 时，固件按通用
`markdown` JSON 报文 POST。可用 `tools/diag_listen2.ps1` 起一个本地接收端验证：

```powershell
pwsh -File tools/diag_listen2.ps1 -Port 9090 -Seconds 300
# 然后短信设置 U http://<PC的RNDIS地址>:9090/recv
```

### 10.5 常见错误码

| 现象 | 含义与处理 |
|---|---|
| `errcode: 310000` | 钉钉安全设置校验未通过。检查：① 关键词是否真的出现在正文里（`K` 是否设置且模板没覆盖掉）；② 加签密钥是否正确、时钟是否已同步 |
| `errcode: 300001` | 无效的 access_token，检查 URL 里的 `access_token` |
| `errcode: 400013` / 频率限制 | 机器人发消息过快，钉钉限流（默认每分钟 20 条）；降低频率 |
| `errcode: 43004` | 机器人被移出群 / 群不存在 |
| HTTP 200 但群里没消息 | 多半是安全设置没通过（钉钉会返回 200 但 `errcode != 0`）；固件在日志与 `STATUS` 里会标注推送失败 |

---

## 十一、架构说明

### 11.1 任务划分

固件在 `cm_opencpu_entry()` 里**只做最小动作**（打印横幅、创建 `app_main` 任务）
然后立刻 `return 0` 交给 SDK 调度器——SDK 在入口处有启动超时保护，入口里不能做阻塞操作。

`app_main_task` 完成初始化后，创建并移交以下任务：

| 任务 | 职责 | 备注 |
|---|---|---|
| `app_main` | 初始化编排，之后转为**流量采样循环** | 启动时读配置、初始化短信/USB/网络/Web |
| `sms_svc` | URC 注入 → 拼行 → PDU 解码 → 长短信重组 → 黑白名单 → 记录 → 转发 → 指令路由与回执 | **所有短信收发都在这个任务里** |
| `net_mgr` | APN 轮询切换、TCP 心跳保活、IPv4/IPv6 刷新、公网 IP 查询 | **唯一主动发 AT 命令的任务** |
| `web` | 裸 TCP HTTP 服务 `:8080` | 会话/路由/表单/渲染 |
| 启动探针 | `boot_probe.c` 的 `:8081`，判定卡在哪一步 | 诊断用 |

### 11.2 ★ eloop 线程约束（很重要的架构约束）

SDK 里 lwIP / socket / DNS / NTP 这些都绑定在 **eloop 线程**上。跨线程创建或收发 socket
会导致**内核复位该连接**（实测：应用侧 socket 创建在别的任务里 ⇒ 连接被 RST / 收不到数据）。

因此本工程的原则是：

- **socket 必须在 eloop 线程内创建与收发**；其它任务只能**投递请求**过去；
- `cm_ntp_sync()` 依赖 asocket + async_dns + eloop，因此 NTP 对时也是**投递到 eloop 执行**；
- Web 服务与启动探针的 socket 都遵循该约束。

### 11.3 AT 虚拟通道（`cm_virt_at_*`）

SDK 给应用的不是"一串能随便写 AT 命令的串口"，而是一个**虚拟 AT 通道**：

- `cm_virt_at_init()` 注册回调 → `cm_virt_at_send()` 发命令 → 内核把模组应答通过回调给你；
- **一次 init 只能发一段命令**，在收到终结响应前不能再发（SDK 流控会返回 -1）；
- URC（如 `+CMT`、`+CMTI`）走独立通路 `cm_virt_at_urc_reg()`，与应答通路分开；
- 也没有"收短信"的 API——**接收必须自己注册 URC + `AT+CMGF=0` + `AT+CNMI=2,2,0,0,0`，自己解 PDU**。

具体的坑（连着用错会报废一整个开机周期）见[陷阱 1~5](#十二平台实测陷阱合集最重要)。

### 11.4 长短信重组

- `sms_concat.c` 维护按 `(ref, total)` 索引的重组槽位，收到 `seq` 片就填入，齐了就交付；
- `sms_concat_tick()` 定期清理半截会话（避免永久占内存）；
- 发送侧 `sms_concat_build_tx()` 按 UDH 分包；回执侧用
  `sms_concat_build_tx_plain()` **每条独立**（不带 UDH）。

### 11.5 配置持久化：`cfg.bin` 与 `rcfg.bin`

刻意分成两个文件，互不干扰：

| 文件 | 归属 | 内容 | 结构 |
|---|---|---|---|
| `cfg.bin` | `app_config.c` | 网络/心跳/流量限额/黑白名单/RNDIS/APN 下标/月度累计/Web 口令 | 结构体 + magic + version + **CRC32**，主/备/临时三文件原子写 |
| `rcfg.bin` | `sms_rcfg.c` | 远程配置：`url[192]` `secret[80]` `keyword[40]` `tmpl[160]` `admin[24]` `ntp[64]` `concat_enable` `admin_claimed` | 576 字节 blob：magic(4)+version(2)+len(2)+payload(564)+checksum(4) |

- `rcfg.bin` 单独存在是为了**不动已验证的 `cfg.bin` 序列化格式**，零回归风险；
- 两者都遵循"**任何异常都回退默认值**"：文件不存在、掉电半截写、magic/版本/校验和不符 → 静默用默认值，不阻塞启动。

### 11.6 模块一览

| 模块 | 文件 | 说明 |
|---|---|---|
| 主入口 | `main.c` | `cm_opencpu_entry()` + 任务创建 |
| SDK 适配层 | `sdk_port.c/.h` | **唯一** include SDK 头文件的文件；换 SDK 只改这里（含虚拟 AT、HTTP、文件、时间、任务创建） |
| 短信 PDU | `sms_pdu.c/.h` | GSM 03.40/03.38 编解码，纯 C |
| 长短信 | `sms_concat.c/.h` | 接收重组 / 发送分包 / SMSC(`AT+CSCA?`) 解析 |
| 短信服务 | `sms_svc.c/.h` | URC → 解码 → 黑白名单 → 记录 → 转发 → 指令路由 |
| 指令解析 | `sms_cmd.c/.h` | 多行指令解析与回执渲染（纯 C） |
| 远程配置 | `sms_rcfg.c/.h` | `rcfg.bin` 持久化与指令应用（纯 C） |
| 管理员路由 | `sms_admin.c/.h` | 权限判定、认领、越权拒绝（纯 C） |
| 转发组装 | `webhook.c/.h` + `webhook_fmt.c/.h` | 类型识别、模板渲染、关键词注入、加签、发送 |
| 网络管理 | `net_mgr.c/.h` + `net_addr.c/.h` | APN 轮换、心跳、双栈地址解析、公网 IP |
| Web | `web_server.c/.h` + `web_ui.c/.h` | 裸 TCP HTTP、登录会话、路由、HTML 渲染与转义 |
| 配置 | `app_config.c/.h` + `app_ctx.c/.h` | `cfg.bin` + 全局上下文与锁 |
| 流量 | `traffic.c/.h` | 平台计数差值、月度限额、告警/断网 |
| USB | `usb_rndis.c/.h` | 期望值 + 状态展示（**应用层无权切网卡**，见陷阱 9） |
| 时间 | `app_time.c/.h` | ISO8601 / `AT+CCLK` 解析、时钟可信判定 |
| 摘要 | `sha256.c/.h` | SHA256 / HMAC-SHA256 / base64 / urlencode / 钉钉加签 |
| 日志 | `app_log.c/.h` | 分级 UART 日志 |
| 探针 | `boot_probe.c/.h`、`diag_stage.c/.h` | 启动阶段记录与 `:8081` 探针 |

---

## 十二、平台实测陷阱合集（最重要）

> 这一节是本项目最有价值的部分。**每一条都是真金白银踩出来的**，
> 全部有上板日志/回显/二进制证据。做同类平台开发的人请**逐条读完再动手**。

### 陷阱 1：虚拟 AT 一次 init 只能发一段

`cm_virt_at_*` 的流控要求在收到**终结响应**之前不能再发下一段。想自己"分三段发一条短信"
（发 `AT+CMGS=<len>` → 等 `>` → 发 PDU → 发 Ctrl-Z）是行不通的：

| 尝试 | 结果 |
|---|---|
| 分三段 `cm_virt_at_send` | `r=12/-1/-1` —— 第 2 段起恒为 -1 |
| 每段各自 init/deinit | 同样 `r=12/-1/-1` |
| 把 `AT+CMGS=N\r` + PDU + Ctrl-Z 拼成一条交给 `sdk_at_cmd` | 模组只回 `>`，报文丢失（输入态在提示符之后才收正文） |
| 批量发送时"整批只 init 一次" | 封装全部 `ret=-1`（它自己也 init/deinit，被占着就报 busy） |

**结论**：一条短信用**一次**发送 API 完成；批量发送时**每条各自 init/deinit**。

### 陷阱 2：`cm_virt_at_sms_send()` 的回调必须"不取数据"

**现象**：每次开机**只有第一次**发送成功，之后恒 `ret=-1`（连 28 字节的短条也失败）。

**根因**：`cm_virt_at_sms_send()` 由内核自己完成
`AT+CMGS= → 等 ">" → 发数据 → 收 OK` 全流程，它**需要读走模组应答**。
我们注册的回调却调用 `cm_virt_at_get()` 把数据抢走了 → 内核判定这次发送失败。

**修复**：发送路径注册**空回调**（只计数，不取数据）。修复后同一开机周期连发两条都 `ret=0`。

> 这是本项目最隐蔽的一个 bug：它在单元测试与"开机第一条"上都是**通过的**。

### 陷阱 3：发送必须 `AT+CMGS=<len>` 且只以 CR 结尾

- 长度参数必须是 **PDU 八位组数**（不是字符数、不是十六进制串长度）。
- 命令**只以 `\r` 结尾**。用 `\r\n` 的话，多出的 `\n` 会被当成报文首字节 →
  `+CMS ERROR: 304`。
- 曾经把 SMSC 号码串误传成 `atCmgs` 参数，拼出 `AT+CMGS=+8613800738500` 这种非法指令。

### 陷阱 4：长短信回执不能指望"连发多条"

实测：同一回执拆成多条发送时，**只有第 1 条能出去**；第 2 条起一律 `ret=-1`，
无论间隔是 1 秒 / 8 秒 / 11 秒 / 22 秒。唯一一次两条都成功，是"两条 127~147 字节的**独立**回执"，
而失败的都是 156~163 字节的连续分片。

**结论**：**每条回执必须是 1 条短信**。为此：

- 菜单压到 **55 个 UCS2 字符**、状态压到 **47 个字符**、`IP` 回执只放两行地址；
- 回执一律用 `sms_concat_build_tx_plain()`（**不带 UDH 的独立短信**）。

### 陷阱 5：应答通路只投递第一个数据突发，读长短信必须靠 `+CMT` URC 通路

**现象**：管理员发一条含长 Webhook URL 的配置短信（手机按 2 片长短信发出），
应用只拿到 4 个字符 `U h1` —— 竟然把半截 URL 当指令执行了。

**证据链**：

| 证据 | 说明 |
|---|---|
| 应用侧 `rxpdu … UDL=140`（应带 134 字节正文），实际只有 6 字节 | 截断发生在应用侧 |
| 外口 `AT+CMGR=2` 读同一份存储 | 拿到**完整 338 字符 PDU**（应答 363 字节）⇒ 模组/存储没问题 |
| `atread AT+CMGL=4 cb=1 drain=0 len=24` | 约 800 字节的应答，**回调只触发 1 次、补读 0 字节、只拿到 24 字节** |

⇒ SDK 虚拟 AT 的**响应通路每次只投递第一个数据突发**，`AT+CMGR` / `AT+CMGL`
这类长应答注定残缺，**长短信永远读不回来**。

**解法**：改用 `AT+CNMI=2,2,0,0,0`（`<mt>=2`），让模组把整条 PDU 通过 **URC 通路**
（`cm_virt_at_urc_reg()` → 逐行回调）推上来，不受响应通路长度限制：

```
rxpdu dcs=8 enc=2 udhi=1 udhl=5 cat=1 seq=1/2 ref=13 PDU=(完整 330+ 字符)
rxpdu …                                 seq=2/2 ref=13 PDU=(完整)
rx num=+8613800000000 len=136 text=U https://oapi.dingtalk.com/robot/send?a…
admin=1 changed=1 save=1        ← 完整解析 + 写入 flash
tx init=0 ret=0 len=83          ← 回执发送成功
```

**代价**：`mt=2` 按标准**不落存储**，URC 丢了就没了。因此固件仍保留
`AT+CMGL=0` 的 10 秒轮询兜底，并把截断 PDU 明确上报 `urc pdu trunc`，**绝不静默执行半截内容**。

### 陷阱 6：URC 行缓冲必须够大（≥512 字节）

一条 UCS2 长短信的 PDU 约 330~360 个十六进制字符，加上 `+CMT: ,<len>` 前缀会超过 400 字节。
早期行缓冲太小 ⇒ 行被截断 ⇒ PDU 残缺。**行缓冲请给 ≥512 字节**。

### 陷阱 7：任务实际栈只有 3072 字节

需求文档写"任务栈 6144 字节"，但 `sdk_port.c` 的 `sdk_task_create()` 里
**实际生效值被钳制为 3072 字节**（原因：栈由内核堆分配，任务数一多就 `osThreadNew` 失败，
Web/探针任务直接建不出来）。

而集成"管理员指令 + 回执分包"时，链式调用合计到了 5~6 KB：

| 函数 | 帧大小 |
|---|---|
| `handle_pdu` | 2688 字节 |
| `sms_admin_handle` | 3024 字节 |
| `sms_concat_build_tx` | 2608 字节 |
| `sms_svc_export` | 784 字节 |
| `webhook_req_t`（URL 320 + body 2048） | ≈ 2.4 KB |
| 渲染缓冲 | 2 × 2 KB |

**主机单元测试完全测不出来**（PC 栈 8 MB），**只有上板才会表现为崩溃**。

**解法**：

1. **所有大对象（>256 字节）必须静态化**，不放在栈上；
2. 用 `app/test/check_stack_budget.ps1` 自动防回归：
   逐个 `.c` 用 `-fstack-usage` 编译，按"任务内已知调用链"求和，
   单函数上限 1280 字节、调用链上限 2048 字节（给 3072 留 1 KB 余量）。
   主机 x86-64 帧通常大于 ARM32，所以**主机通过 = 对固件保守**。

### 陷阱 8：UDH 拼接短信缺片会导致手机整体不显示

**现象**：应用把 4 片的 UDH 长短信只成功发出第 1 片 → **用户手机上什么都看不到**
（手机要收齐所有分片才显示，缺片就整体扣住，最后丢弃）。

**解法**：

1. 回执改为**每片独立成一条短信**（`sms_concat_build_tx_plain()`，不带 UDH）；
2. 回执长度压到 **≤70 个 UCS2 字符**（即"一条短信装得完"），
   菜单 55 字符、状态 47 字符、`IP` 只放两行地址。

### 陷阱 9：应用内 HTTP 服务收不到入向连接

**现象**：`192.168.0.1:8080`（模组的 RNDIS 地址）TCP 连接能建立（不 RST），
但发 `GET / HTTP/1.0` **返回 0 字节**（25 秒超时）；IPv6 侧 `[<ip6>]:8080` 直接连接超时。
PC 侧 `arp -a` 能看到 `192.168.0.1` 的二层应答 ⇒ RNDIS 数据面本身是通的。

**根因**：入向连接由**内核栈**应答，而应用的 `cm_asocket` 监听在**另一个栈**上；
应用**无权改网卡/路由配置**（用户 APP 可调用的 326 个符号里，usb / rndis / netif 相关的
**一个都没有**——这是链接器实证，不是猜测）。这属于平台"栈分离"的固有约束。

**解法（绕开而不是硬修）**：

- **Web 面板不从外部访问**，只作为本地/RNDIS 侧的可视化手段；
- 增加**出向推送**：面板状态渲染成文本后**主动 POST** 到机器人（`面板` / `PANEL` 指令触发）；
- 短信成为**唯一可靠的远程观测通道**：`STATUS` 看状态、`IP` 看地址、`面板` 拉面板快照。

### 陷阱 10：应用层无法运行时切换 USB/RNDIS

同上：链接边界决定了应用**没有** RNDIS 的开关权限。
固件只能把开关**保存为配置期望值**，并如实提示"由底包/NV 决定，重启后生效"。

### 陷阱 11：整包刷写会擦 `nvm` / `factory`

整包的 `download.json` 里有 `erase nvm` / `erase fota_param` / `erase fota_pkg`，
production 模式下还有 `erase factory` 与 `fuse`。实测**整包刷写后 IMEI/SN 会丢失**
（本工程还专门写了写回脚本）。**能同源就只刷 app-only**；
真要刷整包，务必先备份 IMEI/SN，并且**永远不要加 `-m`**。

### 陷阱 12：`AT$MYDOWNLOAD=1` 依赖内核版本

出厂 `MTSH1S00` 内核上返回 `ERROR`（494 条受支持命令里没有它），
只有 **MBR 基线**内核才有 `$MYDOWNLOAD: (0,1)`。所以"能不能免短接进下载模式"
取决于设备当前内核——先 `ATI` 确认，再决定要不要找 BOOT_MODE 触点。

### 陷阱 13：诊断通道是无 UART 日志时的救命稻草

本底板没有可用的 UART 日志出口（`cm_log_printf` 是否真从 UART0 出来也不确定）。
所以工程里做了 `sdk_diag_post()`：把应用内部状态 HTTP POST 到 PC 上的
`tools/diag_listen2.ps1`（默认 `192.168.0.100:9090/diag`）。
上面陷阱 2/4/5 的定位，全靠这条通道拿到的证据。

---

## 十三、常见问题排查

### 13.1 收不到短信回执

按顺序查：

1. **应用真的在跑吗**：`AT+CMGF?` 应为 `+CMGF: 0`、`AT+CNMI?` 应为 `+CNMI: 2,2,0,0,0`。
   若是 `+CMGF: 1`，说明还是旧固件或应用没起来。
2. **你是管理员吗**：出厂管理员号为空。先发 `我要成为管理员`。发 `STATUS` 看 `管:` 一行。
3. **发送侧是否失败**：开诊断上报（`tools/diag_listen2.ps1`），看 `tx init=… ret=… len=…`。
   `ret=-1` 一般就是[陷阱 2](#陷阱-2cm_virt_at_sms_send-的回调必须不取数据)。
4. **SMSC 是否读到**：`AT+CSCA?`（形如 `+CSCA: "+8613800738500",145`）。
   读不到时固件用 PDU 的 `00`（默认短信中心）。
5. **SIM/信号**：`AT+CPIN?` 要 `READY`，`AT+CSER`/`AT+CSQ` 正常，`AT+CEREG?` 要 `0,1`。
6. **外口对照实验**：用 PC 直接驱动 AT 口发一条
   `AT+CMGS=<len>\r` → 等 `>` → PDU → Ctrl-Z，看手机能否收到。
   能收到 ⇒ 问题在应用侧；收不到 ⇒ 查短信中心/SIM。

### 13.2 钉钉推送失败（310000 / errcode 非 0）

- `310000`：安全校验失败。检查关键词是否真的在正文里（可先只配 `K` 不配 `T` 验证）；
  或加签密钥/时钟问题。
- 时钟：`STATUS` 里看 `时:` 字段。`加签:已降级(时钟未同步)` 说明 NTP 没成功、
  模组时钟也不可信（刚上电时 `AT+CCLK?` 通常是 2000 年或 1980 年 —— 明显不可信）。
  可以：① 等 NTP 成功（需要数据通路正常）；② 用 `AT+CCLK="yy/MM/dd,hh:mm:ss+zz"` 手动设一次。
- 想快速验证通路：先把 `U` 指向 PC 上的 `tools/diag_listen2.ps1`（http），
  排除 TLS 因素，再切回 https。
- HTTPS 证书：优先用 `ca.pem` 校验；没有则关闭校验并在日志告警（企业微信/钉钉都是 https，
  要么配 CA，要么接受"不校验"）。

### 13.3 IPv6 地址不变 / 拿不到

- IPv6 需要 PDP 是 `IPV4V6`：`AT+CGDCONT?` 看 `cid=1` 的类型。固件面板有"一键开启 IPv6"，
  它会 `AT+CGDCONT=1,"IPV4V6",<apn>` 并**重新激活 PDP**。
- `AT+CGPADDR=1` 一次读双栈，三种应答形态都要兼容：
  `+CGPADDR: 1,"10.0.0.100","2409:xxxx:xxxx:xxxx::1"`（双栈）、
  `+CGPADDR: 1,"10.0.0.100"`（纯 v4）、`+CGPADDR: 1,"","2409:xxxx:xxxx:xxxx::1"`（纯 v6）。
- 地址不变：重新激活 PDP 会拿到新地址；同一 PDP 会话内地址不会变。
- 公网 IPv4 在运营商 NAT 后是拿不到的，回显服务返回的可能是 NAT 后的地址；
  公网 **IPv6** 是真实全局地址（`2409::/` 是中国移动的全球单播段）。
- 公网 IP 回显：固件**强制 v4 优先**（否则 DNS 默认 v6 优先会量到 v6 出口地址，量错对象）。
  默认 URL `http://ip.3322.net`，可在面板改成别的回显服务。

### 13.4 短信中心（SMSC）

- 开机时固件用 `AT+CSCA?` 读取并存缓存。**SDK 文档要求把 SMSC 写进 PDU**：
  成功时用读到的号码（`+8613800738500,145` 形式），失败时用 PDU 的 `00`（用模组默认）。
- 如果短信发不出去但 `AT+CMGS` 外口测试正常，检查 `AT+CSCA?` 是否为空 →
  用 `AT+CSCA="+8613800738500",145` 设置。
- 换了 SIM 卡后 SMSC 一般由卡/网络提供，`AT+CSCA?` 会返回新值；若返回空，手动设一次。

### 13.5 换卡与 APN 自动轮换

- 固件内置 APN 表 `cmnet` → `3gnet` → `ctnet`（移动 / 联通 / 电信），
  注册/PDP 失败时自动切下一个，并在心跳里继续保活。
- 换卡后建议：重启模组（或让 `net_mgr` 走一轮切换），并检查 `AT+CEREG?` 是否 `0,1`。
- 特殊卡（物联网卡/定向卡）可能需要自定义 APN，请改 `app_config.c` 的 `g_apn_table` 后重编。

### 13.6 长期供电与稳定性

- **5V/2A 直供**，不要走劣质 HUB；LTE 发射瞬间电流冲击很大，供电不足会表现为随机重启。
- 观察重启原因：开机横幅会打印上次复位原因，可识别看门狗复位。
- 定期看 `STATUS` 的收发计数与信号；信号长期低于 15 建议改善天线位置。
- 长时间运行建议打开月度流量限额与告警，避免流量跑飞。
- 设备放在通风处；模组长时间发射会发热。

### 13.7 上电就重启循环

- 最常见原因：**应用镜像与内核不同源**（`cm_stub_DSLN.o` 是硬编码内核地址的跳转表）。
  → 用 `ATI` 确认内核版本，与 SDK 基线对照；不同源请烧整包。
- 其次：**栈溢出**（大对象上了栈）→ 跑 `app/test/check_stack_budget.ps1`。
- 再次：供电不足。
- 定位手段：启动探针 `:8081`（看卡在哪一步）与诊断上报。

---

## 十四、免责声明

1. **合规使用**。本项目是通用短信转发与远程配置工具。请自行确保使用方式符合当地法律法规
   （包括但不限于无线电管理、电信业务、个人信息保护、反垃圾短信相关规定）
   以及 SIM 卡所属运营商的条款。**不得用于发送垃圾短信、诈骗、骚扰或任何违法用途。**
   由此产生的一切后果由使用者承担。
2. **vendor SDK 与工具不随本仓库分发**。中国移动 OpenCPU SDK、预编译底包镜像、
   `aboot`/`adownload`/`arelease` 刷写与打包工具、官方 PDF 手册等均为各自权利人所有，
   本仓库**不包含、也不再分发**它们。请从合法渠道自行获取，并遵守其许可协议。
   本仓库只包含**我们自己编写的应用源码、脚本、文档**与**编译产物（应用镜像）**。
3. **刷机风险自担**。刷写固件可能导致设备变砖、丢失 IMEI/SN/产测数据、失去保修。
   本仓库的作者与贡献者**不对任何直接或间接损失负责**。
   刷写前请备份关键标识、准备好整包与 BOOT_MODE 恢复方案。
4. **无担保**。软件按"原样"提供，不提供任何明示或暗示的担保，包括但不限于
   适销性、特定用途适用性与非侵权性。
5. **隐私**。本仓库内的所有号码、密钥、设备标识、IP 地址均为**占位符或示例值**，
   详见 [PRIVACY.md](PRIVACY.md)。

---

## 十五、License

本项目采用 **MIT License**，全文见 [LICENSE](LICENSE)。

```
Copyright (c) 2026 ML307A-DSLN SMS Gateway contributors
```

> 注意：MIT 仅覆盖**本仓库内我们编写的代码、脚本与文档**。
> vendor SDK、底包镜像与第三方工具**不在**本许可范围内，请遵循各自的许可协议。
