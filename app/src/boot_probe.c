/**
 * @file    boot_probe.c
 * @brief   启动探针实现（见 inc/boot_probe.h）。
 *
 * 动机：现场没有 USB-TTL，模组的 DBG_UART 日志看不到，应用一旦在初始化阶段
 * 崩溃就只能看到"模组反复重启"。本模块在 8081 端口提供极简 HTTP 服务，
 * 页面内容是当前初始化阶段 + diag_stage 的自诊断报告。
 *
 * ★ 关键实现约束（本次故障的根因教训）：
 *   内核 socket 必须在 **eloop 线程**内创建与收发（SDK 官方 examples/asocket
 *   示例反复强调）。因此这里不再"在任务里 accept 循环"，而是把客户端处理函数
 *   通过 sdk_srv_start() 注册给 sdk_port —— 内核会在 eloop 线程内回调它。
 */
#include "boot_probe.h"
#include "diag_stage.h"
#include "sdk_port.h"

#include <stdio.h>
#include <string.h>

#define PROBE_STACK        4096u
#define PROBE_PRIO         1
#define PROBE_PAGE_MAX     2048u
#define PROBE_REQ_MAX      256u

static volatile int s_stage = (int)BOOT_STAGE_NONE;
static volatile int s_listening;
static char         s_req[PROBE_REQ_MAX];
static char         s_page[PROBE_PAGE_MAX];

static const char *const s_names[BOOT_STAGE_COUNT] = {
    "NONE",
    "ENTRY",
    "TASK",
    "WAIT(3s)",
    "CFG(Flash)",
    "RNDIS",
    "TRAFFIC",
    "SMS(virt_at)",
    "NET",
    "WEB",
    "TASKS",
    "RUNNING"
};

void boot_stage_set(boot_stage_t s)
{
    if ((int)s >= 0 && (int)s < (int)BOOT_STAGE_COUNT) {
        s_stage = (int)s;
    }
}

boot_stage_t boot_stage_get(void)
{
    return (boot_stage_t)s_stage;
}

const char *boot_stage_name(boot_stage_t s)
{
    if ((int)s >= 0 && (int)s < (int)BOOT_STAGE_COUNT) {
        return s_names[(int)s];
    }
    return "?";
}

int boot_probe_listening(void)
{
    return s_listening;
}

/* ------------------------------------------------------------------ */
/* 客户端处理：运行在 eloop 线程；返回后连接由 sdk_port 关闭             */
/* ------------------------------------------------------------------ */
static void probe_on_client(int cli)
{
    int n;

    (void)sdk_tcp_recv(cli, s_req, sizeof(s_req) - 1u, 800u);

    n = snprintf(s_page, sizeof(s_page),
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "<!doctype html><meta charset=\"utf-8\">"
                 "<title>BOOT PROBE</title>"
                 "<h3>ML307A-DSLN 启动探针 / BOOT PROBE</h3>"
                 "<p>阶段 stage: <b>%d - %s</b></p>"
                 "<p>运行 uptime: %lu s</p>"
                 "<p>探针端口 port: %u</p>"
                 "<hr><b>启动自诊断 / stage diagnosis</b><pre>",
                 (int)s_stage,
                 boot_stage_name((boot_stage_t)s_stage),
                 (unsigned long)(sdk_uptime_ms() / 1000u),
                 (unsigned)BOOT_PROBE_PORT);

    if (n > 0 && (size_t)n < sizeof(s_page)) {
        /* 追加自诊断报告（失败阶段列表） */
        int m = diag_report(&s_page[n], (uint32_t)(sizeof(s_page) - (uint32_t)n - 1u));
        if (m > 0) n += m;
        if ((size_t)n + 16u < sizeof(s_page)) {
            n += (int)snprintf(&s_page[n], sizeof(s_page) - (size_t)n, "</pre>\n");
        }
    }

    if (n > 0) {
        size_t slen = ((size_t)n < sizeof(s_page)) ? (size_t)n : (sizeof(s_page) - 1u);
        (void)sdk_tcp_send(cli, s_page, slen);
    }
}

/* 任务体：提交异步监听后即挂起（真正的 socket 操作都在 eloop 线程内） */
static void probe_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (sdk_srv_start((uint16_t)BOOT_PROBE_PORT, probe_on_client) == 0) {
            s_listening = 1;
            sdk_log_printf("[boot] probe start submitted on %u\r\n", (unsigned)BOOT_PROBE_PORT);
            break;
        }
        sdk_log_printf("[boot] probe start failed, retry in 2s\r\n");
        sdk_task_sleep_ms(2000u);
    }

    for (;;) {
        sdk_task_sleep_ms(30000u);
    }
}

int boot_probe_start(void)
{
    if (sdk_task_create("boot_probe", probe_task, NULL, PROBE_STACK, PROBE_PRIO) != 0) {
        return -1;
    }
    return 0;
}
