/**
 * @file    sms_cmd.c
 * @brief   短信远程配置指令解析实现（见 inc/sms_cmd.h）。
 */
#include "sms_cmd.h"
#include "sms_concat.h"   /* SMS_TX_PLAIN_PER_PART：单条短信的 UCS2 字符上限 */
#include <string.h>
#include <stdio.h>

/* 去空格 */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* 归一化号码：去掉 '+'、去除非数字，并可去掉前缀 86 */
static void norm_num(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    if (in == NULL || out_size == 0u) { if (out_size) out[0] = '\0'; return; }
    while (*in != '\0' && o + 1u < out_size) {
        if (*in >= '0' && *in <= '9') out[o++] = *in;
        ++in;
    }
    out[o] = '\0';
    if (o > 11u && out[0] == '8' && out[1] == '6') {
        memmove(out, out + 2, o - 1u);          /* 去 86 前缀 */
    }
}

int sms_cmd_is_admin(const char *sender, const char *admin)
{
    char a[32];
    char b[32];
    if (sender == NULL || admin == NULL) return 0;
    norm_num(sender, a, sizeof(a));
    norm_num(admin,  b, sizeof(b));
    if (a[0] == '\0' || b[0] == '\0') return 0;
    return (strcmp(a, b) == 0) ? 1 : 0;
}

/* 取本行参数（整行去掉前导指令后的剩余部分，去首尾空白） */
static void take_arg(const char *p, char *out, size_t out_size)
{
    size_t n;
    p = skip_ws(p);
    n = strlen(p);
    while (n > 0u && (p[n - 1u] == ' ' || p[n - 1u] == '\t' || p[n - 1u] == '\r')) --n;
    if (n >= out_size) n = out_size - 1u;
    memcpy(out, p, n);
    out[n] = '\0';
}

int sms_cmd_parse(const char *text, sms_cmd_set_t *out)
{
    const char *p;
    int         n = 0;

    if (text == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    p = text;
    while (*p != '\0' && n < (int)SMS_CMD_MAX) {
        const char *eol = p;
        size_t      len;
        char        line[SMS_CMD_ARG_MAX + 8u];
        sms_cmd_kind_t k = SMS_CMD_NONE;
        const char  *argp = NULL;

        while (*eol != '\0' && *eol != '\n' && *eol != '\r') ++eol;
        len = (size_t)(eol - p);
        if (len >= sizeof(line)) len = sizeof(line) - 1u;
        memcpy(line, p, len);
        line[len] = '\0';

        /* 跳过空白行 */
        {
            const char *q = skip_ws(line);
            if (*q == '\0') { p = (*eol == '\0') ? eol : eol + 1; continue; }

            if (strncmp(q, "我要配置", strlen("我要配置")) == 0) { k = SMS_CMD_MENU; argp = q + strlen("我要配置"); }
            else if ((q[0] == 'U' || q[0] == 'u') && (q[1] == ' ' || q[1] == '\t')) { k = SMS_CMD_SET_URL;      argp = q + 1; }
            else if ((q[0] == 'T' || q[0] == 't') && (q[1] == ' ' || q[1] == '\t')) { k = SMS_CMD_SET_SECRET;   argp = q + 1; }
            else if ((q[0] == 'K' || q[0] == 'k') && (q[1] == ' ' || q[1] == '\t')) { k = SMS_CMD_SET_KEYWORD;  argp = q + 1; }
            else if ((q[0] == 'M' || q[0] == 'm') && (q[1] == ' ' || q[1] == '\t')) { k = SMS_CMD_SET_TEMPLATE; argp = q + 1; }
            else if (strncmp(q, "SAVE", 4) == 0 || strncmp(q, "save", 4) == 0) { k = SMS_CMD_SAVE; argp = q + 4; }
            else if (strncmp(q, "STATUS", 6) == 0 || strncmp(q, "status", 6) == 0) { k = SMS_CMD_STATUS; argp = q + 6; }
            else if (strncmp(q, "IP", 2) == 0 && (q[2] == '\0' || q[2] == ' ')) { k = SMS_CMD_INFO; argp = q + 2; }
            else if (strncmp(q, "INFO", 4) == 0 || strncmp(q, "info", 4) == 0) { k = SMS_CMD_INFO; argp = q + 4; }
            else if (strncmp(q, "详细信息", strlen("详细信息")) == 0) { k = SMS_CMD_INFO; argp = q + strlen("详细信息"); }
            else if (strncmp(q, "面板", strlen("面板")) == 0) { k = SMS_CMD_PANEL; argp = q + strlen("面板"); }
            else if (strncmp(q, "PANEL", 5) == 0 || strncmp(q, "panel", 5) == 0) { k = SMS_CMD_PANEL; argp = q + 5; }
            else if (strncmp(q, "ADMIN", 5) == 0 || strncmp(q, "admin", 5) == 0) { k = SMS_CMD_SET_ADMIN; argp = q + 5; }
            else if ((q[0] == 'A' || q[0] == 'a') && (q[1] == ' ' || q[1] == '\t')) { k = SMS_CMD_SET_ADMIN; argp = q + 1; }
            else if (strncmp(q, "我要成为管理员", strlen("我要成为管理员")) == 0) { k = SMS_CMD_CLAIM; argp = q + strlen("我要成为管理员"); }
            else if (strncmp(q, "RESET", 5) == 0 || strncmp(q, "reset", 5) == 0) { k = SMS_CMD_RESET; argp = q + 5; }
            else { k = SMS_CMD_ERR; argp = q; }
        }

        out->kind[n] = k;
        if (argp != NULL) take_arg(argp, out->arg[n], SMS_CMD_ARG_MAX);
        else              out->arg[n][0] = '\0';
        ++n;

        if (*eol == '\0') break;
        p = eol + 1;
    }

    out->count = n;
    return n;
}

int sms_cmd_build_status(const sms_cfg_view_t *v, char *out, size_t out_size)
{
    /*
     * ★ 一条短信装完（≤70 个 UCS2 字符）。
     *   追加了「管理员号码 + 是否已认领」（用户要求能直接从状态里看到权限归属），
     *   为此把网络/信号/NTP 的标签压短、模板只显示"已设置"（模板原文在面板/详情里）。
     *   关键词最长显示 8 个字符，避免把整条撑爆。
     */
    char kw[16];
    (void)snprintf(kw, sizeof(kw), "%.8s",
                   (v && v->keyword && v->keyword[0]) ? v->keyword : "未设");
    return snprintf(out, out_size,
                    "网:%s 信:%s 时:%s\n"
                    "U:%s T:%s K:%s\n"
                    "M:%s 收%lu/转%lu\n"
                    "管:%s %s",
                    (v && v->net) ? v->net : "-",
                    (v && v->signal) ? v->signal : "-",
                    (v && v->ntp) ? v->ntp : "-",
                    (v && v->url && v->url[0]) ? "已设" : "未设",
                    (v && v->secret && v->secret[0]) ? "已设" : "未设",
                    kw,
                    (v && v->tmpl && v->tmpl[0]) ? "已设" : "未设",
                    (unsigned long)(v ? v->recv_count : 0u),
                    (unsigned long)(v ? v->fwd_count : 0u),
                    (v && v->admin && v->admin[0]) ? v->admin : "-",
                    (v && v->admin_claimed) ? "已认领" : "未认领");
}

int sms_cmd_build_info(const sms_cfg_view_t *v, char *out, size_t out_size)
{
    /*
     * ★ 一条短信装得下（≤70 个 UCS2 字符）：只放地址。
     *   最坏情况：IPv6 全展开 39 字符 → "IP4:"+15+"\n" + "IP6:"+39+"\n" = 64 字符，
     *   再放一行"详情已发机器人"（7）会到 71，所以按实际长度决定是否附加该行。
     *   完整详情（含 http://[<ip6>]:<port>/ 面板地址）由 `webhook_build_note_rcfg()`
     *   推到钉钉机器人。
     */
    const char *ip4 = (v && v->ip4 && v->ip4[0]) ? v->ip4 : "(无)";
    const char *ip6 = (v && v->ip6 && v->ip6[0]) ? v->ip6 : "(无)";
    int n = snprintf(out, out_size, "IP4:%s\nIP6:%s", ip4, ip6);

    if (n <= 0) return -1;
    /* 剩余空间够就补一行提示（数 UTF-8 字符而不是字节） */
    {
        size_t i;
        size_t chars = 0u;
        for (i = 0; out[i] != '\0'; ++i) {
            if (((unsigned char)out[i] & 0xC0u) != 0x80u) ++chars;
        }
        if (chars + 1u + 7u <= SMS_TX_PLAIN_PER_PART) {
            int m = snprintf(out + n, out_size - (size_t)n, "\n详情已发机器人");
            if (m > 0) n += m;
        }
    }
    return n;
}

int sms_cmd_build_menu(const sms_cfg_view_t *v, char *out, size_t out_size)
{
    /*
     * ★★ 一条短信装完（≤70 个 UCS2 字符）——这是反复实测后唯一稳定的形态。
     *
     * 证据（全部来自上板 diag）：
     *   同一回执拆成多条发送时，**只有第 1 条能出去**，无论条间隔是 1 秒 / 8 秒 /
     *   11 秒 / 22 秒，第 2 条起一律 `ret=-1`；唯一一次两条都成功（v11）是
     *   "两条 127~147 字节的独立回执"，而失败的都是 156~163 字节的连续分片。
     *   结论：不能依赖连发，**每条回执必须是 1 条短信**。
     *   因此这里把 5 条指令压到 67 字符；模板占位符细节与实时状态分别用
     *   HELP / STATUS 查询（各自也是单条回执）。
     */
    /*
     * ★★ 一条短信装完（≤70 个 UCS2 字符）——列表包含**全部**指令，共 55 字符。
     *   详细说明（每条指令的含义）由钉钉机器人推送，见 sms_svc 的 want_menu 分支。
     */
    int n = snprintf(out, out_size,
                     "配置:U网址 T密钥 K词 M模板\n"
                     "SAVE STATUS IP 面板\n"
                     "A管理员号 RESET\n"
                     "我要成为管理员");
    if (n < 0) return -1;
    (void)v;
    return n;
}

int sms_cmd_build_help(char *out, size_t out_size)
{
    (void)out; (void)out_size;
    return -1;      /* 不使用：占位符说明改为在 STATUS 里回显当前模板 */
}

int sms_cmd_build_ack(sms_cmd_kind_t k, const char *arg, const sms_cfg_view_t *v,
                      char *out, size_t out_size)
{
    (void)arg;
    switch (k) {
    case SMS_CMD_SET_URL:      return snprintf(out, out_size, "OK U");
    case SMS_CMD_SET_SECRET:   return snprintf(out, out_size, "OK T");
    case SMS_CMD_SET_KEYWORD:  return snprintf(out, out_size, "OK K");
    case SMS_CMD_SET_TEMPLATE: return snprintf(out, out_size, "OK M");
    case SMS_CMD_SAVE:         return snprintf(out, out_size, "OK SAVED");
    case SMS_CMD_RESET:        return snprintf(out, out_size, "OK RESET");
    case SMS_CMD_STATUS:       return sms_cmd_build_status(v, out, out_size);
    case SMS_CMD_INFO:         return sms_cmd_build_info(v, out, out_size);
    case SMS_CMD_MENU:         return sms_cmd_build_menu(v, out, out_size);
    case SMS_CMD_SET_ADMIN:    return snprintf(out, out_size, "OK ADMIN %s",
                                                (arg != NULL) ? arg : "");
    case SMS_CMD_ERR:          return snprintf(out, out_size, "ERR 无法识别的指令");
    default:                   return 0;
    }
}




