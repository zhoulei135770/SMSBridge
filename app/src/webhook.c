/**
 * @file    webhook.c
 * @brief   Webhook 消息组装与推送实现。
 */
#include "webhook.h"
#include "webhook_fmt.h"    /* 模板渲染 / 关键词注入 / 加签 URL */
#include "sdk_port.h"
#include "app_log.h"

#include <string.h>
#include <stdio.h>

/* 编译期确认对外公布的请求体上限与实现缓冲一致 */
typedef char webhook_body_size_contract[(WEBHOOK_BODY_MAX == 2048u) ? 1 : -1];

/* 有界拷贝（截断不越界） */
static void copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0u) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ================================================================== */
/* JSON 转义与缓冲拼接                                                 */
/* ================================================================== */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} sbuf_t;

static void sb_init(sbuf_t *s, char *buf, size_t cap)
{
    s->buf = buf;
    s->cap = cap;
    s->len = 0u;
    s->overflow = 0;
    if (cap > 0u) buf[0] = '\0';
}

static void sb_putc(sbuf_t *s, char c)
{
    if (s->len + 1u >= s->cap) {
        s->overflow = 1;
        return;
    }
    s->buf[s->len++] = c;
    s->buf[s->len]   = '\0';
}

static void sb_puts(sbuf_t *s, const char *str)
{
    if (str == NULL) return;
    while (*str != '\0') {
        sb_putc(s, *str++);
    }
}

/** 按 JSON 规范转义字符串内容（不含首尾引号）。UTF-8 字节原样透传。 */
static void sb_put_json_escaped(sbuf_t *s, const char *str)
{
    const unsigned char *p = (const unsigned char *)str;

    if (str == NULL) return;

    while (*p != 0u) {
        unsigned char c = *p++;
        switch (c) {
        case '"':  sb_puts(s, "\\\""); break;
        case '\\': sb_puts(s, "\\\\"); break;
        case '\n': sb_puts(s, "\\n");  break;
        case '\r': sb_puts(s, "\\r");  break;
        case '\t': sb_puts(s, "\\t");  break;
        case '\b': sb_puts(s, "\\b");  break;
        case '\f': sb_puts(s, "\\f");  break;
        default:
            if (c < 0x20u) {
                /* 其余控制字符用 \u00XX */
                char tmp[8];
                (void)snprintf(tmp, sizeof(tmp), "\\u%04X", (unsigned)c);
                sb_puts(s, tmp);
            } else {
                sb_putc(s, (char)c);
            }
            break;
        }
    }
}

/* ================================================================== */
/* 类型识别                                                            */
/* ================================================================== */
app_webhook_type_t webhook_detect_type(const char *url)
{
    if (url == NULL) return APP_WEBHOOK_RAW;
    if (strstr(url, "qyapi.weixin.qq.com") != NULL) return APP_WEBHOOK_WECOM;
    if (strstr(url, "oapi.dingtalk.com") != NULL)    return APP_WEBHOOK_DINGTALK;
    return APP_WEBHOOK_RAW;
}

static app_webhook_type_t effective_type(const app_config_t *cfg)
{
    if (cfg == NULL) return APP_WEBHOOK_RAW;
    if (cfg->webhook_type == (uint8_t)APP_WEBHOOK_AUTO) {
        return webhook_detect_type(cfg->webhook_url);
    }
    return (app_webhook_type_t)cfg->webhook_type;
}

/* ================================================================== */
/* 消息体组装                                                          */
/* ================================================================== */
/**
 * 统一组装：不同机器人的 JSON 结构不同（依据各自官方文档）：
 *   企业微信: {"msgtype":"markdown","markdown":{"content":"..."}}
 *   钉钉    : {"msgtype":"markdown","markdown":{"title":"...","text":"..."}}
 *   通用    : {"msgtype":"markdown","markdown":{"title":"...","text":"..."}}
 * 企业微信正文上限 4096 字节、钉钉 20000 字节，这里统一截断到安全的 1800 字节。
 */
static size_t build_payload(const app_config_t *cfg, const char *title,
                            const char *markdown, char *out, size_t cap)
{
    sbuf_t             s;
    app_webhook_type_t type = effective_type(cfg);

    if (markdown == NULL || out == NULL || cap < 64u) return 0u;

    sb_init(&s, out, cap);
    sb_puts(&s, "{\"msgtype\":\"markdown\",\"markdown\":{");

    if (type == APP_WEBHOOK_WECOM) {
        sb_puts(&s, "\"content\":\"");
    } else {
        sb_puts(&s, "\"title\":\"");
        /* 标题只取一行，避免 JSON 里出现换行 */
        {
            size_t n = 0u;
            const char *p = (title != NULL) ? title : "短信网关";
            while (*p != '\0' && *p != '\n' && *p != '\r' && n < 60u) {
                unsigned char c = (unsigned char)*p++;
                if (c == '"' || c == '\\') sb_putc(&s, '\\');
                sb_putc(&s, (char)c);
                ++n;
            }
        }
        sb_puts(&s, "\",\"text\":\"");
    }

    sb_put_json_escaped(&s, markdown);
    sb_puts(&s, "\"}}");

    if (s.overflow) {
        return 0u;      /* 超长时宁可失败也不发半截 JSON */
    }
    return s.len;
}

size_t webhook_build_sms_markdown(const app_config_t *cfg,
                                  const char *number, const char *text,
                                  const char *time,
                                  char *out, size_t cap)
{
    /* 1280 字节正文缓冲放静态区：任务有效栈只有 3072 字节（见 docs/10 坑表） */
    static char body[1280];

    if (out == NULL || cap == 0u) return 0u;

    (void)snprintf(body, sizeof(body),
                   "### 收到新短信\n"
                   "> **发件号码：** %s\n"
                   "> **接收时间：** %s\n\n"
                   "**内容：**\n%s",
                   (number != NULL) ? number : "(未知)",
                   (time != NULL && time[0] != '\0') ? time : "(未知)",
                   (text != NULL) ? text : "(空)");

    return build_payload(cfg, "4G 短信网关 - 新短信", body, out, cap);
}

size_t webhook_build_note_markdown(const app_config_t *cfg,
                                   const char *title, const char *content,
                                   char *out, size_t cap)
{
    /* 同上：正文缓冲放静态区，避免 1280 字节压栈 */
    static char body[1280];

    if (out == NULL || cap == 0u) return 0u;

    (void)snprintf(body, sizeof(body),
                   "### %s\n%s",
                   (title != NULL) ? title : "通知",
                   (content != NULL) ? content : "");

    return build_payload(cfg, title, body, out, cap);
}

/* ================================================================== */
/* 发送                                                                */
/* ================================================================== */
/** 解析机器人返回体里的 errcode / code 字段，判断业务是否成功。 */
static int parse_biz_ok(const char *rsp)
{
    const char *p;

    if (rsp == NULL || rsp[0] == '\0') {
        /* 企业微信成功时返回空体或 {"errcode":0}；钉钉返回 {"errcode":0} */
        return 1;
    }
    p = strstr(rsp, "\"errcode\"");
    if (p == NULL) p = strstr(rsp, "\"code\"");
    if (p == NULL) return 1;              /* 无错误字段，按成功处理 */
    p = strchr(p, ':');
    if (p == NULL) return 1;
    ++p;
    while (*p == ' ') ++p;
    return (*p == '0') ? 1 : 0;
}

static webhook_result_t do_send_raw(const char *url, const char *payload, size_t len)
{
    webhook_result_t r;
    char             rsp[256];
    int              code;

    memset(&r, 0, sizeof(r));
    r.http_code = -1;

    if (url == NULL || url[0] == '\0') {
        (void)snprintf(r.detail, sizeof(r.detail), "未配置 Webhook 地址");
        return r;
    }
    if (payload == NULL || len == 0u) {
        (void)snprintf(r.detail, sizeof(r.detail), "消息体组装失败或超长");
        return r;
    }

    rsp[0] = '\0';
    code = sdk_http_post_json(url, "application/json",
                              payload, (uint32_t)len, rsp, sizeof(rsp));
    r.http_code = code;

    if (code == 200 || code == 204 || code == 201) {
        r.biz_ok = parse_biz_ok(rsp);
        if (r.biz_ok) {
            (void)snprintf(r.detail, sizeof(r.detail), "OK");
        } else {
            (void)snprintf(r.detail, sizeof(r.detail), "机器人返回错误: %.60s", rsp);
        }
    } else if (code < 0) {
        (void)snprintf(r.detail, sizeof(r.detail), "连接/发送失败(code=%d)", code);
    } else {
        (void)snprintf(r.detail, sizeof(r.detail), "HTTP %d: %.60s", code, rsp);
    }
    return r;
}

static webhook_result_t do_send(const app_config_t *cfg, const char *payload, size_t len)
{
    if (cfg == NULL) {
        webhook_result_t r;
        memset(&r, 0, sizeof(r));
        r.http_code = -1;
        (void)snprintf(r.detail, sizeof(r.detail), "配置为空");
        return r;
    }
    return do_send_raw(cfg->webhook_url, payload, len);
}

webhook_result_t webhook_send_sms(const app_config_t *cfg,
                                 const char *number, const char *text,
                                 const char *time)
{
    static char payload[WEBHOOK_BODY_MAX];
    size_t      len;

    len = webhook_build_sms_markdown(cfg, number, text, time, payload, sizeof(payload));
    return do_send(cfg, payload, len);
}

/* ================================================================== */
/* 远程配置驱动的"自定义通知"（设备详情推送等）                          */
/* ================================================================== */
int webhook_build_note_rcfg(const app_config_t *cfg, const sms_remote_cfg_t *rcfg,
                            const char *title, const char *content,
                            uint64_t ts_ms, int clock_valid, webhook_req_t *req)
{
    /* 临时 app_config（约 1.5KB）与两个正文缓冲都放静态区：调用者是短信任务，单线程 */
    static app_config_t s_tmp;
    static char         s_body[WEBHOOK_BODY_MAX];
    static char         s_final[WEBHOOK_BODY_MAX];
    const char         *url;
    const char         *secret;
    const char         *kw;
    int                 n;

    if (cfg == NULL || req == NULL) return -1;
    memset(req, 0, sizeof(*req));

    url    = (rcfg != NULL && rcfg->url[0] != '\0') ? rcfg->url : cfg->webhook_url;
    /* 注意：`app_config_t` 里**没有** webhook_secret 字段（密钥只在远程配置 rcfg 里） */
    secret = (rcfg != NULL) ? rcfg->secret : "";
    kw     = (rcfg != NULL) ? rcfg->keyword : "";
    if (url == NULL || url[0] == '\0') return -2;

    /*
     * 关键：机器人类型识别（钉钉 markdown / 企业微信 / 通用）用的是
     * `cfg->webhook_url`，而生效 URL 在 rcfg 里。这里把生效 URL 放进临时副本，
     * 保证 body 结构与真实目标一致。
     */
    s_tmp = *cfg;
    copy_trunc(s_tmp.webhook_url, sizeof(s_tmp.webhook_url), url);

    n = snprintf(s_body, sizeof(s_body), "### %s\n%s",
                 (title != NULL) ? title : "通知",
                 (content != NULL) ? content : "");
    if (n <= 0) return -3;

    /* 关键词注入（钉钉按正文匹配自定义关键词，空则跳过） */
    if (kw != NULL && kw[0] != '\0') {
        if (webhook_apply_keyword(kw, s_body, s_final, sizeof(s_final)) < 0) return -3;
    } else {
        copy_trunc(s_final, sizeof(s_final), s_body);
    }

    /* 加签：有密钥且时钟可信才签；否则降级为不加签（钉钉会校验 timestamp） */
    if (secret != NULL && secret[0] != '\0') {
        if (clock_valid && ts_ms > 0u) {
            if (webhook_build_url(url, secret, ts_ms, req->url, sizeof(req->url)) < 0) return -3;
            req->signed_ok = 1;
        } else {
            copy_trunc(req->url, sizeof(req->url), url);
            req->sign_downgraded = 1;
        }
    } else {
        copy_trunc(req->url, sizeof(req->url), url);
    }

    req->payload_len = build_payload(&s_tmp, title, s_final, req->payload, sizeof(req->payload));
    if (req->payload_len == 0u) return -3;
    return 0;
}

webhook_result_t webhook_send_note(const app_config_t *cfg,
                                  const char *title, const char *content)
{
    static char payload[WEBHOOK_BODY_MAX];
    size_t      len;

    len = webhook_build_note_markdown(cfg, title, content, payload, sizeof(payload));
    return do_send(cfg, payload, len);
}

/* ================================================================== */
/* 远程配置驱动的转发（模板 -> 关键词 -> JSON -> 加签 URL）             */
/* ================================================================== */
int webhook_build_forward(const app_config_t *cfg, const sms_remote_cfg_t *rcfg,
                          const char *from, const char *time_s, const char *text,
                          const char *pdu, unsigned idx,
                          uint64_t ts_ms, int clock_valid,
                          webhook_req_t *req)
{
    /* 渲染缓冲放静态区：单个调用者是短信任务，且 2×2KB 放栈上会挤爆任务栈 */
    static char s_body[WEBHOOK_BODY_MAX];
    static char s_final[WEBHOOK_BODY_MAX];
    const char *url;
    const char *tmpl;
    int         n;

    if (cfg == NULL || req == NULL) return -1;
    memset(req, 0, sizeof(*req));

    url = (rcfg != NULL && rcfg->url[0] != '\0') ? rcfg->url : cfg->webhook_url;
    if (url == NULL || url[0] == '\0') return -2;

    tmpl = (rcfg != NULL && rcfg->tmpl[0] != '\0') ? rcfg->tmpl : "{from}|{time}|{text}";

    /* 1) 模板渲染 */
    n = webhook_render_template(tmpl, from, time_s, text, pdu, idx, s_body, sizeof(s_body));
    if (n < 0) return -3;

    /* 2) 关键词注入（钉钉按正文匹配关键词，空则跳过） */
    if (rcfg != NULL && rcfg->keyword[0] != '\0') {
        n = webhook_apply_keyword(rcfg->keyword, s_body, s_final, sizeof(s_final));
        if (n < 0) return -3;
    } else {
        copy_trunc(s_final, sizeof(s_final), s_body);
    }

    /* 3) 加签：有密钥且时钟可信才签；否则降级为不加签（钉钉会校验 timestamp） */
    if (rcfg != NULL && rcfg->secret[0] != '\0') {
        if (clock_valid && ts_ms > 0u) {
            if (webhook_build_url(url, rcfg->secret, ts_ms, req->url, sizeof(req->url)) < 0) return -3;
            req->signed_ok = 1;
        } else {
            copy_trunc(req->url, sizeof(req->url), url);
            req->sign_downgraded = 1;
        }
    } else {
        copy_trunc(req->url, sizeof(req->url), url);
    }

    /* 4) 包成机器人 JSON 体（企业微信/钉钉/通用） */
    req->payload_len = build_payload(cfg, "4G 短信网关 - 新短信", s_final,
                                     req->payload, sizeof(req->payload));
    if (req->payload_len == 0u) return -3;
    return 0;
}

webhook_result_t webhook_send_request(const webhook_req_t *req)
{
    if (req == NULL) {
        webhook_result_t r;
        memset(&r, 0, sizeof(r));
        r.http_code = -1;
        (void)snprintf(r.detail, sizeof(r.detail), "转发请求为空");
        return r;
    }
    return do_send_raw(req->url, req->payload, req->payload_len);
}
