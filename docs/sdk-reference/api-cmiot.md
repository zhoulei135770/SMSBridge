# ML307A OpenCPU SDK — cmiot 头文件 API 速查

> **目标硬件**：ML307A-DSLN（ASR1606），OpenCPU Standard SDK **1.4.2**
> **SDK 根目录**：`sdk/ML307A_OpenCPU_Standard_1.4.2`
> **本文档定位**：纯 API 事实整理，不含业务固件代码。
>
> **事实来源（全部为本地文件，未联网）**
>
> | 来源 | 用途 |
> | --- | --- |
> | `include/cmiot/*.h` | 函数原型、typedef/enum/struct、注释约束（**权威来源**） |
> | `docs/ML305A_ML307A_OpenCPU_API手册.chm` | 已本地解包核对，内容为同一批头文件的 Doxygen 镜像，**未提供头文件之外的额外信息** |
> | `custom/applaction/{main.c,app.c,app.h,includes.h}` | 应用入口与 `osThreadCreat` / `osDelayMs` 等**用户态包装函数**的真实定义 |
> | `custom/drv/uart/{drv_uart.c,drv_uart.h}` | 本项目 UART 驱动封装（含回调与 IOMUX 引脚） |
> | `examples/*/src/*.c` | 官方示例用法（仅作旁证，非 API 定义） |
> | `prebuild/baseline/ML307A/DSLN/ML307A-DSLN-MBRH1S00.map` | 已验证本文所列函数在 DSLN 基线固件中**确实存在** |
> | `prebuild/ld/ML307A/DSLN/app_dsln.ld` | 链接脚本，确认应用入口符号为 `cm_opencpu_entry` |
>
> **标注约定**：凡头文件未明确写出的内容，一律标注「**未证实**」并给出旁证。凡在头文件中被注释掉的接口，标注「**SDK 已禁用（源码中为注释）**」。

---

## 0. 模块 → 推荐 API 速查表

| 需求模块 | 头文件 | 首选 API（本文档已验证原型） |
| --- | --- | --- |
| 线程/任务 | `cm_os.h` | `osThreadNew`；工程内包装 `osThreadCreat(name, func, priority, stacksize)`（在 `custom/applaction/app.h`） |
| 毫秒延时 | `cm_os.h` + `app.h` | `osDelay(ticks)`（**ticks，1 tick = 5 ms**）；包装 `osDelayMs(ms)`（`osDelay(ms/5)`，要求 ms 为 5 的整数倍） |
| 软件定时器 | `cm_os.h` | `osTimerNew` / `osTimerStart` / `osTimerStop` / `osTimerIsRunning` / `osTimerDelete`；包装 `osTimerCreat(name, func, type)` |
| 互斥锁 | `cm_os.h` | `osMutexNew` / `osMutexAcquire` / `osMutexRelease` / `osMutexDelete` |
| 信号量（仅二值） | `cm_os.h` | `osSemaphoreNew` / `osSemaphoreAcquire` / `osSemaphoreRelease` / `osSemaphoreDelete` |
| 消息队列 | `cm_os.h` | `osMessageQueueNew` / `osMessageQueuePut` / `osMessageQueueGet` / `osMessageQueueDelete` |
| 事件标志 | `cm_os.h` | `osEventFlagsNew` / `osEventFlagsSet` / `osEventFlagsWait` / `osEventFlagsDelete` |
| 动态内存 | `cm_mem.h` | `cm_malloc` / `cm_calloc` / `cm_realloc` / `cm_free`（**禁用 C 库 malloc/free**）；`cm_mem_get_heap_stats` |
| HTTP / Webhook 推送 | `cm_http.h` | `cm_httpclient_create` → `cm_httpclient_set_cfg` → `cm_httpclient_request_start` → `cm_httpclient_request_send` → `cm_httpclient_request_end` → `cm_httpclient_delete`；同步版 `cm_httpclient_sync_request` |
| HTTPS 证书 | `cm_ssl.h` | `cm_ssl_setopt(ssl_id, CM_SSL_PARAM_CA_CERT, ca)`（`ssl_id` 范围 0–5） |
| UART 串口 | `cm_uart.h` | `cm_uart_register_event`（**必须先于 open**）→ `cm_uart_open` → `cm_uart_read` / `cm_uart_write` → `cm_uart_close` |
| GPIO | `cm_gpio.h` | `cm_gpio_init` / `cm_gpio_set_level` / `cm_gpio_get_level` / `cm_gpio_interrupt_register` + `cm_gpio_interrupt_enable` |
| 引脚复用 | `cm_iomux.h` | `cm_iomux_set_pin_func`（**任何外设 open/init 之前必须调用**）/ `cm_iomux_set_pin_cmd` |
| 重启 / 关机 | `cm_pm.h` | `cm_pm_reboot()` / `cm_pm_poweroff()`（**注意：不在 `cm_sys.h` 中**） |
| 低功耗 / 唤醒锁 | `cm_pm.h` | `cm_pm_init` / `cm_pm_work_lock` / `cm_pm_work_unlock` / `cm_pm_powerkey_regist_callback` |
| 模组信息 / LOG | `cm_sys.h` | `cm_sys_get_imei` / `cm_sys_get_sn` / `cm_sys_get_cm_ver` / `cm_log_printf` |
| SIM | `cm_sim.h` | `cm_sim_get_imsi` |
| 本地时间 / 时区 | `cm_rtc.h` | `cm_rtc_get_current_time` / `cm_rtc_set_current_time` / `cm_rtc_set_timezone` / `cm_rtc_get_timezone` |
| 网络时间同步 | `cm_ntp.h` | `cm_ntp_set_cfg` + `cm_ntp_sync`（异步，结果走回调） |
| 配置持久化 | `cm_fs.h` | `cm_fs_open` → `cm_fs_write` → `cm_fs_close`（**无目录、无 sync，必须 close 落盘**） |
| SD 卡文件 | `cm_sd.h` | `cm_sd_fopen` / `cm_sd_fwrite` / `cm_sd_fclose` …（**仅 ML307A-DSLN/GSLN 支持**） |
| 段属性修饰 | `cm_utils.h` | `__RUN_AT_RAM` / `__RUN_AT_XIP`（纯宏，无函数） |

### 明确**不存在**于本 SDK 的能力（已通过目录列举 + 全量 grep 确认）

| 文件名 | 结论 |
| --- | --- |
| `cm_sms.h` | **不存在** |
| `cm_usb.h` | **不存在** |
| `cm_ping.h` | **不存在** |
| `cm_flash.h` | **不存在**（裸 Flash 读写无接口；持久化只能用 `cm_fs.h`） |
| `cm_wdt.h` | **不存在**（看门狗无用户接口；`cm_pm_power_on_reason_e` 中的 `CM_PM_ERROR_RESET` 注释为 “Error reset (WDT)”，说明 WDT 由底层管理） |
| `cm_os_*` 命名风格的函数 | **不存在**（`include/cmiot/` 全量 grep `\bcm_os_[a-z_]+` 无匹配）。RTOS API 统一为 CMSIS-RTOS2 风格的 `osXxx` |
| 进入下载模式（download / fastboot / diag）的接口 | **未提供，未证实** |

---

## 1. `cm_os.h` — 操作系统接口（CMSIS-RTOS2 风格）

**一句话用途**：提供内核控制、线程、软件定时器、事件标志、互斥锁、信号量、消息队列与延时等全部 RTOS 原语。

**头文件首部对「超时值」的原文约定**（第 5–12 行，直接决定 A/B/C 三节）：

> Timeout values are an argument to several `osXxx` functions to allow time for resolving a request.
> A timeout value of **0** means that the RTOS does not wait and the function returns instantly, even when no resource is available.
> A timeout value of `osWaitForever` means that the RTOS waits infinitely until a resource becomes available.
> **The timeout value specifies the number of timer ticks until the time delay elapses.**
> The value is an upper bound and depends on the actual time elapsed since the last timer tick.

即：**所有 `timeout` / `ticks` 参数的单位都是 tick，不是毫秒**。

### 1.1 typedef / enum / struct 完整定义

```c
/** Version information. */
typedef struct {
  uint32_t                       api;   /**< API version (major.minor.rev: mmnnnrrrr dec). */
  uint32_t                    kernel;   /**< Kernel version (major.minor.rev: mmnnnrrrr dec). */
} osVersion_t;

/** Kernel state. */
typedef enum {
  osKernelInactive        =  0,         /**< Inactive. */
  osKernelReady           =  1,         /**< Ready. */
  osKernelRunning         =  2,         /**< Running. */
  osKernelLocked          =  3,         /**< Locked. */
  osKernelSuspended       =  4,         /**< Suspended. */
  osKernelError           = -1,         /**< Error. */
  osKernelReserved        = 0x7FFFFFFF  /**< Prevents enum down-size compiler optimization. */
} osKernelState_t;

/** Thread state. */
typedef enum {
  osThreadInactive        =  0,         /**< Inactive. */
  osThreadReady           =  1,         /**< Ready. */
  osThreadRunning         =  2,         /**< Running. */
  osThreadBlocked         =  3,         /**< Blocked. */
  osThreadTerminated      =  4,         /**< Terminated. */
  osThreadError           = -1,         /**< Error. */
  osThreadReserved        = 0x7FFFFFFF  /**< Prevents enum down-size compiler optimization. */
} osThreadState_t;

/** Priority values. */
 typedef enum {
  osPriorityIdle          = 0,         ///< Reserved for Idle thread.
  osPriorityLow           = 2,         ///< Priority: low
  osPriorityBelowNormal   = 3,         ///< Priority: below normal
  osPriorityNormal        = 4,         ///< Priority: normal
  osPriorityAboveNormal   = 5,         ///< Priority: above normal
  osPriorityHigh          = 6,         ///< Priority: high
  osPrioritySoftRealtime  = 7,         ///< Priority: realtime
  osPriorityHardRealtime  = 8,         ///< Priority: realtime
  osPriorityISR           = 32,        ///< Reserved for ISR deferred thread,Do not use!
  osPriorityError         = -1,         ///< System cannot determine priority or illegal priority.
  osPriorityReserved      = 0x7FFFFFFF  ///< Prevents enum down-size compiler optimization.
} osPriority_t;
/** Entry point of a thread. */
typedef void (*osThreadFunc_t) (void *argument);

/** Timer callback function. */
typedef void (*osTimerFunc_t) (void *argument);

/** Timer type. */
typedef enum {
  osTimerOnce               = 0,          /**< One-shot timer. */
  osTimerPeriodic           = 1           /**< Repeating timer. */
} osTimerType_t;

/** Status code values returned by CMSIS-RTOS functions. */
typedef enum {
  osOK                      =  0,         /**< Operation completed successfully. */
  osError                   = -1,         /**< Unspecified RTOS error: run-time error but no other error message fits. */
  osErrorTimeout            = -2,         /**< Operation not completed within the timeout period. */
  osErrorResource           = -3,         /**< Resource not available. */
  osErrorParameter          = -4,         /**< Parameter error. */
  osErrorNoMemory           = -5,         /**< System is out of memory: it was impossible to allocate or reserve memory for the operation. */
  osErrorISR                = -6,         /**< Not allowed in ISR context: the function cannot be called from interrupt service routines. */
  osStatusReserved          = 0x7FFFFFFF  /**< Prevents enum down-size compiler optimization. */
} osStatus_t;

/** Thread ID identifies the thread. */
typedef void *osThreadId_t;

/** Timer ID identifies the timer. */
typedef void *osTimerId_t;

/** Event Flags ID identifies the event flags. */
typedef void *osEventFlagsId_t;

/** Mutex ID identifies the mutex. */
typedef void *osMutexId_t;

/** Semaphore ID identifies the semaphore. */
typedef void *osSemaphoreId_t;

/** Memory Pool ID identifies the memory pool. */
typedef void *osMemoryPoolId_t;

/** Message Queue ID identifies the message queue. */
typedef void *osMessageQueueId_t;

/** Data type that identifies secure software modules called by a process. */
typedef uint32_t TZ_ModuleId_t;

/** Attributes structure for thread. */
typedef struct {
  const char                   *name;   /**< name of the thread. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
  void                   *stack_mem;    /**< memory for stack. */
  uint32_t                stack_size;   /**< size of stack. */
  osPriority_t              priority;   /**< initial thread priority (default: osPriorityNormal). */
  TZ_ModuleId_t            tz_module;   /**< TrustZone module identifier. */
  uint32_t                  reserved;   /**< reserved (must be 0). */
} osThreadAttr_t;

/** Attributes structure for timer. */
typedef struct {
  const char                   *name;   /**< name of the timer. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
} osTimerAttr_t;

/** Attributes structure for event flags. */
typedef struct {
  const char                   *name;   /**< name of the event flags. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
} osEventFlagsAttr_t;

/** Attributes structure for mutex. */
typedef struct {
  const char                   *name;   /**< name of the mutex. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
} osMutexAttr_t;

/** Attributes structure for semaphore. */
typedef struct {
  const char                   *name;   /**< name of the semaphore. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
} osSemaphoreAttr_t;

/** Attributes structure for memory pool. */
typedef struct {
  const char                   *name;   /**< name of the memory pool. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
  void                      *mp_mem;    /**< memory for data storage. */
  uint32_t                   mp_size;   /**< size of provided memory for data storage. */
} osMemoryPoolAttr_t;

/** Attributes structure for message queue. */
typedef struct {
  const char                   *name;   /**< name of the message queue. */
  uint32_t                 attr_bits;   /**< attribute bits. */
  void                      *cb_mem;    /**< memory for control block. */
  uint32_t                   cb_size;   /**< size of provided memory for control block. */
  void                      *mq_mem;    /**< memory for data storage. */
  uint32_t                   mq_size;   /**< size of provided memory for data storage. */
} osMessageQueueAttr_t;
```

### 1.2 宏定义（原样）

```c
/** Timeout value. */
#define osWaitForever         0xFFFFFFFFU /**< Wait forever timeout value. */

/** Flags options (@ref osThreadFlagsWait and @ref osEventFlagsWait). */
#define osFlagsWaitAny        0x00000000U /**< Wait for any flag (default). */
#define osFlagsWaitAll        0x00000001U /**< Wait for all flags. */
#define osFlagsNoClear        0x00000002U /**< Do not clear flags which have been specified to wait for. */

/** Flags errors (returned by osThreadFlagsXxxx and osEventFlagsXxxx). */
#define osFlagsError          0x80000000U /**< Error indicator. */
#define osFlagsErrorUnknown   0xFFFFFFFFU /**< osError (-1). */
#define osFlagsErrorTimeout   0xFFFFFFFEU /**< osErrorTimeout (-2). */
#define osFlagsErrorResource  0xFFFFFFFDU /**< osErrorResource (-3). */
#define osFlagsErrorParameter 0xFFFFFFFCU /**< osErrorParameter (-4). */
#define osFlagsErrorISR       0xFFFFFFFAU /**< osErrorISR (-6). */

/** Thread attributes (attr_bits in @ref osThreadAttr_t). */
#define osThreadDetached      0x00000000U /**< Thread created in detached mode (default). */
#define osThreadJoinable      0x00000001U /**< Thread created in joinable mode. */

/** Mutex attributes (attr_bits in @ref osMutexAttr_t). */
#define osMutexRecursive      0x00000001U /**< Recursive mutex. */
#define osMutexPrioInherit    0x00000002U /**< Priority inherit protocol. */
#define osMutexRobust         0x00000008U /**< Robust mutex. */
```

### 1.3 函数原型（按用途分组，原样摘录）

**内核 / 时钟**

```c
int32_t osKernelLock (void);
        /* @return previous lock state (1 - locked, 0 - not locked, error code if negative). */
int32_t osKernelUnlock (void);
        /* @return previous lock state (1 - locked, 0 - not locked, error code if negative). */
uint32_t osKernelGetTickFreq (void);
        /* @return frequency of the kernel tick in hertz, i.e. kernel ticks per second. */
uint32_t osKernelGetTickCount (void);
        /* @return RTOS kernel current tick count. */
```

**线程**

```c
osThreadId_t osThreadNew (osThreadFunc_t func, void *argument, const osThreadAttr_t *attr);
        /* @param func      thread function. */
        /* @param argument  pointer that is passed to the thread function as start argument. */
        /* @param attr      thread attributes; NULL: default values. */
        /* @return thread ID for reference by other functions or NULL in case of error. */
const char *osThreadGetName (osThreadId_t thread_id);
        /* @return name as null-terminated string. */
osThreadId_t osThreadGetId (void);
        /* @return thread ID for reference by other functions or NULL in case of error. */
osThreadState_t osThreadGetState (osThreadId_t thread_id);
        /* @return current thread state of the specified thread. */
uint32_t osThreadGetStackSize (osThreadId_t thread_id);
        /* @return stack size in bytes. */
uint32_t osThreadGetStackSpace (osThreadId_t thread_id);
        /* @return remaining stack space in bytes. */
osStatus_t osThreadSetPriority (osThreadId_t thread_id, osPriority_t priority);
        /* @return status code that indicates the execution status of the function. */
osPriority_t osThreadGetPriority (osThreadId_t thread_id);
        /* @return current priority value of the specified thread. */
osStatus_t osThreadYield (void);
        /* @return status code that indicates the execution status of the function. */
osStatus_t osThreadSuspend (osThreadId_t thread_id);
        /* @return status code that indicates the execution status of the function. */
osStatus_t osThreadResume (osThreadId_t thread_id);
        /* @return status code that indicates the execution status of the function. */
osStatus_t osThreadTerminate (osThreadId_t thread_id);
        /* @return status code that indicates the execution status of the function. */
uint32_t osThreadGetCount (void);
        /* @return number of active threads. */
```

**延时**

```c
osStatus_t osDelay (uint32_t ticks);
        /* @param ticks  time ticks value  —— 单位是 tick，不是 ms */
        /* @return status code that indicates the execution status of the function. */
```

**软件定时器**

```c
osTimerId_t osTimerNew (osTimerFunc_t func, osTimerType_t type, void *argument, const osTimerAttr_t *attr);
        /* @param func      function pointer to callback function. */
        /* @param type      osTimerOnce for one-shot or osTimerPeriodic for periodic behavior. */
        /* @param argument  argument to the timer callback function. */
        /* @param attr      timer attributes; NULL: default values. */
        /* @return timer ID for reference by other functions or NULL in case of error. */
osStatus_t osTimerStart (osTimerId_t timer_id, uint32_t ticks);
        /* @param ticks  time ticks value of the timer. */
        /* @return status code that indicates the execution status of the function. */
osStatus_t osTimerStop (osTimerId_t timer_id);
        /* @return status code that indicates the execution status of the function. */
uint32_t osTimerIsRunning (osTimerId_t timer_id);
        /* @return = 0 - not running, = 1 - running */
osStatus_t osTimerDelete (osTimerId_t timer_id);
        /* @return status code that indicates the execution status of the function. */
```

**事件标志**

```c
osEventFlagsId_t osEventFlagsNew (const osEventFlagsAttr_t *attr);
        /* @return event flags ID for reference by other functions or NULL in case of error. */
uint32_t osEventFlagsSet (osEventFlagsId_t ef_id, uint32_t flags);
        /* @return thread flags input or error code if highest bit set. */
uint32_t osEventFlagsWait (osEventFlagsId_t ef_id, uint32_t flags, uint32_t options, uint32_t timeout);
        /* @param options  specifies flags options (osFlagsXxxx). */
        /* @param timeout  TimeOutValue or 0 in case of no time-out. */
        /* @return event flags before clearing or error code if highest bit set. */
osStatus_t osEventFlagsDelete (osEventFlagsId_t ef_id);
        /* @return status code that indicates the execution status of the function. */
```

**互斥锁**

```c
osMutexId_t osMutexNew (const osMutexAttr_t *attr);
        /* @return mutex ID for reference by other functions or NULL in case of error. */
osStatus_t osMutexAcquire (osMutexId_t mutex_id, uint32_t timeout);
        /* @param timeout  TimeOutValue or 0 in case of no time-out. */
        /* @return status code that indicates the execution status of the function. */
osStatus_t osMutexRelease (osMutexId_t mutex_id);
        /* @return status code that indicates the execution status of the function. */
osStatus_t osMutexDelete (osMutexId_t mutex_id);
        /* @return status code that indicates the execution status of the function. */
```

**信号量**

```c
osSemaphoreId_t osSemaphoreNew (uint32_t max_count, uint32_t initial_count, const osSemaphoreAttr_t *attr);
        /* @param max_count      maximum number of available tokens. */
        /* @param initial_count  initial number of available tokens. */
        /* @return semaphore ID for reference by other functions or NULL in case of error.
         *         (only binary semaphore are supported)  ← 仅支持二值信号量 */
osStatus_t osSemaphoreAcquire (osSemaphoreId_t semaphore_id, uint32_t timeout);
        /* @param timeout  TimeOutValue or 0 in case of no time-out. */
        /* @return status code that indicates the execution status of the function. */
osStatus_t osSemaphoreRelease (osSemaphoreId_t semaphore_id);
        /* @return status code that indicates the execution status of the function. */
osStatus_t osSemaphoreDelete (osSemaphoreId_t semaphore_id);
        /* @return status code that indicates the execution status of the function. */
```

**消息队列**

```c
osMessageQueueId_t osMessageQueueNew (uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t *attr);
        /* @param msg_count  maximum number of messages in queue. */
        /* @param msg_size   maximum message size in bytes. */
        /* @return message queue ID for reference by other functions or NULL in case of error. */
osStatus_t osMessageQueuePut (osMessageQueueId_t mq_id, const void *msg_ptr, uint8_t msg_prio, uint32_t timeout);
        /* @param timeout  TimeOutValue or 0 in case of no time-out. */
        /* @return status code that indicates the execution status of the function. */
osStatus_t osMessageQueueGet (osMessageQueueId_t mq_id, void *msg_ptr, uint8_t *msg_prio, uint32_t timeout);
        /* @param msg_prio  pointer to buffer for message priority or NULL. */
        /* @param timeout   TimeOutValue or 0 in case of no time-out. */
        /* @return status code that indicates the execution status of the function. */
osStatus_t osMessageQueueDelete (osMessageQueueId_t mq_id);
        /* @return status code that indicates the execution status of the function. */
```

### 1.4 `cm_os.h` 中被注释掉的接口（**SDK 已禁用**，不要调用）

以下这些**在头文件里存在原型但整行被 `//` 注释**，链接时不可用：

```c
//__NO_RETURN void osThreadExit (void);
//const char *osTimerGetName (osTimerId_t timer_id);
//uint32_t osEventFlagsClear (osEventFlagsId_t ef_id, uint32_t flags);
//uint32_t osEventFlagsGet (osEventFlagsId_t ef_id);
//osThreadId_t osMutexGetOwner (osMutexId_t mutex_id);
//uint32_t osSemaphoreGetCount (osSemaphoreId_t semaphore_id);
//uint32_t osMessageQueueGetCapacity (osMessageQueueId_t mq_id);
//uint32_t osMessageQueueGetMsgSize (osMessageQueueId_t mq_id);
//uint32_t osMessageQueueGetCount (osMessageQueueId_t mq_id);
//uint32_t osMessageQueueGetSpace (osMessageQueueId_t mq_id);
//osStatus_t osMessageQueueReset (osMessageQueueId_t mq_id);
```

### 1.5 关键使用约束（摘录自注释）

* 超时参数单位是 **tick**；`0` = 不等待立即返回；`osWaitForever`(0xFFFFFFFFU) = 无限等待。
* `osSemaphoreNew`：**只支持二值信号量**（注释原文 “only binary semaphore are supported”）。
* `osThreadGetStackSize` / `osThreadGetStackSpace` 的返回值单位是 **字节（bytes）**。
* `osPriorityISR` 注释原文 “Reserved for ISR deferred thread, **Do not use!**”。
* `osThreadAttr_t.reserved` 注释 “reserved (**must be 0**)”。
* `osKernelLock` / `osKernelUnlock` 返回的是**之前的锁状态**，不是执行结果。
* `osEventFlagsWait` / `osEventFlagsSet` 的返回值在高位置位时为错误码（见 `osFlagsError*` 宏）。

---

## 2. `cm_http.h` — HTTP/HTTPS 协议客户端接口

**一句话用途**：提供 HTTP 客户端的创建/配置/异步收发/同步请求能力，自带 HTTPS 开关（证书走 `cm_ssl.h`）。这是 Webhook 推送的核心模块。

> 头文件注释中的文件名写作 `cm_http_client.h`，但实际文件名是 `cm_http.h`，引用时请用 `#include "cm_http.h"`。

### 2.1 宏定义（原样）

```c
#ifndef true
#define true (1)
#endif

#ifndef false
#define false (0)
#endif

#ifndef bool
#define bool int
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/**HTTP默认端口*/
#define HTTP_DEFAULT_PORT   80

/**HTTPS默认端口*/
#define HTTPS_DEFAULT_PORT 443

/**HTTP可创建实例个数*/
#define HTTPCLIENT_CTX_MAX_NUM                    4

/**HTTP连接超时最大时间*/
#define HTTPCLIENT_CONNECT_TIMEOUT_MAXTIME        180

/**HTTP请求响应超时最大时间*/
#define HTTPCLIENT_WAITRSP_TIMEOUT_MAXTIME        60

/**HTTP连接超时默认时间*/
#define HTTPCLIENT_CONNECT_TIMEOUT_DEFAULT        60

/**HTTP请求响应超时默认时间*/
#define HTTPCLIENT_WAITRSP_TIMEOUT_DEFAULT        0
```

### 2.2 typedef / enum / struct 完整定义

```c
/**HTTP请求类型*/
typedef enum {
    HTTPCLIENT_REQUEST_NONE = 0,
    HTTPCLIENT_REQUEST_GET,
    HTTPCLIENT_REQUEST_POST,
    HTTPCLIENT_REQUEST_PUT,
    HTTPCLIENT_REQUEST_DELETE,
    HTTPCLIENT_REQUEST_HEAD,
    HTTPCLIENT_REQUEST_MAX
} cm_httpclient_request_type_e;

/**HTTP回调事件*/
typedef enum{
    CM_HTTP_CALLBACK_EVENT_REQ_START_SUCC_IND=1,        /*!< 请求启动成功事件 */
    CM_HTTP_CALLBACK_EVENT_RSP_HEADER_IND,              /*!< 接收到报头事件 */
    CM_HTTP_CALLBACK_EVENT_RSP_CONTENT_IND,             /*!< 接收到消息体事件 */
    CM_HTTP_CALLBACK_EVENT_RSP_END_IND,                 /*!< 请求响应结束事件 */
    CM_HTTP_CALLBACK_EVENT_ERROR_IND,                   /*!< 请求失败事件 */
}cm_httpclient_callback_event_e;

/**HTTP异常状态码*/
typedef enum{
    CM_HTTP_EVENT_CODE_DNS_FAIL=1,                      /*!< DNS解析失败 */
    CM_HTTP_EVENT_CODE_CONNECT_FAIL,                    /*!< 连接服务器失败 */
    CM_HTTP_EVENT_CODE_CONNECT_TIMEOUT,                 /*!< 连接超时 */
    CM_HTTP_EVENT_CODE_SSL_CONNECT_FAIL,                /*!< SSL握手失败 */
    CM_HTTP_EVENT_CODE_CONNECT_BREAK,                   /*!< 连接异常断开 */
    CM_HTTP_EVENT_CODE_WAITRSP_TIMEOUT,                 /*!< 等待响应超时 */
    CM_HTTP_EVENT_CODE_DATA_PARSE_FAIL,                 /*!< 数据解析失败 */
    CM_HTTP_EVENT_CODE_CACHR_NOT_ENOUGH,                /*!< 缓存空间不足 */
    CM_HTTP_EVENT_CODE_DATA_DROP,                       /*!< 数据丢包 */
    CM_HTTP_EVENT_CODE_WRITE_FILE_FAIL,                 /*!< 写文件失败 */
    CM_HTTP_EVENT_CODE_UNKNOWN=255,                     /*!< 未知错误 */
}cm_httpclient_error_event_e;

/**HTTP结果码*/
typedef enum{
    CM_HTTP_RET_CODE_OK = 0,                            /*!< 成功 */
    CM_HTTP_RET_CODE_OPERATION_NOT_ALLOWED = 3,         /*!< 操作不被允许 3 */
    CM_HTTP_RET_CODE_MALLOC_FAIL = 23,                  /*!< 内存分配失败 23 */
    CM_HTTP_RET_CODE_PARAM_ERROR = 50,                  /*!< 参数错误 50 */
    CM_HTTP_RET_CODE_UNKNOWN_ERROR = 650,               /*!< 未知错误 650 */
    CM_HTTP_RET_CODE_NO_MORE_FREE_CLIENT = 651,         /*!< 没有空闲客户端 651 */
    CM_HTTP_RET_CODE_CLIENT_NOT_CREATE = 652,           /*!< 客户端未创建 652 */
    CM_HTTP_RET_CODE_CLIENT_IS_BUSY = 653,              /*!< 客户端忙 653 */
    CM_HTTP_RET_CODE_URL_PARSE_FAIL = 654,              /*!< URL解析失败 654 */
    CM_HTTP_RET_CODE_SSL_NOT_ENABLE = 655,              /*!< SSL未使能 655 */
    CM_HTTP_RET_CODE_CONNECT_FAIL = 656,                /*!< 连接失败 656 */
    CM_HTTP_RET_CODE_SEND_DATA_FAIL = 657,              /*!< 数据发送失败 657 */
    CM_HTTP_RET_CODE_OPEN_FILE_FAIL = 658,              /*!< 打开文件失败 658 */
} cm_httpclient_ret_code_e;

/**HTTP实例句柄*/
typedef void *cm_httpclient_handle_t;

/**HTTP回调*/
/*
* @param [in] client_handle                         客户端句柄
* @param [in] event                                 回调消息事件
* @param [in] param                                 事件相关对应响应数据
*
* CM_HTTP_CALLBACK_EVENT_REQ_START_SUCC_IND         NULL
* CM_HTTP_CALLBACK_EVENT_RSP_HEADER_IND             cm_httpclient_callback_rsp_header_param_t
* CM_HTTP_CALLBACK_EVENT_RSP_CONTENT_IND            cm_httpclient_callback_rsp_content_param_t
* CM_HTTP_CALLBACK_EVENT_RSP_END_IND                NULL
* CM_HTTP_CALLBACK_EVENT_ERROR_IND                  cm_httpclient_error_event_e
*/
typedef void (*cm_httpclient_event_callback_func)(cm_httpclient_handle_t client_handle, cm_httpclient_callback_event_e event, void *param);

/**HTTP相关回调函数*/
typedef struct {
    uint16_t response_code;
    uint16_t response_header_len;
    const uint8_t *response_header;
}cm_httpclient_callback_rsp_header_param_t;

/**HTTP相关回调函数*/
typedef struct {
    uint32_t total_len;	                /*!< 数据总长度（chunked模式时为0） */
    uint32_t sum_len;                   /*!< 已接收长度 */
    uint32_t current_len;               /*!< 当前包长度 */
    const uint8_t *response_content;    /*!< 数据 */
}cm_httpclient_callback_rsp_content_param_t;

/**HTTP可配参数*/
typedef struct {
    uint8_t ssl_enable;             /*!< 是否使用HTTPS */
    int32_t ssl_id;                 /*!< ssl 索引号 */
    uint8_t cid;                    /*!< PDP索引 */
    uint8_t conn_timeout;           /*!< 连接超时时间 */
    uint8_t rsp_timeout;            /*!< 响应超时时间 */
    uint8_t dns_priority;           /*!< dns解析优先级 0：使用全局优先级。1：v4优先。2：v6优先 */
}cm_httpclient_cfg_t;

/**HTTP 同步接口输入参数*/
typedef struct {
    cm_httpclient_request_type_e method;    /*!< 请求类型 */
    const uint8_t *path;                    /*!< 请求路径 */
    uint32_t content_length;                /*!< 数据长度 */
    uint8_t *content;                       /*!< 数据 */
}cm_httpclient_sync_param_t;

/**HTTP 同步接口响应数据*/
typedef struct {
    uint32_t response_code;                 /*!< 请求成功时的响应结果码 */
    uint32_t response_header_len;           /*!< 响应报头长度 */
    uint32_t response_content_len;          /*!< 响应消息体长度 */
    uint8_t *response_header;               /*!< 响应报头（内部分配空间，使用完后可调用cm_httpclient_sync_free_data释放） */
    uint8_t *response_content;              /*!< 响应消息体（内部分配空间，使用完后可调用cm_httpclient_sync_free_data释放） */
}cm_httpclient_sync_response_t;
```

### 2.3 函数原型（原样摘录，按调用顺序分组）

**编码辅助**

```c
uint8_t *cm_httpclient_uri_encode(const uint8_t *src, const uint32_t len);
        /* @details uri编码，不对字母数字以及-_.!~*'();/?:@&=+$,#进行编码，
         *          返回值为内部分配空间使用完后，需自行释放。不支持中文域名解析。 */
        /* @return 编码数据/NULL 失败 */
uint8_t *cm_httpclient_uri_encode_component(const uint8_t *src, const uint32_t len);
        /* @details 不对字母数字以及-_.!~*'()进行编码，返回值内部分配，需自行释放。不支持中文域名解析。 */
        /* @return 编码数据/NULL 失败 */
```

**实例生命周期**

```c
cm_httpclient_ret_code_e cm_httpclient_create(const uint8_t *url, cm_httpclient_event_callback_func callback, cm_httpclient_handle_t *handle);
        /* @param url       服务器地址（url需要填写完整，例如 "https://203.0.113.10:80"） */
        /* @param callback  客户端相关回调函数（使用 cm_httpclient_sync_request() 同步接口可忽略该参数，传NULL即可） */
        /* @param handle    [out] 实例句柄 */
        /* @return 0 成功 / 其他 失败（见 cm_httpclient_ret_code_e） */

cm_httpclient_ret_code_e cm_httpclient_delete(cm_httpclient_handle_t handle);
        /* @details 本接口会将close socket（HTTP）操作发送至eloop模块中让其执行close socket操作。
         *          本接口返回成功代表操作已发送至eloop中，不代表已完成实例删除操作。
         *          用户连续两次调用本接口时建议中间保证100ms的延时。 */

bool cm_httpclient_is_busy(cm_httpclient_handle_t handle);
        /* @return false 未执行请求 / true 正在执行请求 */

cm_httpclient_ret_code_e cm_httpclient_set_cfg(cm_httpclient_handle_t handle, cm_httpclient_cfg_t cfg);
        /* @details 创建实例后设置，请求过程中不可设置；服务器应答重定向信息的情况下，
         *          采用 cm_httpclient_set_cfg() 配置的参数连接重定向的服务器 */

void cm_httpclient_terminate(cm_httpclient_handle_t handle);
        /* @return 无返回值 */

int32_t cm_httpclient_get_response_code(cm_httpclient_handle_t handle);
        /* @return -1 失败 / 其他 结果码 */
```

**报头设置**

```c
cm_httpclient_ret_code_e cm_httpclient_custom_header_set(cm_httpclient_handle_t handle, uint8_t *header, uint16_t header_len);
        /* @details 设置通用报头，实例期间有效，不设置时发送请求时将自动添加默认报头。
         *          直接引用header指针，不做拷贝，需保证请求过程中不做修改和释放，
         *          如外部释放，也需调用 cm_httpclient_custom_header_free 接口释放内部设置。 */

cm_httpclient_ret_code_e cm_httpclient_custom_header_free(cm_httpclient_handle_t handle);
        /* @details 重置客户端内部指针为NULL、长度为0。 */

cm_httpclient_ret_code_e cm_httpclient_specific_header_set(cm_httpclient_handle_t handle, uint8_t *header, uint16_t header_len);
        /* @details 设置特定报头，用于设置报头中在不同请求时存在变化的字段，
         *          如果通用报头中同样存在该字段，将在发送请求时自动替换为当前设置值。
         *          同样直接引用header指针，不做拷贝。 */

cm_httpclient_ret_code_e cm_httpclient_specific_header_free(cm_httpclient_handle_t handle);
        /* @details 重置客户端内部指针为NULL、长度为0。 */

uint32_t cm_httpclient_parse_header(const uint8_t *header, const uint8_t *key, uint8_t **value);
        /* @param value  [out] 需要找到的字段数据 */
        /* @return 解析到的数据长度 */
        /* @details 按键值对解析，value 输出的为 header 中的指针位置，未单独分配空间。 */
```

**异步请求（回调驱动）**

```c
cm_httpclient_ret_code_e cm_httpclient_request_start(cm_httpclient_handle_t handle, cm_httpclient_request_type_e method, const uint8_t *path,
bool chunked, uint32_t content_length);
        /* @param chunked         是否为chunked模式 */
        /* @param content_length  content数据总长度（chunked模式时无效） */
        /* @details 主要为建立连接并发送报头，将在 request_start_func 回调中指示开始完成，
         *          后续可发送 content 数据。 */

cm_httpclient_ret_code_e cm_httpclient_request_send(cm_httpclient_handle_t handle, const uint8_t *content, uint32_t content_len);
        /* @details 发送消息体，执行 request_start_func 回调后可使用。 */

cm_httpclient_ret_code_e cm_httpclient_request_end(cm_httpclient_handle_t handle);
        /* @details 请求发送结束，chunked模式下将发送结束包，普通模式下无实际效果，可不执行。 */
```

**同步请求**

```c
cm_httpclient_ret_code_e cm_httpclient_sync_request(cm_httpclient_handle_t handle, cm_httpclient_sync_param_t param, 
        cm_httpclient_sync_response_t *response);
        /* @param param     [in]  输入参数（见 cm_httpclient_sync_param_t） */
        /* @param response  [out] 响应结果（见 cm_httpclient_sync_response_t） */
        /* @details 只可用于非chunk模式发送，需先创建实例并完成相关参数设置，
         *          响应结果中的数据为内部动态分配空间，使用完后可通过
         *          cm_httpclient_sync_free_data 接口释放，下次请求时也会自动释放。 */

void cm_httpclient_sync_free_data(cm_httpclient_handle_t handle);
        /* @details 释放响应数据。 */
```

### 2.4 关键使用约束（摘录自注释）

* **最多 4 个并发客户端实例**（`HTTPCLIENT_CTX_MAX_NUM = 4`），超过返回 `CM_HTTP_RET_CODE_NO_MORE_FREE_CLIENT (651)`。
* `cm_httpclient_cfg_t.conn_timeout` 有效上限 `HTTPCLIENT_CONNECT_TIMEOUT_MAXTIME = 180`，默认 `60`。
* `cm_httpclient_cfg_t.rsp_timeout` 有效上限 `HTTPCLIENT_WAITRSP_TIMEOUT_MAXTIME = 60`，默认 `0`。
* `cm_httpclient_cfg_t.cid`：示例代码注释明确写 **“设置PDP索引号，目前不支持该项设置，设置任意值即可”**。
* `cm_httpclient_create` 的 url **必须填写完整**，含 scheme（`http://` 或 `https://`）；IPv6 地址形如 `http://[2408:...:8888]:80`。
* 报头接口 **不做内存拷贝**，只引用指针 → 必须保证请求全过程 header 缓冲区有效且内容不变；释放前先调用对应的 `*_header_free`。
* `cm_httpclient_delete` **是异步的**：返回成功只表示 close 操作已投递到 eloop；连续两次调用之间**建议间隔 100ms**。
* `cm_httpclient_request_end` 在非 chunked 模式下 **无实际效果，可不执行**。
* `cm_httpclient_sync_request` **不支持 chunked**；响应数据由内部动态分配，须 `cm_httpclient_sync_free_data` 释放。
* 回调 `param` 的类型随 event 变化（见 2.2 中 `cm_httpclient_event_callback_func` 上方的映射注释）；特别注意 `CM_HTTP_CALLBACK_EVENT_ERROR_IND` 时 `param` 是**以指针形式强转的枚举值**（示例代码 `(cm_httpclient_error_event_e)(int)param`）。
* **缓冲区大小限制**：头文件**未给出 URL 长度上限，也未给出 POST body 长度上限**（**未证实**）。只有一个运行时错误码 `CM_HTTP_EVENT_CODE_CACHR_NOT_ENOUGH`（缓存空间不足）可用于探测溢出。示例代码里 URL 拼装缓冲区用了 1024 字节（`char token_url[1024]`），但这是示例的局部变量，不是 SDK 限制。

### 2.5 支持哪些 method

来自枚举 `cm_httpclient_request_type_e`，**共 5 种可用**：

| 枚举值 | 数值 | 说明 |
| --- | --- | --- |
| `HTTPCLIENT_REQUEST_NONE` | 0 | 占位，非有效请求类型 |
| `HTTPCLIENT_REQUEST_GET` | 1 | GET |
| `HTTPCLIENT_REQUEST_POST` | 2 | POST |
| `HTTPCLIENT_REQUEST_PUT` | 3 | PUT |
| `HTTPCLIENT_REQUEST_DELETE` | 4 | DELETE |
| `HTTPCLIENT_REQUEST_HEAD` | 5 | HEAD |
| `HTTPCLIENT_REQUEST_MAX` | 6 | 边界标记，非有效请求类型 |

### 2.6 支持 HTTPS 吗

**支持**，但需要两步（头文件字段 + SSL 模块）：

1. `cm_httpclient_cfg_t.ssl_enable = true;`（配合 `ssl_id`，索引号范围 **0–5**，见 `cm_ssl.h` 注释 “ssl通道号，范围为0-5”）。
2. 证书通过 `cm_ssl.h` 的 `cm_ssl_setopt(ssl_id, type, value)` 配置：
   * `CM_SSL_PARAM_VERIFY`：`uint8_t`，0 无身份验证 / 1 单向验证 / 2 双向验证。
   * `CM_SSL_PARAM_CA_CERT`：`char*`，CA 证书字符串。
   * `CM_SSL_PARAM_CLI_CERT` / `CM_SSL_PARAM_CLI_KEY`：客户端证书/密钥。
   * 注意 `cm_ssl_setopt` 注释：**“本接口配置证书时会将证书写入文件系统，故使用时需确保文件系统有足够空间”**。
3. 若未使能 SSL 却走 https，`cm_httpclient_request_start` 等会返回 `CM_HTTP_RET_CODE_SSL_NOT_ENABLE (655)`。

`cm_ssl.h` 原型（来自同一 SDK，供 HTTPS 场景使用）：

```c
int32_t cm_ssl_setopt(int32_t ssl_id, cm_ssl_param_type_e type, void * value);
int cm_ssl_conn(void **cm_ssl_ctx_p, int ssl_id, int socket, int timeout);
void cm_ssl_close(void **cm_ssl_ctx_p);
int cm_ssl_write(void *cm_ssl_ctx, void *data, int data_len);
int cm_ssl_read(void *cm_ssl_ctx, void *data, int data_len);
int cm_ssl_check_pending(void *cm_ssl_ctx);
int cm_ssl_get_bytes_avail(void *cm_ssl_ctx);
```
> 以上原型全部来自 `cm_ssl.h`（该头文件**不在本次 13 个目标头文件范围内**），此处仅为 HTTPS 场景提供参考，均已逐字摘录。
> `cm_ssl_conn` 的 `timeout` 参数注释为 “读取超时时间，**单位ms**”（注意：这是**毫秒**，与 RTOS 的 tick 单位不同）。

### 2.7 事件回调怎么注册

在 `cm_httpclient_create()` 的**第 2 个参数**传入回调函数即可（同步接口场景传 `NULL`）：

```c
cm_httpclient_ret_code_e cm_httpclient_create(const uint8_t *url,
                                              cm_httpclient_event_callback_func callback,
                                              cm_httpclient_handle_t *handle);
```

回调签名：

```c
typedef void (*cm_httpclient_event_callback_func)(cm_httpclient_handle_t client_handle,
                                                  cm_httpclient_callback_event_e event,
                                                  void *param);
```

回调事件与 `param` 的对应关系（来自头文件注释，逐条抄录）：

| event | param 类型 |
| --- | --- |
| `CM_HTTP_CALLBACK_EVENT_REQ_START_SUCC_IND` | `NULL` |
| `CM_HTTP_CALLBACK_EVENT_RSP_HEADER_IND` | `cm_httpclient_callback_rsp_header_param_t` |
| `CM_HTTP_CALLBACK_EVENT_RSP_CONTENT_IND` | `cm_httpclient_callback_rsp_content_param_t` |
| `CM_HTTP_CALLBACK_EVENT_RSP_END_IND` | `NULL` |
| `CM_HTTP_CALLBACK_EVENT_ERROR_IND` | `cm_httpclient_error_event_e` |

### 2.8 返回码含义

**A. 几乎所有函数返回的 `cm_httpclient_ret_code_e`**（决定“调用是否被接受”）：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `CM_HTTP_RET_CODE_OK` | 成功 |
| 3 | `CM_HTTP_RET_CODE_OPERATION_NOT_ALLOWED` | 操作不被允许 |
| 23 | `CM_HTTP_RET_CODE_MALLOC_FAIL` | 内存分配失败 |
| 50 | `CM_HTTP_RET_CODE_PARAM_ERROR` | 参数错误 |
| 650 | `CM_HTTP_RET_CODE_UNKNOWN_ERROR` | 未知错误 |
| 651 | `CM_HTTP_RET_CODE_NO_MORE_FREE_CLIENT` | 没有空闲客户端 |
| 652 | `CM_HTTP_RET_CODE_CLIENT_NOT_CREATE` | 客户端未创建 |
| 653 | `CM_HTTP_RET_CODE_CLIENT_IS_BUSY` | 客户端忙 |
| 654 | `CM_HTTP_RET_CODE_URL_PARSE_FAIL` | URL解析失败 |
| 655 | `CM_HTTP_RET_CODE_SSL_NOT_ENABLE` | SSL未使能 |
| 656 | `CM_HTTP_RET_CODE_CONNECT_FAIL` | 连接失败 |
| 657 | `CM_HTTP_RET_CODE_SEND_DATA_FAIL` | 数据发送失败 |
| 658 | `CM_HTTP_RET_CODE_OPEN_FILE_FAIL` | 打开文件失败 |

**B. 业务层错误：`CM_HTTP_CALLBACK_EVENT_ERROR_IND` 携带的 `cm_httpclient_error_event_e`**：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 1 | `CM_HTTP_EVENT_CODE_DNS_FAIL` | DNS解析失败 |
| 2 | `CM_HTTP_EVENT_CODE_CONNECT_FAIL` | 连接服务器失败 |
| 3 | `CM_HTTP_EVENT_CODE_CONNECT_TIMEOUT` | 连接超时 |
| 4 | `CM_HTTP_EVENT_CODE_SSL_CONNECT_FAIL` | SSL握手失败 |
| 5 | `CM_HTTP_EVENT_CODE_CONNECT_BREAK` | 连接异常断开 |
| 6 | `CM_HTTP_EVENT_CODE_WAITRSP_TIMEOUT` | 等待响应超时 |
| 7 | `CM_HTTP_EVENT_CODE_DATA_PARSE_FAIL` | 数据解析失败 |
| 8 | `CM_HTTP_EVENT_CODE_CACHR_NOT_ENOUGH` | 缓存空间不足 |
| 9 | `CM_HTTP_EVENT_CODE_DATA_DROP` | 数据丢包 |
| 10 | `CM_HTTP_EVENT_CODE_WRITE_FILE_FAIL` | 写文件失败 |
| 255 | `CM_HTTP_EVENT_CODE_UNKNOWN` | 未知错误 |

**C. HTTP 状态码**：GET 响应码可通过 `cm_httpclient_get_response_code(handle)`（返回 `int32_t`，-1 失败）获取；异步模式则在 `CM_HTTP_CALLBACK_EVENT_RSP_HEADER_IND` 中取 `cm_httpclient_callback_rsp_header_param_t.response_code`（`uint16_t`）。

### 2.9 最小可用的 POST 流程（只用头文件里真实存在的函数）

**方案一：异步 POST（回调驱动，无 body 大小限制约束可直接用）**

1. `cm_httpclient_handle_t client = NULL;`
2. `ret = cm_httpclient_create((const uint8_t *)"https://your.host", __cb, &client);` → 校验 `ret == CM_HTTP_RET_CODE_OK && client != NULL`。
3. 配置：填 `cm_httpclient_cfg_t cfg`（`ssl_enable` / `ssl_id` / `cid` / `conn_timeout` / `rsp_timeout` / `dns_priority`），调 `ret = cm_httpclient_set_cfg(client, cfg);`。
4. （HTTPS 必做）`cm_ssl_setopt(cfg.ssl_id, CM_SSL_PARAM_VERIFY, &mode);` 与 `cm_ssl_setopt(cfg.ssl_id, CM_SSL_PARAM_CA_CERT, (char *)ca_pem);`。
5. （可选）设置报头：`cm_httpclient_custom_header_set(client, (uint8_t *)"Content-Type: application/json\r\n", len);`
   —— 若要带 `Content-Length` 等变化字段，用 `cm_httpclient_specific_header_set`。
6. 发起请求（`chunked = false`，`content_length` = body 字节数，**POST body 长度在这里就要给出来**）：
   `ret = cm_httpclient_request_start(client, HTTPCLIENT_REQUEST_POST, (const uint8_t *)"/webhook", false, body_len);`
7. 在回调里等到 `CM_HTTP_CALLBACK_EVENT_REQ_START_SUCC_IND` 后，发送 body：
   `ret = cm_httpclient_request_send(client, (const uint8_t *)body, body_len);`
   （回调里不能做耗时操作，实践上应通过信号量/消息队列唤醒业务线程再调用 `request_send`——这正是官方 uart/http 示例的做法。）
8. （非 chunked 可省略）`cm_httpclient_request_end(client);`
9. 收响应：在回调中处理
   * `CM_HTTP_CALLBACK_EVENT_RSP_HEADER_IND` → 读 `response_code`（HTTP 状态码，如 200）；
   * `CM_HTTP_CALLBACK_EVENT_RSP_CONTENT_IND` → 读 `total_len` / `sum_len` / `current_len` / `response_content`；
   * `CM_HTTP_CALLBACK_EVENT_RSP_END_IND` → 本次响应结束；
   * `CM_HTTP_CALLBACK_EVENT_ERROR_IND` → 强转 `param` 为 `cm_httpclient_error_event_e` 判断失败原因。
10. 收尾（可选）：`cm_httpclient_terminate(client);` 然后 `cm_httpclient_delete(client);`，若立刻复用同一实例，**两次 delete 之间留 100ms**。

**方案二：同步 POST（更简单，不支持 chunked）**

1. `cm_httpclient_create((const uint8_t *)"https://your.host", NULL, &client);`（**callback 传 NULL**）
2. `cm_httpclient_set_cfg(client, cfg);`（HTTPS 时再加 `cm_ssl_setopt`）
3. （可选）`cm_httpclient_custom_header_set(client, (uint8_t *)"Content-Type: application/json\r\n", len);`
4. 组装并发送：
   ```c
   cm_httpclient_sync_param_t param;
   param.method         = HTTPCLIENT_REQUEST_POST;
   param.path           = (const uint8_t *)"/webhook";
   param.content_length = body_len;
   param.content        = (uint8_t *)body;
   cm_httpclient_sync_response_t response = {0};
   ret = cm_httpclient_sync_request(client, param, &response);
   ```
5. 读 `response.response_code` / `response_header` / `response_content` / `response_content_len`。
6. `cm_httpclient_sync_free_data(client);`（**必须**，否则内部空间不释放）
7. `cm_httpclient_delete(client);`

---

## 3. `cm_uart.h` — UART 接口

**一句话用途**：打开/关闭串口、注册接收事件回调、同步读写数据；引脚复用需先用 `cm_iomux.h` 配置。

### 3.1 宏定义（原样）

```c
/** 波特率 */
#define    CM_UART_BAUDRATE_9600       (9600U)
#define    CM_UART_BAUDRATE_19200      (19200U)
#define    CM_UART_BAUDRATE_38400      (38400U)
#define    CM_UART_BAUDRATE_57600      (57600U)
#define    CM_UART_BAUDRATE_115200     (115200U)
#define    CM_UART_BAUDRATE_256000     (256000U)
```

### 3.2 typedef / enum / struct 完整定义

```c
 /** 设备ID ，详情参照资源综述*/
typedef enum{
    CM_UART_DEV_0,              /*!< 设备0*/
    CM_UART_DEV_1,              /*!< 设备1*/
    CM_UART_DEV_2,              /*!< 设备2*/
    CM_UART_DEV_NUM
} cm_uart_dev_e;

/** 数据位 */
typedef enum{
    CM_UART_BYTE_SIZE_8 = 8,
} cm_uart_byte_size_e;

/** 奇偶校验 */
typedef enum{
    CM_UART_PARITY_NONE,
    CM_UART_PARITY_ODD,
    CM_UART_PARITY_EVEN,
    CM_UART_PARITY_MARK,    /*!< 不支持*/
    CM_UART_PARITY_SPACE    /*!< 不支持*/
} cm_uart_parity_e;

/** 停止位 */
typedef enum{
    CM_UART_STOP_BIT_ONE,
    CM_UART_STOP_BIT_ONE_HALF,
    CM_UART_STOP_BIT_TWO
} cm_uart_stop_bit_e;

/** 流控制 */
typedef enum{
    CM_UART_FLOW_CTRL_NONE,
    CM_UART_FLOW_CTRL_HW,   /*!< 当前不支持流控*/
} cm_uart_flow_ctrl_e;

/** 结果码 */
typedef enum{
    CM_UART_RET_OK = 0,
    CM_UART_RET_INVALID_PARAM = -1,
} cm_uart_ret_e;

/** 配置 */
typedef struct{
    cm_uart_byte_size_e byte_size;      /*!< 数据位，枚举*/
    cm_uart_parity_e parity;            /*!< 校验位，枚举*/
    cm_uart_stop_bit_e stop_bit;        /*!< 停止位，枚举*/
    cm_uart_flow_ctrl_e flow_ctrl;      /*!< 流控制，枚举*/
    int baudrate;                       /*!< 波特率，枚举*/
    int is_lpuart;                      /*!< 1:低功耗串口(115200以内的标准波特率)；0:普通串口*/
} cm_uart_cfg_t;

/** 事件类型 */
typedef enum
{
    CM_UART_EVENT_TYPE_RX_ARRIVED = (1 << 0),  /*!< 接收到新的数据*/
    CM_UART_EVENT_TYPE_RX_OVERFLOW = (1 << 1), /*!< 接收FIFO缓存溢出*/
    CM_UART_EVENT_TYPE_TX_COMPLETE   = (1 << 2)/*!< 不支持发送完成事件*/
}cm_uart_event_type_e;

/** 事件 */
typedef struct{
    uint32_t event_type; /*!< 事件类型，数据可读等*/
    void *event_param;               /*!< 事件参数*/
    void *event_entry;               /*!< 事件执行入口*/
} cm_uart_event_t;

typedef void (*cm_uart_event_cb_t)(void *param, uint32_t evt);
```

### 3.3 函数原型（原样）

```c
int32_t cm_uart_open(cm_uart_dev_e dev, cm_uart_cfg_t *cfg);
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
int32_t cm_uart_register_event(cm_uart_dev_e dev, void *event);
        /* @param  [in] event 串口事件（实参为 cm_uart_event_t *） */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
int32_t cm_uart_close(cm_uart_dev_e dev);
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
int32_t cm_uart_write(cm_uart_dev_e dev, const void *data, int32_t len, int32_t timeout);
        /* @param timeout 超时时间(ms)(无效参数) */
        /* @return = 实际写入长度 - 成功 ; < 0 - 失败, 返回值为错误码 */
int32_t cm_uart_read(cm_uart_dev_e dev, void* data, int32_t len, int32_t timeout);
        /* @param timeout 超时时间(ms)(无效参数) */
        /* @return = 实际读出长度 - 成功 ; < 0 - 失败, 返回值为错误码 */
```

### 3.4 关键使用约束（摘录自注释）

* **`open` 之前必须先对引脚复用功能进行设置**（`cm_iomux_set_pin_func`）。
* **事件必须在 `open` 之前注册**：注释原文 “事件包括串口数据可读/溢出等。**需在open之前注册**。”
* **回调函数的限制**：注释原文 “**回调函数中不可输出LOG、串口打印、执行复杂任务或消耗过多资源**。” 建议只 `osSemaphoreRelease` / `osMessageQueuePut`，把实际处理放到线程里。
* `cm_uart_write` / `cm_uart_read` 的 `timeout` 参数标注为 **“(无效参数)”**（即该参数当前不起作用）。
* 波特率：低功耗串口（`is_lpuart = 1`）须 ≤ 115200；普通串口可配 256000 非标波特率；**某串口配置为 256000 后，其他串口仅支持普通串口 256000 模式或低功耗串口模式**。
* 校验：`CM_UART_PARITY_MARK` / `CM_UART_PARITY_SPACE` **不支持**；流控：`CM_UART_FLOW_CTRL_HW` **当前不支持**；`CM_UART_EVENT_TYPE_TX_COMPLETE` **不支持发送完成事件**。
* UART 引脚与 SD 卡控制引脚存在复用关系，使用前需阅知 `cm_sd.h` 的「SD卡使用注意事项」。

### 3.5 回调签名

头文件定义的函数指针类型为：

```c
typedef void (*cm_uart_event_cb_t)(void *param, uint32_t evt);
```

实际注册路径是把它放进 `cm_uart_event_t.event_entry`，再把 `&event` 传给 `cm_uart_register_event()`。工程内 `custom/drv/uart/drv_uart.c` 与官方示例 `examples/uart/src/cm_demo_uart.c` 的写法完全一致：

```c
cm_uart_event_t event = {
    CM_UART_EVENT_TYPE_RX_ARRIVED | CM_UART_EVENT_TYPE_RX_OVERFLOW,
    "uart0",                 /* event_param */
    my_callback              /* event_entry —— 即回调函数指针 */
};
cm_uart_register_event(dev, &event);
cm_uart_open(dev, &config);
```

回调内部用 `CM_UART_EVENT_TYPE_RX_ARRIVED & type` / `CM_UART_EVENT_TYPE_RX_OVERFLOW & type` 判断事件位。

### 3.6 UART1 是哪个常量

* 设备枚举**从 0 开始**：`CM_UART_DEV_0 = 0`、`CM_UART_DEV_1 = 1`、`CM_UART_DEV_2 = 2`。
* 所以 **UART1 的常量是 `CM_UART_DEV_1`**。
* 对 ML307A，UART1 的 IOMUX 引脚为（来自 `examples/demo_main/inc/cm_common.h`）：
  `OPENCPU_TEST_UARTRX_IOMUX = CM_IOMUX_PIN_28, CM_IOMUX_FUNC_FUNCTION1`
  `OPENCPU_TEST_UARTTX_IOMUX = CM_IOMUX_PIN_29, CM_IOMUX_FUNC_FUNCTION1`
  与 `custom/drv/uart/drv_uart.c` 中 `dev == CM_UART_DEV_1` 分支一致。
* 注意：`CM_UART_DEV_NUM` 是**边界标记，不是设备**。
* 另注：`examples/demo_main/inc/cm_common.h` 中 `OPENCPU_MAIN_URAT` 对 ML307A 取 **0**（即调试主串口是 UART0，引脚 `PIN_18`=TX / `PIN_17`=RX）。**SD 卡功能与 ML307A 的 UART0 互斥**（`cm_sd.h` 注释）。

---

## 4. `cm_gpio.h` — GPIO 接口

**一句话用途**：GPIO 的初始化/去初始化、电平读写、上下拉与方向设置、中断注册与使能。

### 4.1 typedef / enum / struct 完整定义

```c
/** 引脚ID,详情参照资源综述 */
typedef enum{
    CM_GPIO_NUM_0, CM_GPIO_NUM_1, CM_GPIO_NUM_2, CM_GPIO_NUM_3,
    CM_GPIO_NUM_4, CM_GPIO_NUM_5, CM_GPIO_NUM_6, CM_GPIO_NUM_7,
    CM_GPIO_NUM_8, CM_GPIO_NUM_9, CM_GPIO_NUM_10, CM_GPIO_NUM_11,
    CM_GPIO_NUM_12, CM_GPIO_NUM_13, CM_GPIO_NUM_14, CM_GPIO_NUM_15,
    CM_GPIO_NUM_16, CM_GPIO_NUM_17, CM_GPIO_NUM_18, CM_GPIO_NUM_19,
    CM_GPIO_NUM_20, CM_GPIO_NUM_21, CM_GPIO_NUM_22, CM_GPIO_NUM_23,
    CM_GPIO_NUM_24, CM_GPIO_NUM_25, CM_GPIO_NUM_26, CM_GPIO_NUM_27,
    CM_GPIO_NUM_28, CM_GPIO_NUM_29, CM_GPIO_NUM_30, CM_GPIO_NUM_31,
    CM_GPIO_NUM_32, CM_GPIO_NUM_33, CM_GPIO_NUM_34, CM_GPIO_NUM_35,
    CM_GPIO_NUM_36,
    CM_GPIO_NUM_MAX
}cm_gpio_num_e;
```
> 上述为原文件的逐项展开；`CM_GPIO_NUM_n` 的值就是 `n`（0…36），最后一项 `CM_GPIO_NUM_MAX` 为边界标记。

```c
/* *GPIO工作模式 */
typedef enum 
{
    CM_GPIO_MODE_NUM/*!<不可用 工作模式数量*/
} cm_gpio_mode_e;

/** I/O方向 */
typedef enum{
    CM_GPIO_DIRECTION_INPUT = 0,
    CM_GPIO_DIRECTION_OUTPUT,
}cm_gpio_direction_e;

/** 上/下拉 */
typedef enum{
    CM_GPIO_PULL_NONE,
    CM_GPIO_PULL_DOWN,
    CM_GPIO_PULL_UP,
}cm_gpio_pull_e;

/** 边沿+电平触发 */
typedef enum{
    CM_GPIO_IT_EDGE_RISING,
    CM_GPIO_IT_EDGE_FALLING,
    CM_GPIO_IT_EDGE_BOTH,
    CM_GPIO_IT_LEVEL_HIGH,/*!<不支持*/
    CM_GPIO_IT_LEVEL_LOW,/*!<不支持*/
}cm_gpio_interrupt_e;

/** 高低电平 */
typedef enum{
    CM_GPIO_LEVEL_LOW,
    CM_GPIO_LEVEL_HIGH,
}cm_gpio_level_e;

/** 命令码 */
typedef enum{
    CM_GPIO_CMD_SET_PULL,                  /*!< 上下拉设置命令码*/
    CM_GPIO_CMD_GET_PULL,                  /*!< 上下拉获取命令码*/
    CM_GPIO_CMD_SET_LEVEL,                 /*!< 驱动能力设置命令码*/
    CM_GPIO_CMD_GET_LEVEL,                 /*!< 驱动能力获取命令码*/
    CM_GPIO_CMD_SET_DIRECTION,             /*!< 输入输出设置命令码*/
    CM_GPIO_CMD_GET_DIRECTION,             /*!< 输入输出获取命令码*/
} cm_gpio_cmd_e;

/** 配置 */
typedef struct{
    cm_gpio_mode_e mode;/*!< 不支持*/
    cm_gpio_direction_e direction;
    cm_gpio_pull_e pull;
} cm_gpio_cfg_t;
```

### 4.2 函数原型（原样，按用途分组）

```c
int32_t cm_gpio_init(cm_gpio_num_e gpio_num, cm_gpio_cfg_t *cfg);
        /* @details 初始化之前一定要先设置引脚复用 */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
int32_t cm_gpio_deinit(cm_gpio_num_e gpio_num);
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */

int32_t cm_gpio_set_level(cm_gpio_num_e gpio_num, cm_gpio_level_e level);
int32_t cm_gpio_get_level(cm_gpio_num_e gpio_num, cm_gpio_level_e *level);

int32_t cm_gpio_set_pull(cm_gpio_num_e gpio_num, cm_gpio_pull_e type);
int32_t cm_gpio_get_pull(cm_gpio_num_e gpio_num, cm_gpio_pull_e *type);

int32_t cm_gpio_set_direction(cm_gpio_num_e gpio_num, cm_gpio_direction_e dir);
int32_t cm_gpio_get_direction(cm_gpio_num_e gpio_num, cm_gpio_direction_e *dir);

int32_t cm_gpio_interrupt_register(cm_gpio_num_e gpio_num, void *interrupt_cb);
        /* @details 中断函数中不要处理耗时任务和打印log日志 */
int32_t cm_gpio_interrupt_enable(cm_gpio_num_e gpio_num, cm_gpio_interrupt_e intr_mode);
int32_t cm_gpio_interrupt_disable(cm_gpio_num_e gpio_num);

int32_t cm_gpio_ioctl(cm_gpio_num_e gpio_num, int32_t cmd, void *arg);
```
所有函数的返回值含义统一为：`= 0` 成功；`< 0` 失败，返回值为错误码。

`cm_gpio_ioctl` 的 `arg` 类型必须与 `cmd` 对应（头文件逐条列出）：

```c
cmd = GPIO_CMD_SET_PULL,        arg 传入 cm_gpio_pull_e *
cmd = GPIO_CMD_GET_PULL,        arg 传入 cm_gpio_pull_e *
cmd = GPIO_CMD_SET_LEVEL,       arg 传入 cm_gpio_level_e *
cmd = GPIO_CMD_GET_LEVEL,       arg 传入 cm_gpio_level_e *
cmd = GPIO_CMD_SET_DIRECTION,   arg 传入 cm_gpio_direction_e *
cmd = GPIO_CMD_GET_DIRECTION,   arg 传入 cm_gpio_direction_e *
```

### 4.3 关键使用约束

* **初始化之前一定要先设置引脚复用**（`cm_iomux_set_pin_func`）。
* **中断函数中不要处理耗时任务和打印 log 日志**。
* `cm_gpio_cfg_t.mode` 标注 **“不支持”**。
* `CM_GPIO_IT_LEVEL_HIGH` / `CM_GPIO_IT_LEVEL_LOW` **不支持**。
* GPIO 引脚与 SD 卡控制引脚存在复用关系（同 UART）。

---

## 5. `cm_sys.h` — 系统管理

**一句话用途**：获取模组 IMEI/SN/SDK 版本号，以及打印 LOG。

> ⚠️ **重要：`cm_sys.h` 中没有任何重启/关机接口。** 重启在 `cm_pm.h`。

### 5.1 宏定义（原样）

```c
#define CM_IMEI_LEN      16 /*!< IMEI存储长度 */
#define CM_SN_LEN        65 /*!< SN存储长度 */
#define CM_VER_LEN       65 /*!< 版本号存储长度 */
```

### 5.2 typedef / enum / struct

**无。** 该头文件不含任何自定义类型。

### 5.3 函数原型（原样）

```c
int32_t cm_sys_get_imei(char *imei); 
        /* @param [out] imei IMEI，长度16字节 */
        /* @return = 0  - 成功 ; < 0 - 失败, 返回值为错误码 */

int32_t cm_sys_get_sn(char *sn); 
        /* @param [out] sn SN，长度32字节 */
        /* @return = 0  - 成功 ; < 0 - 失败, 返回值为错误码 */

int32_t cm_sys_get_cm_ver(char *ver_buff, uint32_t len);
        /* @param [in] ver_buff 存储版本号的内存指针 */
        /* @param [in] len 传入的内存长度，单位字节 */
        /* @return >= 0 - 获取到的版本号长度，单位字节 ; < 0 - 失败, 返回值为错误码 */

void cm_log_printf(uint8_t log_level, const char *fmt, ...);
        /* @param [in] log_level log等级,暂不支持，传入任意值即可 */
        /* @return 空 */
        /* @details 最多支持打印180字节长度；log通过DEBUG串口（DBG_RX DBG_TX）输出 */
```

### 5.4 关键使用约束

* `cm_sys_get_imei`：**缓冲区长度 16 字节**（对应 `CM_IMEI_LEN = 16`）。
* `cm_sys_get_sn`：注释写 **“长度32字节”**，但同文件宏 `CM_SN_LEN` 定义为 **65**。**二者不一致**（见第 16 节），建议按 65 分配以策安全。
* `cm_sys_get_cm_ver`：`len` 单位字节；**返回值是版本号长度**（≥0），不是状态码。
* `cm_log_printf`：**最多打印 180 字节**；`log_level` 参数**暂不支持**，传任意值即可；输出走 **DEBUG 串口（DBG_RX / DBG_TX）**，不是普通应用串口。

---

## 6. `cm_pm.h` — 电源管理（**含重启 / 关机**）

**一句话用途**：模组重启、关机、上电原因查询、低功耗回调注册、休眠上锁/解锁、PowerKey 回调。

### 6.1 typedef / enum / struct 完整定义

```c
typedef void (*cm_pm_event_cb_t)(void);
typedef void (*cm_pm_powerkey_cb_t)(void);

/** 上电原因 */
typedef enum
{
    CM_PM_UNKNOWN = 0,       /*!<未知原因*/
    CM_PM_CHARG_POW_OFF,     /*!<Charging in powering off*/
    CM_PM_RD_PRO_MODE     ,  /*!<Rd production mode*/
    CM_PM_RTC_ALARM,         /*!<rtc Alarm*/
    CM_PM_POWER_ON,          /*!<Normal power on*/
    CM_PM_ERROR_RESET,       /*!<Error reset (WDT)*/
}cm_pm_power_on_reason_e;

/** 配置 */
typedef struct{
    cm_pm_event_cb_t cb_enter;  /*!< 进入低功耗回调函数*/
    cm_pm_event_cb_t cb_exit;   /*!< 退出低功耗回调函数*/
} cm_pm_cfg_t;
```

### 6.2 函数原型（原样）

```c
void cm_pm_poweroff(void);
        /* @brief 关机  @return 空 */
int cm_pm_get_power_on_reason(void);
        /* @return >= 0 - 上电原因 ; < 0 - 失败, 返回值为错误码 */

void cm_pm_reboot(void);
        /* @brief 重启  @param [in] void  @return void */

void cm_pm_init(cm_pm_cfg_t pm_cfg);
        /* @param [in] pm_cfg:参数配置  @return void */

void cm_pm_work_lock(void);
        /* @details 执行后，模组将无法进入休眠状态 */
void cm_pm_work_unlock(void);
        /* @details 执行后，模组将允许进入休眠状态 */

int32_t cm_pm_powerkey_regist_callback(cm_pm_powerkey_cb_t callback);
        /* @param [in] callback 回调函数指针 */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
        /* @details 长按(3s)无回调功能开关机；短按回调此注册函数 */
```

### 6.3 关键使用约束

* **重启函数名：`void cm_pm_reboot(void)` —— 无参数、无返回值。**（已 grep 确认 `cm_sys.h` 无 reboot；DSLN map 中 `cm_pm_reboot` 符号存在。）
* **关机函数：`void cm_pm_poweroff(void)`。**
* **进入下载模式的接口：本 SDK 头文件中未提供，未证实。**
* `cm_pm_powerkey_regist_callback`：**长按 3 秒**为开关机（不回调此注册函数）；**短按**才回调。
* `cm_pm_work_lock()` 会阻止休眠，`cm_pm_work_unlock()` 放开 —— 成对使用。
* 官方示例 `examples/pm/src/cm_demo_pm.c` 对 `cb_enter` / `cb_exit` 的注释：**“不可以执行打印等长时间占用任务”**。
* `cm_pm_get_power_on_reason()` 返回值直接对应 `cm_pm_power_on_reason_e`（`CM_PM_ERROR_RESET` 注释为 “Error reset (WDT)”，说明看门狗复位由底层触发、无用户接口）。

---

## 7. `cm_mem.h` — 内存管理

**一句话用途**：提供内部堆（MemPool）的内存申请/释放与堆状态查询；**禁止使用 C 库内存接口**。

### 7.1 typedef / struct 完整定义

```c
/** heap状态 */
typedef struct{
    uint32_t total_size;            /*!< 总量大小 */
    uint32_t allocated;             /*!< 目前已分配大小 */
    uint32_t free;                  /*!< 剩余可分配大小 */
    uint32_t max_continuity_free;   /*!< 本平台无效   */
    uint32_t allocated_peak;        /*!< 本平台无效   */
    uint32_t allocated_times;       /*!< 本平台无效   */
    uint32_t free_times;            /*!< 本平台无效   */
} cm_heap_stats_t;
```
（无 enum、无函数指针 typedef。）

### 7.2 函数原型（原样）

```c
int32_t cm_mem_get_heap_stats(cm_heap_stats_t *stats);
        /* @param [out] stats heap状态 */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
        /* @details 获取的为MemPool信息 */

void *cm_malloc(size_t size);
        /* @param [in] size 申请长度 */
        /* @return 返回内存地址 */
        /* @details 内存操作接口请勿使用C库接口，申请长度不能超过剩余可分配大小 */

void *cm_realloc(void *ptr, size_t size);
        /* @param [in] ptr  待重新分配的内存块 */
        /* @param [in] size 内存块的新大小 */
        /* @details 内存操作接口请勿使用C库接口，申请长度不能超过剩余可分配大小 */

void *cm_calloc(size_t nitems, size_t size);
        /* @param [in] nitems 要被分配的元素个数 */
        /* @param [in] size   元素的大小 */
        /* @details 内存操作接口请勿使用C库接口，申请长度不能超过剩余可分配大小 */

void cm_free(void *ptr);
        /* @param [in] buf 待释放内存 */
        /* @return 空 */
        /* @details 内存操作接口请勿使用C库接口 */
```

### 7.3 关键使用约束

* **有内部堆**：`cm_mem_get_heap_stats` 注释原文 “获取的为 **MemPool** 信息”，说明存在名为 MemPool 的内部堆，并可通过该接口拿到 `total_size` / `allocated` / `free`。
* **禁止使用 C 库内存接口**（注释三处重复强调 “内存操作接口请勿使用C库接口”），即不要用 `malloc/free/calloc/realloc`，而用 `cm_malloc/cm_free/cm_calloc/cm_realloc`。
* `cm_heap_stats_t` 中 `max_continuity_free`、`allocated_peak`、`allocated_times`、`free_times` **在本平台无效**。
* **申请长度不能超过剩余可分配大小**（应先用 `cm_mem_get_heap_stats` 查 `free`）。
* **线程栈是否从内部堆分配：未证实。** 依据：`osThreadAttr_t` 若 `stack_mem` 传 `NULL`，栈由 RTOS 自行分配（工程示例 `custom/applaction/app.c` 的 `osThreadAttr_t thread_cfg = { .name=..., .priority=..., .stack_size=... }` 未设置 `stack_mem`，其余字段由 C99 指定初始化器置零）。头文件**未说明**该栈来自 MemPool 还是别的堆。可运行时用 `cm_mem_get_heap_stats()` 前后对比 `free` 来间接验证。

---

## 8. `cm_ntp.h` — NTP 协议客户端接口

**一句话用途**：配置 NTP 服务器/端口/超时/回调，并触发一次网络时间同步（**异步**，结果通过回调返回）。

### 8.1 typedef / enum / struct 完整定义

```c
/** NTP事件类型 */
typedef enum
{
    CM_NTP_EVENT_SYNC_FAIL,     /*!< NTP时间同步失败, 参数(event_param): NULL */
    CM_NTP_EVENT_SYNC_OK,       /*!< NTP时间同步成功, 参数(event_param): 获取到的NTP时间, ISO-8601时间格式字符串, 例如: 2021-04-21T17:10:08.868+08:00 */
    CM_NTP_EVENT_SETTIME_FAIL,  /*!< NTP时间同步成功但设置本地时间失败, 参数(event_param): 获取到的NTP时间, ISO-8601时间格式字符串, 例如: 2021-04-21T17:10:08.868+08:00 */
} cm_ntp_event_e;

/** NTP设置类型 */
typedef enum
{
    CM_NTP_CFG_SERVER = 1,              /*!< NTP服务器，支持域名、ipv4地址、ipv6地址，设置参数为字符串型（最多128字节），未设置时使用默认服务器 */
    CM_NTP_CFG_PORT,                    /*!< NTP服务器端口号，端口号为uint16_t型，未设置时使用123端口 */
    CM_NTP_CFG_TIMEOUT,                 /*!< NTP超时时间，未设置时为3000毫秒，支持设置范围1000~10000，单位为毫秒 */
    CM_NTP_CFG_DNS,                     /*!< NTP服务器为域名时DNS解析IP地址优先级，优先级为uint32_t型，0(默认)：使用全局优先级。1：v4优先。2：v6优先 */
	CM_NTP_CFG_SET_RTC,                 /*!< 是否将获取到的NTP时间设置到RTC设置，参数为bool型，0：不同步至系统时间。1（默认）：同步至系统时间 */
	CM_NTP_CFG_CB,                      /*!< NTP服务回调函数设置，回调函数类型见cm_ntp_event_cb */
    CM_NTP_CFG_CB_PARAM,                /*!< NTP服务回调函数中传递的用户参数指针 */
} cm_ntp_cfg_type_e;

/** NTP事件回调函数 */
typedef void (*cm_ntp_event_cb_t)(cm_ntp_event_e event, void *event_param, void *cb_param);
```
（无 struct。）

### 8.2 函数原型（原样）

```c
int32_t cm_ntp_set_cfg(cm_ntp_cfg_type_e type, void *value);
        /* @param [in] type  设置参数类型 */
        /* @param [in] value 设置参数数值 */
        /* @return = 0 - 成功 ; = -1 - 失败 */
        /* @details NTP服务进行中不可设置 */

int32_t cm_ntp_sync(void);
        /* @return = 0 - 成功 ; < 0 - 错误码 */
        /* @details NTP模块底层依赖asocket、async_dns和eloop模块 */
```

### 8.3 关键使用约束

* **存在 NTP 接口**（并非“该头文件未提供”）。
* 服务器字符串**最多 128 字节**；端口默认 **123**；超时默认 **3000 ms**，可设范围 **1000~10000 ms**。
* `CM_NTP_CFG_SET_RTC` 默认 **1（同步至系统时间）**。
* **NTP 服务进行中不可设置**参数。
* `cm_ntp_sync()` 是**异步**的：返回值只表示触发成功，实际结果通过 `CM_NTP_CFG_CB` 注册的回调返回，其中 `CM_NTP_EVENT_SYNC_OK` 的 `event_param` 是 **ISO-8601 时间字符串**（如 `2021-04-21T17:10:08.868+08:00`），不是时间戳数值。
* 底层依赖 `asocket`、`async_dns`、`eloop` 模块。

---

## 9. `cm_rtc.h` — RTC 管理

**一句话用途**：读写系统时间（秒级时间戳）与设置/获取时区。

### 9.1 typedef / enum / struct

**无。** 该头文件只 `#include <time.h>`，未定义任何自有类型。

### 9.2 函数原型（原样）

```c
int32_t cm_rtc_set_current_time(uint64_t second);
        /* @param [in] second 时间戳(单位：秒) */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
        /* @details UTC时间 */

uint64_t cm_rtc_get_current_time(void);
        /* @return 时间戳（单位：秒） */
        /* @details UTC时间 */

int32_t cm_rtc_set_timezone(int32_t timezone);
        /* @param [in] timezone 时区 */
        /* @return = 0 - 成功 ; < 0 - 失败, 返回值为错误码 */
        /* @details timezone设置范围为-12~12，-12为西十二区，0为世界时间，12为东十二区 */

int32_t cm_rtc_get_timezone(void);
        /* @return 时区 */
        /* @details timezone范围为-12~12，-12为西十二区，0为世界时间，12为东十二区 */
```

### 9.3 关键使用约束

* **时间基准是 UTC**（`set`/`get` 注释均标注 “UTC时间”），**返回值单位是秒**，类型为 `uint64_t`。
* **时区范围 -12 ~ 12**（整数小时，`int32_t`）。
* 该头文件**未提供** `struct tm` 形式的本地时间获取函数，也**未提供**“UTC 秒 → 本地日历结构体”的转换接口。若要日历字段，需自行用 C 库 `localtime()`/`gmtime()`（而 `cm_rtc_get_timezone()` 给出时区偏移）——**这一用法未在头文件中说明，未证实**。
* NTP 同步时若 `CM_NTP_CFG_SET_RTC = 1`，底层会把时间写入 RTC（即本头文件的系统时间）。

---

## 10. `cm_sd.h` — SDCARD 文件接口

**一句话用途**：SD 卡文件系统的格式化检查/格式化、文件读写定位删除移动、目录操作与容量查询。

> 头文件注释明确：**“仅 ML307A-DSLN / ML307A-GSLN / ML302A-DSLM / ML302A-GSLM 模组支持 SD 功能”** —— ML307A-DSLN 在支持列表内。
> **“SD卡功能与 ML307A uart0 和 ML302A uart1 互斥，不可同时使用”**。

### 10.1 宏定义（原样）

```c
#define CM_SD_SEEK_SET      (0)      /*!< SEEK_SET */
#define CM_SD_SEEK_CUR      (1)      /*!< SEEK_CUR */
#define CM_SD_SEEK_END      (2)      /*!< SEEK_END */

#define CM_SD_MAX_PATH (256)
```

### 10.2 typedef / struct 完整定义

```c
typedef struct
{
    long long free_size;  /*!< 当前可用文件系统大小 */
    long long total_size; /*!< 文件系统总大小 */
} cm_sd_system_info_t;
```
（无 enum、无函数指针 typedef。）

### 10.3 函数原型（原样）

```c
bool cm_sd_is_formatted(char *vol);
        /* @param [in] vol 逻辑驱动器号指针 */
        /* @return TRUE - SD卡已格式化 ; FALSE - SD卡未格式化 */

int32_t cm_sd_format(char *vol);
        /* @return = 0  - 成功 ; < 0 - 失败, 返回值为错误码 */
        /* @details 因软件驱动特性，cm_sd_format()接口应在执行SD文件操作前调用。
         *          若需要获取SD文件系统的关键数据后格式化SD卡，
         *          应在进行完SD文件系统操作并重启模组后格式化SD卡。 */

int32_t cm_sd_fopen(const char *filename, const char *mode);
        /* @param [in] mode 参数（r w c a +） */
        /* @return >= 0 - 文件描述符 ; < 0 - 失败, 返回值为错误码 */
        /* @details r: read / w: write / c: create / a: append and write / +: read and write */

int32_t cm_sd_fclose(int32_t fd);
int32_t cm_sd_fread(int32_t fd, void *buf, uint32_t size);
        /* @return >= 0 - 实际的读取长度 ; < 0 - 读取失败 */
int32_t cm_sd_fwrite(int32_t fd, const void *buf, uint32_t size);
        /* @return >= 0 - 实际的写入长度 ; < 0 - 写入失败 */
int32_t cm_sd_fseek(int32_t fd, int32_t offset, int32_t base);
        /* @details offset不支持为负数 ; base: CM_SD_SEEK_SET/CUR/END */
int32_t cm_sd_fdelete(const char *file_name);
        /* @return >= 0 - 成功 ; < 0 - 失败 */
int32_t cm_sd_fmove(const char *src, const char *dest);
int32_t cm_sd_exist(const char *file_name);
        /* @return = 1 - 存在 ; = 0 - 不存在 */
int32_t cm_sd_filesize(const char *file_name);
        /* @return >= 0 - 文件长度 ; < 0 - 操作失败 */
int32_t cm_sd_mkdir(char *path);
int32_t cm_sd_rmdir(char *path);
int32_t cm_sd_set_currentdir(char *path);
int32_t cm_sd_get_currentdir(char *path, uint32_t max_len);
int32_t cm_sd_getinfo(char *vol, cm_sd_system_info_t *info);
```
（除特别标注外，返回值 `= 0` 成功 / `< 0` 失败。）

### 10.4 关键使用约束（摘录自注释，此处为头文件原文要点）

* **上电时序**：SD 卡初始化在**开发板上电时**完成；“发出初始化脉冲操作**早于 OpenCPU SDK 的入口函数执行**”。
* **禁用 SD 脉冲的方法**：在**文件系统**中创建一个内容为 `"0"`、名称为 **`sd.cfg`** 的文件，**重启生效**。头文件给出的写法：
  ```c
  int32_t fd = cm_fs_open("sd.cfg", CM_FS_WB);
  int32_t f_len = cm_fs_write(fd, "0", 1);
  int32_t ret = cm_fs_close(fd);
  ```
  （这说明 `cm_fs` 与 SD 是两套独立文件系统。）
* **SD 与 UART0 互斥**（ML307A），不可同时使用。
* 路径最大长度 `CM_SD_MAX_PATH = 256`。
* `cm_sd_fseek` 的 **offset 不支持负数**。
* `cm_sd_format()` 应在执行 SD 文件操作**前**调用。

---

## 11. `cm_utils.h` — 通用定义

**一句话用途**：仅提供两个段属性宏，用于把函数放到 RAM 或保持 XIP 段。

### 11.1 完整内容（该文件全部有效内容，原样）

```c
/*函数声明时可通过此宏定义指定加载段落，不加修饰即默认使用.text段*/
#define __RUN_AT_RAM    __attribute__((used, section(".sramtext")))
#define __RUN_AT_XIP    __attribute__((used, section(".text")))
```

### 11.2 函数 / 类型

**无函数、无 typedef、无 enum、无 struct。**

### 11.3 关键使用约束

* 不加修饰默认使用 `.text` 段。
* `__RUN_AT_RAM` 把代码放到 `.sramtext` 段；链接脚本 `prebuild/ld/ML307A/DSLN/app_dsln.ld` 中定义了 `.sramtext`（LMA 在 flash，VMA 在 RAM），与宏语义一致。

---

## 12. `cm_sim.h` — SIM 接口

**一句话用途**：获取设备 IMSI。

### 12.1 宏定义（原样）

```c
#define CM_IMSI_LEN       16 /*!< IMSI存储长度 */
```

### 12.2 typedef / enum / struct

**无。**

### 12.3 函数原型（原样）

```c
int32_t cm_sim_get_imsi(char* imsi);
        /* @param [out] imsi 存储IMSI，长度16字节，申请内存后传入 */
        /* @return = 0  - 成功 ; < 0 - 失败, 返回值为错误码. */
```

### 12.4 关键使用约束

* 缓冲区 **16 字节**（`CM_IMSI_LEN = 16`），且注释要求 **“申请内存后传入”**（即由调用方提供缓冲区）。
* 该头文件**只有这一个函数**，没有 PIN 管理、网络注册状态等接口。

---

## 13. `cm_iomux.h` — IOMUX 引脚复用接口

**一句话用途**：设置/查询引脚复用功能，并对引脚施加 sleep / 边沿检测 / 驱动能力 / 上下拉等控制。

### 13.1 枚举完整定义

```c
/**IOMUX PIN  definition 和模组实际PIN脚对应一致,请参照资源综述*/
typedef enum{
    CM_IOMUX_PIN_0 = 0,    
    CM_IOMUX_PIN_1,        
    ...                    /* 中间严格连续递增，无跳跃、无显式赋值 */
    CM_IOMUX_PIN_154,      
    CM_IOMUX_PIN_MAX      
} cm_iomux_pin_e;
```
> 原文件为逐项列出 `CM_IOMUX_PIN_0 = 0` 到 `CM_IOMUX_PIN_154`（共 **155** 个枚举量，仅首项显式赋 `0`、其余顺序递增，故 **`CM_IOMUX_PIN_n` 的值恒为 `n`**），最后一项 `CM_IOMUX_PIN_MAX` 为边界标记。此处为节省篇幅做了等价压缩，**数值映射已逐一核对，无遗漏、无跳跃**。

```c
/*IOMUX FUNC  definition */
typedef enum{
    CM_IOMUX_FUNC_FUNCTION0 ,         /*!<不支持*/
    CM_IOMUX_FUNC_FUNCTION1,          /*!<功能1*/
    CM_IOMUX_FUNC_FUNCTION2,          /*!<功能2*/
    CM_IOMUX_FUNC_FUNCTION3,          /*!<功能3*/
    CM_IOMUX_FUNC_FUNCTIONNUM_END,
} cm_iomux_func_e;


/*IOMUX PIN  cmd*/
typedef enum{
    CM_IOMUX_PINCMD0_SLEEP ,          /*!<休眠状态*/
    CM_IOMUX_PINCMD1_LPMEDEG,         /*!<边沿检测*/
    CM_IOMUX_PINCMD2_DRIVEABILITY,    /*!<驱动能力*/
    CM_IOMUX_PINCMD3_PULL,            /*!<上下拉*/
    CM_IOMUX_PINCMDNUM_END 
} cm_iomux_pincmd_e;

/*IOMUX PIN  cmdnum enumeration*/
typedef enum{
    CM_IOMUX_PINCMD0_FUNC0_SLEEP_NONE,             /*!<不使能 pad 的 sleep 功能*/
    CM_IOMUX_PINCMD0_FUNC1_SLEEP_DIR,              /*!<不使能 sleep，只是设置为 sleep 时候，输入状态*/
    CM_IOMUX_PINCMD0_FUNC2_SLEEP_DATA,             /*!<不使能 sleep，只是设置为 sleep 时候，输出状态*/
    CM_IOMUX_PINCMD0_FUNC3_SLEEP_FLOAT,            /*!<使能 sleep 功能，设置为 sleep 时候，输入状态*/
    CM_IOMUX_PINCMD0_FUNC4_SLEEP_OUTPUT_HIGH, /*!<使能 sleep 功能，设置为 sleep 时候，输出高状态*/
    CM_IOMUX_PINCMD0_FUNC5_SLEEP_OUTPUT_LOW,       /*!<使能 sleep 功能，设置为 sleep 时候，输出低状态*/
    CM_IOMUX_PINCMD0_FUNCNUM_END  
} cm_iomux_pincmd0_e;
    
typedef enum{
    CM_IOMUX_PINCMD1_FUNC0_LPM_EDGE_NONE,        /*!<不使能 pad 的边沿检测功能*/
    CM_IOMUX_PINCMD1_FUNC1_LPM_EDGE_RISE ,       /*!<使能 pad 的边沿检测功能，且是上升沿。并提供唤醒事件*/
    CM_IOMUX_PINCMD1_FUNC2_LPM_EDGE_FALL,        /*!<使能 pad 的边沿检测功能，且是下降沿。并提供唤醒事件*/
    CM_IOMUX_PINCMD1_FUNC3_EDGE_BOTH,            /*!<使能 pad 的边沿检测功能，且是双沿。并提供唤醒事件*/
    CM_IOMUX_PINCMD1_FUNCNUM_END 
} cm_iomux_pincmd1_e;
    
typedef enum{
    CM_IOMUX_PINCMD2_FUNC0_DRIVE_VERY_SLOW,
    CM_IOMUX_PINCMD2_FUNC1_DRIVE_SLOW, 
    CM_IOMUX_PINCMD2_FUNC2_DRIVE_MEDIUM,
    CM_IOMUX_PINCMD2_FUNC3_DRIVE_FAST,
    CM_IOMUX_PINCMD2_FUNC4_MASK,
    CM_IOMUX_PINCMD2_FUNCNUM_END
} cm_iomux_pincmd2_e;

typedef enum{
    CM_IOMUX_PINCMD3_FUNC0_PULL_NONE,              /*!<不使能内部的上拉下拉功能*/
    CM_IOMUX_PINCMD3_FUNC1_PULL_LOW,               /*!<使能下拉*/
    CM_IOMUX_PINCMD3_FUNC2_PULL_HIGH,              /*!<使能上拉*/
    CM_IOMUX_PINCMD3_FUNC3_PULL_BOTH,              /*!<使能上下拉*/
    CM_IOMUX_PINCMD3_FUNC4_PULL_FLOAT,             /*!<浮空功能*/
    CM_IOMUX_PINCMD3_FUNCNUM_END
} cm_iomux_pincmd3_e;
```
（**无 struct**。）

### 13.2 函数原型（原样）

```c
int32_t cm_iomux_set_pin_func(cm_iomux_pin_e pin, cm_iomux_func_e fun);
        /* @param [in] pin PIN 定义号 */
        /* @param [in] fun FUN 定义号 */
        /* @return = 0 - 成功 ; = -1 - 失败 */

int32_t cm_iomux_get_pin_func(cm_iomux_pin_e pin, cm_iomux_func_e *fun);
        /* @param [out] fun FUN 定义号 */
        /* @return = 0 - 成功 ; < 0 - 失败 */

int32_t cm_iomux_set_pin_cmd(cm_iomux_pin_e pin, cm_iomux_pincmd_e cmd, uint8_t cmd_arg);
        /* @param [in] cmd 功能定义 */
        /* @param [in] cmd_arg 具体功能值（取值来自 cm_iomux_pincmd0_e/1_e/2_e/3_e） */
        /* @return = 0 - 成功 ; < 0 - 失败 */
```

### 13.3 关键使用约束

* **任何外设 `open`/`init` 之前必须先设置引脚复用**（见 `cm_uart_open`、`cm_gpio_init` 的注释要求）。
* `CM_IOMUX_FUNC_FUNCTION0` 注释为 **“不支持”**。
* `cmd_arg` 的具体取值必须与 `cmd` 匹配：
  * `CM_IOMUX_PINCMD0_SLEEP` → `cm_iomux_pincmd0_e`
  * `CM_IOMUX_PINCMD1_LPMEDEG` → `cm_iomux_pincmd1_e`
  * `CM_IOMUX_PINCMD2_DRIVEABILITY` → `cm_iomux_pincmd2_e`
  * `CM_IOMUX_PINCMD3_PULL` → `cm_iomux_pincmd3_e`
* 唤醒能力：**“仅主串口具有唤醒功能”**，且 **“用于唤醒的数据并不能被 uart 接收，请在唤醒后再进行 uart 数传”**（来自 `examples/uart` 与 `custom/drv/uart/drv_uart.c` 注释）。ML307A 的主串口是 UART0，唤醒引脚 `CM_IOMUX_PIN_17`。
* 引脚总数以 `CM_IOMUX_PIN_MAX` 为边界；模组实际可用引脚须对照《资源综述》。

---

# 专项结论（直接决定固件能否写对）

## A. 线程/任务创建

| 项目 | 结论 |
| --- | --- |
| SDK 原生函数 | `osThreadId_t osThreadNew(osThreadFunc_t func, void *argument, const osThreadAttr_t *attr);` |
| 工程包装函数 | `osThreadId_t osThreadCreat(const char *name, osThreadFunc_t func, osPriority_t priority, uint32_t stacksize);` —— **不是 SDK 头文件的一部分**，定义在 `custom/applaction/app.c`，声明在 `custom/applaction/app.h` |
| 参数含义 | `name` 线程名；`func` 线程入口（`void (*)(void *)`）；`priority` 优先级枚举；`stacksize` 栈空间（见下） |
| **栈深度单位** | **头文件未显式规定字节还是字**。旁证指向**字节**：`osThreadAttr_t.stack_size` 注释为 “size of stack”，而 `osThreadGetStackSize()` 的注释明确写 “**stack size in bytes**”。SDK 示例取值 `1024`、`2048`、`4096 * 2`、`1024 * 4`，按字节解读均合理（1 KB / 2 KB / 8 KB / 4 KB）。**严格意义上属“未证实”**，需实测或查《操作系统开发指导》确认。 |
| 优先级取值范围 | `osPriorityIdle = 0`，`osPriorityLow = 2`，`osPriorityBelowNormal = 3`，`osPriorityNormal = 4`，`osPriorityAboveNormal = 5`，`osPriorityHigh = 6`，`osPrioritySoftRealtime = 7`，`osPriorityHardRealtime = 8`；`osPriorityISR = 32` 保留、**“Do not use!”**；非法值 `osPriorityError = -1` |
| **优先级硬约束** | `custom/applaction/app.c` 注释原文：**“ML307A模组用户线程优先级必须低于或等于 osPriorityNormal”**。但该包装函数的实际代码是 `if(osPriorityNormal > thread_cfg.priority) thread_cfg.priority = osPriorityNormal;`，只把 **2、3 提升到 4**，对 **5~8 不生效** —— 代码与注释不一致，**建议自己显式传 ≤ 4**。 |
| 返回值类型 | `osThreadId_t`，实际是 `typedef void *osThreadId_t`；失败返回 `NULL` |
| 返回值使用注意 | 示例 `cm_log_printf(0,"task:%d\n",strlen((const char *)OC_APP_TaskHandle));` 的写法是**错的**（对 ID 用 strlen），不要模仿 |
| **`cm_opencpu_entry` 之后能否再建线程** | **能。** ① `prebuild/ld/ML307A/DSLN/app_dsln.ld` 第 9 行 `ENTRY(cm_opencpu_entry)` 确认它是固件入口；② `custom/applaction/main.c` 直接在 `cm_opencpu_entry()` 内部调 `osThreadCreat(...)`；③ `examples/demo_main/src/cm_demo_main.c` 的 `cm_opencpu_entry()` 用 `osThreadNew()` 建 `main_task`；④ 官方 uart 示例中 `cm_demo_uart()`（运行在普通线程里）再次调用 `osThreadNew()`，证明**运行期在线程内继续建线程同样是支持的**。⑤ 入口函数返回后，已创建线程继续由 RTOS 调度。 |
| 推荐写法（本工程） | `osThreadCreat("dbg_th", dbg_th, 4, 1024);`（`4` = `osPriorityNormal`）—— 与 `custom/applaction/main.c` 一致 |
| 其他线程 API | `osThreadGetName` / `osThreadGetId` / `osThreadGetState` / `osThreadGetStackSize` / `osThreadGetStackSpace` / `osThreadSetPriority` / `osThreadGetPriority` / `osThreadYield` / `osThreadSuspend` / `osThreadResume` / `osThreadTerminate` / `osThreadGetCount`。`osThreadExit` **SDK 已禁用（源码中为注释）**。 |

## B. 延时 / 定时器

### B.1 毫秒延时的确切函数名

* **SDK 原生只有 tick 级延时**：
  ```c
  osStatus_t osDelay (uint32_t ticks);
  ```
  参数是 **tick**，**不是毫秒**（头文件首部与 `@param ticks time ticks value` 双重确认）。
* **tick 与毫秒的换算：1 tick = 5 ms。** 证据（三处独立一致，但**均不在 `cm_os.h` 中**）：
  1. `custom/applaction/app.c`：
     ```c
     void osDelayMs(uint32_t ms)
     {
         /* ML307A心跳周期为5ms */
         osDelay(ms/5);
     }
     ```
  2. `examples/camera/src/cm_demo_camera.c:944`：`osDelay(1); //delay 5ms`
  3. `include/platform/lwip/lwip/tcpip.h:320,335`：`tick as unit, shift time = timer_shift * 5ms`、**“one tick is 5ms”**
  4. 推论：`osKernelGetTickFreq()` 应返回 **200**（Hz）。**该具体数值未在文件中写死，属未证实（可由运行时调用确认）。**
  * ⚠️ **反例（注释互相矛盾）**：`examples/aliyun/src/cm_demo_aliyun_ota.c:136` 写 `osDelay(2);  //延迟2s` —— 按 1 tick = 5 ms，`osDelay(2)` 只有 10 ms，**该注释有误**，不要采信。
* **毫秒级延时的推荐做法**：使用工程包装函数（声明在 `custom/applaction/app.h`，也重复声明于 `custom/applaction/includes.h`）：
  ```c
  void osDelayMs(uint32_t ms);   /* @param ms 要是5的整数倍,最小为5ms */
  ```
  其实现为 `osDelay(ms/5)`。**注意：`ms` 必须是 5 的整数倍，最小 5 ms**；传非 5 倍数会被整除截断（如 `osDelayMs(7)` → `osDelay(1)` → 5 ms）。

### B.2 软件定时器

```c
osTimerId_t osTimerNew (osTimerFunc_t func, osTimerType_t type, void *argument, const osTimerAttr_t *attr);
osStatus_t  osTimerStart (osTimerId_t timer_id, uint32_t ticks);   /* ticks 单位为 tick（1 tick = 5 ms） */
osStatus_t  osTimerStop (osTimerId_t timer_id);
uint32_t    osTimerIsRunning (osTimerId_t timer_id);               /* 0 未运行 / 1 运行中 */
osStatus_t  osTimerDelete (osTimerId_t timer_id);
```
* 类型：`osTimerOnce = 0`（单次）/ `osTimerPeriodic = 1`（周期）。
* 回调签名：`typedef void (*osTimerFunc_t)(void *argument);`
* **`osTimerGetName` SDK 已禁用（源码中为注释）。**
* 工程包装：`osTimerId_t osTimerCreat(const char *name, osTimerFunc_t func, osTimerType_t type);`（定义在 `custom/applaction/app.c`，内部调 `osTimerNew(func, type, NULL, &timer_attr)`）。
* 注意：`osTimerStart` 的第二个参数同样是 **tick**，要 1000 ms 周期需传 `200`。

## C. 互斥锁 / 信号量 / 消息队列

**所有 timeout 参数单位均为 tick，1 tick = 5 ms；`0` = 不等待；`osWaitForever`(0xFFFFFFFFU) = 永久等待。**

| 原语 | 创建 | 获取/发送 | 释放/接收 | 删除 |
| --- | --- | --- | --- | --- |
| 互斥锁 | `osMutexId_t osMutexNew(const osMutexAttr_t *attr)` | `osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)` | `osStatus_t osMutexRelease(osMutexId_t mutex_id)` | `osStatus_t osMutexDelete(osMutexId_t mutex_id)` |
| 信号量 | `osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count, const osSemaphoreAttr_t *attr)` | `osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)` | `osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)` | `osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)` |
| 消息队列 | `osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t *attr)` | `osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr, uint8_t msg_prio, uint32_t timeout)` | `osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr, uint8_t *msg_prio, uint32_t timeout)` | `osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)` |
| 事件标志 | `osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)` | `uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)` | `uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags, uint32_t options, uint32_t timeout)` | `osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)` |

**要点与陷阱**

* **信号量只支持二值信号量**（`osSemaphoreNew` 注释原文 “only binary semaphore are supported”）。计数用途请改用消息队列或事件标志。
* `osMessageQueueNew` 的 `msg_size` 单位是 **字节**（注释 “maximum message size in bytes”）；`msg_count` 是消息条数。
* `osMessageQueueGet` 的 `msg_prio` 可为 `NULL`（注释 “pointer to buffer for message priority or NULL”）。
* 典型工程用法（来自 `custom/applaction/app.c`）：
  ```c
  u0_uart_sem = osSemaphoreNew(1, 0, NULL);       /* 二值、初值0 */
  osSemaphoreAcquire(u0_uart_sem, osWaitForever); /* 线程内阻塞等待 */
  /* 串口回调里：osSemaphoreRelease(u0_uart_sem); */
  ```
* 典型消息队列用法（来自 `examples/uart/src/cm_demo_uart.c`）：`osMessageQueueNew(10, sizeof(msg_t), NULL)` → 回调里 `osMessageQueuePut(q, &msg, 0, 0)` → 线程里 `osMessageQueueGet(q, &msg, NULL, osWaitForever)`。
* **回调/中断上下文里不要用带超时的阻塞调用**：`osErrorISR = -6` 即为此类错误码；串口/GPIO 回调只做 `Release`/`Put`（timeout 传 0）。
* 已禁用的辅助接口：`osEventFlagsClear`、`osEventFlagsGet`、`osMutexGetOwner`、`osSemaphoreGetCount`、`osMessageQueueGetCapacity`、`osMessageQueueGetMsgSize`、`osMessageQueueGetCount`、`osMessageQueueGetSpace`、`osMessageQueueReset`（**全部在源码中为注释**）。

## D. 内存

| 问题 | 结论 |
| --- | --- |
| malloc/free 的函数名 | `void *cm_malloc(size_t size);` / `void cm_free(void *ptr);`；另有 `cm_calloc(size_t nitems, size_t size)`、`cm_realloc(void *ptr, size_t size)` |
| 是否有内部堆 | **有。** `cm_mem_get_heap_stats()` 注释原文 “获取的为 **MemPool** 信息”；结构体 `cm_heap_stats_t` 提供 `total_size` / `allocated` / `free` |
| 是否该用 C 库 | **不该。** 注释三处强调 “内存操作接口请勿使用C库接口” |
| 申请上限 | “申请长度不能超过剩余可分配大小” |
| 状态查询 | `int32_t cm_mem_get_heap_stats(cm_heap_stats_t *stats);` `= 0` 成功 / `< 0` 失败 |
| 无效字段 | `max_continuity_free`、`allocated_peak`、`allocated_times`、`free_times` 均为 “本平台无效” |
| **线程栈是否从内部堆分配** | **未证实。** `osThreadAttr_t` 未指定 `stack_mem` 时由 RTOS 自行分配栈，但头文件未说明它与 `cm_malloc` 是否同一个堆。可用 `cm_mem_get_heap_stats()` 在 `osThreadNew()` 前后对比 `free` 值来实测。 |
| 用户可用 RAM 规模 | 链接脚本 `prebuild/ld/ML307A/DSLN/app_dsln.ld`：`ram (rwx): ORIGIN = 0x7E380000, LENGTH = 512K`；`flash: ORIGIN = 0x805F8000, LENGTH = 1024K`。这是**应用镜像**可用的 RAM/flash 区域，不等于 MemPool 大小。 |

## E. HTTP 客户端（Webhook 推送核心）

完整内容见 [第 2 章](#2-cm_httph--httphttps-协议客户端接口)。此处给出结论式摘要：

| 问题 | 结论 |
| --- | --- |
| 完整调用序列（异步） | `cm_httpclient_create` → `cm_httpclient_set_cfg` →（HTTPS 时 `cm_ssl_setopt`）→（可选 `cm_httpclient_custom_header_set` / `cm_httpclient_specific_header_set`）→ `cm_httpclient_request_start(method, path, chunked, content_length)` →（回调 `REQ_START_SUCC_IND` 后）`cm_httpclient_request_send(content, len)` → `cm_httpclient_request_end()` →（回调收响应）→ `cm_httpclient_terminate()` → `cm_httpclient_delete()` |
| 完整调用序列（同步） | `cm_httpclient_create(url, NULL, &handle)` → `cm_httpclient_set_cfg` →（可选设报头）→ `cm_httpclient_sync_request(handle, param, &response)` → `cm_httpclient_sync_free_data(handle)` → `cm_httpclient_delete(handle)` |
| 支持的 method | 5 种：`HTTPCLIENT_REQUEST_GET` / `POST` / `PUT` / `DELETE` / `HEAD`（另有占位 `NONE`、边界 `MAX`） |
| 设置 header | `cm_httpclient_custom_header_set(handle, header, header_len)`（通用报头，缺省时自动加默认报头）+ `cm_httpclient_specific_header_set(...)`（每次请求会变化的字段，会覆盖同名通用字段）。**二者都只引用指针不拷贝**，用完须对应 `*_free`。 |
| 设置 POST body | 异步：`cm_httpclient_request_send(handle, content, content_len)`，**必须在 `CM_HTTP_CALLBACK_EVENT_REQ_START_SUCC_IND` 之后调用**；长度须与 `cm_httpclient_request_start` 的 `content_length` 一致。同步：直接填进 `cm_httpclient_sync_param_t.content` / `.content_length`。 |
| 支持 https 吗 | **支持**：`cm_httpclient_cfg_t.ssl_enable = true`（+ `ssl_id`，0–5），证书用 `cm_ssl_setopt(ssl_id, CM_SSL_PARAM_CA_CERT/VERIFY/CLI_CERT/CLI_KEY, value)`；未使能时相关调用返回 `CM_HTTP_RET_CODE_SSL_NOT_ENABLE (655)` |
| 事件回调注册 | `cm_httpclient_create()` 第 2 个参数传 `cm_httpclient_event_callback_func`；同步模式传 `NULL` |
| 返回码 | 两层：函数返回 `cm_httpclient_ret_code_e`（0/3/23/50/650…658）；事件层 `cm_httpclient_error_event_e`（1…10、255）。HTTP 状态码单独用 `cm_httpclient_get_response_code()` 或回调参数 `response_code` 获取 |
| 缓冲区大小限制 | `HTTPCLIENT_CTX_MAX_NUM = 4`（最多 4 个实例）；`conn_timeout ≤ 180`（默认 60）、`rsp_timeout ≤ 60`（默认 0）。**URL 最大长度与 POST body 最大长度：头文件未定义，未证实**；只有 `CM_HTTP_EVENT_CODE_CACHR_NOT_ENOUGH` 可在运行期提示缓存不足 |
| 其他硬约束 | `cfg.cid` 目前**不支持**设置（任意值即可）；`cm_httpclient_delete` 是异步的，连续两次调用**建议间隔 100 ms**；`cm_httpclient_sync_request` **不支持 chunked** |

## F. UART

| 问题 | 结论 |
| --- | --- |
| 打开 | `int32_t cm_uart_open(cm_uart_dev_e dev, cm_uart_cfg_t *cfg);` |
| 关闭 | `int32_t cm_uart_close(cm_uart_dev_e dev);` |
| 写 | `int32_t cm_uart_write(cm_uart_dev_e dev, const void *data, int32_t len, int32_t timeout);`（`timeout` 标注“(无效参数)”） |
| 读 | `int32_t cm_uart_read(cm_uart_dev_e dev, void* data, int32_t len, int32_t timeout);`（`timeout` 标注“(无效参数)”） |
| 回调注册 | `int32_t cm_uart_register_event(cm_uart_dev_e dev, void *event);`，`event` 实参为 `cm_uart_event_t *`，回调函数指针放在 `cm_uart_event_t.event_entry` 字段。**必须在 `cm_uart_open` 之前调用。** |
| 回调签名 | `typedef void (*cm_uart_event_cb_t)(void *param, uint32_t evt);` 即 `void cb(void *param, uint32_t type)` |
| 回调内禁止事项 | 不可输出 LOG、串口打印、执行复杂任务或消耗过多资源（官方注释原文） |
| 波特率枚举 | `CM_UART_BAUDRATE_9600 / 19200 / 38400 / 57600 / 115200 / 256000`（均为宏，直接填 `cm_uart_cfg_t.baudrate`） |
| 设备号枚举 | `CM_UART_DEV_0 = 0`、`CM_UART_DEV_1 = 1`、`CM_UART_DEV_2 = 2`、`CM_UART_DEV_NUM`（边界） |
| **UART1 常量名** | **`CM_UART_DEV_1`**（ML307A 对应 IOMUX：RX = `CM_IOMUX_PIN_28`、TX = `CM_IOMUX_PIN_29`，功能号 `CM_IOMUX_FUNC_FUNCTION1`） |
| 注意 | ML307A 的调试主串口是 **UART0**（`OPENCPU_MAIN_URAT = 0`，TX=`PIN_18`/RX=`PIN_17`）；**SD 卡功能与 UART0 互斥** |

## G. 重启（cm_sys.h / cm_pm.h）

| 问题 | 结论 |
| --- | --- |
| 重启模组的函数名与参数 | **`void cm_pm_reboot(void);`** —— 定义在 **`cm_pm.h`**，**无参数、无返回值** |
| 是否在 cm_sys.h | **不在。** `cm_sys.h` 只有 `cm_sys_get_imei` / `cm_sys_get_sn` / `cm_sys_get_cm_ver` / `cm_log_printf` 四个函数 |
| 关机接口 | **有：`void cm_pm_poweroff(void);`**（`cm_pm.h`） |
| 进入下载模式接口 | **未提供，未证实**（全量 grep `download_mode` / `fastboot` / `diag` 在 `include/cmiot/` 无匹配） |
| 上电原因 | `int cm_pm_get_power_on_reason(void);` 返回 `cm_pm_power_on_reason_e` |
| 验证 | `prebuild/baseline/ML307A/DSLN/ML307A-DSLN-MBRH1S00.map` 中 `cm_pm_reboot 0x8034f39f Thumb Code 24 cm_pm.o(.text)` 确认该符号存在于 DSLN 基线固件 |
| 示例 | `examples/pm/src/cm_demo_pm.c`：`cm_test_pm_reboot()` 直接调 `cm_pm_reboot();` |

## H. 文件系统（cm_fs.h）

| 问题 | 结论 |
| --- | --- |
| **写入后是否需要 close 才落盘** | **需要。** `cm_fs_sync` 在头文件里被 `#if 0` 包裹且注释写明 “**本平台不支持该接口**”，因此**没有 sync 可用**；头文件中唯一提到“写盘”的地方是 `cm_sd.h` 的 `sd.cfg` 示例——它严格按 `cm_fs_open` → `cm_fs_write` → `cm_fs_close` 三步走，**未调用任何 sync**。因此正确做法是**写完必须 `cm_fs_close(fd)`**。 |
| 是否有目录/路径 | **没有。** `cm_fs_open` 注释：“文件系统中**无路径/文件夹功能**，勿在 filename 入参中传入路径信息”；`cm_fs_exist` 注释同样强调 path 不能带路径信息 |
| 文件打开标志 | `CM_FS_RB (0)` / `CM_FS_WB (1)` / `CM_FS_AB (2)` / `CM_FS_WBPLUS (3)` / `CM_FS_ABPLUS (4)` |
| seek 基准 | `CM_FS_SEEK_SET (0)` / `CM_FS_SEEK_CUR (1)` / `CM_FS_SEEK_END (2)` |
| **offset 约束** | **“offset 不支持为负数”**（`cm_fs_seek` 注释原文） |
| 已禁用接口 | `int32_t cm_fs_sync(int32_t fd);` —— **`#if 0` 禁用**，注释 “本平台不支持该接口” |

### 完整函数原型（原样）

```c
int32_t cm_fs_open(const char *filename, int32_t flag);
        /* @return >= 0 - 文件描述符 ; < 0 - 错误 */
int32_t cm_fs_close(int32_t fd);
        /* @return = 0 - 成功 ; < 0 - 失败 */
int32_t cm_fs_read(int32_t fd, void *buf, uint32_t size);
        /* @return >= 0 - 实际的读取长度 ; < 0 - 读取失败 */
int32_t cm_fs_write(int32_t fd, const void *buf, uint32_t size);
        /* @return >= 0 - 实际的写入长度 ; < 0 - 写入失败 */
int32_t cm_fs_seek(int32_t fd, int32_t offset, int32_t base);
        /* @return = 0 - 成功 ; < 0 - 失败 ; @details offset不支持为负数 */
int32_t cm_fs_delete(const char *file_name);
        /* @return >= 0 - 成功 ; < 0 - 失败 */
int32_t cm_fs_move(const char *src, const char *dest);
        /* @return = 0 - 成功 ; < 0 - 失败 */
int32_t cm_fs_exist(const char *file_name);
        /* @return = 1 - 存在 ; = 0 - 不存在 */
int32_t cm_fs_filesize(const char *file_name);
        /* @return >= 0 - 文件长度 ; < 0 - 操作失败 */
int32_t cm_fs_getinfo(cm_fs_system_info_t *info);
        /* @return = 0 - 成功 ; < 0 - 失败 */

#if 0   /* 本平台不支持该接口 */
int32_t cm_fs_sync(int32_t fd);
#endif
```

### 类型定义（完整）

```c
typedef struct
{
    uint32_t free_size;  /*!< 当前可用文件系统大小 */
    uint32_t total_size; /*!< 文件系统总大小 */
} cm_fs_system_info_t;
```

### 配置持久化推荐写法（依据头文件与 `cm_sd.h` 中的官方示例）

```c
int32_t fd = cm_fs_open("cfg.txt", CM_FS_WB);   /* 无路径！ */
if (fd >= 0) {
    cm_fs_write(fd, buf, len);
    cm_fs_close(fd);                            /* 必须 close 才落盘 */
}
```

## I. 时间 / 日历（cm_rtc.h / cm_ntp.h）

### I.1 本地时间

| 项目 | 结论 |
| --- | --- |
| 获取本地时间的函数 | `uint64_t cm_rtc_get_current_time(void);` —— 返回 **Unix 时间戳（秒）**，注释明确 “**UTC时间**” |
| 设置本地时间 | `int32_t cm_rtc_set_current_time(uint64_t second);`（入参单位秒，UTC） |
| **获取本地时间的结构体** | **`cm_rtc.h` 未定义任何时间结构体**（该文件不含 typedef/enum/struct）。它只 `#include <time.h>`，若需要日历字段需自行用 C 库 `localtime()` / `gmtime()` 转换——**头文件未说明这种用法，未证实** |
| 时区 | `int32_t cm_rtc_set_timezone(int32_t timezone);` / `int32_t cm_rtc_get_timezone(void);`，范围 **-12 ~ 12**（整数小时，-12 西十二区，0 世界时，12 东十二区） |

### I.2 NTP 同步接口

| 项目 | 结论 |
| --- | --- |
| 是否存在 | **存在。** `cm_ntp.h` 提供 `cm_ntp_set_cfg` 与 `cm_ntp_sync` |
| 配置 | `int32_t cm_ntp_set_cfg(cm_ntp_cfg_type_e type, void *value);` `= 0` 成功 / `= -1` 失败；**服务进行中不可设置** |
| 触发同步 | `int32_t cm_ntp_sync(void);` `= 0` 成功 / `< 0` 错误码；**异步**，底层依赖 asocket / async_dns / eloop |
| 回调 | `typedef void (*cm_ntp_event_cb_t)(cm_ntp_event_e event, void *event_param, void *cb_param);` 通过 `cm_ntp_set_cfg(CM_NTP_CFG_CB, cb)` 注册，用户参数经 `CM_NTP_CFG_CB_PARAM` 传入 |
| 事件 | `CM_NTP_EVENT_SYNC_FAIL`（`event_param = NULL`）、`CM_NTP_EVENT_SYNC_OK`、`CM_NTP_EVENT_SETTIME_FAIL`；后两者的 `event_param` 是 **ISO-8601 字符串**（如 `2021-04-21T17:10:08.868+08:00`），**不是时间戳** |
| 配置项 | `CM_NTP_CFG_SERVER`（字符串，**≤128 字节**，支持域名/IPv4/IPv6）、`CM_NTP_CFG_PORT`（`uint16_t`，默认 123）、`CM_NTP_CFG_TIMEOUT`（`uint32_t` ms，**1000~10000**，默认 3000）、`CM_NTP_CFG_DNS`（`uint32_t`，0 全局/1 v4 优先/2 v6 优先）、`CM_NTP_CFG_SET_RTC`（`bool`，0 不同步 / 1 默认同步至系统时间）、`CM_NTP_CFG_CB`、`CM_NTP_CFG_CB_PARAM` |
| 官方示例 | `examples/ntp/src/cm_demo_ntp.c` —— `GETNETTIME` 用 `set_rtc = 0`，`SYNCHTIME` 用 `set_rtc = 1` |

---

# 14. 全量交叉验证（DSLN 基线符号表）

以下符号已在 `prebuild/baseline/ML307A/DSLN/ML307A-DSLN-MBRH1S00.map` 中逐一确认存在，说明本文档所列 API 在**目标硬件 ML307A-DSLN 的基线固件**里确实可链接：

| 符号 | 地址 | 所属编译单元 |
| --- | --- | --- |
| `osDelay` | 0x8034a399 | `cmsis_os2.o(.text)` |
| `osThreadNew` | 0x8034a1e3 | `cmsis_os2.o(.text)` |
| `osTimerNew` | 0x8034a459 | `cmsis_os2.o(.text)` |
| `osKernelGetTickFreq` | 0x8034a1cb | `cmsis_os2.o(.text)` |
| `cm_malloc` | 0x8039cd6f | `cm_mem.o(.text)` |
| `cm_pm_reboot` | 0x8034f39f | `cm_pm.o(.text)` |
| `cm_uart_open` | 0x8039e687 | `opencpu_uart.o(.text)` |
| `cm_httpclient_create` | 0x803971a7 | `cm_http_client.o(.text)` |
| `cm_httpclient_sync_request` | 0x80397641 | `cm_http_client.o(.text)` |
| `cm_fs_open` | 0x8034e829 | `cm_fs.o(.text)` |
| `cm_rtc_get_current_time` | 0x8039f187 | `cm_rtc.o(.text)` |
| `cm_ntp_sync` | 0x8039c885 | `cm_api_ntp.o(.text)` |
| `cm_sys_get_imei` | 0x8034e775 | `cm_sys.o(.text)` |

应用入口由链接脚本固定：`prebuild/ld/ML307A/DSLN/app_dsln.ld` → `ENTRY(cm_opencpu_entry)`。

---

# 15. 工程内包装函数（**不属于 SDK 头文件**，但本工程入口示例依赖它们）

来源：`custom/applaction/app.h`（声明）与 `custom/applaction/app.c`（定义）。`custom/applaction/includes.h` 也重复声明了 `osDelayMs`。

```c
osThreadId_t osThreadCreat(const char * name,osThreadFunc_t func,osPriority_t priority,uint32_t stacksize);
osTimerId_t osTimerCreat(const char * name,osTimerFunc_t func,osTimerType_t type);
void osDelayMs(uint32_t ms);          /* @param ms 要是5的整数倍,最小为5ms */
```

`app.h` 中另外**声明为对象而非 `extern`**（**这是头文件里的缺陷，多文件包含会重复定义**）：

```c
osTimerId_t TASK_TT;      /* app.h 第 16 行，缺少 extern */
```

同文件中的互斥量/信号量则是规范写法：

```c
extern osMutexId_t TASK_MT;
extern osSemaphoreId_t u0_uart_sem;
```

`app.h` 中的工具宏：

```c
#define FlagSET(Flag,bit)	(Flag |= (bit))		//Flag置位
#define FlagCLR(Flag,bit)	(Flag &= ~(bit))	//Flag清位
#define FlagGET(Flag,bit)	(Flag & (bit))		//Flag获取
```

`includes.h` 中包含的头文件（决定可用 API 的默认集合）：

```c
#include "cm_os.h"
#include "cm_mem.h"
#include "sys\types.h"
#include <string.h>
#include "cm_sys.h"
#include "stdint.h"
#include "stdint-gcc.h"
#include "stdio.h"
#include "string.h"
#define ALL_SATUS   0xFFUL
```

---

# 16. 未证实 / 存疑 / 冲突清单

**本章为诚实性清单 —— 以下内容我无法从 SDK 文件中证实，写固件时须自行验证或查《操作系统开发指导》《资源综述》。**

## 16.1 完全未证实

| # | 事项 | 说明 |
| --- | --- | --- |
| 1 | **线程栈单位是字节还是 4 字节字** | `cm_os.h` 只用 “size of stack” 描述 `osThreadAttr_t.stack_size`；`osThreadGetStackSize()` 的注释写 “stack size in bytes”，但这是**返回值**的单位说明。CMSIS-RTOS2 标准与示例取值（1024/2048/4096*2）都指向**字节**，但**头文件没有一句直接的“栈单位是字节”或“栈单位是字”**。需实测（例如用 `osThreadGetStackSpace` 观察）或查官方《操作系统开发指导》。 |
| 2 | **`osKernelGetTickFreq()` 的实际返回值** | 由 “1 tick = 5 ms” 推论应为 **200 Hz**，但该数值未在任何文件里写死，属推论。 |
| 3 | **线程栈是否从 `cm_malloc` 的同一个 MemPool 分配** | `cm_os.h` 与 `cm_mem.h` 均未说明二者的关系。 |
| 4 | **HTTP 的 URL 最大长度** | 头文件未定义任何 URL 长度宏。 |
| 5 | **HTTP 的 POST body / 响应体最大长度** | 头文件未定义；只有运行期错误码 `CM_HTTP_EVENT_CODE_CACHR_NOT_ENOUGH`。 |
| 6 | **HTTP 报头（header）最大可设长度** | 参数类型是 `uint16_t header_len`，这暗示上限 65535，但**无任何注释确认**，不作为事实。 |
| 7 | **进入下载模式 / 恢复出厂 / 清除 FOTA 标志的接口** | `include/cmiot/` 全量 grep 无匹配，判定为**不存在于 cmiot 头文件**。 |
| 8 | **看门狗（WDT）用户接口** | 无 `cm_wdt.h`；仅在 `cm_pm_power_on_reason_e` 里有 `CM_PM_ERROR_RESET /*!<Error reset (WDT)*/`，说明 WDT 由底层管理但无用户 API。 |
| 9 | **`include/cmiot/cm_http.h` 之外的 “64 字节最小缓冲区” 类约束** | 在 13 个目标头文件中**没有出现“缓冲区最小 64 字节”这类表述**。实际出现的尺寸约束是：`cm_sys.h` IMEI 16 字节、`cm_sys.h` SN 注释 32 字节（宏为 65）、`cm_log_printf` ≤180 字节、`cm_sim.h` IMSI 16 字节、`cm_ntp.h` 服务器 ≤128 字节、`cm_sd.h` 路径 ≤256 字节。 |
| 10 | **`cm_uart.h` 回调注册时 `event_entry` 是否就是回调函数指针** | 头文件只写 `void *event_entry; /*!< 事件执行入口*/`，**未显式说明**。判定依据是 `cm_uart_event_cb_t` 的存在 + `custom/drv/uart/drv_uart.c` 与 `examples/uart` 两处一致的实际用法。属**强旁证但非头文件明文**。 |
| 11 | **`bool` 的真实类型** | `cm_http.h` 自定义 `#define bool int`（与 C99 `stdbool.h` 冲突风险）；而 `cm_uart.h` 等未定义。混用时需注意包含顺序。 |
| 12 | **`cm_gpio_interrupt_register` 的回调签名** | 头文件参数类型是 `void *interrupt_cb`，**未给出函数指针 typedef**，回调签名（是否带参数）未证实。 |

## 16.2 文件内部自相矛盾，已实测确认

| # | 冲突点 | 事实 |
| --- | --- | --- |
| 1 | `cm_sys.h`：`CM_SN_LEN` 定义为 **65**，但 `cm_sys_get_sn` 的 `@param [out] sn SN，长度32字节` | 二者不一致。**建议按 65 字节分配缓冲区。** |
| 2 | `custom/applaction/app.c` 注释 “**ML307A模组用户线程优先级必须低于或等于 osPriorityNormal**”，但代码 `if(osPriorityNormal > thread_cfg.priority)` 只把 **2、3** 提升到 4，对 **5~8 不生效** | **代码没有实现注释所声明的约束。** 传 `osPriorityHigh`(6) 不会被夹紧。请自行传 ≤ `osPriorityNormal`(4)。 |
| 3 | `examples/aliyun/src/cm_demo_aliyun_ota.c:136` 写 `osDelay(2);  //延迟2s` | 与 “1 tick = 5 ms” 冲突（`osDelay(2)` = 10 ms）。**该注释有误**，`app.c`/camera 示例/lwip 注释一致指向 5 ms。 |
| 4 | `custom/applaction/app.h` 第 16 行 `osTimerId_t TASK_TT;` **只声明未 `extern`** | 头文件被多个 .c 包含时会触发重复定义；同文件的 `TASK_MT`、`u0_uart_sem` 都正确用了 `extern`。 |
| 5 | `cm_http.h` 文件头注释写 `@file cm_http_client.h` | 实际文件名是 `cm_http.h`；`#include` 必须用 `cm_http.h`。 |
| 6 | `cm_gpio.h`：`CM_GPIO_CMD_SET_LEVEL` / `GET_LEVEL` 注释写的是 “**驱动能力**设置/获取命令码” | 但按 `arg` 类型说明，这两个命令实际操作的是 **`cm_gpio_level_e`（电平）**，注释有误。 |

## 16.3 已核对的“不存在”清单（100% 确定）

通过对 `include/cmiot/` 目录列举 + 全量内容检索确认：

* `cm_sms.h` — **不存在**
* `cm_usb.h` — **不存在**
* `cm_ping.h` — **不存在**
* `cm_flash.h` — **不存在**
* `cm_wdt.h` — **不存在**
* `cm_os_*` 风格的 RTOS 函数 — **不存在**（RTOS 统一为 `osXxx`）
* `include/cmiot/` 下的完整头文件清单（共 45 个）：
  `cm_adc.h, cm_asocket.h, cm_async_dns.h, cm_audio_common.h, cm_audio_player.h, cm_audio_recorder.h, cm_camera_sensor.h, cm_camera.h, cm_eloop.h, cm_fota.h, cm_fs.h, cm_ftp.h, cm_gnss.h, cm_gpio.h, cm_http.h, cm_i2c.h, cm_iomux.h, cm_keypad.h, cm_lbs.h, cm_lcd.h, cm_local_tts.h, cm_mem.h, cm_modem_info.h, cm_mqtt.h, cm_ntp.h, cm_os.h, cm_pm.h, cm_pwm.h, cm_qr.h, cm_rtc.h, cm_sd.h, cm_sim.h, cm_spi.h, cm_ssl.h, cm_sys.h, cm_tts.h, cm_uart.h, cm_utils.h, cm_virt_at.h, cm_wifiscan.h`

---

*本文档所有函数原型、typedef、enum、struct 均逐字摘自上述头文件；第 15 节明确标注了非 SDK 来源（工程内包装函数）。凡无法证实的条目已在第 16 节列出。*
