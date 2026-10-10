/**
 * @file    storage.h
 * @brief   数据存储层 —— SQLite3 本地持久化（WAL 模式 + 批量事务写入）
 *
 * 设计目标
 * --------
 * 1. **不阻塞实时链路**：CAN 接收与协议解析是硬实时路径，绝不能被磁盘 IO 拖慢。
 *    因此存储线程与解析线程通过一个独立的写入队列解耦，
 *    解析结果先入内存队列，存储线程按「每 50 帧或每 1 秒」批量提交。
 * 2. **写入性能**：
 *      - PRAGMA journal_mode = WAL  —— 读写互不阻塞，断电安全性好；
 *      - PRAGMA synchronous  = NORMAL —— WAL 下兼顾性能与安全；
 *      - 显式 BEGIN/COMMIT 批量事务 —— 单条 INSERT 自动提交会产生 fsync 风暴，
 *        批量提交可把 1000 条/s 的写入开销降低一个数量级。
 * 3. **两张表**：
 *      - can_raw    原始 CAN 报文（时间戳、CAN ID、DLC、原始 8 字节）
 *      - charge_data 解析后的物理量（总电压、总电流、SOC、温度、状态机状态…）
 * 4. **查询接口**：按时间范围 / 按 CAN ID 查询历史数据，支持导出 CSV。
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 默认数据库文件路径 */
#define STORAGE_DEFAULT_DB   "./gb27930_data.db"

/** 批量提交阈值：任一条件满足即落盘 */
#define STORAGE_BATCH_FRAMES   50      /**< 累计 50 帧 */
#define STORAGE_BATCH_MS       1000    /**< 或距上次提交 1000 ms */

/** 写入队列容量（帧） */
#define STORAGE_QUEUE_SIZE     4096u

/*==============================================================================
 *                              记录结构
 *============================================================================*/

/** 原始 CAN 报文记录 */
typedef struct
{
    int64_t  ts_us;        /**< 微秒时间戳（自 Unix 纪元，取自内核 SO_TIMESTAMPNS） */
    uint32_t can_id;       /**< 29 位扩展帧 ID */
    uint8_t  dlc;          /**< 数据长度 */
    uint8_t  is_error;     /**< 1 = 错误帧 */
    uint8_t  data[8];      /**< 原始数据 */
} storage_raw_t;

/** 解析后的充电数据记录（与充电机侧状态机一一对应） */
typedef struct
{
    int64_t  ts_us;              /**< 微秒时间戳 */

    /* --- 来自 BCP 充电参数（配置阶段一次性解析） --- */
    float    bcp_max_single_voltage;   /**< 单体最高允许充电电压 (V) */
    float    bcp_max_current;          /**< 最高允许充电电流 (A) */
    float    bcp_nominal_energy;       /**< 动力蓄电池标称总能量 (kWh) */
    float    bcp_max_total_voltage;    /**< 最高允许充电总电压 (V) */
    float    bcp_max_temperature;      /**< 最高允许温度 (℃) */
    float    bcp_soc;                  /**< 当前荷电状态 SOC (%) */
    float    bcp_current_voltage;      /**< 当前电池电压 (V) */

    /* --- 来自 BCL 电池充电需求（充电中周期性） ---
     * ★ 标准表 17 的 BCL 只有 5 字节 3 个字段，**没有**"允许充电电压/电流"。
     *   下面两列保留是为了不动数据库 schema，语义已改为
     *   **从 BCP 取的"本次会话被允许的充电上限"**（SPN2819 / SPN2817），
     *   未收到 BCP 时为 0。写库代码见 gb27930_fill_storage_record()。 */
    float    bcl_voltage_demand;       /**< 电压需求 (V) */
    float    bcl_current_demand;       /**< 电流需求 (A) */
    uint8_t  bcl_charge_mode;          /**< 充电模式 1=恒压 2=恒流 */
    float    bcl_allow_voltage;        /**< 允许充电电压 (V) —— **实为 BCP 的 SPN2819** */
    float    bcl_allow_current;        /**< 允许充电电流 (A) —— **实为 BCP 的 SPN2817** */

    /* --- 来自 BCS 电池充电总状态 --- */
    float    bcs_measure_voltage;      /**< 充电电压测量值 (V) */
    float    bcs_measure_current;      /**< 充电电流测量值 (A) */
    float    bcs_max_single_voltage;   /**< 最高单体电压 (V)，BCS 的 B5-B6 低 12 位 */
    uint8_t  bcs_max_single_no;        /**< ★ 列名是"最高单体编号"，但标准表 18 的 BCS
                                        *   **没有单体编号字段** —— B5-B6 的 13-16 位是
                                        *   "最高单体**所在组号**"。本列实际存的就是该组号
                                        *   （1/位，0 偏移，0~15）。
                                        *   列名保持不变以避免 schema 迁移，**查库时勿误读**。 */
    float    bcs_current_soc;          /**< 当前 SOC (%)，BCS 的 **1 %/位** */

    /* --- 来自 BSM 动力蓄电池状态信息 --- */
    float    bsm_max_temp;             /**< 最高温度 (℃) */
    uint8_t  bsm_max_temp_no;          /**< 最高温度检测点编号（1 偏移） */
    float    bsm_min_temp;             /**< 最低温度 (℃) */
    uint8_t  bsm_min_temp_no;          /**< 最低温度检测点编号（1 偏移） */
    uint8_t  bsm_fault;                /**< ★ 列名是"故障标志位"，但标准表 20 的 BSM
                                        *   **没有故障标志位这个字段**，只有 6 个 2 位状态
                                        *   字段。本列实际存的是 **B6 的原始字节**
                                        *   （SPN3090 单体电压 / SPN3091 SOC /
                                        *     SPN3092 过流 / SPN3093 过温，各占 2 位）。
                                        *   B7（SPN3094 绝缘 / SPN3095 连接器 /
                                        *   SPN3096 充电允许）不入库，见 ctx->bsm.status_b7。
                                        *   列名保持不变以避免 schema 迁移，**查库时勿误读**。 */

    /* --- 状态机上下文 --- */
    uint8_t  state;                    /**< 充电机侧状态机状态 */
    uint8_t  error_code;               /**< 最近一次异常码 */
    uint32_t session_id;               /**< 充电会话编号 */
} storage_charge_t;

/** 历史查询回调：返回非 0 可提前终止遍历 */
typedef int (*storage_raw_cb)(void *arg, const storage_raw_t *rec);
typedef int (*storage_charge_cb)(void *arg, const storage_charge_t *rec);

/*==============================================================================
 *                              生命周期
 *============================================================================*/

/**
 * @brief  打开 / 创建数据库并建表
 * @param  path 数据库文件路径，NULL 使用 STORAGE_DEFAULT_DB
 * @return 0 成功；-1 打开失败；-2 建表失败
 */
int  storage_open(const char *path);

/**
 * @brief  关闭数据库（会先 flush 所有缓存记录）
 */
void storage_close(void);

/**
 * @brief  当前数据库是否可用
 */
int  storage_is_open(void);

/*==============================================================================
 *                              写入接口
 *============================================================================*/

/**
 * @brief  缓存一条原始 CAN 报文
 * @return 0 成功；-1 未打开；-2 队列满（已丢弃并计数）
 * @note   内部无锁环形队列，可从多个线程并发调用
 */
int  storage_push_raw(const storage_raw_t *rec);

/**
 * @brief  缓存一条解析后的充电数据
 */
int  storage_push_charge(const storage_charge_t *rec);

/**
 * @brief  立即提交所有缓存（用于优雅退出、关键时刻落盘）
 * @return 提交的记录总数
 */
int  storage_flush(void);

/**
 * @brief  周期性调用：检查是否满足「50 帧或 1 秒」批量提交条件
 * @return 本次提交的帧数（0 表示未触发提交）
 * @note   由存储线程每 100 ms 调用一次即可。
 */
int  storage_tick(void);

/** 统计量 */
uint64_t storage_raw_written(void);      /**< 累计写入原始帧数 */
uint64_t storage_charge_written(void);   /**< 累计写入解析记录数 */
uint64_t storage_raw_dropped(void);      /**< 因队列满丢弃的原始帧数 */
uint64_t storage_charge_dropped(void);   /**< 因队列满丢弃的解析记录数 */
uint64_t storage_commit_count(void);     /**< 累计 COMMIT 次数 */
uint64_t storage_db_size_bytes(void);    /**< 数据库文件大小 */

/*==============================================================================
 *                              查询接口
 *============================================================================*/

/**
 * @brief  按时间范围查询原始 CAN 报文
 * @param  from_us 起始微秒时间戳（含），0 表示不限
 * @param  to_us   结束微秒时间戳（含），0 表示不限
 * @param  can_id  指定 CAN ID，0 表示不限
 * @param  limit   最多返回条数（<=0 表示不限，但内部上限 100000）
 * @param  cb      回调
 * @param  arg     回调参数
 * @return >=0 实际返回条数；<0 错误
 */
int  storage_query_raw(int64_t from_us, int64_t to_us, uint32_t can_id,
                       int limit, storage_raw_cb cb, void *arg);

/**
 * @brief  按时间范围查询解析后的充电数据（按时间升序）
 */
int  storage_query_charge(int64_t from_us, int64_t to_us,
                          int limit, storage_charge_cb cb, void *arg);

/**
 * @brief  取「最近 N 条」解析记录（按时间降序，最新的先回调）
 * @param  limit 最多返回条数（<=0 时内部按 512 处理，上限 100000）
 * @note   历史曲线界面用这个接口：不关心起始时间，只要最近的一段，
 *         比「全表扫描再截断」快得多（走 idx_charge_ts 索引倒序）。
 */
int  storage_query_charge_last(int limit, storage_charge_cb cb, void *arg);

/**
 * @brief  统计时间范围内的充电数据摘要（最大/最小电压、电流、SOC、温度）
 */
typedef struct
{
    int      count;
    float    voltage_min, voltage_max;   /**< 基于 BCS 充电电压测量值 */
    float    current_min, current_max;   /**< 基于 BCS 充电电流测量值 */
    float    soc_min,     soc_max;
    float    temp_min,    temp_max;
    int64_t  ts_first,    ts_last;
    double   energy_kwh;                 /**< 积分估算充入电量 (kWh) */
} storage_summary_t;

int  storage_summary(int64_t from_us, int64_t to_us, storage_summary_t *out);

/**
 * @brief  导出时间范围内的原始报文为 CSV
 * @return >=0 导出行数；<0 错误
 */
int  storage_export_raw_csv(int64_t from_us, int64_t to_us, const char *csv_path);

/**
 * @brief  导出时间范围内的解析数据为 CSV
 */
int  storage_export_charge_csv(int64_t from_us, int64_t to_us, const char *csv_path);

/**
 * @brief  清理早于指定时间的历史数据（配合定期维护任务）
 * @return >=0 删除行数
 */
int  storage_purge_before(int64_t before_us);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
