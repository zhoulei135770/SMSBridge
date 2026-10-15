/**
 * @file    net_mgr.h
 * @brief   网络管理：APN 自动轮询切换、心跳保活、网络状态维护（需求 3.1，文档第三节 1）。
 *
 * 说明：
 *   - 本模块是**唯一**允许发起 AT 指令的业务任务（SDK 明确虚拟 AT 不支持多线程）。
 *   - 心跳不依赖 ICMP：SDK 无 PING API，AT+MPING 属厂商私有指令且虚拟 AT 官方声明
 *     "仅支持 3GPP 指令"，能否使用未证实。因此心跳采用 **TCP 连通性探测**
 *     （sdk_tcp_probe），它验证的是我们真正需要的端到端数据通路。
 */
#ifndef NET_MGR_H
#define NET_MGR_H

#include <stdint.h>
#include "app_config.h"
#include "net_addr.h"

typedef struct {
    uint8_t  registered;        /* 1=已注册附着 */
    uint8_t  online;            /* 1=心跳通过，判定网络可用 */
    uint8_t  heartbeat_ok;      /* 最近一次心跳结果 */
    uint8_t  apn_index;         /* 当前 APN 下标 */
    char     apn[16];           /* 当前 APN 名称 */
    int      csq;               /* 0..31，99 未知，-1 查询失败 */
    char     ip[NET_ADDR_V4_MAX];   /* PDP 的 IPv4 地址，可能为空 */
    char     ip6[NET_ADDR_V6_MAX];  /* PDP 的 IPv6 全局地址（双栈/纯 v6 时） */
    char     pub_ip[NET_ADDR_MAX];  /* 回显服务查到的**公网 IPv4 出口**（已强制 v4 优先） */
    char     pub_ip6[NET_ADDR_MAX]; /* 回显服务看到的**IPv6 出口**（若请求走了 v6 才会填） */
    char     pdp_type[8];       /* 当前生效的 PDP 类型："IP"/"IPV4V6"/"IPV6" */
    uint8_t  ipv6_on;           /* 1=当前确实拿到了 IPv6 地址 */
    uint8_t  ipv6_enable;       /* 生效中的配置开关（与 ipv6_on 可能不同） */
    uint8_t  pubip_tried;       /* 1=至少查询过一次公网 IP */
    uint8_t  pubip_ok;          /* 1=最近一次公网 IP 查询成功 */
    uint32_t pubip_uptime_ms;   /* 最近一次**成功**查询的 uptime(ms) */
    uint32_t fail_count;        /* 连续心跳失败次数 */
    uint32_t apn_switch_count;  /* 开机以来 APN 切换次数 */
    uint32_t heartbeat_count;   /* 累计心跳次数 */
    uint32_t last_heartbeat_ms; /* 上次心跳的 uptime(ms) */
    uint32_t next_heartbeat_ms; /* 下次心跳的 uptime(ms) */
} net_status_t;

/** 初始化（记录起始状态，不阻塞）。 */
void net_mgr_init(app_config_t *cfg);

/** 网络管理任务主体（由 main 创建）。 */
void net_mgr_task(void *arg);

/** 读取状态快照（线程安全）。 */
void net_mgr_status(net_status_t *st);

/** Web 面板【一键切换到下一个 APN】：切 APN + 重新激活 + 落盘，返回新下标。 */
int net_mgr_switch_apn(void);

/** 手动触发一次心跳（Web 或调试用），返回 1 成功 / 0 失败。 */
int net_mgr_ping_now(void);

/**
 * @brief 【实时开启/关闭 IPv6】：写配置 + **立即**用新的 PDP 类型重新激活。
 *
 * - `enable=1` -> `AT+CGDCONT=1,"IPV4V6","<apn>"`（双栈，v4/v6 同时可用）
 * - `enable=0` -> `AT+CGDCONT=1,"IP","<apn>"`
 *
 * 异步执行：只置标志并唤醒 net_mgr 任务（AT 只能由 net_mgr 发起），
 * 因此本函数立即返回；新的地址需几秒后通过状态快照观察。
 * @return 0 已受理；<0 参数错误
 */
int net_mgr_set_ipv6(int enable);

/** 【立即查询公网 IP】：异步触发一次回显服务查询（同样由 net_mgr 任务执行）。 */
int net_mgr_refresh_pubip(void);

#endif /* NET_MGR_H */
