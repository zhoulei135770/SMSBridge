/**
 * @file    sms_cmd.h
 * @brief   短信远程配置指令解析（纯逻辑，无 SDK 依赖，可主机单元测试）。
 *
 * 触发与指令集见 docs/09-短信远程配置与钉钉转发-实现规格.md：
 *   触发：管理员号码发来含「我要配置」=> 回模板 + 当前状态
 *   指令：U <url> / T <secret> / K <keyword> / M <template> / SAVE / STATUS / RESET
 *   回填支持一条短信多行（每行一条指令）
 */
#ifndef SMS_CMD_H
#define SMS_CMD_H

#include <stdint.h>
#include <stddef.h>

#define SMS_CMD_MAX        8u     /**< 单条短信最多解析出的指令数 */
#define SMS_CMD_ARG_MAX    192u   /**< 单个取值上限（URL 可能较长） */
#define SMS_CMD_REPLY_MAX  1200u  /**< 回执文本上限（配合拼接短信发送） */

typedef enum {
    SMS_CMD_NONE = 0,
    SMS_CMD_SET_URL,        /**< U <url> */
    SMS_CMD_SET_SECRET,     /**< T <secret>，"-" 表示清空 */
    SMS_CMD_SET_KEYWORD,    /**< K <keyword>，"-" 表示清空 */
    SMS_CMD_SET_TEMPLATE,   /**< M <template> */
    SMS_CMD_SAVE,           /**< SAVE */
    SMS_CMD_STATUS,         /**< STATUS */
    SMS_CMD_INFO,           /**< IP / INFO / 详细信息：短信回地址摘要 + 机器人推完整详情 */
    SMS_CMD_PANEL,          /**< 面板 / PANEL：把面板状态页内容渲染成文本推机器人 */
    SMS_CMD_SET_ADMIN,      /**< A <号码> / ADMIN <号码>：更换管理员号码 */
    SMS_CMD_CLAIM,          /**< 我要成为管理员：首次认领（仅在未认领时有效） */
    SMS_CMD_MENU,           /**< 我要配置 */
    SMS_CMD_RESET,          /**< RESET */
    SMS_CMD_ERR             /**< 语法错误 */
} sms_cmd_kind_t;

typedef struct {
    sms_cmd_kind_t kind[SMS_CMD_MAX];
    char           arg[SMS_CMD_MAX][SMS_CMD_ARG_MAX];
    int            count;
} sms_cmd_set_t;

/** 当前配置与状态视图（用于渲染回执，全部由调用方提供） */
typedef struct {
    const char *url;
    const char *secret;
    const char *keyword;
    const char *tmpl;
    const char *net;        /**< 例如 "已注册 CHINA MOBILE" */
    const char *signal;     /**< 例如 "31" */
    const char *ntp;        /**< 例如 "已对时 2026-10-15 10:20:31" */
    const char *ip4;        /**< PDP 上的 IPv4（可空串） */
    const char *ip6;        /**< PDP 上的 IPv6 全局地址（可空串） */
    const char *panel_port; /**< Web 面板端口字符串，例如 "8080" */
    const char *admin;      /**< 当前管理员号码（STATUS 里显示） */
    uint8_t     admin_claimed; /**< 1 = 管理员已认领 */
    uint32_t    recv_count;
    uint32_t    fwd_count;
} sms_cfg_view_t;

/** 发件人是否为配置管理员（与 admin 比较，允许带/不带 '+' 与前缀 86） */
int sms_cmd_is_admin(const char *sender, const char *admin);

/** 解析多行指令文本；返回指令条数（0 = 无有效指令） */
int sms_cmd_parse(const char *text, sms_cmd_set_t *out);

/** 渲染「我要配置」回执（模板 + 当前状态，配合拼接短信发送） */
int sms_cmd_build_menu(const sms_cfg_view_t *v, char *out, size_t out_size);

/** 渲染单条指令的回执（OK xxx / ERR xxx / 状态文本） */
int sms_cmd_build_ack(sms_cmd_kind_t k, const char *arg, const sms_cfg_view_t *v,
                      char *out, size_t out_size);

/** 渲染状态文本（STATUS 指令与菜单共用） */
int sms_cmd_build_status(const sms_cfg_view_t *v, char *out, size_t out_size);

/**
 * @brief 渲染「IP / 详细信息」回执：**一条短信装得下**的地址摘要。
 *
 * 只放 IP4/IP6 两行（IPv6 全局地址最长 39 字符，加上标签也不超过 70 个 UCS2 字符）；
 * 完整详情（含 Web 面板 URL、网络/对时/配置状态、收发计数）走钉钉机器人推送，
 * 因为多片/多条短信在本底包上不可靠（只有第 1 条能发出去，见 docs/12 §五之三）。
 *
 * @return 写入字节数；<=0 失败
 */
int sms_cmd_build_info(const sms_cfg_view_t *v, char *out, size_t out_size);

#endif /* SMS_CMD_H */

