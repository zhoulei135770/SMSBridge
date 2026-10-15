/**
 * @file    sms_admin.c
 * @brief   管理员短信指令路由实现（见 inc/sms_admin.h）。
 */
#include "sms_admin.h"

#include <string.h>
#include <stdio.h>

/** 指令简写标签（仅用于错误回执） */
static const char *kind_tag(sms_cmd_kind_t k)
{
    switch (k) {
    case SMS_CMD_SET_URL:      return "U";
    case SMS_CMD_SET_SECRET:   return "T";
    case SMS_CMD_SET_KEYWORD:  return "K";
    case SMS_CMD_SET_TEMPLATE: return "M";
    case SMS_CMD_SAVE:         return "SAVE";
    case SMS_CMD_STATUS:       return "STATUS";
    case SMS_CMD_MENU:         return "MENU";
    case SMS_CMD_RESET:        return "RESET";
    default:                   return "CMD";
    }
}

/**
 * @brief 追加一段回执文本（段间以 '\n' 分隔）。
 * @return 0 完整追加；-1 空间不足（已截断，缓冲区仍以 '\0' 结尾）
 */
static int append_reply(char *buf, size_t cap, size_t *len, const char *text)
{
    size_t n;

    if (buf == NULL || len == NULL || cap == 0u) return -1;
    if (text == NULL || text[0] == '\0') return 0;

    if (*len > 0u) {
        if (*len + 2u > cap) return -1;
        buf[(*len)++] = '\n';
    }

    n = strlen(text);
    if (*len + n + 1u > cap) {
        n = (cap > (*len + 1u)) ? (cap - *len - 1u) : 0u;
        if (n > 0u) memcpy(&buf[*len], text, n);
        *len += n;
        buf[*len] = '\0';
        return -1;
    }
    memcpy(&buf[*len], text, n);
    *len += n;
    buf[*len] = '\0';
    return 0;
}

/** 把发件人号码写进配置的管理员字段（去空白、限长） */
static void copy_admin(sms_remote_cfg_t *c, const char *sender)
{
    size_t i = 0u;
    if (c == NULL || sender == NULL) return;
    while (*sender == ' ' || *sender == '\t') ++sender;
    while (sender[i] != '\0' && sender[i] != ' ' && sender[i] != '\t' &&
           i + 1u < sizeof(c->admin)) {
        c->admin[i] = sender[i];
        ++i;
    }
    c->admin[i] = '\0';
}
int sms_admin_handle(sms_remote_cfg_t *rcfg, const char *sender, const char *text,
                     const sms_cfg_view_t *view, sms_admin_out_t *out)
{
    /*
     * ★ 栈预算：短信任务的**有效**栈是 3072 字节（sdk_task_create 里有钳制），
     *   而 sms_cmd_set_t 就有 1572 字节、单条回执缓冲 1200 字节。放栈上必然溢出，
     *   因此都放静态区；本函数只由短信任务串行调用，不存在重入。
     */
    static sms_cmd_set_t s_set;
    static char          s_one[SMS_CMD_REPLY_MAX];
    sms_cfg_view_t local;
    size_t         len = 0u;
    int            n, i;

    if (out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->reply[0] = '\0';

    if (rcfg == NULL || sender == NULL || text == NULL) return 0;

    /*
     * ★ 首次认领（需求）：固件默认管理员可以被"第一条「我要成为管理员」"接管。
     *   仅在 rcfg->admin_claimed == 0 时生效；认领后必须由现任管理员发 A <号码> 更换。
     */
    if (rcfg->admin_claimed == 0u) {
        /* 复用函数级静态 s_set（1572 字节，放栈上会撞 1280 字节栈预算） */
        if (sms_cmd_parse(text, &s_set) == 1 &&
            s_set.kind[0] == SMS_CMD_CLAIM) {
            copy_admin(rcfg, sender);
            rcfg->admin_claimed = 1u;
            out->is_admin  = 1;
            out->changed   = 1;
            out->need_save = 1;
            out->reply_len = snprintf(out->reply, sizeof(out->reply),
                                      "OK 管理员已认领 %s", rcfg->admin);
            return 1;
        }
    }

    /* 非管理员：只转发，绝不执行配置（docs/09 第一节） */
    if (!sms_cmd_is_admin(sender, rcfg->admin)) return 0;
    out->is_admin = 1;

    if (view == NULL) {
        sms_rcfg_make_view(rcfg, "未知", "-", "未对时", 0u, 0u, &local);
        view = &local;
    }

    n = sms_cmd_parse(text, &s_set);
    if (n <= 0) {
        (void)append_reply(out->reply, sizeof(out->reply), &len, "ERR 无法识别的指令");
        out->reply_len = (int)len;
        return 1;
    }

    for (i = 0; i < n; ++i) {
        char *one = s_one;              /* 静态回执缓冲（避免 1200 字节上栈） */
        int   rc;
        int   k = 0;

        /* IP / 详细信息：只回地址摘要，并把"完整详情推机器人"标记给调用方 */
        if (s_set.kind[i] == SMS_CMD_INFO) {
            char *ib = s_one;
            int   ik = sms_cmd_build_info(view, ib, SMS_CMD_REPLY_MAX);
            out->want_info = 1;
            if (ik > 0) (void)append_reply(out->reply, sizeof(out->reply), &len, ib);
            continue;
        }

        /* 面板 / PANEL：短信只回一句确认，内容整份推机器人 */
        if (s_set.kind[i] == SMS_CMD_PANEL) {
            out->want_panel = 1;
            (void)append_reply(out->reply, sizeof(out->reply), &len, "OK 面板内容已推机器人");
            continue;
        }

        /* 我要配置：短信列全部指令（一条装下），详细说明推机器人 */
        if (s_set.kind[i] == SMS_CMD_MENU) {
            out->want_menu = 1;
        }

        switch (s_set.kind[i]) {
        case SMS_CMD_SAVE:  out->need_save   = 1; break;
        case SMS_CMD_RESET: out->need_reboot = 1; break;
        default: break;
        }

        rc = sms_rcfg_apply(rcfg, s_set.kind[i], s_set.arg[i]);
        if (rc > 0) out->changed = 1;

        one[0] = '\0';
        if (s_set.kind[i] == SMS_CMD_ERR) {
            k = snprintf(one, SMS_CMD_REPLY_MAX, "ERR 无法识别的指令");
        } else if (rc < 0) {
            k = snprintf(one, SMS_CMD_REPLY_MAX, "ERR %s", kind_tag(s_set.kind[i]));
        } else {
            k = sms_cmd_build_ack(s_set.kind[i], s_set.arg[i], view, one, SMS_CMD_REPLY_MAX);
        }
        if (k > 0) {
            (void)append_reply(out->reply, sizeof(out->reply), &len, one);
        }
    }

    out->reply_len = (int)len;
    return 1;
}



