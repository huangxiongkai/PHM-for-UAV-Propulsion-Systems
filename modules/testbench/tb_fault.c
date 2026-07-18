/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */
#include <testbench/tb.h>

/* ---- 故障函数指针类型 ---- */
typedef void (*fault_fn_t)(float elapsed, TB_PhysicsState *s);

/* ---- 故障实现函数 ---- */

/* 无故障 */
static void fault_none(float e, TB_PhysicsState *s)
{
    (void)e;
    (void)s;
}

/* 快速掉压: 5V/s 速率 */
static void fault_volt_drop(float e, TB_PhysicsState *s)
{
    s->voltage -= e * 5.0f;
    if (s->voltage < 10.0f) s->voltage = 10.0f;  /* 保护下限 */
}

/* 快速升温: 3℃/s 速率 */
static void fault_temp_rise(float e, TB_PhysicsState *s)
{
    s->temperature += e * 3.0f;
    if (s->temperature > 200.0f) s->temperature = 200.0f;  /* 保护上限 */
}

/* 欠压故障: 直接强制欠压 */
static void fault_volt_undervolt(float e, TB_PhysicsState *s)
{
    (void)e;
    s->voltage = 14.0f;
}

/* 过温故障: 直接强制过温 */
static void fault_temp_overtemp(float e, TB_PhysicsState *s)
{
    (void)e;
    s->temperature = 120.0f;
}

/* ---- 故障函数指针表 ---- */
static const fault_fn_t g_fault_table[TB_FAULT_COUNT] = {
    [TB_FAULT_NONE]           = fault_none,
    [TB_FAULT_VOLT_FAST_DROP] = fault_volt_drop,
    [TB_FAULT_TEMP_FAST_RISE] = fault_temp_rise,
    [TB_FAULT_ADC_STUCK]      = fault_none,   /* 硬件级，在 tb_core 拦截 */
    [TB_FAULT_NTC_OPEN]       = fault_none,   /* 硬件级，在 tb_core 拦截 */
    [TB_FAULT_VOLT_UNDERVOLT] = fault_volt_undervolt,
    [TB_FAULT_TEMP_OVERTEMP]  = fault_temp_overtemp,
};

/**
 * @brief  应用故障到物理状态
 * @param  t 当前实验时间（秒）
 * @param  start_t 故障开始时间（秒）
 * @param  fault 故障类型
 * @param  s 物理世界状态（输入/输出）
 */
void tb_fault_apply(float t, float start_t, TB_FaultType fault, TB_PhysicsState *s)
{
    if (t < start_t) return;
    if (fault >= TB_FAULT_COUNT) return;
    g_fault_table[fault](t - start_t, s);
}