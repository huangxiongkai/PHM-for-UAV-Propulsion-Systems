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

/* ---- 阶段参数（集中到文件顶部，方便调参） ---- */
#define TAKEOFF_END     3.0f
#define HOVER_END       10.0f
#define CRUISE_END      20.0f

/* 电压参数 */
#define V_START         24.0f
#define V_TAKEOFF_SLOPE -0.1f    /* 起飞阶段电压斜率: -0.1V/s */
#define V_HOVER         23.7f
#define V_CRUISE_SLOPE  -0.02f   /* 巡航阶段电压斜率: -0.02V/s */
#define V_LAND          23.0f

/* 温度参数 */
#define T_START         30.0f
#define T_TAKEOFF_SLOPE 1.0f     /* 起飞阶段温度斜率: +1℃/s */
#define T_HOVER         35.0f
#define T_CRUISE        36.0f
#define T_LAND          35.0f

/* 油门参数 */
#define THR_START       1000u
#define THR_TAKEOFF_END 1200u
#define THR_HOVER       1600u
#define THR_CRUISE      1550u
#define THR_LAND        1200u

/**
 * @brief  获取指定时刻的场景状态
 * @param  t 实验经过时间（秒）
 * @param  s 输出：物理世界状态
 */
void tb_scenario_get_state(float t, TB_PhysicsState *s)
{
    if (t < TAKEOFF_END)
    {
        /* ---- 起飞阶段 [0, 3s) ---- */
        s->voltage = V_START + V_TAKEOFF_SLOPE * t;
        s->temperature = T_START + T_TAKEOFF_SLOPE * t;
        /* 油门线性增加: 1000 → 1200 */
        float ratio = t / TAKEOFF_END;
        s->throttle = THR_START + (uint16_t)((THR_TAKEOFF_END - THR_START) * ratio);
        s->phase = PHASE_TAKEOFF;
    }
    else if (t < HOVER_END)
    {
        /* ---- 悬停阶段 [3s, 10s) ---- */
        s->voltage = V_HOVER;
        s->temperature = T_HOVER;
        s->throttle = THR_HOVER;
        s->phase = PHASE_HOVER;
    }
    else if (t < CRUISE_END)
    {
        /* ---- 巡航阶段 [10s, 20s) ---- */
        float dt = t - HOVER_END;
        s->voltage = V_HOVER + V_CRUISE_SLOPE * dt;
        s->temperature = T_CRUISE;
        s->throttle = THR_CRUISE;
        s->phase = PHASE_CRUISE;
    }
    else
    {
        /* ---- 降落阶段 [20s, ∞) ---- */
        s->voltage = V_LAND;
        s->temperature = T_LAND;
        s->throttle = THR_LAND;
        s->phase = PHASE_LAND;
    }
}