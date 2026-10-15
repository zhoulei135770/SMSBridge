/**
 * @file    app_config.h
 * @brief   系统配置结构体、默认值、序列化与校验（纯 C99，无 SDK 依赖）。
 *
 * 对应需求文档「6. 配置持久化存储（Flash）」：所有可配置项集中在一个结构体内，
 * 通过 sdk_port 的 sdk_cfg_load()/sdk_cfg_save() 落盘，附带 magic + version + CRC32
 * 三重校验，避免掉电写入产生半截配置导致设备变砖。
 *
 * 说明（与原需求文档的差异）：
 *   - 需求文档写的是「片内 Flash 开辟 4KB 扇区裸写」。实际 SDK 只对外暴露
 *     文件系统 cm_fs.h（无目录），不暴露裸扇区擦写接口。因此本模块把结构体
 *     整体作为一条记录写入文件系统中的一个文件（config.bin / config.bak），
 *     擦写次数由文件系统管理，逻辑上等价于「仅在保存/重置/超限时写」。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define APP_CFG_MAGIC        0x4D4C3337u  /* 'ML37' */
#define APP_CFG_VERSION      0x00010002u  /* v1.2：追加 IPv6 / 公网 IP 回显字段 */

#define APP_CFG_URL_MAX      192
#define APP_CFG_HOST_MAX     64
#define APP_CFG_PASS_MAX     33          /* 32 字符 + '\0' */
#define APP_CFG_SHA256_HEX   65          /* 64 hex + '\0' */
#define APP_CFG_NUM_MAX      24
#define APP_CFG_LIST_MAX     8           /* 黑白名单各最多 8 个号码（需求 5.4） */
#define APP_CFG_PUBIP_URL_MAX 96         /* 公网 IP 回显服务地址 */

/* APN 候选列表：需求 3.1 指定 cmnet / 3gnet / ctnet */
#define APP_APN_COUNT        3

/* 超限动作：需求 4.2 */
typedef enum {
    APP_LIMIT_ACTION_ALERT_ONLY = 0,   /* 仅告警 */
    APP_LIMIT_ACTION_ALERT_AND_CUT = 1 /* 告警 + 断开 PDP 断网 */
} app_limit_action_t;

/* Webhook 类型：影响 Markdown 消息体格式（企业微信 / 钉钉字段名不同） */
typedef enum {
    APP_WEBHOOK_AUTO = 0,      /* 按 URL 关键字自动识别 */
    APP_WEBHOOK_WECOM = 1,     /* 企业微信机器人 */
    APP_WEBHOOK_DINGTALK = 2,  /* 钉钉机器人 */
    APP_WEBHOOK_RAW = 3        /* 通用 JSON（{"text": "..."}），便于对接自建服务 */
} app_webhook_type_t;

/**
 * @brief 系统配置。字段顺序即为序列化顺序，**不要**在中间插字段，
 *        只在末尾追加并递增 APP_CFG_VERSION。
 */
typedef struct {
    /* ---- 网络 ---- */
    char     webhook_url[APP_CFG_URL_MAX];   /* Webhook 地址（企业微信/钉钉） */
    uint8_t  webhook_type;                   /* app_webhook_type_t */
    uint8_t  webhook_enable;                 /* 0=关闭推送 1=开启 */
    char     ping_target[APP_CFG_HOST_MAX];  /* 心跳目标（域名或 IP） */
    uint16_t ping_port;                      /* 心跳探测端口，默认 80 */
    uint32_t heartbeat_ms;                   /* 心跳周期(ms)，默认 300000 */
    uint16_t heartbeat_fail_limit;           /* 连续失败几次判定网络失效，默认 3 */

    /* ---- Web 登录 ---- */
    char     pass_sha256[APP_CFG_SHA256_HEX];/* 登录口令的 SHA-256 十六进制摘要 */
    char     pass_hint[APP_CFG_PASS_MAX];    /* 明文口令（仅用于回显提示，可为空） */

    /* ---- 短信过滤（需求 3.3） ---- */
    uint8_t  white_count;
    uint8_t  black_count;
    uint8_t  reserved0[2];
    char     sms_white[APP_CFG_LIST_MAX][APP_CFG_NUM_MAX]; /* 白名单：非空时仅白名单放行 */
    char     sms_black[APP_CFG_LIST_MAX][APP_CFG_NUM_MAX]; /* 黑名单：命中直接拦截 */

    /* ---- 流量管理（需求 4.2） ---- */
    uint32_t traffic_limit_mb;               /* 月度限额(MB)，0 = 不限制 */
    uint8_t  traffic_action;                 /* app_limit_action_t */
    uint8_t  reserved1[3];
    uint32_t traffic_sample_sec;             /* 采样周期(秒)，默认 60 */

    /* ---- USB 流量共享（需求 2） ---- */
    uint8_t  rndis_enable;                   /* 0=关闭 1=开启（默认 1） */
    uint8_t  rndis_apply_now;                /* 1=即时生效 0=重启生效（运行时切换是否可用） */
    uint8_t  reserved2[2];

    /* ---- 持久化的运行状态（随配置一起落盘） ---- */
    uint32_t apn_index;                      /* 当前生效的 APN 下标 */
    uint32_t traffic_used_kb;                /* 本机统计的月度已用(KB)，用于交叉校验 */
    uint32_t month_tag;                      /* 本月标记 YYYYMM，用于跨月自动清零 */
    uint32_t reset_count;                    /* 流量重置次数（诊断用） */

    /* ---- IPv6 与公网地址（v1.2 追加，务必只在末尾加字段） ---- */
    uint8_t  ipv6_enable;                    /* 1=用 IPV4V6 双栈激活 PDP（默认 1） */
    uint8_t  pubip_enable;                   /* 1=定期向回显服务查询公网 IPv4（默认 1） */
    uint8_t  reserved3[2];
    char     pubip_url[APP_CFG_PUBIP_URL_MAX];/* 公网 IP 回显服务（返回纯文本或 JSON） */
} app_config_t;

/** 落盘封装：magic + version + payload + crc32 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    uint32_t crc32;
    app_config_t cfg;
} app_config_blob_t;

/** APN 候选表（需求 3.1）。索引与 app_config_t.apn_index 对应。 */
extern const char *const g_apn_table[APP_APN_COUNT];

/**
 * @brief 用默认值填充配置。
 * @note 默认启用 RNDIS（需求 2.2 默认策略）。
 */
void app_config_defaults(app_config_t *cfg);

/**
 * @brief 校验配置合法性，非法字段就地修正为默认值。
 * @return 修正过的字段个数（0 表示本来就是合法的）。
 */
int app_config_sanitize(app_config_t *cfg);

/** @brief 计算 payload 的 CRC32（IEEE 802.3 多项式，与 zlib crc32 一致）。 */
uint32_t app_config_crc32(const void *data, size_t len);

/**
 * @brief 序列化为带校验头的 blob。
 * @return 写入的字节数，0 表示空间不足。
 */
size_t app_config_serialize(const app_config_t *cfg, void *out, size_t out_size);

/**
 * @brief 从 blob 反序列化。
 * @return 0 成功；<0 表示 magic/version/CRC/长度 任一校验失败。
 */
int app_config_deserialize(const void *in, size_t in_size, app_config_t *cfg);

/** @brief 查询当前 APN 名称（越界返回第一个）。 */
const char *app_config_apn(const app_config_t *cfg);

/** @brief 切到下一个 APN，返回新的下标。 */
uint32_t app_config_apn_next(app_config_t *cfg);

/** @brief 把 "YYYYMM" 形式的本月标记写入 out（需 7 字节）。 */
void app_config_month_tag(char *out);

/** @brief 号码是否命中白名单（白名单为空视为不启用，返回 0）。 */
int app_config_in_whitelist(const app_config_t *cfg, const char *number);

/** @brief 号码是否命中黑名单。 */
int app_config_in_blacklist(const app_config_t *cfg, const char *number);

/**
 * @brief 综合黑白名单判定（需求 3.3 的语义）。
 * @return 1 = 允许转发；0 = 拦截。reason 输出可读原因，可为 NULL。
 */
int app_config_sms_allowed(const app_config_t *cfg, const char *number, const char **reason);

#endif /* APP_CONFIG_H */
