/**
 * @file    sms_admin.h
 * @brief   管理员短信指令路由（纯逻辑，无 SDK 依赖，可主机单元测试）。
 *
 * 把「收到一条（可能已重组的）短信」到「该做什么」的决策全部收在这里：
 *   - 非管理员号码：is_admin = 0，调用方走转发流程，**绝不执行配置**；
 *   - 管理员号码  ：解析多行指令 -> 逐条应用到 sms_remote_cfg_t -> 生成回执，
 *                   并给出「是否要落盘」「是否要重启」两个动作标志。
 *
 * 这样 sms_svc 只剩"收 / 发 / 调度"，决策逻辑可以在 PC 上测个遍。
 * 依据：docs/09-短信远程配置与钉钉转发-实现规格.md 第一节。
 */
#ifndef SMS_ADMIN_H
#define SMS_ADMIN_H

#include <stdint.h>
#include <stddef.h>
#include "sms_cmd.h"
#include "sms_rcfg.h"

/** 路由结果 */
typedef struct {
    int  is_admin;      /**< 1 = 发件人是配置管理员（本条不转发） */
    int  changed;       /**< 1 = 远程配置被修改（调用方需刷新视图） */
    int  need_save;     /**< 1 = 收到 SAVE，应落盘 rcfg.bin */
    int  need_reboot;   /**< 1 = 收到 RESET，回执发完后应重启 */
    int  want_info;     /**< 1 = 收到 IP/详细信息：回执发完后把完整详情推到机器人 */
    int  want_panel;    /**< 1 = 收到 面板/PANEL：把面板状态页内容推机器人 */
    int  want_menu;     /**< 1 = 收到 我要配置：把指令详细说明推机器人 */
    int  reply_len;     /**< >0 = 需回复 reply 文本；0 = 无需回复 */
    char reply[SMS_CMD_REPLY_MAX];
} sms_admin_out_t;

/**
 * @brief 处理一条来信。
 *
 * @param rcfg   远程配置（**就地修改**；调用方需持有唯一副本并串行调用）
 * @param sender 发件号码（原始形式，可带 +86）
 * @param text   完整正文（UTF-8，可多行）
 * @param view   渲染菜单/状态用的视图；传 NULL 时用 rcfg 生成一个占位视图
 * @param out    结果（不可为 NULL）
 * @return out->is_admin：1 = 已按管理员指令处理（不转发）；0 = 非管理员（应转发）
 *
 * @note 纯函数（除写入 rcfg/out 外无副作用）：不落盘、不重启、不发短信，
 *       这些动作由调用方按 out 的标志执行。
 */
int sms_admin_handle(sms_remote_cfg_t *rcfg, const char *sender, const char *text,
                     const sms_cfg_view_t *view, sms_admin_out_t *out);

#endif /* SMS_ADMIN_H */


