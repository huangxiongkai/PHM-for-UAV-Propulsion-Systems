/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */
#ifndef MODULES_DRIVERS_BSP_PERF_H_
#define MODULES_DRIVERS_BSP_PERF_H_

/* ---- 数据测试 开关 ---- */
/* 由 bsp_perf.h 统一管理，注释此行即可关闭性能统计，Release 零开销 */
#define USE_PERF

#include <stdint.h>
#include <rtthread.h>

#ifdef USE_PERF

#include "stm32f1xx_hal.h"

/* ---- 线程统计 ID ---- */
#define PERF_ACQUIRE    0
#define PERF_PREDICT    1
#define PERF_SUPERVISOR 2
#define PERF_ACTUATOR   3
#define PERF_THREAD_CNT 4

/* ---- 统计结构体 ---- */
/**
 * @brief  线程执行时间统计结构体
 * @note   记录平均/最小/最大/最后一次执行时间，单位微秒
 */
typedef struct {
    uint32_t avg_us;   /* 指数平滑后的平均执行时间 */
    uint32_t min_us;   /* 历史最小值 */
    uint32_t max_us;   /* 历史最大值 */
    uint32_t last_us;  /* 最近一次执行时间 */
    uint32_t cnt;      /* 累积计数（用于 avg 计算） */
} perf_stat_t;

/**
 * @brief  初始化 DWT 计数器
 * @note   开启 Cortex-M3 DWT Cycle Counter，用于微秒级计时
 */
void perf_init(void);

/* ---- DWT 读取 ---- */
/**
 * @brief  读取 DWT Cycle Counter 当前值
 * @return 当前周期计数
 * @note   72MHz 时 72 计数 = 1 微秒
 */
static inline uint32_t perf_get_cyc(void)
{
    return DWT->CYCCNT;
}

/**
 * @brief  周期转为微秒
 * @param  cyc  周期计数
 * @return 微秒数
 * @note   基于 SystemCoreClock 自动计算
 */
static inline uint32_t perf_cyc_to_us(uint32_t cyc)
{
    return cyc / (SystemCoreClock / 1000000u);
}

/**
 * @brief  计算两个周期计数之间的微秒差
 * @param  start_cyc  起始周期
 * @param  end_cyc    终止周期
 * @return 微秒差
 * @note   自动处理 32 位溢出（无符号减法）
 */
uint32_t perf_diff_us(uint32_t start_cyc, uint32_t end_cyc);

/* ---- 统计更新 ---- */
/**
 * @brief  更新线程统计
 * @param  st   统计结构体指针
 * @param  us   本轮执行时间（微秒）
 * @note   自动更新 min/max/avg/last/cnt
 */
void perf_update_stat(perf_stat_t *st, uint32_t us);

/**
 * @brief  获取指定线程的统计结构体
 * @param  thread_id  线程 ID (0-3)
 * @return 统计结构体指针
 */
perf_stat_t* perf_get_stat(int thread_id);

/* ---- 跨线程时间戳 ---- */
/**
 * @brief  标记 Event 发送时刻（Supervisor 调用）
 * @note   在 rt_event_send() 前调用
 */
void perf_mark_event_send(void);

/**
 * @brief  获取 Event 响应延迟（Actuator 调用）
 * @return 从 send 到 recv 的微秒数
 * @note   在 rt_event_recv() 后调用
 */
uint32_t perf_get_event_latency_us(void);

/**
 * @brief  标记 E2E 起始时刻（Acquire 调用）
 * @note   在 ADC DMA 处理完成后调用
 */
void perf_mark_e2e_start(void);

/**
 * @brief  获取 E2E 延迟（Actuator 调用）
 * @return 从起始到执行完成的微秒数
 * @note   在 Actuator 执行动作后调用
 */
uint32_t perf_get_e2e_latency_us(void);

/* ---- Idle hook / CPU 占用率 ---- */
/**
 * @brief  Idle hook 回调
 * @note   由 RT-Thread idle 线程自动调用，统计 CPU 空闲时间
 */
void perf_idle_hook(void);

/**
 * @brief  获取 CPU 占用率
 * @return 占用率（基点为单位，10000 = 100.00%）
 * @note   需启用 RT_USING_IDLE_HOOK，否则返回 0
 */
uint32_t perf_get_cpu_usage_bp(void);

#else /* USE_PERF 未定义：所有接口退化为空操作，零开销 */

typedef struct {
    uint32_t avg_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t last_us;
    uint32_t cnt;
} perf_stat_t;

#define PERF_ACQUIRE    0
#define PERF_PREDICT    1
#define PERF_SUPERVISOR 2
#define PERF_ACTUATOR   3
#define PERF_THREAD_CNT 4

static inline void perf_init(void) {}
static inline uint32_t perf_get_cyc(void) { return 0; }
static inline uint32_t perf_cyc_to_us(uint32_t cyc) { (void)cyc; return 0; }
static inline uint32_t perf_diff_us(uint32_t s, uint32_t e) { (void)s; (void)e; return 0; }
static inline void perf_update_stat(perf_stat_t *st, uint32_t us) { (void)st; (void)us; }

static perf_stat_t g_perf_dummy = {0, 0, 0, 0, 0};
static inline perf_stat_t* perf_get_stat(int id) { (void)id; return &g_perf_dummy; }

static inline void perf_mark_event_send(void) {}
static inline uint32_t perf_get_event_latency_us(void) { return 0; }
static inline void perf_mark_e2e_start(void) {}
static inline uint32_t perf_get_e2e_latency_us(void) { return 0; }
static inline void perf_idle_hook(void) {}
static inline uint32_t perf_get_cpu_usage_bp(void) { return 0; }

#endif /* USE_PERF */

#endif /* MODULES_DRIVERS_BSP_PERF_H_ */
