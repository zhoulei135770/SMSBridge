/**
 * @file    web_server.c
 * @brief   内置 Web 管理服务实现（裸 TCP 微型 HTTP 服务，端口 8080）。
 *
 * 设计取舍：
 *   - 单用户、单连接、串行处理（需求 5.5），不做并发，避免为每个连接开栈。
 *   - 请求头解析用定长静态缓冲，绝不 malloc，也绝不在栈上放大缓冲。
 *   - 关闭 RNDIS / 重启 这类会"断掉自己这条连接"的操作，都是**先把响应发出去**，
 *     再在函数末尾执行，否则浏览器只能看到连接被重置。
 */
#include "web_server.h"
#include "web_ui.h"
#include "sdk_port.h"
#include "app_log.h"
#include "app_ctx.h"
#include "net_mgr.h"
#include "traffic.h"
#include "usb_rndis.h"
#include "sms_svc.h"
#include "sha256.h"
#include "app_time.h"     /* 对时状态展示（app_time_is_valid） */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================== */
/* 缓冲                                                                */
/* ================================================================== */
#define REQ_MAX      4096u
#define BODY_MAX     3072u
#define EXPORT_MAX   (WEB_UI_EXPORT_MAX)

static int      s_listen_fd = -1;
static uint32_t s_session_token;
static uint32_t s_auth_deadline_ms;
static int      s_authed;

static char s_req[REQ_MAX];
static char s_body[BODY_MAX];
static char s_page[WEB_UI_STATUS_PAGE_MAX];
static char s_export[EXPORT_MAX];
static char s_small[WEB_UI_LOGIN_PAGE_MAX];

/* 延迟动作：响应发完后再执行，避免自己把自己的连接断掉 */
static int s_defer_reboot;
static int s_defer_rndis;   /* -1 不变 / 0 关 / 1 开 */
static int s_defer_ipv6 = -1;  /* -1 不变 / 0 关 / 1 开（响应后立即重拨 PDP） */
static int s_defer_pubip;      /* 1 = 响应后立刻重查一次公网 IP */

#define SESSION_TTL_MS  (8u * 3600u * 1000u)   /* 8 小时 */

/* ================================================================== */
/* 工具：请求头查找 / URL 解码 / 表单解析                               */
/* ================================================================== */
static int ci_equal_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
    return 1;
}

/** 在头部文本里找 "Name:" 后的值起点，找不到返回 NULL。 */
static const char *header_find(const char *hdrs, const char *name)
{
    size_t      nlen = strlen(name);
    const char *p    = hdrs;

    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) break;
        if (ci_equal_n(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') ++v;
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

static int hex2i(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** URL 解码（'+' 视为空格）。返回 0 成功。 */
static int url_decode(const char *in, size_t in_len, char *out, size_t cap)
{
    size_t i = 0u, o = 0u;

    if (in == NULL || out == NULL || cap == 0u) return -1;
    while (i < in_len && o + 1u < cap) {
        char c = in[i];
        if (c == '+') {
            out[o++] = ' ';
            ++i;
        } else if (c == '%' && i + 2u < in_len) {
            int hi = hex2i(in[i + 1u]);
            int lo = hex2i(in[i + 2u]);
            if (hi < 0 || lo < 0) return -2;
            out[o++] = (char)((hi << 4) | lo);
            i += 3u;
        } else {
            out[o++] = c;
            ++i;
        }
    }
    out[o] = '\0';
    return 0;
}

/** 取表单字段值。key 不存在返回 0，存在返回 1（值可能为空串）。 */
static int form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t      klen;
    const char *p;

    if (out != NULL && cap > 0u) out[0] = '\0';
    if (body == NULL || key == NULL) return 0;

    klen = strlen(key);
    p    = body;

    while (*p != '\0') {
        const char *amp;
        if (ci_equal_n(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            amp = strchr(v, '&');
            if (url_decode(v, (amp != NULL) ? (size_t)(amp - v) : strlen(v),
                           out, cap) != 0) {
                return 1;   /* 存在但解码失败 */
            }
            return 1;
        }
        amp = strchr(p, '&');
        if (amp == NULL) break;
        p = amp + 1;
    }
    return 0;
}

static int form_int(const char *body, const char *key, int32_t *out)
{
    char  tmp[24];
    char *end = NULL;
    long  v;

    if (out == NULL) return 0;
    if (!form_get(body, key, tmp, sizeof(tmp))) return 0;
    if (tmp[0] == '\0') return 0;

    v = strtol(tmp, &end, 10);
    if (end == tmp) return 0;
    *out = (int32_t)v;
    return 1;
}

/** 解析多行号码列表（textarea，最多 8 个）。返回写入个数。 */
static int form_number_list(const char *body, const char *key,
                            char list[][APP_CFG_NUM_MAX], uint8_t *count)
{
    char   buf[512];
    char  *save = NULL;
    char  *line;
    int    n = 0;

    if (count == NULL) return 0;
    *count = 0u;
    if (!form_get(body, key, buf, sizeof(buf))) return 0;

    line = strtok_r(buf, "\r\n,;", &save);
    while (line != NULL && n < APP_CFG_LIST_MAX) {
        /* 去掉首尾空白 */
        char *s = line;
        char *e;
        while (*s == ' ' || *s == '\t') ++s;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        *e = '\0';

        if (s[0] != '\0') {
            (void)snprintf(list[n], APP_CFG_NUM_MAX, "%s", s);
            ++n;
        }
        line = strtok_r(NULL, "\r\n,;", &save);
    }
    *count = (uint8_t)n;
    return n;
}

/* ================================================================== */
/* HTTP 响应                                                           */
/* ================================================================== */
static void send_all(int fd, const char *buf, size_t len)
{
    if (len > 0u) (void)sdk_tcp_send(fd, buf, len);
}

/** 组装并发送 "200 OK" + Content-Type + Content-Length + 正文。 */
static void send_html(int fd, const char *body, size_t body_len, const char *ctype)
{
    char   hdr[256];
    int    n;

    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %u\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 (ctype != NULL) ? ctype : "text/html; charset=utf-8",
                 (unsigned)body_len);
    if (n > 0) send_all(fd, hdr, (size_t)n);
    send_all(fd, body, body_len);
}

static void send_plain(int fd, const char *msg)
{
    size_t n = web_ui_plain(s_page, sizeof(s_page), msg);
    send_all(fd, s_page, n);
}

static void send_redirect(int fd, const char *loc)
{
    size_t n = web_ui_redirect(s_small, sizeof(s_small), loc);
    send_all(fd, s_small, n);
}

/* ================================================================== */
/* 登录态                                                              */
/* ================================================================== */
static uint32_t make_token(void)
{
    /* 用 uptime + 配置 CRC 派生一个不可预测度有限的会话号（内网单用户场景足够） */
    uint32_t a = sdk_uptime_ms();
    uint32_t b = app_config_crc32(&g_cfg, sizeof(g_cfg));
    return (uint32_t)(a * 2654435761u) ^ (b + 0x9E3779B9u);
}

int web_server_authed(void)
{
    if (!s_authed) return 0;
    if (sdk_uptime_ms() > s_auth_deadline_ms) {
        s_authed = 0;
        return 0;
    }
    return 1;
}

static void auth_login(void)
{
    s_session_token  = make_token();
    s_authed         = 1;
    s_auth_deadline_ms = sdk_uptime_ms() + SESSION_TTL_MS;
    LOG_I("web", "登录成功，会话已建立（8 小时有效，重启失效）");
}

static void auth_logout(void)
{
    s_authed = 0;
    s_session_token = 0u;
    LOG_I("web", "已退出登录");
}

/** 校验请求里携带的会话 Cookie。 */
static int check_cookie(const char *hdrs)
{
    const char *cookie;
    const char *p;

    if (!s_authed) return 0;
    cookie = header_find(hdrs, "Cookie");
    if (cookie == NULL) return 0;

    p = strstr(cookie, "sid=");
    if (p == NULL) return 0;
    p += 4;

    {
        uint32_t v = 0u;
        int      i;
        for (i = 0; i < 8; ++i) {
            int h = hex2i(p[i]);
            if (h < 0) return 0;
            v = (v << 4) | (uint32_t)h;
        }
        if (v != s_session_token) return 0;
    }
    return 1;
}

/* ================================================================== */
/* 组装状态快照                                                        */
/* ================================================================== */
/*
 * web_ui_status_page() 需要 web_sms_row_t 数组，其中 text 是 const char* 指针。
 * 为避免把上千字节的记录数组在栈上搬来搬去，这里用文件作用域静态缓冲承载，
 * 由 build_status() 填充、路由函数在同一次连接处理内使用（单连接串行，无并发）。
 */
static sms_record_t  s_sms_recs[SMS_RECORD_NUM];
static web_sms_row_t s_sms_rows[SMS_RECORD_NUM];
static size_t        s_sms_row_count;

static void build_status(web_status_t *st)
{
    /* 静态快照：这几个结构合计 1KB+，放栈上会把 eloop 线程（3072 有效栈）顶穿 */
    static net_status_t     ns;
    static traffic_status_t ts;
    static rndis_status_t   rs;
    size_t           n;
    size_t           i;

    memset(st, 0, sizeof(*st));

    net_mgr_status(&ns);
    traffic_get_status(&g_cfg, &ts);
    usb_rndis_status(&rs);

    st->online      = (ns.online || ns.registered) ? 1 : 0;
    (void)snprintf(st->apn, sizeof(st->apn), "%s", ns.apn);
    st->csq         = ns.csq;
    (void)snprintf(st->ip, sizeof(st->ip), "%s", ns.ip);
    /* IPv6 与公网地址（新增） */
    (void)snprintf(st->ip6, sizeof(st->ip6), "%s", ns.ip6);
    (void)snprintf(st->pub_ip, sizeof(st->pub_ip), "%s", ns.pub_ip);
    (void)snprintf(st->pub_ip6, sizeof(st->pub_ip6), "%s", ns.pub_ip6);
    (void)snprintf(st->pdp_type, sizeof(st->pdp_type), "%s", ns.pdp_type);
    st->ipv6_enable = (ns.ipv6_enable != 0u) ? 1 : 0;
    st->ipv6_on     = (ns.ipv6_on != 0u) ? 1 : 0;
    st->pubip_tried = (ns.pubip_tried != 0u) ? 1 : 0;
    st->pubip_ok    = (ns.pubip_ok != 0u) ? 1 : 0;
    if (ns.pubip_uptime_ms != 0u) {
        uint32_t age_s = (sdk_uptime_ms() - ns.pubip_uptime_ms) / 1000u;
        if (age_s < 60u) {
            (void)snprintf(st->pubip_when, sizeof(st->pubip_when), "%lu 秒前", (unsigned long)age_s);
        } else {
            (void)snprintf(st->pubip_when, sizeof(st->pubip_when), "%lu 分钟前",
                           (unsigned long)(age_s / 60u));
        }
    }

    /* 短信远程配置只读快照（rcfg.bin；与短信任务并发安全） */
    {
        static sms_remote_cfg_t rc;
        static sms_stat_t       ss;
        int              loaded = 0;

        memset(&rc, 0, sizeof(rc));
        memset(&ss, 0, sizeof(ss));
        if (sms_svc_rcfg_snapshot(&rc, &loaded, &ss) == 0) {
            st->rcfg_loaded = loaded;
            (void)snprintf(st->rcfg_admin, sizeof(st->rcfg_admin), "%s", rc.admin);
            (void)snprintf(st->rcfg_url, sizeof(st->rcfg_url), "%s", rc.url);
            st->rcfg_secret_set = (rc.secret[0] != '\0') ? 1 : 0;
            (void)snprintf(st->rcfg_keyword, sizeof(st->rcfg_keyword), "%s", rc.keyword);
            (void)snprintf(st->rcfg_tmpl, sizeof(st->rcfg_tmpl), "%s", rc.tmpl);
            st->rcfg_concat = (rc.concat_enable != 0u) ? 1 : 0;
            st->sms_parts        = ss.parts;
            st->sms_reassembled  = ss.reassembled;
            st->sms_admin_cmd    = ss.admin_cmd;
            st->sms_replies      = ss.replies;
        }
        if (app_time_is_valid(sdk_utc_seconds())) {
            (void)snprintf(st->sms_ntp, sizeof(st->sms_ntp), "%s", "已对时");
        } else {
            (void)snprintf(st->sms_ntp, sizeof(st->sms_ntp), "%s", "未对时(加签降级)");
        }
    }

    st->rndis_on       = (rs.active > 0) ? 1 : 0;
    st->rndis_config   = rs.configured;
    (void)snprintf(st->rndis_note, sizeof(st->rndis_note), "%s%s",
                   usb_rndis_note(), rs.changed ? "（与运行状态不一致）" : "");

    st->month_used_kb  = ts.used_kb;
    st->month_limit_mb = ts.limit_mb;
    st->session_bytes  = ts.session_kb * 1024u;
    st->up_bytes       = ts.up_kb * 1024u;     /* 上行：get_tx_byte_all 差值 */
    st->down_bytes     = ts.down_kb * 1024u;   /* 下行：get_rx_byte_all 差值 */
    st->directional_ok = ts.directional_ok;
    st->over_limit     = ts.over_limit;

    {
        char buf[32];
        buf[0] = '\0';
        if (sdk_get_imei(buf, sizeof(buf)) == 0) {
            (void)snprintf(st->imei, sizeof(st->imei), "%s", buf);
        }
        buf[0] = '\0';
        if (sdk_get_version(buf, sizeof(buf)) == 0) {
            (void)snprintf(st->fw_ver, sizeof(st->fw_ver), "%s", buf);
        } else {
            (void)snprintf(st->fw_ver, sizeof(st->fw_ver), "v1.1");
        }
    }
    sdk_uptime_str(st->uptime, sizeof(st->uptime));
    if (sdk_local_time_str(st->now, sizeof(st->now)) != 0) {
        (void)snprintf(st->now, sizeof(st->now), "未同步");
    }

    /* 短信列表：text 指向静态记录缓冲，在同一次请求处理期间始终有效 */
    n = sms_svc_recent(s_sms_recs, SMS_RECORD_NUM);
    for (i = 0u; i < n; ++i) {
        (void)snprintf(s_sms_rows[i].number, sizeof(s_sms_rows[i].number),
                       "%s", s_sms_recs[i].number);
        (void)snprintf(s_sms_rows[i].time, sizeof(s_sms_rows[i].time),
                       "%s", s_sms_recs[i].time);
        s_sms_rows[i].text = s_sms_recs[i].text;
    }
    s_sms_row_count = n;
}

/* ================================================================== */
/* 路由                                                                */
/* ================================================================== */
static void route_status_page(int fd, char *hdrs)
{
    static web_status_t st;      /* 1.4KB+：静态，避免叠加进 eloop 线程的调用栈 */
    size_t       n;

    (void)hdrs;
    build_status(&st);

    n = web_ui_status_page(s_page, sizeof(s_page), &st, &g_cfg,
                           s_sms_rows, s_sms_row_count);
    if (n == 0u) {
        send_plain(fd, "页面渲染失败（缓冲区不足）");
        return;
    }
    send_html(fd, s_page, n, "text/html; charset=utf-8");
}

static void route_login_page(int fd, int failed)
{
    size_t n = web_ui_login_page(s_small, sizeof(s_small), failed);
    send_html(fd, s_small, n, "text/html; charset=utf-8");
}

static void route_do_login(int fd, const char *body)
{
    char pass[64];
    char hex[SHA256_HEX_SIZE];
    int  failed = 0;

    if (!form_get(body, "password", pass, sizeof(pass)) || pass[0] == '\0') {
        failed = 1;
    } else {
        sha256_hex(pass, strlen(pass), hex);
        if (sha256_hex_equal(hex, g_cfg.pass_sha256)) {
            auth_login();
        } else {
            failed = 1;
            LOG_W("web", "登录失败：口令错误");
        }
    }
    if (failed) {
        route_login_page(fd, 1);
        return;
    }
    send_redirect(fd, "/");
}

static void route_config(int fd, const char *body)
{
    app_config_t *c = &g_cfg;
    int32_t       v;
    int           rndis_old;
    int           need_reboot = 0;
    char          tmp[APP_CFG_URL_MAX];
    char          pass[64];
    int           changed = 0;

    app_ctx_lock();

    rndis_old = (int)c->rndis_enable;

    if (form_get(body, "webhook_url", tmp, sizeof(tmp))) {
        (void)snprintf(c->webhook_url, sizeof(c->webhook_url), "%s", tmp);
        ++changed;
    }
    if (form_int(body, "webhook_type", &v))        c->webhook_type = (uint8_t)v;
    /* checkbox：存在且值为 1 才算开启 */
    if (form_get(body, "webhook_enable", tmp, sizeof(tmp))) {
        c->webhook_enable = (uint8_t)((tmp[0] == '1') ? 1 : 0);
    } else {
        c->webhook_enable = 0u;
    }
    if (form_get(body, "ping_target", tmp, sizeof(tmp))) {
        (void)snprintf(c->ping_target, sizeof(c->ping_target), "%s", tmp);
    }
    if (form_int(body, "ping_port", &v) && v > 0 && v <= 65535) {
        c->ping_port = (uint16_t)v;
    }
    if (form_int(body, "heartbeat_ms", &v))        c->heartbeat_ms = (uint32_t)v;
    if (form_int(body, "heartbeat_fail_limit", &v)) c->heartbeat_fail_limit = (uint16_t)v;
    if (form_int(body, "traffic_limit_mb", &v))    c->traffic_limit_mb = (uint32_t)v;
    if (form_int(body, "traffic_action", &v))      c->traffic_action = (uint8_t)v;
    if (form_int(body, "traffic_sample_sec", &v))  c->traffic_sample_sec = (uint32_t)v;
    if (form_int(body, "rndis_enable", &v))        c->rndis_enable = (uint8_t)((v != 0) ? 1 : 0);
    if (form_int(body, "rndis_apply_now", &v))     c->rndis_apply_now = (uint8_t)((v != 0) ? 1 : 0);
    /* IPv6 / 公网地址（v1.2）：改了 IPv6 开关时顺带让 net_mgr 立即重拨 */
    if (form_int(body, "ipv6_enable", &v)) {
        uint8_t want = (uint8_t)((v != 0) ? 1 : 0);
        if (c->ipv6_enable != want) {
            c->ipv6_enable = want;
            s_defer_ipv6 = (int)want;
            LOG_I("web", "IPv6 开关变更为%s（将在响应后立即重新激活 PDP）", want ? "开启" : "关闭");
        }
    }
    if (form_int(body, "pubip_enable", &v))        c->pubip_enable = (uint8_t)((v != 0) ? 1 : 0);
    if (form_get(body, "pubip_url", tmp, sizeof(tmp))) {
        (void)snprintf(c->pubip_url, sizeof(c->pubip_url), "%s", tmp);
        s_defer_pubip = 1;                        /* 地址可能变了，稍后重查一次 */
    }

    /* 口令：留空表示不修改（避免把口令清空导致进不去面板） */
    if (form_get(body, "password", pass, sizeof(pass)) && pass[0] != '\0') {
        sha256_hex(pass, strlen(pass), c->pass_sha256);
        c->pass_hint[0] = '\0';          /* 不再保留明文提示 */
        LOG_I("web", "登录口令已更新（仅存 SHA-256 摘要）");
        ++changed;
    }

    (void)form_number_list(body, "sms_white", c->sms_white, &c->white_count);
    (void)form_number_list(body, "sms_black", c->sms_black, &c->black_count);

    (void)app_config_sanitize(c);

    /* USB 流量共享开关变化：真正生效放到响应之后（否则会切断当前连接） */
    if ((int)c->rndis_enable != rndis_old) {
        s_defer_rndis = (int)c->rndis_enable;
        LOG_I("web", "USB 流量共享配置变更为%s", c->rndis_enable ? "开启" : "关闭");
    }

    app_ctx_unlock();

    /* 立刻把限额同步给平台 */
    traffic_apply_limit(&g_cfg);

    if (app_ctx_save() != 0) {
        send_plain(fd, "配置校验通过，但写入 Flash 失败，请重试");
        return;
    }

    (void)need_reboot;
    LOG_I("web", "配置已保存（本次修改 %d 处）", changed);
    send_redirect(fd, "/");
}

static void route_export(int fd)
{
    size_t n = sms_svc_export(s_export, sizeof(s_export));
    char   hdr[256];
    int    k;

    k = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: %u\r\n"
                 "Content-Disposition: attachment; filename=\"sms_log.txt\"\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 (unsigned)n);
    if (k > 0) send_all(fd, hdr, (size_t)k);
    send_all(fd, s_export, n);
}

static void route_request(int fd, const char *method, const char *path,
                          const char *hdrs, const char *body)
{
    int authed = check_cookie(hdrs);

    /* 未登录：除 /login 外一律回登录页（需求 5.1） */
    if (!authed) {
        if (strcmp(method, "POST") == 0 && strcmp(path, "/login") == 0) {
            route_do_login(fd, body);
            return;
        }
        if (strcmp(path, "/logout") == 0) {
            auth_logout();
        }
        route_login_page(fd, 0);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            route_status_page(fd, (char *)hdrs);
        } else if (strcmp(path, "/export") == 0) {
            route_export(fd);
        } else if (strcmp(path, "/logout") == 0) {
            auth_logout();
            send_redirect(fd, "/");
        } else if (strcmp(path, "/favicon.ico") == 0) {
            send_plain(fd, "");
        } else {
            send_plain(fd, "404 Not Found");
        }
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/config") == 0) {
            route_config(fd, body);
        } else if (strcmp(path, "/action/apn_next") == 0) {
            LOG_I("web", "收到【一键切换下一个 APN】");
            (void)net_mgr_switch_apn();
            send_redirect(fd, "/");
        } else if (strcmp(path, "/action/traffic_reset") == 0) {
            LOG_I("web", "收到【一键重置本月流量统计】");
            app_ctx_lock();
            (void)traffic_reset_month(&g_cfg);
            app_ctx_unlock();
            (void)app_ctx_save();
            send_redirect(fd, "/");
        } else if (strcmp(path, "/action/reboot") == 0) {
            LOG_W("web", "收到【一键重启模组】");
            send_plain(fd, "模组即将重启，请等待约 20 秒后刷新页面。");
            s_defer_reboot = 1;
        } else if (strcmp(path, "/action/ping") == 0) {
            (void)net_mgr_ping_now();
            send_redirect(fd, "/");
        } else if (strcmp(path, "/action/ipv6_on") == 0) {
            LOG_I("web", "收到【实时开启 IPv6 双栈】");
            send_redirect(fd, "/");
            s_defer_ipv6 = 1;
        } else if (strcmp(path, "/action/ipv6_off") == 0) {
            LOG_I("web", "收到【关闭 IPv6，切回纯 IPv4】");
            send_redirect(fd, "/");
            s_defer_ipv6 = 0;
        } else if (strcmp(path, "/action/pubip_refresh") == 0) {
            LOG_I("web", "收到【刷新公网 IP】");
            send_redirect(fd, "/");
            s_defer_pubip = 1;
        } else if (strcmp(path, "/login") == 0) {
            route_do_login(fd, body);
        } else {
            send_plain(fd, "404 Not Found");
        }
        return;
    }

    send_plain(fd, "405 Method Not Allowed");
}

/* ================================================================== */
/* 单连接处理                                                          */
/* ================================================================== */
static void handle_client(int fd)
{
    size_t total  = 0u;
    long   hdr_end = -1;
    char  *line_end;
    char   method[8];
    char   path[128];
    char  *p;
    long   content_len = 0;
    const char *cl;
    size_t leftover = 0u;

    s_req[0] = '\0';

    /* 1) 读到请求头结束 */
    while (total + 1u < sizeof(s_req)) {
        int r = sdk_tcp_recv(fd, &s_req[total], sizeof(s_req) - 1u - total, 3000u);
        if (r <= 0) break;
        total += (size_t)r;
        s_req[total] = '\0';
        p = strstr(s_req, "\r\n\r\n");
        if (p != NULL) {
            hdr_end = (long)(p - s_req) + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        /* ★ 诊断：没读到完整请求头时，把实际收到的字节数上报（判定"数据是否到达应用"） */
        sdk_diag_post("web hdr fail total=%u first=%.20s", (unsigned)total, s_req);
        send_plain(fd, "400 Bad Request");
        return;
    }
    sdk_diag_post("web hdr ok total=%u", (unsigned)total);
    if (hdr_end > (long)sizeof(s_req) - 1) {
        send_plain(fd, "431 Request Header Fields Too Large");
        return;
    }

    /* 2) 先把已经读进来的 body 尾巴搬走，再把头部区域截断以便字符串解析 */
    if ((size_t)hdr_end < total) {
        leftover = total - (size_t)hdr_end;
        if (leftover > sizeof(s_body) - 1u) leftover = sizeof(s_body) - 1u;
        memcpy(s_body, &s_req[hdr_end], leftover);
    }
    s_body[leftover] = '\0';
    s_req[hdr_end]   = '\0';

    /* 3) 解析请求行 */
    line_end = strstr(s_req, "\r\n");
    if (line_end == NULL) {
        send_plain(fd, "400 Bad Request");
        return;
    }
    *line_end = '\0';
    method[0] = '\0';
    path[0]   = '\0';
    if (sscanf(s_req, "%7s %127s", method, path) != 2) {
        send_plain(fd, "400 Bad Request");
        return;
    }
    *line_end = '\r';   /* 还原，后续 header_find 需要完整头部文本 */

    /* 4) Content-Length 与剩余 body */
    cl = header_find(&s_req[line_end - s_req + 2], "Content-Length");
    if (cl != NULL) {
        content_len = strtol(cl, NULL, 10);
        if (content_len < 0) content_len = 0;
        if (content_len > (long)sizeof(s_body) - 1) {
            content_len = (long)sizeof(s_body) - 1;   /* 截断保护，够解析表单 */
        }
    }
    while (content_len > 0 && (size_t)content_len > leftover) {
        int r = sdk_tcp_recv(fd, &s_body[leftover],
                             (size_t)content_len - leftover, 2000u);
        if (r <= 0) break;
        leftover += (size_t)r;
        s_body[leftover] = '\0';
    }

    LOG_D("web", "%s %s（body %u 字节）", method, path, (unsigned)leftover);

    route_request(fd, method, path,
                  &s_req[line_end - s_req + 2], s_body);
}

/* ================================================================== */
/* 任务与初始化                                                        */
/* ================================================================== */
void web_server_init(app_config_t *cfg)
{
    (void)cfg;
    s_authed       = 0;
    s_session_token = 0u;
    s_listen_fd    = -1;
    s_defer_reboot = 0;
    s_defer_rndis  = -1;
    s_defer_ipv6   = -1;
    s_defer_pubip  = 0;
    LOG_I("web", "Web 管理服务已初始化（端口 %u）", (unsigned)WEB_SERVER_PORT);
}

/* ------------------------------------------------------------------ */
/* 客户端处理：由 sdk_port 在 **eloop 线程** 内回调                      */
/*                                                                     */
/* 内核 socket 必须在 eloop 线程内创建与收发（SDK 官方 asocket 示例明确  */
/* 要求），因此这里不再自己 accept，而是把处理函数交给 sdk_srv_start。    */
/* ------------------------------------------------------------------ */
static void web_on_client(int fd)
{
    LOG_D("web", "客户端已连接 (fd=%d)", fd);

    handle_client(fd);

    /* 延迟动作：先保证响应已发出，再执行会断开连接的操作 */
    if (s_defer_rndis >= 0) {
        int want        = s_defer_rndis;
        int need_reboot = 0;
        s_defer_rndis   = -1;
        sdk_task_sleep_ms(500u);
        if (usb_rndis_set(want, &need_reboot) < 0 || need_reboot) {
            LOG_W("web", "USB 流量共享需重启后生效（当前底包不支持即时切换）");
        }
    }
    if (s_defer_reboot) {
        s_defer_reboot = 0;
        sdk_task_sleep_ms(800u);
        app_ctx_lock();
        (void)app_ctx_save();
        app_ctx_unlock();
        sdk_reboot();
    }

    /* IPv6 开关：交回 net_mgr 任务执行（AT 只能由它发起），这里只是唤醒 */
    if (s_defer_ipv6 >= 0) {
        int want = s_defer_ipv6;
        s_defer_ipv6 = -1;
        sdk_task_sleep_ms(300u);
        (void)net_mgr_set_ipv6(want);
        LOG_I("web", "已请求 net_mgr %s IPv6（PDP 重新激活中）", want ? "开启" : "关闭");
    }
    if (s_defer_pubip) {
        s_defer_pubip = 0;
        (void)net_mgr_refresh_pubip();
    }
}

void web_server_task(void *arg)
{
    uint32_t last_ip_wait_log = 0u;
    int      started          = 0;

    (void)arg;

    /* 等系统与网络栈就绪 */
    sdk_task_sleep_ms(2000u);

    for (;;) {
        if (!started) {
            /* 同时开 IPv4 与 IPv6 监听：v4 供 RNDIS/内网，v6 供"公网 IPv6 直连面板"实验 */
    if (sdk_srv_start((uint16_t)WEB_SERVER_PORT, web_on_client) == 0) {
        (void)sdk_srv_start_family((uint16_t)WEB_SERVER_PORT, web_on_client, 10 /* AF_INET6 */);
                started = 1;
                LOG_I("web", "Web 面板监听已提交（端口 %u）；就绪后访问 http://<模组IP>:%u/",
                      (unsigned)WEB_SERVER_PORT, (unsigned)WEB_SERVER_PORT);
            } else {
                LOG_E("web", "启动异步监听失败（端口 %u），5 秒后重试", (unsigned)WEB_SERVER_PORT);
            }
        }

        /* 周期检查：登录会话过期 + 等待 PDP 地址的提示 */
        if (!web_server_authed() && s_authed) {
            LOG_I("web", "登录会话已过期，需重新登录");
        }
        if (sdk_uptime_ms() - last_ip_wait_log > 30000u) {
            net_status_t ns;
            net_mgr_status(&ns);
            if (ns.ip[0] == '\0') {
                LOG_W("web", "尚无 PDP 地址；USB(RNDIS) 侧 http://192.168.0.1:%u/ 不受影响",
                      (unsigned)WEB_SERVER_PORT);
            }
            last_ip_wait_log = sdk_uptime_ms();
        }

        sdk_task_sleep_ms(5000u);
    }
}

