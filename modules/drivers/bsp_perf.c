/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */

/* 必须先包含 bsp_perf.h 以获取 USE_PERF 宏，再用 #ifdef 门控编译单元 */
#include "bsp_perf.h"

#ifdef USE_PERF

#include <rtthread.h>
#include "stm32f1xx_hal.h"

/**
 * @brief  初始化 DWT Cycle Counter
 * @note   开启 Cortex-M3 DWT 调试模块，清零计数器，启用周期计数
 *         Idle hook 仅在 RT-Thread 开启 RT_USING_IDLE_HOOK 或 RT_USING_HOOK 时可用
 */
void perf_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#if defined(RT_USING_IDLE_HOOK) || defined(RT_USING_HOOK)
    rt_thread_idle_sethook(perf_idle_hook);
#endif
}

/**
 * @brief  计算 CPU 周期差值（自动处理 32 位溢出）
 * @param  start_cyc  起始周期计数
 * @param  end_cyc    终止周期计数
 * @return 微秒数
 * @note   利用无符号减法自动处理溢出，72MHz 时 72 cycles = 1 µs
 */
uint32_t perf_diff_us(uint32_t start_cyc, uint32_t end_cyc)
{
    return (end_cyc - start_cyc) / (SystemCoreClock / 1000000u);
}

/* ---- 线程执行时间统计 ---- */
static perf_stat_t g_perf_stats[PERF_THREAD_CNT];

/**
 * @brief  更新线程执行时间统计
 * @param  st   统计结构体指针
 * @param  us   本轮执行时间（微秒）
 * @note   维护 min/max/last_us，avg_us 使用 7/8 + 1/8 指数平滑
 */
void perf_update_stat(perf_stat_t *st, uint32_t us)
{
    st->last_us = us;
    if (us < st->min_us) st->min_us = us;
    if (us > st->max_us) st->max_us = us;
    if (st->cnt == 0)
        st->avg_us = us;
    else
        st->avg_us = (st->avg_us * 7 + us) >> 3;
    if (st->cnt < 0xFFFFFFFFu) st->cnt++;
}

/**
 * @brief  获取线程统计结构体指针
 * @param  thread_id  线程 ID (PERF_ACQUIRE / PREDICT / SUPERVISOR / ACTUATOR)
 * @return 对应线程的 perf_stat_t 指针，越界时返回第 0 项
 */
perf_stat_t* perf_get_stat(int thread_id)
{
    if (thread_id < 0 || thread_id >= PERF_THREAD_CNT)
        return &g_perf_stats[0];
    return &g_perf_stats[thread_id];
}

/* ---- 跨线程时间戳：Event send → recv 延迟 ---- */
static volatile uint32_t g_event_send_cyc = 0;

/**
 * @brief  标记 Event 发送时刻（Supervisor 调用）
 * @note   在 rt_event_send() 前调用，记录当前 DWT 周期计数
 */
void perf_mark_event_send(void)
{
    g_event_send_cyc = perf_get_cyc();
}

/**
 * @brief  计算 Event 响应延迟（Actuator 调用）
 * @return 从 send 到 recv 的微秒数
 * @note   在 rt_event_recv() 后调用，计算与 mark_event_send 的差值
 */
uint32_t perf_get_event_latency_us(void)
{
    return perf_diff_us(g_event_send_cyc, perf_get_cyc());
}

/* ---- 跨线程时间戳：E2E 延迟 ---- */
static volatile uint32_t g_e2e_start_cyc = 0;

/**
 * @brief  标记 E2E 起始时刻（Acquire 调用）
 * @note   在 ADC DMA 中断处理完毕后调用，记录当前 DWT 周期计数
 */
void perf_mark_e2e_start(void)
{
    g_e2e_start_cyc = perf_get_cyc();
}

/**
 * @brief  计算 E2E 延迟（Actuator 调用）
 * @return 从起始到 Actuator 执行的微秒数
 * @note   在 Actuator 线程执行动作后调用，计算与 mark_e2e_start 的差值
 */
uint32_t perf_get_e2e_latency_us(void)
{
    return perf_diff_us(g_e2e_start_cyc, perf_get_cyc());
}

/* ---- Idle hook + CPU 占用率 ---- */
#if defined(RT_USING_IDLE_HOOK) || defined(RT_USING_HOOK)
static volatile uint32_t g_idle_cnt = 0;
static uint32_t g_idle_baseline = 0;
static rt_tick_t g_cpu_last_tick = 0;
static uint32_t g_cpu_usage_bp = 0;
static uint8_t  g_cpu_first = 1;

/**
 * @brief  Idle hook 回调（由 RT-Thread idle 线程自动调用）
 * @note   每次 idle 执行时累加计数器，用于后续计算 CPU 占用率
 */
void perf_idle_hook(void)
{
    g_idle_cnt++;
}

/**
 * @brief  计算 CPU 占用率（基点为单位，10000 = 100.00%）
 * @return CPU 占用率（基点），未启用 idle hook 时返回 0
 * @note   第一秒采集 baseline，后续每秒更新一次
 *         算法：usage = (1 - idle_cnt/baseline) * 10000
 */
uint32_t perf_get_cpu_usage_bp(void)
{
    rt_tick_t now = rt_tick_get();

    if (g_idle_baseline == 0)
    {
        if (g_cpu_first)
        {
            g_cpu_last_tick = now;
            g_idle_cnt = 0;
            g_cpu_first = 0;
            return 0;
        }

        uint32_t elapsed = now - g_cpu_last_tick;
        if (elapsed >= RT_TICK_PER_SECOND)
        {
            g_idle_baseline = g_idle_cnt;
            g_idle_cnt = 0;
            g_cpu_last_tick = now;
            return 0;
        }
        return 0;
    }

    {
        uint32_t elapsed = now - g_cpu_last_tick;
        if (elapsed >= RT_TICK_PER_SECOND)
        {
            uint32_t idle_cnt_copy = g_idle_cnt;
            uint32_t idle_bp;

            g_idle_cnt = 0;
            g_cpu_last_tick = now;

            idle_bp = (uint32_t)((uint64_t)idle_cnt_copy * 10000u / g_idle_baseline);
            if (idle_bp > 10000u) idle_bp = 10000u;
            g_cpu_usage_bp = 10000u - idle_bp;
        }
    }

    return g_cpu_usage_bp;
}
#else
/**
 * @brief  Idle hook 优雅降级（未启用 RT_USING_IDLE_HOOK 时）
 * @note   空实现，不影响其他 perf 功能
 */
void perf_idle_hook(void) { }

/**
 * @brief  CPU 占用率优雅降级（未启用 RT_USING_IDLE_HOOK 时）
 * @return 固定返回 0
 */
uint32_t perf_get_cpu_usage_bp(void) { return 0; }
#endif

/**
 * @brief  自动初始化（由 RT-Thread 组件初始化机制调用）
 * @return 成功返回 0
 * @note   通过 INIT_BOARD_EXPORT 宏在系统启动时自动执行
 */
static int perf_auto_init(void)
{
    perf_init();
    return 0;
}
INIT_BOARD_EXPORT(perf_auto_init);

#endif /* USE_PERF */
