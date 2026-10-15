/**
 * @file    sms_rcfg.h
 * @brief   短信远程配置的持久化与指令应用（纯逻辑，无 SDK 依赖）。
 *
 * 设计取舍：**不动既有的 app_config（cfg.bin）**，远程配置单独存 rcfg.bin，
 * 避免改动已验证的配置序列化格式，零回归风险。
 */
#ifndef SMS_RCFG_H
#define SMS_RCFG_H

#include <stdint.h>
#include <stddef.h>
#include "sms_cmd.h"

#define SMS_RCFG_MAGIC     0x52434647u   /* "RCFG" */
#define SMS_RCFG_VERSION   1u

#define SMS_RCFG_URL_MAX     192u
#define SMS_RCFG_SECRET_MAX  80u
#define SMS_RCFG_KEYWORD_MAX 40u
#define SMS_RCFG_TMPL_MAX    160u
#define SMS_RCFG_ADMIN_MAX   24u
#define SMS_RCFG_NTP_MAX     64u

/**
 * @brief 序列化后的落盘尺寸（**调用方必须按这个尺寸准备缓冲**）。
 *
 * 结构体内全部是 char 数组与 uint8_t，无内部填充，因此：
 *   SMS_RCFG_CFG_SIZE  = 6 个字符串数组之和 + concat_enable(1) + admin_claimed(1) + reserved(2) = 564
 *   SMS_RCFG_BLOB_SIZE = magic(4) + version(2) + len(2) + payload(564) + checksum(4) = 576
 *
 * 注意：曾经的验证夹具用了 512 字节缓冲，导致 sms_rcfg_serialize() 返回 -1，
 * 负返回值又被当成 size_t 长度 → 段错误。落盘/载入缓冲一律用本宏。
 * sms_rcfg.c 里有编译期断言保证它与 sizeof(sms_remote_cfg_t) 同步。
 */
#define SMS_RCFG_CFG_SIZE   (SMS_RCFG_URL_MAX + SMS_RCFG_SECRET_MAX + SMS_RCFG_KEYWORD_MAX + \
                             SMS_RCFG_TMPL_MAX + SMS_RCFG_ADMIN_MAX + SMS_RCFG_NTP_MAX + 4u)
#define SMS_RCFG_BLOB_SIZE  (SMS_RCFG_CFG_SIZE + 12u)

/** 远程配置的落盘文件名（独立于 app_config 的 cfg.bin，零回归风险） */
#define SMS_RCFG_FILE       "rcfg.bin"

typedef struct {
    char     url[SMS_RCFG_URL_MAX];
    char     secret[SMS_RCFG_SECRET_MAX];
    char     keyword[SMS_RCFG_KEYWORD_MAX];
    char     tmpl[SMS_RCFG_TMPL_MAX];
    char     admin[SMS_RCFG_ADMIN_MAX];
    char     ntp[SMS_RCFG_NTP_MAX];
    uint8_t  concat_enable;
    uint8_t  admin_claimed;  /* 1 = 管理员已被认领（首次「我要成为管理员」成功后置位） */
    uint8_t  reserved[2];
} sms_remote_cfg_t;

/** 默认值：admin 为空（由「我要成为管理员」认领）、tmpl={from}|{time}|{text}、ntp=ntp.aliyun.com、concat=1 */
void sms_rcfg_defaults(sms_remote_cfg_t *c);

/** 序列化（带头 magic/version/校验和），返回写入长度，<0 失败 */
int  sms_rcfg_serialize(const sms_remote_cfg_t *c, void *out, size_t out_size);

/** 反序列化；magic/版本/校验不符返回负值（调用方应回退默认值） */
int  sms_rcfg_deserialize(const void *in, size_t in_len, sms_remote_cfg_t *c);

/**
 * @brief 把一条解析后的指令应用到配置。
 * @return 1 = 配置已修改；0 = 无需修改（SAVE/STATUS/MENU/RESET）；
 *         <0  = 参数错误（例如空值）
 */
int  sms_rcfg_apply(sms_remote_cfg_t *c, sms_cmd_kind_t kind, const char *arg);

/** 生成状态视图（供 sms_cmd_build_menu/build_status 渲染） */
void sms_rcfg_make_view(const sms_remote_cfg_t *c, const char *net, const char *signal,
                        const char *ntp_state, uint32_t recv, uint32_t fwd,
                        sms_cfg_view_t *v);

/* ==================================================================== */
/* 落盘（rcfg.bin）。逻辑部分纯 C，可主机测试；文件 I/O 走 sdk_port。      */
/* ==================================================================== */

/**
 * @brief 从已读入内存的 blob 恢复配置，**任何异常都回退默认值**。
 *
 * 覆盖现场可能出现的所有坏情况：文件不存在（blob_len < 0）、掉电导致的半截写
 * （长度不足）、magic/版本/校验和不符。调用方无需自己做判断。
 *
 * @return 1 = 已从 blob 恢复；0 = 已填入默认值；<0 = 参数错误（c == NULL）
 */
int  sms_rcfg_restore(const void *blob, int32_t blob_len, sms_remote_cfg_t *c);

/**
 * @brief 把配置落盘到 rcfg.bin（序列化 + sdk_file_write，内部含回读校验）。
 * @return 0 成功；<0 失败（-1 参数错、-2 序列化异常、-3 写入失败）
 */
int  sms_rcfg_save(const sms_remote_cfg_t *c);

/**
 * @brief 从 rcfg.bin 载入配置；文件缺失或损坏时静默回退默认值（不阻塞启动）。
 * @return 1 = 从文件恢复；0 = 使用默认值；<0 = 参数错误
 */
int  sms_rcfg_load(sms_remote_cfg_t *c);

#endif /* SMS_RCFG_H */

