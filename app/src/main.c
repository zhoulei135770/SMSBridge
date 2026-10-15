/**
 * @file    main.c
 * @brief   固件主入口（模组上电自启动）。对应需求文档「五、非功能需求 4. 主入口」。
 *
 * 上电流程（分为"入口"与"初始化任务"两段）：
 *
 *   cm_opencpu_entry()（必须快速返回！SDK 在此处有启动超时保护）
 *     1. 日志就绪（打印开机横幅与上次复位原因，可识别看门狗复位）
 *     2. 创建 app_main 任务后立即 return 0，由 SDK 调度器接管
 *
 *   app_main_task()（真正的初始化，运行在任务上下文）
 *     3. 加载 Flash 配置（首次上电写默认配置）
 *     4. 应用 USB 流量共享（默认开启，需求 2.2）
 *     5. 流量统计初始化（读平台计数、处理跨月、下发限额）
 *     6. 短信通道初始化（注册 +CMT URC、配置 CMGF/CNMI）
 *     7. 网络管理初始化（APN 激活、心跳）
 *     8. Web 服务初始化
 *     9. 创建 sms_svc / net_mgr / web 三个任务
 *    10. 本任务转为流量采样循环（需求 4.1 / 4.5）
 *
 * 设计说明（重要，勿改回）：
 *   早期版本把全部初始化直接写在 cm_opencpu_entry() 里，其中包含读 Flash、
 *   以及多条需要等待响应的 AT 命令（AT+CMGF / AT+CNMI，单条超时可达数秒），
 *   累计阻塞十几秒。实测表现为模组上电后 USB/网络刚起来约 6 秒就反复重启。
 *   SDK 官方示例（custom/drv/uart/drv_uart.c）的入口只做 UART 打开并创建线程
 *   后立刻返回，因此这里遵循同样的"入口只做最小动作、初始化放任务里"的约定。
 *
 * 任务栈：需求文档「四、硬件资源约束 2」要求 6144 字节，这里也按 6144 申请；但
 *   **实际生效值由 sdk_port.c 的 sdk_task_create() 钳制为 3072 字节**
 *   （原因：栈由内核堆分配，任务数一多就 osThreadNew 失败 —— web/探针任务建不出来）。
 *   因此业务代码里的**大对象一律不得放栈上**，且已加自动检查：
 *       pwsh -File app/test/check_stack_budget.ps1   （-fstack-usage + 调用链预算）
 *   本工程 APP 可用 RAM 为 512KB（依据 prebuild/ld/ML307A/DSLN/app_dsln.ld 原文）。
 */
#include "app_ctx.h"
#include "app_log.h"
#include "boot_probe.h"
#include "sdk_port.h"
#include "sms_svc.h"
#include "net_mgr.h"
#include "traffic.h"
#include "usb_rndis.h"
#include "web_server.h"

#include <string.h>

/* ================================================================== */
/* 任务栈与优先级                                                      */
/* ================================================================== */
#define TASK_STACK_MAIN     6144u   /* 顺带承担流量采样循环 */
#define TASK_STACK_SMS      6144u
#define TASK_STACK_NET      6144u
#define TASK_STACK_WEB      6144u

/* SDK 会把用户线程优先级钳制到 osPriorityNormal，这里统一用它 */
#define TASK_PRIO_NORMAL    1

/* ================================================================== */
/* 流量采样循环（需求 4.1 / 4.5），由 app_main_task 调用，不返回        */
/* ================================================================== */
static void traffic_loop(void)
{
    for (;;) {
        uint32_t period_sec;

        app_ctx_lock();
        period_sec = g_cfg.traffic_sample_sec;
        app_ctx_unlock();

        if (period_sec < 10u) period_sec = 60u;
        if (period_sec > 3600u) period_sec = 3600u;   /* 最长 1 小时，保证复位动作及时 */

        sdk_task_sleep_ms(period_sec * 1000u);

        app_ctx_lock();
        /* 只有跨月 / 超限阈值变化才需要落盘（需求 6.3：减少 Flash 擦写） */
        if (traffic_sample(&g_cfg) != 0) {
            (void)app_ctx_save();
        }
        app_ctx_unlock();
    }
}

/* ================================================================== */
/* 初始化任务（所有阻塞操作都在这里，不在入口函数里）                    */
/* ================================================================== */
static void app_main_task(void *arg)
{
    int rc;

    (void)arg;

    /* 0) 给 SDK 系统一点时间完成文件系统/网络栈的挂载，避免过早访问外设 */
    boot_stage_set(BOOT_STAGE_WAIT);
    sdk_task_sleep_ms(3000u);

    /* 1) 配置：Flash 读取 -> 校验 -> 首次上电写默认 */
    boot_stage_set(BOOT_STAGE_CFG);
    rc = app_ctx_init();
    if (rc < 0) {
        LOG_W("main", "配置读取异常，已用默认配置继续运行（下次保存会覆盖）");
    }

    /* 2) USB 流量共享：默认启用（需求 2.2） */
    boot_stage_set(BOOT_STAGE_RNDIS);
    usb_rndis_init(&g_cfg);

    /* 3) 流量统计 */
    boot_stage_set(BOOT_STAGE_TRAFFIC);
    traffic_init(&g_cfg);

    /* 4) 短信通道：必须在其它 AT 使用者之前把 URC 注册好 */
    boot_stage_set(BOOT_STAGE_SMS);
    rc = sms_svc_init();
    if (rc < 0) {
        LOG_E("main", "短信服务初始化失败(rc=%d)：短信收发不可用，其余功能继续", rc);
    }

    /* 5) 网络管理 */
    boot_stage_set(BOOT_STAGE_NET);
    net_mgr_init(&g_cfg);

    /* 5b) 时钟同步（钉钉加签前提）：模组时钟优先，失败转 NTP；
     *     短信服务任务之后也会按限速（每分钟）自动重试 */
    sms_svc_time_sync_try();

    /* 6) Web 服务 */
    boot_stage_set(BOOT_STAGE_WEB);
    web_server_init(&g_cfg);

    /* 7) 创建其余任务 */
    boot_stage_set(BOOT_STAGE_TASKS);
    if (sdk_task_create("sms_svc", sms_svc_task, NULL, TASK_STACK_SMS, TASK_PRIO_NORMAL) != 0) {
        LOG_E("main", "创建 sms_svc 任务失败");
    }
    if (sdk_task_create("net_mgr", net_mgr_task, NULL, TASK_STACK_NET, TASK_PRIO_NORMAL) != 0) {
        LOG_E("main", "创建 net_mgr 任务失败");
    }
    if (sdk_task_create("web", web_server_task, NULL, TASK_STACK_WEB, TASK_PRIO_NORMAL) != 0) {
        LOG_E("main", "创建 web_server 任务失败");
    }

    boot_stage_set(BOOT_STAGE_RUNNING);
    LOG_I("main", "初始化完成：短信 / 网络保活 / 流量管理 / Web 面板 已全部启动");
    LOG_I("main", "Web 面板: http://<模组IP>:8080/（IP 在获取到 PDP 地址后会打印）");

    /* 8) 本任务转为流量采样循环 */
    traffic_loop();
}

/* ================================================================== */
/* 主入口（只做最小动作，必须快速返回）                                 */
/* ================================================================== */
int cm_opencpu_entry(char *param)
{
    (void)param;

    /* 日志：越早越好，这样后面的初始化失败都看得到；本身无阻塞 */
    app_log_init();
    boot_stage_set(BOOT_STAGE_ENTRY);

    /*
     * 启动探针：把初始化进度通过 USB 网络暴露到 8081 端口。
     * 现场没有 USB-TTL 时，这是唯一能"看到应用卡在哪一步"的手段。
     * 探针失败不影响主流程（只是失去可观测性）。
     */
    if (boot_probe_start() != 0) {
        LOG_W("main", "启动探针创建失败（不影响功能，仅失去 8081 进度页）");
    }

    boot_stage_set(BOOT_STAGE_TASK);
    if (sdk_task_create("app_main", app_main_task, NULL, TASK_STACK_MAIN, TASK_PRIO_NORMAL) != 0) {
        LOG_E("main", "创建 app_main 任务失败");
        return -1;
    }

    /* 返回 0 后由 SDK 调度器接管；上面的任务开始执行初始化 */
    return 0;
}
