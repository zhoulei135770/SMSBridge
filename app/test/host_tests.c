/**
 * @file    host_tests.c
 * @brief   主机端单元测试（PC 上用 gcc 直接跑，不需要模组）。
 *
 * 覆盖范围（全部是"纯 C、无 SDK 依赖"的模块，因此可以在 PC 上把逻辑先测对）：
 *   1. SHA-256：FIPS 180-4 已知向量 + 出厂默认口令摘要
 *   2. app_config：默认值 / 合法性修正 / CRC32 基准 / 序列化往返 / 损坏检测 /
 *                  黑白名单语义
 *   3. sms_pdu：48 条由独立 Python 编码器生成的用例往返比对 + 健壮性（非法输入不崩）
 *   4. webhook：JSON 转义（引号/换行/中文）与企业微信-钉钉报文结构差异
 *   5. web_ui：**缓冲区越界**（用极小 cap 反复调用）与 HTML 转义（XSS）
 *   6. sms_cmd / sms_rcfg：短信远程配置的指令解析、持久化尺寸契约、
 *      坏文件回退（不存在/掉电半截写/校验和损坏）与 rcfg.bin 落盘往返
 *
 * 编译运行：pwsh -File app/test/run_host_tests.ps1
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "sms_pdu.h"
#include "sms_svc.h"      /* 仅为取 SMS_RECORD_NUM / SMS_RECORD_TEXT_MAX 等尺寸常量 */
#include "app_config.h"
#include "sha256.h"
#include "webhook.h"
#include "web_ui.h"
#include "sms_cmd.h"
#include "sms_rcfg.h"
#include "sms_admin.h"
#include "sms_concat.h"
#include "webhook_fmt.h"
#include "app_time.h"
#include "net_addr.h"
#include "pdu_vectors.h"

/* ================================================================== */
/* 极简断言框架                                                        */
/* ================================================================== */
static int g_pass;
/** 测试辅助：显式把管理员设为某号码（出厂默认现在是"空 + 未认领"） */
static void t_set_admin(sms_remote_cfg_t *c, const char *num)
{
    (void)snprintf(c->admin, sizeof(c->admin), "%s", num);
    c->admin_claimed = 1u;
}
static int g_fail;
static const char *g_case;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            printf("         用例: %s\n", (g_case != NULL) ? g_case : "-");\
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want)                                               \
    do {                                                                   \
        if (strcmp((got), (want)) == 0) { ++g_pass; }                       \
        else {                                                             \
            ++g_fail;                                                      \
            printf("  [FAIL] %s:%d  字符串不等\n", __FILE__, __LINE__);    \
            printf("         用例: %s\n", (g_case != NULL) ? g_case : "-");\
            printf("         期望: %s\n", (want));                         \
            printf("         实际: %s\n", (got));                          \
        }                                                                  \
    } while (0)

/* ================================================================== */
/* sdk_port 桩：webhook.c 会引用 sdk_http_post_json                     */
/* ================================================================== */
static char  g_stub_rsp[128];
static int   g_stub_code = 200;

int sdk_http_post_json(const char *url, const char *content_type,
                       const char *body, uint32_t body_len,
                       char *rsp, size_t rsp_size)
{
    (void)url; (void)content_type; (void)body; (void)body_len;
    if (rsp != NULL && rsp_size > 0u) {
        size_t n = strlen(g_stub_rsp);
        if (n >= rsp_size) n = rsp_size - 1u;
        memcpy(rsp, g_stub_rsp, n);
        rsp[n] = '\0';
    }
    return g_stub_code;
}

/* ================================================================== */
/* sdk_port 桩：sms_rcfg.c 的落盘 I/O（用单槽内存"假文件系统"替代 cm_fs）*/
/* ================================================================== */
static uint8_t g_fake_fs[2048];
static int32_t g_fake_fs_len = -1;      /* -1 = 文件不存在 */
static int     g_fake_fs_writes;

int32_t sdk_file_read(const char *name, void *out, uint32_t out_size)
{
    if (name == NULL || out == NULL) return -1;
    if (strcmp(name, SMS_RCFG_FILE) != 0) return -2;
    if (g_fake_fs_len < 0) return -2;                     /* 文件不存在 */
    if ((uint32_t)g_fake_fs_len > out_size) return -1;    /* 缓冲不足 */
    memcpy(out, g_fake_fs, (size_t)g_fake_fs_len);
    return g_fake_fs_len;
}

int32_t sdk_file_write(const char *name, const void *in, uint32_t size)
{
    if (name == NULL || in == NULL || size == 0u) return -1;
    if (strcmp(name, SMS_RCFG_FILE) != 0) return -2;
    if (size > sizeof(g_fake_fs)) return -1;
    memcpy(g_fake_fs, in, size);
    g_fake_fs_len = (int32_t)size;
    ++g_fake_fs_writes;
    return (int32_t)size;
}

/* ================================================================== */
/* 工具                                                                */
/* ================================================================== */
static void bytes_to_hex_upper(const char *data, size_t len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)data[i];
        out[i * 2]     = hex[(c >> 4) & 0x0Fu];
        out[i * 2 + 1] = hex[c & 0x0Fu];
    }
    out[len * 2] = '\0';
}

/* ================================================================== */
/* 1. SHA-256                                                          */
/* ================================================================== */
static void test_sha256(void)
{
    char hex[SHA256_HEX_SIZE];

    printf("[1] SHA-256\n");

    g_case = "sha256(\"\")";
    sha256_hex("", 0u, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    g_case = "sha256(\"abc\")";
    sha256_hex("abc", 3u, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    g_case = "sha256(448bit 多块)";
    sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* 出厂默认口令 admin123 —— 必须与 app_config.c 里写死的摘要一致，否则永远登录不上 */
    g_case = "sha256(\"admin123\") == 默认配置摘要";
    sha256_hex("admin123", 8u, hex);
    CHECK_STR(hex, "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9");

    g_case = "sha256_hex_equal 大小写不敏感";
    CHECK(sha256_hex_equal("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789",
                           "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") == 1);
    g_case = "sha256_hex_equal 不同则不等";
    CHECK(sha256_hex_equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                           "0000000000000000000000000000000000000000000000000000000000000000") == 0);
}

/* ================================================================== */
/* 2. app_config                                                       */
/* ================================================================== */
static void test_config(void)
{
    app_config_t cfg;
    uint8_t      blob[sizeof(app_config_blob_t) + 16u];
    size_t       n;
    uint32_t     crc;
    const char  *reason = NULL;

    printf("[2] app_config\n");

    g_case = "CRC32 基准向量 123456789 -> 0xCBF43926";
    crc = app_config_crc32("123456789", 9u);
    CHECK(crc == 0xCBF43926u);

    g_case = "默认值：默认启用 RNDIS（需求 2.2）";
    app_config_defaults(&cfg);
    CHECK(cfg.rndis_enable == 1u);
    CHECK(cfg.apn_index == 0u);
    g_case = "默认值：IPv6 双栈与公网 IP 查询默认开启（v1.2）";
    CHECK(cfg.ipv6_enable == 1u);
    CHECK(cfg.pubip_enable == 1u);
    CHECK_STR(cfg.pubip_url, "http://ip.3322.net");
    g_case = "默认 APN 表";
    CHECK_STR(g_apn_table[0], "cmnet");
    CHECK_STR(g_apn_table[1], "3gnet");
    CHECK_STR(g_apn_table[2], "ctnet");

    g_case = "默认配置必须通过合法性校验（0 处修正）";
    CHECK(app_config_sanitize(&cfg) == 0);

    g_case = "非法值被修正";
    cfg.heartbeat_ms        = 5u;             /* 太小 */
    cfg.traffic_sample_sec  = 0u;             /* 非法 */
    cfg.rndis_enable        = 9u;             /* 越界 */
    cfg.apn_index           = 99u;            /* 越界 */
    cfg.webhook_type        = 42u;            /* 越界 */
    cfg.pass_sha256[0]      = 'x';            /* 不是 64 位 hex */
    CHECK(app_config_sanitize(&cfg) > 0);
    CHECK(cfg.heartbeat_ms >= 10000u);
    CHECK(cfg.traffic_sample_sec >= 10u);
    CHECK(cfg.rndis_enable <= 1u);
    CHECK(cfg.apn_index < APP_APN_COUNT);
    CHECK(cfg.webhook_type <= (uint8_t)APP_WEBHOOK_RAW);
    g_case = "非法口令摘要被回退为出厂口令";
    CHECK_STR(cfg.pass_sha256,
              "240be518fabd2724ddb6f04eeb1da5967448d7e831c08c8fa822809f74c720a9");

    g_case = "序列化 + 反序列化往返一致";
    app_config_defaults(&cfg);
    (void)snprintf(cfg.webhook_url, sizeof(cfg.webhook_url),
                   "https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=abc123");
    cfg.traffic_limit_mb = 2048u;
    cfg.rndis_enable     = 0u;
    cfg.ipv6_enable      = 0u;
    (void)snprintf(cfg.pubip_url, sizeof(cfg.pubip_url), "%s", "https://api.ipify.org");
    (void)snprintf(cfg.sms_white[0], APP_CFG_NUM_MAX, "13800138000");
    cfg.white_count = 1u;
    n = app_config_serialize(&cfg, blob, sizeof(blob));
    CHECK(n == sizeof(app_config_blob_t));
    {
        app_config_t back;
        memset(&back, 0, sizeof(back));
        g_case = "反序列化成功";
        CHECK(app_config_deserialize(blob, n, &back) == 0);
        g_case = "往返后字段一致";
        CHECK(back.traffic_limit_mb == 2048u);
        CHECK(back.rndis_enable == 0u);
        CHECK(back.white_count == 1u);
        CHECK(back.ipv6_enable == 0u);                       /* v1.2 新增字段 */
        CHECK_STR(back.pubip_url, "https://api.ipify.org");
        CHECK_STR(back.webhook_url, cfg.webhook_url);
    }

    g_case = "篡改 payload 内一个字节就被 CRC 检出";
    /* 注意：必须改在 payload（cfg 结构体）内部，改在尾部填充区 CRC 覆盖不到 */
    blob[16u + 3u] ^= 0xFFu;
    {
        app_config_t back;
        CHECK(app_config_deserialize(blob, n, &back) != 0);
    }
    g_case = "magic 错误被检出";
    app_config_defaults(&cfg);
    n = app_config_serialize(&cfg, blob, sizeof(blob));
    blob[0] ^= 0xFFu;
    {
        app_config_t back;
        CHECK(app_config_deserialize(blob, n, &back) != 0);
    }
    g_case = "长度不足被拒绝";
    {
        app_config_t back;
        CHECK(app_config_deserialize(blob, 8u, &back) != 0);
    }

    /* 黑白名单语义（需求 3.3） */
    g_case = "白名单为空 -> 不启用白名单，全部放行";
    app_config_defaults(&cfg);
    CHECK(app_config_sms_allowed(&cfg, "+8613800138000", &reason) == 1);

    g_case = "黑名单命中 -> 拦截";
    cfg.black_count = 1u;
    (void)snprintf(cfg.sms_black[0], APP_CFG_NUM_MAX, "13800138000");
    CHECK(app_config_sms_allowed(&cfg, "+8613800138000", &reason) == 0);
    CHECK_STR(reason, "blacklist");

    g_case = "黑名单优先级高于白名单";
    cfg.white_count = 1u;
    (void)snprintf(cfg.sms_white[0], APP_CFG_NUM_MAX, "13800138000");
    CHECK(app_config_sms_allowed(&cfg, "13800138000", &reason) == 0);

    g_case = "白名单非空 -> 名单外号码被拦截";
    cfg.black_count = 0u;
    (void)snprintf(cfg.sms_white[0], APP_CFG_NUM_MAX, "13900139000");
    CHECK(app_config_sms_allowed(&cfg, "13800138000", &reason) == 0);
    g_case = "白名单内号码放行（带国家码也能匹配）";
    CHECK(app_config_sms_allowed(&cfg, "+8613900139000", &reason) == 1);

    g_case = "号码规整化：空格/横线/括号不影响匹配";
    CHECK(app_config_sms_allowed(&cfg, "+86 139-0013 (9000)", &reason) == 1);

    g_case = "APN 轮询回绕";
    app_config_defaults(&cfg);
    CHECK(app_config_apn_next(&cfg) == 1u);
    CHECK(app_config_apn_next(&cfg) == 2u);
    CHECK(app_config_apn_next(&cfg) == 0u);
    CHECK_STR(app_config_apn(&cfg), "cmnet");
}

/** 哨兵检测：ret 之后到 cap-1 的字节不应被函数写入（返回 1 = 干净）。 */
static int canary_clean(const char *buf, size_t cap, size_t ret)
{
    size_t k;
    for (k = ret + 1u; k < cap; ++k) {
        if (buf[k] != (char)0xAA) return 0;
    }
    return 1;
}

static void check_one(const char *label, size_t cap, size_t ret, const char *buf)
{
    g_case = label;
    if (ret >= cap) {
        ++g_fail;
        printf("  [FAIL] %s cap=%u 返回值 %u 超出容量\n",
               label, (unsigned)cap, (unsigned)ret);
        return;
    }
    ++g_pass;
    if (buf[ret] != '\0') {
        ++g_fail;
        printf("  [FAIL] %s cap=%u ret=%u 未以 \\0 结尾\n",
               label, (unsigned)cap, (unsigned)ret);
        return;
    }
    ++g_pass;
    if (!canary_clean(buf, cap, ret)) {
        ++g_fail;
        printf("  [FAIL] %s cap=%u ret=%u **写越界**（ret 之后有哨兵被改写）\n",
               label, (unsigned)cap, (unsigned)ret);
        {
            size_t k;
            for (k = ret + 1u; k < cap; ++k) {
                if (buf[k] != (char)0xAA) {
                    printf("         首个被改写的偏移 = %u（原 0xAA，现 0x%02X）\n",
                           (unsigned)k, (unsigned char)buf[k]);
                    break;
                }
            }
        }
        return;
    }
    ++g_pass;
}

/* ================================================================== */
/* 3. sms_pdu（独立编码器往返）                                        */
/* ================================================================== */
static void test_sms_pdu(void)
{
    int i;

    printf("[3] sms_pdu（%d 条独立编码器用例）\n", SMS_VEC_COUNT);

    for (i = 0; i < SMS_VEC_COUNT; ++i) {
        const struct sms_pdu_vector *v = &g_vectors[i];
        sms_pdu_t pdu;
        sms_pdu_status_t st;
        char      hex[SMS_TEXT_MAX_BYTES * 2 + 8];

        g_case = v->name;

        st = sms_pdu_decode(v->pdu, &pdu);
        if (st != SMS_PDU_OK) {
            ++g_fail;
            printf("  [FAIL] 解码失败 st=%d(%s) 用例=%s\n",
                   (int)st, sms_pdu_strerror(st), v->name);
            continue;
        }
        ++g_pass;

        if (v->want_number != NULL) {
            CHECK_STR(pdu.number, v->want_number);
        }
        if (v->want_text_hex != NULL) {
            bytes_to_hex_upper(pdu.text, pdu.text_len, hex);
            if (strcmp(hex, v->want_text_hex) != 0) {
                ++g_fail;
                printf("  [FAIL] 正文不符 用例=%s\n", v->name);
                printf("         期望 hex: %s\n", v->want_text_hex);
                printf("         实际 hex: %s\n", hex);
                printf("         实际文本: %s\n", pdu.text);
            } else {
                ++g_pass;
            }
        }
        if (v->want_scts != NULL) {
            CHECK_STR(pdu.scts, v->want_scts);
        }
        if (v->want_alpha >= 0) {
            CHECK((int)pdu.alpha == v->want_alpha);
        }
        if (v->want_udhi >= 0) {
            CHECK((int)pdu.udhi == v->want_udhi);
        }
        if (v->want_udl >= 0) {
            CHECK((int)pdu.udl == v->want_udl);
        }
        if (v->want_tz != INT32_MIN) {
            CHECK((int)pdu.tz_quarters == v->want_tz);
        }
        if (v->want_concat_present >= 0) {
            if ((int)pdu.concat.present != v->want_concat_present) {
                ++g_fail;
                printf("  [FAIL] concat.present 期望 %d 实际 %d 用例=%s"
                       "（udhi=%d udhl=%d udl=%d alpha=%d）\n",
                       v->want_concat_present, (int)pdu.concat.present, v->name,
                       (int)pdu.udhi, (int)pdu.udh_len, (int)pdu.udl, (int)pdu.alpha);
            } else {
                ++g_pass;
            }
        }
        if (v->want_concat_present == 1) {
            if (v->want_concat_16bit >= 0 && (int)pdu.concat.is_16bit != v->want_concat_16bit) {
                ++g_fail;
                printf("  [FAIL] concat.is_16bit 期望 %d 实际 %d 用例=%s（udhl=%d）\n",
                       v->want_concat_16bit, (int)pdu.concat.is_16bit, v->name,
                       (int)pdu.udh_len);
            } else if (v->want_concat_16bit >= 0) {
                ++g_pass;
            }
            if (v->want_concat_ref >= 0)   CHECK((int)pdu.concat.ref == v->want_concat_ref);
            if (v->want_concat_total >= 0) CHECK((int)pdu.concat.total == v->want_concat_total);
            if (v->want_concat_seq >= 0)   CHECK((int)pdu.concat.seq == v->want_concat_seq);
        }
    }

    /* 健壮性：非法输入必须返回错误码而不是崩溃（配合 -fsanitize=address） */
    printf("[3b] sms_pdu 健壮性\n");
    {
        sms_pdu_t pdu;
        char huge[SMS_PDU_MAX_HEX + 64];

        g_case = "NULL 入参";
        CHECK(sms_pdu_decode(NULL, &pdu) == SMS_PDU_ERR_ARG);
        g_case = "空串";
        CHECK(sms_pdu_decode("", &pdu) == SMS_PDU_ERR_TRUNC);
        g_case = "奇数长度十六进制";
        CHECK(sms_pdu_decode("0791", &pdu) != SMS_PDU_OK);
        g_case = "非法字符";
        CHECK(sms_pdu_decode("ZZZZZZZZZZZZZZZZZZZZ", &pdu) == SMS_PDU_ERR_HEX);
        g_case = "只有 SCA 的截断报文";
        CHECK(sms_pdu_decode("07917283010010F5", &pdu) != SMS_PDU_OK);
        g_case = "超长报文不越界";
        memset(huge, 'A', sizeof(huge) - 1u);
        huge[sizeof(huge) - 1u] = '\0';
        (void)sms_pdu_decode(huge, &pdu);   /* 只要不崩即通过 */
        ++g_pass;
        g_case = "半截合法报文";
        {
            /* 取第一条向量的前 1/3 长度 */
            size_t len = strlen(g_vectors[0].pdu) / 3u;
            char   cut[SMS_PDU_MAX_HEX];
            if (len > 0u && len < sizeof(cut)) {
                memcpy(cut, g_vectors[0].pdu, len);
                cut[len] = '\0';
                CHECK(sms_pdu_decode(cut, &pdu) != SMS_PDU_OK);
            }
        }
    }
}

/* ================================================================== */
/* 4. webhook JSON                                                     */
/* ================================================================== */
static void test_webhook(void)
{
    app_config_t cfg;
    char         buf[2048];
    size_t       n;

    printf("[4] webhook\n");

    g_case = "URL 自动识别机器人类型";
    CHECK(webhook_detect_type("https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=x")
          == APP_WEBHOOK_WECOM);
    CHECK(webhook_detect_type("https://oapi.dingtalk.com/robot/send?access_token=x")
          == APP_WEBHOOK_DINGTALK);
    CHECK(webhook_detect_type("http://192.168.1.10:9000/hook") == APP_WEBHOOK_RAW);

    g_case = "企业微信报文用 markdown.content";
    app_config_defaults(&cfg);
    cfg.webhook_type = (uint8_t)APP_WEBHOOK_WECOM;
    n = webhook_build_sms_markdown(&cfg, "+8613800138000", "测试\"引号\"与\\反斜杠", "2025-01-02 03:04:05",
                                   buf, sizeof(buf));
    CHECK(n > 0u);
    CHECK(strstr(buf, "\"msgtype\":\"markdown\"") != NULL);
    CHECK(strstr(buf, "\"content\":\"") != NULL);
    CHECK(strstr(buf, "\\\"引号\\\"") != NULL);      /* 引号必须被转义 */
    CHECK(strstr(buf, "\\\\反斜杠") != NULL);         /* 反斜杠必须被转义 */

    g_case = "钉钉报文用 markdown.title + text";
    cfg.webhook_type = (uint8_t)APP_WEBHOOK_DINGTALK;
    n = webhook_build_note_markdown(&cfg, "流量告警", "已用 100MB", buf, sizeof(buf));
    CHECK(n > 0u);
    CHECK(strstr(buf, "\"title\":\"") != NULL);
    CHECK(strstr(buf, "\"text\":\"") != NULL);

    g_case = "换行被转义为 \\n，报文仍是单行 JSON";
    CHECK(strstr(buf, "\\n") != NULL);
    CHECK(strchr(buf, '\n') == NULL);

    g_case = "中文按 UTF-8 原样透传（不做 \\u 转义）";
    n = webhook_build_note_markdown(&cfg, "中文标题", "中文内容", buf, sizeof(buf));
    CHECK(n > 0u);
    CHECK(strstr(buf, "中文内容") != NULL);
}

/* ================================================================== */
/* 5. web_ui 缓冲区边界与转义                                          */
/* ================================================================== */
static void test_web_ui(void)
{
    static char big[WEB_UI_STATUS_PAGE_MAX];
    /* 注意：本缓冲必须 >= caps[] 里的最大值。曾因写成 512 而把 cap=1024
     * 传进来，导致状态页写越界冲掉全局计数器（表现为“通过数变成垃圾值”和
     * 运行期访问违例），这是测试代码自身的 bug，不是被测代码的缺陷。 */
    static char small[4096];
    app_config_t cfg;
    web_status_t st;
    web_sms_row_t rows[2];
    size_t caps[] = { 1u, 2u, 3u, 16u, 64u, 255u, 1024u };
    size_t i;

    printf("[5] web_ui 缓冲区边界与转义\n");

    app_config_defaults(&cfg);
    memset(&st, 0, sizeof(st));
    st.online = 1;
    (void)snprintf(st.apn, sizeof(st.apn), "cmnet");
    st.csq = 23;
    st.rndis_on = 1;
    (void)snprintf(st.rndis_note, sizeof(st.rndis_note), "即时生效");
    (void)snprintf(st.ip, sizeof(st.ip), "10.11.12.13");
    /* IPv6 / 公网地址 / 短信远程配置（本轮新增展示项） */
    (void)snprintf(st.ip6, sizeof(st.ip6), "2409:xxxx:xxxx:xxxx::1");
    (void)snprintf(st.pub_ip, sizeof(st.pub_ip), "203.0.113.7");
    (void)snprintf(st.pdp_type, sizeof(st.pdp_type), "IPV4V6");
    st.ipv6_enable = 1;
    st.ipv6_on = 1;
    st.pubip_tried = 1;
    st.pubip_ok = 1;
    (void)snprintf(st.pubip_when, sizeof(st.pubip_when), "3 分钟前");
    st.rcfg_loaded = 1;
    (void)snprintf(st.rcfg_admin, sizeof(st.rcfg_admin), "13800000000");
    (void)snprintf(st.rcfg_url, sizeof(st.rcfg_url), "https://oapi.dingtalk.com/robot/send?access_token=r");
    st.rcfg_secret_set = 1;
    (void)snprintf(st.rcfg_keyword, sizeof(st.rcfg_keyword), "报警");
    (void)snprintf(st.rcfg_tmpl, sizeof(st.rcfg_tmpl), "{from}|{time}|{text}");
    st.rcfg_concat = 1;
    (void)snprintf(st.sms_ntp, sizeof(st.sms_ntp), "已对时");
    st.sms_parts = 7u;
    st.sms_reassembled = 2u;
    st.sms_admin_cmd = 3u;
    st.sms_replies = 3u;
    st.month_used_kb = 123456u;
    st.month_limit_mb = 2048u;
    st.directional_ok = 0;
    st.over_limit = 1;
    (void)snprintf(st.uptime, sizeof(st.uptime), "3天 04:15:22");

    /* 恶意内容：短信号码与正文里塞 HTML/脚本 */
    (void)snprintf(rows[0].number, sizeof(rows[0].number), "<script>bad()</script>");
    (void)snprintf(rows[0].time, sizeof(rows[0].time), "2025-01-02 03:04:05");
    rows[0].text = "<img src=x onerror=alert(1)> & \"quoted\"";
    rows[1].number[0] = '\0';
    rows[1].time[0] = '\0';
    rows[1].text = NULL;

    /* 5a: 极小缓冲区不得越界（用 0xAA 哨兵字节检测写越界） */
    g_case = "极小容量下写入不越界";
    for (i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
        size_t cap = caps[i];
        size_t r;

        memset(small, 0xAA, sizeof(small));
        r = web_ui_login_page(small, cap, 1);
        check_one("login_page", cap, r, small);

        memset(small, 0xAA, sizeof(small));
        r = web_ui_redirect(small, cap, "/");
        check_one("redirect", cap, r, small);

        memset(small, 0xAA, sizeof(small));
        r = web_ui_plain(small, cap, "err");
        check_one("plain", cap, r, small);

        memset(small, 0xAA, sizeof(small));
        r = web_ui_status_page(small, cap, &st, &cfg, rows, 2u);
        check_one("status_page", cap, r, small);

        /* 中等缓冲也测一次（最容易踩到截断逻辑） */
        {
            static char mid[8192];
            memset(mid, 0xAA, sizeof(mid));
            r = web_ui_status_page(mid, sizeof(mid), &st, &cfg, rows, 2u);
            check_one("status_page(8K)", sizeof(mid), r, mid);
        }
        {
            static char mid[8192];
            memset(mid, 0xAA, sizeof(mid));
            r = web_ui_export_txt(mid, sizeof(mid), rows, 2u);
            check_one("export_txt(8K)", sizeof(mid), r, mid);
        }
    }

    /* 5b: cap = 0 不允许写 */
    g_case = "cap=0 时返回 0 且不写入";
    {
        char one = (char)0x5A;
        size_t r = web_ui_plain(&one, 0u, "x");
        CHECK(r == 0u);
        CHECK(one == (char)0x5A);
    }

    /* 5c: 完整渲染内容检查 */
    g_case = "状态页包含全部配置表单字段名（需求 5.4）";
    memset(big, 0, sizeof(big));
    {
        size_t r = web_ui_status_page(big, sizeof(big), &st, &cfg, rows, 2u);
        CHECK(r > 0u);
        CHECK(strstr(big, "name=\"webhook_url\"") != NULL);
        CHECK(strstr(big, "name=\"webhook_type\"") != NULL);
        CHECK(strstr(big, "name=\"ping_target\"") != NULL);
        CHECK(strstr(big, "name=\"heartbeat_ms\"") != NULL);
        CHECK(strstr(big, "name=\"password\"") != NULL);
        CHECK(strstr(big, "name=\"sms_white\"") != NULL);
        CHECK(strstr(big, "name=\"sms_black\"") != NULL);
        CHECK(strstr(big, "name=\"traffic_limit_mb\"") != NULL);
        CHECK(strstr(big, "name=\"traffic_action\"") != NULL);
        CHECK(strstr(big, "name=\"traffic_sample_sec\"") != NULL);
        CHECK(strstr(big, "name=\"rndis_enable\"") != NULL);
    }
    g_case = "状态页包含动作入口（需求 5.3）";
    CHECK(strstr(big, "/action/apn_next") != NULL);
    CHECK(strstr(big, "/action/reboot") != NULL);
    CHECK(strstr(big, "/action/traffic_reset") != NULL);
    CHECK(strstr(big, "/export") != NULL);
    CHECK(strstr(big, "/logout") != NULL);

    g_case = "短信内容被 HTML 转义（XSS 防护）";
    CHECK(strstr(big, "<script>bad()</script>") == NULL);
    CHECK(strstr(big, "onerror=alert(1)") == NULL || strstr(big, "&lt;img") != NULL);
    CHECK(strstr(big, "&lt;script&gt;") != NULL);

    g_case = "超限时页面出现醒目告警";
    CHECK(strstr(big, "已超限") != NULL);

    g_case = "分方向统计不可用时显示 N/A 而非 0";
    CHECK(strstr(big, "N/A") != NULL);

    g_case = "登录页含密码表单";
    {
        size_t r = web_ui_login_page(small, sizeof(small), 1);
        CHECK(r > 0u);
        CHECK(strstr(small, "name=\"password\"") != NULL);
        CHECK(strstr(small, "/login") != NULL);
    }

    /* ---- 回归测试：这两个断言曾经失败，属于真实缺陷 ----
     * 1) 登录页完整长度约 3.3KB，若 WEB_UI_LOGIN_PAGE_MAX 偏小（曾为 2048），
     *    页面会在 <style> 中被截断，<form> 根本不会输出，浏览器上表现为
     *    "只有样式、没有输入框"，用户永远无法登录。
     * 2) 状态页在黑白名单全满 + 16 条长短信时接近 15KB，必须能完整渲染。
     */
    g_case = "新增展示项：IPv6 / 公网地址 / 短信远程配置都出现在状态页";
    {
        static char      sp[WEB_UI_STATUS_PAGE_MAX];
        size_t           r = web_ui_status_page(sp, sizeof(sp), &st, &cfg, rows, 2u);

        CHECK(r > 0u && r < sizeof(sp));
        CHECK(strstr(sp, "模组 IPv6 (PDP)") != NULL);
        CHECK(strstr(sp, "2409:xxxx:xxxx:xxxx::1") != NULL);
        CHECK(strstr(sp, "公网 IPv4 出口") != NULL);
        CHECK(strstr(sp, "203.0.113.7") != NULL);
        CHECK(strstr(sp, "IPV4V6") != NULL);
        CHECK(strstr(sp, "短信远程配置") != NULL);          /* 只读卡片 */
        CHECK(strstr(sp, "13800000000") != NULL);
        CHECK(strstr(sp, "报警") != NULL);
        CHECK(strstr(sp, "action=\"/action/ipv6_off\"") != NULL);  /* 已开启 -> 显示关闭按钮 */
        CHECK(strstr(sp, "action=\"/action/pubip_refresh\"") != NULL);
        /* 回显服务看到的是 v6 出口时（DNS 默认 v6 优先的坑），必须单独显示、不冒充 IPv4 */
        {
            web_status_t st6 = st;
            static char  sp6[WEB_UI_STATUS_PAGE_MAX];
            size_t       r6;
            (void)snprintf(st6.pub_ip6, sizeof(st6.pub_ip6), "2409:xxxx:xxxx:xxxx::1");
            st6.pub_ip[0] = '\0';
            r6 = web_ui_status_page(sp6, sizeof(sp6), &st6, &cfg, rows, 2u);
            CHECK(r6 > 0u);
            CHECK(strstr(sp6, "公网 IPv6 出口") != NULL);
            CHECK(strstr(sp6, "2409:xxxx:xxxx:xxxx::1") != NULL);
            CHECK(strstr(sp6, "公网 IPv4 出口") == NULL || strstr(sp6, "未取到") != NULL);
        }
        CHECK(strstr(sp, "name=\"ipv6_enable\"") != NULL);   /* 配置表单里的开关 */
        CHECK(strstr(sp, "name=\"pubip_url\"") != NULL);
        CHECK(strstr(sp, "</html>") != NULL);
    }

    g_case = "未开启 IPv6 / 未查询公网 IP 时的提示文案";
    {
        static char  sp[WEB_UI_STATUS_PAGE_MAX];
        web_status_t st2;
        size_t       r;

        st2 = st;
        st2.ip6[0] = '\0';
        st2.ipv6_enable = 0;
        st2.pub_ip[0] = '\0';
        st2.pubip_tried = 0;
        r = web_ui_status_page(sp, sizeof(sp), &st2, &cfg, rows, 2u);
        CHECK(r > 0u);
        CHECK(strstr(sp, "未开启（PDP 为纯 IPv4）") != NULL);
        CHECK(strstr(sp, "尚未查询") != NULL);
        CHECK(strstr(sp, "action=\"/action/ipv6_on\"") != NULL);   /* 未开启 -> 显示开启按钮 */
    }

    g_case = "回归：登录页必须能完整渲染进 WEB_UI_LOGIN_PAGE_MAX";
    {
        static char lp[WEB_UI_LOGIN_PAGE_MAX];
        size_t      r = web_ui_login_page(lp, sizeof(lp), 0);

        CHECK(r > 0u);
        CHECK(r < sizeof(lp));
        if (strstr(lp, "name=\"password\"") == NULL || strstr(lp, "</html>") == NULL) {
            ++g_fail;
            printf("  [FAIL] 登录页被截断：长度 %u / 上限 %d，"
                   "含 <form>=%d 含 </html>=%d\n",
                   (unsigned)r, (int)WEB_UI_LOGIN_PAGE_MAX,
                   strstr(lp, "<form") != NULL, strstr(lp, "</html>") != NULL);
        } else {
            ++g_pass;
        }
    }

    g_case = "回归：状态页（名单全满 + 16 条长短信）必须完整渲染";
    {
        static char      sp[WEB_UI_STATUS_PAGE_MAX];
        static web_sms_row_t big_rows[SMS_RECORD_NUM];
        static char      long_text[SMS_RECORD_TEXT_MAX];
        app_config_t     c2;
        size_t           r;
        size_t           k;

        app_config_defaults(&c2);
        for (k = 0; k < APP_CFG_LIST_MAX; ++k) {
            memset(c2.sms_white[k], '1', APP_CFG_NUM_MAX - 1u);
            c2.sms_white[k][APP_CFG_NUM_MAX - 1u] = '\0';
            memset(c2.sms_black[k], '2', APP_CFG_NUM_MAX - 1u);
            c2.sms_black[k][APP_CFG_NUM_MAX - 1u] = '\0';
        }
        c2.white_count = APP_CFG_LIST_MAX;
        c2.black_count = APP_CFG_LIST_MAX;
        memset(c2.webhook_url, 'u', sizeof(c2.webhook_url) - 1u);
        c2.webhook_url[sizeof(c2.webhook_url) - 1u] = '\0';

        memset(long_text, 'A', sizeof(long_text) - 1u);
        long_text[sizeof(long_text) - 1u] = '\0';
        for (k = 0; k < SMS_RECORD_NUM; ++k) {
            memset(big_rows[k].number, '9', sizeof(big_rows[k].number) - 1u);
            big_rows[k].number[sizeof(big_rows[k].number) - 1u] = '\0';
            big_rows[k].time[0] = '\0';
            big_rows[k].text = long_text;
        }

        st.directional_ok = 1;
        st.over_limit = 1;
        r = web_ui_status_page(sp, sizeof(sp), &st, &c2, big_rows, SMS_RECORD_NUM);
        CHECK(r > 0u);
        CHECK(r < sizeof(sp));
        g_case = "回归：状态页完整（含 </html>）";
        CHECK(strstr(sp, "</html>") != NULL);
    }

    g_case = "导出接口可用且内容长度自洽";
    {
        static char exp[WEB_UI_EXPORT_MAX];
        size_t r = web_ui_export_txt(exp, sizeof(exp), rows, 2u);
        CHECK(r > 0u);
        CHECK(exp[r] == '\0');
        CHECK(strstr(exp, "Content-Length:") != NULL);
    }
}

/* ================================================================== */
/* 6. sms_cmd / sms_rcfg：短信远程配置（模块 3 / 模块 5）               */
/* ================================================================== */
static void test_sms_rcfg(void)
{
    sms_remote_cfg_t c, d;
    uint8_t blob[SMS_RCFG_BLOB_SIZE];
    int n, r;

    printf("[6] sms_rcfg 远程配置持久化（模块 5）\n");

    g_case = "尺寸契约：结构体与尺寸宏同步";
    CHECK(sizeof(sms_remote_cfg_t) == SMS_RCFG_CFG_SIZE);
    CHECK(SMS_RCFG_BLOB_SIZE == SMS_RCFG_CFG_SIZE + 12u);   /* 4+2+2 头 + 4 校验 */

    g_case = "默认值";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    CHECK_STR(c.tmpl, "{from}|{time}|{text}");
    CHECK_STR(c.ntp, "ntp.aliyun.com");
    CHECK(c.concat_enable == 1u);
    CHECK(c.url[0] == '\0' && c.secret[0] == '\0' && c.keyword[0] == '\0');

    g_case = "序列化尺寸与过小缓冲拒绝";
    n = sms_rcfg_serialize(&c, blob, sizeof(blob));
    CHECK(n == (int)SMS_RCFG_BLOB_SIZE);
    {
        uint8_t small[SMS_RCFG_BLOB_SIZE - 1u];            /* 曾经的验证夹具就栽在 512 */
        CHECK(sms_rcfg_serialize(&c, small, sizeof(small)) < 0);
    }

    g_case = "序列化往返一致";
    strcpy(c.url, "https://oapi.dingtalk.com/robot/send?access_token=abc");
    strcpy(c.secret, "SECabcdef");
    strcpy(c.keyword, "报警");
    strcpy(c.tmpl, "{from}:{text}");
    c.concat_enable = 0u;
    n = sms_rcfg_serialize(&c, blob, sizeof(blob));
    CHECK(n == (int)SMS_RCFG_BLOB_SIZE);
    CHECK(sms_rcfg_deserialize(blob, (size_t)n, &d) == 0);
    CHECK_STR(d.url, c.url);
    CHECK_STR(d.secret, c.secret);
    CHECK_STR(d.keyword, c.keyword);
    CHECK_STR(d.tmpl, c.tmpl);
    CHECK(d.concat_enable == 0u);

    g_case = "损坏检测（magic / 校验和 / 长度）";
    {
        uint8_t b2[SMS_RCFG_BLOB_SIZE];
        memcpy(b2, blob, sizeof(blob));
        b2[0] ^= 0xFFu;
        CHECK(sms_rcfg_deserialize(b2, sizeof(b2), &d) < 0);
    }
    {
        uint8_t b2[SMS_RCFG_BLOB_SIZE];
        memcpy(b2, blob, sizeof(blob));
        b2[SMS_RCFG_BLOB_SIZE - 1u] ^= 0x01u;
        CHECK(sms_rcfg_deserialize(b2, sizeof(b2), &d) < 0);
    }
    CHECK(sms_rcfg_deserialize(blob, 4u, &d) < 0);
    CHECK(sms_rcfg_deserialize(blob, sizeof(blob) - 1u, &d) < 0);

    g_case = "异常一律回退默认值（不把残留数据当配置）";
    strcpy(d.url, "残留数据");
    CHECK(sms_rcfg_restore(blob, n, &d) == 1);              /* 好数据 -> 恢复 */
    CHECK_STR(d.url, c.url);
    strcpy(d.url, "残留数据");
    CHECK(sms_rcfg_restore(blob, -2, &d) == 0);              /* 文件不存在 */
    CHECK_STR(d.url, "");
    CHECK_STR(d.admin, "");   /* 回退默认：管理员为空，待认领 */
    strcpy(d.url, "残留数据");
    CHECK(sms_rcfg_restore(blob, n - 1, &d) == 0);           /* 掉电半截写 */
    CHECK_STR(d.url, "");
    CHECK(sms_rcfg_restore(NULL, 0, &d) == 0);
    CHECK(sms_rcfg_restore(blob, n, NULL) < 0);

    g_case = "指令应用（与模块 3 联动）";
    {
        sms_cmd_set_t s;
        int i, changed = 0;

        sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
        n = sms_cmd_parse("U https://oapi.dingtalk.com/robot/send?access_token=zzz\n"
                          "T SEC999\nK 报警\nM {from}:{text}\nSAVE", &s);
        CHECK(n == 5);
        for (i = 0; i < n; ++i) {
            r = sms_rcfg_apply(&c, s.kind[i], s.arg[i]);
            if (r > 0) ++changed;
        }
        CHECK(changed == 4);                                /* SAVE 不修改配置 */
        CHECK_STR(c.url, "https://oapi.dingtalk.com/robot/send?access_token=zzz");
        CHECK_STR(c.secret, "SEC999");
        CHECK_STR(c.keyword, "报警");
        CHECK_STR(c.tmpl, "{from}:{text}");

        g_case = "清空 / 恢复默认 / 非法值";
        CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_KEYWORD, "-") > 0);
        CHECK_STR(c.keyword, "");
        CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_TEMPLATE, "-") > 0);
        CHECK_STR(c.tmpl, "{from}|{time}|{text}");
        CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_URL, "-") > 0);
        CHECK_STR(c.url, "");
        CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_KEYWORD, "") < 0);
        CHECK(sms_rcfg_apply(&c, SMS_CMD_STATUS, NULL) == 0);
        CHECK(sms_rcfg_apply(NULL, SMS_CMD_SAVE, NULL) < 0);
        {
            char big[SMS_RCFG_KEYWORD_MAX + 4u];
            memset(big, 'x', sizeof(big) - 1u);
            big[sizeof(big) - 1u] = '\0';
            CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_KEYWORD, big) < 0);   /* 超长拒收 */
            CHECK_STR(c.keyword, "");
        }
        {
            char edge[SMS_RCFG_KEYWORD_MAX];                /* 恰好 max-1 字符应放行 */
            memset(edge, 'y', sizeof(edge) - 1u);
            edge[sizeof(edge) - 1u] = '\0';
            CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_KEYWORD, edge) > 0);
            CHECK(strlen(c.keyword) == SMS_RCFG_KEYWORD_MAX - 1u);
        }
    }

    g_case = "菜单渲染（完整模板 + 状态，≤3 条独立短信）";
    {
        sms_cfg_view_t v;
        char txt[SMS_CMD_REPLY_MAX];
        sms_tx_pkt_t pk[SMS_TX_MAX_PARTS];
        int k;
        sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
        strcpy(c.url, "https://oapi.dingtalk.com/robot/send?access_token=x");
        strcpy(c.secret, "SECx");
        sms_rcfg_make_view(&c, "已注册", "31", "已对时",
                           12u, 11u, &v);
        n = sms_cmd_build_menu(&v, txt, sizeof(txt));
        CHECK(n > 0);
        CHECK(strstr(txt, "U网址") != NULL);
        CHECK(strstr(txt, "SAVE STATUS IP 面板") != NULL);
        CHECK(strstr(txt, "M模板") != NULL);
        CHECK(strstr(txt, "收12/转11") == NULL);   /* 菜单只放指令；状态走 STATUS */
        /* 关键约束：切成"每片 ≤70 字、彼此独立"的短信，总条数 ≤3 */
        k = sms_concat_build_tx_plain("+8613800738500", "+8613800000000", txt, pk, SMS_TX_MAX_PARTS);
        CHECK(k == 1);                              /* 一条装完，绝不依赖连发 */
        /* STATUS 单独成条时也必须 ≤1 条 */
        n = sms_cmd_build_status(&v, txt, sizeof(txt));
        CHECK(n > 0);
        CHECK(strstr(txt, "网:已注册") != NULL);
        CHECK(strstr(txt, "M:已设") != NULL);   /* 模板只显示已设/未设，原文见面板/详情 */
        CHECK(strstr(txt, "收12/转11") != NULL);
        k = sms_concat_build_tx_plain("+8613800738500", "+8613800000000", txt, pk, SMS_TX_MAX_PARTS);
        CHECK(k == 1);
        r = sms_cmd_build_ack(SMS_CMD_SAVE, NULL, &v, txt, sizeof(txt));
        CHECK(r > 0);
        CHECK_STR(txt, "OK SAVED");
    }

    g_case = "落盘：save -> load 一致";
    g_fake_fs_len = -1;
    g_fake_fs_writes = 0;
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    strcpy(c.url, "https://oapi.dingtalk.com/robot/send?access_token=disk");
    strcpy(c.secret, "SECdisk");
    strcpy(c.keyword, "磁盘");
    strcpy(c.admin, "13800138000");
    CHECK(sms_rcfg_save(&c) == 0);
    CHECK(g_fake_fs_writes == 1);
    CHECK(g_fake_fs_len == (int32_t)SMS_RCFG_BLOB_SIZE);     /* 落盘就是 576 字节 */
    CHECK(sms_rcfg_load(&d) == 1);
    CHECK_STR(d.url, c.url);
    CHECK_STR(d.secret, c.secret);
    CHECK_STR(d.keyword, c.keyword);
    CHECK_STR(d.admin, c.admin);

    g_case = "落盘：文件不存在 -> 默认值";
    g_fake_fs_len = -1;
    CHECK(sms_rcfg_load(&d) == 0);
    CHECK_STR(d.admin, "");   /* 回退默认：管理员为空，待认领 */
    CHECK_STR(d.url, "");
    CHECK(d.concat_enable == 1u);

    g_case = "落盘：掉电半截写 / 内容损坏 -> 默认值（不崩）";
    g_fake_fs_len = (int32_t)(SMS_RCFG_BLOB_SIZE / 2u);      /* 半截文件 */
    CHECK(sms_rcfg_load(&d) == 0);
    CHECK_STR(d.admin, "");   /* 回退默认：管理员为空，待认领 */
    g_fake_fs[10] ^= 0xFFu;                                  /* 内容损坏（校验和挡下） */
    g_fake_fs_len = (int32_t)SMS_RCFG_BLOB_SIZE;
    CHECK(sms_rcfg_load(&d) == 0);
    CHECK_STR(d.admin, "");   /* 回退默认：管理员为空，待认领 */

    g_case = "落盘：参数校验";
    CHECK(sms_rcfg_save(NULL) < 0);
    CHECK(sms_rcfg_load(NULL) < 0);
}

/* ================================================================== */
/* 7. sms_admin：管理员指令路由（集成层，纯逻辑）                        */
/* ================================================================== */
static void test_sms_admin(void)
{
    sms_remote_cfg_t c;
    sms_admin_out_t  out;
    sms_cfg_view_t   v;
    int              rc;

    printf("[7] sms_admin 管理员指令路由\n");

    memset(&v, 0, sizeof(v));
    v.net        = "已注册 CHINA MOBILE";
    v.signal     = "31";
    v.ntp        = "已对时 2026-10-15 10:20:31";
    v.recv_count = 12u;
    v.fwd_count  = 11u;

    g_case = "非管理员：只转发，改不动配置";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    rc = sms_admin_handle(&c, "13800138000",
                          "U https://evil.example/x\nT SECevil\nSAVE\nRESET", &v, &out);
    CHECK(rc == 0);
    CHECK(out.is_admin == 0);
    CHECK(out.changed == 0);
    CHECK(out.need_save == 0);
    CHECK(out.need_reboot == 0);
    CHECK(out.reply_len == 0);
    CHECK_STR(c.url, "");
    CHECK_STR(c.secret, "");

    g_case = "管理员「我要配置」-> 单条指令菜单";
    sms_rcfg_defaults(&c);
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    v.url = c.url; v.secret = c.secret; v.keyword = c.keyword; v.tmpl = c.tmpl;
    rc = sms_admin_handle(&c, "+8613800000000", "我要配置", &v, &out);
    CHECK(rc == 1);
    CHECK(out.reply_len > 0);
    CHECK((size_t)out.reply_len == strlen(out.reply));
    CHECK(strstr(out.reply, "SAVE STATUS IP 面板") != NULL);
    CHECK(strstr(out.reply, "U网址") != NULL);
    CHECK(strstr(out.reply, "网:") == NULL);      /* 状态走 STATUS，菜单保持单条 */
    CHECK(out.changed == 0 && out.need_save == 0 && out.need_reboot == 0);

    g_case = "管理员多行回填：4 条改动 + SAVE 标志 + 合并回执";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    rc = sms_admin_handle(&c, "13800000000",
                          "U https://oapi.dingtalk.com/robot/send?access_token=zzz\n"
                          "T SEC999\nK 报警\nM {from}|{time}|{text}\nSAVE", &v, &out);
    CHECK(rc == 1);
    CHECK(out.is_admin == 1);
    CHECK(out.changed == 1);
    CHECK(out.need_save == 1);
    CHECK(out.need_reboot == 0);
    CHECK_STR(c.url, "https://oapi.dingtalk.com/robot/send?access_token=zzz");
    CHECK_STR(c.secret, "SEC999");
    CHECK_STR(c.keyword, "报警");
    CHECK(strstr(out.reply, "OK U") != NULL);
    CHECK(strstr(out.reply, "OK T") != NULL);
    CHECK(strstr(out.reply, "OK K") != NULL);
    CHECK(strstr(out.reply, "OK M") != NULL);
    CHECK(strstr(out.reply, "OK SAVED") != NULL);

    g_case = "STATUS / RESET / 非法指令";
    rc = sms_admin_handle(&c, "13800000000", "STATUS", &v, &out);
    CHECK(rc == 1);
    CHECK(strstr(out.reply, "网:") != NULL);
    CHECK(out.need_save == 0 && out.need_reboot == 0);
    rc = sms_admin_handle(&c, "13800000000", "RESET", &v, &out);
    CHECK(rc == 1);
    CHECK(out.need_reboot == 1);
    CHECK(out.changed == 0);
    rc = sms_admin_handle(&c, "13800000000", "HELLO WORLD", &v, &out);
    CHECK(rc == 1);
    CHECK(out.changed == 0);
    CHECK(strstr(out.reply, "ERR") != NULL);
    rc = sms_admin_handle(&c, "13800000000", "", &v, &out);
    CHECK(rc == 1);                       /* 管理员但无可执行指令 */
    CHECK(strstr(out.reply, "ERR") != NULL);

    g_case = "空值/超长参数：回执报错但不改配置";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    rc = sms_admin_handle(&c, "13800000000", "K \n", &v, &out);
    CHECK(rc == 1);
    CHECK(out.changed == 0);
    CHECK(strstr(out.reply, "ERR K") != NULL);

    g_case = "回执超长：截断而不越界";
    {
        char   many[SMS_CMD_MAX * 16u];
        size_t o = 0u;
        int    i;
        for (i = 0; i < (int)SMS_CMD_MAX; ++i) {
            const char *s = "我要配置\n";
            size_t      l = strlen(s);
            memcpy(&many[o], s, l);
            o += l;
        }
        many[o] = '\0';
        rc = sms_admin_handle(&c, "13800000000", many, &v, &out);
        CHECK(rc == 1);
        CHECK(out.reply_len > 0);
        CHECK(out.reply_len < (int)SMS_CMD_REPLY_MAX);
        CHECK((size_t)out.reply_len == strlen(out.reply));
        CHECK(out.reply[(size_t)out.reply_len] == '\0');
    }

    g_case = "参数校验：rcfg/out 为空不崩";
    CHECK(sms_admin_handle(NULL, "13800000000", "STATUS", &v, &out) == 0);
    CHECK(sms_admin_handle(&c, "13800000000", "STATUS", &v, NULL) == 0);
    CHECK(sms_admin_handle(&c, NULL, "STATUS", &v, &out) == 0);
}

/* ================================================================== */
/* 8. 短信中心（AT+CSCA? 应答解析）                                     */
/* ================================================================== */
static void test_csca(void)
{
    char sca[24];
    char tiny[4];

    printf("[8] 短信中心 AT+CSCA? 解析\n");

    g_case = "国际号码（含引号 + 类型 145）";
    CHECK(sms_concat_parse_csca("+CSCA: \"+8613800738500\",145\r\nOK", sca, sizeof(sca)) == 0);
    CHECK_STR(sca, "+8613800738500");

    g_case = "国内号码（无引号 + 类型 129）";
    CHECK(sms_concat_parse_csca("+CSCA: 13800738500,129", sca, sizeof(sca)) == 0);
    CHECK_STR(sca, "13800738500");

    g_case = "空号码 = 用模组默认，按失败处理";
    CHECK(sms_concat_parse_csca("+CSCA: \"\",145\r\nOK", sca, sizeof(sca)) < 0);
    CHECK_STR(sca, "");

    g_case = "异常输入";
    CHECK(sms_concat_parse_csca("ERROR", sca, sizeof(sca)) < 0);
    CHECK(sms_concat_parse_csca(NULL, sca, sizeof(sca)) < 0);
    CHECK_STR(sca, "");
    CHECK(sms_concat_parse_csca("+CSCA: \"+8613800738500\",145", NULL, 0u) < 0);

    g_case = "缓冲过小时拒绝且保证以 '\\0' 结尾";
    CHECK(sms_concat_parse_csca("+CSCA: \"+8613800738500\",145", tiny, sizeof(tiny)) < 0);
    CHECK(tiny[sizeof(tiny) - 1u] == '\0');
}

/* ================================================================== */
/* 9. webhook 转发组装（模板 -> 关键词 -> JSON -> 加签 URL）             */
/* ================================================================== */
static void test_webhook_forward(void)
{
    app_config_t     cfg;
    sms_remote_cfg_t rc;
    webhook_req_t    req;

    printf("[9] webhook 转发组装（远程配置 / 关键词 / 加签 / 降级）\n");

    app_config_defaults(&cfg);
    cfg.webhook_enable = 1u;
    cfg.webhook_type   = (uint8_t)APP_WEBHOOK_DINGTALK;
    (void)snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "%s",
                   "https://oapi.dingtalk.com/robot/send?access_token=CFG");

    g_case = "参数校验";
    CHECK(webhook_build_forward(NULL, NULL, "1", "t", "x", NULL, 1u, 0u, 0, &req) == -1);
    CHECK(webhook_build_forward(&cfg, NULL, "1", "t", "x", NULL, 1u, 0u, 0, NULL) == -1);

    g_case = "任何来源都没有 URL -> -2";
    {
        app_config_t     c2 = cfg;
        sms_remote_cfg_t r2;
        c2.webhook_url[0] = '\0';
        sms_rcfg_defaults(&r2);
        CHECK(webhook_build_forward(&c2, &r2, "138", "t", "x", NULL, 1u, 0u, 0, &req) == -2);
    }

    g_case = "远程配置优先 + 模板渲染 + 关键词注入 + 加签";
    sms_rcfg_defaults(&rc);
    (void)snprintf(rc.url, sizeof(rc.url), "%s",
                   "https://oapi.dingtalk.com/robot/send?access_token=RMT");
    (void)snprintf(rc.tmpl, sizeof(rc.tmpl), "%s", "来自{from}的{text}#{idx}");
    (void)snprintf(rc.keyword, sizeof(rc.keyword), "%s", "报警");
    (void)snprintf(rc.secret, sizeof(rc.secret), "%s", "SECabc");
    CHECK(webhook_build_forward(&cfg, &rc, "13800138000", "2026-10-15 10:20:31",
                                "有人闯入", "00FF", 7u, 1792030831123ULL, 1, &req) == 0);
    CHECK(strstr(req.url, "access_token=RMT") != NULL);          /* 短信里设的 URL 生效 */
    CHECK(strstr(req.url, "timestamp=1792030831123") != NULL);
    CHECK(strstr(req.url, "sign=") != NULL);
    CHECK(req.signed_ok == 1);
    CHECK(req.sign_downgraded == 0);
    CHECK(req.payload_len == strlen(req.payload));
    CHECK(strstr(req.payload, "【报警】") != NULL);                /* 关键词前缀 */
    CHECK(strstr(req.payload, "来自13800138000的有人闯入#7") != NULL);
    CHECK(strstr(req.payload, "\"msgtype\":\"markdown\"") != NULL);
    CHECK(strstr(req.payload, "\"title\"") != NULL);              /* 钉钉结构：title + text */

    g_case = "加签值必须是 URL 编码过的 base64（不含裸 + / =）";
    {
        const char *sp = strstr(req.url, "sign=");
        CHECK(sp != NULL);
        if (sp != NULL) {
            sp += 5;
            CHECK(strchr(sp, '+') == NULL);
            CHECK(strchr(sp, '/') == NULL);
            CHECK(strchr(sp, '=') == NULL);
            CHECK(strstr(sp, "%2B") != NULL || strstr(sp, "%2F") != NULL ||
                  strstr(sp, "%3D") != NULL);   /* 末尾补位 '=' 必被编码 */
        }
    }

    g_case = "有密钥但时钟不可信 -> 降级为不加签";
    CHECK(webhook_build_forward(&cfg, &rc, "138", "t", "x", NULL, 1u, 0u, 0, &req) == 0);
    CHECK(req.signed_ok == 0);
    CHECK(req.sign_downgraded == 1);
    CHECK(strstr(req.url, "timestamp=") == NULL);
    CHECK(strstr(req.url, "access_token=RMT") != NULL);           /* URL 本身仍可用 */

    g_case = "远程配置为空 -> 回落 app_config 的 URL，且不加关键词/不加签";
    {
        sms_remote_cfg_t r3;
        sms_rcfg_defaults(&r3);                                   /* url/secret/keyword 全空 */
        (void)snprintf(r3.tmpl, sizeof(r3.tmpl), "%s", "{from}|{text}");
        CHECK(webhook_build_forward(&cfg, &r3, "139", "tt", "hello", NULL, 3u, 0u, 0, &req) == 0);
        CHECK(strstr(req.url, "access_token=CFG") != NULL);
        CHECK(strstr(req.url, "timestamp=") == NULL);
        CHECK(strstr(req.payload, "139|hello") != NULL);
        CHECK(strstr(req.payload, "【") == NULL);
    }

    g_case = "企业微信：显式类型与按 URL 自动识别都走 content 字段";
    {
        app_config_t c3 = cfg;
        (void)snprintf(c3.webhook_url, sizeof(c3.webhook_url), "%s",
                       "https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=K");
        /* 自动识别（webhook_type=AUTO 时才按 URL 判断） */
        c3.webhook_type = (uint8_t)APP_WEBHOOK_AUTO;
        CHECK(webhook_detect_type(c3.webhook_url) == APP_WEBHOOK_WECOM);
        CHECK(webhook_build_forward(&c3, NULL, "139", "t", "hi", NULL, 1u, 0u, 0, &req) == 0);
        CHECK(strstr(req.payload, "\"content\"") != NULL);
        CHECK(strstr(req.payload, "\"title\"") == NULL);
        /* 显式指定为钉钉时，即使 URL 是企业微信也按钉钉结构组装（配置优先） */
        c3.webhook_type = (uint8_t)APP_WEBHOOK_DINGTALK;
        CHECK(webhook_build_forward(&c3, NULL, "139", "t", "hi", NULL, 1u, 0u, 0, &req) == 0);
        CHECK(strstr(req.payload, "\"title\"") != NULL);
    }

    g_case = "{pdu} 占位符与超长正文";
    {
        sms_remote_cfg_t r4;
        sms_rcfg_defaults(&r4);
        (void)snprintf(r4.url, sizeof(r4.url), "%s", "https://oapi.dingtalk.com/robot/send?x=1");
        (void)snprintf(r4.tmpl, sizeof(r4.tmpl), "%s", "PDU={pdu}");
        CHECK(webhook_build_forward(&cfg, &r4, "1", "t", "x", "00AB", 1u, 0u, 0, &req) == 0);
        CHECK(strstr(req.payload, "PDU=00AB") != NULL);
    }
    {
        static char big[4000];
        sms_remote_cfg_t r5;
        memset(big, 'A', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        sms_rcfg_defaults(&r5);
        (void)snprintf(r5.url, sizeof(r5.url), "%s", "https://oapi.dingtalk.com/robot/send?x=1");
        CHECK(webhook_build_forward(&cfg, &r5, "1", "t", big, NULL, 1u, 0u, 0, &req) < 0);
    }
}

/* ================================================================== */
/* 10. app_time：时间解析与可信判定                                     */
/* ================================================================== */
static void test_app_time(void)
{
    int64_t t = 0;

    printf("[10] app_time 时间解析（钉钉加签前提）\n");

    g_case = "历法换算锚点（与 Python datetime 独立计算一致）";
    CHECK(app_time_utc_from_fields(1970, 1, 1, 0, 0, 0) == 0LL);
    CHECK(app_time_utc_from_fields(2000, 1, 1, 0, 0, 0) == 946684800LL);
    CHECK(app_time_utc_from_fields(2024, 2, 29, 0, 0, 0) == 1709164800LL);   /* 闰日 */
    CHECK(app_time_utc_from_fields(2038, 1, 19, 0, 0, 0) == 2147472000LL);
    CHECK(app_time_utc_from_fields(2026, 10, 15, 0, 0, 0) == 1792022400LL);
    /* 2100 不是闰年：2/28 的次日必须是 3/1 */
    CHECK(app_time_utc_from_fields(2100, 2, 28, 0, 0, 0) == 4107456000LL);
    CHECK(app_time_utc_from_fields(2100, 3, 1, 0, 0, 0) == 4107542400LL);
    CHECK(app_time_utc_from_fields(2100, 3, 1, 0, 0, 0) -
          app_time_utc_from_fields(2100, 2, 28, 0, 0, 0) == 86400LL);

    g_case = "ISO-8601（NTP 回调格式）";
    CHECK(app_time_parse_iso8601("2026-10-15T10:20:31+08:00", &t) == 0 && t == 1792030831LL);
    CHECK(app_time_parse_iso8601("2026-10-15T02:20:31Z", &t) == 0 && t == 1792030831LL);
    CHECK(app_time_parse_iso8601("2021-04-21T17:10:08.868+08:00", &t) == 0 && t == 1618996208LL);
    CHECK(app_time_parse_iso8601("2026-10-15 02:20:31", &t) == 0 && t == 1792030831LL);
    CHECK(app_time_parse_iso8601("2026-10-15T02:20:31-05:30", &t) == 0 &&
          t == 1792030831LL + 5LL * 3600LL + 30LL * 60LL);
    CHECK(app_time_parse_iso8601("2026-13-01T00:00:00Z", &t) < 0);       /* 月份越界 */
    CHECK(app_time_parse_iso8601("garbage", &t) < 0);
    CHECK(app_time_parse_iso8601(NULL, &t) < 0);

    g_case = "AT+CCLK?（模组时钟）";
    CHECK(app_time_parse_cclk("+CCLK: \"26/10/15,10:20:31+32\"\r\nOK", &t) == 0 &&
          t == 1792030831LL);                                            /* +32 = UTC+8 */
    CHECK(app_time_parse_cclk("+CCLK: \"2026/10/15,02:20:31+00\"", &t) == 0 &&
          t == 1792030831LL);
    CHECK(app_time_parse_cclk("+CCLK: \"26/10/15,02:20:31\"", &t) == 0 &&
          t == 1792030831LL);                                            /* 无时区按 UTC */
    CHECK(app_time_parse_cclk("ERROR", &t) < 0);
    CHECK(app_time_parse_cclk(NULL, &t) < 0);

    g_case = "时钟可信判定（决定加签还是降级）";
    CHECK(app_time_is_valid(1792030831LL) == 1);
    CHECK(app_time_is_valid(0) == 0);
    CHECK(app_time_is_valid(APP_TIME_MIN_VALID - 1) == 0);
    CHECK(app_time_is_valid(APP_TIME_MIN_VALID) == 1);
    CHECK(app_time_is_valid(APP_TIME_MAX_VALID + 1) == 0);

    g_case = "毫秒时间戳";
    CHECK(app_time_to_ts_ms(1792030831LL, 12345u) == 1792030831345ULL);
    CHECK(app_time_to_ts_ms(0, 12345u) == 0ULL);                        /* 时钟无效 -> 0 */
}

/* ================================================================== */
/* 11. net_addr：IPv4/IPv6 判定、CGPADDR 双栈解析、公网回显提取          */
/* ================================================================== */
static void test_net_addr(void)
{
    char v4[NET_ADDR_V4_MAX];
    char v6[NET_ADDR_V6_MAX];
    char out[NET_ADDR_MAX];

    printf("[11] net_addr 地址解析（IPv6 双栈 / 公网 IP）\n");

    g_case = "IPv4 判定";
    CHECK(net_addr_is_ipv4("10.11.12.13") == 1);
    CHECK(net_addr_is_ipv4("0.0.0.0") == 1);
    CHECK(net_addr_is_ipv4("255.255.255.255") == 1);
    CHECK(net_addr_is_ipv4("256.1.1.1") == 0);
    CHECK(net_addr_is_ipv4("10.11.12") == 0);
    CHECK(net_addr_is_ipv4("10.11.12.13.14") == 0);
    CHECK(net_addr_is_ipv4("10.11.12.") == 0);
    CHECK(net_addr_is_ipv4("10.11.12.a") == 0);
    CHECK(net_addr_is_ipv4("") == 0);
    CHECK(net_addr_is_ipv4(NULL) == 0);

    g_case = "IPv6 判定";
    CHECK(net_addr_is_ipv6("2409:xxxx:xxxx:xxxx::1") == 1);
    CHECK(net_addr_is_ipv6("::1") == 1);
    CHECK(net_addr_is_ipv6("fe80::1") == 1);
    CHECK(net_addr_is_ipv6("2409:xxxx:xxxx:xxxx::1") == 1);  /* 8 组 */
    CHECK(net_addr_is_ipv6("::ffff:192.168.0.1") == 1);                        /* 内嵌 IPv4 */
    CHECK(net_addr_is_ipv6("2409:xxxx:xxxx:xxxx::1") == 0);                           /* 两个 :: */
    CHECK(net_addr_is_ipv6("2409:xxxx:xxxx:xxxx::1") == 0); /* 9 组 */
    CHECK(net_addr_is_ipv6("10.11.12.13") == 0);                               /* 纯 v4 */
    CHECK(net_addr_is_ipv6("2409:xxxx:xxxx:xxxx::1") == 0);                        /* 组内超 4 位 */
    CHECK(net_addr_is_ipv6("2409:8a00::zz") == 0);
    CHECK(net_addr_is_ipv6(NULL) == 0);

    g_case = "AT+CGPADDR 双栈解析";
    CHECK(net_addr_parse_cgpaddr("\r\n+CGPADDR: 1,\"10.20.30.40\",\"2409:8a00::1\"\r\nOK",
                                 v4, sizeof(v4), v6, sizeof(v6)) == 0);
    CHECK_STR(v4, "10.20.30.40");
    CHECK_STR(v6, "2409:8a00::1");

    g_case = "纯 IPv4 应答";
    CHECK(net_addr_parse_cgpaddr("+CGPADDR: 1,\"10.20.30.40\"", v4, sizeof(v4), v6, sizeof(v6)) == 0);
    CHECK_STR(v4, "10.20.30.40");
    CHECK_STR(v6, "");

    g_case = "纯 IPv6 应答（IPv4 字段为空）";
    CHECK(net_addr_parse_cgpaddr("+CGPADDR: 1,\"\",\"2409:8a00::9\"",
                                 v4, sizeof(v4), v6, sizeof(v6)) == 0);
    CHECK_STR(v4, "");
    CHECK_STR(v6, "2409:8a00::9");

    g_case = "单字段但是 IPv6";
    CHECK(net_addr_parse_cgpaddr("+CGPADDR: 1,\"2409:8a00::7\"",
                                 v4, sizeof(v4), v6, sizeof(v6)) == 0);
    CHECK_STR(v4, "");
    CHECK_STR(v6, "2409:8a00::7");

    g_case = "0.0.0.0 视为未获得地址";
    CHECK(net_addr_parse_cgpaddr("+CGPADDR: 1,\"0.0.0.0\"",
                                 v4, sizeof(v4), v6, sizeof(v6)) < 0);
    CHECK_STR(v4, "");

    g_case = "无 +CGPADDR 行 / 只有 IMSI 之类的编号";
    CHECK(net_addr_parse_cgpaddr("ERROR", v4, sizeof(v4), v6, sizeof(v6)) < 0);
    CHECK(net_addr_parse_cgpaddr(NULL, v4, sizeof(v4), v6, sizeof(v6)) < 0);
    CHECK(net_addr_parse_cgpaddr("+CGPADDR: 1", v4, sizeof(v4), v6, sizeof(v6)) < 0);

    g_case = "公网 IP 回显提取（裸文本 / 带空白 / JSON）";
    CHECK(net_addr_extract_echo("203.0.113.9\n", out, sizeof(out)) == 0);
    CHECK_STR(out, "203.0.113.9");
    CHECK(net_addr_extract_echo("  \r\n  198.51.100.4  ", out, sizeof(out)) == 0);
    CHECK_STR(out, "198.51.100.4");
    CHECK(net_addr_extract_echo("{\"ip\":\"203.0.113.77\"}", out, sizeof(out)) == 0);
    CHECK_STR(out, "203.0.113.77");
    CHECK(net_addr_extract_echo("您的IP是:[2409:xxxx:xxxx:xxxx::1]", out, sizeof(out)) == 0);
    CHECK_STR(out, "2409:xxxx:xxxx:xxxx::1");
    CHECK(net_addr_extract_echo("no address here", out, sizeof(out)) < 0);
    CHECK(net_addr_extract_echo("", out, sizeof(out)) < 0);
    CHECK(net_addr_extract_echo(NULL, out, sizeof(out)) < 0);
}

/* ================================================================== */
/* 12. 独立分片编码（无 UDH，避免"拼接短信收不齐 = 全都看不到"）          */
/* ================================================================== */static void test_plain_tx(void)
{
    sms_tx_pkt_t pkts[SMS_TX_MAX_PARTS];
    char         text[300];
    int          n, i;

    printf("[12] 独立分片发送编码（无 UDH）\n");

    g_case = "单字符正文 -> 1 条";
    n = sms_concat_build_tx_plain("+8613800738500", "+8613800000000", "Hi", pkts, SMS_TX_MAX_PARTS);
    CHECK(n == 1);
    g_case = "首字节必须是 0x01（SUBMIT 且无 UDHI）";
    /* PDU 结构：SCA(08 91 + 7 字节) 之后紧跟首字节 */
    CHECK(strncmp(pkts[0].pdu, "0891683108708305F0", 18) == 0);
    CHECK(strncmp(pkts[0].pdu + 18, "01", 2) == 0);
    g_case = "不得出现 UDH 拼接头（05 00 03）";
    CHECK(strstr(pkts[0].pdu, "050003") == NULL);

    g_case = "长正文切成多条，每条都是完整短信";
    memset(text, 0, sizeof(text));
    /* 100 个中文字符 -> 按 70 字/条 应为 2 条 */
    for (i = 0; i < 100; ++i) {
        (void)snprintf(text + i * 3, 4, "%s", "中");
    }
    g_case = "100 个汉字 -> 2 条";
    n = sms_concat_build_tx_plain("+8613800738500", "+8613800000000", text, pkts, SMS_TX_MAX_PARTS);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(pkts[0].total == 2u && pkts[0].seq == 1u);
        CHECK(pkts[1].seq == 2u);
        CHECK(strstr(pkts[0].pdu, "050003") == NULL);
        CHECK(strstr(pkts[1].pdu, "050003") == NULL);
        /* UDL 字段：第 1 条应为 140 字节（70 字 × 2） */
        CHECK(strstr(pkts[0].pdu, "088C") != NULL || strstr(pkts[0].pdu, "8C") != NULL);
    }

    g_case = "超过 max_pkts 时报错而不是截断";
    n = sms_concat_build_tx_plain("+8613800738500", "+8613800000000", text, pkts, 1);
    CHECK(n < 0);
    g_case = "参数校验";
    CHECK(sms_concat_build_tx_plain(NULL, "+861", "x", pkts, SMS_TX_MAX_PARTS) == 1);
    CHECK(sms_concat_build_tx_plain("+8613800738500", NULL, "x", pkts, SMS_TX_MAX_PARTS) < 0);
    CHECK(sms_concat_build_tx_plain("+8613800738500", "+861", NULL, pkts, SMS_TX_MAX_PARTS) < 0);
}

/* ================================================================== */
/* 13. 截断 PDU 必须报错（2026-10-15 上板事故回归用例）                  */
/* ================================================================== */
/*
 * 事故：虚拟 AT 分块投递导致只读回 82 个十六进制字符，而 UDL 声称 140 字节。
 * 旧解码器把正文长度"夹到实际字节数"，于是把碎片 "U h" 当成合法短信交给业务层，
 * 结果把 Webhook URL 改成了 "h1"。此用例保证以后这种 PDU 一定被判为截断。
 */
static void test_pdu_trunc(void)
{
    sms_pdu_t p;
    /* 现场抓到的第 1 片（SMS-DELIVER + UDHI + UCS2，UDL=0x8C=140，正文只有 6 字节） */
    static const char *trunc_hex =
        "0891683108708305F0440D91689198920727F0000862015122811223"
        "8C0500030B0201005500200068";
    /* 完整版：补足 134 字节正文（0x8C=140 = UDH 6 + 正文 134），UDL 与数据一致 */
    static char full_hex[420];

    printf("[13] 截断 PDU 判定（回归：碎片被当命令执行过）\n");

    g_case = "截断的 UCS2 长短信分片 -> SMS_PDU_ERR_TRUNC";
    CHECK(sms_pdu_decode(trunc_hex, &p) == SMS_PDU_ERR_TRUNC);

    g_case = "同一 PDU 补足正文后 -> 解码成功";
    {
        size_t i;
        size_t n = strlen(trunc_hex);
        memcpy(full_hex, trunc_hex, n + 1u);
        for (i = 0; i < 128u; ++i) {                 /* 补 128 字节 'A'(0x0041) */
            full_hex[n + i * 4u + 0u] = '0';
            full_hex[n + i * 4u + 1u] = '0';
            full_hex[n + i * 4u + 2u] = '4';
            full_hex[n + i * 4u + 3u] = '1';
        }
        full_hex[n + 128u * 4u] = '\0';
        CHECK(sms_pdu_decode(full_hex, &p) == SMS_PDU_OK);
        if (p.text_len >= 3u) {
            CHECK(p.text[0] == 'U' && p.text[1] == ' ' && p.text[2] == 'h');
        }
        CHECK(p.concat.present == 1u);
        CHECK(p.concat.seq == 1u && p.concat.total == 2u);
        CHECK(p.udhi == 1u);
    }

    g_case = "7bit 声明 160 septet 但数据不足 -> 截断";
    {
        /* SMS-DELIVER、7bit(DCS=0)、UDL=0xA0=160、实际正文只有 2 字节 */
        static const char *t7 =
            "0891683108708305F0040D91689198920727F000000620151228112"
            "2A04142";
        CHECK(sms_pdu_decode(t7, &p) == SMS_PDU_ERR_TRUNC);
    }
}

/* ================================================================== */
/* 14. IP / 详细信息：短信回摘要（一条装下）+ 机器人推完整详情            */
/* ================================================================== */
static void test_info_cmd(void)
{
    sms_remote_cfg_t c;
    sms_cfg_view_t   v;
    sms_admin_out_t  out;
    sms_tx_pkt_t     pk[SMS_TX_MAX_PARTS];
    static char      txt[SMS_CMD_REPLY_MAX];
    int              n;

    printf("[14] IP / 详细信息（短信摘要 + 机器人详情）\n");

    g_case = "解析 IP / INFO / 详细信息";
    {
        sms_cmd_set_t s;
        CHECK(sms_cmd_parse("IP", &s) == 1 && s.kind[0] == SMS_CMD_INFO);
        CHECK(sms_cmd_parse("INFO", &s) == 1 && s.kind[0] == SMS_CMD_INFO);
        CHECK(sms_cmd_parse("详细信息", &s) == 1 && s.kind[0] == SMS_CMD_INFO);
        CHECK(sms_cmd_parse("我要配置", &s) == 1 && s.kind[0] == SMS_CMD_MENU);
    }

    g_case = "回执只含两个地址，且一条短信装得下（含最长 IPv6）";
    memset(&v, 0, sizeof(v));
    v.ip4 = "10.46.199.212";
    v.ip6 = "2409:xxxx:xxxx:xxxx::1";   /* 39 字符全展开 */
    n = sms_cmd_build_info(&v, txt, sizeof(txt));
    CHECK(n > 0);
    CHECK(strstr(txt, "IP4:10.46.199.212") != NULL);
    CHECK(strstr(txt, "IP6:2409:xxxx:xxxx:xxxx::1") != NULL);
    CHECK(sms_concat_build_tx_plain("+8613800738500", "+8613800000000", txt, pk,
                                    SMS_TX_MAX_PARTS) == 1);
    v.ip6 = "2409:xxxx:xxxx:xxxx::1";
    n = sms_cmd_build_info(&v, txt, sizeof(txt));
    CHECK(n > 0);
    CHECK(strstr(txt, "详情已发机器人") != NULL);
    CHECK(sms_concat_build_tx_plain("+8613800738500", "+8613800000000", txt, pk,
                                    SMS_TX_MAX_PARTS) == 1);

    g_case = "管理员发 IP -> 回执 + want_info 标记";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    strcpy(c.admin, "13800000000");
    v.url = c.url; v.secret = c.secret; v.keyword = c.keyword; v.tmpl = c.tmpl;
    v.ip4 = "10.46.199.212";
    v.ip6 = "2409:xxxx:xxxx:xxxx::1";
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613800000000", "IP", &v, &out) == 1);
    CHECK(out.want_info == 1);
    CHECK(out.reply_len > 0);
    CHECK(strstr(out.reply, "IP6:") != NULL);
    CHECK(out.need_save == 0 && out.need_reboot == 0);

    g_case = "非管理员发 IP -> 不执行指令";
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613000000000", "IP", &v, &out) == 0);
    CHECK(out.want_info == 0 && out.reply_len == 0);
}

/* ================================================================== */
/* 15. 面板指令 / 管理员认领与更换 / 全功能菜单                          */
/* ================================================================== */
static void test_admin_claim(void)
{
    sms_remote_cfg_t c;
    sms_cfg_view_t   v;
    sms_admin_out_t  out;
    sms_tx_pkt_t     pk[SMS_TX_MAX_PARTS];
    static char      txt[SMS_CMD_REPLY_MAX];

    printf("[15] 面板 / 管理员认领与更换 / 全功能菜单\n");

    g_case = "解析 面板 / PANEL / A 号码 / 我要成为管理员";
    {
        sms_cmd_set_t s;
        CHECK(sms_cmd_parse("面板", &s) == 1 && s.kind[0] == SMS_CMD_PANEL);
        CHECK(sms_cmd_parse("PANEL", &s) == 1 && s.kind[0] == SMS_CMD_PANEL);
        CHECK(sms_cmd_parse("A 13900000000", &s) == 1 && s.kind[0] == SMS_CMD_SET_ADMIN);
        CHECK_STR(s.arg[0], "13900000000");
        CHECK(sms_cmd_parse("我要成为管理员", &s) == 1 && s.kind[0] == SMS_CMD_CLAIM);
        CHECK(sms_cmd_parse("U http://x", &s) == 1 && s.kind[0] == SMS_CMD_SET_URL);
    }

    g_case = "菜单列出全部功能且一条短信装得下";
    memset(&v, 0, sizeof(v));
    CHECK(sms_cmd_build_menu(&v, txt, sizeof(txt)) > 0);
    CHECK(strstr(txt, "U网址") != NULL && strstr(txt, "SAVE") != NULL);
    CHECK(strstr(txt, "STATUS") != NULL && strstr(txt, "面板") != NULL);
    CHECK(strstr(txt, "A管理员号") != NULL && strstr(txt, "我要成为管理员") != NULL);
    CHECK(sms_concat_build_tx_plain("+8613800738500", "+8613800000000", txt, pk,
                                    SMS_TX_MAX_PARTS) == 1);

    g_case = "首次认领：未认领时任意号码发「我要成为管理员」即成为管理员并落盘";
    sms_rcfg_defaults(&c);
    CHECK(c.admin_claimed == 0u);
    sms_rcfg_make_view(&c, "已注册", "31", "已对时", 0u, 0u, &v);
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613900000000", "我要成为管理员", &v, &out) == 1);
    CHECK(out.is_admin == 1);
    CHECK(out.need_save == 1);
    CHECK_STR(c.admin, "+8613900000000");
    CHECK(c.admin_claimed == 1u);

    g_case = "已认领后：别的号码再发无效";
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613900000001", "我要成为管理员", &v, &out) == 0);
    CHECK(out.need_save == 0);

    g_case = "现任管理员发 A 号码 可更换管理员";
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613900000000", "A 13911112222", &v, &out) == 1);
    CHECK_STR(c.admin, "13911112222");
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613900000000", "STATUS", &v, &out) == 0);   /* 旧号失权 */
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "13911112222", "STATUS", &v, &out) == 1);      /* 新号有权 */

    g_case = "非法管理员号码被拒";
    CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_ADMIN, "") < 0);
    CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_ADMIN, "123") < 0);
    CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_ADMIN, "139abc") < 0);
    CHECK(sms_rcfg_apply(&c, SMS_CMD_SET_ADMIN, "13911112222") == 1);

    g_case = "面板指令 -> want_panel；我要配置 -> want_menu";
    sms_rcfg_defaults(&c);
    t_set_admin(&c, "13800000000");   /* 显式指定管理员：出厂默认为空 */
    t_set_admin(&c, "13800000000");
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613800000000", "面板", &v, &out) == 1);
    CHECK(out.want_panel == 1);
    memset(&out, 0, sizeof(out));
    CHECK(sms_admin_handle(&c, "+8613800000000", "我要配置", &v, &out) == 1);
    CHECK(out.want_menu == 1);
}

/* ================================================================== */
int main(void)
{
    /* 关键：设为无缓冲，否则一旦运行期崩溃（访问违例），
     * 块缓冲的输出会全部丢失，什么都看不到。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("==== ML307A 4G 短信网关 主机单元测试 ====\n\n");

    test_sha256();
    test_config();
    test_sms_pdu();
    test_webhook();
    test_web_ui();
    test_sms_rcfg();
    test_sms_admin();
    test_csca();
    test_webhook_forward();
    test_app_time();
    test_net_addr();
    test_plain_tx();
    test_pdu_trunc();
    test_info_cmd();
    test_admin_claim();
    test_admin_claim();

    printf("\n==== 结果: 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}














