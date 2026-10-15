/**
 * @file    web_ui.c
 * @brief   4G 短信网关内置 Web 管理面板 —— 页面渲染（纯 C99，无 SDK 依赖，无动态内存）。
 *
 * 设计要点（对应需求文档 5.2 / 5.3 / 5.4 与「五、非功能需求」）：
 *   1. 所有渲染函数形如 size_t f(char *buf, size_t cap, ...)，返回写入字节数（不含 '\0'）。
 *      任何容量下都不会越界写；空间不足时静默截断，并始终保证以 '\0' 结尾。
 *   2. 内部统一用私有的 put()/putn() 追加文本，不使用 sprintf/snprintf 拼长串；
 *      十进制转换由 put_u32() 自行实现，连数字格式化也不依赖 libc 的 printf。
 *   3. 来自模组 / 短信 / 配置的动态内容一律经 put_escaped_n() 做 HTML 转义
 *      （& < > " ' 五个字符）；短信号码与正文按不可信输入处理。
 *   4. 页面自包含：CSS 内嵌，无任何外部 CDN / 字体 / 图片；JS 仅用 onsubmit 内联
 *      confirm 做二次确认（刻意不引入 <script> 标签，便于注入检测）。
 *   5. 本文件不引用 app_config.c 的任何符号（如 g_apn_table / app_config_apn），
 *      当前 APN 名称取自 web_status_t.apn，便于主机侧单独编译与单元测试。
 */
#include "web_ui.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 私有常量                                                            */
/* ------------------------------------------------------------------ */

/** 短信正文最长扫描长度：避免对不可信的 const char * 做无界遍历 */
#define UI_TEXT_SCAN_MAX   1024u
/** HTTP 响应头最大长度（本模块自用的小块栈缓冲，实测头部 < 190 字节） */
#define UI_HEAD_MAX        224u

/* 各下拉框选项文案：下标即提交值，与 app_config.h 的枚举一一对应 */
static const char *const UI_OPT_WEBHOOK_TYPE[4] = {
    "自动识别", "企业微信机器人", "钉钉机器人", "通用 JSON"
};
static const char *const UI_OPT_LIMIT_ACTION[2] = { "仅告警", "告警并断网" };
static const char *const UI_OPT_ONOFF[2]        = { "关闭", "开启" };
static const char *const UI_OPT_APPLY[2]        = { "重启后生效", "即时生效" };

/* ------------------------------------------------------------------ */
/* 私有助手 1：小 append 机（全部渲染路径的唯一出口）                   */
/* ------------------------------------------------------------------ */

/**
 * @brief 把 n 字节原始内容追加到输出缓冲（核心助手，永不越界）。
 * @param p    指向「当前写指针」的指针，成功后前移
 * @param left 指向「剩余可用空间」的指针，含结尾 '\0' 占位，成功后减小
 * @param s    源数据，可为 NULL（此时按 n=0 处理）
 * @param n    期望写入的字节数
 * @return 0 = 完整写入；-1 = 发生截断（或 *left 已为 0）
 * @note 只要进入时 *left > 0，返回前一定在 *p 处写入 '\0'。
 */
static int putn(char **p, size_t *left, const char *s, size_t n)
{
    size_t avail;
    size_t want = n;

    if (p == NULL || left == NULL || *left == 0u) {
        return -1;
    }
    if (s == NULL) {
        want = 0u;
        n = 0u;
    }
    avail = *left - 1u;          /* 留 1 字节给结尾 '\0' */
    if (n > avail) {
        n = avail;
    }
    if (n > 0u) {
        memcpy(*p, s, n);
        *p += n;
        *left -= n;
    }
    **p = '\0';
    return (n == want) ? 0 : -1;
}

/**
 * @brief 追加一个 '\0' 结尾字符串（私有小 append 助手，见文件头说明）。
 * @param p    当前写指针的地址
 * @param left 剩余空间（含结尾 '\0' 占位）
 * @param s    源字符串，可为 NULL（等价空串）
 * @return 0 = 完整写入；-1 = 截断
 */
static int put(char **p, size_t *left, const char *s)
{
    return putn(p, left, s, (s != NULL) ? strlen(s) : 0u);
}

/**
 * @brief 受限 strlen：最多只看 max 字节，绝不越界读取。
 * @param s   源字符串，可为 NULL
 * @param max 最多扫描的字节数
 * @return 实际长度（<= max）
 */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0u;
    if (s == NULL) {
        return 0u;
    }
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/**
 * @brief 追加无符号十进制整数（不依赖 printf，避免格式化开销与截断告警）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param v    待输出数值
 */
static void put_u32(char **p, size_t *left, uint32_t v)
{
    char tmp[10];
    size_t n = 0u;

    if (v == 0u) {
        put(p, left, "0");
        return;
    }
    while (v > 0u && n < sizeof tmp) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    while (n > 0u) {                       /* tmp 中是逆序数字，反序吐出 */
        char c = tmp[--n];
        putn(p, left, &c, 1u);
    }
}

/**
 * @brief 追加有符号十进制整数（用于 CSQ / dBm）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param v    待输出数值
 */
static void put_i32(char **p, size_t *left, int32_t v)
{
    uint32_t mag;

    if (v < 0) {
        put(p, left, "-");
        mag = (uint32_t)(-(int64_t)v);     /* 借 int64 规避 INT32_MIN 取负溢出 */
    } else {
        mag = (uint32_t)v;
    }
    put_u32(p, left, mag);
}

/**
 * @brief 判断字符是否需要 HTML 转义。
 * @param c 待判断字符
 * @return 1 = 需要转义
 */
static int needs_escape(char c)
{
    return (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') ? 1 : 0;
}

/**
 * @brief HTML 转义追加：& < > " ' 五个字符转成实体，其余原样输出。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param s    源字符串，可为 NULL（等价空串）
 * @param max  最多扫描的字节数（防止对未终止的定长字段越界读取）
 * @return 实际消费的源字节数
 * @note 短信号码、短信正文、配置项等一切外部内容都必须经过本函数。
 */
static size_t put_escaped_n(char **p, size_t *left, const char *s, size_t max)
{
    size_t i = 0u;

    if (s == NULL) {
        return 0u;
    }
    while (i < max && s[i] != '\0') {
        const char *rep = NULL;
        switch (s[i]) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:   rep = NULL;     break;
        }
        if (rep != NULL) {
            put(p, left, rep);
            i++;
        } else {
            size_t j = i;                  /* 连续普通字符一次性拷贝，减少调用 */
            while (j < max && s[j] != '\0' && !needs_escape(s[j])) {
                j++;
            }
            putn(p, left, s + i, j - i);
            i = j;
        }
    }
    return i;
}

/** 转义追加一个定长字段（外部来源的数组字段一律用本宏，避免越界读） */
#define PUT_ESC_FIELD(pp, lf, fld) put_escaped_n((pp), (lf), (fld), sizeof(fld))

/**
 * @brief 追加字节量的人类可读形式（B / KB / MB / GB，保留两位小数）。
 * @param p     当前写指针的地址
 * @param left  剩余空间
 * @param bytes 字节数
 * @note 输入上限约 4TB（本模块只会传入 uint32 KB 或 uint32 字节），整数部分不会溢出。
 */
static void put_bytes(char **p, size_t *left, uint64_t bytes)
{
    if (bytes < 1024u) {
        put_u32(p, left, (uint32_t)bytes);
        put(p, left, " B");
        return;
    }
    if (bytes < 1024u * 1024u) {
        uint64_t i = bytes / 1024u;
        uint32_t f = (uint32_t)((bytes % 1024u) * 100u / 1024u);
        put_u32(p, left, (uint32_t)i);
        put(p, left, ".");
        if (f < 10u) {
            put(p, left, "0");
        }
        put_u32(p, left, f);
        put(p, left, " KB");
        return;
    }
    if (bytes < 1024u * 1024u * 1024u) {
        uint64_t i = bytes / (1024u * 1024u);
        uint32_t f = (uint32_t)((bytes % (1024u * 1024u)) * 100u / (1024u * 1024u));
        put_u32(p, left, (uint32_t)i);
        put(p, left, ".");
        if (f < 10u) {
            put(p, left, "0");
        }
        put_u32(p, left, f);
        put(p, left, " MB");
        return;
    }
    {
        uint64_t i = bytes / (1024u * 1024u * 1024u);
        uint32_t f = (uint32_t)((bytes % (1024u * 1024u * 1024u)) * 100u
                                / (1024u * 1024u * 1024u));
        put_u32(p, left, (uint32_t)i);
        put(p, left, ".");
        if (f < 10u) {
            put(p, left, "0");
        }
        put_u32(p, left, f);
        put(p, left, " GB");
    }
}

/**
 * @brief 追加纯文本导出用的字段（把 CR/LF/TAB 折成空格，保持导出格式整齐）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param s    源字符串，可为 NULL
 * @param max  最多扫描字节数
 */
static void put_txt_field(char **p, size_t *left, const char *s, size_t max)
{
    size_t i;

    if (s == NULL) {
        return;
    }
    for (i = 0u; i < max && s[i] != '\0'; i++) {
        char c = s[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        putn(p, left, &c, 1u);
    }
}

/**
 * @brief 追加纯文本原文（保留短信正文里的换行），受 max 限长。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param s    源字符串，可为 NULL
 * @param max  最多扫描字节数
 */
static void put_txt_raw(char **p, size_t *left, const char *s, size_t max)
{
    if (s == NULL) {
        return;
    }
    putn(p, left, s, bounded_len(s, max));
}

/**
 * @brief 在「正文已渲染在 buf 起始处」的前提下，把 HTTP 响应头插到正文前面。
 * @param buf       缓冲起始
 * @param cap       缓冲容量（> 0）
 * @param body_len  已写入的正文长度（不含 '\0'）
 * @param ctype     Content-Type 取值
 * @param disp      额外的 Content-Disposition 整行（含 CRLF），可为 NULL
 * @return 最终写入的字节数（含头部）
 * @note 用 memmove 右移正文给头部腾位置；Content-Length 与实际保留的正文长度严格一致，
 *       容量不足时先压缩正文、再退化为「只发头部」，任何情况下都不越界。
 */
static size_t http_ok_with_body(char *buf, size_t cap, size_t body_len,
                                const char *ctype, const char *disp)
{
    char head[UI_HEAD_MAX];
    char *hp;
    size_t hleft;
    size_t hdr_len = 0u;
    size_t body;
    int i;

    if (body_len > cap - 1u) {
        body_len = cap - 1u;
    }
    body = body_len;
    for (i = 0; i < 4; i++) {
        hp = head;
        hleft = sizeof head;
        put(&hp, &hleft, "HTTP/1.1 200 OK\r\nContent-Type: ");
        put(&hp, &hleft, (ctype != NULL) ? ctype : "text/plain; charset=utf-8");
        put(&hp, &hleft, "\r\n");
        if (disp != NULL) {
            put(&hp, &hleft, disp);
        }
        put(&hp, &hleft, "Content-Length: ");
        put_u32(&hp, &hleft, (uint32_t)body);
        put(&hp, &hleft, "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
        hdr_len = (size_t)(hp - head);
        if (hdr_len + body + 1u <= cap) {
            break;                              /* 头部 + 正文都放得下 */
        }
        {
            size_t nb = (cap > hdr_len + 1u) ? (cap - 1u - hdr_len) : 0u;
            if (nb == body) {
                break;                          /* 已收敛（或头部本身放不下） */
            }
            body = nb;
        }
    }
    if (hdr_len > cap - 1u) {                   /* 极端小缓冲：头部也放不下 */
        hdr_len = cap - 1u;
        body = 0u;
    }
    if (body > 0u) {
        memmove(buf + hdr_len, buf, body);
    }
    memcpy(buf, head, hdr_len);
    buf[hdr_len + body] = '\0';
    return hdr_len + body;
}

/* ------------------------------------------------------------------ */
/* 私有助手 2：HTML 片段                                               */
/* ------------------------------------------------------------------ */

/** 内嵌样式表（自包含，不引用任何外部资源；无 <script> 标签） */
static const char UI_CSS[] =
    "*{box-sizing:border-box}"
    "body{margin:0;padding:10px;background:#f2f5f9;color:#1f2933;"
    "font:14px/1.55 -apple-system,'Segoe UI',Roboto,'Microsoft YaHei',sans-serif}"
    ".wrap{max-width:920px;margin:0 auto}"
    "h1{font-size:17px;margin:0}"
    "h2{font-size:15px;margin:0 0 10px;padding-bottom:6px;border-bottom:1px solid #e3e8ef}"
    ".card{background:#fff;border:1px solid #e3e8ef;border-radius:10px;padding:12px;margin:10px 0}"
    ".banner{background:#dc2626;color:#fff;border-radius:10px;padding:10px 12px;"
    "margin:10px 0;font-weight:600}"
    ".top{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px}"
    ".it{background:#f8fafc;border:1px solid #eef2f7;border-radius:8px;padding:8px}"
    ".k{color:#6b7a8d;font-size:12px}"
    ".v{font-weight:600;word-break:break-all}"
    ".ok{color:#16a34a}.bad{color:#dc2626}.warn{color:#d97706}.muted{color:#6b7a8d}"
    ".bar{height:10px;background:#e3e8ef;border-radius:6px;overflow:hidden;margin:8px 0 4px}"
    ".bar i{display:block;height:100%;background:#2563eb}"
    ".bar.over i{background:#dc2626}"
    ".row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}"
    "button,.btn{display:inline-block;border:0;border-radius:8px;padding:9px 14px;font-size:14px;"
    "background:#2563eb;color:#fff;text-decoration:none;cursor:pointer}"
    ".btn.gray{background:#64748b}.btn.red{background:#dc2626}"
    "label{display:block;margin:8px 0 2px;color:#3e4c59;font-size:13px}"
    "input[type=text],input[type=number],input[type=password],select,textarea{"
    "width:100%;padding:8px;border:1px solid #cbd5e1;border-radius:8px;font-size:14px;"
    "background:#fff;color:#1f2933}"
    "textarea{font-family:ui-monospace,Consolas,monospace}"
    ".chk{display:flex;align-items:center;gap:6px;margin:12px 0 2px;font-size:13px}"
    ".chk input{width:auto}"
    "fieldset{border:1px solid #e3e8ef;border-radius:8px;margin:0 0 12px;padding:6px 10px 12px}"
    "legend{color:#2563eb;font-size:13px;padding:0 4px}"
    "table{width:100%;border-collapse:collapse;font-size:13px}"
    "th,td{text-align:left;padding:7px 6px;border-bottom:1px solid #eef2f7;vertical-align:top}"
    "th{color:#6b7a8d;font-weight:600}"
    ".msg{white-space:pre-wrap;word-break:break-word}"
    ".num{white-space:nowrap}"
    ".err{background:#fee2e2;color:#991b1b;border:1px solid #fecaca;border-radius:8px;"
    "padding:8px;margin:0 0 10px}"
    ".note{color:#6b7a8d;font-size:12px;text-align:center;padding:6px 0 18px}";

/**
 * @brief 渲染 HTML 头（DOCTYPE / meta / 内嵌样式 / body 开始）。
 * @param p     当前写指针的地址
 * @param left  剩余空间
 * @param title 标题（内部固定串，无需转义）
 */
static void ui_head(char **p, size_t *left, const char *title)
{
    put(p, left, "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n");
    put(p, left, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
    put(p, left, "<title>");
    put(p, left, title);
    put(p, left, "</title>\n<style>");
    put(p, left, UI_CSS);
    put(p, left, "</style>\n</head>\n<body>\n<div class=\"wrap\">\n");
}

/**
 * @brief 渲染 HTML 尾（安全提示 + 收尾标签）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 */
static void ui_foot(char **p, size_t *left)
{
    put(p, left, "<p class=\"note\">本页面通过 <b>HTTP 明文</b> 传输，仅限 USB RNDIS "
                 "内网使用；同一时间仅支持单用户访问，请勿把本端口暴露到公网。</p>\n");
    put(p, left, "</div>\n</body>\n</html>\n");
}

/**
 * @brief 渲染状态卡片中的一格（值来自内部信任字符串）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param k    标题（内部固定串）
 * @param cls  值额外样式类，可为 NULL
 * @param v    值，必须是字面量或本地缓冲（'\\0' 结尾）
 */
static void ui_item_plain(char **p, size_t *left, const char *k, const char *cls, const char *v)
{
    put(p, left, "<div class=\"it\"><div class=\"k\">");
    put(p, left, k);
    put(p, left, "</div><div class=\"v");
    if (cls != NULL) {
        put(p, left, " ");
        put(p, left, cls);
    }
    put(p, left, "\">");
    put(p, left, v);
    put(p, left, "</div></div>\n");
}

/**
 * @brief 渲染状态卡片中的一格（值来自模组 / 配置的定长字段，转义 + 限长）。
 * @param p    当前写指针的地址
 * @param left 剩余空间
 * @param k    标题（内部固定串）
 * @param cls  值额外样式类，可为 NULL
 * @param v    字段地址
 * @param vmax 字段最多扫描字节数（通常 sizeof 字段）
 */
static void ui_item_field(char **p, size_t *left, const char *k, const char *cls,
                          const char *v, size_t vmax)
{
    put(p, left, "<div class=\"it\"><div class=\"k\">");
    put(p, left, k);
    put(p, left, "</div><div class=\"v");
    if (cls != NULL) {
        put(p, left, " ");
        put(p, left, cls);
    }
    put(p, left, "\">");
    if (v == NULL || v[0] == '\0') {
        put(p, left, "-");
    } else {
        put_escaped_n(p, left, v, vmax);
    }
    put(p, left, "</div></div>\n");
}

/** 渲染 <label> */
static void ui_label(char **p, size_t *left, const char *for_id, const char *text)
{
    put(p, left, "<label for=\"");
    put(p, left, for_id);
    put(p, left, "\">");
    put(p, left, text);
    put(p, left, "</label>\n");
}

/**
 * @brief 渲染文本输入框（含 label）。
 * @param name  name/id 属性值（后端按此名解析）
 * @param label 中文标题
 * @param value 当前值（转义，最多扫描 vmax+1 字节）
 * @param vmax  maxlength 值
 * @param attrs 额外属性（可为 NULL）
 */
static void ui_text(char **p, size_t *left, const char *name, const char *label,
                    const char *value, size_t vmax, const char *attrs)
{
    ui_label(p, left, name, label);
    put(p, left, "<input type=\"text\" id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\" maxlength=\"");
    put_u32(p, left, (uint32_t)vmax);
    put(p, left, "\" value=\"");
    put_escaped_n(p, left, value, vmax + 1u);
    put(p, left, "\"");
    if (attrs != NULL) {
        put(p, left, " ");
        put(p, left, attrs);
    }
    put(p, left, ">\n");
}

/**
 * @brief 渲染数字输入框（含 label）。
 * @param name  name/id 属性值
 * @param label 中文标题
 * @param value 当前值
 * @param vmin  min 值
 * @param vmax  max 值
 */
static void ui_number(char **p, size_t *left, const char *name, const char *label,
                      uint32_t value, uint32_t vmin, uint32_t vmax)
{
    ui_label(p, left, name, label);
    put(p, left, "<input type=\"number\" id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\" min=\"");
    put_u32(p, left, vmin);
    put(p, left, "\" max=\"");
    put_u32(p, left, vmax);
    put(p, left, "\" value=\"");
    put_u32(p, left, value);
    put(p, left, "\">\n");
}

/**
 * @brief 渲染口令输入框（含 label，值永不回显）。
 * @param name        name/id 属性值
 * @param label       中文标题
 * @param placeholder 占位提示
 */
static void ui_password(char **p, size_t *left, const char *name, const char *label,
                        const char *placeholder)
{
    ui_label(p, left, name, label);
    put(p, left, "<input type=\"password\" id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\" maxlength=\"");
    put_u32(p, left, (uint32_t)(APP_CFG_PASS_MAX - 1));
    put(p, left, "\" value=\"\" autocomplete=\"new-password\" placeholder=\"");
    put(p, left, placeholder);
    put(p, left, "\">\n");
}

/**
 * @brief 渲染下拉框（含 label），选项值为下标。
 * @param name  name/id 属性值
 * @param label 中文标题
 * @param opts  选项文案数组
 * @param n     选项个数
 * @param cur   当前选中下标（越界则不选中，浏览器回落到第一项）
 */
static void ui_select(char **p, size_t *left, const char *name, const char *label,
                      const char *const *opts, size_t n, unsigned cur)
{
    size_t i;

    ui_label(p, left, name, label);
    put(p, left, "<select id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\">\n");
    for (i = 0u; i < n; i++) {
        put(p, left, "<option value=\"");
        put_u32(p, left, (uint32_t)i);
        put(p, left, "\"");
        if ((unsigned)i == cur) {
            put(p, left, " selected");
        }
        put(p, left, ">");
        put(p, left, opts[i]);
        put(p, left, "</option>\n");
    }
    put(p, left, "</select>\n");
}

/**
 * @brief 渲染复选框（勾选时提交 name=1，未勾选时浏览器不发该字段）。
 * @param name    name/id 属性值
 * @param label   文案
 * @param checked 非 0 表示勾选
 * @note 后端解析约定：字段缺失即视为 0（关闭）。
 */
static void ui_checkbox(char **p, size_t *left, const char *name, const char *label, int checked)
{
    put(p, left, "<label class=\"chk\"><input type=\"checkbox\" id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\" value=\"1\"");
    if (checked) {
        put(p, left, " checked");
    }
    put(p, left, "> ");
    put(p, left, label);
    put(p, left, "</label>\n");
}

/**
 * @brief 渲染号码列表多行文本框（一行一个号码，最多 APP_CFG_LIST_MAX 个）。
 * @param name  name/id 属性值
 * @param label 中文标题
 * @param items 号码二维数组
 * @param count 有效条数（会按 [0, APP_CFG_LIST_MAX] 收拢；非空项也会一并回显，避免丢数据）
 */
static void ui_list_textarea(char **p, size_t *left, const char *name, const char *label,
                             const char items[][APP_CFG_NUM_MAX], int count)
{
    int i;
    int cnt = count;

    if (cnt < 0) {
        cnt = 0;
    }
    if (cnt > APP_CFG_LIST_MAX) {
        cnt = APP_CFG_LIST_MAX;
    }
    ui_label(p, left, name, label);
    put(p, left, "<textarea id=\"");
    put(p, left, name);
    put(p, left, "\" name=\"");
    put(p, left, name);
    put(p, left, "\" rows=\"4\" placeholder=\"一行一个号码，最多 8 个\">");
    for (i = 0; i < APP_CFG_LIST_MAX; i++) {
        if (i >= cnt && items[i][0] == '\0') {
            continue;
        }
        put_escaped_n(p, left, items[i], APP_CFG_NUM_MAX);
        put(p, left, "\n");
    }
    put(p, left, "</textarea>\n");
}

/* ------------------------------------------------------------------ */
/* 公共接口 1：登录页                                                  */
/* ------------------------------------------------------------------ */
size_t web_ui_login_page(char *buf, size_t cap, int auth_failed)
{
    char *p;
    size_t left;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    p = buf;
    left = cap;

    ui_head(&p, &left, "登录 - 4G 短信网关");
    put(&p, &left, "<div class=\"card\" style=\"max-width:420px;margin:6vh auto\">\n");
    put(&p, &left, "<h1>4G 短信网关</h1>\n<p class=\"muted\">请输入管理口令后进入状态与配置页。</p>\n");
    if (auth_failed) {
        put(&p, &left, "<div class=\"err\">口令错误，请重新输入。</div>\n");
    }
    put(&p, &left, "<form method=\"post\" action=\"/login\">\n");
    ui_label(&p, &left, "password", "管理口令");
    put(&p, &left, "<input type=\"password\" id=\"password\" name=\"password\" maxlength=\"");
    put_u32(&p, &left, (uint32_t)(APP_CFG_PASS_MAX - 1));
    put(&p, &left, "\" autocomplete=\"current-password\" autofocus>\n");
    put(&p, &left, "<div style=\"margin-top:14px\"><button type=\"submit\" "
                   "style=\"width:100%\">登录</button></div>\n");
    put(&p, &left, "</form>\n");
    ui_foot(&p, &left);

    return (size_t)(p - buf);
}

/* ------------------------------------------------------------------ */
/* 公共接口 2：状态 + 配置页                                           */
/* ------------------------------------------------------------------ */
size_t web_ui_status_page(char *buf, size_t cap,
                          const web_status_t *st,
                          const app_config_t *cfg,
                          const web_sms_row_t *rows, size_t row_count)
{
    char *p;
    size_t left;
    char tmp[48];
    uint64_t limit_kb = 0u;
    unsigned pct = 0u;
    size_t i;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    if (st == NULL || cfg == NULL) {
        return web_ui_plain(buf, cap, "500 Internal Error: missing context");
    }
    if (rows == NULL) {
        row_count = 0u;
    }
    p = buf;
    left = cap;

    ui_head(&p, &left, "4G 短信网关 - 状态与配置");

    /* ---- 顶部超限告警横幅（需求 4.2） ---- */
    if (st->over_limit) {
        put(&p, &left, "<div class=\"banner\">⚠ 本月流量已超出限额，");
        if (cfg->traffic_action == APP_LIMIT_ACTION_ALERT_AND_CUT) {
            put(&p, &left, "已按配置断开网络（PDP 已释放）");
        } else {
            put(&p, &left, "当前为「仅告警」模式，网络未断开");
        }
        put(&p, &left, "，请检查资费或调整月度限额。</div>\n");
    }

    /* ---- 标题栏 ---- */
    put(&p, &left, "<div class=\"card top\"><h1>4G 短信网关</h1><div>\n");
    put(&p, &left, st->online ? "<span class=\"ok\">● 网络在线</span>"
                              : "<span class=\"bad\">● 网络离线</span>");
    put(&p, &left, "\n<a class=\"btn gray\" href=\"/logout\">退出登录</a>\n</div></div>\n");

    /* ---- 状态卡片（需求 5.2） ---- */
    put(&p, &left, "<div class=\"card\"><h2>运行状态</h2><div class=\"grid\">\n");
    ui_item_plain(&p, &left, "网络状态", st->online ? "ok" : "bad",
                  st->online ? "在线" : "离线");
    if (st->csq < 0 || st->csq == 99) {
        ui_item_plain(&p, &left, "信号强度", "muted", "未知");
    } else {
        char *tp = tmp;
        size_t tl = sizeof tmp;
        put_u32(&tp, &tl, (uint32_t)st->csq);
        put(&tp, &tl, " / ");
        put_i32(&tp, &tl, (int32_t)(-113 + 2 * st->csq));
        put(&tp, &tl, " dBm");
        ui_item_plain(&p, &left, "信号强度 (CSQ)",
                      (st->csq >= 15) ? "ok" : ((st->csq >= 8) ? "warn" : "bad"), tmp);
    }
    ui_item_field(&p, &left, "当前 APN", NULL, st->apn, sizeof st->apn);
    ui_item_plain(&p, &left, "USB 流量共享 (RNDIS)", st->rndis_on ? "ok" : "muted",
                  st->rndis_on ? "已开启" : "已关闭");
    ui_item_field(&p, &left, "生效方式", NULL, st->rndis_note, sizeof st->rndis_note);
    {
        char *tp = tmp;
        size_t tl = sizeof tmp;
        put(&tp, &tl, st->rndis_config ? "开启" : "关闭");
        if (((st->rndis_config != 0) ? 1 : 0) != ((st->rndis_on != 0) ? 1 : 0)) {
            put(&tp, &tl, "（与当前状态不同，需按生效方式应用）");
            ui_item_plain(&p, &left, "配置中的 RNDIS 开关", "warn", tmp);
        } else {
            put(&tp, &tl, "（与当前状态一致）");
            ui_item_plain(&p, &left, "配置中的 RNDIS 开关", NULL, tmp);
        }
    }
    ui_item_field(&p, &left, "模组 IPv4 (PDP)", NULL, st->ip, sizeof st->ip);
    /* IPv6：同时显示"配置开关"与"实际是否拿到地址"，避免"开了但运营商没给"被误解 */
    if (st->ip6[0] != '\0') {
        ui_item_field(&p, &left, "模组 IPv6 (PDP)", "ok", st->ip6, sizeof st->ip6);
    } else {
        ui_item_plain(&p, &left, "模组 IPv6 (PDP)",
                      st->ipv6_enable ? "warn" : "muted",
                      st->ipv6_enable ? "双栈已开启，但尚未获取到地址"
                                      : "未开启（PDP 为纯 IPv4）");
    }
    ui_item_field(&p, &left, "PDP 类型", NULL,
                  (st->pdp_type[0] != '\0') ? st->pdp_type : "-", sizeof st->pdp_type);
    if (st->pub_ip[0] != '\0') {
        ui_item_field(&p, &left, "公网 IPv4 出口", "ok", st->pub_ip, sizeof st->pub_ip);
    } else if (!st->pubip_tried) {
        ui_item_plain(&p, &left, "公网 IPv4 出口", "muted", "尚未查询");
    } else {
        ui_item_plain(&p, &left, "公网 IPv4 出口", "warn", "查询失败或未取到");
    }
    if (st->pub_ip6[0] != '\0') {
        /* 回显服务看到的是 v6 出口 —— 说明请求走了 IPv6（本工程已强制 v4 优先） */
        ui_item_field(&p, &left, "公网 IPv6 出口", "warn", st->pub_ip6, sizeof st->pub_ip6);
    }
    if (st->pubip_when[0] != '\0') {
        ui_item_field(&p, &left, "公网地址更新于", NULL, st->pubip_when, sizeof st->pubip_when);
    }
    ui_item_field(&p, &left, "IMEI", NULL, st->imei, sizeof st->imei);
    ui_item_field(&p, &left, "ICCID", NULL, st->iccid, sizeof st->iccid);
    ui_item_field(&p, &left, "固件版本", NULL, st->fw_ver, sizeof st->fw_ver);
    ui_item_field(&p, &left, "运行时长", NULL, st->uptime, sizeof st->uptime);
    ui_item_field(&p, &left, "当前时间", NULL, st->now, sizeof st->now);
    put(&p, &left, "</div></div>\n");

    /* ---- 流量卡片（需求 4.2 / 5.2） ---- */
    if (st->month_limit_mb > 0u) {
        limit_kb = (uint64_t)st->month_limit_mb * 1024u;
        pct = (unsigned)(((uint64_t)st->month_used_kb * 100u) / limit_kb);
        if (pct > 999u) {
            pct = 999u;
        }
    }
    put(&p, &left, "<div class=\"card\"><h2>流量统计</h2>\n");
    put(&p, &left, "<div class=\"k\">本月已用</div><div class=\"v\">");
    put_bytes(&p, &left, (uint64_t)st->month_used_kb * 1024u);
    put(&p, &left, " / ");
    if (st->month_limit_mb > 0u) {
        put_u32(&p, &left, st->month_limit_mb);
        put(&p, &left, " MB");
    } else {
        put(&p, &left, "不限");
    }
    put(&p, &left, "</div>\n<div class=\"bar");
    if (st->over_limit || (limit_kb > 0u && (uint64_t)st->month_used_kb * 1024u >= limit_kb)) {
        put(&p, &left, " over");
    }
    put(&p, &left, "\"><i style=\"width:");
    put_u32(&p, &left, (pct > 100u) ? 100u : pct);
    put(&p, &left, "%\"></i></div>\n<div class=\"k\">");
    if (limit_kb == 0u) {
        put(&p, &left, "未设置月度限额");
    } else {
        put_u32(&p, &left, pct);
        put(&p, &left, "%");
        if (st->over_limit) {
            put(&p, &left, " <span class=\"bad\">已超限</span>");
        }
    }
    put(&p, &left, "</div>\n<div class=\"grid\" style=\"margin-top:10px\">\n");
    {
        char *tp = tmp;
        size_t tl = sizeof tmp;
        if (st->directional_ok) {
            put_bytes(&tp, &tl, st->up_bytes);
        } else {
            /* 本版 ML307A-DSLN 底包不提供分方向字节统计，如实显示 N/A 而不填 0 */
            put(&tp, &tl, "N/A");
        }
        ui_item_plain(&p, &left, "上行（本次会话）", NULL, tmp);
        tp = tmp;
        tl = sizeof tmp;
        if (st->directional_ok) {
            put_bytes(&tp, &tl, st->down_bytes);
        } else {
            put(&tp, &tl, "N/A");
        }
        ui_item_plain(&p, &left, "下行（本次会话）", NULL, tmp);
        tp = tmp;
        tl = sizeof tmp;
        put_bytes(&tp, &tl, st->session_bytes);
        ui_item_plain(&p, &left, "本次会话合计", NULL, tmp);
    }
    if (!st->directional_ok) {
        put(&p, &left, "<p class=\"muted\">注：本固件所用底包未提供上下行分方向字节统计，"
                       "故上行/下行显示为 N/A；月度与本次会话合计量来自平台流量计数器，"
                       "数据真实有效。</p>\n");
    }
    put(&p, &left, "</div></div>\n");

    /* ---- 短信远程配置只读卡片（docs/09 / docs/10） ---- */
    /*
     * 为什么必须显示：转发 URL/密钥/关键词/模板有**两个来源** —— 本页表单（app_config）
     * 与短信指令（rcfg.bin）。短信里设的值优先，只在本页看会以为"改了没生效"。
     */
    put(&p, &left, "<div class=\"card\"><h2>短信远程配置");
    put(&p, &left, st->rcfg_loaded ? "（来自 rcfg.bin）" : "（默认值，尚未保存过）");
    put(&p, &left, "</h2>\n<p class=\"note\">这几项可用管理员手机短信设置（"
                   "<b>我要配置</b> / <b>U</b> URL / <b>T</b> 密钥 / <b>K</b> 关键词 / "
                   "<b>M</b> 模板 / <b>SAVE</b>）；<b>非空时优先于本页「告警推送」里的 Webhook 地址</b>，"
                   "想改回用本页配置，可发短信让 URL 置空或在本页填写后重启（本页保存不会覆盖它们）。</p>\n");
    put(&p, &left, "<div class=\"grid\">\n");
    ui_item_field(&p, &left, "配置管理员号码", NULL, st->rcfg_admin, sizeof st->rcfg_admin);
    ui_item_field(&p, &left, "短信配置的 Webhook", 
                  (st->rcfg_url[0] != '\0') ? "warn" : NULL,
                  (st->rcfg_url[0] != '\0') ? st->rcfg_url : "未设置（用本页配置）",
                  sizeof st->rcfg_url);
    ui_item_plain(&p, &left, "加签密钥", st->rcfg_secret_set ? "ok" : "muted",
                  st->rcfg_secret_set ? "已设置" : "未设置");
    ui_item_field(&p, &left, "关键词", NULL,
                  (st->rcfg_keyword[0] != '\0') ? st->rcfg_keyword : "未设置",
                  sizeof st->rcfg_keyword);
    ui_item_field(&p, &left, "内容模板", NULL, st->rcfg_tmpl, sizeof st->rcfg_tmpl);
    ui_item_plain(&p, &left, "长短信拼接", st->rcfg_concat ? "ok" : "muted",
                  st->rcfg_concat ? "已启用" : "已关闭");
    ui_item_field(&p, &left, "对时状态", NULL,
                  (st->sms_ntp[0] != '\0') ? st->sms_ntp : "-", sizeof st->sms_ntp);
    {
        char *tp = tmp;
        size_t tl = sizeof tmp;
        put(&tp, &tl, "分片 ");
        put_u32(&tp, &tl, st->sms_parts);
        put(&tp, &tl, " / 重组 ");
        put_u32(&tp, &tl, st->sms_reassembled);
        put(&tp, &tl, " / 指令 ");
        put_u32(&tp, &tl, st->sms_admin_cmd);
        put(&tp, &tl, " / 回执 ");
        put_u32(&tp, &tl, st->sms_replies);
        ui_item_plain(&p, &left, "长短信与指令统计", NULL, tmp);
    }
    put(&p, &left, "</div></div>\n");

    /* ---- 最近短信列表（需求 5.3）+ 导出入口 ---- */
    put(&p, &left, "<div class=\"card\"><div class=\"top\"><h2>最近短信（");
    put_u32(&p, &left, (uint32_t)row_count);
    put(&p, &left, " 条）</h2><a class=\"btn gray\" href=\"/export\">导出短信日志</a></div>\n");
    if (row_count == 0u) {
        put(&p, &left, "<p class=\"muted\">暂无短信记录。</p>\n");
    } else {
        put(&p, &left, "<table><thead><tr><th style=\"width:32%\">号码</th>"
                       "<th style=\"width:30%\">时间</th><th>内容</th></tr></thead><tbody>\n");
        for (i = 0u; i < row_count; i++) {
            put(&p, &left, "<tr><td class=\"num\">");
            PUT_ESC_FIELD(&p, &left, rows[i].number);
            put(&p, &left, "</td><td class=\"num\">");
            PUT_ESC_FIELD(&p, &left, rows[i].time);
            put(&p, &left, "</td><td class=\"msg\">");
            if (rows[i].text != NULL) {
                put_escaped_n(&p, &left, rows[i].text, UI_TEXT_SCAN_MAX);
            } else {
                put(&p, &left, "<span class=\"muted\">(无内容)</span>");
            }
            put(&p, &left, "</td></tr>\n");
        }
        put(&p, &left, "</tbody></table>\n");
    }
    put(&p, &left, "</div>\n");

    /* ---- 快捷操作区 ---- */
    put(&p, &left, "<div class=\"card\"><h2>快捷操作</h2><div class=\"row\">\n");
    /* IPv6：实时开启/关闭（会立即用新的 PDP 类型重新激活，几秒后地址才出现） */
    if (st->ipv6_enable) {
        put(&p, &left, "<form method=\"post\" action=\"/action/ipv6_off\">"
                       "<button type=\"submit\" class=\"btn gray\">关闭 IPv6（切回纯 IPv4 并立即重拨）</button></form>\n");
    } else {
        put(&p, &left, "<form method=\"post\" action=\"/action/ipv6_on\">"
                       "<button type=\"submit\">开启 IPv6 双栈（立即重拨）</button></form>\n");
    }
    put(&p, &left, "<form method=\"post\" action=\"/action/pubip_refresh\">"
                   "<button type=\"submit\" class=\"btn gray\">刷新公网 IP</button></form>\n");
    put(&p, &left, "<form method=\"post\" action=\"/action/apn_next\">"
                   "<button type=\"submit\">切换下一个 APN</button></form>\n");
    put(&p, &left, "<form method=\"post\" action=\"/action/reboot\" "
                   "onsubmit=\"return confirm('确定要重启模组吗？连接会中断约 30 秒。');\">"
                   "<button type=\"submit\" class=\"btn red\">重启模组</button></form>\n");
    put(&p, &left, "<form method=\"post\" action=\"/action/traffic_reset\" "
                   "onsubmit=\"return confirm('确定把本月流量统计清零吗？');\">"
                   "<button type=\"submit\" class=\"btn gray\">重置本月流量</button></form>\n");
    put(&p, &left, "<a class=\"btn gray\" href=\"/export\">导出短信日志</a>\n");
    put(&p, &left, "<a class=\"btn gray\" href=\"/logout\">退出登录</a>\n");
    put(&p, &left, "</div></div>\n");

    /* ---- 配置表单（需求 5.4） ---- */
    put(&p, &left, "<form class=\"card\" method=\"post\" action=\"/config\">\n");
    put(&p, &left, "<h2>配置（保存后写入 Flash）</h2>\n");

    put(&p, &left, "<fieldset><legend>告警推送</legend>\n");
    ui_text(&p, &left, "webhook_url", "Webhook 地址", cfg->webhook_url,
            (size_t)(APP_CFG_URL_MAX - 1),
            "placeholder=\"https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=...\"");
    ui_select(&p, &left, "webhook_type", "Webhook 类型",
              UI_OPT_WEBHOOK_TYPE, 4u, (unsigned)cfg->webhook_type);
    ui_checkbox(&p, &left, "webhook_enable",
                "启用 Webhook 推送（收到放行的短信时推送）",
                cfg->webhook_enable ? 1 : 0);
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>网络心跳</legend>\n");
    ui_text(&p, &left, "ping_target", "心跳目标（域名或 IP）", cfg->ping_target,
            (size_t)(APP_CFG_HOST_MAX - 1), "placeholder=\"www.baidu.com\"");
    ui_number(&p, &left, "ping_port", "心跳端口", cfg->ping_port, 1u, 65535u);
    ui_number(&p, &left, "heartbeat_ms", "心跳周期（毫秒）", cfg->heartbeat_ms,
              10000u, 86400000u);
    ui_number(&p, &left, "heartbeat_fail_limit", "连续失败次数判定断网",
              cfg->heartbeat_fail_limit, 1u, 20u);
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>Web 登录口令</legend>\n");
    ui_password(&p, &left, "password", "新口令", "留空表示不修改");
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>流量管理</legend>\n");
    ui_number(&p, &left, "traffic_limit_mb", "月度限额（MB，0 = 不限）",
              cfg->traffic_limit_mb, 0u, 1048576u);
    ui_select(&p, &left, "traffic_action", "超限动作",
              UI_OPT_LIMIT_ACTION, 2u, (unsigned)cfg->traffic_action);
    ui_number(&p, &left, "traffic_sample_sec", "采样周期（秒）",
              cfg->traffic_sample_sec, 10u, 3600u);
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>USB 流量共享 (RNDIS)</legend>\n");
    ui_select(&p, &left, "rndis_enable", "RNDIS 开关",
              UI_OPT_ONOFF, 2u, (unsigned)cfg->rndis_enable);
    ui_select(&p, &left, "rndis_apply_now", "生效方式",
              UI_OPT_APPLY, 2u, (unsigned)cfg->rndis_apply_now);
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>IPv6 与公网地址</legend>\n");
    ui_select(&p, &left, "ipv6_enable", "IPv6 双栈（PDP 类型 IPV4V6）",
              UI_OPT_ONOFF, 2u, (unsigned)cfg->ipv6_enable);
    put(&p, &left, "<p class=\"note\">保存后会在下次激活 PDP 时生效；想<b>立即生效</b>用上方"
                   "「开启 IPv6 双栈 / 关闭 IPv6」按钮（会马上重新拨号，地址几秒后出现）。</p>\n");
    ui_select(&p, &left, "pubip_enable", "定期查询公网出口 IP",
              UI_OPT_ONOFF, 2u, (unsigned)cfg->pubip_enable);
    ui_text(&p, &left, "pubip_url", "公网 IP 回显服务", cfg->pubip_url,
            (size_t)(APP_CFG_PUBIP_URL_MAX - 1),
            "placeholder=\"http://ip.3322.net\"");
    put(&p, &left, "<p class=\"note\">该服务返回本机出口 IP（纯文本或 JSON 均可解析），"
                   "默认 <code>http://ip.3322.net</code>；IPv4 在运营商 NAT 后时，"
                   "这里显示的是<b>运营商出口地址</b>，而 IPv6 全局地址本身就是公网地址。</p>\n");
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<fieldset><legend>短信黑白名单</legend>\n");
    ui_list_textarea(&p, &left, "sms_white", "白名单（非空时仅放行名单内号码）",
                     cfg->sms_white, (int)cfg->white_count);
    ui_list_textarea(&p, &left, "sms_black", "黑名单（命中直接拦截，优先级高于白名单）",
                     cfg->sms_black, (int)cfg->black_count);
    put(&p, &left, "</fieldset>\n");

    put(&p, &left, "<button type=\"submit\">保存配置（写入 Flash）</button>\n");
    put(&p, &left, "<p class=\"note\">保存后立即落盘；RNDIS 是否即时生效由上方「生效方式」决定。</p>\n");
    put(&p, &left, "</form>\n");

    ui_foot(&p, &left);
    return (size_t)(p - buf);
}

/* ------------------------------------------------------------------ */
/* 公共接口 3：短信日志导出                                            */
/* ------------------------------------------------------------------ */
size_t web_ui_export_txt(char *buf, size_t cap,
                         const web_sms_row_t *rows, size_t row_count)
{
    char *p;
    size_t left;
    size_t body_len;
    size_t i;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    if (rows == NULL) {
        row_count = 0u;
    }
    p = buf;
    left = cap;

    /* 先把正文渲染在缓冲起始处，随后由 http_ok_with_body() 前置响应头 */
    put(&p, &left, "4G 短信网关 - 短信日志导出\r\n");
    put(&p, &left, "记录条数: ");
    put_u32(&p, &left, (uint32_t)row_count);
    put(&p, &left, "\r\n------------------------------------------------------------\r\n");
    for (i = 0u; i < row_count; i++) {
        put(&p, &left, "[");
        put_u32(&p, &left, (uint32_t)(i + 1u));
        put(&p, &left, "] ");
        put_txt_field(&p, &left, rows[i].time, sizeof rows[i].time);
        put(&p, &left, "  ");
        put_txt_field(&p, &left, rows[i].number, sizeof rows[i].number);
        put(&p, &left, "\r\n");
        put_txt_raw(&p, &left, rows[i].text, UI_TEXT_SCAN_MAX);
        put(&p, &left, "\r\n");
    }
    body_len = (size_t)(p - buf);

    return http_ok_with_body(buf, cap, body_len, "text/plain; charset=utf-8",
                             "Content-Disposition: attachment; filename=\"sms_log.txt\"\r\n");
}

/* ------------------------------------------------------------------ */
/* 公共接口 4：302 跳转                                                */
/* ------------------------------------------------------------------ */
size_t web_ui_redirect(char *buf, size_t cap, const char *location)
{
    char *p;
    size_t left;
    size_t i;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    if (location == NULL || location[0] == '\0') {
        location = "/";
    }
    p = buf;
    left = cap;

    put(&p, &left, "HTTP/1.1 302 Found\r\nLocation: ");
    /* 剔除 CR/LF，防止 Location 值被用于 HTTP 响应头注入 */
    for (i = 0u; location[i] != '\0'; i++) {
        char c = location[i];
        if (c == '\r' || c == '\n') {
            continue;
        }
        putn(&p, &left, &c, 1u);
    }
    put(&p, &left, "\r\nContent-Type: text/plain; charset=utf-8\r\n"
                   "Content-Length: 0\r\nCache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n");
    return (size_t)(p - buf);
}

/* ------------------------------------------------------------------ */
/* 公共接口 5：纯文本 / 错误响应                                       */
/* ------------------------------------------------------------------ */
size_t web_ui_plain(char *buf, size_t cap, const char *msg)
{
    char *p;
    size_t left;
    size_t body_len;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    p = buf;
    left = cap;
    put(&p, &left, (msg != NULL) ? msg : "");
    body_len = (size_t)(p - buf);

    return http_ok_with_body(buf, cap, body_len, "text/plain; charset=utf-8", NULL);
}
