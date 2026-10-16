# 08 · 应用崩溃根因确认：内核 socket 线程模型 + eloop 缺失

> 结论时间：2026-10-15
> 本文记录本项目**两个真正的根因**及其 SDK 依据。前几轮（docs/06）修的都是次要问题。

---

## 一、根因 1：内核 socket 只能在 eloop 线程内操作

### 现象

应用启动后模组**周期性重启**（周期从 15s 逐步缩短到 3s），而**厂商基线（无应用）完全稳定**（0/60 掉线）。

### SDK 依据（官方示例 `examples/asocket/src/cm_demo_asocket.c`）

代码里**反复出现**这条注释：

```c
/* 执行环境为eloop线程(和异步socket在同一个线程,可以安全地处理socket创建和数据收发等操作) */
```

即：**socket 的创建与收发必须在 eloop 线程内完成**，这是内核异步 socket 模型的要求。

### 错误做法（修复前）

在**应用自己的任务**里用裸 lwip 同步调用：

```c
fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
lwip_bind(fd, ...);  lwip_listen(fd, 2);
while (1) { int cli = lwip_accept(fd, NULL, NULL); ... }
```

跨线程使用内核 socket → **模组复位**。
（这也解释了为什么 8080/8081 从未监听：任务在建立监听前就把模组搞崩了。）

### 正确做法（已改）

```c
/* sdk_port.c —— 所有 socket 操作都在 eloop 线程内 */
static void sdk_srv_start_ev(cm_eloop_event_handle_t ev, void *param)
{                                  /* ← eloop 线程 */
    c->fd = cm_asocket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP, sdk_srv_on_event, c);
    bind(c->fd, ...);   listen(c->fd, 4);
}

static void sdk_srv_on_event(int sock, cm_asocket_event_e event, void *param)
{                                  /* ← eloop 线程 */
    if (event == CM_ASOCKET_EV_ACCEPT_IND) {
        int cli = accept(sock, NULL, NULL);
        c->on_client(cli);         /* 应用回调也跑在 eloop 线程 */
        close(cli);
    }
}

int sdk_srv_start(uint16_t port, sdk_srv_client_cb_t on_client);
```

Web 面板（8080）与启动探针（8081）都改为 `sdk_srv_start(port, 处理函数)`，
不再自己 accept 循环。

**顺带修掉**：心跳探测原本也用裸 `lwip_connect`（同样跨线程），
改用文档化的 `cm_httpclient` 同步接口。

### 验证结果

改完后：**`掉线 0 / 30`，重启循环彻底消失** ✅

---

## 二、根因 2：eloop 必须由应用自己创建并运行

只把 socket 改成 asocket 还不够——**监听端口仍然没有建立**（HTTP 超时而非拒绝，
说明投递到 eloop 的"开始监听"事件没有任何线程执行）。

### SDK 依据（官方示例 `cm_demo_asocket.c` 第 441~452 行）

```c
static void __test_eloop_init(void)
{
    /* 创建事件循环 */
    s_main_eloop = cm_eloop_create(MAX_EVENT_NUM);

    /* 创建eloop执行线程 */
    osThreadAttr_t thread_attr = {0};
    thread_attr.name = "demo_eloop_main";
    thread_attr.stack_size = 1024;
    osThreadNew(__eloop_thread_proc, NULL, &thread_attr);
}
/* __eloop_thread_proc 内：cm_eloop_wait_event(s_main_eloop, CM_ELOOP_WAIT_FOREVER); */
```

并且 `cm_asocket.h` 对 `cm_asocket_eloop()` 的说明是
"获取本模块运行的 Event Loop(**初始化传入**或者系统默认Event Loop)" ——
说明存在一个"把 eloop 传给 asocket 模块"的初始化。该函数未在头文件中声明，
但**应用链接边界里确实导出了 `cm_asocket_init`**（`arm-none-eabi-nm` 可见），
即它是应用侧应当调用的初始化入口。

### 修复（已加到 `sdk_port.c`）

```c
extern int cm_asocket_init(cm_eloop_handle_t eloop);   /* 头文件未声明，stub 已导出 */

static void sdk_eloop_task(void *arg)
{
    for (;;) cm_eloop_wait_event(s_eloop, 1000u);      /* 真正执行事件 */
}

int sdk_eloop_start(void)
{
    s_eloop = cm_eloop_create(32u);                    /* 1) 创建事件循环 */
    (void)cm_asocket_init(s_eloop);                    /* 2) 绑定给 asocket 模块 */
    sdk_task_create("eloop", sdk_eloop_task, ...);     /* 3) 起线程跑它 */
    ...
}
```

`sdk_srv_start()` 会自动先调用 `sdk_eloop_start()`（幂等）。

---

## 三、正确的启动顺序（最终形态）

```
cm_opencpu_entry()                      仅：日志 + 创建 app_main 任务
  └─ app_main_task()
       ├─ diag_load / diag_note_boot    自诊断（跳过历史崩溃阶段）
       ├─ 阶段 1..13 依次执行
       ├─ sdk_eloop_start()             ★ 创建 eloop + cm_asocket_init + eloop 线程
       ├─ sdk_srv_start(8081, probe)    ★ 异步监听（eloop 线程内 bind/listen）
       ├─ sdk_srv_start(8080, web)      ★ 异步监听
       └─ traffic_loop()                常驻
```

---

## 四、关键教训（供后续开发遵守）

| # | 教训 |
|---|---|
| 1 | **符号能链接 ≠ 可以调用**。裸 `lwip_*` 在应用边界内，但必须遵守 eloop 线程模型；正确路径是 `cm_asocket_*`。 |
| 2 | **看官方示例，尤其是完整示例（`examples/*`），不要只看某个驱动小样**。`drv_virt_at.c` 与 `cm_demo_main.c` 对 `cm_virt_at_deinit()` 的写法就不一致，应以完整示例为准。 |
| 3 | **应用要自己跑 eloop**（`cm_eloop_create` + 线程 + `cm_eloop_wait_event`），否则 asocket 事件与投递动作都不会执行。 |
| 4 | 没有 UART 日志时，**"自诊断固件"（`diag_stage`）+ 网络探针**是可用的观测手段；崩溃阶段可跳过并报告。 |
| 5 | 崩溃"周期"是可测量信号：周期随代码改动而变，可用来判断崩溃点前移/后移。 |

---

## 五、当前状态

- 重启循环：**已解决**（0/30 掉线）
- Web 面板监听：**待验证**（eloop 补齐版已编译并挂起等待烧录）
- 应用功能：短信 / 网络保活 / 流量统计 / Web 面板代码均已完成，主机单元测试 549 项通过
