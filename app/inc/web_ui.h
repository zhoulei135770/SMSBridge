/**
 * @file    web_ui.h
 * @brief   模组内置 Web 管理面板 —— 页面渲染接口（纯 C99，无 SDK 依赖，无动态内存）。
 *
 * 约定（与需求文档 5.2 / 5.3 / 5.4 对应）：
 *   - 所有渲染函数形如 size_t f(char *buf, size_t cap, ...)，返回**写入的字节数**
 *     （不含结尾 '\0'）。写入永不越界；容量不足时截断，并保证以 '\0' 结尾。
 *   - web_ui_login_page() / web_ui_status_page() 只产出 **HTML 正文**，
 *     由 web_server 补 "HTTP/1.1 200 OK" + Content-Type + Content-Length 头。
 *   - web_ui_redirect() / web_ui_plain() / web_ui_export_txt() 产出 **完整 HTTP 响应**
 *     （状态行 + 头部 + 正文），可直接整块 write 到 socket。
 *   - 所有来自模组 / 短信 / 配置的动态内容均已做 HTML 转义（& < > " '），
 *     短信号码与正文视为不可信输入。
 *
 * 缓冲区建议：按下方 *_MAX 宏分配（web_server 侧静态/栈上缓冲）。
 */
#ifndef WEB_UI_H
#define WEB_UI_H
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"
#include "net_addr.h"     /* NET_ADDR_* 尺寸 */
#include "sms_rcfg.h"     /* 远程配置快照的字段尺寸 */

/* 状态页需要展示的运行状态快照 */
typedef struct {
    int      online;              /* 1=网络在线 0=离线 */
    char     apn[32];             /* 当前生效 APN */
    int      csq;                 /* 信号 CSQ 0-31，-1 或 99 表示未知 */
    int      rndis_on;            /* USB 流量共享开关当前状态 */
    char     rndis_note[48];      /* 生效方式说明，如 "即时生效" / "重启后生效" */
    char     ip[NET_ADDR_V4_MAX]; /* PDP 的 IPv4 地址，可为空串 */
    char     imei[24];
    char     iccid[24];
    char     fw_ver[24];
    char     uptime[24];          /* 已运行时长，如 "3天 04:15:22" */
    char     now[24];             /* 模组当前时间 */
    uint32_t month_used_kb;       /* 本月已用 KB */
    uint32_t month_limit_mb;      /* 月度限额 MB，0=不限 */
    uint32_t up_bytes;            /* 上行累计（本次会话） */
    uint32_t down_bytes;          /* 下行累计（本次会话） */
    uint32_t session_bytes;       /* 本次会话合计 */
    int      directional_ok;      /* 1=底包支持上下行分方向统计；0=显示为 N/A */
    int      over_limit;          /* 1=已超限，页面顶部红色告警 */
    int      rndis_config;        /* 配置里的开关值（可能与当前状态不同） */

    /* ---- IPv6 与公网地址（新增） ---- */
    char     ip6[NET_ADDR_V6_MAX];    /* PDP 的 IPv6 全局地址，可为空串 */
    char     pub_ip[NET_ADDR_MAX];    /* 回显服务查到的公网 IPv4 出口 */
    char     pub_ip6[NET_ADDR_MAX];   /* 回显服务看到的 IPv6 出口（请求走 v6 时才填） */
    char     pdp_type[8];             /* 当前 PDP 类型：IP / IPV4V6 / IPV6 */
    int      ipv6_enable;             /* 配置里的 IPv6 开关 */
    int      ipv6_on;                 /* 1 = 当前确实拿到 IPv6 地址 */
    int      pubip_tried;             /* 1 = 已尝试查询过公网 IP */
    int      pubip_ok;                /* 1 = 最近一次查询成功 */
    char     pubip_when[24];          /* 公网 IP 的更新时间描述，如 "3 分钟前" */

    /* ---- 短信远程配置只读快照（由短信指令设置，见 docs/09、docs/10） ---- */
    int      rcfg_loaded;             /* 1 = 来自 rcfg.bin，0 = 默认值 */
    char     rcfg_admin[SMS_RCFG_ADMIN_MAX];
    char     rcfg_url[SMS_RCFG_URL_MAX];
    int      rcfg_secret_set;
    char     rcfg_keyword[SMS_RCFG_KEYWORD_MAX];
    char     rcfg_tmpl[SMS_RCFG_TMPL_MAX];
    int      rcfg_concat;
    char     sms_ntp[40];
    uint32_t sms_parts;
    uint32_t sms_reassembled;
    uint32_t sms_admin_cmd;
    uint32_t sms_replies;
} web_status_t;

/* 一条短信的展示行 */
typedef struct {
    char        number[24];
    char        time[24];
    const char *text;             /* UTF-8，可能为 NULL */
} web_sms_row_t;

/**
 * @brief 渲染登录页 HTML 正文（未登录时唯一可访问的页面）。
 * @param buf         输出缓冲，建议 >= WEB_UI_LOGIN_PAGE_MAX
 * @param cap         缓冲容量（字节）
 * @param auth_failed 非 0 时在表单上方显示「口令错误」提示
 * @return 写入字节数（不含 '\0'）
 * @note  表单为 POST /login，字段名 password；服务端应回 302 到 "/"。
 */
size_t web_ui_login_page(char *buf, size_t cap, int auth_failed);

/**
 * @brief 渲染状态 + 配置单页 HTML 正文（展示与提交在同一页完成）。
 * @param buf       输出缓冲，建议 >= WEB_UI_STATUS_PAGE_MAX
 * @param cap       缓冲容量（字节）
 * @param st        运行状态快照，不可为 NULL
 * @param cfg       当前配置，不可为 NULL
 * @param rows      最近短信数组，可为 NULL（等价于 0 条）
 * @param row_count 短信条数
 * @return 写入字节数（不含 '\0'）
 * @note  提交目标：POST /config（字段名见需求 5.4）；
 *        动作按钮：POST /action/apn_next、POST /action/reboot、POST /action/traffic_reset；
 *        链接：GET /export、GET /logout。
 */
size_t web_ui_status_page(char *buf, size_t cap,
                          const web_status_t *st,
                          const app_config_t *cfg,
                          const web_sms_row_t *rows, size_t row_count);

/**
 * @brief 渲染短信日志导出（完整 HTTP 响应，Content-Type: text/plain，附件下载）。
 * @param buf       输出缓冲，建议 >= WEB_UI_EXPORT_MAX
 * @param cap       缓冲容量（字节）
 * @param rows      短信数组，可为 NULL
 * @param row_count 短信条数
 * @return 写入字节数（不含 '\0'）；Content-Length 与实际正文长度严格一致
 */
size_t web_ui_export_txt(char *buf, size_t cap,
                         const web_sms_row_t *rows, size_t row_count);

/**
 * @brief 渲染 302 跳转响应（完整 HTTP 响应，无正文）。
 * @param buf      输出缓冲，建议 >= 256
 * @param cap      缓冲容量（字节）
 * @param location 目标路径；NULL 或空串按 "/" 处理；其中的 CR/LF 会被剔除以防响应头注入
 * @return 写入字节数（不含 '\0'）
 */
size_t web_ui_redirect(char *buf, size_t cap, const char *location);

/**
 * @brief 渲染纯文本 / 错误响应（完整 HTTP 响应，text/plain; charset=utf-8）。
 * @param buf 输出缓冲，建议 >= 512
 * @param cap 缓冲容量（字节）
 * @param msg 正文，可为 NULL（等价空串）
 * @return 写入字节数（不含 '\0'）；Content-Length 与实际正文长度严格一致
 */
size_t web_ui_plain(char *buf, size_t cap, const char *msg);

/* web_server 侧分配缓冲用的尺寸建议值。
 *
 * ⚠ 这两个数值由主机测试实测校准过（app/test/host_tests.c 有回归断言）：
 *   - 登录页含内联 CSS，完整渲染需要约 3.3KB。曾误设为 2048，
 *     结果页面在 <style> 中间被截断、<form> 根本没输出 —— 表现为
 *     "浏览器打开只有样式没有输入框，永远登录不进去"。现取 4096 留余量。
 *   - 状态页在"黑白名单全满 + 16 条最长短信"时约 15KB 量级，取 24576 留余量。
 */
#define WEB_UI_STATUS_PAGE_MAX   24576
#define WEB_UI_LOGIN_PAGE_MAX     4096
#define WEB_UI_EXPORT_MAX        32768
#endif
