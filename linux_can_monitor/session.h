/**
 * @file    session.h
 * @brief   充电历史会话存储（i.MX 本地 SQLite）
 *
 * 需求
 * ----
 *   每次充电结束后，把这一次的曲线和统计参数存到本地；
 *   最多保留最近 9 次（不含正在进行的这一次），超了就把最远的一次挤掉。
 *   历史界面用九宫格展示这 9 次的缩略图，点进去看完整曲线 + 参数。
 *
 * 为什么单独一个连接
 * ------------------
 *   storage.c 已经持有主连接（WAL 模式）。SQLite 在 WAL 下支持多连接并发读，
 *   这里自己开一个连接，好处是：
 *     · 不必把 sqlite3 的类型暴露到 storage.h（那个头被很多文件包含）
 *     · 历史表的读写和主流程完全解耦，出问题不会影响实时采集
 *   代价只是一点点内存。
 *
 * 曲线怎么存
 * ----------
 *   一条会话的曲线压成一个紧凑的 BLOB：每个采样点 8 字节
 *     [0..1] SOC   0.1%（大端 uint16）
 *     [2..3] 电压  0.1V
 *     [4..5] 电流  0.1A
 *     [6]    最高温度 ℃（+50 偏移，和国标一致）
 *     [7]    保留
 *   300 点 = 2400 字节，9 次一共 21 KB 左右，读九宫格时全部载入毫无压力。
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 历史里最多保留几次充电（不含正在进行的这一次） */
#define SESSION_MAX         9
/** 一次会话最多保存多少个采样点 */
#define SESSION_POINTS      300
/** BLOB 里每个采样点的字节数 */
#define SESSION_PT_BYTES    8

/** 一次充电会话的元信息（曲线另有存储） */
typedef struct
{
    int      id;            /**< 数据库自增主键 */
    int64_t  start_us;      /**< 开始时间（Unix 微秒） */
    int64_t  end_us;        /**< 结束时间（Unix 微秒） */
    int      soc_start_x10; /**< 起始 SOC，0.1% */
    int      soc_end_x10;   /**< 结束 SOC，0.1% */
    int      energy_wh;     /**< 本次充电电量，Wh */
    int      charge_sec;    /**< 充电时长，秒 */
    int      full;          /**< 1 = 这次是充满结束的 */
    int      n;             /**< 曲线点数 */
} session_info_t;

/**
 * @brief  打开历史库并建表，同时做一次"只保留最近 N 次"的清理
 * @param  db_path 主数据库路径（和 storage.c 用的是同一个文件）
 * @return 0 成功，-1 失败
 */
int session_init(const char *db_path);

/** 关闭连接 */
void session_close(void);

/** 是否可用 */
int session_ready(void);

/**
 * @brief  保存一次充电会话（曲线 + 参数）
 * @param  info      元信息（id / n 由函数填写）
 * @param  soc_x10   曲线：SOC，0.1%
 * @param  v_x10     曲线：总电压，0.1V
 * @param  i_x10     曲线：总电流，0.1A
 * @param  temp_c    曲线：最高温度，℃
 * @param  n         采样点数
 * @return 0 成功，-1 失败
 * @note   保存成功后自动把超出 SESSION_MAX 的最旧记录删掉。
 */
int session_save(session_info_t *info,
                 const int *soc_x10, const int *v_x10,
                 const int *i_x10, const int *temp_c, int n);

/**
 * @brief  读最近若干次会话的元信息
 * @param  out 输出数组
 * @param  max 最多读几个
 * @return 实际条数（新的在前，即 out[0] 是最近一次）
 */
int session_list(session_info_t *out, int max);

/** 历史条数 */
int session_count(void);

/**
 * @brief  载入某一次会话的完整曲线
 * @param  id        会话 id
 * @param  soc_x10   输出：SOC 曲线（容量 >= max）
 * @param  v_x10     输出：电压曲线
 * @param  i_x10     输出：电流曲线
 * @param  temp_c    输出：温度曲线
 * @param  max       输出缓冲能放几个点
 * @param  info      输出：元信息，可为 NULL
 * @return 实际点数；失败返回 -1
 */
int session_load_curve(int id,
                       int *soc_x10, int *v_x10, int *i_x10, int *temp_c,
                       int max, session_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
