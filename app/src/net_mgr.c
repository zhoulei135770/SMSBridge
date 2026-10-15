/**
 * @file    net_mgr.c
 * @brief   网络管理实现：APN 轮询 + 心跳保活。
 */
#include "net_mgr.h"
#include "sdk_port.h"
#include "app_log.h"
#include "app_ctx.h"

#include <string.h>
#include <stdio.h>

static net_status_t s_st;
static sdk_mutex_t  s_lock;
static sdk_sem_t    s_ping_req;      /* Web 手动触发心跳 */
static volatile int s_force_apn_switch;
static volatile int s_ping_now;
static volatile int s_ipv6_req = -1; /* -1 无请求；0 关闭；1 开启（实时应用） */
static volatile int s_pubip_now;     /* Web 手动触发公网 IP 查询 */
static uint32_t     s_ipv6_last_apply_ms;  /* 上次切换 PDP 类型的时刻（限速） */

/** 公网 IP 查询间隔：10 分钟（回显服务是外网资源，别打太勤） */
#define PUBIP_INTERVAL_MS   600000u
/** 公网 IP 应答缓冲（静态，避免占 3072 的任务栈） */
#define PUBIP_RSP_MAX       256u
/** IPv6 开关最小间隔：PDP 反复重建会招来运营商去附着/限速 */
#define IPV6_TOGGLE_MIN_MS  15000u

/* ================================================================== */
/* 内部：状态更新（带锁）                                              */
/* ================================================================== */
static void st_lock(void)
{
    if (s_lock == NULL) s_lock = sdk_mutex_new();
    (void)sdk_mutex_lock(s_lock, 2000u);
}

static void st_unlock(void)
{
    (void)sdk_mutex_unlock(s_lock);
}

void net_mgr_status(net_status_t *st)
{
    if (st == NULL) return;
    st_lock();
    *st = s_st;
    st_unlock();
}

/* ================================================================== */
/* 内部：APN 切换                                                      */
/* ================================================================== */
/* ================================================================== */
/* 内部：PDP 类型与 APN 应用                                           */
/* ================================================================== */
/** 依据配置决定 PDP 类型：开启 IPv6 用双栈，否则纯 IPv4。 */
static const char *pdp_type_of(const app_config_t *cfg)
{
    return (cfg != NULL && cfg->ipv6_enable != 0u) ? SDK_PDP_IPV4V6 : SDK_PDP_IPV4;
}

static void apply_apn(const char *apn, int do_save)
{
    const char *type;
    int         v6;

    if (apn == NULL) return;

    app_ctx_lock();
    type = pdp_type_of(&g_cfg);
    v6   = (g_cfg.ipv6_enable != 0u) ? 1 : 0;
    LOG_I("net", "正在激活 PDP：APN=%s 类型=%s（IPv6 %s）", apn, type, v6 ? "开" : "关");
    (void)sdk_net_set_pdp(apn, type);
    if (do_save) {
        (void)app_ctx_save();
    }
    app_ctx_unlock();

    st_lock();
    (void)snprintf(s_st.pdp_type, sizeof(s_st.pdp_type), "%s", type);
    s_st.ipv6_enable = (uint8_t)v6;
    st_unlock();
}

static int switch_to_next_apn(const char *why)
{
    const char *next;

    app_ctx_lock();
    (void)app_config_apn_next(&g_cfg);
    next = app_config_apn(&g_cfg);
    s_st.apn_index = (uint8_t)g_cfg.apn_index;
    (void)snprintf(s_st.apn, sizeof(s_st.apn), "%s", next);
    ++s_st.apn_switch_count;
    app_ctx_unlock();

    LOG_W("net", "网络异常（%s），自动切换 APN -> %s（第 %lu 次）",
          (why != NULL) ? why : "unknown", s_st.apn,
          (unsigned long)s_st.apn_switch_count);

    apply_apn(next, 1);
    return 0;
}

int net_mgr_switch_apn(void)
{
    s_force_apn_switch = 1;
    return 0;
}

int net_mgr_ping_now(void)
{
    s_ping_now = 1;
    sdk_sem_post(s_ping_req);
    return 1;
}

int net_mgr_set_ipv6(int enable)
{
    s_ipv6_req = (enable != 0) ? 1 : 0;
    sdk_sem_post(s_ping_req);        /* 唤醒任务，立刻处理 */
    return 0;
}

int net_mgr_refresh_pubip(void)
{
    s_pubip_now = 1;
    sdk_sem_post(s_ping_req);
    return 0;
}

/* ================================================================== */
/* 内部：公网 IP 查询（HTTP 回显服务）                                 */
/* ================================================================== */
/**
 * @brief 向配置里的回显服务查一次公网出口地址。
 *
 * ★ 必须显式指定 DNS 优先级：本底包 `cm_async_dns.h` 原文是"开机默认 v6 优先"，
 *   若用全局优先级，请求可能走 IPv6 出口，量回来的是 IPv6 地址而不是公网 IPv4。
 *   这里用 1（v4 优先）查公网 IPv4；若返回的仍是 IPv6，就把它记到 pub_ip6 里并告警，
 *   而不是冒充成 IPv4。
 *
 * @note 阻塞调用（cm_httpclient 同步接口，不要求调用者在 eloop 线程），由 net_mgr 任务串行执行。
 */
static void do_pubip_query(const app_config_t *cfg)
{
    static char rsp[PUBIP_RSP_MAX];
    char        ip[NET_ADDR_MAX];
    int         code;

    if (cfg == NULL || cfg->pubip_url[0] == '\0') return;

    rsp[0] = '\0';
    code = sdk_http_get_text(cfg->pubip_url, 1u /* IPv4 优先 */, rsp, sizeof(rsp));

    st_lock();
    s_st.pubip_tried = 1u;
    st_unlock();

    if (code == 200 && net_addr_extract_echo(rsp, ip, sizeof(ip)) == 0) {
        int is_v6 = net_addr_is_ipv6(ip);

        st_lock();
        if (is_v6) {
            (void)snprintf(s_st.pub_ip6, sizeof(s_st.pub_ip6), "%s", ip);
            s_st.pub_ip[0] = '\0';
            s_st.pubip_ok  = 0u;        /* IPv4 出口没量到，如实标记失败 */
        } else {
            (void)snprintf(s_st.pub_ip, sizeof(s_st.pub_ip), "%s", ip);
            s_st.pubip_ok        = 1u;
            s_st.pubip_uptime_ms = sdk_uptime_ms();
        }
        st_unlock();

        if (is_v6) {
            LOG_W("net", "回显服务返回的是 IPv6 出口 %s（已强制 v4 优先仍走 v6，"
                         "可能该域名只有 AAAA 记录），公网 IPv4 未取到", ip);
        } else {
            LOG_I("net", "公网 IPv4 出口（来自 %s）：%s", cfg->pubip_url, ip);
        }
    } else {
        st_lock();
        s_st.pubip_ok = 0u;
        st_unlock();
        LOG_W("net", "公网 IP 查询失败（HTTP %d，应答 %.40s）", code, rsp);
    }
}

/**
 * @brief 重新激活 PDP 后等待地址出现（最多约 15 秒）。
 * @param want_v6 1 = 等 IPv6；0 = 等 IPv4
 * @return 1 已拿到；0 超时
 */
static int wait_for_addr(int want_v6)
{
    int i;

    for (i = 0; i < 5; ++i) {
        char v4[NET_ADDR_V4_MAX];
        char v6[NET_ADDR_V6_MAX];

        sdk_task_sleep_ms(3000u);
        v4[0] = '\0';
        v6[0] = '\0';
        if (sdk_net_pdp_addrs(v4, sizeof(v4), v6, sizeof(v6)) == 0) {
            if ((want_v6 && v6[0] != '\0') || (!want_v6 && v4[0] != '\0')) {
                return 1;
            }
        }
    }
    return 0;
}

/* ================================================================== */
/* 内部：一次心跳                                                      */
/* ================================================================== */
static int do_heartbeat(const app_config_t *cfg)
{
    int ok;
    uint32_t t0 = sdk_uptime_ms();

    if (cfg == NULL || cfg->ping_target[0] == '\0') return 0;

    ok = (sdk_tcp_probe(cfg->ping_target, cfg->ping_port, 5000u) == 0);

    LOG_I("net", "心跳 %s:%u -> %s（耗时 %lu ms）",
          cfg->ping_target, (unsigned)cfg->ping_port,
          ok ? "可达" : "不可达",
          (unsigned long)(sdk_uptime_ms() - t0));

    st_lock();
    s_st.heartbeat_ok = (uint8_t)(ok ? 1 : 0);
    ++s_st.heartbeat_count;
    s_st.last_heartbeat_ms = sdk_uptime_ms();
    if (ok) {
        s_st.fail_count = 0u;
        s_st.online = 1u;
    } else {
        ++s_st.fail_count;
        s_st.online = 0u;
    }
    st_unlock();

    return ok;
}

/* ================================================================== */
/* 初始化                                                              */
/* ================================================================== */
void net_mgr_init(app_config_t *cfg)
{
    memset(&s_st, 0, sizeof(s_st));
    s_lock = sdk_mutex_new();
    s_ping_req = sdk_sem_new(0u);

    if (cfg != NULL) {
        s_st.apn_index = (uint8_t)cfg->apn_index;
        (void)snprintf(s_st.apn, sizeof(s_st.apn), "%s", app_config_apn(cfg));
        s_st.csq = -1;
        s_st.next_heartbeat_ms = sdk_uptime_ms() + 10000u;   /* 开机 10 秒后首次心跳 */
    }
    LOG_I("net", "网络管理启动：APN=%s 心跳目标=%s:%u 周期=%lu ms",
          s_st.apn,
          (cfg != NULL) ? cfg->ping_target : "?",
          (unsigned)((cfg != NULL) ? cfg->ping_port : 0u),
          (unsigned long)((cfg != NULL) ? cfg->heartbeat_ms : 0u));
}

/* ================================================================== */
/* 任务主体                                                            */
/* ================================================================== */
void net_mgr_task(void *arg)
{
    app_config_t cfg;
    uint32_t     last_ip_check = 0u;
    uint32_t     ip_was_valid  = 0u;
    uint32_t     ip6_was_valid = 0u;

    (void)arg;

    /* 上电后先按当前 APN + PDP 类型激活一次 */
    app_ctx_lock();
    cfg = g_cfg;
    app_ctx_unlock();
    apply_apn(app_config_apn(&cfg), 0);

    for (;;) {
        uint32_t now = sdk_uptime_ms();
        int      need_switch = 0;
        const char *why = NULL;

        /* ---- 1) 取配置快照并处理 Web 下发的立即切换/立即心跳请求 ---- */
        app_ctx_lock();
        cfg = g_cfg;
        app_ctx_unlock();

        if (s_force_apn_switch) {
            s_force_apn_switch = 0;
            switch_to_next_apn("Web 手动切换");
            sdk_task_sleep_ms(3000);
            continue;
        }

        /* ---- 1b) 【实时开启/关闭 IPv6】：写配置 + 立刻用新 PDP 类型重新激活 ---- */
        if (s_ipv6_req >= 0) {
            int want = s_ipv6_req;
            s_ipv6_req = -1;

            /* 限速：PDP 反复重建会被运营商去附着，15 秒内不接受第二次切换 */
            if ((now - s_ipv6_last_apply_ms) < IPV6_TOGGLE_MIN_MS) {
                LOG_W("net", "IPv6 开关请求过于频繁（距上次 %lu ms），已忽略",
                      (unsigned long)(now - s_ipv6_last_apply_ms));
                continue;
            }
            s_ipv6_last_apply_ms = now;

            app_ctx_lock();
            if ((int)g_cfg.ipv6_enable != want) {
                g_cfg.ipv6_enable = (uint8_t)want;
                (void)app_ctx_save();
                LOG_I("net", "IPv6 已%s（配置已落盘，立即重新激活 PDP）", want ? "开启" : "关闭");
            } else {
                LOG_I("net", "IPv6 已经是%s状态，仍重新激活一次 PDP", want ? "开启" : "关闭");
            }
            app_ctx_unlock();

            apply_apn(app_config_apn(&cfg), 0);
            /* 轮询等地址出现（最多约 15 秒），比固定 sleep 更可靠 */
            LOG_I("net", "等待 %s 地址分配…", want ? "IPv6" : "IPv4");
            if (wait_for_addr(want)) {
                LOG_I("net", "%s 地址已就绪", want ? "IPv6" : "IPv4");
            } else {
                LOG_W("net", "重新激活后 15 秒内未拿到 %s 地址，后续轮询会继续更新",
                      want ? "IPv6" : "IPv4");
            }
            continue;
        }

        (void)sdk_sem_wait(s_ping_req, 0u);   /* 清掉已挂起的信号 */
        if (s_ping_now) {
            s_ping_now = 0;
            (void)do_heartbeat(&cfg);
        }

        /* ---- 2) 注册状态 + 信号质量 + 双栈地址（每 5 秒查一次，避免频繁占用 AT） ---- */
        if ((now - last_ip_check) >= 5000u) {
            int reg = sdk_net_registered();
            int csq = sdk_net_csq();
            char ip4[NET_ADDR_V4_MAX];
            char ip6[NET_ADDR_V6_MAX];

            last_ip_check = now;
            ip4[0] = '\0';
            ip6[0] = '\0';
            (void)sdk_net_pdp_addrs(ip4, sizeof(ip4), ip6, sizeof(ip6));

            st_lock();
            s_st.registered = (uint8_t)((reg == 1) ? 1 : 0);
            s_st.csq        = csq;
            (void)snprintf(s_st.ip, sizeof(s_st.ip), "%s", ip4);
            (void)snprintf(s_st.ip6, sizeof(s_st.ip6), "%s", ip6);
            s_st.ipv6_on = (uint8_t)((ip6[0] != '\0') ? 1 : 0);
            st_unlock();

            /* 首次拿到地址时打印访问入口（IPv4 优先，因为浏览器多半走 v4） */
            if (ip4[0] != '\0' && !ip_was_valid) {
                LOG_I("net", "PDP 已就绪，IPv4=%s（Web 面板: http://%s:8080）", ip4, ip4);
                ip_was_valid = 1u;
            } else if (ip4[0] == '\0' && ip_was_valid) {
                LOG_W("net", "PDP 的 IPv4 地址丢失");
                ip_was_valid = 0u;
            }
            if (ip6[0] != '\0' && !ip6_was_valid) {
                LOG_I("net", "PDP 已获得 IPv6 全局地址：%s", ip6);
                ip6_was_valid = 1u;
            } else if (ip6[0] == '\0' && ip6_was_valid) {
                LOG_W("net", "PDP 的 IPv6 地址丢失");
                ip6_was_valid = 0u;
            }

            LOG_I("net", "网络状态：注册=%s CSQ=%d IPv4=%s IPv6=%s 在线=%s",
                  (reg == 1) ? "是" : "否", csq,
                  (ip4[0] != '\0') ? ip4 : "(无)",
                  (ip6[0] != '\0') ? ip6 : "(无)",
                  s_st.online ? "是" : "否");

            /* ---- 2b) 公网出口地址：配置开启时按 10 分钟周期（或 Web 手动）查询 ---- */
            if (cfg.pubip_enable != 0u && cfg.pubip_url[0] != '\0' &&
                (ip4[0] != '\0' || ip6[0] != '\0')) {
                int due = (s_st.pubip_uptime_ms == 0u) ||
                          ((now - s_st.pubip_uptime_ms) >= PUBIP_INTERVAL_MS);
                if (s_pubip_now || due) {
                    s_pubip_now = 0;
                    do_pubip_query(&cfg);
                }
            } else {
                s_pubip_now = 0;
            }
        }

        /* ---- 3) 未注册 / 无 IP 的处理 ---- */
        if (s_st.registered == 0) {
            /* 需求 3.1：网络失效自动切换下一个 APN。注册失败要连续多次才切，
             * 避免刚上电搜网阶段就被切走。 */
            if (s_st.fail_count >= cfg.heartbeat_fail_limit) {
                need_switch = 1;
                why = "未注册/未附着";
            } else {
                ++s_st.fail_count;
                LOG_W("net", "尚未注册网络（第 %lu 次探测）",
                      (unsigned long)s_st.fail_count);
            }
        } else if (s_st.ip[0] == '\0' && s_st.ip6[0] == '\0') {
            /* 已注册但 v4/v6 都没有：重新激活一次 PDP（双栈下只要有一个就算就绪） */
            LOG_W("net", "已注册但无 PDP 地址，重新激活");
            (void)sdk_net_set_pdp(app_config_apn(&cfg), pdp_type_of(&cfg));
            sdk_task_sleep_ms(3000);
            continue;
        } else {
            s_st.fail_count = 0u;
        }

        /* ---- 4) 周期心跳 ---- */
        if (now >= s_st.next_heartbeat_ms) {
            st_lock();
            s_st.next_heartbeat_ms = now + cfg.heartbeat_ms;
            st_unlock();

            if (!do_heartbeat(&cfg)) {
                if (s_st.fail_count >= cfg.heartbeat_fail_limit) {
                    need_switch = 1;
                    why = "心跳连续失败";
                }
            }
        }

        /* ---- 5) 执行 APN 切换 ---- */
        if (need_switch) {
            switch_to_next_apn(why);
            st_lock();
            s_st.fail_count = 0u;
            st_unlock();
            sdk_task_sleep_ms(5000);
            continue;
        }

        sdk_task_sleep_ms(1000u);
    }
}
