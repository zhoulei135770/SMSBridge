/**
 * @file    sms_svc.c
 * @brief   短信接收服务实现。
 */
#include "sms_svc.h"
#include "sdk_port.h"
#include "app_log.h"
#include "app_ctx.h"
#include "webhook.h"
#include "sms_admin.h"       /* 管理员指令路由（纯逻辑） */
#include "sms_concat.h"      /* 长短信重组 / 回执分包 */
#include "sms_rcfg.h"        /* 远程配置持久化（rcfg.bin） */
#include "app_time.h"
#include "net_mgr.h"        /* net_mgr_status / sdk_rndis_is_active */
#include "traffic.h"        /* traffic_get_status（面板内容用） */        /* 时钟可信判定与毫秒时间戳（钉钉加签前提） */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>      /* strtol（CMGL 应答里取索引） */

/* ================================================================== */
/* URC 行队列（URC 回调 -> 服务任务）                                   */
/* ================================================================== */
#define SMS_LINE_Q      12u
#define SMS_LINE_MAX    512u   /* 长短信 +CMT 的 PDU 行可达 350+ 字符，必须放得下 */

static char      s_q[SMS_LINE_Q][SMS_LINE_MAX];
static uint8_t   s_q_head;
static uint8_t   s_q_tail;
static uint8_t   s_q_count;
static sdk_mutex_t s_q_lock;
static sdk_sem_t   s_q_sem;
static volatile uint32_t s_q_drop;

/* ================================================================== */
/* 记录环形缓存（供 Web 展示与导出）                                    */
/* ================================================================== */
static sms_record_t s_rec[SMS_RECORD_NUM];
static uint8_t      s_rec_w;        /* 下一个写入位置 */
static uint8_t      s_rec_count;
static sdk_mutex_t  s_rec_lock;

static sms_stat_t   s_stat;

/* ================================================================== */
/* 远程配置（rcfg.bin）与短信中心（SMSC）缓存                          */
/* ================================================================== */
/*
 * 只有短信任务会读写 s_rcfg（启动时载入一次，之后收到 SAVE/指令时更新），
 * 因此不需要额外加锁；Web 面板若要只读展示，应另走 sms_svc_rcfg_snapshot()。
 */
static sms_remote_cfg_t s_rcfg;
static int              s_rcfg_loaded;      /* 1 = rcfg.bin 有效，0 = 用默认值 */
static sdk_mutex_t      s_rcfg_lock;        /* 保护 s_rcfg：短信任务写，Web 任务只读快照 */
static char             s_sca[24];          /* AT+CSCA? 读到的短信中心（含 '+'），空 = 用模组默认 */
static uint32_t         s_sca_next_try_ms;  /* 下次允许重读 AT+CSCA? 的时刻（开机未入网时限速重试） */
static char             s_view_net[40];     /* 状态视图用：网络（静态，视图只持有指针） */
static char             s_view_sig[8];      /* 状态视图用：信号 */
static char             s_view_ntp[64];     /* 状态视图用：对时状态 */
static char             s_view_ip4[24];     /* 状态视图用：PDP IPv4 */
static char             s_view_ip6[48];     /* 状态视图用：PDP IPv6 全局地址 */

/* 回执发送缓冲：8 片 × 640 十六进制字符 ≈ 5.1KB，放静态区避免爆栈 */
static sms_tx_pkt_t     s_tx_pkts[SMS_TX_MAX_PARTS];

/* ================================================================== */
/* URC 行入队（在 URC 回调线程执行，必须极快）                          */
/* ================================================================== */
static void queue_line(const char *line, size_t len)
{
    if (s_q_lock == NULL || len == 0u) return;
    if (len >= SMS_LINE_MAX) len = SMS_LINE_MAX - 1u;

    if (sdk_mutex_lock(s_q_lock, 100u) != 0) {
        ++s_q_drop;
        return;
    }
    if (s_q_count >= SMS_LINE_Q) {
        ++s_q_drop;                 /* 队列满：丢弃最旧策略太慢，这里直接丢新并计数 */
        (void)sdk_mutex_unlock(s_q_lock);
        return;
    }
    memcpy(s_q[s_q_tail], line, len);
    s_q[s_q_tail][len] = '\0';
    s_q_tail = (uint8_t)((s_q_tail + 1u) % SMS_LINE_Q);
    ++s_q_count;
    (void)sdk_mutex_unlock(s_q_lock);

    sdk_sem_post(s_q_sem);
}

static int queue_pop(char *out, size_t out_size)
{
    int ok = 0;

    if (s_q_lock == NULL) return 0;
    if (sdk_mutex_lock(s_q_lock, 100u) != 0) return 0;

    if (s_q_count > 0u) {
        (void)snprintf(out, out_size, "%s", s_q[s_q_head]);
        s_q_head = (uint8_t)((s_q_head + 1u) % SMS_LINE_Q);
        --s_q_count;
        ok = 1;
    }
    (void)sdk_mutex_unlock(s_q_lock);
    return ok;
}

void sms_svc_on_urc(const char *line)
{
    const char *p;
    const char *start;

    if (line == NULL) return;

    /* SDK 可能一次给出多行（用 \r\n 分隔），这里拆行后逐行入队 */
    p     = line;
    start = line;
    while (*p != '\0') {
        if (*p == '\r' || *p == '\n') {
            if (p > start) {
                queue_line(start, (size_t)(p - start));
            }
            start = p + 1;
        }
        ++p;
    }
    if (p > start) {
        queue_line(start, (size_t)(p - start));
    }
}

/* ================================================================== */
/* 工具                                                                */
/* ================================================================== */
static int is_hex_pdu(const char *s, size_t *out_len)
{
    size_t n = 0;

    if (s == NULL) return 0;
    while (*s != '\0') {
        char c = *s++;
        if (c == ' ' || c == ':' || c == ',') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
        ++n;
    }
    if (n < 20u || (n % 2u) != 0u) return 0;
    if (out_len != NULL) *out_len = n;
    return 1;
}

/** 从 +CMTI: "SM",12 里取出索引。 */
static int parse_cmti_index(const char *line)
{
    const char *p = strchr(line, ',');

    if (p == NULL) return -1;
    ++p;
    while (*p == ' ' || *p == '"') ++p;
    if (*p < '0' || *p > '9') return -1;
    {
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            ++p;
        }
        return v;
    }
}

/** 有界拷贝：截断但不越界（比 snprintf 更省，也避免编译器的截断告警）。 */
static void copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0u) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/** 存入环形缓存。 */
static void store_record(const sms_record_t *rec)
{
    if (s_rec_lock == NULL) return;
    if (sdk_mutex_lock(s_rec_lock, 500u) != 0) return;
    s_rec[s_rec_w] = *rec;
    s_rec_w = (uint8_t)((s_rec_w + 1u) % SMS_RECORD_NUM);
    if (s_rec_count < SMS_RECORD_NUM) ++s_rec_count;
    (void)sdk_mutex_unlock(s_rec_lock);
}

/* ================================================================== */
/* 时间同步（钉钉加签的前提）                                          */
/* ================================================================== */
static uint32_t s_time_next_try_ms;      /* 下次允许尝试对时的时刻 */

/**
 * @brief 尝试对时：先问模组自己的时钟（AT+CCLK?，入网即有），再异步发起 NTP。
 * @note 服务任务每秒调用一次；时钟已可信或未到重试窗口时直接返回（不做 AT）。
 */
static void time_sync_try(void)
{
    if (app_time_is_valid(sdk_utc_seconds())) return;
    if (sdk_uptime_ms() < s_time_next_try_ms) return;
    s_time_next_try_ms = sdk_uptime_ms() + 60000u;      /* 失败最多每分钟重试一次 */

    if (sdk_time_sync_from_modem() == 0) {
        char t[40];
        LOG_I("sms", "已用模组时钟对时：%s",
              (sdk_local_time_str(t, sizeof(t)) == 0) ? t : "(已写入 RTC)");
        return;
    }

    if (sdk_time_sync_state() == 0) return;             /* 上一次 NTP 还在进行中 */

    if (sdk_time_sync_start(s_rcfg.ntp) == 0) {
        LOG_I("sms", "已提交 NTP 对时请求（服务器=%s）",
              (s_rcfg.ntp[0] != '\0') ? s_rcfg.ntp : "SDK 默认");
    } else {
        LOG_W("sms", "NTP 对时提交失败（时钟不可信时钉钉加签将降级）");
    }
}

/** 供启动流程在入网后主动触发一次（避免干等重试窗口）。 */
void sms_svc_time_sync_try(void)
{
    time_sync_try();
}

/* ================================================================== */
/* 短信中心（SMSC）与状态视图                                          */
/* ================================================================== */
/**
 * @brief 读取 `AT+CSCA?` 并缓存短信中心号码。
 * @note SDK 明文要求"发送 PDU 格式短信时，信息编码中要带 SMSC"，
 *       而 sms_concat_build_tx() 会把号码写进 PDU 首部；读不到就用模组默认（PDU 里写 00）。
 */
static void sca_refresh(void)
{
    static char resp[192];

    s_sca[0]           = '\0';
    s_sca_next_try_ms  = sdk_uptime_ms() + 60000u;   /* 失败后至少 60 秒才重试 */
    if (sdk_at_cmd("AT+CSCA?", resp, sizeof(resp), 5000u) < 0) {
        LOG_W("sms", "AT+CSCA? 无响应，回执将使用模组默认短信中心");
        return;
    }
    if (sms_concat_parse_csca(resp, s_sca, sizeof(s_sca)) != 0) {
        LOG_W("sms", "AT+CSCA? 未返回有效号码（%.60s），使用模组默认", resp);
    }
}

/** 需要发短信前确保有短信中心：为空且已过重试窗口时再读一次。 */
static void sca_ensure(void)
{
    if (s_sca[0] != '\0') return;
    if (sdk_uptime_ms() < s_sca_next_try_ms) return;
    sca_refresh();
}

/**
 * @brief 组装状态视图（菜单/状态回执用）。
 * @param rc 远程配置**快照**（由调用方先取副本，避免持锁期间做 AT）
 * @note 内部会查网络注册与信号（各一条 AT，最长几秒）；只在处理管理员短信时调用。
 */
static void build_view(const sms_remote_cfg_t *rc, sms_cfg_view_t *v)
{
    int reg;
    int csq;

    if (v == NULL) return;

    reg = sdk_net_registered();
    if (reg == 1)       (void)snprintf(s_view_net, sizeof(s_view_net), "%s", "已注册");
    else if (reg == 0)  (void)snprintf(s_view_net, sizeof(s_view_net), "%s", "未注册");
    else                (void)snprintf(s_view_net, sizeof(s_view_net), "%s", "查询失败");

    csq = sdk_net_csq();
    if (csq >= 0) (void)snprintf(s_view_sig, sizeof(s_view_sig), "%d", csq);
    else          s_view_sig[0] = '\0';

    /*
     * 对时状态：决定钉钉加签能否使用（docs/09 §4.3）。
     * ★ 只放"已对时/未对时"，**不带时间戳**：整条 STATUS 回执必须 ≤70 个 UCS2 字符
     *   才能一条短信发完（本底包 cm_virt_at_sms_send 实测每次开机只有第一次调用成功，
     *   见 app/src/sdk_port.c 的发送注释）。时间戳请用 Web 面板查看。
     */
    if (app_time_is_valid(sdk_utc_seconds())) {
        (void)snprintf(s_view_ntp, sizeof(s_view_ntp), "%s", "已对时");
    } else if (sdk_time_sync_state() < 0) {
        (void)snprintf(s_view_ntp, sizeof(s_view_ntp), "%s", "对时失败");
    } else {
        (void)snprintf(s_view_ntp, sizeof(s_view_ntp), "%s", "未对时");
    }

    sms_rcfg_make_view(rc, s_view_net, s_view_sig, s_view_ntp,
                       s_stat.received, s_stat.forwarded, v);
    /* PDP 上的 IPv4/IPv6（一条 AT+CGPADDR=1 同时拿到两个地址） */
    (void)sdk_net_pdp_addrs(s_view_ip4, sizeof(s_view_ip4), s_view_ip6, sizeof(s_view_ip6));
    v->ip4        = s_view_ip4;
    v->ip6        = s_view_ip6;
    v->panel_port = "8080";
}

/**
 * @brief 把设备完整详情推到钉钉/企业微信机器人（`IP` / `详细信息` 指令触发）。
 *
 * 为什么详情走机器人而不是短信：本底包上"同一回执拆多条短信"只有第 1 条能发出去
 * （见 docs/12 §五之三），所以短信只回一条地址摘要，长内容用机器人推。
 * 面板地址同时给出 IPv6 与 IPv4 两种写法，便于对比能不能从外网访问。
 */
static void push_device_info(const sms_remote_cfg_t *rcfg, const sms_cfg_view_t *v,
                             const app_config_t *cfg)
{
    static char          detail[900];
    static webhook_req_t req;
    int64_t              utc      = sdk_utc_seconds();
    int                  clock_ok = app_time_is_valid(utc) ? 1 : 0;
    webhook_result_t     r;
    int                  br;
    const char          *ip4 = (v && v->ip4 && v->ip4[0]) ? v->ip4 : "(无)";
    const char          *ip6 = (v && v->ip6 && v->ip6[0]) ? v->ip6 : "(无)";

    (void)snprintf(detail, sizeof(detail),
                   "- IPv4: %s\n"
                   "- IPv6: %s\n\n"
                   "**面板地址（外网入向不可达，仅作记录）**\n\n"
                   "- http://[%s]:%s/\n"
                   "- http://%s:%s/\n\n"
                   "- 网络: %s / 信号 %s / 对时: %s\n"
                   "- U: %s\n- T: %s\n- K: %s\n- 模板: %s\n\n"
                   "**收发**  收 %lu / 转 %lu",
                   ip4, ip6,
                   ip6, (v && v->panel_port) ? v->panel_port : "8080",
                   ip4, (v && v->panel_port) ? v->panel_port : "8080",
                   (v && v->net) ? v->net : "-",
                   (v && v->signal) ? v->signal : "-",
                   (v && v->ntp) ? v->ntp : "-",
                   (rcfg && rcfg->url[0])    ? "已设置" : "未设置",
                   (rcfg && rcfg->secret[0]) ? "已设置" : "未设置",
                   (rcfg && rcfg->keyword[0]) ? rcfg->keyword : "未设置",
                   (rcfg && rcfg->tmpl[0]) ? rcfg->tmpl : "(默认)",
                   (unsigned long)(v ? v->recv_count : 0u),
                   (unsigned long)(v ? v->fwd_count : 0u));

    br = webhook_build_note_rcfg(cfg, rcfg, "设备详情 - ML307A 短信网关", detail,
                                 app_time_to_ts_ms(utc, sdk_uptime_ms()), clock_ok, &req);
    if (br != 0) {
        LOG_E("sms", "设备详情推送组装失败(rc=%d)：未配置 Webhook 地址？", br);
        sdk_diag_post("info note build fail rc=%d", br);
        return;
    }
    r = webhook_send_request(&req);
    LOG_I("sms", "设备详情已推送：http=%d biz_ok=%d %s", r.http_code, r.biz_ok, r.detail);
    sdk_diag_post("info note http=%d biz=%d signed=%d downgrade=%d",
                  r.http_code, r.biz_ok, req.signed_ok, req.sign_downgraded);
}

/**
 * @brief 把**面板状态页的全部内容**渲染成文本推到机器人（`面板` / `PANEL` 指令）。
 *
 * 背景（docs/12 §五之四）：本底包下应用内 HTTP 服务**收不到任何入向连接**
 * （入向 TCP 由模组内核栈应答，不交给应用的 lwIP；连无人监听的端口都"能连上"），
 * Web 面板无法从外部访问。这里把面板要看的数据改成**出向推送**。
 */
static void push_panel_info(const sms_remote_cfg_t *rcfg, const app_config_t *cfg)
{
    static char          detail[1200];
    static webhook_req_t req;
    int64_t              utc      = sdk_utc_seconds();
    int                  clock_ok = app_time_is_valid(utc) ? 1 : 0;
    webhook_result_t     r;
    int                  br;
    char                 ip4[24] = {0};
    char                 ip6[48] = {0};
    net_status_t         st;
    traffic_status_t     tr;
    int                  rndis;

    (void)sdk_net_pdp_addrs(ip4, sizeof(ip4), ip6, sizeof(ip6));
    memset(&st, 0, sizeof(st));
    net_mgr_status(&st);
    memset(&tr, 0, sizeof(tr));
    traffic_get_status(cfg, &tr);
    rndis = sdk_rndis_is_active();

    (void)snprintf(detail, sizeof(detail),
                   "**运行时间**  %u 分钟\n\n"
                   "- IPv4: %s\n"
                   "- IPv6: %s\n"
                   "- 公网IPv4: %s\n"
                   "- 公网IPv6: %s\n\n"
                   "- PDP: %s / APN: %s / IPv6: %s\n"
                   "- 注册: %s / CSQ: %d\n"
                   "- 对时: %s\n\n"
                   "**流量**\n\n"
                   "- 上行 %lu KB / 下行 %lu KB\n"
                   "- 本月 %lu KB / 限额 %lu MB%s\n\n"
                   "**短信**  收 %lu / 转 %lu / 失败 %lu\n\n"
                   "- USB共享(RNDIS): %s\n"
                   "- 管理员: %s（%s）\n\n"
                   "**配置**\n\n"
                   "- U: %s\n- T: %s\n- K: %s\n- 模板: %s",
                   (unsigned)(sdk_uptime_ms() / 60000u),
                   (ip4[0] != '\0') ? ip4 : "(无)",
                   (ip6[0] != '\0') ? ip6 : "(无)",
                   (st.pub_ip[0] != '\0') ? st.pub_ip : "(未查询)",
                   (st.pub_ip6[0] != '\0') ? st.pub_ip6 : "(未查询)",
                   (st.pdp_type[0] != '\0') ? st.pdp_type : "-",
                   (st.apn[0] != '\0') ? st.apn : "-",
                   (cfg != NULL && cfg->ipv6_enable) ? "开" : "关",
                   st.registered ? "已注册" : "未注册",
                   st.csq,
                   (s_view_ntp[0] != '\0') ? s_view_ntp : "-",
                   (unsigned long)tr.up_kb, (unsigned long)tr.down_kb,
                   (unsigned long)tr.used_kb, (unsigned long)tr.limit_mb,
                   tr.over_limit ? "（已超限）" : "",
                   (unsigned long)s_stat.received, (unsigned long)s_stat.forwarded,
                   (unsigned long)s_stat.failed,
                   (rndis == 1) ? "开" : ((rndis == 0) ? "关" : "不可用"),
                   (rcfg->admin[0] != '\0') ? rcfg->admin : "(未认领)",
                   (rcfg->admin_claimed != 0u) ? "已认领" : "未认领",
                   (rcfg->url[0] != '\0') ? "已设置" : "未设置",
                   (rcfg->secret[0] != '\0') ? "已设置" : "未设置",
                   (rcfg->keyword[0] != '\0') ? rcfg->keyword : "未设置",
                   (rcfg->tmpl[0] != '\0') ? rcfg->tmpl : "(默认)");

    br = webhook_build_note_rcfg(cfg, rcfg, "设备面板 - ML307A 短信网关", detail,
                                 app_time_to_ts_ms(utc, sdk_uptime_ms()), clock_ok, &req);
    if (br != 0) {
        LOG_E("sms", "面板推送组装失败(rc=%d)", br);
        sdk_diag_post("panel note build fail rc=%d", br);
        return;
    }
    r = webhook_send_request(&req);
    LOG_I("sms", "面板内容已推送：http=%d biz_ok=%d", r.http_code, r.biz_ok);
    sdk_diag_post("panel note http=%d biz=%d", r.http_code, r.biz_ok);
}

/** 把**全部指令的详细说明**推到机器人（「我要配置」触发；短信侧只放 55 字清单） */
static void push_menu_detail(const sms_remote_cfg_t *rcfg, const app_config_t *cfg)
{
    static const char    detail[] =
        "**配置指令**\n\n"
        "- `U <Webhook地址>` 设置钉钉/企业微信机器人地址\n"
        "- `T <加签密钥>` 设置加签密钥（`-` 清除）\n"
        "- `K <关键词>` 设置转发关键词（`-` 清除）\n"
        "- `M <模板>` 转发模板，占位符 {from}/{time}/{text}/{idx}\n"
        "- `SAVE` 保存到 flash（不保存则重启后丢失）\n"
        "- `RESET` 恢复默认配置并重启\n\n"
        "**查询指令**\n\n"
        "- `STATUS` 回一条状态短信（含管理员与认领状态）\n"
        "- `IP` / `详细信息` 回 IPv4/IPv6 摘要，完整详情推机器人\n"
        "- `面板` / `PANEL` 把面板状态页内容推机器人\n\n"
        "**管理员**\n\n"
        "- `A <号码>` 更换管理员号码（仅现任管理员可用）\n"
        "- `我要成为管理员` 仅当尚未认领时有效：发件人成为第一任管理员";
    static webhook_req_t req;
    int64_t              utc      = sdk_utc_seconds();
    int                  clock_ok = app_time_is_valid(utc) ? 1 : 0;
    webhook_result_t     r;
    int                  br;

    br = webhook_build_note_rcfg(cfg, rcfg, "指令说明 - ML307A 短信网关", detail,
                                 app_time_to_ts_ms(utc, sdk_uptime_ms()), clock_ok, &req);
    if (br != 0) {
        sdk_diag_post("menu note build fail rc=%d", br);
        return;
    }
    r = webhook_send_request(&req);
    sdk_diag_post("menu note http=%d biz=%d", r.http_code, r.biz_ok);
}

static void rcfg_snapshot(sms_remote_cfg_t *out)
{
    if (out == NULL) return;
    if (s_rcfg_lock != NULL && sdk_mutex_lock(s_rcfg_lock, 500u) == 0) {
        *out = s_rcfg;
        (void)sdk_mutex_unlock(s_rcfg_lock);
    } else {
        *out = s_rcfg;          /* 锁尚未建立（初始化早期）时退化为直接读 */
    }
}

int sms_svc_rcfg_snapshot(sms_remote_cfg_t *out, int *loaded, sms_stat_t *st)
{
    if (out == NULL) return -1;
    rcfg_snapshot(out);
    if (loaded != NULL) *loaded = s_rcfg_loaded;
    if (st != NULL)     *st = s_stat;
    return 0;
}

/* ================================================================== */
/* 回执发送（UCS2 分包 + 逐片 PDU 发送）                               */
/* ================================================================== */
static int send_reply(const char *dst, const char *text)
{
    int n;
    int i;
    int sent = 0;

    if (dst == NULL || text == NULL || text[0] == '\0') return -1;

    /* 首次发送前若还没取到短信中心，再试一次（可能当时还没入网） */
    sca_ensure();

    /*
     * 用**独立分片**（无 UDH）而不是拼接短信：
     *   拼接短信要求手机收齐所有片才显示，实测本底包连发多片时后续片会被拒，
     *   结果用户"什么都看不到"。独立分片则成功一条显示一条。
     */
    n = sms_concat_build_tx_plain((s_sca[0] != '\0') ? s_sca : NULL, dst, text,
                                  s_tx_pkts, (int)SMS_TX_MAX_PARTS);
    if (n <= 0) {
        ++s_stat.reply_failed;
        LOG_E("sms", "回执分包失败(rc=%d)，未发送", n);
        return -1;
    }

    LOG_I("sms", "回执 %d 条（每条独立）-> %s（%u 字节）", n, dst, (unsigned)strlen(text));
    sdk_diag_post("tx_reply dst=%s parts=%d sca=%s", dst, n,
                  (s_sca[0] != '\0') ? s_sca : "(默认)");

    for (i = 0; i < n; ++i) {
        int attempt;
        int rc = -1;

        for (attempt = 1; attempt <= 3; ++attempt) {
            rc = sdk_sms_send_pdu(s_tx_pkts[i].pdu);
            if (rc == 0) break;
            sdk_diag_post("tx_part %d/%d attempt%d FAIL rc=%d", i + 1, n, attempt, rc);
            /* 实际冷却由 sdk_sms_send_pdu() 内部按 SMS_TX_MIN_GAP_MS 统一负责 */
        }

        if (rc == 0) {
            ++sent;
        } else {
            LOG_E("sms", "回执第 %d/%d 条发送失败(rc=%d)", i + 1, n, rc);
        }
    }

    if (sent > 0) ++s_stat.replies;
    if (sent != n) ++s_stat.reply_failed;
    return (sent == n) ? 0 : -1;
}

/* ================================================================== */
/* 处理一条解码后的短信                                                */
/* ================================================================== */
static void handle_pdu(const char *pdu_hex)
{
    /*
     * ★ 栈预算：短信任务有效栈 3072 字节（sdk_task_create 钳制），而本函数会一路
     *   调到 sms_admin_handle / sms_concat_build_tx / webhook_build_forward。
     *   因此所有大对象一律静态：pdu(1.5K) / cmsg(1.3K) / cfg_snapshot(784) /
     *   rec(322) / out(1220) / req(2.4K)。本函数只由短信任务串行调用，无重入。
     */
    static sms_pdu_t        pdu;         /* 结构体较大（约 1.6KB），放静态区避免爆栈 */
    static sms_concat_msg_t cmsg;        /* 重组结果约 1.3KB，同样放静态区 */
    static sms_record_t     rec;
    static app_config_t     cfg_snapshot;
    static sms_remote_cfg_t rcfg_local;  /* 本条短信处理期间的远程配置副本 */
    static sms_cfg_view_t   view;        /* 状态视图（持有静态串指针） */
    static sms_admin_out_t  out;
    sms_pdu_status_t st;
    const char      *reason = NULL;
    const char      *text;
    const char      *number;
    const char      *scts;
    uint8_t          concat_seq   = 0u;
    uint8_t          concat_total = 0u;

    memset(&rec, 0, sizeof(rec));

    st = sms_pdu_decode(pdu_hex, &pdu);
    if (st != SMS_PDU_OK) {
        ++s_stat.decode_error;
        LOG_E("sms", "PDU 解码失败：%s（原文 %.80s）", sms_pdu_strerror(st), pdu_hex);
        return;
    }

    ++s_stat.received;

    /*
     * 接收侧原始 PDU 上报（诊断用，2026-10-15 加）：
     * 现场没有 UART 日志，长短信重组出问题时只有把整条 PDU 拿出来解码才能定位。
     * 一行包含：DCS、编码、UDHI、UDHL、级联字段、前 24 字节正文、完整 PDU 十六进制。
     */
    sdk_diag_post("rxpdu dcs=%u enc=%d udhi=%u udhl=%u cat=%u seq=%u/%u ref=%u txt=%.24s PDU=%s",
                  (unsigned)pdu.dcs, (int)pdu.alpha, (unsigned)pdu.udhi,
                  (unsigned)pdu.udh_len, (unsigned)pdu.concat.present,
                  (unsigned)pdu.concat.seq, (unsigned)pdu.concat.total,
                  (unsigned)pdu.concat.ref, pdu.text, pdu_hex);

    /* 复制配置快照，避免长时间持锁 */
    app_ctx_lock();
    cfg_snapshot = g_cfg;
    app_ctx_unlock();

    text   = pdu.text;
    number = pdu.number;
    scts   = pdu.scts;

    /* 长短信：分片重组（docs/09 第三节）。未启用拼接时退化为"每片当独立短信" */
    if (pdu.concat.present && s_rcfg.concat_enable) {
        int cr = sms_concat_feed(&pdu, &cmsg, sdk_uptime_ms());

        if (cr == SMS_CONCAT_BUFFERED) {
            ++s_stat.parts;
            LOG_I("sms", "长短信分片 %u/%u 已缓存（来自 %s，等待其余分片）",
                  (unsigned)pdu.concat.seq, (unsigned)pdu.concat.total, pdu.number);
            return;
        }
        if (cr != SMS_CONCAT_COMPLETE) {
            ++s_stat.parts;
            ++s_stat.concat_drop;
            LOG_W("sms", "长短信重组失败(rc=%d, seq=%u/%u)，丢弃该片",
                  cr, (unsigned)pdu.concat.seq, (unsigned)pdu.concat.total);
            return;
        }
        ++s_stat.reassembled;
        text   = cmsg.text;
        number = cmsg.number;
        scts   = cmsg.scts;
        concat_seq   = pdu.concat.total;    /* 已收齐：显示 total/total */
        concat_total = pdu.concat.total;
        LOG_I("sms", "长短信重组完成：%u 字节 / %u 片（来自 %s）",
              (unsigned)cmsg.text_len, (unsigned)pdu.concat.total, cmsg.number);
    }

    copy_trunc(rec.number, sizeof(rec.number), number);
    copy_trunc(rec.time, sizeof(rec.time), scts);
    /* 正文超过 255 字节会被截断（RAM 环形缓存成本考虑，需求 3.5） */
    copy_trunc(rec.text, sizeof(rec.text), text);
    rec.text_len   = (uint16_t)strlen(rec.text);
    rec.multipart  = pdu.concat.present;
    rec.part_seq   = concat_seq;
    rec.part_total = concat_total;
    rec.raw_len    = (uint16_t)strlen(pdu_hex);

    LOG_I("sms", "收到短信 号码=%s 时间=%s 编码=%s 长度=%u%s",
          rec.number,
          (rec.time[0] != '\0') ? rec.time : "未知",
          (pdu.alpha == SMS_ALPHA_7BIT) ? "7bit" :
          (pdu.alpha == SMS_ALPHA_UCS2) ? "UCS2" : "8bit",
          (unsigned)rec.text_len,
          rec.multipart ? "（长短信，已重组）" : "");
    LOG_D("sms", "内容: %s", rec.text);
    sdk_diag_post("rx num=%s len=%u text=%.40s", rec.number, (unsigned)rec.text_len, rec.text);

    /* 黑白名单过滤（需求 3.3）：黑名单号码连配置指令都不下发 */
    if (!app_config_sms_allowed(&cfg_snapshot, rec.number, &reason)) {
        rec.blocked = 1u;
        ++s_stat.blocked;
        LOG_W("sms", "短信被拦截（%s）：%s", (reason != NULL) ? reason : "policy", rec.number);
        store_record(&rec);
        return;
    }

    /* URC 到达即入缓存，Web 面板可先看到 */
    store_record(&rec);

    /*
     * 管理员指令链路（docs/09 第一节）：
     *   管理员 -> 解析并应用指令 -> 回执 -> SAVE 落盘 / RESET 重启，且**不转发**；
     *   其他人 -> 只转发，绝不执行配置。
     * 先取配置副本再操作：临界区只用于复制，AT/渲染/发送都在锁外做。
     */
    {
        int handled;

        rcfg_snapshot(&rcfg_local);
        build_view(&rcfg_local, &view);

        /* 指令只改副本；改完再整体写回 s_rcfg（保证 Web 侧读到的永远是一致快照） */
        handled = sms_admin_handle(&rcfg_local, rec.number, text, &view, &out);
        sdk_diag_post("admin=%d changed=%d save=%d reboot=%d reply_len=%d",
                      handled, out.changed, out.need_save, out.need_reboot, out.reply_len);        if (handled) {
            ++s_stat.admin_cmd;
            LOG_I("sms", "管理员指令：%d 字节回执，配置改动=%d 保存=%d 重启=%d",
                  out.reply_len, out.changed, out.need_save, out.need_reboot);

            if (out.changed || out.need_save) {
                int saved = 0;
                if (s_rcfg_lock != NULL && sdk_mutex_lock(s_rcfg_lock, 1000u) == 0) {
                    s_rcfg = rcfg_local;
                    if (out.need_save) {
                        saved = (sms_rcfg_save(&s_rcfg) == 0) ? 1 : 0;
                        s_rcfg_loaded = saved;
                    }
                    (void)sdk_mutex_unlock(s_rcfg_lock);
                }
                if (out.need_save) {
                    if (saved) LOG_I("sms", "远程配置已保存到 rcfg.bin");
                    else       LOG_E("sms", "远程配置保存失败（配置仍在内存中生效）");
                }
            }

            if (out.reply_len > 0) {
                (void)send_reply(rec.number, out.reply);
            }
            if (out.want_info) {
                push_device_info(&rcfg_local, &view, &cfg_snapshot);
            }
            if (out.want_panel) {
                push_panel_info(&rcfg_local, &cfg_snapshot);
            }
            if (out.want_menu) {
                push_menu_detail(&rcfg_local, &cfg_snapshot);
            }
            if (out.need_reboot) {
                LOG_W("sms", "收到 RESET：3 秒后重启模组");
                sdk_task_sleep_ms(3000u);
                sdk_reboot();
            }
            return;
        }
    }

    /* 推送 Webhook / 钉钉（需求 3.4 + docs/09 第四节） */
    if (cfg_snapshot.webhook_enable != 0u &&
        (rcfg_local.url[0] != '\0' || cfg_snapshot.webhook_url[0] != '\0')) {
        /* 2.4KB 的请求体放静态区；本函数只在短信任务里被调用 */
        static webhook_req_t req;
        int64_t              utc      = sdk_utc_seconds();
        int                  clock_ok = app_time_is_valid(utc) ? 1 : 0;
        int                  br;

        br = webhook_build_forward(&cfg_snapshot, &rcfg_local, rec.number, rec.time, rec.text,
                                   pdu_hex, (unsigned)s_stat.received,
                                   app_time_to_ts_ms(utc, sdk_uptime_ms()), clock_ok, &req);
        if (br != 0) {
            ++s_stat.failed;
            LOG_E("sms", "转发请求组装失败(rc=%d)：正文过长或未配置地址", br);
        } else {
            webhook_result_t r;

            if (req.sign_downgraded) {
                LOG_W("sms", "已配置加签密钥但时钟不可信 -> 本条降级为仅关键词");
            }
            r = webhook_send_request(&req);
            if (r.http_code == 200 || r.http_code == 204 || r.http_code == 201) {
                if (r.biz_ok) {
                    ++s_stat.forwarded;
                    LOG_I("sms", "Webhook 推送成功（HTTP %d%s）", r.http_code,
                          req.signed_ok ? "，已加签" : "");
                    /* 回写 forwarded 标记（重新入缓存的代价高，这里就地更新） */
                    {
                        uint8_t idx = (uint8_t)((s_rec_w + SMS_RECORD_NUM - 1u) % SMS_RECORD_NUM);
                        if (sdk_mutex_lock(s_rec_lock, 200u) == 0) {
                            s_rec[idx].forwarded = 1u;
                            (void)sdk_mutex_unlock(s_rec_lock);
                        }
                    }
                } else {
                    ++s_stat.failed;
                    LOG_E("sms", "Webhook 业务失败：%s", r.detail);
                }
            } else {
                ++s_stat.failed;
                LOG_E("sms", "Webhook 推送失败：%s", r.detail);
            }
        }
    } else {
        LOG_W("sms", "Webhook 未启用或未配置，本条未推送");
    }
}

/* ================================================================== */
/* +CMTI 兜底：从 SIM 存储读取                                          */
/* ================================================================== */
/** 接收路径的"完整性体检"用的解码缓冲（1.4KB，静态，避免吃任务栈） */
static sms_pdu_t s_pdu_probe;

/** 从一条 CMGR/CMGL 应答里找出 PDU 行并处理；返回 1=已处理，-2=PDU 被截断（需重读），0=没找到 */
static int handle_cmgr_text(const char *resp, int index)
{
    char  *save = NULL;
    char  *line;
    static char buf[1024];

    if (resp == NULL) return 0;

    (void)snprintf(buf, sizeof(buf), "%s", resp);
    line = strtok_r(buf, "\r\n", &save);
    while (line != NULL) {
        size_t len = 0;
        if (is_hex_pdu(line, &len)) {
            /*
             * ★ 先做一次"完整性体检"：AT 读回可能被分块投递截断（现场实测过：
             *   UDL 声称 140 字节、实际只到手 6 字节）。截断时**绝不能**把碎片
             *   当短信处理，否则会把半截 URL 之类的内容当成指令执行。
             */
            if (sms_pdu_decode(line, &s_pdu_probe) == SMS_PDU_ERR_TRUNC) {
                LOG_W("sms", "索引 %d 的 PDU 不完整（%u 字符），稍后重读", index, (unsigned)len);
                sdk_diag_post("rxpart trunc idx=%d hexlen=%u", index, (unsigned)len);
                return -2;
            }
            LOG_I("sms", "索引 %d 解析到 PDU（%u 字符）", index, (unsigned)len);
            handle_pdu(line);
            return 1;
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    return 0;
}

static void handle_cmti(int index)
{
    static char resp[1024];
    int         attempt;

    if (index < 0) return;

    /* PDU 被截断时重读（虚拟 AT 分块投递，重读往往就能拿全） */
    for (attempt = 1; attempt <= 3; ++attempt) {
        int rc;

        resp[0] = '\0';
        if (sdk_sms_read_index(index, resp, sizeof(resp)) < 0 && resp[0] == '\0') {
            LOG_W("sms", "+CMTI 索引 %d 读取失败（AT+CMGR，第 %d 次）", index, attempt);
            sdk_task_sleep_ms(500u);
            continue;
        }
        rc = handle_cmgr_text(resp, index);
        if (rc == -2) {                     /* 截断：稍后再读一次 */
            sdk_task_sleep_ms(800u);
            continue;
        }
        if (rc == 1) {
            (void)sdk_sms_delete_index(index);      /* 处理完就删，避免占满存储 */
        }
        return;
    }
    LOG_E("sms", "索引 %d 连续 3 次读取都不完整，保留在存储里待下次轮询", index);
}

/**
 * @brief 轮询未读短信（URC 兜底）。
 *
 * 为什么不只依赖 URC：`AT+CNMI` 已设为"存储 + 上报"，若 URC 因时序原因没被取到，
 * 短信会留在 ME 存储里；这里周期性 `AT+CMGL=0`（仅未读）就能把它捞回来。
 * 因为 `AT+CMGR`/`CMGL` 会把未读置为已读，与 URC 路径不会重复处理。
 */
static void poll_stored_sms(void)
{
    static char resp[1400];        /* 极小概率会有多条未读，一次读完 */
    const char *p;
    int         idx;

    resp[0] = '\0';
    if (sdk_sms_list_unread(resp, sizeof(resp)) < 0) return;
    if (strstr(resp, "+CMGL:") == NULL) return;      /* 没有未读 */

    LOG_I("sms", "轮询到未读短信（URC 可能漏了），逐条处理");
    sdk_diag_post("poll unread: %.80s", resp);

    /*
     * 逐条处理：从 +CMGL 行里取索引 -> 用 CMGR 精确读取（避免 CMGL 多条的缓冲截断）
     * -> 处理 -> 删除。
     */
    p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        const char *q = p + 6;
        char       *endp = NULL;
        long        v;

        while (*q == ' ') ++q;
        v = strtol(q, &endp, 10);
        if (endp == q || v < 0 || v > 999) { p = q; continue; }
        idx = (int)v;
        p = q;

        {
            static char one[1024];
            one[0] = '\0';
            if (sdk_sms_read_index(idx, one, sizeof(one)) >= 0 && one[0] != '\0') {
                if (handle_cmgr_text(one, idx)) {
                    (void)sdk_sms_delete_index(idx);
                }
            }
        }
    }
}

/* ================================================================== */
/* 服务任务                                                            */
/* ================================================================== */
void sms_svc_task(void *arg)
{
    static char line[SMS_LINE_MAX];
    int         expect_pdu = 0;
    int         s_read_probe_done = 0;
    uint32_t    last_sms_poll = 0u;

    (void)arg;

    for (;;) {
        /* 有 URC 才唤醒；超时用于周期性自检 */
        (void)sdk_sem_wait(s_q_sem, 1000u);

        /* 丢弃超时未收齐的长短信分片（30 秒），避免占着重组槽位 */
        sms_concat_tick(sdk_uptime_ms());

        /* 时钟不可信时尝试对时（钉钉加签前提；内部限速，不会每秒发 AT） */
        time_sync_try();

        /* URC 兜底：每 10 秒查一次"未读短信"，即使 +CMTI 漏了也不会丢短信 */
        if ((sdk_uptime_ms() - last_sms_poll) >= 10000u) {
            last_sms_poll = sdk_uptime_ms();
            poll_stored_sms();
        }

        /*
         * ★ 一次性"长应答读取"探针（启动后 20 秒执行一次，只诊断不处理）。
         * 目的：AT+CMGR 读长短信时分块投递被截断（模组侧 338 字符、应用只拿到 82），
         * 而这条路径无法凭空复现。用 AT+CMGL=4 把存储里的全部短信读一遍并上报长度，
         * 就能在**不需要手机配合**的情况下反复定位（存储里的长短信会一直保留）。
         */
        if (!s_read_probe_done && sdk_uptime_ms() > 20000u) {
            static char probe[1024];
            s_read_probe_done = 1;
            probe[0] = '\0';
            (void)sdk_at_cmd("AT+CMGL=4", probe, sizeof(probe), 15000u);
            sdk_diag_post("probe cmgl4 got=%u head=%.40s",
                          (unsigned)strlen(probe), probe);
        }

        while (queue_pop(line, sizeof(line))) {
            /* 1) +CMT: 之后紧跟着是 PDU 行 */
            if (strncmp(line, "+CMT:", 5) == 0) {
                expect_pdu = 1;
                LOG_D("sms", "URC: %s", line);
                continue;
            }
            /* 2) +CMTI: 短信存到索引（本底包若配置为 mode 1 会走这里） */
            if (strncmp(line, "+CMTI:", 6) == 0) {
                int idx = parse_cmti_index(line);
                LOG_I("sms", "URC: %s -> 使用 AT+CMGR 读取索引 %d", line, idx);
                expect_pdu = 0;
                handle_cmti(idx);
                continue;
            }
            /* 3) 期望 PDU 且该行是合法十六进制报文 */
            if (expect_pdu) {
                size_t len = 0;
                if (is_hex_pdu(line, &len)) {
                    expect_pdu = 0;
                    /* URC 内联 PDU 若被截断，绝不能当短信执行；存储里还有一份，
                       交给 10 秒轮询用 CMGR 重新读取。 */
                    if (sms_pdu_decode(line, &s_pdu_probe) == SMS_PDU_ERR_TRUNC) {
                        LOG_W("sms", "+CMT 内联 PDU 不完整（%u 字符），丢弃并等轮询重读", (unsigned)len);
                        sdk_diag_post("urc pdu trunc len=%u", (unsigned)len);
                        continue;
                    }
                    handle_pdu(line);
                    continue;
                }
                /* 可能是 "+CMT: ,25" 与 PDU 之间夹了空行，忽略空行继续等 */
                if (line[0] == '\0') continue;
                LOG_W("sms", "期望 PDU 但收到非报文行：%.60s", line);
                expect_pdu = 0;
                continue;
            }
            /* 4) 其它 URC（+CIEV/+CDS 等）只记日志 */
            LOG_D("sms", "URC(忽略): %s", line);
        }

        if (s_q_drop > 0u) {
            LOG_W("sms", "URC 行队列曾溢出，累计丢弃 %lu 行（短信可能丢失）",
                  (unsigned long)s_q_drop);
            s_q_drop = 0u;
        }
    }
}

/* ================================================================== */
/* 对外接口                                                            */
/* ================================================================== */
int sms_svc_init(void)
{
    int rc;

    memset(s_rec, 0, sizeof(s_rec));
    memset(&s_stat, 0, sizeof(s_stat));
    s_rec_w     = 0u;
    s_rec_count = 0u;
    s_q_head    = 0u;
    s_q_tail    = 0u;
    s_q_count   = 0u;
    s_q_drop    = 0u;

    s_sca[0]      = '\0';
    s_sca_next_try_ms = 0u;
    s_rcfg_loaded = 0;
    sms_rcfg_defaults(&s_rcfg);          /* 先填默认值，载入失败也不会用未初始化数据 */
    sms_concat_reset();

    s_q_lock   = sdk_mutex_new();
    s_rec_lock = sdk_mutex_new();
    s_rcfg_lock = sdk_mutex_new();
    s_q_sem    = sdk_sem_new(0u);
    if (s_q_lock == NULL || s_rec_lock == NULL || s_q_sem == NULL || s_rcfg_lock == NULL) {
        LOG_E("sms", "短信服务初始化失败：同步对象创建失败");
        return -1;
    }

    /* 注册 URC 并配置 PDU 模式 / 新短信直接上报 */
    rc = sdk_sms_channel_init(sms_svc_on_urc);
    if (rc < 0) {
        LOG_E("sms", "短信通道初始化失败(rc=%d)，短信功能不可用", rc);
        return -2;
    }
    if (rc > 0) {
        LOG_W("sms", "短信通道部分配置失败(rc=%d)，请用 AT 手工核对 CMGF/CNMI", rc);
    }

    /* 远程配置：rcfg.bin -> 失败/首启都是默认值（模块 5 已保证坏数据不炸）
     * 此刻短信任务尚未创建，不存在并发写，直接载入即可。 */
    s_rcfg_loaded = (sms_rcfg_load(&s_rcfg) == 1) ? 1 : 0;
    LOG_I("sms", "远程配置：%s（管理员=%s 关键词=%s 模板=%s URL=%s）",
          s_rcfg_loaded ? "rcfg.bin" : "默认值",
          s_rcfg.admin,
          (s_rcfg.keyword[0] != '\0') ? s_rcfg.keyword : "无",
          s_rcfg.tmpl,
          (s_rcfg.url[0] != '\0') ? s_rcfg.url : "未设置");

    /* 短信中心：SDK 要求发送 PDU 时携带 SMSC */
    sca_refresh();
    if (s_sca[0] != '\0') {
        LOG_I("sms", "短信中心(SMSC)：%s", s_sca);
    }

    LOG_I("sms", "短信服务就绪（PDU 模式 + URC 上报 + 长短信重组 + 远程配置）");
    return rc;
}

size_t sms_svc_recent(sms_record_t *out, size_t max)
{
    size_t n = 0u;
    size_t i;

    if (out == NULL || max == 0u) return 0u;
    if (s_rec_lock == NULL) return 0u;
    if (sdk_mutex_lock(s_rec_lock, 1000u) != 0) return 0u;

    for (i = 0u; i < s_rec_count && n < max; ++i) {
        /* 从最新往回取：最新在 (s_rec_w - 1) */
        uint8_t idx = (uint8_t)((s_rec_w + SMS_RECORD_NUM - 1u - i) % SMS_RECORD_NUM);
        out[n++] = s_rec[idx];
    }
    (void)sdk_mutex_unlock(s_rec_lock);
    return n;
}

void sms_svc_stat(sms_stat_t *st)
{
    if (st == NULL) return;
    *st = s_stat;
}

void sms_svc_clear(void)
{
    if (s_rec_lock == NULL) return;
    if (sdk_mutex_lock(s_rec_lock, 1000u) != 0) return;
    memset(s_rec, 0, sizeof(s_rec));
    s_rec_w     = 0u;
    s_rec_count = 0u;
    (void)sdk_mutex_unlock(s_rec_lock);
    LOG_I("sms", "短信缓存已清空");
}

size_t sms_svc_export(char *buf, size_t cap)
{
    /* 16×322 = 5152 字节：放栈上会超过 3072 的任务栈，改静态（只由 eloop 线程串行调用） */
    static sms_record_t rows[SMS_RECORD_NUM];
    size_t       n, i;
    size_t       off = 0u;

    if (buf == NULL || cap == 0u) return 0u;

    n = sms_svc_recent(rows, SMS_RECORD_NUM);

    {
        char head[512];        /* 11 个计数字段的极端长度约 394 字节，留足余量避免截断 */
        int  k = snprintf(head, sizeof(head),
                          "4G 短信网关 - 短信日志导出\r\n"
                          "导出条数: %u\r\n"
                          "统计: 收到 %lu / 拦截 %lu / 推送成功 %lu / 推送失败 %lu / 解码失败 %lu\r\n"
                          "长短信: 分片 %lu / 重组完成 %lu / 丢弃 %lu；管理员指令 %lu；回执 成功 %lu / 失败 %lu\r\n"
                          "==================================================\r\n",
                          (unsigned)n,
                          (unsigned long)s_stat.received, (unsigned long)s_stat.blocked,
                          (unsigned long)s_stat.forwarded, (unsigned long)s_stat.failed,
                          (unsigned long)s_stat.decode_error,
                          (unsigned long)s_stat.parts, (unsigned long)s_stat.reassembled,
                          (unsigned long)s_stat.concat_drop, (unsigned long)s_stat.admin_cmd,
                          (unsigned long)s_stat.replies, (unsigned long)s_stat.reply_failed);
        if (k > 0) {
            size_t kl = (size_t)k;
            if (kl >= cap) kl = cap - 1u;
            memcpy(buf, head, kl);
            buf[kl] = '\0';
            off = kl;
        }
    }

    for (i = 0u; i < n; ++i) {
        char   item[SMS_RECORD_TEXT_MAX + 192];
        int    k;
        char   flag[64];

        if (rows[i].blocked) {
            (void)snprintf(flag, sizeof(flag), "[已拦截]");
        } else if (rows[i].forwarded) {
            (void)snprintf(flag, sizeof(flag), "[已推送]");
        } else {
            (void)snprintf(flag, sizeof(flag), "[未推送]");
        }
        if (rows[i].multipart) {
            size_t fl = strlen(flag);
            (void)snprintf(&flag[fl], sizeof(flag) - fl, "[长短信 %u/%u]",
                           (unsigned)rows[i].part_seq, (unsigned)rows[i].part_total);
        }

        k = snprintf(item, sizeof(item),
                     "序号  : %u  %s\r\n"
                     "号码  : %s\r\n"
                     "时间  : %s\r\n"
                     "内容  : %s\r\n"
                     "--------------------------------------------------\r\n",
                     (unsigned)(i + 1u), flag,
                     rows[i].number,
                     (rows[i].time[0] != '\0') ? rows[i].time : "(未知)",
                     rows[i].text);
        if (k <= 0) continue;
        if (off + (size_t)k + 1u > cap) break;
        memcpy(&buf[off], item, (size_t)k);
        off += (size_t)k;
        buf[off] = '\0';
    }

    if (off == 0u && cap > 1u) {
        buf[0] = '\0';
    }
    return off;
}






