# ML307A-DSLN 编译与烧录流程核实

> 核实时间：本轮调研
> 核实对象：中移物联网 ML307A-DSLN（ASR1606 / CRANEL 平台）OpenCPU 固件
> 证据等级约定：
> - **【SDK原文】** = 来自 SDK 仓库/本地 SDK 副本的实际文件内容（第一手）
> - **【工具实证】** = 来自本机已有工具二进制/配置的字符串或配置项（第一手，但属逆向观察）
> - **【社区经验】** = 中文技术博客/论坛，非官方
> - **【未证实】** = 本次无法获得可靠依据，禁止臆造
>
> ⚠️ 重要环境事实（与任务前提不符，必须先说明）：
> 1. **本工作区已经存在完整 SDK 与一次成功编译产物**，并非"没有 SDK"：
>    - SDK 根目录：`sdk\ML307A_OpenCPU_Standard_1.4.2\`
>    - 已生成的烧录包：`sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip`（10,392,274 字节）
>    - 因此本文第 1～3、5 章结论大部分可直接在本机复核，无需再联网。
> 2. **本环境无法读取 PDF**：`web_fetch` 对 `application/pdf` 与 `application/octet-stream` 均直接报错
>    （`unsupported content type`），且 shell 无出网能力（`Invoke-WebRequest`/`curl.exe` 均 TLS 失败）。
>    所以中移官方 PDF 手册（SDK开发指导手册、EVB用户手册、硬件设计手册、AT手册）本轮**未能读取正文**，
>    相关条目一律标注为【未证实】或仅以【社区经验】佐证。这是本次调研最大的证据缺口。

---

## 结论速览

1. **编译不需要 Keil，也不需要装任何外部工具链。** 官方流程是 `ML307A_build.bat <子型号>` → 内部调用
   **GNU make + 仓库自带的 arm-none-eabi-gcc 8.3.1**，目标架构 `-mcpu=cortex-r5`。
   PATH 由 bat 自己临时拼装（`tools`、`tools\utils\python3`、`tools\toolchain\gcc-arm-none-eabi\bin`、
   `tools\scripts`、`tools\utils`、`tools\aboot`），即"绿色便携"方案。【SDK原文】

2. **编译产物是 `.elf` / `.bin` / `.map`，不是 `.axf`。** 输出目录固定为
   `out/images/<MODEL_NAME>/<MODEL_TARGET>/`，对 DSLN 即 `out/images/ML307A/DSLN/`，
   文件为 `ML307A_DSLN_APP.elf`、`ML307A_DSLN_APP.bin`、`ML307A_DSLN_APP.map`（本机实测 121,856 / 29,472 / 713,177 字节）。【SDK原文】

3. **SDK 里根本不存在 `.pac`。** 全仓库（含 bat/mk/py/json 与所有配置）对 `.pac` 零命中。
   官方打包脚本 `tools\scripts\pack.bat` 用 ASR 的 `tools\aboot\arelease.exe` 把**基线固件包 + 用户 APP**
   合成**一个 zip**：`ML307A_DSLN_APP.zip`。`.pac` 是**展锐 UNISOC** 平台的格式，
   与 ML307A/ASR1606 **不是同一体系**。【SDK原文】

4. **"1MB Flash"指的是用户 APP 分区/链接区，不是模组总 Flash。** 链接脚本原文
   `flash (rwx): ORIGIN = 0x805F8000, LENGTH = 1024K` 与分区表 `user_app size = 1024KiB` 完全吻合；
   DSLN 整片 QSPI NOR 是 **8MiB**（`FLASH_OPT=8M` → `QSPI_NOR_8MB_...`）。
   而 **APP 可用 RAM 只有 512K**（`ram (rwx): ORIGIN = 0x7E380000, LENGTH = 512K`），
   任务里"3135KB RAM"这一数字在 SDK 中**找不到依据**，应视为 SoC 规格而非 OpenCPU 应用可用内存。【SDK原文】

5. **用户下载的 QFlash 是"和而不同"的工具**：它是 **Quectel（移远）** 的 QFlash V8.1，其主体是
   **展锐 UNISOC 下载工具**的换皮（`Setting\UpgradeDownload.ini` 里写着 `UserGuide = UNISOC Download User Guide V4.1.pdf`），
   但它**另外内置了 ASR 平台的下载器** `ASR\1\adownload1.exe`
   （自述字符串：*"Asrmicro aboot download console application"*），
   其引擎 `aboot.dll` 能解析 **`download.json` 形式的 aboot release package**，并认识 `arom-tiny` / `crane` 标识——
   与 ML307A SDK 的 `ASR_CRANEL_EVB.json`（`"product": "arom-tiny"`、`"firmwareGenerator": {"name": "crane"}`）**同源**。
   但**全工具内对 `ML307`/`CRANEL`/`ASR1606` 零命中**，所以"QFlash 能否直接烧 ML307A-DSLN"**属推断，未证实**。【工具实证】

6. **日志串口有 SDK 原文依据：UART0，921600 波特率。** `custom\applaction\main.c` 里
   `uart_open(CM_UART_DEV_0, 921600, u0_callback);` + `u0_printf("ML307A Start\r\n");`，
   且 `drv_uart.c` 把 `CM_UART_DEV_0` 复用为 `CM_IOMUX_PIN_17/PIN_18`（FUNCTION1）。
   而**"无 BOOT/RST 按键时短接哪两个触点、时序如何"本轮未获得任何官方或可靠依据，必须实测确认**。【SDK原文】/【未证实】

---

## 1. 官方编译流程（build.bat/README 原文摘录 + 逐条解读 + 依赖工具清单）

### 1.1 官方编译入口：`ML307A_build.bat`（仓库根目录）

原文（**逐字**，本机 `sdk\ML307A_OpenCPU_Standard_1.4.2\ML307A_build.bat`，与 GitHub/Gitee raw 抓取内容一致）：

```bat
@goto start
:: ML307A OpenCPU编译脚本，请勿修改
:: 当前支持DSLN/DCLN/GSLN/GCLN子型号编译
:: SDK编译路径不支持中文、空格

:start

:: 参数设置
@set PROJECT_ROOT=%CD%
@set MODEL_NAME=ML307A
@set HW_REVISION=H0
@set CURRENT_HOUR=%TIME:~0,2%
@if %CURRENT_HOUR% LEQ 9 set CURRENT_HOUR=0%TIME:~1,1%
@set SW_REVISION=%DATE:~0,4%%DATE:~5,2%%DATE:~8,2%%CURRENT_HOUR%%TIME:~3,2%
@set BUILD_CMD=all
@set GNSS_OPT=NO
@set FLASH_OPT=4M

@if /i "%1" == "DCLN" (
    @set MODEL_TARGET=DCLN
    @set FLASH_OPT=4M
    @set GNSS_OPT=NO
)else if /i "%1" == "DSLN" (
    @set MODEL_TARGET=DSLN
    @set FLASH_OPT=8M
    @set GNSS_OPT=NO
)else if /i "%1" == "GSLN" (
    @set MODEL_TARGET=GSLN
    @set FLASH_OPT=8M
    @set GNSS_OPT=YES
)else if /i "%1" == "GCLN" (
    @set MODEL_TARGET=GCLN
    @set FLASH_OPT=4M
    @set GNSS_OPT=YES
)else if /i "%1" == "CLEAN" (
    @set BUILD_CMD=clean
)else if /i "%1" == "help" (
    @echo usage: build.bat version demo
    @echo     version:
    @echo       DCLN:compile ML307A DCLN H0 version
    @echo       DSLN:compile ML307A DSLN H0 version
    @echo       GSLN:compile ML307A GSLN H0 version
    @echo       GCLN:compile ML307A GCLN H0 version
    @echo       CLEAN:clean the project
    @exit /B
)else (
    @echo param error
    @echo example: ML307A_build.bat DCLN
    @exit /B
)

@if /i "%2" == "demo" (
    @set DEMO_SUPPORT=y
)else (
    @set DEMO_SUPPORT=n
)

:: 环境变量配置
@call :add_path %PROJECT_ROOT%\tools
@call :add_path %PROJECT_ROOT%\tools\utils\python3
@call :add_path %PROJECT_ROOT%\tools\toolchain\gcc-arm-none-eabi\bin
@call :add_path %PROJECT_ROOT%\tools\scripts
@call :add_path %PROJECT_ROOT%\tools\utils
@call :add_path %PROJECT_ROOT%\tools\aboot

:: 执行编译
@make  -f tools\scripts\Makefile %BUILD_CMD% -j4 -e ^
MODEL_NAME=%MODEL_NAME% MODEL_TARGET=%MODEL_TARGET% HW_REVISION=%HW_REVISION% SW_REVISION=%SW_REVISION% DEMO_SUPPORT=%DEMO_SUPPORT%

@if %ERRORLEVEL% neq 0 (
    @echo compile error
    @exit /B
)

:: 固件打包
@ if /i "%BUILD_CMD%" == "all" (
    @call tools\scripts\pack.bat
)

:add_path
@(echo ";%PATH%;" | find /C /I ";%1;" > nul) || set "PATH=%1;%PATH%"
@goto :eof
```

`ML305A_build.bat` 结构完全相同，只是 `MODEL_NAME=ML305A`，子型号为 `DC`（`FLASH_OPT=4M`）/`DS`（`FLASH_OPT=8M`），且**没有 GCLN/GSLN 两个 GNSS 型号分支**。

#### 逐行/逐块解读

| 原文要点 | 解读 |
| --- | --- |
| `SDK编译路径不支持中文、空格` | **硬约束**。SDK 放在含中文或空格的路径下会失败。 |
| `@set PROJECT_ROOT=%CD%` | **必须在 SDK 根目录下执行 bat**（用 `%CD%` 而不是 `%~dp0`），否则路径全错。 |
| `MODEL_NAME=ML307A` / `MODEL_TARGET=DSLN` | 二者共同决定输出目录 `out/images/ML307A/DSLN`、目标名 `ML307A_DSLN_APP`、链接脚本 `prebuild/ld/ML307A/DSLN/app_dsln.ld`。 |
| `HW_REVISION=H0` | 硬编码为 H0，并作为 `-DCM_PROD_VERSION_H0` 传入编译。 |
| `SW_REVISION=...` | 由 `DATE`/`TIME` 拼出的时间戳（**注意**：依赖本地日期时间格式，非中文区域设置下格式可能不同）。 |
| `FLASH_OPT` / `GNSS_OPT` | **不通过命令行传给 make**，而是用 `@set` 变成环境变量；随后 `pack.bat` 在同一个 cmd 会话里用 `%FLASH_OPT%`/`%GNSS_OPT%` 读取。所以它们是**打包阶段的开关**，不影响编译期宏。DSLN 的取值是 `FLASH_OPT=8M`、`GNSS_OPT=NO`。 |
| 第 2 个参数 `demo` | `DEMO_SUPPORT=y` → 编译 `examples\*` 全部 demo（`examples/examples.mk`），否则 `DEMO_SUPPORT=n` 编 `custom\*` 用户代码（`custom/custom.mk`）。**注意 `-e` 参数使环境变量覆盖 Makefile 内定义**，所以 bat 传的值生效。 |
| `:add_path` 里的 `find /C /I` | 幂等地把目录插到 PATH 最前（已存在则不重复添加）。 |
| `make ... -j4 -e` | `-j4` 四路并行；`-e` = environment overrides makefile。 |
| `@call tools\scripts\pack.bat` | **只有 `all` 目标才打包**；`clean` 不会打包。 |

#### 官方帮助文本 / 使用方式

```
usage: build.bat version demo
    version:
      DCLN:compile ML307A DCLN H0 version
      DSLN:compile ML307A DSLN H0 version
      GSLN:compile ML307A GSLN H0 version
      GCLN:compile ML307A GCLN H0 version
      CLEAN:clean the project
```

**给 ML307A-DSLN 用的命令（官方推荐顺序）**

```bat
:: 1) 清理历史
ML307A_build.bat clean
:: 2) 编译用户代码（custom/）
ML307A_build.bat DSLN
:: 3) 若编译 examples 里的全部示例
ML307A_build.bat DSLN demo
```

（第 2、3 步的顺序与"先 clean 再编译"的推荐做法，见【社区经验】：CSDN 陈大鱼《[OpenCPU开发]（二）中移ML307A模组二次开发编译和烧录》明确写
"2. 在SDK根目录下，执行 ML307A_build.bat clean 清除历史编译信息。3. 在SDK根目录下，执行 ML307A_build.bat DSLN 启动编译。如无编译错误，可在 out/images/ 目录下获取编译生成的软件版本。"）

### 1.2 编译规则：`tools\scripts\Makefile`（原文摘录）

```make
# 配置文件
include tools/scripts/config.mk

# 库文件及用户代码
OC_RAM_FILES :=
OC_FILES :=
OC_LIBS :=
INC :=
CDEFINES :=
include src/cm_lib/cm_lib.mk
include third-party/third-party.mk
ifeq ($(DEMO_SUPPORT),y)
include examples/examples.mk
else
include custom/custom.mk
endif

# 版本控制及目标文件
ifeq ($(MODEL_TARGET),)
endif
APP_IMG_DIR := out/images/$(MODEL_NAME)/$(MODEL_TARGET)
APP_TARGET := $(MODEL_NAME)_$(MODEL_TARGET)_APP
APP_LD_OLD := prebuild/ld/$(MODEL_NAME)/$(MODEL_TARGET)/app_$(MODEL_TARGET).ld
APP_LD := $(APP_IMG_DIR)/$(APP_TARGET).ld.obj
APP_CORE_STUB := prebuild/objs/$(MODEL_NAME)/$(MODEL_TARGET)/cm_stub_$(MODEL_TARGET).o

# GCC配置
CC := arm-none-eabi-gcc
CXX := arm-none-eabi-g++
AR  := arm-none-eabi-ar
OBJCOPY  := arm-none-eabi-objcopy
SIZE := arm-none-eabi-size

# 编译参数
CFLAGS :=  -mthumb-interwork  -std=gnu11   -mcpu=cortex-r5     -mno-unaligned-access -g --specs=nosys.specs -Wall -ffunction-sections -fdata-sections
CDEFINES += -DCM_HARDWARE_VERSION=\"$(MODEL_TARGET)\" -DCM_HARDWARE_$(MODEL_TARGET)_SUPPORT -DCM_OPENCPU_MODEL_$(MODEL_NAME) -DCM_PROD_VERSION_$(HW_REVISION) -DCM_BUILD_TIMESTAMP=\"$(SW_REVISION)\"
LDFLAGS := -lgcc -lc $(APP_CORE_STUB) -Wl,--gc-sections  -nostartfiles --specs=nosys.specs  -mcpu=cortex-r5  -mtune=generic-armv7-r   -mno-unaligned-access  -Wl,-Map=$(APP_IMG_DIR)/$(APP_TARGET).map  -T $(APP_LD) -Wl,--start-group $(OC_LIBS) -Wl,--end-group
INC += -Iinclude\cmiot -Iinclude\platform ...
```

```make
# 生成APP文件
all: check  $(OBJS) $(OBJS_RAM) $(APP_LD)
	@echo "linking $(MODEL_NAME) libs..."
	@echo "Generating $(APP_TARGET).elf..."
	@$(CXX)  $(OBJS) $(OBJS_RAM) $(LDFLAGS) -o $(APP_IMG_DIR)/$(APP_TARGET).elf
	@$(SIZE) -B $(APP_IMG_DIR)/$(APP_TARGET).elf
	@echo "Generating $(APP_TARGET).bin..."
	@$(OBJCOPY) -O binary -R .note -R .comment -S $(APP_IMG_DIR)/$(APP_TARGET).elf $(APP_IMG_DIR)/$(APP_TARGET).bin
	@echo "Done"

$(APP_LD): $(APP_LD_OLD)
	@$(CXX) -E -P -w - <$(APP_LD_OLD) -o $(APP_LD)

check:
	@do_check.bat $(addprefix build\,$(subst /,\,$(SRC_DIRS)))
	...
```

**要点**
- 编译器确为 **GCC（arm-none-eabi-gcc/g++ 8.3.1）**，`-mcpu=cortex-r5`，`--specs=nosys.specs`，`-nostartfiles`。
- 最终链接使用 **C++ 驱动**（`$(CXX)`）但对象是 C 代码；链接脚本是 `prebuild/ld/ML307A/DSLN/app_dsln.ld` 经 `-E -P` 预处理后的 `out/images/ML307A/DSLN/ML307A_DSLN_APP.ld.obj`。
- **核心桩文件** `prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o` 参与链接（`LDFLAGS` 里显式给出），这是与预编译内核对接的关键。
- 编译期宏只有这些：`CM_HARDWARE_VERSION`、`CM_HARDWARE_<DSLN>_SUPPORT`、`CM_OPENCPU_MODEL_ML307A`、`CM_PROD_VERSION_H0`、`CM_BUILD_TIMESTAMP`（外加 `examples` 里的 `CM_LBS_SUPPORT`/`CM_SLEEP_SUPPORT`）。

### 1.3 配置文件 `tools\scripts\config.mk`（全文）

```make
HWVER :=DCLN 
DEMO_SUPPORT:=n
MODEL_NAME:=ML307A 
PROD_VER:=H0 
BuildTimeStamp:=20220810153543 
```

> 注意：这些是**默认值**，会被 bat 通过 `-e` 与命令行变量覆盖（`HWVER` 这个变量实际未被 bat 传，属于遗留项）。

### 1.4 README.md 说了什么（原文摘录）

README 只描述目录与手册清单，**不含编译命令**：

```
# 中移物联网 OpenCPU SDK 简介 

## **注意事项（必读）**
ML302A所以型号开发未完成，用户不可使用

## **代码目录**
| 名称          | 描述                       |
| ------------- | --------------------------|
| custom        | 用户代码目录、用户程序入口  |
| docs          | 文档                      |
| examples      | 示例代码                  |
| include       | OpenCPU API 头文件        |
| out           | 编译输出目录               |
| prebuild      | 预编译文件                 |
| third-party   | 第三方开源代码库            |
| tools         | 编译工具脚本               |

## **指导手册**
| 名称             | 描述                           |
| SDK开发指导      | 编译环境、SDK编译说明、下载调试  |
| API手册          | API全览说明文档，chm格式        |
| 资源综述          | 软硬件资源说明                 |
| 操作系统开发指导  | 互斥锁、信号量、定时器 ...      |
| 外围接口开发指导  | GPIO、UART、I2C、SPI ...      |
| 数据业务开发指导  | 网络状态、连接、数据发送        |
```

**关键信息**：
- README 官方指定"**编译环境、SDK编译说明、下载调试**"的权威出处是 **《SDK开发指导》手册**——但该 PDF **不在仓库里**（仓库 `docs/` 只有 `ML305A_ML307A_OpenCPU_API手册.chm`），需要在 [Gitee YZRDEG/ML307A](https://gitee.com/YZRDEG/ML307A) 的 `ML307A_OPENCPU手册/ML307A_SDK开发指导手册.pdf` 获取。本轮因环境无法读 PDF 未能引用其正文。
- "ML302A 所以型号开发未完成，用户不可使用" → **不要选 ML302A**。

### 1.5 依赖工具清单（结论）

| 依赖 | 是否需要自备 | 依据 |
| --- | --- | --- |
| **ARM 交叉编译器** | ❌ 不需要 | 仓库自带 `tools\toolchain\gcc-arm-none-eabi\bin\arm-none-eabi-gcc.exe`，实测版本 `arm-none-eabi-gcc.exe (GNU Tools for Arm Embedded Processors 8-2019-q3-update) 8.3.1 20190703 (release)` |
| **make** | ❌ 不需要 | 仓库自带 `tools\utils\make.exe`（219,662 字节），bat 已把 `tools\utils` 加入 PATH |
| **python3** | ❌ 不需要 | 仓库自带 `tools\utils\python3`（含 `python36.zip` 与 `Crypto` 等包）；`pack.bat` 调用 `python tools\scripts\unzip.py` |
| **GCC（宿主）** | ❌ 不需要 | 同上，全部为预编译绿色包 |
| **Keil / MDK** | ❌ **完全不需要** | 全仓库 bat/mk/py/json 对 `keil`/`uvproj`/`MDK` **零命中**；编译命令是 `make` + `arm-none-eabi-gcc` |
| **`arelease`（ASR 打包器）** | ❌ 不需要 | 仓库自带 `tools\aboot\arelease.exe`（5,864,448 字节） |
| **Windows** | ✅ 需要 | bat 脚本、`make.exe`、`arelease.exe` 均为 Windows 可执行文件 |
| **无中文/无空格的路径** | ✅ 硬性要求 | `:: SDK编译路径不支持中文、空格` |

**→ 因此"没有 SDK、没装 Keil"不构成障碍；把 SDK 解压到纯英文无空格路径，双击 bat 即可。**

---

## 2. `prebuild/baseline/ML307A` 内容与底包作用

### 2.1 完整列表（`prebuild/baseline/ML307A/` 下 4 个子目录 × 4 个文件）

用 GitHub contents API 抓取（[API 链接](https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild/baseline/ML307A)）与本机目录比对，结果一致：

| 子型号目录 | 文件 | 大小（字节） |
| --- | --- | --- |
| `DCLN/` | `ML307A-DCLN-MBRH1S00.axf` | 58,554,896（[API](https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild/baseline/ML307A/DCLN)） |
| | `ML307A-DCLN-MBRH1S00.map` | — |
| | `ML307A-DCLN-MBRH1S00_2.0.0.2306201618_release.txt` | — |
| | `ML307A-DCLN-MBRH1S00_2.0.0.2306201618_release.zip` | 6,198,155 |
| **`DSLN/`（← ML307A-DSLN 用这个）** | **`ML307A-DSLN-MBRH1S00.axf`** | **71,065,204** |
| | **`ML307A-DSLN-MBRH1S00.map`** | **7,552,126** |
| | **`ML307A-DSLN-MBRH1S00_2.0.0.2306201630_release.txt`** | **8,659,072** |
| | **`ML307A-DSLN-MBRH1S00_2.0.0.2306201630_release.zip`** | **10,388,175** |
| `GCLN/` | `ML307A-GCLN-MBRH1S00.*` | zip 6,960,114 |
| `GSLN/` | `ML307A-GSLN-MBRH1S00.*` | zip 11,184,318 |

（DSLN 四个文件的准确名称与大小见 [contents API 返回](https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild/baseline/ML307A/DSLN)：
`ML307A-DSLN-MBRH1S00.axf` / `.map` / `ML307A-DSLN-MBRH1S00_2.0.0.2306201630_release.txt` / `..._release.zip`）

> 命名规则：`ML307A-<子型号>-<硬件/版本码><...>`，版本串 `2.0.0.2306201630` = 2.0.0 + 2023-06-20 16:30。
> **`ML307A-DSLN-*` 就是 ML307A-DSLN 对应的底包**，四个子型号各有自己的一份，**不可混用**。

### 2.2 底包在构建/链接流程里的作用（原文依据）

**关键点：`.axf` 并不参与打包。** `pack.bat` 用的是同目录的 **`.zip`**：

```bat
@set BASE_LINE_DIR=%PROJECT_ROOT%\prebuild\baseline\%MODEL_NAME%\%MODEL_TARGET%
@set BUILD_IMG_DIR=%PROJECT_ROOT%\out\images\%MODEL_NAME%\%MODEL_TARGET%
@set BASE_IMG_DIR=%BUILD_IMG_DIR%\DBG
::解压基础固件包
@python tools\scripts\unzip.py %BASE_LINE_DIR% %BASE_IMG_DIR%
```

`unzip.py` 原文：

```python
    file_names = glob.glob(zip_path + "/*.zip")
    for file in file_names:
        unzip_file(file, out_path)
```

**即：`pack.bat` 把 `prebuild/baseline/ML307A/DSLN/*.zip` 解压到 `out/images/ML307A/DSLN/DBG/`。**

解压出来的内容（本机实测 `..._release.zip` 条目）：

| 文件 | 大小 | 作用（据 `download.json`/`fota.json`） |
| --- | --- | --- |
| `cp.bin` | 5,181,440 | **CP（通信处理器）内核镜像** → 分区 `cp` |
| `dsp.bin` | 97,924 | DSP 镜像 → 分区 `dsp` |
| `rf.bin` | 2,126 | 射频参数 → 分区 `rfbin` |
| `ReliableData.bin` | 32,768 | 可靠数据 → 分区 `rd` |
| `apn.bin` | 26,518 | APN 配置 → 分区 `apn` |
| `logo.bin` | 22,452 | 开机 logo → 分区 `logo` |
| `updater.bin` | 61,212 | FOTA updater → 分区 `updater` |
| `bootloader.ubi` | 81,920 | UBI 格式 bootloader → 分区 `bootloader` |
| `preboot.img` / `flasher.img` | 20,908 / 29,276 | aboot 下载引擎用的 preboot/flasher |
| `fwcerts.bin` | 2,036 | 固件证书 |
| `partition.bin` / `flashinfo.bin` | 2,188 / 648 | 分区表 / Flash 信息 |
| `system.img` | 5,390,336 | 系统镜像（FOTA 用，见 `fota.json`） |
| `firmware.bin` | 3,296,791 | 整包固件 |
| **`download.json`** | 2,841 | **aboot 下载描述文件（核心！）** |
| `fota.json` | 96 | FOTA 描述 |

`fota.json` 原文：

```json
[
   {
      "image" : "system.img",
      "partition" : "system",
      "start" : 90112
   }
]
```

`download.json` 原文（节选，注意 `user_app` 这一项在**基线包里只是 `erase`**）：

```json
[
   { "command":"require","group":"0","name":"product","value":"arom-tiny" },
   { "command":"require","group":"0","name":"version","value":"0.5" },
   { "command":"require","group":"0","name":"version-bootrom","value":"2021.10.16" },
   { "command":"progress","group":"0","name":"weight","value":13213696 },
   { "command":"call","group":"0","image":"preboot.img" },
   { "command":"call","group":"0","image":"flasher.img" },
   { "command":"partition","group":"0","image":"flashinfo.bin" },
   { "command":"partition","group":"0","image":"partition.bin" },
   { "command":"flash","group":"1","image":"bootloader.ubi","partition":"bootloader","weight":172032 },
   { "command":"flash","group":"2","image":"partition.bin","partition":"ptable","weight":8192 },
   { "command":"flash","group":"2","image":"fwcerts.bin","partition":"fwcerts","weight":16384 },
   { "command":"flash","group":"2","image":"ReliableData.bin","partition":"rd","weight":65536 },
   { "command":"flash","group":"2","image":"apn.bin","partition":"apn","weight":61440 },
   { "command":"flash","group":"2","image":"cp.bin","partition":"cp","weight":10362880 },
   { "command":"flash","group":"2","image":"dsp.bin","partition":"dsp","weight":196608 },
   { "command":"flash","group":"2","image":"rf.bin","partition":"rfbin","weight":8192 },
   { "command":"flash","group":"2","image":"logo.bin","partition":"logo","weight":49152 },
   { "command":"flash","group":"2","image":"updater.bin","partition":"updater","weight":126976 },
   { "command":"erase","group":"3","partition":"fota_param","weight":12288 },
   { "command":"erase","group":"3","partition":"fota_pkg","weight":4096 },
   { "command":"erase","group":"3","partition":"user_app","weight":1048576 },
   { "command":"erase","group":"4","partition":"nvm","weight":1048576 },
   { "command":"erase","group":"5","partition":"factory","productionOnly":true,"weight":32768 }
]
```

**底包的三重作用**

1. **提供整机固件的所有非用户分区**（bootloader / cp / dsp / rfbin / logo / updater / rd / apn / partition / fwcerts）——因为 OpenCPU 只允许用户写 `user_app` 分区，其余部分必须来自官方基线。
2. **提供 `download.json` 这个"烧录脚本"**，告诉 PC 端下载工具"往哪个分区写哪个文件"。最终包里的 `download.json` 会在 group 6 追加用户 APP（见第 3 章）。
3. **`.axf` / `.map` / `_release.txt` 是符号与调试信息**（厂家预编译内核的 ELF/符号表），供调试定位用；**`build.bat` / `pack.bat` 全程不引用 `.axf`**（全仓库脚本对 `.axf` 零命中）。

### 2.3 `prebuild/` 其它子目录（本机实测）

| 目录 | 内容 |
| --- | --- |
| `prebuild/baseline/{ML302A,ML305A,ML307A}/<子型号>/` | 如上，底包 zip + axf + map + txt |
| `prebuild/ld/{ML302A,ML305A,ML307A}/<子型号>/app_<子型号>.ld` | 链接脚本，如 `ML307A/DSLN/app_dsln.ld` |
| `prebuild/objs/{ML302A,ML305A,ML307A}/<子型号>/cm_stub_<子型号>.o` | 内核桩对象，如 `ML307A/DSLN/cm_stub_dsln.o` |
| `prebuild/libs/` | **只有 3 个库**：`cm_ftp.a`(299,024) / `cm_local_tts.a`(495,874) / `lib_tts.a`(897,938) |

### 2.4 ⭐ 链接脚本给出了权威的内存预算：`prebuild/ld/ML307A/DSLN/app_dsln.ld`

```ld
OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)

ENTRY(cm_opencpu_entry)
...
MEMORY {
    ram (rwx): ORIGIN = 0x7E380000, LENGTH = 512K
    flash (rwx): ORIGIN = 0x805F8000, LENGTH = 1024K
}
...
    ASSERT(__flash_end <= ORIGIN(flash) + LENGTH(flash), "FLASH overflow")
    ASSERT(__ram_end <= ORIGIN(ram) + LENGTH(ram), "RAM overflow")
```

- **`ENTRY(cm_opencpu_entry)`** → 用户程序入口就是 `core\applaction\main.c` 里的 `int cm_opencpu_entry(char *param)`。【SDK原文】
- **APP 可用 Flash = 从 `0x805F8000` 起 `1024K`（1MiB）** ← 这就是"1MB Flash"的真实含义；超了会触发 `"FLASH overflow"` 断言。
- **APP 可用 RAM = 从 `0x7E380000` 起 `512K`**。
- 与分区表互相印证（`tools/aboot/config/partition/CRANEL_SINGLE_FLASH_LAYOUT_8M.json`）：

```json
        { "name": "user_app", "format": "raw", "size": "1024KiB" },
        { "name": "nvm",      "format": "raw", "size": "1024KiB" },
```

- 并解释"DS = 大容量"：`ML307A_build.bat DSLN` 设 `FLASH_OPT=8M`，`pack.bat` 据此选变体 `CRANEL_A0_08MB`；
  `tools/aboot/config/flash/QSPI_NOR_8MB_B64KB_S4KB_P256.json` 原文 `"flashSize": "8MiB"`（`QSPI_NOR_4MB_...` 对应 `FLASH_OPT=4M`）。
  **→ DSLN 整片 QSPI NOR = 8MiB；"1MB"是 APP 分区，不是总容量。**
- 关于 **RAM 3135KB**：SDK 内**无任何依据**（链接脚本只给 APP 512K）。**标注为【未证实】**，需查 ASR1606 数据手册或《ML307A_OpenCPU资源综述.pdf》。

---

## 3. 生成 .pac 的完整链路

### 3.1 先给结论：**SDK 不产出 `.pac`**

- 全仓库检索 `\.pac\b`（含 `*.bat`/`*.mk`/`Makefile`/`*.py`/`*.json`）→ **零命中**。
- 官方打包脚本 `pack.bat` 的输出文件名写死为 **`.zip`**。

**因此"编译产物 → .pac"这条路在 ML307A/ASR1606 上不成立**。`.pac` 属于**展锐 UNISOC** 平台的固件包格式。

### 3.2 真实链路（每一步都有原文依据）

```
ML307A_build.bat DSLN
   │
   ├─(1) 生成链接脚本
   │     prebuild/ld/ML307A/DSLN/app_dsln.ld  --arm-none-eabi-g++ -E -P-->  out/images/ML307A/DSLN/ML307A_DSLN_APP.ld.obj
   │
   ├─(2) 编译 + 链接（arm-none-eabi-g++，含 cm_stub_DSLN.o 与 prebuild/libs/*.a）
   │     --> out/images/ML307A/DSLN/ML307A_DSLN_APP.elf
   │     --arm-none-eabi-size -B--> 打印尺寸
   │     --arm-none-eabi-objcopy -O binary-->  out/images/ML307A/DSLN/ML307A_DSLN_APP.bin
   │
   └─(3) make 成功后，bat 调 tools\scripts\pack.bat：
         a) python tools\scripts\unzip.py
              prebuild\baseline\ML307A\DSLN\*.zip  -->  out\images\ML307A\DSLN\DBG\   (全部基线镜像 + download.json)
         b) 拼模板名 TEMPLATE = CRANEL_A0  (+ _GNSS 若 GNSS_OPT==YES)  (+ _04MB / _08MB 按 FLASH_OPT)
              → DSLN：GNSS_OPT=NO、FLASH_OPT=8M →  TEMPLATE = CRANEL_A0_08MB
         c) arelease -c tools\aboot -g -p ASR_CRANEL_EVB -v CRANEL_A0_08MB \
                    -i "cp=...\DBG\cp.bin,rd=...\DBG\ReliableData.bin,rfbin=...\DBG\rf.bin,user_app=...\ML307A_DSLN_APP.bin" \
                    out\images\ML307A\DSLN\ML307A_DSLN_APP.zip
         → 最终可烧录包：out\images\ML307A\DSLN\ML307A_DSLN_APP.zip
```

`pack.bat` 原文（全文）：

```bat
@goto start
:: ML307A/ML302A 固件打包脚本
:start

@set BASE_LINE_DIR=%PROJECT_ROOT%\prebuild\baseline\%MODEL_NAME%\%MODEL_TARGET%
@set BUILD_IMG_DIR=%PROJECT_ROOT%\out\images\%MODEL_NAME%\%MODEL_TARGET%
@set BASE_IMG_DIR=%BUILD_IMG_DIR%\DBG

@set RD_IMG=%BASE_IMG_DIR%\ReliableData.bin
@set RF_IMG=%BASE_IMG_DIR%\rf.bin
@set CP_IMG=%BASE_IMG_DIR%\cp.bin
@set APP_IMG=%BUILD_IMG_DIR%\%MODEL_NAME%_%MODEL_TARGET%_APP.bin
@set STD_IMG="cp=%CP_IMG%,rd=%RD_IMG%,rfbin=%RF_IMG%,user_app=%APP_IMG%"
@set PKG_FILE=%MODEL_NAME%_%MODEL_TARGET%_APP.zip
@set TEMPLATE=CRANEL_A0

::解压基础固件包
@python tools\scripts\unzip.py %BASE_LINE_DIR% %BASE_IMG_DIR%

@if "%GNSS_OPT%" == "YES" (
    set TEMPLATE=%TEMPLATE%_GNSS
)

@if "%FLASH_OPT%" == "4M" (
    set TEMPLATE=%TEMPLATE%_04MB
) else (
    set TEMPLATE=%TEMPLATE%_08MB
)

@if exist %BUILD_IMG_DIR% (
    @echo Generating %PKG_FILE%...
    arelease -c %PROJECT_ROOT%\tools\aboot -g -p ASR_CRANEL_EVB -v %TEMPLATE% -i %STD_IMG%  %BUILD_IMG_DIR%\%PKG_FILE% > nul
    @echo Done
) else (
    @echo directory %BUILD_IMG_DIR% not exist
)
```

**`-v CRANEL_A0_08MB` 是什么？** 它不是模板文件名，而是 `tools\aboot\config\product\ASR_CRANEL_EVB.json` 里定义的**变体（variants）名**：

```json
{
  "name": "ASR_CRANEL_EVB",
  "product": "arom-tiny",
  "version": "0.5",
  "version-bootrom": "2021.10.16",
  "template": "CRANEL_A0_TEMPLATE",
  "layout": "CRANEL_SINGLE_FLASH_LAYOUT_4M",
  "keyAlg": "rsa",
  "hashAlg": "sha256",
  "secureBoot": false,
  "firmwareGenerator": {
    "name": "crane",
    "call-max-download-size": "60KiB",
    "flash-max-download-size": "1MiB",
    "use-lzma-compression": true
  },
  "fota": ["system"],
  "variants": [
    { "name": "CRANEL_A0_04MB",      ... "flash": "QSPI_NOR_4MB_B64KB_S4KB_P256", "template": "CRANEL_A0_TEMPLATE",      "layout": "CRANEL_SINGLE_FLASH_LAYOUT_4M" },
    { "name": "CRANEL_A0_GNSS_04MB", ... "flash": "QSPI_NOR_4MB_B64KB_S4KB_P256", "template": "CRANEL_A0_GNSS_TEMPLATE", "layout": "CRANEL_SINGLE_FLASH_LAYOUT_4M_GNSS" },
    { "name": "CRANEL_A0_08MB",      ... "flash": "QSPI_NOR_8MB_B64KB_S4KB_P256", "template": "CRANEL_A0_TEMPLATE",      "layout": "CRANEL_SINGLE_FLASH_LAYOUT_8M" },
    { "name": "CRANEL_A0_GNSS_08MB", ... "flash": "QSPI_NOR_8MB_B64KB_S4KB_P256", "template": "CRANEL_A0_GNSS_TEMPLATE", "layout": "CRANEL_SINGLE_FLASH_LAYOUT_8M_GNSS" },
    { "name": "CRANEL_A0_FS_08MB",   ... },
    { "name": "CRANEL_A0_FS_GNSS_08MB", ... }
  ]
}
```

→ `CRANEL_A0_08MB` = **8MiB QSPI NOR Flash + `CRANEL_A0_TEMPLATE` + `CRANEL_SINGLE_FLASH_LAYOUT_8M` 布局**。
`tools\aboot\config\template\CRANEL_A0_TEMPLATE.json` 定义了烧录动作序列（`preboot`/`flasher`/`partition`/`flash ...`/`erase ...`），其中包含：

```json
    { "command": "flash", "name": "user_app", "type": "image", "partition": "user_app", "group": "6" },
```

**这就是 `arelease` 的作用：把基线包里的 `download.json` 与模板合并，并在 group 6 插入"写 user_app"这一步。**

### 3.3 本机实证：产物长什么样

`out/images/ML307A/DSLN/` 实测（时间戳 2026/10/14，说明本工作区已经成功编译过一次）：

| 文件 | 大小 |
| --- | --- |
| `ML307A_DSLN_APP.elf` | 121,856 |
| `ML307A_DSLN_APP.bin` | **29,472**（就是被写进 `user_app` 分区的东西） |
| `ML307A_DSLN_APP.map` | 713,177 |
| `ML307A_DSLN_APP.ld.obj` | 3,128 |
| **`ML307A_DSLN_APP.zip`** | **10,392,274** ← **最终烧录包** |
| `DBG/` | 基线解压出的全部镜像（`cp.bin` 5,181,440 等） |

最终包 `ML307A_DSLN_APP.zip` 的条目（实测）：

```
ReliableData.bin   apn.bin        bootloader.ubi   cp.bin         download.json
dsp.bin            firmware.bin   flasher.img      flashinfo.bin  fota.json
fwcerts.bin        logo.bin       partition.bin    preboot.img    rf.bin
system.img         updater.bin    user_app.bin     (共 18 项)
```

包内 `download.json` **末尾**（对比 3.2 节的基线包，多出了 group 6 的 `flash user_app`）：

```json
   { "command" : "flash", "group" : "2", "image" : "updater.bin", "partition" : "updater", "weight" : 131072 },
   { "command" : "erase", "group" : "3", "partition" : "fota_param", "weight" : 12288 },
   { "command" : "erase", "group" : "3", "partition" : "fota_pkg", "weight" : 4096 },
   { "command" : "flash", "group" : "6", "image" : "user_app.bin", "partition" : "user_app", "weight" : 1081344 },
   { "command" : "erase", "group" : "4", "partition" : "nvm", "weight" : 1048576 },
   { "command" : "erase", "group" : "5", "partition" : "factory", "productionOnly" : true, "weight" : 32768 }
]
```

**→ `arelease -g` 输出的 `.zip`（含 `download.json` + `user_app.bin`）就是"从编译产物到最终可烧录固件"的终点。**

### 3.4 另一条链路：FOTA/OTA 差分包（不属于"烧录"，但同目录有工具）

`tools/adiff/` 内含 ASR 官方文档 `README.docx`（标题 **"ASR1601 FOTA"**，版权 ASR Microelectronics）与 `FBFMake_CF_V1.5.exe` / `FBFMake_CF_V1.6-150.exe` / `adiff.exe`。原文摘录：

```bat
:: fbfmake_fast.bat
FBFMake_CF_V1.6-150.exe -f config -d 0x10000 -a a -b b -o fbf_dfota.bin -q

:: fbfmake_full.bat
FBFMake_CF_V1.6-150.exe -o fbf.bin -f config -a a -b a
```

文档说明：
> "运行fbfmake_fast.bat快速制作差分包，生成差分文件fbf_dfota.bin"
> "使用工具adiff.exe制作差分包：`adiff.exe system_old.img system_new.img system_patch.bin`"
> "运行后会产生两个升级包，system_patch.bin_1代表最小系统差分升级文件，system_patch.bin_2代表非最小系统升级文件"
> "生成的差分文件fbf_dfota.bin放入服务器端，使用AT命令可以下载升级测试：`AT$MYFOTA=<channel>,<mode>,<destination_ip/url>,<username>,<password>`"

**→ 这条链路产出的是 `fbf_dfota.bin` / `fbf.bin` / `system_patch.bin`（OTA 升级文件），不是 `.pac`，也不是本地烧录包。**

### 3.5 签名工具（可选，成品烧录流程未强制）

`tools/scripts/PackageSign.py` 提供 RSA-2048 + MD5 的固件签名/验签：

```python
# 签名流程：①提取bin文件md5码；②对md5码进行RSA签名（pkcs1_15填充）；③将签名结果及md5码放置于bin文件开头，增加固定长度256（RSA） + 16(MD5)
# 秘钥存储：公钥存储在模组中，私钥存储在编译环境中
# 将会在bin文件开头增加272固定长度，包括256bytes的签名信息及16bytes的md5信息
```

`build.bat`/`pack.bat` **都没有调用它**，所以量产烧录默认**不需要**签名。

---

## 4. 下载模式进入方式 + QFlash 配置 + CDC 日志串口

> 本章证据最弱，请重点看每条的等级标注。

### 4.1 【SDK原文】CDC 日志串口 = UART0，波特率 921600

`custom/applaction/main.c` 原文：

```c
int cm_opencpu_entry(char *param)
{
	uart_open(CM_UART_DEV_0, 921600, u0_callback);
	u0_printf("ML307A Start\r\n");

	/* 创建进程 */
	osThreadCreat("dbg_th", dbg_th, 4, 1024);

	return 0;
}
```

`custom/applaction/app.c` 注释：

```c
void dbg_th(void* argument)
{
    /*串口由于LOG功能,已在cm_opencpu_entry()开头初始化*/
    ...
    u0_printf("Send %s (%d)\r\n",rxBuff , readLen);
}
```

`custom/drv/uart/drv_uart.c` 原文（引脚复用 + 打印实现）：

```c
    if(dev == CM_UART_DEV_0){
        cm_iomux_set_pin_func(CM_IOMUX_PIN_17,CM_IOMUX_FUNC_FUNCTION1);
        cm_iomux_set_pin_func(CM_IOMUX_PIN_18,CM_IOMUX_FUNC_FUNCTION1);
        event.event_param = "uart0";
    }
    ...
    if(dev == CM_UART_DEV_0){
        /* 配置uart唤醒功能，使能边沿检测才具备唤醒功能，仅主串口具有唤醒功能 ... */
        cm_iomux_set_pin_cmd(CM_IOMUX_PIN_17,CM_IOMUX_PINCMD1_LPMEDEG,CM_IOMUX_PINCMD1_FUNC1_LPM_EDGE_RISE);
    }

void u0_printf(char *str, ...)
{
    ...
    cm_uart_write(CM_UART_DEV_0,txBuff0,len,1000);
}
```

**结论（SDK原文）**
- 日志/`printf` 走 **`CM_UART_DEV_0`**，即 **IOMUX `PIN_17`(TX) / `PIN_18`(RX)**，`FUNCTION1`。
- **波特率 921600**。
- 开机标志字符串：**`ML307A Start`**；后续日志形如 `[00001] I/app:` 并可带 ANSI 颜色（`drv_uart.h` 里 `#define DEBUG` 与 `#define COLR_LOG_MODE` 默认开启）。
- ⚠️ 注意 `include/cmiot/cm_uart.h` 的波特率宏里**没有 921600**（只列到 256000），文档还说"低功耗串口…115200以内的标准波特率；普通串口可配置为256000非标波特率"。
  但 `cm_uart_cfg_t.baudrate` 是 `int`，示例代码直接传 921600。**921600 能否稳定工作需实测确认**（若日志乱码，优先怀疑这个）。

**串口号怎么找（【社区经验】，需实测）**
- 驱动：官方提供 `ML307A-Windows驱动相关_V1.0.0.zip`（见 [Gitee YZRDEG/ML307A](https://gitee.com/YZRDEG/ML307A) 根目录）。
- 插上 USB 后设备管理器应出现多个串口/RNDIS 口；社区教程（杨奉武）建议"如果没有出现端口号，把 Type-C 口旋转 180 度"（该板 Type-C 正反接分别连到 4G 模组与另一颗芯片——**这条针对的是他那块教学板，未必适用 DEMO-ML307X-V1.04**）。
- **本板（DEMO-ML307X-V1.04，USB 为 RNDIS + CDC 调试串口）具体是哪个 COM 号、CDC 是否就是 UART0，需实测确认。**

### 4.2 【工具实证】进入下载模式的途径（三选一，均需实测）

QFlash V8.1 内置的 ASR 下载器 `ASR\1\adownload1.exe` 反查到的 ASCII 字符串（该 exe 自述为 **"Asrmicro aboot download console application"**）：

```
Asrmicro aboot download console application.
Download release package FILE to boards.
  -p, --port=port
Use named serial ports separate with comma
Or auto enable arom usb ports device
  -u, --usb-only
Use arom usb ports only
  -r, --reboot      Reboot device after finished
  -a                enable sending AT cmd to fallback to download mode.
  %s -p COM1,COM2 -s 115200 aboot.zip
  %s -u -a -s 115200 aboot.zip
enabling device <%s> into downloading mode...
sending "AT$MYDOWNLOAD=1" command to serial device <%s>...
sending "AT+QDOWNLOAD=1" command to serial device <%s>...
Arom Usb Boot Port
arom|arom-tiny
AROM-TINY-BOOT2
download.json
release package '%s' has no download.json
```

**由此可确定的三种进入下载模式的方式：**
1. **USB AROM boot 模式**：模块进入 boot 后，PC 上出现名为 **`Arom Usb Boot Port`** 的设备，工具可自动识别（`-u -a`，`-a` = auto enable arom usb ports）。
2. **AT 指令强制回落下载模式**：`-a` 参数会向串口发 **`AT$MYDOWNLOAD=1`**（中移/ASR 风格）或 **`AT+QDOWNLOAD=1`**（Quectel 风格），默认串口速率 `115200`。
   ⭐ **本机 SDK 实证：`AT$MYDOWNLOAD` 确实存在于 ML307A-DSLN 的 `cp.bin` 里**——在预编译内核的 AT 命令表中检索到：
   ```
   $MYDOWNLOAD: (0,1)
   ```
   （同一表中还有 `$MYFOTA=<channel>,<mode>,...`、`$MYMINISYS=<enable>`、`$MYSYSINFO` 等，与 `tools/adiff/README.docx` 描述的中移 AT 命令族一致。）
   **→ `AT$MYDOWNLOAD=1` 是 ML307A 模组真实支持的 AT 命令（取值范围 0/1）**，可作为"不短接触点"的首选尝试路径；但"发这条命令后需要多久、用哪个串口、是否必须 USB 已连接"**需实测**。
3. **硬件 BOOT + 复位时序**（有按键的板子）：【社区经验】杨奉武教程原文——
   > "先一直按着BOOT按键，然后按一下RST按键，然后松开RST按键，最后松开BOOT，然后等15S左右设备管理器里面应该可以显示端口号了（此时模组进入了烧录程序模式）"

   他后来那块板做了"一键BOOT"："只需要按一下boot按键就可以；去掉了原先繁琐的先按下boot 再复位才能进入boot下载模式的操作"。

### 4.3 【未证实】DEMO-ML307X-V1.04 无 BOOT/RST 时"短接哪两个触点"

**本轮的明确结论：没有任何官方文档或可靠来源指出 DEMO-ML307X-V1.04 上用于进入下载模式的测试点编号、丝印或短接时序。**

- 本环境**无法读取 PDF**（`web_fetch` 直接拒绝 `application/pdf`；shell 无出网），因此以下本应能回答该问题的官方手册**未能读取正文**：
  - `ML307A_OPENCPU手册/ML307A_SDK开发指导手册.pdf`（README 指定的"下载调试"出处）
  - `ML307A_资料/ML307系列_EVB用户手册_V001适用_V1.1.0.pdf`
  - `ML307A_资料/ML307系列_EVB用户手册_V002适用_V1.0.0.pdf`
  - `ML307A_OPENCPU手册/ML307A_硬件设计手册_OpenCPU版本适用.pdf`
  - `ML307A_资料/ML307A_维护诊断工具用户手册_V1.0.0.pdf`
  - `ML307A_资料/AT_Commands_Reference_Guide_4G_Series_V1.2.0.pdf`（本可确认 `AT$MYDOWNLOAD` 的正式语法）
- 检索到的唯一相关社区描述（CSDN 陈大鱼那篇）在该位置只有一句图片说明：**"拉低RES的引脚电平，进入到下载模式"**——没有给出触点编号、也没有说明"RES"是哪个网络（复位脚？BOOT 脚？）。**不足以照做，不作为结论。**
- **因此本项一律标注"未证实 / 需实测确认"。请勿照抄任何网上流传的触点编号。**

**建议的实测排查顺序（不含未证实结论，仅为方法论）**
1. 先在现有固件上试 **`AT$MYDOWNLOAD=1`**（证据最强的软路径，4.2 节）。
2. 若必须硬件进入：用万用表/原理图确认底板是否有 **BOOT/下载模式选择网络**或 **TEST POINT（TP）**，优先找丝印为 `BOOT`/`DOWNLOAD`/`FORCE_DL`/`TP*` 的焊盘，以及 **ASR1606 的 BOOT strap 引脚**（**具体引脚名与电平要求需查硬件设计手册/datasheet，本文不臆造**）。
3. 参照通用时序：**先使其处于下载模式电平（等效"按住 BOOT"）→ 再复位一次（等效"点 RST"）→ 保持→ 释放**，然后观察设备管理器是否出现 `Arom Usb Boot Port` 之类设备。

### 4.4 QFlash 里该选什么？（【工具实证】+【未证实】）

**先纠正一个关键误解：QFlash 是 Quectel（移远）的工具，不是中移的工具。**

本机 `<DOWNLOADS>\QFlash_V8.1_CN.zip` (400,100,321 字节) 内含：
```
QFlash_V8.1_CN/QFlash_V8.1.zip
QFlash_V8.1_CN/Quectel_QFlash_用户指南_V4.5.pdf     ← 文件名即 "Quectel"
```

解开 `QFlash_V8.1.zip`（2,127 个条目）后：

**(a) 它的主体是展锐 UNISOC 下载工具的换皮**
- `Setting\UpgradeDownload.ini` 原文含：`UserGuide = UNISOC Download User Guide V4.1.pdf`、`ShowPacPath`、`KeepPacNVState`、`ShowConfirmDialog=1`（注释：*"Show prompted whether load latest pac file"*）、`PacketMode=1`。
- `App\BMFileType.ini` 是展锐 FDL/FDL1/FDL2/MERKLE/UIS8852 那一套下载动作表。
- `bootloader.ini` 里全是 `[XY1100]`/`[XY4100]`/`[UIS8852]`/`[4101]` 等**展锐/芯翼平台**。
- **→ QFlash 主界面加载的 `.pac` 是"展锐 pac"，与 ASR1606 无关。**

**(b) 但它确实内置了 ASR 平台分支**
```
QFlash_V8.1/ASR/1/adownload1.exe        (1,225,728 字节)
QFlash_V8.1/aboot.dll
MainConfig.ini:  ASR_Reset_after_DL=1
```
- `aboot.dll` 字符串含：`download.json`、`release package '%s' has no download.json`、`arom|arom-tiny`、`AROM-TINY-BOOT2`、`crane`、`!CRANE!`、`config/template/`、`dbconfig/product/`。
- 这些与 ML307A SDK 的 `ASR_CRANEL_EVB.json`（`product: arom-tiny`、`firmwareGenerator.name: crane`）和 `tools/aboot/config/{product,template,flash,partition}` **完全同源**。
- `adownload1.exe` 的用法字符串明确接受 **`aboot.zip`** 形式的 release package——**正是 `ML307A_DSLN_APP.zip` 这种文件**。

**(c) 但对 `ML307` 零命中**
- 对 QFlash V8.1 全部 1,699 个文件（<80MB）做 **ASCII 与 UTF-16LE 双编码**扫描，
  检索 `ML307`、`CRANEL`、`ASR1606`、`ASR1603`、`ML305`、`翱捷`、`中移` → **全部 0 命中**
  （`OneMO` 的若干"命中"经查为 Qt/ICU 库中的子串巧合，不是有效证据）。

**→ 回答"QFlash 里选什么型号"：**
- **无法给出经证实的型号选项。** QFlash 的模型/产品列表中没有 ML307A/CRANEL/ASR1606 的字样。
- **不能确认** QFlash 能直接烧 ML307A-DSLN。**属"有可能但未证实"**：其 ASR 分支（`ASR/1/adownload1.exe` + `aboot.dll`）与 ML307A 的 aboot 引擎同源、且能解析同构的 `download.json` release package，但 QFlash 自带的 `dbconfig/product` 里是否有 `arom-tiny` 对应的产品/变体配置**未能验证**（相关配置未以明文出现在解压目录中）。
- **建议**：
  1. **首选官方路线**——用中移/ASR 提供的下载工具（`arelease` 同一套 aboot 体系的 PC 端下载器；官方包在 [Gitee YZRDEG/ML307A](https://gitee.com/YZRDEG/ML307A) 根目录的 `aboot-tools-2022.03.01-win-x64.rar`，另有 `win10-CMUART.rar`、`中移物联网智能模组串口助手工具.rar`）。**未证实**该 rar 内是否即为可直接烧 `ML307A_DSLN_APP.zip` 的 GUI 工具。
  2. 若要试 QFlash：在 QFlash 中找 **ASR 平台入口**（而非展锐/MTK 入口），把 **`ML307A_DSLN_APP.zip`**（不是 `.pac`！）作为 "release package" 加载，**需实测确认**是否被接受、以及是否需要先让模组进入 `Arom Usb Boot Port` 状态。
  3. **不要**在 QFlash 的展锐 pac 页面里选"芯片型号"来烧 ML307A——那是另一个体系。

**关于"下载模式"选项（【工具实证】，仅供参考）**
- `MainConfig.ini`：`DL_Method=1`（注释 `#UART DL:0, USB DL:1`）→ 默认走 **USB 下载**；`ASR_Reset_after_DL=1` → 下载后复位。
- `bootloader.ini` `[Common]`：`Baudrate_DL=921600`、`Baudrate_Log=921600`、`Baudrate_AT=9600`
  —— 这是展锐分支的公共段，但 **921600 与 SDK 的 UART0 日志波特率一致**，可作为旁证。
- `adownload1.exe` 默认示例串口速率 `115200`（AT 回落用）。

### 4.5 本节小结表

| 问题 | 结论 | 等级 |
| --- | --- | --- |
| 日志/CDC 波特率 | **921600** | 【SDK原文】`main.c` |
| 日志走哪个 UART | `CM_UART_DEV_0` = IOMUX `PIN_17`/`PIN_18` FUNCTION1 | 【SDK原文】`drv_uart.c` |
| 开机标志串 | `ML307A Start` | 【SDK原文】`main.c` |
| CDC 对应哪个 COM 号 | 需实测 | 【未证实】 |
| 是否有免短接的软入口 | **有：`AT$MYDOWNLOAD=1`**（模组 AT 表实测支持 `$MYDOWNLOAD: (0,1)`） | 【工具实证】+ 内核镜像实证 |
| 下载模式 USB 设备名 | `Arom Usb Boot Port` | 【工具实证】`adownload1.exe`/`aboot.dll` |
| 无按键时短接哪两个触点 | **不得而知，禁止臆造** | 【未证实】 |
| 进入下载模式时序 | 有按键板的社区时序已知（BOOT按住→点RST→松RST→松BOOT）；无按键板对应做法未证实 | 【社区经验】/【未证实】 |
| QFlash 选哪个芯片型号 | **无法证实**；QFlash 无 ML307/CRANEL/ASR1606 字样；其 ASR 分支与 ML307 的 aboot 同源但未证实可用于本模组 | 【未证实】 |

---

## 5. 组件裁剪开关

### 5.1 核心认知：**GNSS/Audio/LCD/Camera/MQTTS/LWM2M 这些 API 在预编译内核里，不在用户工程里，无法用宏"编掉"**

证据：
- 这些能力全都在 `include/cmiot/*.h`（`cm_gnss.h`、`cm_audio_*.h`、`cm_lcd.h`、`cm_camera.h`、`cm_mqtt.h`、`cm_tts.h`、`cm_ftp.h`…），而 `include/` 只是头文件；
- 对应 `examples/<组件>/<组件>.mk` **只加自己的 demo 源文件和 include 路径，不链接任何库、不定义任何开关宏**。例如：

```make
# examples/gnss/gnss.mk
EXAMPLE_GNSS_DIR := examples/gnss
OC_FILES +=  $(EXAMPLE_GNSS_DIR)/src/cm_demo_gnss.c
INC      += -I'$(EXAMPLE_GNSS_DIR)/inc'
```
```make
# examples/audio/audio.mk
OC_FILES +=  $(EXAMPLE_ADUIO_DIR)/src/cm_demo_audio.c
INC      += -I'$(EXAMPLE_ADUIO_DIR)/inc'
```
```make
# examples/lcd/lcd.mk / camera/cam.mk / mqtt/mqtt.mk / tts/tts.mk / keypad/keypad.mk / wifiscan/wifiscan.mk / sd/sd.mk / fota/fota.mk
```
（以上均为逐字原文；`examples/lcd/lcd.mk`、`examples/camera/cam.mk`、`examples/mqtt/mqtt.mk`、`examples/tts/tts.mk` 结构完全相同）

- 全仓库 `.mk` 中出现的"开关宏"**只有 3 个**（实测 grep `SUPPORT|ENABLE_|DISABLE_|FEATURE_|lwm2m|mqtts`）：

| 文件 | 原文行 |
| --- | --- |
| `tools/scripts/config.mk` | `DEMO_SUPPORT:=n` |
| `examples/examples.mk` | `ifeq ($(strip $(DEMO_SUPPORT)),y)` |
| `custom/custom.mk` | `ifeq ($(strip $(DEMO_SUPPORT)),n)` |
| `examples/lbs/lbs.mk` | `CDEFINES   += -DCM_LBS_SUPPORT` |
| `examples/pm/pm.mk` | `CDEFINES   += -DCM_SLEEP_SUPPORT` |

- **`LWM2M`、`MQTTS`、`FTP` 的开关宏/库名在整个 SDK 中不存在**：
  - `LWM2M`：SDK 内**完全未出现**（无头文件、无示例、无库）→ 该组件**不在本 SDK 中**。
  - `MQTTS`：**未在 SDK 中出现**（只有 `cm_mqtt.h` 与 `examples/mqtt`；TLS 能力由 `cm_ssl.h` 提供）。
  - `FTP`：只有 **`src/cm_lib/cm_ftp/`**（中移封装，链接 `prebuild/libs/cm_ftp.a`），**没有单独宏开关**，靠 mk 的 include 控制。
  - `GPS/GNSS`：只有头文件与示例；**代码级无开关宏**，只有打包阶段 `GNSS_OPT`。

**→ 所以"省 Flash"的现实手段是下面 5 条（都是"改 mk 包含关系"或"改打包参数"，不是改宏）。**

### 5.2 手段一：`DEMO_SUPPORT` —— 最大的一刀

`examples/examples.mk` 原文（`DEMO_SUPPORT=y` 时**全部 28 个 demo 一起编进去**）：

```make
EXAMPLE_DIR :=examples

ifeq ($(strip $(DEMO_SUPPORT)),y)
include $(EXAMPLE_DIR)/demo_main/demo_main.mk
include $(EXAMPLE_DIR)/uart/uart.mk
include $(EXAMPLE_DIR)/asocket/asocket.mk
include $(EXAMPLE_DIR)/fs/fs.mk
include $(EXAMPLE_DIR)/gpio/gpio.mk
include $(EXAMPLE_DIR)/pm/pm.mk
include $(EXAMPLE_DIR)/lbs/lbs.mk
include $(EXAMPLE_DIR)/http/http.mk
include $(EXAMPLE_DIR)/virt_at/virt_at.mk
include $(EXAMPLE_DIR)/ntp/ntp.mk
include $(EXAMPLE_DIR)/i2c/i2c.mk
include $(EXAMPLE_DIR)/adc/adc.mk
include $(EXAMPLE_DIR)/spi/spi.mk
include $(EXAMPLE_DIR)/lcd/lcd.mk
include $(EXAMPLE_DIR)/keypad/keypad.mk
include $(EXAMPLE_DIR)/pwm/pwm.mk
include $(EXAMPLE_DIR)/mqtt/mqtt.mk
include $(EXAMPLE_DIR)/audio/audio.mk
include $(EXAMPLE_DIR)/fota/fota.mk
include $(EXAMPLE_DIR)/tts/tts.mk
include $(EXAMPLE_DIR)/sd/sd.mk
include $(EXAMPLE_DIR)/cJSON/cJSON.mk
include $(EXAMPLE_DIR)/gnss/gnss.mk
include $(EXAMPLE_DIR)/aliyun/aliyun.mk
include $(EXAMPLE_DIR)/wifiscan/wifiscan.mk
include $(EXAMPLE_DIR)/camera/cam.mk
include $(EXAMPLE_DIR)/ftp/ftp.mk
endif
```

**做法**：生产固件**不要用 `demo` 参数**（即保持 `DEMO_SUPPORT=n`，走 `custom/custom.mk`），或者在 `examples.mk` 里注释掉不需要的 `include` 行。
**注意**：这是"编译进去的 demo 代码量"，`-Wl,--gc-sections` 会回收未引用代码，所以省的量要看实际引用情况——**省的是 APP 的 1MB Flash，不是内核的 `cp.bin`**。

### 5.3 手段二：`src/cm_lib/cm_lib.mk` —— 剔除中移封装组件（收益最大）

原文（全文）：

```make
CM_LIB_DIR :=src/cm_lib

include $(CM_LIB_DIR)/cm_async_dns_adapt/async_dns_adapt.mk
include $(CM_LIB_DIR)/modem/modem.mk
include $(CM_LIB_DIR)/cm_tts_play/cm_tts_play.mk
include $(CM_LIB_DIR)/cm_ftp/cm_ftp.mk
```

被包含者实际链接的**静态库**（原文）：

```make
# src/cm_lib/cm_tts_play/cm_tts_play.mk
CM_TTS_DIR :=src/cm_lib/cm_tts_play
OC_FILES +=  $(CM_TTS_DIR)/cm_tts_play.c
OC_LIBS += prebuild/libs/cm_local_tts.a
OC_LIBS += prebuild/libs/lib_tts.a

# src/cm_lib/cm_ftp/cm_ftp.mk
ETHIRDPARTY_CJSON_DIR := src/cm_lib/cm_ftp
OC_LIBS += prebuild/libs/cm_ftp.a

# src/cm_lib/modem/modem.mk
OC_FILES +=  $(CM_CYCLEQUEUE_DIR)/cm_modem.c
INC      += -I'$(CM_CYCLEQUEUE_DIR)'

# src/cm_lib/cm_async_dns_adapt/async_dns_adapt.mk
OC_FILES +=  $(CM_ASYNC_DNS_ADAPT_DIR)/async_dns_adapt.c
INC      += -I'$(CM_ASYNC_DNS_ADAPT_DIR)'
```

**对应库体积（实测）**：

| 库 | 大小 | 关掉它的做法 |
| --- | --- | --- |
| `prebuild/libs/cm_local_tts.a` | 495,874 | 注释 `include $(CM_LIB_DIR)/cm_tts_play/cm_tts_play.mk` |
| `prebuild/libs/lib_tts.a` | 897,938 | 同上 |
| `prebuild/libs/cm_ftp.a` | 299,024 | 注释 `include $(CM_LIB_DIR)/cm_ftp/cm_ftp.mk` |

**→ 关掉 TTS（`cm_tts_play`）一项，理论上可省下约 1.36MB 的候选库（实际进入镜像的量取决于被引用符号，但这是 SDK 里最"重"的可选组件）。关掉 FTP 省约 300KB 候选。**
**注意**：`cm_tts_play.c` 里的 `lib_tts.a` 部分 `.text` 被**链接脚本强制放进 RAM 执行段**：
```ld
    .sramtext ALIGNMENT : {
        ...
        *lib_tts.a:(SECTION_TEXT)
        ...
    } AT>flash
```
→ 也就是说 **TTS 会占用 512K RAM 的一部分**。裁掉 TTS 对 RAM 也友好。

### 5.4 手段三：`third-party/third-party.mk` —— 剔除第三方库

原文（全文）：

```make
THIRD-PARTY_DIR := third-party

include $(THIRD-PARTY_DIR)/cJSONFiles/cJSON.mk
include $(THIRD-PARTY_DIR)/aliyun/aliyun.mk
#include $(THIRD-PARTY_DIR)/zbar/zbar.mk
```

- **`zbar` 官方自己已经注释掉了**（原文第 4 行就是以 `#` 开头的）→ 二维码扫描默认不参与编译。
- **`aliyun`（阿里云 LinkSDK）是最"胖"的第三方组件**：`third-party/aliyun/aliyun.mk` 原文共链接 **40 多个 .c 文件**，其中包含**一整套 mbedTLS**（`aes.c`、`bignum.c`、`rsa.c`、`ssl_tls.c`、`x509_crt.c` … 约 30 个文件）+ OTA 组件。
  不用阿里云 → 注释 `include $(THIRD-PARTY_DIR)/aliyun/aliyun.mk`。
- **`cJSON`** 若不用 JSON 也可注释。

### 5.5 手段四：`custom/custom.mk` / `custom/drv/drv.mk` —— 精简自己的驱动

```make
# custom/custom.mk
include tools/scripts/config.mk
CUSTOM_DIR := custom

#application
APPLACTION_DIR := custom/applaction
OC_FILES +=  $(APPLACTION_DIR)/app.c
OC_FILES +=  $(APPLACTION_DIR)/main.c
INC      += -I'$(APPLACTION_DIR)'

ifeq ($(strip $(DEMO_SUPPORT)),n)
include $(CUSTOM_DIR)/drv/drv.mk
endif
```

```make
# custom/drv/drv.mk
DRV_UART_DIR := custom/drv

#uart
OC_FILES +=  $(DRV_UART_DIR)/uart/drv_virt_at.c
OC_FILES +=  $(DRV_UART_DIR)/uart/drv_uart.c
INC      += -I'$(DRV_UART_DIR)/uart'

#lw_oopc
INC      += -I'$(DRV_UART_DIR)/lw_oopc'
```

- `app.c` / `main.c` 是**必备入口**（`main.c` 提供 `cm_opencpu_entry`，链接脚本 `ENTRY(cm_opencpu_entry)`，且 `.text` 段里 `*(.text.cm_opencpu_entry)` 被强制保留）。**不能删**。
- `drv_virt_at.c`（虚拟 AT 测试）、`drv_uart.c` 可按需精简。

### 5.6 手段五：`GNSS_OPT` / `FLASH_OPT` —— 打包期的分区布局开关（不是宏）

- 两者由 `ML307A_build.bat` 按子型号设置，被 `pack.bat` 读取，用于选择 **arelease 变体**。
- DSLN：`FLASH_OPT=8M`、`GNSS_OPT=NO` → `CRANEL_A0_08MB` → `CRANEL_SINGLE_FLASH_LAYOUT_8M`。
- 若置 `GNSS_OPT=YES` → 变体 `CRANEL_A0_GNSS_08MB` → 布局 `CRANEL_SINGLE_FLASH_LAYOUT_8M_GNSS`，与 8M 布局的**差异（逐字对比）**：
  - `system` 分区下**新增** `jacana_fw`(size 0) 与 `jacana_pvt`(128KiB)（GNSS 固件/标定区）；
  - **`nvm` 从 1024KiB 缩小为 512KiB**；
  - `user_app` **两者都是 1024KiB**（不变）。
- **→ `GNSS_OPT` 改变的是 Flash 分区划分，不是"把 GNSS 代码编掉"。GNSS 能力本身在 `cp.bin` 内核里。**
- ⚠️ **DSLN 官方设定 `GNSS_OPT=NO`**。请勿在 DSLN 上随意用 GSLN 的 `GNSS_OPT=YES`。

### 5.7 裁剪开关速查表

| 想省掉 | 改哪里 | 改法 | 预计收益（候选库/源码） |
| --- | --- | --- | --- |
| 全部官方 demo | 不加 `demo` 参数 | `DEMO_SUPPORT=n` | 28 个 demo 源文件 |
| TTS（本地 TTS） | `src/cm_lib/cm_lib.mk` | 注释 `include .../cm_tts_play/cm_tts_play.mk` | ≈1.36MB 库（且释放 RAM） |
| FTP | `src/cm_lib/cm_lib.mk` | 注释 `include .../cm_ftp/cm_ftp.mk` | ≈299KB 库 |
| 阿里云 IoT（含整套 mbedTLS） | `third-party/third-party.mk` | 注释 `include .../aliyun/aliyun.mk` | 40+ 源文件（最大项） |
| cJSON | `third-party/third-party.mk` | 注释 `include .../cJSONFiles/cJSON.mk` | cJSON.c |
| zbar（扫码） | 已默认注释 | — | — |
| GNSS | **无法用宏裁掉**；仅能改打包布局 `GNSS_OPT` | `ML307A_build.bat` 内变量 | 分区布局变化 |
| Audio / LCD / Camera / MQTT | **在 SDK 中找不到任何开关宏** | — | — |
| MQTTS（MQTT over TLS） | **SDK 中不存在此组件名** | — | — |
| LWM2M | **SDK 中完全没有该组件** | — | — |

---

## 6. 待实测确认清单

按优先级排序。**前 3 条决定项目能否推进。**

| # | 待确认项 | 为什么重要 | 建议的确认方法 |
| --- | --- | --- | --- |
| 1 | **DEMO-ML307X-V1.04 无 BOOT/RST 时如何进入下载模式**：短接哪两个触点？时序？ | 决定"能不能烧进去" | ① 先试软路径 `AT$MYDOWNLOAD=1`；② 查硬件设计手册/原理图找 BOOT strap 与 TP 丝印；③ **禁止臆造触点编号** |
| 2 | **`AT$MYDOWNLOAD=1` 在 ML307A-DSLN 上的实际行为**：需哪个串口/波特率、USB 是否必须先插、生效后 PC 端出现什么设备、耗时多久 | 若可用，则完全绕开触点问题 | 用 USB-TTL 或 CDC 口发 AT，观察设备管理器；对照《AT_Commands_Reference_Guide_4G_Series_V1.2.0.pdf》（本轮未读到正文）确认正式语法 |
| 3 | **QFlash V8.1 是否可用于 ML307A-DSLN**：ASR 分支能否加载 `ML307A_DSLN_APP.zip`；若不能，官方 PC 端下载工具的确切名称与出处 | 决定烧录工具选型 | 试加载 `sdk\...\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip`；若失败，取 [Gitee YZRDEG/ML307A](https://gitee.com/YZRDEG/ML307A) 的 `aboot-tools-2022.03.01-win-x64.rar` 解压查看其 GUI 工具名与说明 |
| 4 | **CDC 调试串口的具体 COM 号与是否即 UART0**；921600 是否稳定（`cm_uart.h` 枚举里没有 921600） | 决定能否看到日志 | 插 USB 看设备管理器，用串口助手 921600 试收 `ML307A Start`；若乱码改试 115200/256000 |
| 5 | **"3135KB RAM"的出处**；APP 只有 512K RAM（链接脚本）是否够用 | 内存规划 | 查 ASR1606 datasheet / `ML307A_OpenCPU资源综述.pdf` |
| 6 | **DSLN 到底是 8MiB Flash 还是 1MB** | 影响固件规划 | 链接脚本 + 分区表已表明：**总片 8MiB、APP 分区 1MiB**。若与实物不符需供应商澄清 |
| 7 | **RNDIS + CDC 同时可用时，下载模式与 RNDIS 是否冲突** | 影响烧录与调试能否并行 | 实测 |
| 8 | **`ML307A_DSLN_APP.zip` 是否需要额外签名/证书才能被模组接受** | `secureBoot: false`，理论上不需要 | 查 `ML307A_固件升级开发指导手册.pdf`（未读到）或直接实测 |
| 9 | **烧录后是否需手动复位** | QFlash 的 `ASR_Reset_after_DL=1` 表明会自动复位；杨奉武教程则说要点 Back 再按 RST | 实测 |
| 10 | **`ML307A_build.bat` 的 `SW_REVISION` 在非中文/非 zh-CN 系统区域设置下是否生成非法版本号** | 可能编译正常但版本串异常 | 在目标机器上跑一次看输出 |

---

## 7. 来源 URL（逐条列出）

### 7.1 SDK 仓库（GitHub 主源）

- 仓库首页：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release>
- 根目录列目录 API：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/>
- `ML307A_build.bat`：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/ML307A_build.bat>
- `ML305A_build.bat`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/ML305A_build.bat?ref=master>
- `README.md`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/README.md>
- `tools/scripts/` 目录（`Makefile`/`pack.bat`/`config.mk`/`unzip.py`/`PackageSign.py`/`do_check.bat`/`clean.bat`/密钥）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/scripts>
- `tools/aboot/` 目录（`arelease.exe`/`config`/`images`）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/aboot>
- `tools/aboot/config/`（`flash`/`partition`/`product`/`security`/`template`）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/aboot/config>
- `tools/aboot/config/template/`（`CRANEL_A0_TEMPLATE.json` 等）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/aboot/config/template>
- `tools/toolchain/`（`gcc-arm-none-eabi`）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/toolchain>
- `tools/utils/`（`make.exe`/`python3`/DLL）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/utils>
- `custom/`、`custom/custom.mk`、`custom/applaction`、`custom/drv`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/custom>
- `src/cm_lib` 及子 mk：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/src/cm_lib>
- `third-party/third-party.mk`、`examples/examples.mk`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/third-party> / <https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/examples>
- `prebuild/` / `prebuild/libs` / `prebuild/ld` / `prebuild/objs`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild>
- `prebuild/baseline/ML307A`：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild/baseline/ML307A>
- **`prebuild/baseline/ML307A/DSLN`（本次核心证据）**：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/prebuild/baseline/ML307A/DSLN>
- `docs/`（仅有 `ML305A_ML307A_OpenCPU_API手册.chm`）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/docs>
- `include/cmiot`（API 头文件全表）：<https://api.github.com/repos/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/include/cmiot>

### 7.2 Gitee 镜像（本环境唯一可稳定取到纯文本的通道）

> 说明：本环境 `raw.githubusercontent.com` 与 `cdn.jsdelivr.net` 取文件失败（SSL / 403）。
> 可用通道是 **Gitee raw 主机 `raw.giteeusercontent.com`**，需把
> `https://gitee.com/<u>/<r>/raw/<b>/<p>` 改写成 `https://raw.giteeusercontent.com/<u>/<r>/raw/<b>/<p>`。
> 已交叉验证：Gitee 的 `README.md` 内容与 GitHub contents API 返回的 base64 解码结果**逐字一致**。

- Gitee 仓库首页：<https://gitee.com/opensrc_per/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release>
- 本文实际取文件的 URL 形式（示例）：
  - `https://raw.giteeusercontent.com/opensrc_per/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/raw/master/ML307A_build.bat`
  - `.../raw/master/tools/scripts/pack.bat`
  - `.../raw/master/tools/scripts/Makefile`
  - `.../raw/master/tools/scripts/config.mk`
  - `.../raw/master/tools/scripts/unzip.py`
  - `.../raw/master/tools/scripts/PackageSign.py`
  - `.../raw/master/custom/custom.mk`、`custom/applaction/main.c`、`custom/applaction/app.c`、`custom/applaction/app.h`
  - `.../raw/master/custom/drv/drv.mk`
  - `.../raw/master/src/cm_lib/cm_lib.mk`、`cm_tts_play/cm_tts_play.mk`、`cm_ftp/cm_ftp.mk`、`modem/modem.mk`、`cm_async_dns_adapt/async_dns_adapt.mk`
  - `.../raw/master/third-party/third-party.mk`、`cJSONFiles/cJSON.mk`、`aliyun/aliyun.mk`
  - `.../raw/master/examples/examples.mk`、`gnss/gnss.mk`、`audio/audio.mk`、`lcd/lcd.mk`、`camera/cam.mk`、`tts/tts.mk`、`ftp/ftp.mk`、`mqtt/mqtt.mk`
  - `.../raw/master/tools/aboot/config/template/CRANEL_A0_TEMPLATE.json`
- Gitee contents API（本例用于列目录）：<https://gitee.com/api/v5/repos/opensrc_per/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/contents/tools/scripts>

### 7.3 中移官方手册仓库（**本轮未能读取 PDF 正文，仅确认文件存在**）

- 仓库：<https://gitee.com/YZRDEG/ML307A>
- 根目录列目录：<https://gitee.com/api/v5/repos/YZRDEG/ML307A/contents/>
- `ML307A_OPENCPU手册/` 目录（含所需官方 PDF）：<https://gitee.com/api/v5/repos/YZRDEG/ML307A/contents/ML307A_OPENCPU%E6%89%8B%E5%86%8C>
  - `ML307A_SDK开发指导手册.pdf` ← **README 指定的"编译环境、SDK编译说明、下载调试"权威出处**
  - `ML307A_固件升级开发指导手册.pdf`
  - `ML307A_硬件设计手册_OpenCPU版本适用.pdf`
  - `ML307A_OpenCPU资源综述.pdf`
  - `ML307A_外围接口开发指导手册.pdf`、`ML307A_音频开发指导手册.pdf`、`ML307A_文件系统开发指导手册.pdf`、`ML307A_电源管理开发指导手册.pdf`、`操作系统开发指导手册.pdf`、`Modem开发指导手册.pdf`、`HTTP_HTTPS开发指导手册.pdf`
  - 原始 PDF 直链（示例）：`https://raw.giteeusercontent.com/YZRDEG/ML307A/raw/master/ML307A_OPENCPU%E6%89%8B%E5%86%8C/ML307A_SDK%E5%BC%80%E5%8F%91%E6%8C%87%E5%AF%BC%E6%89%8B%E5%86%8C.pdf`
    → **本环境 `web_fetch` 对该 URL 返回 `unsupported content type "application/octet-stream"`，shell 亦无出网，故未能读取。**
- `ML307A_资料/` 目录：<https://gitee.com/api/v5/repos/YZRDEG/ML307A/contents/ML307A_%E8%B5%84%E6%96%99>
  - `ML307系列_EVB用户手册_V001适用_V1.1.0.pdf`、`ML307系列_EVB用户手册_V002适用_V1.0.0.pdf`
  - `ML307A_硬件设计手册_AT版本适用_V1.0.2.pdf`、`ML307A_参考设计_V001_20220707.pdf`、`通信模组PCB设计指导_V1.0.0.pdf`
  - `AT_Commands_Reference_Guide_4G_Series_V1.2.0.pdf`
  - `ML307A_维护诊断工具用户手册_V1.0.0.pdf`、`固件升级用户手册_V4.0.0.pdf`
- 根目录提供的工具/驱动压缩包（**未下载验证内容**）：
  - `aboot-tools-2022.03.01-win-x64.rar`
  - `ML307A-Windows驱动相关_V1.0.0.zip`
  - `win10-CMUART.rar`、`中移物联网智能模组串口助手工具.rar`、`CATStudio_V3_0_7_82.zip`
  - `ML307A_OpenCPU_Standard_1.2.0.2023022209_release.7z`（旧版 SDK）
- 中移 OneMO 下载中心（页面为动态加载，本轮仅取到标题"下载中心"）：<https://os.iot.10086.cn/download/case>

### 7.4 社区经验（**非官方，仅供参考**）

- CSDN 陈大鱼《[OpenCPU开发]（二）中移ML307A模组二次开发编译和烧录》——编译命令顺序（clean→DSLN）、`out/images/` 产物位置、"拉低RES的引脚电平，进入到下载模式"：
  <https://blog.csdn.net/weixin_44845407/article/details/145448327>
- 博客园 杨奉武《中移ML307A(4G Cat1,C-SDK,OpenCPU)模组学习开发-程序固件烧录说明(烧录AT指令固件)》——驱动安装、BOOT+RST 时序、下载完成后需先 Back 再按 RST：
  <https://www.cnblogs.com/yangfengwu/p/18201691>
- 腾讯云开发者社区 杨奉武《中移ML307A(C-SDK,OpenCPU)学习开发-程序固件烧录说明》（同上内容）：
  <https://cloud.tencent.cn/developer/article/2418978>
- 语雀《ML307 OpenCPU教程》（本轮仅取到标题，正文为动态渲染）：
  <https://www.yuque.com/doiot/data/pvr2ugczronsric1>

### 7.5 本机第一手证据（非 URL，但可复现）

| 路径 | 用途 |
| --- | --- |
| `sdk\ML307A_OpenCPU_Standard_1.4.2\` | 完整 SDK（bat/mk/ld/objs/libs/aboot/adiff 全部可读） |
| `sdk\ML307A_OpenCPU_Standard_1.4.2\out\images\ML307A\DSLN\ML307A_DSLN_APP.zip` | **已成功生成的可烧录包（10,392,274 字节）** |
| `sdk\...\out\images\ML307A\DSLN\{ML307A_DSLN_APP.elf,.bin,.map,.ld.obj}` | 编译产物 |
| `sdk\...\out\images\ML307A\DSLN\DBG\` | 基线解压结果（含 `download.json`） |
| `sdk\...\prebuild\ld\ML307A\DSLN\app_dsln.ld` | **内存预算权威来源（1MB flash / 512K ram）** |
| `sdk\...\docs\ML305A_ML307A_OpenCPU_API手册.chm` | 已用 `hh.exe -decompile` 成功解出 209 个 Doxygen HTML（API 头文件源码），**纯 API 手册，无烧录章节** |
| `sdk\...\tools\adiff\README.docx` | ASR 官方《ASR1601 FOTA》文档（`.pac` 无关，讲 FOTA 差分包） |
| `sdk\...\tools\toolchain\gcc-arm-none-eabi\bin\arm-none-eabi-gcc.exe` | 实测 `arm-none-eabi-gcc 8.3.1 20190703 (release) [gcc-8-branch revision 273027]` |
| `<DOWNLOADS>\QFlash_V8.1_CN.zip` | 内含 `Quectel_QFlash_用户指南_V4.5.pdf` 与 `QFlash_V8.1.zip` |
| └ `QFlash_V8.1\ASR\1\adownload1.exe` | 自述 *"Asrmicro aboot download console application"*，含 `AT$MYDOWNLOAD=1`/`AT+QDOWNLOAD=1`/`Arom Usb Boot Port`/`aboot.zip` |
| └ `QFlash_V8.1\aboot.dll` | 含 `download.json`/`arom\|arom-tiny`/`crane`/`config/template/`/`dbconfig/product/` |
| └ `QFlash_V8.1\Setting\UpgradeDownload.ini` | 含 `UserGuide = UNISOC Download User Guide V4.1.pdf`（证明主体为展锐工具） |
| └ `QFlash_V8.1\bootloader.ini` | `Baudrate_DL=921600` / `Baudrate_Log=921600`（展锐分支） |
| `sdk\...\out\images\ML307A\DSLN\DBG\cp.bin` | 内核镜像中检索到 AT 命令表 `$MYDOWNLOAD: (0,1)`、`$MYFOTA=<...>`、`$MYMINISYS=<enable>` |

---

## 附：一句话回答"如何从中移 ML307A-DSLN 的 SDK 产出可烧录固件"

> 把 SDK 放到**纯英文无空格**路径 → 在该目录执行
> `ML307A_build.bat clean` → `ML307A_build.bat DSLN`（不加 `demo`）→
> 得到 **`out\images\ML307A\DSLN\ML307A_DSLN_APP.zip`**（约 10.4MB，内含 `download.json` + `user_app.bin` + 全部基线镜像）。
> **不是 `.pac`**。这个 zip 就是交给下载工具的东西；工具需要能识别 **ASR aboot release package**。
> 日志看 **UART0 / 921600**，找 `ML307A Start`。
