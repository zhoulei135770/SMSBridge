/**
 * @file    sdk_port.h
 * @brief   SDK 适配层：**唯一**允许 include ML307A OpenCPU SDK 头文件的模块。
 *
 * 设计目的（对应需求文档「五、非功能需求 4/5」）：
 *   1. 业务模块（配置、短信、流量、Web、Webhook、网络管理）全部是纯 C99，
 *      只依赖本头文件，因此可以在 PC 上用 gcc 直接单元测试（见 app/test/）。
 *   2. 换 SDK 版本时只需改 sdk_port.c 一个文件。
 *   3. 所有会阻塞/可能失败的 SDK 调用都在这里统一做超时与错误码归一化。
 *
 * 已核实可用的 SDK 能力与来源（详见 docs/sdk-reference/）：
 *   - 任务/延时/定时器/互斥锁 : include/cmiot/cm_os.h（CMSIS-RTOS2 风格，1 tick = 5ms）
 *   - 文件系统持久化          : include/cmiot/cm_fs.h（无目录，cm_fs_sync 被 #if 0 禁用）
 *   - 重启 / 上电原因          : include/cmiot/cm_pm.h（cm_pm_reboot / cm_pm_get_power_on_reason）
 *   - 时间                    : include/cmiot/cm_rtc.h（cm_rtc_get_current_time 返回 UTC 秒）
 *   - HTTP(S) 客户端          : include/cmiot/cm_http.h（同步接口 cm_httpclient_sync_request）
 *   - 短信收发 / AT 通道       : include/cmiot/cm_virt_at.h（只有发送；接收靠 URC 回调）
 *   - 信号 / 小区             : include/cmiot/cm_modem_info.h
 *   - 流量统计（平台持久化）   : include/platform/lwip/lwip/lwip_api.h（lwip_nwst_*）
 *   - 流量方向统计            : include/platform/lwip/lwip/lwip_stats.h（get_ip_flow_stats）
 *   - RNDIS 运行时开关        : include/platform/lwip/lwip/lwip_api.h
 *                              (lwip_rndis_hotplug_link / _break / lwip_set_usb_forward_flag)
 *   - RNDIS 网卡状态          : include/platform/lwip/lwip/netif.h（netif_usb_get_status）
 *   - TCP 服务端              : include/platform/lwip/lwip/sockets.h（lwip_socket/accept/...）
 */
#ifndef SDK_PORT_H
#define SDK_PORT_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================== */
/* 日志                                                                */
/* ================================================================== */
/** 直接写一行日志（不做格式化，由 app_log 负责拼装）。 */
void sdk_log_puts(const char *s);
/** 带 SDK 日志级别的格式化输出（内部用 vsnprintf 到定长栈缓冲）。 */
void sdk_log_printf(const char *fmt, ...);

/* ================================================================== */
/* 任务 / 时间 / 同步                                                  */
/* ================================================================== */
typedef void *sdk_mutex_t;
/** 任务函数类型 */
typedef void (*sdk_task_fn_t)(void *arg);

/**
 * @brief 创建任务。
 * @param stack_bytes 栈大小（字节）。需求文档指定 6144 字节。
 * @param prio        0=低于普通 1=普通（SDK 会把用户线程钳制到 osPriorityNormal）
 * @return 0 成功，<0 失败。
 * @note 必须在 cm_opencpu_entry() 返回前创建（此后 SDK 调度器接管）。
 */
int sdk_task_create(const char *name, sdk_task_fn_t fn, void *arg,
                    uint32_t stack_bytes, int prio);

/** 毫秒延时（内部按 5ms/tick 换算，自动向上取整）。 */
void sdk_task_sleep_ms(uint32_t ms);
/** 系统启动以来的毫秒数（单调，用于超时判定）。 */
uint32_t sdk_uptime_ms(void);

sdk_mutex_t sdk_mutex_new(void);
int         sdk_mutex_lock(sdk_mutex_t m, uint32_t timeout_ms);
int         sdk_mutex_unlock(sdk_mutex_t m);

/** 计数信号量（二进制信号量用 initial_count = 1） */
typedef void *sdk_sem_t;
sdk_sem_t sdk_sem_new(uint32_t initial_count);
void      sdk_sem_post(sdk_sem_t s);
/** @param timeout_ms 0xFFFFFFFF = 永久等待；@return 0 成功，<0 超时 */
int       sdk_sem_wait(sdk_sem_t s, uint32_t timeout_ms);

/* ---- 日历时间 ---- */
/** 当前 UTC 秒（时间未同步时返回 0）。 */
int64_t sdk_utc_seconds(void);
/** 本地时间字符串 "YYYY-MM-DD HH:MM:SS"，成功返回 0。 */
int     sdk_local_time_str(char *buf, size_t buf_size);
/** 本月标记 "YYYYMM"，成功返回 0；时间无效返回 <0。 */
int     sdk_month_tag(char *buf, size_t buf_size);
/** 已运行时长字符串，如 "3天 04:15:22"。 */
void    sdk_uptime_str(char *buf, size_t buf_size);

/* ---- 时钟同步（钉钉加签的前提，docs/09 第四节）---- */
/**
 * @brief 触发一次 NTP 对时（**非阻塞**）。
 *
 * SDK 的 `cm_ntp_sync()` 底层依赖 asocket + async_dns + eloop，而应用侧 socket 只能
 * 在 eloop 线程内操作（docs/08），所以这里把调用投递到 eloop 线程执行
 * （`cm_eloop_register_event` + `cm_eloop_post_event`）。结果由 SDK 回调写入内部状态。
 *
 * @param server NTP 服务器（域名/IP，最长 127 字节）；NULL/空串用 SDK 默认服务器
 * @return 0 已提交；<0 提交失败（-1 参数/eloop 不可用，-2 注册事件失败，-3 投递失败）
 * @note SDK 默认 `CM_NTP_CFG_SET_RTC = 1`，对时成功后系统时间自动生效；回调里还会
 *       自己解析 ISO-8601 串再写一次 RTC 作为兜底。
 */
int  sdk_time_sync_start(const char *server);

/** NTP 对时状态：0 = 未开始/进行中；1 = 成功；-1 = 失败。 */
int  sdk_time_sync_state(void);

/** NTP 回调收到的 ISO-8601 时间串（未成功时为空串）。 */
const char *sdk_time_sync_text(void);

/** 把 UTC 秒写进系统 RTC（cm_rtc_set_current_time）。@return 0 成功。 */
int  sdk_rtc_set_utc(int64_t utc_seconds);

/**
 * @brief 用模组自身的时钟对时（`AT+CCLK?`）。
 *
 * 入网后模组时钟通常已被网络校准，这条路径不依赖 NTP 服务器可达性，
 * 适合作为开机第一选择；成功后把 UTC 写进 RTC。
 *
 * @return 0 成功且已写入 RTC；-1 AT 失败；-2 应答解析失败；-3 时间不可信；-4 写 RTC 失败
 */
int  sdk_time_sync_from_modem(void);

/* ================================================================== */
/* 配置持久化（cm_fs 文件系统）                                        */
/* ================================================================== */
/**
 * @brief 读取配置 blob。
 * @param out      目标缓冲
 * @param out_size 缓冲大小
 * @return 实际读到的字节数，<0 表示不存在或读失败。
 * @note 内部会先在主文件 cfg.bin 中找，失败再退回备份 cfgbak.bin。
 */
int32_t sdk_cfg_load(void *out, uint32_t out_size);

/**
 * @brief 原子写配置 blob。
 *
 * 流程：写 cfgtmp.bin -> 回读校验 -> 旧 cfg.bin 改名为 cfgbak.bin ->
 *       cfgtmp.bin 改名为 cfg.bin。任一步失败返回 <0，旧配置仍然有效。
 * 只在保存配置 / 重置流量 / 流量超限时调用（需求 6.3）。
 */
int32_t sdk_cfg_save(const void *in, uint32_t size);

/**
 * @brief 通用文件读取（cm_fs）。
 * @return 读到的字节数，<0 失败（-2 = 文件不存在）。
 * @note 供启动自诊断写入"面包屑"使用；业务配置请用 sdk_cfg_load/save。
 */
int32_t sdk_file_read(const char *name, void *out, uint32_t out_size);

/**
 * @brief 通用文件写入（cm_fs，wb 截断重建 + 回读校验）。
 * @return 写入字节数，<0 失败。
 */
int32_t sdk_file_write(const char *name, const void *in, uint32_t size);

/* ================================================================== */
/* 系统                                                                */
/* ================================================================== */
/** 重启模组（cm_pm_reboot）。 */
void sdk_reboot(void);
/** 上电原因：1=正常上电 2=看门狗/异常复位 0=未知。 */
int  sdk_power_on_reason(void);
/** 获取 IMEI（15 位 + '\0'）。 */
int  sdk_get_imei(char *buf, size_t buf_size);
/** 获取固件版本字符串。 */
int  sdk_get_version(char *buf, size_t buf_size);

/* ================================================================== */
/* AT 通道（cm_virt_at）                                               */
/* ================================================================== */
/**
 * @brief 发送一条 AT 指令并收集应答。
 * @param cmd        例如 "AT+CGDCONT?"（函数内部自动补 "\r\n"）
 * @param resp       应答缓冲（收集到的原始文本，已保证 '\0' 结尾，可为 NULL）
 * @param resp_size  应答缓冲大小
 * @param timeout_ms 等待 "OK"/"ERROR" 的总超时
 * @return >=0 收到 "OK"；-1 超时；-2 收到 "ERROR"；-3 参数/初始化失败。
 *
 * @note SDK 明确要求「每次使用虚拟 AT 都需要初始化和去初始化」，且
 *       「不支持多线程同时操作虚拟 AT」。本函数内部用互斥锁串行化，
 *       但仍然建议只在 net_mgr 任务里调用。
 */
int sdk_at_cmd(const char *cmd, char *resp, size_t resp_size, uint32_t timeout_ms);

/* ================================================================== */
/* 短信（cm_virt_at：发送 + URC 接收）                                  */
/* ================================================================== */
/** URC 回调：每收到一行主动上报文本（如 "+CMT: ,25" 与后续 PDU 行）回调一次。 */
typedef void (*sdk_urc_cb_t)(const char *line);

/**
 * @brief 初始化短信通道：注册 URC 回调，并下发 AT+CMGF=0 / AT+CNMI=2,1,0,0,0。
 * @return 0 成功；<0 失败（URC 注册成功但 AT 配置失败时返回正数告警码）。
 * @note 必须在 cm_opencpu_entry() 里尽早调用，否则开机的第一条短信可能丢失。
 */
int sdk_sms_channel_init(sdk_urc_cb_t cb);

/**
 * @brief 以 PDU 模式发送一条短信（SMS-SUBMIT 十六进制串）。
 *
 * @param pdu_hex PDU 十六进制串，**SMSC 必须已写在 PDU 首部**（sms_concat_build_tx 已带）
 * @return 0 成功；<0 失败
 *
 * @note 第三方参数 `atCmgs` 的官方注释是「AT+CMGS= 之后的参数」，即 PDU 的**八位组长**，
 *       因此这里传 `strlen(pdu_hex)/2`，而不是短信中心号码。
 *       旧实现误把 SCA 号码串当 atCmgs（会拼出 `AT+CMGS=+8613800738500`，非法），已修正。
 *       该点仍属"未上板证实"项，见 docs/research/01-sms-rndis-ping.md 的未证实清单。
 */
int sdk_sms_send_pdu(const char *pdu_hex);

/**
 * @brief 列出**未读**短信（`AT+CMGL=0`，PDU 模式），应答原样写入 resp。
 *
 * 用途：URC（+CMTI）万一没被应用取到，靠周期性轮询兜底，避免短信永久丢失。
 * 因为 `AT+CMGR` 会把未读标记为已读，两条路径天然互斥、不会重复处理。
 *
 * @return >=0 收到 OK；<0 失败
 */
int sdk_sms_list_unread(char *resp, size_t resp_size);

/** 按索引读取一条短信（`AT+CMGR=<index>`，同时把未读标记为已读）。 */
int sdk_sms_read_index(int index, char *resp, size_t resp_size);

/** 按索引删除一条短信（`AT+CMGD=<index>`）。 */
int sdk_sms_delete_index(int index);

/** 已用短信存储条数（`AT+CPMS?` 的第一个已用数）；<0 查询失败。 */
int sdk_sms_storage_used(void);

/* ================================================================== */
/* 网络 / 信号 / PDP                                                   */
/* ================================================================== */
/** 网络是否已注册并附着：1 是，0 否，<0 查询失败。 */
int sdk_net_registered(void);
/** 信号质量 CSQ 0..31；99 未知，-1 查询失败。 */
int sdk_net_csq(void);
/** 读取当前 PDP 的 IPv4 地址，成功返回 0 并写入 ip（至少 16 字节）。 */
int sdk_net_local_ip(char *ip, size_t ip_size);

/* ---- PDP 类型（IPv6 双栈）---- */
#define SDK_PDP_IPV4     "IP"       /**< 仅 IPv4 */
#define SDK_PDP_IPV6     "IPV6"     /**< 仅 IPv6 */
#define SDK_PDP_IPV4V6   "IPV4V6"   /**< 双栈（推荐：一次拿到 v4+v6） */

/**
 * @brief 配置 PDP 上下文（`AT+CGDCONT=1,<type>,"<apn>"`）并触发激活。
 *
 * @param apn      APN 名称（如 cmnet）
 * @param pdp_type SDK_PDP_IPV4 / SDK_PDP_IPV6 / SDK_PDP_IPV4V6（NULL 按 IPV4V6）
 * @return 0 已下发；<0 参数错误
 * @note 3GPP 标准指令，虚拟 AT 官方声明支持。切换 PDP 类型属于"重新激活"，
 *       旧地址会先失效，新的 v4/v6 地址需几秒后才分配下来。
 */
int sdk_net_set_pdp(const char *apn, const char *pdp_type);

/**
 * @brief 一次读出 PDP 的 IPv4 与 IPv6 地址（`AT+CGPADDR=1`）。
 *
 * 应答可能是 `+CGPADDR: 1,"10.1.2.3","2409:8a00::1"`（双栈）、
 * `+CGPADDR: 1,"10.1.2.3"`（纯 v4）或 `+CGPADDR: 1,"","2409:8a00::1"`（纯 v6）。
 *
 * @param v4 输出 IPv4（无则空串，可为 NULL） @param v6 输出 IPv6（无则空串，可为 NULL）
 * @return 0 至少解析出一个地址；<0 失败
 */
int sdk_net_pdp_addrs(char *v4, size_t v4_size, char *v6, size_t v6_size);
/** 去激活 PDP（断网），成功返回 0。 */
int sdk_net_deactivate(void);
/** 域名解析，成功返回 0 并写入点分十进制 IP。 */
int sdk_host_resolve(const char *host, char *ip, size_t ip_size);
/** TCP 连通性探测（心跳用）：可连通返回 0。 */
int sdk_tcp_probe(const char *host, uint16_t port, uint32_t timeout_ms);

/* ================================================================== */
/* 流量统计                                                            */
/* ================================================================== */
/*
 * 重要（链接器实测结论，见 docs/sdk-reference/app-link-symbols.txt）：
 *   用户 APP 的链接边界是 prebuild/objs/ML307A/DSLN/cm_stub_DSLN.o（326 个符号）。
 *   内核 map 里存在、但**不在该边界内**的 lwip_nwst_* / netif_usb_* /
 *   lwip_rndis_hotplug_* / lwip_iflist 全部会导致 undefined reference，
 *   应用层无法调用。
 *   边界内可用于流量统计的是两个平台导出函数：
 *       u32_t get_rx_byte_all(void);  累计接收字节
 *       u32_t get_tx_byte_all(void);  累计发送字节
 *   它们未出现在任何 SDK 头文件中（属未文档化导出），但**链接可用**，
 *   语义需上板实测确认（方法见 docs/05-上板验收清单.md）。
 */
/**
 * @brief 读取平台累计收发字节数（自模组启动/计数清零以来）。
 * @return 0 成功；<0 两个计数都读到 0 且无法判定（视为不可用）。
 */
int sdk_traffic_get_total_bytes(uint64_t *total_rx, uint64_t *total_tx);

/* ================================================================== */
/* USB 流量共享（RNDIS）                                               */
/* ================================================================== */
/*
 * 结论（链接器实测，非推测）：ML307A-DSLN 底包**没有向应用导出任何 USB / RNDIS
 * 相关符号**（cm_stub_DSLN.o 的 326 个符号里 usb/rndis/netif/iflist 全部为 0 命中），
 * 因此应用层**无法运行时开启或关闭 RNDIS**。
 * 处理方式：
 *   - sdk_rndis_apply() 恒返回 -1，usb_rndis 模块据此把策略降级为「重启后生效」，
 *     并在 Web 面板与日志中如实说明。
 *   - RNDIS 的实际开关由底包 / NV 决定；候选的替代路径（AT 命令）与实测步骤
 *     见 docs/05-上板验收清单.md 与 docs/04-需求实现对照与差异说明.md。
 */
/**
 * @brief 尝试应用 RNDIS 开关。
 * @return 恒为 -1（本底包不支持），调用方应据此提示"重启后生效"。
 */
int sdk_rndis_apply(int enable);

/**
 * @brief 查询 RNDIS 网卡是否活动。
 * @return 恒为 -1（本底包未导出查询接口，状态未知）。
 */
int sdk_rndis_is_active(void);

/* ================================================================== */
/* HTTP(S) 客户端（Webhook 推送）                                      */
/* ================================================================== */
/**
 * @brief 同步 POST 一段 JSON，用于企业微信/钉钉 Webhook 推送。
 * @param url        完整 URL，支持 http:// 与 https://
 * @param content_type  Content-Type 头（如 "application/json"）
 * @param body       消息体（UTF-8）
 * @param body_len   消息体长度
 * @param rsp        响应缓冲（可为 NULL）
 * @param rsp_size   响应缓冲大小
 * @return HTTP 状态码（200/204 等）；<0 表示连接/发送失败，具体见返回值：
 *         -1 参数错误 -2 实例创建失败 -3 URL 解析失败 -4 请求失败 -5 超时
 */
int sdk_http_post_json(const char *url, const char *content_type,
                       const char *body, uint32_t body_len,
                       char *rsp, size_t rsp_size);

/**
 * @brief 同步 GET 一段文本（用于查询公网 IP 之类的回显服务）。
 * @param url      完整 URL（http:// 或 https://）
 * @param dns_priority 0=用全局优先级（**本底包开机默认 IPv6 优先**，见 cm_async_dns.h）、
 *                     1=IPv4 优先、2=IPv6 优先
 * @param rsp      响应正文缓冲（可为 NULL）
 * @param rsp_size 缓冲大小
 * @return HTTP 状态码；<0 失败（含义同 sdk_http_post_json）
 * @note 想量"公网 IPv4 出口"必须传 1，否则请求可能走 IPv6 出口，拿回来的是 IPv6 地址。
 */
int sdk_http_get_text(const char *url, uint8_t dns_priority, char *rsp, size_t rsp_size);

/* ================================================================== */
/* TCP 服务端（内置 Web 管理面板）                                     */
/* ================================================================== */
/** 创建监听套接字，成功返回 fd（>=0）。 */
int  sdk_tcp_listen(uint16_t port);
/** 接受连接，超时返回 -1，出错返回 -2。 */
int  sdk_tcp_accept(int listen_fd, uint32_t timeout_ms);
/** 接收数据，返回 >0 字节数 / 0 对端关闭 / -1 超时 / -2 出错。 */
int  sdk_tcp_recv(int fd, void *buf, size_t len, uint32_t timeout_ms);
/** 发送全部数据，返回实际发送字节数。 */
int  sdk_tcp_send(int fd, const void *buf, size_t len);

/* ================================================================== */
/* 异步 socket 服务端（★ 正确路径）                                    */
/*                                                                     */
/* 内核的 socket 必须在 eloop 线程内创建与收发（见 SDK 官方 asocket 示例）*/
/* 因此服务端不能用"任务里 accept 循环"的写法，而应注册回调：           */
/*   sdk_srv_start(port, on_client)；                                   */
/* 内核在新连接到达时，会在 **eloop 线程**内调用 on_client(cli_fd)，     */
/* 回调里可以同步 recv/send（也允许调用 sdk_at_cmd 等），返回后连接关闭。*/
/* ================================================================== */
/** 客户端连接回调（运行在 eloop 线程；返回后该连接被关闭）。 */
typedef void (*sdk_srv_client_cb_t)(int cli_fd);

/**
 * @brief 在指定端口启动异步监听（内部完成 cm_asocket_open + bind + listen）。
 * @return 0 成功（异步，监听结果见 sdk_srv_listening）；<0 失败。
 */
int  sdk_srv_start(uint16_t port, sdk_srv_client_cb_t on_client);

/**
 * @brief 同上，但可指定地址族：family=2 为 AF_INET，10 为 AF_INET6。
 *
 * 为什么要 IPv6 监听：2026-10-15 发现原实现只开了 AF_INET 监听，
 * 因此"通过 IPv6 访问 Web 面板"在原理上就不可能成功（应用根本没有 v6 监听）。
 * IPv6 版本绑 `::`（全零，不依赖 lwip 的 in6addr_any 导出符号）。
 *
 * @return 0 已请求创建（结果见诊断上报 srv<port>[v6] bind/listen）；<0 失败
 */
int  sdk_srv_start_family(uint16_t port, sdk_srv_client_cb_t on_client, int family);

/** 该端口是否已进入监听状态（1=是）。 */
int  sdk_srv_listening(uint16_t port);

/**
 * @brief 创建并启动事件循环（eloop）线程，供 asocket 与异步动作使用。
 *
 * 官方示例 examples/asocket 要求应用自己 cm_eloop_create + 起线程执行
 * cm_eloop_wait_event，并用 cm_asocket_init(eloop) 绑定给 asocket 模块；
 * 只调 cm_asocket_open 而不跑 eloop，事件与监听都不会生效。
 * sdk_srv_start() 会自动调用本函数，通常无需单独调用。
 * @return 0 成功；<0 失败。
 */
int  sdk_eloop_start(void);

/**
 * @brief 诊断上报：把一行文本 POST 到 PC 侧监听端口（默认 192.168.0.100:9090）。
 *
 * 用内核侧同步 HTTP 客户端发送，**不依赖**应用自己的 socket 监听，
 * 因此适合用于诊断"本机监听为什么没建立"这类循环依赖问题。
 * PC 侧用 python -m http.server 9090 或 tools/diag_listen.ps1 即可接收。
 */
void sdk_diag_post(const char *fmt, ...);

/**
 * @brief 诊断码：监听建立过程中最先失败的环节。
 *   1=asocket 拿不到 eloop  2=监听事件未被提交/执行  21=asocket_open 失败
 *   22=bind 失败  23=listen 失败  30=监听成功  0=全部正常
 */
int  sdk_diag_code(void);

/**
 * @brief 复位时序上报：诊断码非 0 时，延时 (3+code) 秒后主动重启（最多 6 次）。
 *
 * 用于"没有 UART 日志、又没有联网"时的兜底观测：PC 侧测量 USB 上下线周期
 * 即可反推诊断码。诊断码为 0（一切正常）时本函数立即返回，不影响运行。
 */
void sdk_diag_beacon(void);
/** 关闭连接。 */
void sdk_tcp_close(int fd);

#endif /* SDK_PORT_H */

