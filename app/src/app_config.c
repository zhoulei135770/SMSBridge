/**
 * @file    app_config.c
 * @brief   系统配置：默认值、合法性修正、序列化（magic+version+CRC32）、黑白名单判定。
 *          纯 C99，无 SDK 依赖，可在主机上单元测试。
 */
#include "app_config.h"

#include <string.h>

const char *const g_apn_table[APP_APN_COUNT] = {
    "cmnet",   /* 中国移动 */
    "3gnet",   /* 中国联通 */
    "ctnet"    /* 中国电信 */
};

/* ================================================================== */
/* 小工具                                                              */
/* ================================================================== */

/** 有界字符串拷贝，保证以 '\0' 结尾，返回是否发生截断。 */
static int copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n;
    if (dst == NULL || dst_size == 0u) return 0;
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    n = strlen(src);
    if (n >= dst_size) {
        memcpy(dst, src, dst_size - 1u);
        dst[dst_size - 1u] = '\0';
        return 1;
    }
    memcpy(dst, src, n + 1u);
    return 0;
}

/** 号码规整化：去掉空白、连字符、括号，把 86 前缀与国际前缀统一为可比形式。 */
static void normalize_number(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    const char *p = in;

    if (out_size == 0u) return;
    out[0] = '\0';
    if (in == NULL) return;

    for (; *p != '\0' && n + 1u < out_size; ++p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '-' || c == '(' || c == ')' || c == '+') continue;
        if (c < '0' || c > '9') continue;
        out[n++] = c;
    }
    out[n] = '\0';

    /* 统一去掉 0086 / 86 前缀，便于 "+8613800138000" 与 "13800138000" 互相匹配 */
    if (n > 11u && out[0] == '0' && out[1] == '0') {
        memmove(out, out + 4, n - 3u);        /* 0086 -> 去掉前 4 位 */
        return;
    }
    if (n > 11u && out[0] == '8' && out[1] == '6') {
        memmove(out, out + 2, n - 1u);
    }
}

/* ================================================================== */
/* CRC32（IEEE 802.3，反射实现，与 zlib crc32 结果一致）                */
/* ================================================================== */
uint32_t app_config_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t   i;
    int      b;

    if (data == NULL) return 0u;

    for (i = 0; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        for (b = 0; b < 8; ++b) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ================================================================== */
/* 默认值                                                              */
/* ================================================================== */
void app_config_defaults(app_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));

    /* 网络 / 心跳 */
    cfg->webhook_url[0]  = '\0';
    cfg->webhook_type    = (uint8_t)APP_WEBHOOK_AUTO;
    cfg->webhook_enable  = 1u;
    copy_str(cfg->ping_target, sizeof(cfg->ping_target), "www.baidu.com");
    cfg->ping_port          = 80u;
    cfg->heartbeat_ms       = 300000u;   /* 5 分钟 */
    cfg->heartbeat_fail_limit = 3u;

    /* Web 登录：出厂默认口令 admin123，仅存 SHA-256 摘要（见 tools/hash_pwd.py） */
    copy_str(cfg->pass_sha256, sizeof(cfg->pass_sha256),
             "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9");
    copy_str(cfg->pass_hint, sizeof(cfg->pass_hint), "admin123");

    /* 黑白名单：默认都为空（白名单为空表示不启用白名单，全部放行） */
    cfg->white_count = 0u;
    cfg->black_count = 0u;

    /* 流量：默认不限量、仅告警、60 秒采样 */
    cfg->traffic_limit_mb   = 0u;
    cfg->traffic_action     = (uint8_t)APP_LIMIT_ACTION_ALERT_ONLY;
    cfg->traffic_sample_sec = 60u;

    /* USB 流量共享：需求 2.2 默认启用 */
    cfg->rndis_enable    = 1u;
    cfg->rndis_apply_now = 1u;

    /* 运行状态 */
    cfg->apn_index       = 0u;
    cfg->traffic_used_kb = 0u;
    cfg->month_tag       = 0u;
    cfg->reset_count     = 0u;

    /* IPv6 / 公网地址（v1.2）：默认开启双栈与公网 IP 查询 */
    cfg->ipv6_enable  = 1u;
    cfg->pubip_enable = 1u;
    copy_str(cfg->pubip_url, sizeof(cfg->pubip_url), "http://ip.3322.net");
}

/* ================================================================== */
/* 合法性修正                                                          */
/* ================================================================== */
int app_config_sanitize(app_config_t *cfg)
{
    app_config_t def;
    int          fixed = 0;

    if (cfg == NULL) return 0;
    app_config_defaults(&def);

    if (cfg->webhook_type > (uint8_t)APP_WEBHOOK_RAW) {
        cfg->webhook_type = def.webhook_type;
        ++fixed;
    }
    if (cfg->webhook_enable > 1u) {
        cfg->webhook_enable = 1u;
        ++fixed;
    }
    if (cfg->ping_port == 0u) {
        cfg->ping_port = def.ping_port;
        ++fixed;
    }
    /* 心跳周期：最小 10s，最大 24h（需求 5.4 允许自定义） */
    if (cfg->heartbeat_ms < 10000u || cfg->heartbeat_ms > 86400000u) {
        cfg->heartbeat_ms = def.heartbeat_ms;
        ++fixed;
    }
    if (cfg->heartbeat_fail_limit == 0u || cfg->heartbeat_fail_limit > 20u) {
        cfg->heartbeat_fail_limit = def.heartbeat_fail_limit;
        ++fixed;
    }
    /* 采样周期：最小 10s，最大 24h */
    if (cfg->traffic_sample_sec < 10u || cfg->traffic_sample_sec > 86400u) {
        cfg->traffic_sample_sec = def.traffic_sample_sec;
        ++fixed;
    }
    if (cfg->traffic_action > (uint8_t)APP_LIMIT_ACTION_ALERT_AND_CUT) {
        cfg->traffic_action = def.traffic_action;
        ++fixed;
    }
    if (cfg->rndis_enable > 1u) {
        cfg->rndis_enable = 1u;
        ++fixed;
    }
    if (cfg->rndis_apply_now > 1u) {
        cfg->rndis_apply_now = def.rndis_apply_now;
        ++fixed;
    }
    if (cfg->black_count > APP_CFG_LIST_MAX) {
        cfg->black_count = APP_CFG_LIST_MAX;
        ++fixed;
    }
    if (cfg->white_count > APP_CFG_LIST_MAX) {
        cfg->white_count = APP_CFG_LIST_MAX;
        ++fixed;
    }
    if (cfg->apn_index >= APP_APN_COUNT) {
        cfg->apn_index = 0u;
        ++fixed;
    }
    /* 口令摘要必须是 64 位十六进制，否则回退默认口令，避免把设备锁死 */
    {
        size_t i;
        int    bad = 0;
        if (strlen(cfg->pass_sha256) != 64u) {
            bad = 1;
        } else {
            for (i = 0; i < 64u; ++i) {
                char c = cfg->pass_sha256[i];
                int  hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex) { bad = 1; break; }
            }
        }
        if (bad) {
            copy_str(cfg->pass_sha256, sizeof(cfg->pass_sha256), def.pass_sha256);
            copy_str(cfg->pass_hint, sizeof(cfg->pass_hint), def.pass_hint);
            ++fixed;
        }
    }
    /* 末尾兜底，确保字符串都以 '\0' 结尾（防止外部改动后越界） */
    cfg->webhook_url[APP_CFG_URL_MAX - 1u]    = '\0';
    cfg->ping_target[APP_CFG_HOST_MAX - 1u]   = '\0';
    cfg->pass_hint[APP_CFG_PASS_MAX - 1u]     = '\0';
    cfg->pass_sha256[APP_CFG_SHA256_HEX - 1u] = '\0';
    {
        int i;
        for (i = 0; i < APP_CFG_LIST_MAX; ++i) {
            cfg->sms_white[i][APP_CFG_NUM_MAX - 1u] = '\0';
            cfg->sms_black[i][APP_CFG_NUM_MAX - 1u] = '\0';
        }
    }
    /* 限额上限保护：100TB 视为误输入 */
    if (cfg->traffic_limit_mb > 100u * 1024u * 1024u) {
        cfg->traffic_limit_mb = def.traffic_limit_mb;
        ++fixed;
    }
    /* IPv6 / 公网 IP 回显（v1.2） */
    if (cfg->ipv6_enable > 1u) {
        cfg->ipv6_enable = def.ipv6_enable;
        ++fixed;
    }
    if (cfg->pubip_enable > 1u) {
        cfg->pubip_enable = def.pubip_enable;
        ++fixed;
    }
    /* 回显地址必须带 scheme，否则 HTTP 客户端无法解析；空串表示不查询 */
    if (cfg->pubip_url[0] != '\0' &&
        strncmp(cfg->pubip_url, "http://", 7u) != 0 &&
        strncmp(cfg->pubip_url, "https://", 8u) != 0) {
        copy_str(cfg->pubip_url, sizeof(cfg->pubip_url), def.pubip_url);
        ++fixed;
    }
    cfg->pubip_url[APP_CFG_PUBIP_URL_MAX - 1u] = '\0';
    return fixed;
}

/* ================================================================== */
/* 序列化 / 反序列化                                                   */
/* ================================================================== */
size_t app_config_serialize(const app_config_t *cfg, void *out, size_t out_size)
{
    app_config_blob_t *blob = (app_config_blob_t *)out;

    if (cfg == NULL || out == NULL) return 0u;
    if (out_size < sizeof(app_config_blob_t)) return 0u;

    memset(blob, 0, sizeof(*blob));
    blob->magic        = APP_CFG_MAGIC;
    blob->version      = APP_CFG_VERSION;
    blob->payload_size = (uint32_t)sizeof(app_config_t);
    blob->cfg          = *cfg;
    blob->crc32        = app_config_crc32(&blob->cfg, sizeof(app_config_t));

    return sizeof(app_config_blob_t);
}

int app_config_deserialize(const void *in, size_t in_size, app_config_t *cfg)
{
    const app_config_blob_t *blob = (const app_config_blob_t *)in;
    uint32_t                 crc;

    if (in == NULL || cfg == NULL) return -1;
    if (in_size < sizeof(app_config_blob_t)) return -1;
    if (blob->magic != APP_CFG_MAGIC) return -2;
    if (blob->version != APP_CFG_VERSION) return -3;
    if (blob->payload_size != (uint32_t)sizeof(app_config_t)) return -4;

    crc = app_config_crc32(&blob->cfg, sizeof(app_config_t));
    if (crc != blob->crc32) return -5;

    *cfg = blob->cfg;
    return 0;
}

/* ================================================================== */
/* APN 辅助                                                            */
/* ================================================================== */
const char *app_config_apn(const app_config_t *cfg)
{
    uint32_t idx = (cfg != NULL) ? cfg->apn_index : 0u;
    if (idx >= APP_APN_COUNT) idx = 0u;
    return g_apn_table[idx];
}

uint32_t app_config_apn_next(app_config_t *cfg)
{
    if (cfg == NULL) return 0u;
    cfg->apn_index = (cfg->apn_index + 1u) % APP_APN_COUNT;
    return cfg->apn_index;
}

void app_config_month_tag(char *out)
{
    /* 说明：模组本地时间由 sdk_port 提供；这里只做占位实现，
     * 真正的 YYYYMM 由调用方用 sdk_rtc_now() 填好后覆盖。
     * 保留此函数是为了让黑白名单之外的时间相关逻辑集中在同一处。 */
    if (out == NULL) return;
    out[0] = '0';
    out[1] = '0';
    out[2] = '0';
    out[3] = '0';
    out[4] = '0';
    out[5] = '0';
    out[6] = '\0';
}

/* ================================================================== */
/* 黑白名单                                                            */
/* ================================================================== */
/** 在名单里查找号码，命中返回下标，未命中返回 -1。 */
static int list_find(const char list[][APP_CFG_NUM_MAX], uint8_t count, const char *number)
{
    char   target[APP_CFG_NUM_MAX];
    char   item[APP_CFG_NUM_MAX];
    int    i;

    normalize_number(number, target, sizeof(target));
    if (target[0] == '\0') return -1;

    for (i = 0; i < (int)count && i < APP_CFG_LIST_MAX; ++i) {
        if (list[i][0] == '\0') continue;
        normalize_number(list[i], item, sizeof(item));
        if (item[0] == '\0') continue;
        /* 支持后缀匹配：白名单里填 "13800138000" 也能匹配 "+8613800138000" */
        {
            size_t lt = strlen(target);
            size_t li = strlen(item);
            if (li == lt && memcmp(target, item, lt) == 0) return i;
            if (li < lt && memcmp(target + (lt - li), item, li) == 0) return i;
            if (lt < li && memcmp(target, item + (li - lt), lt) == 0) return i;
        }
    }
    return -1;
}

int app_config_in_whitelist(const app_config_t *cfg, const char *number)
{
    if (cfg == NULL) return 0;
    return list_find((const char (*)[APP_CFG_NUM_MAX])cfg->sms_white, cfg->white_count, number) >= 0;
}

int app_config_in_blacklist(const app_config_t *cfg, const char *number)
{
    if (cfg == NULL) return 0;
    return list_find((const char (*)[APP_CFG_NUM_MAX])cfg->sms_black, cfg->black_count, number) >= 0;
}

int app_config_sms_allowed(const app_config_t *cfg, const char *number, const char **reason)
{
    const char *why = "pass";

    if (cfg == NULL) {
        if (reason != NULL) *reason = "no config";
        return 0;
    }
    if (number == NULL || number[0] == '\0') {
        if (reason != NULL) *reason = "empty number";
        return 0;
    }

    /* 1) 黑名单优先拦截（需求 3.3：黑名单内号码直接拦截，不推送） */
    if (cfg->black_count > 0u && app_config_in_blacklist(cfg, number)) {
        why = "blacklist";
        if (reason != NULL) *reason = why;
        return 0;
    }

    /* 2) 白名单非空时，仅白名单放行（白名单为空不启用白名单） */
    if (cfg->white_count > 0u && !app_config_in_whitelist(cfg, number)) {
        why = "not in whitelist";
        if (reason != NULL) *reason = why;
        return 0;
    }

    if (reason != NULL) *reason = why;
    return 1;
}
