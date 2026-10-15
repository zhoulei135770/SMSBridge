/**
 * @file    app_ctx.h
 * @brief   全局运行上下文：配置单例 + 配置保存入口。
 *
 * 说明：配置被多个任务（net_mgr / traffic / web_server / sms_svc）只读访问，
 *       只有 Web 保存配置时会整体替换。这里用一把互斥锁保护，读多写少。
 */
#ifndef APP_CTX_H
#define APP_CTX_H

#include "app_config.h"
#include "sdk_port.h"

/** 全局配置（只读访问前请用 app_cfg_lock_shared/app_cfg_unlock） */
extern app_config_t g_cfg;
/** 配置锁 */
extern sdk_mutex_t  g_cfg_lock;

/**
 * @brief 上电初始化配置：Flash 读取 -> 校验 -> 失败则写入默认配置（需求 6.2）。
 * @return 0 = 读到有效配置；1 = 首次上电，已写入默认配置；<0 = 读取异常但已用默认值运行。
 */
int  app_ctx_init(void);

/** 加锁 / 解锁（读配置时使用，临界区务必短） */
void app_ctx_lock(void);
void app_ctx_unlock(void);

/**
 * @brief 把当前 g_cfg 落盘（内部加锁 + CRC 校验，需求 6.3）。
 * @return 0 成功，<0 失败。
 */
int  app_ctx_save(void);

#endif /* APP_CTX_H */
