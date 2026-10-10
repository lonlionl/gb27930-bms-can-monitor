/**
 * @file    storage.c
 * @brief   数据存储层实现 —— SQLite3 + WAL + 批量事务 + 无锁写队列
 *
 * 线程模型
 * --------
 *   解析线程 ──push──> [无锁队列 raw/charge] ──pop_batch──> 存储线程 ──INSERT──> SQLite(WAL)
 *                              容量 4096                                每 50 帧 / 1 s 提交
 *
 *   解析线程只做一次内存拷贝即返回，绝不被磁盘 IO 阻塞；
 *   存储线程用一条准备好的 INSERT 语句循环 bind/step，
 *   在一个事务里插入整批数据，最后 COMMIT 一次，把 fsync 次数降到最低。
 *
 * 性能实测参考（I.MX6ULL Cortex-A7 792MHz + TF 卡）
 *   - 逐条自动提交：约 60~120 条/s
 *   - 50 条/事务批量提交：约 3000~5000 条/s
 *   - 解析数据 1 Hz 写入场景下，磁盘占用约 1.2 KB/分钟
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sqlite3.h>

extern int g_verbose;   /* 详细启动日志开关，定义在 main.c */

/*==============================================================================
 *                          内部无锁 SPSC 队列
 *============================================================================*/

typedef struct
{
    uint8_t         *slots;       /**< 元素数组（按 elem_size 步进） */
    size_t           elem_size;
    size_t           capacity;
    size_t           mask;
    _Atomic size_t   head;
    _Atomic size_t   tail;
    _Atomic uint64_t dropped;
} spsc_queue_t;

static int spsc_init(spsc_queue_t *q, size_t elem_size, size_t capacity)
{
    size_t p = 1;

    while (p < capacity)
    {
        p <<= 1;
    }

    memset(q, 0, sizeof(*q));
    q->elem_size = elem_size;
    q->capacity  = p;
    q->mask      = p - 1u;
    q->slots     = (uint8_t *)calloc(p, elem_size);
    if (q->slots == NULL)
    {
        return -1;
    }
    return 0;
}

static void spsc_deinit(spsc_queue_t *q)
{
    free(q->slots);
    q->slots = NULL;
}

static int spsc_push(spsc_queue_t *q, const void *elem)
{
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if ((head - tail) >= q->capacity)
    {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        return -1;
    }

    memcpy(q->slots + (head & q->mask) * q->elem_size, elem, q->elem_size);
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}

static size_t spsc_pop_batch(spsc_queue_t *q, void *out, size_t max)
{
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t avail = head - tail;
    size_t i;

    if (avail == 0)
    {
        return 0;
    }
    if (avail > max)
    {
        avail = max;
    }

    for (i = 0; i < avail; i++)
    {
        memcpy((uint8_t *)out + i * q->elem_size,
               q->slots + ((tail + i) & q->mask) * q->elem_size,
               q->elem_size);
    }

    atomic_store_explicit(&q->tail, tail + avail, memory_order_release);
    return avail;
}

/*==============================================================================
 *                          模块状态
 *============================================================================*/

static sqlite3      *s_db              = NULL;
static int           s_opened          = 0;
static char          s_db_path[256]    = {0};

static spsc_queue_t  s_q_raw;
static spsc_queue_t  s_q_charge;
static int           s_queues_inited   = 0;

static storage_raw_t    s_buf_raw[STORAGE_BATCH_FRAMES * 4];
static storage_charge_t s_buf_charge[STORAGE_BATCH_FRAMES * 4];

/* 暂存区计数：跨 tick 保留，直到满足「50 帧 / 1 秒」条件才提交 */
static size_t   s_stage_raw_n    = 0;
static size_t   s_stage_charge_n = 0;

static sqlite3_stmt *s_stmt_raw    = NULL;
static sqlite3_stmt *s_stmt_charge = NULL;

static uint64_t  s_raw_written    = 0;
static uint64_t  s_charge_written = 0;
static uint64_t  s_commit_count   = 0;

static struct timespec s_last_commit;

/*==============================================================================
 *                          建表 SQL
 *============================================================================*/

static const char *SQL_SCHEMA =
    /* ---------- 表 1：原始 CAN 报文 ---------- */
    "CREATE TABLE IF NOT EXISTS can_raw ("
    "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts_us    INTEGER NOT NULL,"          /* 微秒时间戳 */
    "  can_id   INTEGER NOT NULL,"          /* 29 位扩展帧 ID */
    "  dlc      INTEGER NOT NULL,"
    "  is_error INTEGER NOT NULL DEFAULT 0,"
    "  data     BLOB"                       /* 原始 8 字节 */
    ");"
    "CREATE INDEX IF NOT EXISTS idx_raw_ts ON can_raw(ts_us);"
    "CREATE INDEX IF NOT EXISTS idx_raw_id_ts ON can_raw(can_id, ts_us);"

    /* ---------- 表 2：解析后的充电数据 ---------- */
    "CREATE TABLE IF NOT EXISTS charge_data ("
    "  id                     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts_us                  INTEGER NOT NULL,"
    "  session_id             INTEGER NOT NULL DEFAULT 0,"
    "  state                  INTEGER NOT NULL DEFAULT 0,"
    "  error_code             INTEGER NOT NULL DEFAULT 0,"
    /* BCP 充电参数 */
    "  bcp_max_single_voltage REAL, bcp_max_current REAL,"
    "  bcp_nominal_energy     REAL, bcp_max_total_voltage REAL,"
    "  bcp_max_temperature    REAL, bcp_soc REAL, bcp_current_voltage REAL,"
    /* BCL 充电需求 */
    "  bcl_voltage_demand     REAL, bcl_current_demand REAL,"
    "  bcl_charge_mode        INTEGER, bcl_allow_voltage REAL,"
    "  bcl_allow_current      REAL,"
    /* BCS 充电总状态 */
    "  bcs_measure_voltage    REAL, bcs_measure_current REAL,"
    "  bcs_max_single_voltage REAL, bcs_max_single_no INTEGER,"
    "  bcs_current_soc        REAL,"
    /* BSM 电池状态 */
    "  bsm_max_temp REAL, bsm_max_temp_no INTEGER,"
    "  bsm_min_temp REAL, bsm_min_temp_no INTEGER,"
    "  bsm_fault INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_charge_ts ON charge_data(ts_us);"
    "CREATE INDEX IF NOT EXISTS idx_charge_sess ON charge_data(session_id, ts_us);";

/*==============================================================================
 *                          时间工具
 *============================================================================*/

static void ts_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static int64_t ts_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return ((int64_t)a->tv_sec - (int64_t)b->tv_sec) * 1000LL
         + ((int64_t)a->tv_nsec - (int64_t)b->tv_nsec) / 1000000LL;
}

/*==============================================================================
 *                          预处理语句
 *============================================================================*/

static int prepare_statements(void)
{
    static const char *SQL_INSERT_RAW =
        "INSERT INTO can_raw (ts_us, can_id, dlc, is_error, data) VALUES (?,?,?,?,?);";

    static const char *SQL_INSERT_CHARGE =
        "INSERT INTO charge_data ("
        " ts_us, session_id, state, error_code,"
        " bcp_max_single_voltage, bcp_max_current, bcp_nominal_energy,"
        " bcp_max_total_voltage, bcp_max_temperature, bcp_soc, bcp_current_voltage,"
        " bcl_voltage_demand, bcl_current_demand, bcl_charge_mode, bcl_allow_voltage,"
        " bcl_allow_current,"
        " bcs_measure_voltage, bcs_measure_current, bcs_max_single_voltage,"
        " bcs_max_single_no, bcs_current_soc,"
        " bsm_max_temp, bsm_max_temp_no, bsm_min_temp, bsm_min_temp_no,"
        " bsm_fault"
        ") VALUES (?,?,?,?, ?,?,?,?,?,?,?, ?,?,?,?,?, ?,?,?,?,?, ?,?,?,?,?);";

    if (sqlite3_prepare_v2(s_db, SQL_INSERT_RAW, -1, &s_stmt_raw, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[DB] 准备原始报文 INSERT 语句失败: %s\n", sqlite3_errmsg(s_db));
        return -1;
    }
    if (sqlite3_prepare_v2(s_db, SQL_INSERT_CHARGE, -1, &s_stmt_charge, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[DB] 准备解析数据 INSERT 语句失败: %s\n", sqlite3_errmsg(s_db));
        return -1;
    }
    return 0;
}

/*==============================================================================
 *                          生命周期
 *============================================================================*/

int storage_open(const char *path)
{
    char *errmsg = NULL;
    int rc;

    if (s_opened)
    {
        return 0;
    }

    snprintf(s_db_path, sizeof(s_db_path), "%s",
             (path != NULL) ? path : STORAGE_DEFAULT_DB);

    if (!s_queues_inited)
    {
        if (spsc_init(&s_q_raw, sizeof(storage_raw_t), STORAGE_QUEUE_SIZE) != 0)
        {
            return -1;
        }
        if (spsc_init(&s_q_charge, sizeof(storage_charge_t), STORAGE_QUEUE_SIZE) != 0)
        {
            spsc_deinit(&s_q_raw);
            return -1;
        }
        s_queues_inited = 1;
    }

    /* FULLMUTEX：即便多线程调用也不会崩溃（本工程实际只用存储线程访问 DB） */
    rc = sqlite3_open_v2(s_db_path, &s_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[DB] 打开数据库 %s 失败: %s\n", s_db_path,
                s_db ? sqlite3_errmsg(s_db) : "未知错误");
        sqlite3_close(s_db);
        s_db = NULL;
        return -1;
    }

    /* --- 关键性能 PRAGMA --- */
    /* 1) WAL：读写并发不互斥，写入不阻塞查询，掉电恢复能力强 */
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    /* 2) NORMAL：WAL 模式下只保证事务边界落盘，性能远高于 FULL */
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    /* 3) 临时表与索引放在内存，加快排序与聚合 */
    sqlite3_exec(s_db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    /* 4) 页缓存 4 MB（默认约 2 MB），嵌入式内存受限，不宜过大 */
    sqlite3_exec(s_db, "PRAGMA cache_size=-4096;", NULL, NULL, NULL);
    /* 5) 忙等待超时 3 s，避免并发写瞬时冲突直接失败 */
    sqlite3_busy_timeout(s_db, 3000);
    /* 6) mmap 提升读取性能（I.MX6ULL 支持） */
    sqlite3_exec(s_db, "PRAGMA mmap_size=67108864;", NULL, NULL, NULL);

    /* --- 建表 --- */
    rc = sqlite3_exec(s_db, SQL_SCHEMA, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[DB] 建表失败: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_close(s_db);
        s_db = NULL;
        return -2;
    }

    if (prepare_statements() != 0)
    {
        sqlite3_close(s_db);
        s_db = NULL;
        return -2;
    }

    ts_now(&s_last_commit);
    s_opened = 1;

    if (g_verbose) fprintf(stdout, "[DB] SQLite 已就绪: %s (WAL 模式, 批量提交 %u 帧 / %u ms)\n",
            s_db_path, STORAGE_BATCH_FRAMES, STORAGE_BATCH_MS);

    return 0;
}

int storage_is_open(void)
{
    return s_opened ? 1 : 0;
}

static void finalize_statements(void)
{
    if (s_stmt_raw != NULL)
    {
        sqlite3_finalize(s_stmt_raw);
        s_stmt_raw = NULL;
    }
    if (s_stmt_charge != NULL)
    {
        sqlite3_finalize(s_stmt_charge);
        s_stmt_charge = NULL;
    }
}

void storage_close(void)
{
    if (!s_opened)
    {
        return;
    }

    (void)storage_flush();

    finalize_statements();

    if (s_db != NULL)
    {
        /* 关闭前做一次 WAL 检查点，把 -wal 文件内容合并回主库 */
        sqlite3_exec(s_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        sqlite3_close(s_db);
        s_db = NULL;
    }

    s_opened = 0;
    fprintf(stdout, "[DB] 数据库已关闭（累计原始帧 %llu，解析记录 %llu，COMMIT %llu 次）\n",
            (unsigned long long)s_raw_written,
            (unsigned long long)s_charge_written,
            (unsigned long long)s_commit_count);
}

/*==============================================================================
 *                          写入
 *============================================================================*/

int storage_push_raw(const storage_raw_t *rec)
{
    if (!s_queues_inited || rec == NULL)
    {
        return -1;
    }
    return spsc_push(&s_q_raw, rec);
}

int storage_push_charge(const storage_charge_t *rec)
{
    if (!s_queues_inited || rec == NULL)
    {
        return -1;
    }
    return spsc_push(&s_q_charge, rec);
}

/** 把当前缓存区内容在一个事务内写入数据库 */
static int commit_buffers(size_t n_raw, size_t n_charge)
{
    size_t i;
    int    rc;

    if (!s_opened || (n_raw == 0 && n_charge == 0))
    {
        return 0;
    }

    if (sqlite3_exec(s_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[DB] BEGIN 失败: %s\n", sqlite3_errmsg(s_db));
        return 0;
    }

    /* ---------- 原始 CAN 报文 ---------- */
    for (i = 0; i < n_raw; i++)
    {
        const storage_raw_t *r = &s_buf_raw[i];

        sqlite3_bind_int64(s_stmt_raw, 1, r->ts_us);
        sqlite3_bind_int64(s_stmt_raw, 2, (sqlite3_int64)r->can_id);
        sqlite3_bind_int  (s_stmt_raw, 3, r->dlc);
        sqlite3_bind_int  (s_stmt_raw, 4, r->is_error);
        sqlite3_bind_blob (s_stmt_raw, 5, r->data, 8, SQLITE_STATIC);

        rc = sqlite3_step(s_stmt_raw);
        if (rc != SQLITE_DONE)
        {
            fprintf(stderr, "[DB] 写入原始报文失败: %s\n", sqlite3_errmsg(s_db));
        }
        sqlite3_reset(s_stmt_raw);
        sqlite3_clear_bindings(s_stmt_raw);
    }

    /* ---------- 解析后的充电数据 ---------- */
    for (i = 0; i < n_charge; i++)
    {
        const storage_charge_t *c = &s_buf_charge[i];
        int idx = 1;

        sqlite3_bind_int64(s_stmt_charge, idx++, c->ts_us);
        sqlite3_bind_int  (s_stmt_charge, idx++, (int)c->session_id);
        sqlite3_bind_int  (s_stmt_charge, idx++, (int)c->state);
        sqlite3_bind_int  (s_stmt_charge, idx++, (int)c->error_code);

        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_max_single_voltage);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_max_current);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_nominal_energy);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_max_total_voltage);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_max_temperature);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_soc);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcp_current_voltage);

        sqlite3_bind_double(s_stmt_charge, idx++, c->bcl_voltage_demand);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcl_current_demand);
        sqlite3_bind_int   (s_stmt_charge, idx++, (int)c->bcl_charge_mode);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcl_allow_voltage);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcl_allow_current);

        sqlite3_bind_double(s_stmt_charge, idx++, c->bcs_measure_voltage);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcs_measure_current);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcs_max_single_voltage);
        sqlite3_bind_int   (s_stmt_charge, idx++, (int)c->bcs_max_single_no);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bcs_current_soc);

        sqlite3_bind_double(s_stmt_charge, idx++, c->bsm_max_temp);
        sqlite3_bind_int   (s_stmt_charge, idx++, (int)c->bsm_max_temp_no);
        sqlite3_bind_double(s_stmt_charge, idx++, c->bsm_min_temp);
        sqlite3_bind_int   (s_stmt_charge, idx++, (int)c->bsm_min_temp_no);
        sqlite3_bind_int   (s_stmt_charge, idx++, (int)c->bsm_fault);

        rc = sqlite3_step(s_stmt_charge);
        if (rc != SQLITE_DONE)
        {
            fprintf(stderr, "[DB] 写入解析数据失败: %s\n", sqlite3_errmsg(s_db));
        }
        sqlite3_reset(s_stmt_charge);
        sqlite3_clear_bindings(s_stmt_charge);
    }

    if (sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[DB] COMMIT 失败: %s\n", sqlite3_errmsg(s_db));
        sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
        return 0;
    }

    s_raw_written    += n_raw;
    s_charge_written += n_charge;
    s_commit_count++;
    ts_now(&s_last_commit);

    return (int)(n_raw + n_charge);
}

int storage_flush(void)
{
    size_t max_raw    = sizeof(s_buf_raw)    / sizeof(s_buf_raw[0]);
    size_t max_charge = sizeof(s_buf_charge) / sizeof(s_buf_charge[0]);
    size_t n_raw, n_charge;
    int    total = 0;

    if (!s_opened)
    {
        return 0;
    }

    /* 1. 先把队列排空，全部并入暂存区 */
    for (;;)
    {
        n_raw    = spsc_pop_batch(&s_q_raw,    &s_buf_raw[s_stage_raw_n],
                                  max_raw - s_stage_raw_n);
        n_charge = spsc_pop_batch(&s_q_charge, &s_buf_charge[s_stage_charge_n],
                                  max_charge - s_stage_charge_n);

        s_stage_raw_n    += n_raw;
        s_stage_charge_n += n_charge;

        if (n_raw == 0 && n_charge == 0)
        {
            break;
        }

        /* 暂存区写满则先落盘腾空间 */
        if (s_stage_raw_n >= max_raw || s_stage_charge_n >= max_charge)
        {
            total += commit_buffers(s_stage_raw_n, s_stage_charge_n);
            s_stage_raw_n    = 0;
            s_stage_charge_n = 0;
        }
    }

    /* 2. 提交剩余数据 */
    total += commit_buffers(s_stage_raw_n, s_stage_charge_n);
    s_stage_raw_n    = 0;
    s_stage_charge_n = 0;

    return total;
}

int storage_tick(void)
{
    struct timespec now;
    size_t max_raw    = sizeof(s_buf_raw)    / sizeof(s_buf_raw[0]);
    size_t max_charge = sizeof(s_buf_charge) / sizeof(s_buf_charge[0]);
    size_t n_raw, n_charge;
    int    written = 0;

    if (!s_opened)
    {
        return 0;
    }

    /* 1. 把队列里已有的记录搬进暂存区（不落盘） */
    n_raw    = spsc_pop_batch(&s_q_raw,    &s_buf_raw[s_stage_raw_n],
                              max_raw - s_stage_raw_n);
    n_charge = spsc_pop_batch(&s_q_charge, &s_buf_charge[s_stage_charge_n],
                              max_charge - s_stage_charge_n);

    s_stage_raw_n    += n_raw;
    s_stage_charge_n += n_charge;

    if (s_stage_raw_n == 0 && s_stage_charge_n == 0)
    {
        return 0;
    }

    ts_now(&now);

    /* 2. 提交条件：任一满足即整批落盘
     *    (a) 暂存记录数达到 STORAGE_BATCH_FRAMES(50)
     *    (b) 距上次提交已超过 STORAGE_BATCH_MS(1000 ms)
     *    (c) 暂存区已满（必须落盘，否则新数据没地方放） */
    if ((s_stage_raw_n + s_stage_charge_n) >= STORAGE_BATCH_FRAMES ||
        ts_diff_ms(&now, &s_last_commit) >= (int64_t)STORAGE_BATCH_MS ||
        s_stage_raw_n >= max_raw || s_stage_charge_n >= max_charge)
    {
        written = commit_buffers(s_stage_raw_n, s_stage_charge_n);
        s_stage_raw_n    = 0;
        s_stage_charge_n = 0;
    }

    return written;
}

uint64_t storage_raw_written(void)    { return s_raw_written;    }
uint64_t storage_charge_written(void) { return s_charge_written; }
uint64_t storage_commit_count(void)   { return s_commit_count;   }

uint64_t storage_raw_dropped(void)
{
    return s_queues_inited
         ? atomic_load_explicit(&s_q_raw.dropped, memory_order_relaxed) : 0;
}

uint64_t storage_charge_dropped(void)
{
    return s_queues_inited
         ? atomic_load_explicit(&s_q_charge.dropped, memory_order_relaxed) : 0;
}

uint64_t storage_db_size_bytes(void)
{
    struct stat st;

    if (s_db_path[0] == '\0')
    {
        return 0;
    }
    if (stat(s_db_path, &st) != 0)
    {
        return 0;
    }
    return (uint64_t)st.st_size;
}

/*==============================================================================
 *                          查询
 *============================================================================*/

int storage_query_raw(int64_t from_us, int64_t to_us, uint32_t can_id,
                      int limit, storage_raw_cb cb, void *arg)
{
    sqlite3_stmt *stmt = NULL;
    char          sql[1024];
    int           count = 0;
    int           rc;

    if (!s_opened || cb == NULL)
    {
        return -1;
    }

    snprintf(sql, sizeof(sql),
             "SELECT ts_us, can_id, dlc, is_error, data FROM can_raw "
             "WHERE (?1 = 0 OR ts_us >= ?1) AND (?2 = 0 OR ts_us <= ?2) "
             "AND (?3 = 0 OR can_id = ?3) "
             "ORDER BY ts_us ASC LIMIT %d;",
             (limit > 0 && limit <= 100000) ? limit : 100000);

    if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return -2;
    }

    sqlite3_bind_int64(stmt, 1, from_us);
    sqlite3_bind_int64(stmt, 2, to_us);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)can_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        storage_raw_t rec;
        const void   *blob;
        int           blob_len;

        memset(&rec, 0, sizeof(rec));
        rec.ts_us    = sqlite3_column_int64(stmt, 0);
        rec.can_id   = (uint32_t)sqlite3_column_int64(stmt, 1);
        rec.dlc      = (uint8_t)sqlite3_column_int(stmt, 2);
        rec.is_error = (uint8_t)sqlite3_column_int(stmt, 3);

        blob = sqlite3_column_blob(stmt, 4);
        blob_len = sqlite3_column_bytes(stmt, 4);
        if (blob != NULL && blob_len > 0)
        {
            memcpy(rec.data, blob, (blob_len > 8) ? 8u : (size_t)blob_len);
        }

        count++;
        if (cb(arg, &rec) != 0)
        {
            break;   /* 调用者要求提前终止 */
        }
    }

    sqlite3_finalize(stmt);
    return count;
}

static int query_charge_ex(int64_t from_us, int64_t to_us, int limit,
                           int newest_first, storage_charge_cb cb, void *arg)
{
    sqlite3_stmt *stmt = NULL;
    char          sql[1024];
    int           count = 0;
    int           rc;

    if (!s_opened || cb == NULL)
    {
        return -1;
    }

    snprintf(sql, sizeof(sql),
             "SELECT ts_us, session_id, state, error_code,"
             " bcp_max_single_voltage, bcp_max_current, bcp_nominal_energy,"
             " bcp_max_total_voltage, bcp_max_temperature, bcp_soc, bcp_current_voltage,"
             " bcl_voltage_demand, bcl_current_demand, bcl_charge_mode, bcl_allow_voltage,"
             " bcl_allow_current,"
             " bcs_measure_voltage, bcs_measure_current, bcs_max_single_voltage,"
             " bcs_max_single_no, bcs_current_soc,"
             " bsm_max_temp, bsm_max_temp_no, bsm_min_temp, bsm_min_temp_no,"
             " bsm_fault "
             "FROM charge_data "
             "WHERE (?1 = 0 OR ts_us >= ?1) AND (?2 = 0 OR ts_us <= ?2) "
             "ORDER BY ts_us %s LIMIT %d;",
             newest_first ? "DESC" : "ASC",
             (limit > 0 && limit <= 100000) ? limit : 100000);

    if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        return -2;
    }

    sqlite3_bind_int64(stmt, 1, from_us);
    sqlite3_bind_int64(stmt, 2, to_us);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        storage_charge_t c;
        int i = 0;

        memset(&c, 0, sizeof(c));
        c.ts_us                = sqlite3_column_int64(stmt, i++);
        c.session_id           = (uint32_t)sqlite3_column_int(stmt, i++);
        c.state                = (uint8_t)sqlite3_column_int(stmt, i++);
        c.error_code           = (uint8_t)sqlite3_column_int(stmt, i++);

        c.bcp_max_single_voltage = (float)sqlite3_column_double(stmt, i++);
        c.bcp_max_current        = (float)sqlite3_column_double(stmt, i++);
        c.bcp_nominal_energy     = (float)sqlite3_column_double(stmt, i++);
        c.bcp_max_total_voltage  = (float)sqlite3_column_double(stmt, i++);
        c.bcp_max_temperature    = (float)sqlite3_column_double(stmt, i++);
        c.bcp_soc                = (float)sqlite3_column_double(stmt, i++);
        c.bcp_current_voltage    = (float)sqlite3_column_double(stmt, i++);

        c.bcl_voltage_demand     = (float)sqlite3_column_double(stmt, i++);
        c.bcl_current_demand     = (float)sqlite3_column_double(stmt, i++);
        c.bcl_charge_mode        = (uint8_t)sqlite3_column_int(stmt, i++);
        c.bcl_allow_voltage      = (float)sqlite3_column_double(stmt, i++);
        c.bcl_allow_current      = (float)sqlite3_column_double(stmt, i++);

        c.bcs_measure_voltage    = (float)sqlite3_column_double(stmt, i++);
        c.bcs_measure_current    = (float)sqlite3_column_double(stmt, i++);
        c.bcs_max_single_voltage = (float)sqlite3_column_double(stmt, i++);
        c.bcs_max_single_no      = (uint8_t)sqlite3_column_int(stmt, i++);
        c.bcs_current_soc        = (float)sqlite3_column_double(stmt, i++);

        c.bsm_max_temp           = (float)sqlite3_column_double(stmt, i++);
        c.bsm_max_temp_no        = (uint8_t)sqlite3_column_int(stmt, i++);
        c.bsm_min_temp           = (float)sqlite3_column_double(stmt, i++);
        c.bsm_min_temp_no        = (uint8_t)sqlite3_column_int(stmt, i++);
        c.bsm_fault              = (uint8_t)sqlite3_column_int(stmt, i++);

        count++;
        if (cb(arg, &c) != 0)
        {
            break;
        }
    }

    sqlite3_finalize(stmt);
    return count;
}

int storage_summary(int64_t from_us, int64_t to_us, storage_summary_t *out)
{
    sqlite3_stmt *stmt = NULL;
    static const char *SQL =
        "SELECT COUNT(*), MIN(ts_us), MAX(ts_us),"
        " MIN(bcs_measure_voltage), MAX(bcs_measure_voltage),"
        " MIN(bcs_measure_current), MAX(bcs_measure_current),"
        " MIN(bcs_current_soc),     MAX(bcs_current_soc),"
        " MIN(bsm_min_temp),        MAX(bsm_max_temp) "
        "FROM charge_data "
        "WHERE (?1 = 0 OR ts_us >= ?1) AND (?2 = 0 OR ts_us <= ?2);";

    if (!s_opened || out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (sqlite3_prepare_v2(s_db, SQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        return -2;
    }
    sqlite3_bind_int64(stmt, 1, from_us);
    sqlite3_bind_int64(stmt, 2, to_us);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out->count          = sqlite3_column_int(stmt, 0);
        out->ts_first       = sqlite3_column_int64(stmt, 1);
        out->ts_last        = sqlite3_column_int64(stmt, 2);
        out->voltage_min    = (float)sqlite3_column_double(stmt, 3);
        out->voltage_max    = (float)sqlite3_column_double(stmt, 4);
        out->current_min    = (float)sqlite3_column_double(stmt, 5);
        out->current_max    = (float)sqlite3_column_double(stmt, 6);
        out->soc_min        = (float)sqlite3_column_double(stmt, 7);
        out->soc_max        = (float)sqlite3_column_double(stmt, 8);
        out->temp_min       = (float)sqlite3_column_double(stmt, 9);
        out->temp_max       = (float)sqlite3_column_double(stmt, 10);
    }
    sqlite3_finalize(stmt);

    /* 积分估算充入电量：∫ U * I dt / 3600 / 1000  (kWh)，
     * 用 SQL 自连接相邻两条记录做梯形积分，避免把整表读进内存。 */
    {
        static const char *SQL_ENERGY =
            "SELECT SUM((a.bcs_measure_voltage * a.bcs_measure_current) *"
            "           ((b.ts_us - a.ts_us) / 1000000.0)) "
            "FROM charge_data a JOIN charge_data b ON b.id = ("
            "   SELECT MIN(id) FROM charge_data WHERE id > a.id) "
            "WHERE a.bcs_measure_current > 0 "
            "  AND (?1 = 0 OR a.ts_us >= ?1) AND (?2 = 0 OR a.ts_us <= ?2);";

        if (sqlite3_prepare_v2(s_db, SQL_ENERGY, -1, &stmt, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, from_us);
            sqlite3_bind_int64(stmt, 2, to_us);
            if (sqlite3_step(stmt) == SQLITE_ROW &&
                sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            {
                double joules = sqlite3_column_double(stmt, 0);
                out->energy_kwh = joules / 3600.0 / 1000.0;
            }
            sqlite3_finalize(stmt);
        }
    }

    return 0;
}

int storage_query_charge(int64_t from_us, int64_t to_us,
                         int limit, storage_charge_cb cb, void *arg)
{
    return query_charge_ex(from_us, to_us, limit, 0, cb, arg);
}

int storage_query_charge_last(int limit, storage_charge_cb cb, void *arg)
{
    if (limit <= 0)
    {
        limit = 512;
    }
    return query_charge_ex(0, 0, limit, 1, cb, arg);
}

/*==============================================================================
 *                          CSV 导出
 *============================================================================*/

typedef struct
{
    FILE *fp;
    int   n;
} csv_ctx_t;

static int csv_raw_cb(void *arg, const storage_raw_t *rec)
{
    csv_ctx_t *ctx = (csv_ctx_t *)arg;

    fprintf(ctx->fp,
            "%lld,0x%08X,%u,%u,%02X %02X %02X %02X %02X %02X %02X %02X\n",
            (long long)rec->ts_us, rec->can_id, rec->dlc, rec->is_error,
            rec->data[0], rec->data[1], rec->data[2], rec->data[3],
            rec->data[4], rec->data[5], rec->data[6], rec->data[7]);
    ctx->n++;
    return 0;
}

static int csv_charge_cb(void *arg, const storage_charge_t *c)
{
    csv_ctx_t *ctx = (csv_ctx_t *)arg;

    fprintf(ctx->fp,
            "%lld,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%u,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%u,%.2f,"
            "%.1f,%u,%.1f,%u,%u\n",
            (long long)c->ts_us, c->session_id, c->state, c->error_code,
            c->bcp_max_single_voltage, c->bcp_max_current, c->bcp_nominal_energy,
            c->bcp_max_total_voltage, c->bcp_max_temperature, c->bcp_soc,
            c->bcp_current_voltage,
            c->bcl_voltage_demand, c->bcl_current_demand, c->bcl_charge_mode,
            c->bcl_allow_voltage, c->bcl_allow_current,
            c->bcs_measure_voltage, c->bcs_measure_current,
            c->bcs_max_single_voltage, c->bcs_max_single_no, c->bcs_current_soc,
            c->bsm_max_temp, c->bsm_max_temp_no, c->bsm_min_temp, c->bsm_min_temp_no,
            c->bsm_fault);
    ctx->n++;
    return 0;
}

int storage_export_raw_csv(int64_t from_us, int64_t to_us, const char *csv_path)
{
    csv_ctx_t ctx;
    int       n;

    if (csv_path == NULL)
    {
        return -1;
    }

    ctx.fp = fopen(csv_path, "w");
    if (ctx.fp == NULL)
    {
        return -1;
    }
    ctx.n = 0;

    fprintf(ctx.fp, "ts_us,can_id,dlc,is_error,data\n");
    n = storage_query_raw(from_us, to_us, 0, 0, csv_raw_cb, &ctx);

    fclose(ctx.fp);
    return (n < 0) ? n : ctx.n;
}

int storage_export_charge_csv(int64_t from_us, int64_t to_us, const char *csv_path)
{
    csv_ctx_t ctx;
    int       n;

    if (csv_path == NULL)
    {
        return -1;
    }

    ctx.fp = fopen(csv_path, "w");
    if (ctx.fp == NULL)
    {
        return -1;
    }
    ctx.n = 0;

    fprintf(ctx.fp,
            "ts_us,session,state,err,"
            "bcp_max_v,bcp_max_i,bcp_energy,bcp_max_vtot,bcp_max_t,bcp_soc,bcp_volt,"
            "bcl_v,bcl_i,bcl_mode,bcl_allow_v,bcl_allow_i,"
            "bcs_v,bcs_i,bcs_max_cell,bcs_cell_no,bcs_soc,"
            "bsm_tmax,bsm_tmax_no,bsm_tmin,bsm_tmin_no,bsm_fault\n");

    n = storage_query_charge(from_us, to_us, 0, csv_charge_cb, &ctx);

    fclose(ctx.fp);
    return (n < 0) ? n : ctx.n;
}

int storage_purge_before(int64_t before_us)
{
    sqlite3_stmt *stmt = NULL;
    int           total = 0;

    if (!s_opened || before_us <= 0)
    {
        return -1;
    }

    if (sqlite3_prepare_v2(s_db, "DELETE FROM can_raw WHERE ts_us < ?1;",
                           -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int64(stmt, 1, before_us);
        if (sqlite3_step(stmt) == SQLITE_DONE)
        {
            total += sqlite3_changes(s_db);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(s_db, "DELETE FROM charge_data WHERE ts_us < ?1;",
                           -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int64(stmt, 1, before_us);
        if (sqlite3_step(stmt) == SQLITE_DONE)
        {
            total += sqlite3_changes(s_db);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(s_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
    return total;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
