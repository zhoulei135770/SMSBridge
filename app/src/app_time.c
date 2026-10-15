/**
 * @file    app_time.c
 * @brief   时间解析实现（见 inc/app_time.h）。
 */
#include "app_time.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 历法换算                                                            */
/* ------------------------------------------------------------------ */
/* Howard Hinnant 的 days_from_civil：把公历年月日换算成"1970-01-01 起的天数"。
 * 全部是整数运算，天然覆盖闰年与"百年不闰、四百年再闰"规则。 */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;

    y  -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                       /* [0, 399] */
    doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;     /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

int64_t app_time_utc_from_fields(int year, int mon, int day, int hour, int min, int sec)
{
    int64_t days = days_from_civil((int64_t)year, mon, day);

    return days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + (int64_t)sec;
}

int app_time_is_valid(int64_t utc_seconds)
{
    return (utc_seconds >= APP_TIME_MIN_VALID && utc_seconds <= APP_TIME_MAX_VALID) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 解析                                                                */
/* ------------------------------------------------------------------ */
/** 读固定位数的十进制数字；不足位数或遇到非数字返回 0（失败） */
static int read_uint(const char **pp, int digits, int *out)
{
    const char *p  = *pp;
    int         v  = 0;
    int         i;

    for (i = 0; i < digits; ++i) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *pp  = p + digits;
    *out = v;
    return 1;
}

static int fields_ok(int y, int mo, int d, int h, int mi, int se)
{
    if (y < 1970 || y > 2200) return 0;
    if (mo < 1 || mo > 12)    return 0;
    if (d  < 1 || d  > 31)    return 0;
    if (h  < 0 || h  > 23)    return 0;
    if (mi < 0 || mi > 59)    return 0;
    if (se < 0 || se > 60)    return 0;      /* 60 = 闰秒容错 */
    return 1;
}

int app_time_parse_iso8601(const char *s, int64_t *utc_out)
{
    const char *p = s;
    int  y, mo, d, h, mi, se = 0;
    int  tz_min = 0;

    if (s == NULL || utc_out == NULL) return -1;

    if (!read_uint(&p, 4, &y))   return -1;
    if (*p != '-')               return -1;
    ++p;
    if (!read_uint(&p, 2, &mo))  return -1;
    if (*p != '-')               return -1;
    ++p;
    if (!read_uint(&p, 2, &d))   return -1;
    if (*p != 'T' && *p != ' ' && *p != 't') return -1;
    ++p;
    if (!read_uint(&p, 2, &h))   return -1;
    if (*p != ':')               return -1;
    ++p;
    if (!read_uint(&p, 2, &mi))  return -1;
    if (*p == ':') {
        ++p;
        if (!read_uint(&p, 2, &se)) return -1;
    }
    if (*p == '.') {                          /* 小数秒：跳过 */
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == 'Z' || *p == 'z') {
        tz_min = 0;
    } else if (*p == '+' || *p == '-') {
        int sg = (*p == '+') ? 1 : -1;
        int th = 0, tm = 0;
        ++p;
        if (!read_uint(&p, 2, &th)) return -1;
        if (*p == ':') {
            ++p;
            if (!read_uint(&p, 2, &tm)) return -1;
        } else if (*p >= '0' && *p <= '9') {
            if (!read_uint(&p, 2, &tm)) return -1;
        }
        if (th > 23 || tm > 59) return -1;
        tz_min = sg * (th * 60 + tm);
    }

    if (!fields_ok(y, mo, d, h, mi, se)) return -1;

    *utc_out = app_time_utc_from_fields(y, mo, d, h, mi, se) - (int64_t)tz_min * 60;
    return 0;
}

int app_time_parse_cclk(const char *at_resp, int64_t *utc_out)
{
    const char *p;
    int  y, mo, d, h, mi, se = 0;
    int  tz_q = 0;

    if (at_resp == NULL || utc_out == NULL) return -1;

    p = strstr(at_resp, "+CCLK:");
    if (p == NULL) return -1;
    p += 6;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '"') ++p;

    /* 年：4 位（后面紧跟 '/'）或 2 位（20xx） */
    if (p[0] != '\0' && p[1] != '\0' && p[2] != '\0' && p[3] != '\0' && p[4] == '/') {
        if (!read_uint(&p, 4, &y)) return -1;
    } else {
        if (!read_uint(&p, 2, &y)) return -1;
        y += 2000;
    }
    if (*p != '/') return -1;
    ++p;
    if (!read_uint(&p, 2, &mo)) return -1;
    if (*p != '/') return -1;
    ++p;
    if (!read_uint(&p, 2, &d)) return -1;
    if (*p != ',') return -1;
    ++p;
    if (!read_uint(&p, 2, &h)) return -1;
    if (*p != ':') return -1;
    ++p;
    if (!read_uint(&p, 2, &mi)) return -1;
    if (*p == ':') {
        ++p;
        if (!read_uint(&p, 2, &se)) return -1;
    }
    if (*p == '+' || *p == '-') {
        int sg = (*p == '+') ? 1 : -1;
        int q  = 0;
        ++p;
        if (!read_uint(&p, 2, &q)) return -1;
        if (q > 96) return -1;                /* ±24h 上限 */
        tz_q = sg * q;
    }

    if (!fields_ok(y, mo, d, h, mi, se)) return -1;

    /* 时区单位是 15 分钟 */
    *utc_out = app_time_utc_from_fields(y, mo, d, h, mi, se) - (int64_t)tz_q * 15 * 60;
    return 0;
}

uint64_t app_time_to_ts_ms(int64_t utc_seconds, uint32_t uptime_ms)
{
    if (utc_seconds <= 0) return 0u;
    return (uint64_t)utc_seconds * 1000u + (uint64_t)(uptime_ms % 1000u);
}
