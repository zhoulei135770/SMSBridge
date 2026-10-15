/**
 * @file    web_server.h
 * @brief   内置 Web 管理服务（需求 5）：裸 TCP 微型 HTTP 服务，端口 8080。
 *
 * 实现选型：使用 lwip 标准 socket（lwip_socket/bind/listen/accept/recv/send/select）。
 * 已核实这些符号在 ML307A-DSLN 底包中真实链接存在。
 *
 * 安全边界（需求 5.5）：HTTP 明文、仅内网 RNDIS、单用户访问。登录态保存在 RAM，
 * 重启后需重新登录（需求 5.1）。
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include "app_config.h"

/** 监听端口（需求 5：8080） */
#define WEB_SERVER_PORT   8080

/** 初始化（只做状态清零，不打开端口，避免任务未创建就占资源）。 */
void web_server_init(app_config_t *cfg);

/** Web 服务任务主体（由 main 创建）。 */
void web_server_task(void *arg);

/** 查询当前是否有已登录会话（用于状态展示/调试）。 */
int  web_server_authed(void);

#endif /* WEB_SERVER_H */
