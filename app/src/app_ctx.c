/**
 * @file    app_ctx.c
 * @brief   全局配置上下文实现。
 */
#include "app_ctx.h"
#include "app_log.h"

#include <string.h>

app_config_t g_cfg;
sdk_mutex_t  g_cfg_lock;

/* 静态缓冲区，避免在栈上放 1KB 的结构体 */
static app_config_blob_t s_blob;

int app_ctx_init(void)
{
    int32_t n;
    int     rc;

    if (g_cfg_lock == NULL) {
        g_cfg_lock = sdk_mutex_new();
    }

    app_config_defaults(&g_cfg);

    n = sdk_cfg_load(&s_blob, (uint32_t)sizeof(s_blob));
    if (n <= 0) {
        LOG_W("cfg", "Flash 中无有效配置，写入默认配置（首次上电）");
        (void)app_ctx_save();
        return 1;
    }

    rc = app_config_deserialize(&s_blob, (size_t)n, &g_cfg);
    if (rc != 0) {
        LOG_E("cfg", "配置校验失败(rc=%d)，回退默认配置", rc);
        app_config_defaults(&g_cfg);
        (void)app_ctx_save();
        return -1;
    }

    rc = app_config_sanitize(&g_cfg);
    if (rc > 0) {
        LOG_W("cfg", "配置有 %d 个字段非法，已修正并回写", rc);
        (void)app_ctx_save();
    }

    LOG_I("cfg", "配置加载成功：APN=%s 限额=%luMB RNDIS=%s 白名单=%u 黑名单=%u",
          app_config_apn(&g_cfg),
          (unsigned long)g_cfg.traffic_limit_mb,
          g_cfg.rndis_enable ? "开" : "关",
          (unsigned)g_cfg.white_count, (unsigned)g_cfg.black_count);
    return 0;
}

void app_ctx_lock(void)
{
    (void)sdk_mutex_lock(g_cfg_lock, 10000u);
}

void app_ctx_unlock(void)
{
    (void)sdk_mutex_unlock(g_cfg_lock);
}

int app_ctx_save(void)
{
    size_t n;

    /* 保存前做一次合法性检查，避免把坏值写进 Flash */
    (void)app_config_sanitize(&g_cfg);

    n = app_config_serialize(&g_cfg, &s_blob, sizeof(s_blob));
    if (n == 0u) {
        LOG_E("cfg", "序列化失败，未写入 Flash");
        return -1;
    }

    if (sdk_cfg_save(&s_blob, (uint32_t)n) < 0) {
        LOG_E("cfg", "Flash 写入失败（原配置仍然有效）");
        return -2;
    }

    LOG_I("cfg", "配置已保存到 Flash（%u 字节）", (unsigned)n);
    return 0;
}
