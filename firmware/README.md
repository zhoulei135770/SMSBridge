# firmware/ —— 应用镜像与刷写脚本

本目录只放**我们编译出来的应用镜像**与配套的刷写脚本。

---

## 目录内容

| 文件 | 说明 |
|---|---|
| `user_app.bin` | **应用镜像（app-only）**。由本仓库 `app/` 源码经 ML307A OpenCPU SDK 编译、`arelease` 打包链路的产物中抽出。烧进设备的 `user_app` 分区。 |
| `user_app.bin.sha256` | `user_app.bin` 的 SHA256 校验值（`Get-FileHash -Algorithm SHA256` 生成）。 |
| `flash_apponly.ps1` | **可复用的 app-only 刷写脚本**，参数化调用官方 `adownload.exe`。见下文。 |

> 本目录**只有应用镜像**。vendor 的 `preboot.img` / `flasher.img` / `flashinfo.bin` /
> `partition.bin`，以及整包 `ML307A_DSLN_APP.zip`、OpenCPU SDK、`aboot`/`adownload`
> 刷写工具，**都不随本仓库分发**，请按下文自行获取。

---

## 这个镜像是什么（以及不是什么）

- **是**：应用层固件。包含短信网关的全部业务逻辑（短信收发与 PDU 编解码、长短信重组、
  远程配置、钉钉/企业微信转发、网络管理与公网地址查询、Web 面板与出向状态推送等）。
- **不是**：完整固件包。它**不包含**内核（`cp.bin`）、`system.img`、`bootloader.ubi`、
  `rf.bin`、`nvm`、`factory` 等分区。
- **因此**：刷写它**不会**改动内核，**不会**擦 `nvm`/`factory`，也就**不会**丢 IMEI——
  前提是**设备当前内核与编译此镜像所用的 SDK 基线同源**（见下）。

镜像尺寸与校验值：

```powershell
Get-Item firmware\user_app.bin | Select-Object Name, Length
Get-FileHash firmware\user_app.bin -Algorithm SHA256
Get-Content firmware\user_app.bin.sha256
```

---

## 前提：app-only 必须与设备内核同源

应用镜像里链接了 SDK 提供的 `cm_stub_DSLN.o`（**硬编码内核绝对地址的跳转表**）。
它与设备实际运行的内核**必须来自同一基线**，否则调用会跳到错误地址，表现为上电即崩溃 / 重启循环。

- 编译本仓库前，请先确认 SDK 版本（本工程验证的是 **ML307A_OpenCPU_Standard_1.4.2**）。
- 刷写前，请先读设备内核版本：`ATI` → 形如 `ML307A-DSLN-MBRH1S00`。
- **若内核版本与 SDK 基线不一致 ⇒ 不要用 app-only，请烧整包**（整包会顺带更新内核）。
  整包刷写有擦 `nvm`/`factory` 的风险，请自行评估与备份。

---

## 刷写：`flash_apponly.ps1`

### 它做什么

1. 从 `--ReleaseZip` 指定的 SDK 整包中抽出 `preboot.img` / `flasher.img` /
   `flashinfo.bin` / `partition.bin`（**只抽这 4 个刷写器必需文件**）；
2. 换上 `--AppBin` 指定的应用镜像为 `user_app.bin`；
3. 生成 app-only 的 `download.json`（**不含任何 `erase` / `fuse` / `productionOnly`**，
   脚本会自检并拒绝生成破坏性包）；
4. 通过**正常模式 AT 口**（默认 `COM3 @115200`）发软入口 `AT$MYDOWNLOAD=1`，
   让模组自己重启进 aboot 下载模式（**免短接 BOOT_MODE**）；
5. 调 `adownload.exe -u -a -r -q -s 115200 <包>` 刷写；
6. 轮询 AT 口，等设备回到正常模式，并打印 `ATI` / `AT+CPIN?` / `AT+CMGF?` / `AT+CNMI?`
   做一次快速自检。

### 参数

```
-ReleaseZip     SDK 整包 ML307A_DSLN_APP.zip 路径（默认 <仓库根>\sdk\...\ML307A_DSLN_APP.zip）
-AdownloadExe   adownload.exe 完整路径（必填；工具需自行获取）
-AppBin         要刷的应用镜像（默认 firmware\user_app.bin）
-AtPort         正常模式 AT 串口（默认 COM3）
-BaudRate       AT 口/下载口速率（默认 115200）
-WorkDir        解包组包临时目录（默认 %TEMP%\ml307a_apponly_flash）
-NormalPortTimeoutSec  发软入口后等 AT 口消失的秒数（默认 30）
-FlashTimeoutSec       adownload.exe 最长执行秒数（默认 600）
-ReturnTimeoutSec      刷完后等设备回正常模式的秒数（默认 120）
-SkipEnterDownload     跳过第 4 步（已用 BOOT_MODE 短接等方式进下载模式时）
-DryRun                只抽包+组包+打印命令行，不碰设备、不刷写
```

### 用法

```powershell
# 0) 先干跑，只验证包能否正确组出来（不接触设备）
pwsh -File firmware\flash_apponly.ps1 `
    -AdownloadExe C:\aboot-tools\adownload.exe `
    -ReleaseZip C:\sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip `
    -DryRun

# 1) 正式刷写（COM3 @115200）
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe

# 2) 换串口 + 刷自己刚编译出来的镜像
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe `
    -AtPort COM7 `
    -AppBin sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.bin
```

### ⚠ 绝对不要加 `-m`

`adownload.exe` 的 `-m` / `--production` 是**生产模式**，会让 `download.json` 里标了
`"productionOnly": true` 的动作（`erase factory`、`fuse`）真正执行，**可能抹掉 IMEI 与产测数据**。
本脚本刻意不提供该开关。

### 软入口不可用时怎么办

`AT$MYDOWNLOAD=1` 只在 **MBR 基线内核**上有效；出厂 `MTSH1S00` 内核会返回 `ERROR`
（实测：494 条支持的 AT 命令里没有它）。此时只能：

- 找到底板的 BOOT_MODE / KEYOUT1 触点（管脚 82），**短接到 GND 后上电**进下载模式；
  具体触点与丝印请查你手上底板的原理图——**不要凭猜测短接**；
- 进下载模式后再执行（跳过软入口那一步）：

```powershell
pwsh -File firmware\flash_apponly.ps1 -AdownloadExe C:\aboot-tools\adownload.exe -SkipEnterDownload
```

---

## 刷写成功的判据

`adownload.exe` 日志里应能看到：

```
<COMx> processing command [partition partition.bin]
<COMx> #=> [INFO: Aboot ] published N partition information table.
<COMx> processing command [flash user_app user_app.bin]
<COMx> (flasher) programming partition 'user_app' with datap = 0x..., size = 0x...
<COMx> (flasher) program "user_app" done.
<COMx> processing command [reboot]
... "status": "SUCCEEDED" ... all finished. total time: xx.xxxs
```

回到正常模式后，用 AT 口确认**新应用真的在跑**：

```
AT+CMGF?   ->   +CMGF: 0            ← PDU 模式，本固件已接管
AT+CNMI?   ->   +CNMI: 2,2,0,0,0    ← +CMT 直投已开启
```

再用管理员手机号发一条 `STATUS`，应收到状态回执短信。

---

## 免责声明

- vendor SDK、`arelease`/`aboot`/`adownload` 工具**不随本仓库分发**，请自行从合法渠道获取并遵守其许可。
- 刷机有风险（变砖、丢失 IMEI/产测数据、失去保修）。**请自行评估并承担全部后果**。
- 本仓库只做合规用途的说明；使用者须自行确保符合当地法律法规与运营商条款。
