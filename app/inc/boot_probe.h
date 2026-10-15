/**
 * @file    boot_probe.h
 * @brief   启动探针：把应用初始化进度通过 USB 网络（RNDIS）暴露出来。
 *
 * 背景：模组 LOG 走 DBG_UART（PIN_17/18，921600），现场没有 USB-TTL 时
 * 完全看不到日志。为定位"应用启动即重启"这类问题，本模块在初始化**最开始**
 * 就起一个极简 HTTP 服务（默认 8081 端口），页面内容就是当前所处的初始化阶段。
 *
 * 用法：
 *   1) cm_opencpu_entry() 里尽早调用 boot_probe_start()
 *   2) 每个初始化步骤前后调用 boot_stage_set(阶段)
 *   3) 电脑上浏览器打开 http://192.168.0.1:8081/ ，即可看到卡在哪一步
 *
 * 设计约束：探针本身必须极简且不阻塞主流程——创建失败只返回错误码，
 * 由调用方决定是否忽略；任何 socket 失败都在任务内重试。
 */
#ifndef BOOT_PROBE_H
#define BOOT_PROBE_H

#include <stdint.h>

/** 探针 HTTP 端口（8080 留给正式 Web 面板，避免冲突） */
#define BOOT_PROBE_PORT 8081u

/** 初始化阶段（顺序即执行顺序，用于定位崩溃点） */
typedef enum {
    BOOT_STAGE_NONE = 0,      /* 探针尚未就绪 */
    BOOT_STAGE_ENTRY,         /* 已进入 cm_opencpu_entry */
    BOOT_STAGE_TASK,          /* app_main 任务已开始运行 */
    BOOT_STAGE_WAIT,          /* 等待 SDK 系统就绪（3 秒延时） */
    BOOT_STAGE_CFG,           /* 读 Flash 配置 */
    BOOT_STAGE_RNDIS,         /* USB 流量共享 */
    BOOT_STAGE_TRAFFIC,       /* 流量统计初始化（含平台计数器） */
    BOOT_STAGE_SMS,           /* 短信通道：虚拟 AT + CMGF/CNMI */
    BOOT_STAGE_NET,           /* 网络管理初始化 */
    BOOT_STAGE_WEB,           /* Web 服务初始化 */
    BOOT_STAGE_TASKS,         /* 创建其它任务 */
    BOOT_STAGE_RUNNING,       /* 初始化全部完成，进入正常运行 */
    BOOT_STAGE_COUNT
} boot_stage_t;

/** 设置当前阶段（任意线程可调用，内部只写一个 volatile int） */
void boot_stage_set(boot_stage_t s);

/** 读取当前阶段 */
boot_stage_t boot_stage_get(void);

/** 阶段名称（用于探针页面与日志） */
const char *boot_stage_name(boot_stage_t s);

/** 启动探针任务；返回 0 成功，<0 失败（失败不影响主流程） */
int boot_probe_start(void);

/** 探针监听是否已就绪（1=已在 8081 监听） */
int boot_probe_listening(void);

#endif /* BOOT_PROBE_H */
