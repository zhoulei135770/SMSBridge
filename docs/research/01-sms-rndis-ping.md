# 短信 / RNDIS / PING 能力核实

> **核实对象**：中移 ML307A-DSLN（ASR1606）OpenCPU SDK `ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release`
> **交叉核对**：另一份被自述为 SDK 1.4.3 的镜像 `liu-big/OpenCPU-SDK`（用于回答"较新版本是否有 cm_sms.h / cm_usb.h 等"）
> **核实方法**：直接抓取并逐字阅读 `include/cmiot`、`include/platform/lwip`、`src/cm_lib`、`examples`、`tools/scripts`、构建脚本的**原文**；`raw.githubusercontent.com` 抓取失败时改用 jsDelivr 镜像（`fastly.jsdelivr.net` / `cdn.jsdelivr.net`）+ GitHub contents/blobs API 核对。
> **标注约定**：
> - **【官方依据】**= SDK 头文件/源码/构建脚本原文，或中移官方文档正文
> - **【社区经验】**= 第三方博客、个人开源仓库、代理商文档
> - **【未证实】**= 本次没有任何可引用来源，**不编造 API 名**
>
> **本次的取证限制（重要）**：中移官方手册与 AT 手册都是 PDF，本环境的抓取器不支持 PDF 正文解析（返回 `unsupported content type "application/octet-stream"`），因此**官方 PDF 里的内容一律不当作已核对**，只用其目录/文件名作为线索，并在下文标注"PDF 未解析"。凡涉及 PDF 的结论都降级为"未证实/待核对"。

---

## 结论速览

| # | 结论（一句话） | 置信度 |
|---|---|---|
| 1 | **短信**：官方途径只有"虚拟 AT 通道 + 3GPP 短信指令"，SDK **没有** `cm_sms.h` 这类短信专用 API；发送可用官方封装 `cm_virt_at_sms_send()`，接收必须用 `cm_virt_at_urc_reg()` 注册 URC 回调后自行解码 PDU（`+CMT` / `+CMGL`）。 | **高**（SDK 原文 + 两个版本清单 + 社区落地代码） |
| 2 | **RNDIS**：**未证实**存在任何运行时开关 RNDIS 的 API（1.4.2 连 `cm_usb.h` 都没有；1.4.3 新增的 `cm_usb.h` 只管 USB 虚拟串口 USB2COM，没有任何 RNDIS/网卡接口）；官方构建面只有型号/容量/DEMO 开关，**没有 USB/RNDIS 编译期选项**；"电脑侧默认网关 = 192.168.0.1"**未证实**（SDK 平台层唯一可见的编译期默认值是 `LWIP_ROUTER_IP = 0xC0A80000`，即 192.168.**0**.x 家族）。 | **中**（否定性结论有原文，但"官方配置方式"与具体 IP 无官方正文） |
| 3 | **PING / 流量**：SDK 无 `cm_ping.h`、无 PING API；官方 AT 侧社区资料一致给出 `AT+MPING="host"`，但它能否走 OpenCPU 的虚拟 AT **未证实**（官方头文件明示虚拟 AT 只支持 3GPP 指令，并点名 `AT+MUESTATS` 等私有指令不支持）；PDP 上下行**字节数**没有任何官方接口（`cm_radio_info_t` 只有 `tx_time`/`rx_time` 累计**时长**），只能应用层自行计数。 | **中** |

---

## 1. 短信收发途径

### 1.1 先确认"没有短信专用 API"

`include/cmiot` 全量清单（1.4.2 共 **40** 个头文件）中确实**没有** `cm_sms.h`、`cm_usb.h`、`cm_ping.h`、`cm_network.h`、`cm_flash.h`、`cm_wdt.h`：

- 【官方依据】目录清单：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/tree/master/include/cmiot>
- 【旁证，较新版本】一个自述为 SDK 1.4.3 的镜像，其 `include/cmiot` 共 **42** 个文件，**只多出 `cm_cpuinfo.h` 与 `cm_usb.h`**，仍然没有 `cm_sms.h` / `cm_ping.h` / `cm_network.h` / `cm_flash.h` / `cm_wdt.h`：
  <https://github.com/liu-big/OpenCPU-SDK/tree/main/include/cmiot>
  （该镜像自述"ML307A-DCLN/ML305A-DC 无法通过最小系统 FOTA 从 SDK 1.4.2 升级至 SDK 1.4.3"，见仓库首页 <https://github.com/liu-big/OpenCPU-SDK>；版本号来自仓库描述，属**旁证**而非官方发布页。）
- 【官方依据】1.4.2 的 `examples/` 目录里也**没有** sms 示例（有 adc/asocket/aliyun/audio/cJSON/camera/demo_main/fota/fs/ftp/gnss/gpio/http/i2c/keypad/lbs/lcd/mqtt/ntp/pm/pwm/sd/spi/tts/uart/**virt_at**/wifiscan）。
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/tree/master/examples>

**结论**：ML307A OpenCPU 的短信能力**全部挂在虚拟 AT 通道**上，"官方途径 = 虚拟 AT（3GPP 短信指令）+ 自行 PDU 编解码"。不存在 `cm_sms.h`。

### 1.2 官方头文件原文摘录（1.4.2 与 1.4.3 的 `cm_virt_at.h` 逐字一致，均为 4774 字节）

来源：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_virt_at.h>
（1.4.3 镜像同一文件：<https://github.com/liu-big/OpenCPU-SDK/blob/main/include/cmiot/cm_virt_at.h>）

文件头的"虚拟 AT 需注意事项"（原文照录，与三问都相关）：

```
 * 1、虚拟AT不完全支持中移开发的AT手册中的指令,仅支持3GPP的AT指令，
 *    例如：
 *    AT+MUESTATS=radio  AT+MUESTATS=cell  AT+MUESTATS=sband
 *    等AT指令不支持,用户需根据实际使用测试结果为准；其中部分功能
 *    的AT指令也有所区别，虚拟AT都遵循了使用3GPP标准的AT指令，
 *    例如：
 *    (1)获取ICCID的指令为【AT*ICCID?】(2)开启拨号上网的AT指令为
 *   【AT+MDIALUPCFG="auto",1】,关闭为【AT+MDIALUPCFG="auto",0】
 * 2、为了避免虚拟AT使用频繁导致程序阻塞，暂不支持使用多线程操作虚
 *    拟AT，同时在使用虚拟AT时每次都需要初始化和去初始化。
 * 3、虚拟AT读取数据的方式是使用消息队列一次性读取固定长度的数据，
 *    当需要读取的数据过长时，需要多次发指令进行一逐读取；例如：
 *    读取短信消息。
 * 4、使用接口cm_virt_at_sms_send发送PDU格式的短信时，需要在发送
 *    的信息编码中加入：短信中心号码（SMSC）。
```

关键函数原型（原文照录）：

```c
/**来电短信等消息主动上报调函数类型*/
typedef void (*cm_at_urc_callback)(char *urc);
/**虚拟AT通道回调函数类型*/
typedef void (*cm_virt_at_cb)(void *param);

int32_t cm_virt_at_init(cm_virt_at_cb cb);
int32_t cm_virt_at_send(uint8_t * data,int32_t len);      /* @details 暂不支持发送短信 */
int32_t cm_virt_at_get(unsigned char* data,int len);
void    cm_virt_at_deinit(void);
void    cm_virt_at_urc_reg(cm_at_urc_callback cb);        /* @brief 初始化消息主动上报通道 */

/**
 * @brief 发送短信
 * @param [in] atCmgs   AT命令AT+CGMS=后的参数
 * @param [in] msgData  需要发送的短信信息
 * @param [in] simId    选择需要发送短信的SIM卡
 */
int cm_virt_at_sms_send(char* atCmgs, char* msgData, int simId);

/**
 * @brief 本地短信存储
 * @param [in] smsNum   接收短信的电话号码
 * @param [in] msgData  需要发送的短信信息
 * @param [in] simId    选择需要发送短信的SIM卡
 */
int cm_virt_at_msg_memory(char* smsNum, char* msgData, int simId);
```

> 注意：`cm_virt_at_send()` 的注释明确写 **"暂不支持发送短信"** —— 发短信必须走 `cm_virt_at_sms_send()`。另外 `atCmgs` 注释写作 `AT+CGMS=`（原文如此，疑为 `AT+CMGS=` 的笔误，**未证实**），含义是"AT+CMGS= 之后的参数"，即 PDU 长度那一项。

### 1.3 URC（收到短信后如何上报到应用层）

**官方接口**：`void cm_virt_at_urc_reg(cm_at_urc_callback cb);`，回调类型 `typedef void (*cm_at_urc_callback)(char *urc);`，注释即"来电短信等消息主动上报"。也就是说，**官方只提供"注册一个回调、回调参数是原始 URC 字符串"这一层**，PDU 的解析/解码完全由应用自己做。

**官方示例只演示指令收发，没有 URC 演示**：`examples/virt_at/src/cm_demo_virt_at.c` 里只做了 `cm_virt_at_init(oc_resp_cb)` → `cm_virt_at_send()` → 在回调里 `cm_virt_at_get()` → `cm_virt_at_deinit()`：
<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/examples/virt_at/src/cm_demo_virt_at.c>

**社区落地代码（可直接参考的完整流程）**：一个"用 ML307A OpenCPU 转发手机卡短信到服务器"的开源项目，`applaction/sms_recv.c`（26 KB）完整实现了收短信：
<https://github.com/lghh45/ML307A_OPENCPU_-/blob/master/applaction/sms_recv.c>
（`sms_pdu.c` 为自写 PDU 解码，含 GSM7/UCS2 与长短信 UDC 拼接；`sms_queue.c` 为上传队列。）

其关键流程与要点（**社区经验**，与官方头文件条款完全吻合）：

```c
/* 1) 注册 URC 回调（只做拷贝 + 置事件，回调里不做耗时动作） */
cm_virt_at_urc_reg(urc_cb);

/* 2) 打开 PDU 模式 与 "+CMT 直接上报" */
at_exec("AT+CMGF=0", ...);              /* 0 = PDU 模式 */
at_exec("AT+CNMI=2,2,0,0,0", ...);      /* 第 2 个参数=2：新短信直接用 +CMT 上报，PDU 就在 URC 里 */

/* 3) URC 回调里识别 +CMT，转交工作线程解析 PDU */
static void urc_cb(char *urc) {
    log_text("[urc] ", urc);                       /* 底层可能一次交上来多行 URC，用 strstr 而非 strncmp */
    if (strstr(urc, "+CMT:") == NULL) return;      /* 只关心 +CMT；+CLIP/RING 是来电 */
    memcpy(s_cmt_buf, urc, len); s_cmt_pending = 1;
    osEventFlagsSet(s_evt, EVT_SMS_CMT);           /* 事件标志唤醒任务线程 */
}

/* 4) 工作线程：osEventFlagsWait(...) → sms_handle_cmt() → sms_pdu_decode() → 入队 */
```

同一个文件里还记录了这些**社区实测结论**（对设计很有用）：
- 每条 AT 事务都是"`cm_virt_at_init(cb)` → `cm_virt_at_send()` → 信号量等应答 → 在应答回调里循环 `cm_virt_at_get()` → `cm_virt_at_deinit()`"，并用**互斥锁**串行化（对应官方注意事项 2："暂不支持多线程操作虚拟 AT"）。
- 长短信按 UDH 的 ref/part 自行拼接（含分段超时丢弃）。
- 兜底自检用 `AT+CMGL=4`（列出全部短信）、`AT+CSMS?`（`mt=1` 表示网络支持收短信）、`AT+CPMS?`、`AT+CNMI?`。
- 代码注释里明确写了"`cm_virt_at.h` 注意事项 3 提到，响应过长时需要多次发指令逐次读取；若实测发现 `+CMT` PDU 被截断，需要在这里补重发续读的逻辑"。
- 【社区经验，未证实】`+CLIP`/`RING` 也是经同一条 URC 通道上报的（来电可顺带做上报）。
- 【社区经验，未证实的风险】URC 是否只在虚拟 AT 通道处于 init 状态时才会送达，未见官方说明；该项目的写法是"回调常驻 + 每次命令临时 init/deinit"，**是否漏收 URC 需要在真机压测确认**。

**AT 层面的收发指令（社区实测于 ML307A-DSLN / ML307R）**：`AT+CMGF`、`AT+CMGS`、`AT+CMGL`、`AT+CNMI`、`AT+CPMS`、`AT+CSMS`，数值都以 PDU 模式为准。
- 【社区经验】ML307A 中文短信实测（`AT+CMGF=0` → `AT+CMGS=18` → PDU → 以 `1A` 结束）：<https://blog.csdn.net/qq_41954594/article/details/150221963>
- 【社区经验】中移模组 AT 使用手册整理（含 `AT+MPING`、`AT+MDNSGIP`、`AT+MIPOPEN` 等；该文描述的是 ML307A 系列）：<https://aiot.csdn.net/69df5f590a2f6a37c5a01c41.html>

### 1.4 本节未证实项

- 【未证实】`cm_virt_at_sms_send()` 的 `atCmgs` / `msgData` 具体格式（PDU 十六进制字符串？是否需要自带 CRLF、是否自动补 `1A`）——头文件只给了文字说明，没有示例，需真机验证。
- 【未证实】是否存在任何官方"短信发送结果/发送完成"的 URC（如 `+CMGS:`）经 `cm_virt_at_urc_reg` 上报；也没有官方文档说明 URC 回调的线程上下文。
- 【未证实】中移官方《ML307A_SDK开发指导手册》《ML307A_OpenCPU资源综述》里是否另有"短信"章节（PDF 未解析）。可自行下载核对：
  <https://gitee.com/YZRDEG/ML307A/tree/master/ML307A_OPENCPU%E6%89%8B%E5%86%8C>
  官方下载入口：<https://os.iot.10086.cn/download/case>（页面为 JS 渲染，本次只取到标题"下载中心"）

---

## 2. RNDIS 开关与默认网关 IP

### 2.1 有没有"运行时开启/关闭 RNDIS"的 API？——没有找到，判为未证实

- 【官方依据】1.4.2 `include/cmiot` 全量清单里**没有 `cm_usb.h`，也没有任何 RNDIS/网卡相关头文件**：
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/tree/master/include/cmiot>
- 【官方依据】1.4.3 镜像**新增了 `cm_usb.h`，但它只管 USB 虚拟串口（USB2COM），不含 RNDIS**。原文摘录：
  <https://github.com/liu-big/OpenCPU-SDK/blob/main/include/cmiot/cm_usb.h>

```c
/**
 *  @file    cm_usb.h
 *  @brief   usb虚拟串口功能
 */
#define CM_USB_EVT_INSERT   1
#define CM_USB_EVT_REMOVE   0

typedef void (*cm_usb2com_recv_cb)(void *data, int32_t len);
typedef void (*cm_usb2com_status_cb)(int32_t evt);

void    cm_usb2com_register_recv_cb(cm_usb2com_recv_cb recv_cb);
int32_t cm_usb2com_send_data(void *data, int32_t len);
void    cm_usb2com_register_status_cb(cm_usb2com_status_cb status_cb);
```

  也就是说：**到 1.4.3 为止，OpenCPU 公开 API 里唯一的 USB 能力是"USB 当串口用"，没有 USB 网卡/RNDIS 的启停或查询接口。**

### 2.2 官方"如何配置 RNDIS"？——仅找到两条线索

**线索 A（官方 SDK 原文，模块侧的 AT 开关，指向"拨号上网"）**：`cm_virt_at.h` 里写明

```
 *    (1)获取ICCID的指令为【AT*ICCID?】(2)开启拨号上网的AT指令为
 *   【AT+MDIALUPCFG="auto",1】,关闭为【AT+MDIALUPCFG="auto",0】
```

也就是说，模块侧有一个可经虚拟 AT 下发的"拨号上网开关"：`AT+MDIALUPCFG="auto",1` / `"auto",0`。
- 【官方依据】来源同上：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_virt_at.h>
- 【未证实】它是否等价于"开/关 USB RNDIS 网卡"（"拨号上网"在 AT 版手册里可能是 PPP/自动拨号语境），头文件没有说明；需要看 AT 手册正文或实测。**不要把它当成 RNDIS API 使用。**

**线索 B（官方构建脚本，编译期没有 USB/RNDIS 开关）**：`ML307A_build.bat` 原文（已完整解码核对）：

```
:: 当前支持DSLN/DCLN/GSLN/GCLN子型号编译
:: SDK编译路径不支持中文、空栰
...
@if /i "%1" == "DCLN" ( ... FLASH_OPT=4M ... GNSS_OPT=NO )
@if /i "%1" == "DSLN" ( ... FLASH_OPT=8M ... GNSS_OPT=NO )
@if /i "%1" == "GSLN" ( ... FLASH_OPT=8M ... GNSS_OPT=YES )
@if /i "%1" == "GCLN" ( ... FLASH_OPT=4M ... GNSS_OPT=YES )
@if /i "%2" == "demo" ( @set DEMO_SUPPORT=y ) else ( @set DEMO_SUPPORT=n )
```

`tools/scripts/config.mk` 原文（96 字节）：

```
HWVER :=DCLN
DEMO_SUPPORT:=n
MODEL_NAME:=ML307A
PROD_VER:=H0
BuildTimeStamp:=20220810153543
```

- 【官方依据】<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/ML307A_build.bat>、<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/tools/scripts/config.mk>
- **结论**：OpenCPU 的构建配置面只有"型号/容量/GNSS/DEMO"这几个开关，**没有 USB、RNDIS、ECM、MBIM 之类的编译期选项，也没有 menuconfig 式的 NV 菜单**。（"菜单配置 NV"这一说法在 SDK 里**未证实**。）

### 2.3 RNDIS 在 SDK 里的真实位置：预编译平台层（lwIP），不是 OpenCPU 公开 API

RNDIS/USB 网卡在 ASR 平台 lwIP 里是**编译进去的**，可以在随 SDK 附带的平台头文件里看到，但这些**不在 `include/cmiot` 下**，属于平台内部实现：

- `include/platform/lwip/arch/lwipopts.h`（官方 SDK 原文）：`#define RNDIS_HEADER_LEN 44`、`#define RNDIS_HDR_LEN (PMSG_SIZE + RNDIS_HEADER_LEN)`，并有 `IF_USB_DEBUG`（"Enable debugging in if usb"）等调试位。
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/arch/lwipopts.h>
- `include/platform/lwip/arch/lwipopts_crane.h`：`#define LWIP_NETIF_PC (1)`、`#define LWIP_NETIF_BRIDGE (1)`、`#define LWIP_DHCP_SERVER (1)`、`#define LWIP_NETIF_LTE (1)`。
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/arch/lwipopts_crane.h>
- `include/platform/lwip/netif/netif_td.h`：`#define LWIP_ROUTER_IP (0xC0A80000) /* 192.168.x.x */`；并有 `struct td_info_t { u32_t ip_addr; u32_t dns_ip_1; u32_t dns_ip_2; u32_t gw_ip; u32_t mask; };`
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/netif/netif_td.h>
- `include/platform/lwip/lwip/dhcpd_api.h`：**这是"软件侧读取（PC 看到的）网关 IP"的唯一现成接口**，原文：

```c
extern u32_t                g_server_ip; // Router server ip
// return router ip address in network order
static inline u32_t dhcpd_router_ip(void) { return htonl(g_server_ip); }
static inline u32_t dhcpd_get_ip(void)    { return g_server_ip; }
```

  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/lwip/dhcpd_api.h>

### 2.4 电脑侧默认网关是不是 192.168.0.1？

**结论：未证实。** 证据状态如下：

- 【官方依据（间接）】SDK 平台头文件里唯一可见的**编译期默认**是 `LWIP_ROUTER_IP (0xC0A80000)`，注释写 `/* 192.168.x.x */` —— `0xC0A80000` 是 **192.168.0.0**，即默认家族是 **192.168.0.x**，与"192.168.0.1"不一致（是否被其它预编译库覆盖，**未证实**）。
  来源：`netif_td.h`，链接同上。
- 【官方依据】真正的值是**运行期变量** `g_server_ip`，头文件不给数值；`dhcpd_router_ip()` 才是读取它的方法（见 2.3）。
- 【社区经验，且**容易误用**】公开资料里确实出现过"192.168.0.1"，但它来自**另一款基于 ML307A 的成品短信转发器**："手机连接短信宝辐射出来的 WIFI（SMS-Forwarder-AP）… 浏览器打开 **192.168.0.1**" —— 这是该产品的 **WiFi SoftAP 登录地址**，**不是** ML307A 的 RNDIS 网关。
  来源：<https://www.yingdao.com/community/detaildiscuss?id=929944702702145536>
- 【未证实】中移《ML307A 拨号上网用户手册》（PDF，本次未解析）最可能写明该 IP：
  <https://gitee.com/YZRDEG/ML307A/raw/master/ML307A_%E8%B5%84%E6%96%99/ML307A_%E6%8B%A8%E5%8F%B7%E4%B8%8A%E7%BD%91%E7%94%A8%E6%88%B7%E6%89%8B%E5%86%8C_V1.0.0.pdf>
  （同目录还有《TCP/IP 用户手册 V3.0.0》《AT_Commands_Reference_Guide_4G_Series_V1.2.0》，目录页：<https://gitee.com/YZRDEG/ML307A/tree/master/ML307A_%E8%B5%84%E6%96%99>）

**结论写法建议**：在固件/文档里**不要把 192.168.0.1 写成事实**，应写"待实测/以中移《拨号上网用户手册》为准"。

### 2.5 "软件如何查询该 IP"

按可用性从高到低：

1. **OpenCPU 官方 API：没有。** 逐一核对 1.4.2 的 `include/cmiot` 40 个头文件，没有任何"查询本机/网卡/DHCP 网关 IP"的 `cm_*` 接口；`cm_asocket.h` 只把标准 socket 接口做了别名（`bind/getpeername/getsockname/getsockopt/...`），`getsockname()` 只能拿到**socket 自身的本地地址**，拿不到 RNDIS 网卡的网关。
   <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_asocket.h>
2. **虚拟 AT（3GPP，可实测）**：`AT+CGPADDR` 可查模块自身 PDP 地址（社区实测响应形如 `+CGPADDR: 1,"10.0.0.100","2408:..."`）——这是**模块的公网/私网地址，不是 PC 侧网关**。
   【社区经验】<https://blog.csdn.net/qq_41954594/article/details/150221963>
3. **平台内部接口（非官方 OpenCPU API，不建议生产使用）**：`dhcpd_router_ip()` / `g_server_ip`（`dhcpd_api.h`）。它给出的正是"作为 DHCP 服务器的本机地址"，即 PC 侧看到的默认网关。风险：属于预编译平台层的内部符号，没有官方兼容承诺。
4. **最省事的办法**：在 PC 侧 `ipconfig` 读 RNDIS 网络适配器的默认网关（部署/运维层面的答案）。

---

## 3. PING 与 PDP 流量统计

### 3.1 SDK 有没有 PING 能力？

- 【官方依据】**没有 PING 专用 API**：1.4.2 与 1.4.3 的 `include/cmiot` 清单里都没有 `cm_ping.h`（1.4.3 只多了 `cm_cpuinfo.h`、`cm_usb.h`）。
  1.4.2：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/tree/master/include/cmiot>
  1.4.3：<https://github.com/liu-big/OpenCPU-SDK/tree/main/include/cmiot>
- 【官方依据（边界条件）】官方头文件第一章就限定："虚拟 AT 不完全支持中移开发的 AT 手册中的指令，**仅支持 3GPP 的 AT 指令**"，并点名 `AT+MUESTATS=radio/cell/sband` 等**不支持**。`AT+MPING` 属于中移私有指令，**因此它能否经虚拟 AT 使用，官方未证实，必须实测。**
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_virt_at.h>
- 【社区经验，两条独立来源一致】中移 4G 系列模组（ML307A / ML307R）的 AT 侧 PING 指令是：

```
AT+MPING="www.baidu.com"
+MPING: 0,"2409:xxxx:xxxx:xxxx::1",16,60,49
+MPING: 0,"2409:xxxx:xxxx:xxxx::1",16,75,49
+MPING: "statistics",4,0,60,75,67
```

  来源：<https://aiot.csdn.net/69df5f590a2f6a37c5a01c41.html>（"（四）PING 示例"，同页明确说是 ML307A 系列能力）、<https://cloud.tencent.com.cn/developer/article/2417930>（ML307R，同族 AT 指令集，含完整回显与 statistics 行）。
  **注意**：这两条都是第三方资料，不是中移官方手册正文。
- 【官方依据（可用的底层能力）】lwIP 的 raw socket 在本平台是**打开**的：`lwipopts_crane.h` 中 `#define LWIP_RAW 1`，且 SDK 直接提供了 `include/platform/lwip/lwip/raw.h`；同时 `cm_asocket_open(int domain, int type, int protocol, ...)` 的 domain/type/protocol 是**原样透传**给 lwIP 的。
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/arch/lwipopts_crane.h>
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/lwip/raw.h>
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_asocket.h>
  → **理论上**可用 `AF_INET + SOCK_RAW + IPPROTO_ICMP` 自己实现 ICMP Echo（需自造 IP/ICMP 头与校验和）。【未证实】该路径在 OpenCPU 沙箱里是否被允许、LTE 侧是否放行 ICMP、收包回调是否可用——**没有官方示例，也没有社区成功案例**，判为"理论可行、未验证"。
- 【未证实】官方《TCP/IP 用户手册 V3.0.0》是否收录 `AT+MPING`（PDF 未解析）：
  <https://gitee.com/YZRDEG/ML307A/raw/master/ML307A_%E8%B5%84%E6%96%99/TCP_IP%E7%94%A8%E6%88%B7%E6%89%8B%E5%86%8C_V3.0.0.pdf>
  同理《AT_Commands_Reference_Guide_4G_Series_V1.2.0.pdf》：<https://gitee.com/YZRDEG/ML307A/raw/master/ML307A_%E8%B5%84%E6%96%99/AT_Commands_Reference_Guide_4G_Series_V1.2.0.pdf>

### 3.2 如何查询 PDP 上下行流量统计？

**结论：官方没有任何"流量字节数"接口。** 逐项核对如下：

1. 【官方依据】`cm_modem_info.h` 的无线信息结构体里，与"量"有关的字段**只有累计时长，不是字节**：

```c
typedef struct {
    uint8_t  rat;            /**无线接入模式*/
    uint16_t rsrp; rsrq; rssi; rxlev; tx_power;
    uint32_t tx_time;        /**上行累计发送时长 */
    uint32_t rx_time;        /**下行累计接收时长 */
    ...
} cm_radio_info_t;

int32_t cm_modem_info_cell(cm_cell_info_t cell_info[], uint16_t cell_info_num);
int32_t cm_modem_info_radio(cm_radio_info_t *radio_info);   /* @details 该函数无法获取rat，需通过虚拟AT+COPS？获取 */
```

  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/cmiot/cm_modem_info.h>
  （`tx_time`/`rx_time` 的单位头文件未写，**未证实**，按字面是"时长"。）

2. 【官方依据】SDK 附带的 modem 库源码 `src/cm_lib/modem/cm_modem.h` / `cm_modem.c`（1.4.2）也没有流量接口，只有 `cm_modem_get_csq`、`cm_modem_get_radio_info`、`cm_modem_activate_pdp` / `cm_modem_deactivate_pdp` / `cm_modem_get_pdp_state`（内部就是发 `AT+CGACT=1,<cid>` / `AT+CGACT?`）、`cm_modem_set_cfun` / `cm_modem_get_cfun`、`cm_modem_set_edrx_cfg`、`cm_modem_set_psm_cfg`、`cm_modem_get_cpin`、`cm_modem_get_cgmr` 等，全部清单已核对，无流量接口。
   <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/src/cm_lib/modem/cm_modem.h>
   <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/src/cm_lib/modem/cm_modem.c>
3. 【官方依据，较新版本也没有】1.4.3 的 `cm_modem.h` 相对 1.4.2 新增的只有 `cm_modem_get_pds_state()`、`cm_modem_set_cgreg_state()`、`cm_modem_get_cgreg_state()`（GPRS 注册状态），**依然没有任何流量/字节统计**。
   <https://github.com/liu-big/OpenCPU-SDK/blob/main/src/cm_lib/modem/cm_modem.h>
4. 【官方依据，平台层只有"包计数/速率"】`include/platform/lwip/netif/netif_td_api.h` 暴露了：

```c
extern struct td_speed_t td_record_tx;
extern struct td_speed_t td_record_rx;
extern u32_t td_tx_pkt_cnt;
extern u32_t td_rx_pkt_cnt;
u32_t td_get_tx_speed(void);
u32_t td_get_rx_speed(void);
```

  这是**包个数与速率**（`struct td_speed_t { u32_t ticks; t1; t2; speed; }`，见 `netif_td.h`），**不是字节数**，而且属于平台内部接口：
  <https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release/blob/master/include/platform/lwip/netif/netif_td_api.h>
5. 【官方依据，lwIP 自身统计是关的】`lwipopts_crane.h` 中 `#define LWIP_STATS 0`，所以不能指望 lwIP 的 `stats.h` 计数（虽然 `lwip/stats.h` 头文件存在）。
6. 【未证实】AT 侧是否有官方"流量统计"指令：`cm_virt_at.h` 提到过 `AT+MUESTATS`（中移私有），但**明确说虚拟 AT 不支持**，因此即使该指令能报统计量，OpenCPU 侧也用不了；中移 AT 手册是否有专门的流量/字节统计指令，本次**未能核对（PDF 未解析）**。

**可行的替代方案**：【社区/工程经验】在应用层自己按 socket/HTTP/MQTT 收发字节累加（`send`/`recv` 的返回值求和），并周期持久化到 `cm_fs`（文件系统无目录概念）；把 `cm_radio_info_t.tx_time/rx_time` 仅当"链路活跃时长"使用。

---

## 对固件设计的直接影响

| 核实结论 | 对应的设计决定 |
|---|---|
| 1. 没有 `cm_sms.h`，短信 = 虚拟 AT + 自写 PDU 编解码；URC 只给原始字符串 | **设计决定**：自建 `sms` 模块——① `cm_virt_at_urc_reg()` 常驻注册，回调内**只拷贝 + `osEventFlagsSet()`**；② 一个专用 SMS 线程做 `osEventFlagsWait()` → PDU 解码 → 入队（`osMessageQueueNew/Put/Get`，见 `cm_os.h`）；③ 全局**互斥锁串行化**所有虚拟 AT 事务（官方明示不支持多线程操作虚拟 AT，且每次都要 init/deinit）；④ 自己实现 PDU 解码（GSM7/UCS2、UDH 长短信拼接、SCTS 时间戳），发送走 `cm_virt_at_sms_send()` 并**在 PDU 中带 SMSC**（官方注意事项 4）；⑤ 收不到短信时用 `AT+CSMS?`/`AT+CPMS?`/`AT+CNMI?`/`AT+CMGL=4` 做自检与补收；⑥ 落地存储用 `cm_fs_open/write/read`（**无路径/目录**，`cm_fs_sync` 被 `#if 0` 关掉）；⑦ 预留"URC 被截断时重发续读"的逻辑（官方注意事项 3 点名"读取短信消息"） |
| 2. 没有 RNDIS 运行时 API；构建期也没有 USB/RNDIS 开关；无法从 OpenCPU 官方 API 拿到网关 IP | **设计决定**：① **不要**在固件里规划"运行时开/关 RNDIS"功能，也不要依赖任何 `cm_usb_rndis_*` 之类的接口（不存在，等于编造）；② 电脑侧上网能力划归 AT 版/NV/出厂配置范畴，需中移 FAE 书面确认（问题清单见文末）；③ 若产品必须知道 PC 侧网关 IP，**做成可配置项**（默认不写 192.168.0.1，UI/文档写"待实测"），或在允许使用内部接口的前提下用 `dhcpd_router_ip()` 兜底并加注释说明其不受支持；④ 1.4.3 的 `cm_usb.h` 可用来做"USB 插拔 + 虚拟串口透传"（`cm_usb2com_register_status_cb` / `cm_usb2com_send_data`），这与联网无关，别混用 |
| 3. 没有 PING API；`AT+MPING` 属私有指令、虚拟 AT 是否支持未证实；流量无官方字节统计 | **设计决定**：① 心跳/在线检测**不要依赖 ICMP**：默认方案用"TCP connect / HTTP HEAD / MQTT keepalive 时长"；② 如确需 PING，先做 30 分钟真机实验验证 `AT+MPING` 经 `cm_virt_at_send()` 是否回 `OK` 与 `+MPING:`（成功则按私有指令处理并加降级分支，失败则退回 TCP 探测；raw socket 路线作为实验分支，不当主线）；③ 流量统计**自建**：在 socket/HTTP/MQTT 收发的返回处累加字节，按小时/天滚动持久化到 `cm_fs`，上报给平台；④ `cm_radio_info_t.tx_time/rx_time` 只用于"链路活跃时长/在线率"指标，**不要**当流量用 |

---

## 附：未证实项与最小验证步骤

**未证实清单（不要写进固件设计文档当事实）**
1. `192.168.0.1` 是 ML307A RNDIS 默认网关（SDK 头文件里唯一可见默认是 `0xC0A80000` = 192.168.0.x；"192.168.0.1"来源是另一个产品的 SoftAP）。
2. `AT+MDIALUPCFG="auto",1/0` 是否等于"开/关 RNDIS 网卡"。
3. `AT+MPING` 能否经 OpenCPU 虚拟 AT 使用（官方明示只支持 3GPP 指令）。
4. `cm_virt_at_sms_send()` 的入参格式与发送结果反馈方式。
5. URC 是否只在虚拟 AT init 期间送达（影响"回调常驻 + 命令临时 init"的写法是否漏短信）。
6. 中移官方是否有"流量字节数"AT 指令（PDF 未解析）。
7. `cm_radio_info_t.tx_time/rx_time` 的单位。

**最小验证步骤（1 天内可完成，全部可自证）**
1. AT 版本模组 + PC：插 USB，PC 侧 `ipconfig` 记录 RNDIS 适配器默认网关 → 得到 RNDIS 网关实测值（回答第 1 项）。
2. 在 OpenCPU 固件里用 `cm_virt_at_send()` 依次发：`AT+MPING="www.baidu.com"`、`AT+MDIALUPCFG?`、`AT+CGPADDR`、`AT+CMGF?`，看是否返回 `OK`/`ERROR`（回答第 2、3 项与虚拟 AT 边界）。
3. `cm_virt_at_sms_send()` 用一条已知 PDU（含 SMSC）实测（回答第 4 项）；同时用 `cm_virt_at_urc_reg()` 打日志，连续 2 小时收短信，统计有无漏收（回答第 5 项）。
4. 向中移 FAE 发问：① RNDIS 是否提供 OpenCPU API，若无，官方推荐的配置途径与默认网关 IP；② AT 手册中是否存在 PDP 上下行字节数统计指令，OpenCPU 侧是否可用；③ `AT+MPING` 是否列为支持指令。（把本文件的未证实项 1/2/3/6 一起贴过去最省事。）
5. 若需要 PDF 原文佐证：下载 <https://gitee.com/YZRDEG/ML307A/tree/master/ML307A_%E8%B5%84%E6%96%99> 中的《AT_Commands_Reference_Guide_4G_Series_V1.2.0》《TCP_IP 用户手册 V3.0.0》《ML307A 拨号上网用户手册 V1.0.0》，搜索关键词：`MPING`、`RNDIS`、`MDIALUPCFG`、`流量`/`statistics`。

**信息来源一览**
- SDK 1.4.2 源码镜像（官方 SDK 的第三方镜像）：<https://github.com/viys/ML305A_ML307A_OpenCPU_Standard_1.4.2.2023062518_release>
- SDK 1.4.3 旁证镜像：<https://github.com/liu-big/OpenCPU-SDK>
- 社区落地实现（ML307A OpenCPU 收短信/转发）：<https://github.com/lghh45/ML307A_OPENCPU_->
- 中移官方 OpenCPU 手册目录（Gitee）：<https://gitee.com/YZRDEG/ML307A/tree/master/ML307A_OPENCPU%E6%89%8B%E5%86%8C>
- 中移官方资料（AT 手册/TCP-IP 手册/拨号上网手册，Gitee）：<https://gitee.com/YZRDEG/ML307A/tree/master/ML307A_%E8%B5%84%E6%96%99>
- 官方下载中心（OneMO）：<https://os.iot.10086.cn/download/case>
