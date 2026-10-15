/**
 * @file    sms_rcfg.c
 * @brief   短信远程配置持久化与指令应用实现（见 inc/sms_rcfg.h）。
 */
#include "sms_rcfg.h"
#include "sdk_port.h"      /* sdk_file_read / sdk_file_write（主机测试里由桩函数替代） */
#include <string.h>

/* 编译期契约：SMS_RCFG_CFG_SIZE / SMS_RCFG_BLOB_SIZE 必须与真实结构体一致。
 * 若结构体里加了字段却忘了改尺寸宏，这行会直接编译失败（而不是现场越界）。 */
typedef char sms_rcfg_size_contract[(sizeof(sms_remote_cfg_t) == SMS_RCFG_CFG_SIZE) ? 1 : -1];

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t len;              /* 后续负载长度 */
    sms_remote_cfg_t cfg;
    uint32_t checksum;         /* 负载字节异或 + 长度混合，简单可靠 */
} sms_rcfg_blob_t;

/* 落盘布局也必须与对外公布的尺寸宏一致（含对齐/填充），否则读写缓冲会不够 */
typedef char sms_rcfg_blob_contract[(sizeof(sms_rcfg_blob_t) == SMS_RCFG_BLOB_SIZE) ? 1 : -1];

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n;
    if (dst_size == 0u) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= dst_size) n = dst_size - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void sms_rcfg_defaults(sms_remote_cfg_t *c)
{
    if (c == NULL) return;
    memset(c, 0, sizeof(*c));
    copy_str(c->tmpl,  sizeof(c->tmpl),  "{from}|{time}|{text}");
    /*
     * ★ 出厂**不预设管理员**（按用户要求）：第一任管理员 = 第一条
     *   「我要成为管理员」短信的发件人（见 sms_admin.c 的认领逻辑）。
     *   在有人认领之前，任何号码都没有配置权限（普通短信仍按关键词转发）。
     */
    c->admin[0]      = '\0';
    c->admin_claimed = 0u;
    copy_str(c->ntp,   sizeof(c->ntp),   "ntp.aliyun.com");
    c->concat_enable = 1u;
}

static uint32_t rcfg_checksum(const sms_remote_cfg_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint32_t       x = 0x5A5A5A5Au;
    size_t         i;
    for (i = 0; i < sizeof(*c); ++i) {
        x = (x << 5) ^ (x >> 27) ^ (uint32_t)p[i];
    }
    return x ^ (uint32_t)sizeof(*c);
}

int sms_rcfg_serialize(const sms_remote_cfg_t *c, void *out, size_t out_size)
{
    sms_rcfg_blob_t b;

    if (c == NULL || out == NULL || out_size < sizeof(b)) return -1;

    memset(&b, 0, sizeof(b));
    b.magic    = SMS_RCFG_MAGIC;
    b.version  = (uint16_t)SMS_RCFG_VERSION;
    b.len      = (uint16_t)sizeof(sms_remote_cfg_t);
    b.cfg      = *c;
    b.checksum = rcfg_checksum(c);

    memcpy(out, &b, sizeof(b));
    return (int)sizeof(b);
}

int sms_rcfg_deserialize(const void *in, size_t in_len, sms_remote_cfg_t *c)
{
    sms_rcfg_blob_t b;

    if (in == NULL || c == NULL || in_len < sizeof(b)) return -1;
    memcpy(&b, in, sizeof(b));

    if (b.magic != SMS_RCFG_MAGIC) return -2;
    if (b.version != (uint16_t)SMS_RCFG_VERSION) return -3;
    if (b.len != (uint16_t)sizeof(sms_remote_cfg_t)) return -4;
    if (b.checksum != rcfg_checksum(&b.cfg)) return -5;

    /* 强制字符串终止，防止外部数据未带 '\0' */
    b.cfg.url[sizeof(b.cfg.url) - 1u]             = '\0';
    b.cfg.secret[sizeof(b.cfg.secret) - 1u]       = '\0';
    b.cfg.keyword[sizeof(b.cfg.keyword) - 1u]     = '\0';
    b.cfg.tmpl[sizeof(b.cfg.tmpl) - 1u]           = '\0';
    b.cfg.admin[sizeof(b.cfg.admin) - 1u]         = '\0';
    b.cfg.ntp[sizeof(b.cfg.ntp) - 1u]             = '\0';

    *c = b.cfg;
    return 0;
}

int sms_rcfg_apply(sms_remote_cfg_t *c, sms_cmd_kind_t kind, const char *arg)
{
    if (c == NULL) return -1;

    switch (kind) {
    case SMS_CMD_SET_ADMIN:
        /*
         * 更换管理员号码：只接受 6~20 位数字（允许前置 '+' 与空格）。
         * 手工指定即视为"已认领"，避免之后被别人的「我要成为管理员」抢走。
         */
        if (arg == NULL || arg[0] == '\0') return -1;
        {
            const char *p = arg;
            int         digits = 0;

            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '+') ++p;
            while (*p >= '0' && *p <= '9') { ++p; ++digits; }
            if (digits < 6 || digits > 20) return -2;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '\0') return -2;
            if (strlen(arg) >= SMS_RCFG_ADMIN_MAX) return -2;
            copy_str(c->admin, sizeof(c->admin), arg);
            c->admin_claimed = 1u;
            return 1;
        }

    case SMS_CMD_SET_URL:
        if (arg == NULL || arg[0] == '\0') return -1;
        if (arg[0] == '-' && arg[1] == '\0') { c->url[0] = '\0'; return 1; }
        if (strlen(arg) >= SMS_RCFG_URL_MAX) return -2;
        copy_str(c->url, sizeof(c->url), arg);
        return 1;

    case SMS_CMD_SET_SECRET:
        if (arg == NULL || arg[0] == '\0') return -1;
        if (arg[0] == '-' && arg[1] == '\0') { c->secret[0] = '\0'; return 1; }
        if (strlen(arg) >= SMS_RCFG_SECRET_MAX) return -2;
        copy_str(c->secret, sizeof(c->secret), arg);
        return 1;

    case SMS_CMD_SET_KEYWORD:
        if (arg == NULL || arg[0] == '\0') return -1;
        if (arg[0] == '-' && arg[1] == '\0') { c->keyword[0] = '\0'; return 1; }
        if (strlen(arg) >= SMS_RCFG_KEYWORD_MAX) return -2;
        copy_str(c->keyword, sizeof(c->keyword), arg);
        return 1;

    case SMS_CMD_SET_TEMPLATE:
        if (arg == NULL || arg[0] == '\0') return -1;
        if (arg[0] == '-' && arg[1] == '\0') { copy_str(c->tmpl, sizeof(c->tmpl), "{from}|{time}|{text}"); return 1; }
        if (strlen(arg) >= SMS_RCFG_TMPL_MAX) return -2;
        copy_str(c->tmpl, sizeof(c->tmpl), arg);
        return 1;

    case SMS_CMD_SAVE:
    case SMS_CMD_STATUS:
    case SMS_CMD_MENU:
    case SMS_CMD_RESET:
        return 0;                       /* 不改配置，由调用方处理动作 */

    default:
        return -3;                      /* 不支持的指令（含 ERR） */
    }
}

void sms_rcfg_make_view(const sms_remote_cfg_t *c, const char *net, const char *signal,
                        const char *ntp_state, uint32_t recv, uint32_t fwd,
                        sms_cfg_view_t *v)
{
    if (v == NULL) return;
    memset(v, 0, sizeof(*v));
    if (c == NULL) return;
    v->admin        = c->admin;
    v->admin_claimed = c->admin_claimed;
    v->url        = c->url;
    v->secret     = c->secret;
    v->keyword    = c->keyword;
    v->tmpl       = c->tmpl;
    v->net        = net;
    v->signal     = signal;
    v->ntp        = ntp_state;
    v->recv_count = recv;
    v->fwd_count  = fwd;
}

/* ==================================================================== */
/* 落盘（rcfg.bin）                                                      */
/* ==================================================================== */
/*
 * 576 字节的 blob 放栈上会吃掉短信任务（栈预算 3072）近 1/5，
 * 因此用模块级静态缓冲；调用点只有"启动时载入"与"短信任务收到 SAVE"，
 * 二者不会并发（短信任务在启动流程之后才创建），故不加重入保护。
 */
static uint8_t s_rcfg_io_buf[SMS_RCFG_BLOB_SIZE];

int sms_rcfg_restore(const void *blob, int32_t blob_len, sms_remote_cfg_t *c)
{
    if (c == NULL) return -1;

    /* 文件不存在、读失败、掉电半截写：一律回退默认值 */
    if (blob == NULL || blob_len < (int32_t)sizeof(sms_rcfg_blob_t)) {
        sms_rcfg_defaults(c);
        return 0;
    }
    if (sms_rcfg_deserialize(blob, (size_t)blob_len, c) != 0) {
        sms_rcfg_defaults(c);
        return 0;
    }
    return 1;
}

int sms_rcfg_save(const sms_remote_cfg_t *c)
{
    int      n;
    int32_t  w;

    if (c == NULL) return -1;

    n = sms_rcfg_serialize(c, s_rcfg_io_buf, sizeof(s_rcfg_io_buf));
    if (n != (int)SMS_RCFG_BLOB_SIZE) return -2;   /* 尺寸契约被破坏 */

    w = sdk_file_write(SMS_RCFG_FILE, s_rcfg_io_buf, (uint32_t)n);
    if (w != (int32_t)n) return -3;
    return 0;
}

int sms_rcfg_load(sms_remote_cfg_t *c)
{
    int32_t n;

    if (c == NULL) return -1;
    n = sdk_file_read(SMS_RCFG_FILE, s_rcfg_io_buf, (uint32_t)sizeof(s_rcfg_io_buf));
    return sms_rcfg_restore(s_rcfg_io_buf, n, c);
}


