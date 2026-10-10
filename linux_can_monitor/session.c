/**
 * @file    session.c
 * @brief   充电历史会话存储实现 —— SQLite3 + 紧凑 BLOB 曲线
 */

#include "session.h"

extern int g_verbose;   /* 由 main.c 定义：调试日志开关 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          /* clock_gettime：会话起止时间戳 */
#include <sqlite3.h>

extern int g_verbose;   /* 由 main.c 定义 */

/* 只在 -v 时输出的日志宏（和 main.c 里的同名宏一致） */
#ifndef V
#define V(...)   do { if (g_verbose) { printf(__VA_ARGS__); } } while (0)
#endif

/*==============================================================================
 *                              内部状态
 *============================================================================*/

static sqlite3 *s_db = NULL;

/** 取当前 Unix 时间（微秒） */
static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

/*==============================================================================
 *                              建表与清理
 *============================================================================*/

int session_init(const char *db_path)
{
    char *err = NULL;
    const char *sql;
    char        buf[512];

    if (db_path == NULL)
    {
        return -1;
    }

    if (sqlite3_open(db_path, &s_db) != SQLITE_OK)
    {
        fprintf(stderr, "[SESSION] 打开历史库失败: %s\n",
                s_db ? sqlite3_errmsg(s_db) : "?");
        if (s_db != NULL) { sqlite3_close(s_db); s_db = NULL; }
        return -1;
    }

    sqlite3_busy_timeout(s_db, 2000);

    sql =
        "CREATE TABLE IF NOT EXISTS charge_session ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  start_us      INTEGER NOT NULL,"
        "  end_us        INTEGER NOT NULL,"
        "  soc_start_x10 INTEGER NOT NULL,"
        "  soc_end_x10   INTEGER NOT NULL,"
        "  energy_wh     INTEGER NOT NULL,"
        "  charge_sec    INTEGER NOT NULL,"
        "  is_full       INTEGER NOT NULL DEFAULT 0,"
        "  n_points      INTEGER NOT NULL,"
        "  curve         BLOB"
        ");";

    if (sqlite3_exec(s_db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[SESSION] 建表失败: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }

    /* 只保留最近 SESSION_MAX 条：把 id 比"第 N 新的那条"还小的全删掉。
     * 这条 SQL 在每次启动和每次保存后都会跑一遍，保证历史不会无限增长。 */
    snprintf(buf, sizeof(buf),
             "DELETE FROM charge_session WHERE id NOT IN "
             "(SELECT id FROM charge_session ORDER BY id DESC LIMIT %d);",
             SESSION_MAX);

    if (sqlite3_exec(s_db, buf, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[SESSION] 清理旧历史失败: %s\n", err ? err : "?");
        sqlite3_free(err);      /* 清理失败不算致命，继续用 */
    }

    V("[SESSION] 充电历史库已就绪（最多保留 %d 次）\n", SESSION_MAX);
    return 0;
}

void session_close(void)
{
    if (s_db != NULL)
    {
        sqlite3_close(s_db);
        s_db = NULL;
    }
}

int session_ready(void)
{
    return (s_db != NULL) ? 1 : 0;
}

/*==============================================================================
 *                              曲线编解码
 *============================================================================*/

/** 把 4 条曲线打包成紧凑 BLOB */
static unsigned char *pack_curve(const int *soc, const int *v,
                                 const int *i, const int *t, int n,
                                 int *out_len)
{
    unsigned char *b;
    int k;

    if (n <= 0 || n > SESSION_POINTS)
    {
        n = (n > SESSION_POINTS) ? SESSION_POINTS : 0;
    }
    if (n <= 0)
    {
        *out_len = 0;
        return NULL;
    }

    b = (unsigned char *)malloc((size_t)n * SESSION_PT_BYTES);
    if (b == NULL) { *out_len = 0; return NULL; }

    for (k = 0; k < n; k++)
    {
        unsigned char *p = b + (size_t)k * SESSION_PT_BYTES;
        unsigned int   s = (unsigned int)((soc  ? soc[k]  : 0) & 0xFFFF);
        unsigned int   u = (unsigned int)((v    ? v[k]    : 0) & 0xFFFF);
        unsigned int   c = (unsigned int)((i    ? i[k]    : 0) & 0xFFFF);
        int            e = (t != NULL) ? t[k] : 0;

        if (e < -50) { e = -50; }
        if (e >  200) { e = 200; }

        p[0] = (unsigned char)((s >> 8) & 0xFF);
        p[1] = (unsigned char)(s & 0xFF);
        p[2] = (unsigned char)((u >> 8) & 0xFF);
        p[3] = (unsigned char)(u & 0xFF);
        p[4] = (unsigned char)((c >> 8) & 0xFF);
        p[5] = (unsigned char)(c & 0xFF);
        p[6] = (unsigned char)((e + 50) & 0xFF);    /* 和国标一样 +50 偏移 */
        p[7] = 0;
    }

    *out_len = n * SESSION_PT_BYTES;
    return b;
}

/** 解包 BLOB 到 4 条曲线 */
static int unpack_curve(const unsigned char *b, int len,
                        int *soc, int *v, int *i, int *t, int max)
{
    int n = len / SESSION_PT_BYTES;
    int k;

    if (b == NULL || len <= 0) { return 0; }
    if (n > max) { n = max; }

    for (k = 0; k < n; k++)
    {
        const unsigned char *p = b + (size_t)k * SESSION_PT_BYTES;

        if (soc != NULL) { soc[k] = (int)(((unsigned int)p[0] << 8) | p[1]); }
        if (v   != NULL) { v[k]   = (int)(((unsigned int)p[2] << 8) | p[3]); }
        if (i   != NULL) { i[k]   = (int)(((unsigned int)p[4] << 8) | p[5]); }
        if (t   != NULL) { t[k]   = (int)p[6] - 50; }
    }
    return n;
}

/*==============================================================================
 *                              保存 / 读取
 *============================================================================*/

int session_save(session_info_t *info,
                 const int *soc_x10, const int *v_x10,
                 const int *i_x10, const int *temp_c, int n)
{
    sqlite3_stmt  *st = NULL;
    unsigned char *blob;
    int            blob_len = 0;
    char           cleanup[256];
    char          *err = NULL;
    int            rc;

    if (s_db == NULL || info == NULL) { return -1; }

    if (info->start_us == 0) { info->start_us = now_us(); }
    if (info->end_us   == 0) { info->end_us   = now_us(); }

    blob = pack_curve(soc_x10, v_x10, i_x10, temp_c, n, &blob_len);
    if (blob == NULL)
    {
        fprintf(stderr, "[SESSION] 本次没有曲线数据，跳过保存\n");
        return -1;
    }

    rc = sqlite3_prepare_v2(s_db,
            "INSERT INTO charge_session"
            " (start_us,end_us,soc_start_x10,soc_end_x10,energy_wh,charge_sec,"
            "  is_full,n_points,curve) VALUES (?,?,?,?,?,?,?,?,?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[SESSION] 准备 INSERT 失败: %s\n", sqlite3_errmsg(s_db));
        free(blob);
        return -1;
    }

    sqlite3_bind_int64(st, 1, info->start_us);
    sqlite3_bind_int64(st, 2, info->end_us);
    sqlite3_bind_int  (st, 3, info->soc_start_x10);
    sqlite3_bind_int  (st, 4, info->soc_end_x10);
    sqlite3_bind_int  (st, 5, info->energy_wh);
    sqlite3_bind_int  (st, 6, info->charge_sec);
    sqlite3_bind_int  (st, 7, info->full);
    sqlite3_bind_int  (st, 8, blob_len / SESSION_PT_BYTES);
    sqlite3_bind_blob (st, 9, blob, blob_len, SQLITE_STATIC);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    free(blob);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "[SESSION] 写入历史失败: %s\n", sqlite3_errmsg(s_db));
        return -1;
    }

    info->id = (int)sqlite3_last_insert_rowid(s_db);
    info->n  = blob_len / SESSION_PT_BYTES;

    /* 超出上限就把最旧的挤掉 */
    snprintf(cleanup, sizeof(cleanup),
             "DELETE FROM charge_session WHERE id NOT IN "
             "(SELECT id FROM charge_session ORDER BY id DESC LIMIT %d);",
             SESSION_MAX);
    if (sqlite3_exec(s_db, cleanup, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "[SESSION] 淘汰旧历史失败: %s\n", err ? err : "?");
        sqlite3_free(err);
    }

    V("[SESSION] 已保存第 %d 次充电历史（%d 点, SOC %d.%d%% -> %d.%d%%）\n",
           info->id, info->n,
           info->soc_start_x10 / 10, info->soc_start_x10 % 10,
           info->soc_end_x10 / 10, info->soc_end_x10 % 10);
    return 0;
}

int session_list(session_info_t *out, int max)
{
    sqlite3_stmt *st = NULL;
    int           cnt = 0;

    if (s_db == NULL || out == NULL || max <= 0) { return 0; }

    if (sqlite3_prepare_v2(s_db,
            "SELECT id,start_us,end_us,soc_start_x10,soc_end_x10,"
            "       energy_wh,charge_sec,is_full,n_points "
            "FROM charge_session ORDER BY id DESC LIMIT ?;",
            -1, &st, NULL) != SQLITE_OK)
    {
        return 0;
    }

    sqlite3_bind_int(st, 1, max);

    while (sqlite3_step(st) == SQLITE_ROW && cnt < max)
    {
        session_info_t *o = &out[cnt];

        memset(o, 0, sizeof(*o));
        o->id            = sqlite3_column_int(st, 0);
        o->start_us      = sqlite3_column_int64(st, 1);
        o->end_us        = sqlite3_column_int64(st, 2);
        o->soc_start_x10 = sqlite3_column_int(st, 3);
        o->soc_end_x10   = sqlite3_column_int(st, 4);
        o->energy_wh     = sqlite3_column_int(st, 5);
        o->charge_sec    = sqlite3_column_int(st, 6);
        o->full          = sqlite3_column_int(st, 7);
        o->n             = sqlite3_column_int(st, 8);
        cnt++;
    }

    sqlite3_finalize(st);
    return cnt;
}

int session_count(void)
{
    sqlite3_stmt *st = NULL;
    int           n = 0;

    if (s_db == NULL) { return 0; }

    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM charge_session;",
                           -1, &st, NULL) != SQLITE_OK)
    {
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int session_load_curve(int id,
                       int *soc_x10, int *v_x10, int *i_x10, int *temp_c,
                       int max, session_info_t *info)
{
    sqlite3_stmt *st = NULL;
    const void   *blob;
    int           len;
    int           n = 0;

    if (s_db == NULL) { return -1; }

    if (sqlite3_prepare_v2(s_db,
            "SELECT start_us,end_us,soc_start_x10,soc_end_x10,energy_wh,"
            "       charge_sec,is_full,n_points,curve "
            "FROM charge_session WHERE id=?;",
            -1, &st, NULL) != SQLITE_OK)
    {
        return -1;
    }

    sqlite3_bind_int(st, 1, id);

    if (sqlite3_step(st) == SQLITE_ROW)
    {
        if (info != NULL)
        {
            memset(info, 0, sizeof(*info));
            info->id            = id;
            info->start_us      = sqlite3_column_int64(st, 0);
            info->end_us        = sqlite3_column_int64(st, 1);
            info->soc_start_x10 = sqlite3_column_int(st, 2);
            info->soc_end_x10   = sqlite3_column_int(st, 3);
            info->energy_wh     = sqlite3_column_int(st, 4);
            info->charge_sec    = sqlite3_column_int(st, 5);
            info->full          = sqlite3_column_int(st, 6);
            info->n             = sqlite3_column_int(st, 7);
        }

        blob = sqlite3_column_blob(st, 8);
        len  = sqlite3_column_bytes(st, 8);
        n    = unpack_curve((const unsigned char *)blob, len,
                            soc_x10, v_x10, i_x10, temp_c, max);
    }

    sqlite3_finalize(st);
    return n;
}
