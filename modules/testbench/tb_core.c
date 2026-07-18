    /*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */
#include <rtthread.h>
#include <testbench/tb.h>

/* ---- NTC 温度-ADC 查找表（与 mid_filter.c 一致） ---- */
#define T_LUT_LEN   17
static const uint16_t t_adc_lut[T_LUT_LEN] = { 0, 256, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304, 2560, 2816, 3072, 3328, 3584, 3840, 4095 };
static const float    t_val_lut[T_LUT_LEN] = { -40.0f, -25.5f, -13.2f, -4.8f, 2.2f, 8.2f, 13.9f, 19.5f, 25.0f, 30.8f, 37.0f, 43.9f, 52.0f, 62.1f, 76.4f, 101.8f, 250.0f };

/* ---- 全局状态 ---- */
static float           g_exp_time = 0;       // 实验时钟（秒）：tb_tick() 每次累加 dt，故障注入的时间基准
static rt_tick_t       g_last_tick = 0;      // 上次 tb_tick() 调用时的 rt_tick，用于计算时间差 dt
static uint8_t         g_active = 0;         // Testbench 激活标志：1=故障注入开启，0=透传真实 ADC
static TB_FaultType    g_cur_fault;          // 当前实验的故障类型（编译时由 TB_EXPERIMENT_FAULT 宏决定）
static float           g_fault_start_s;      // 故障启动时刻（秒）：g_exp_time >= 此值时开始劫持 ADC
static TB_ExperimentInfo g_exp_info;         // 实验元数据：实验ID、场景ID、故障类型、启动时刻（供 CSV 日志使用）

/* ---- 故障类型名称（全局，供 tb_logger.c 通过 extern 引用） ---- */
const char *tb_fault_name[] = {
    "None", "VoltDrop", "TempRise", "AdcStuck", "NtcOpen", "UltVolt", "OverTemp"
};

/**
 * @brief  电压值 → ADC 码值反向映射
 * @param  v 电压值 (V)，范围 [15.0, 26.0]
 * @return ADC 码值 [0, 4095]
 */
static uint16_t volt_to_raw(float v)
{
    /* 线性映射: 15V → 0, 26V → 4095 */
    float raw_f = (v - 15.0f) / 11.0f * 4095.0f;
    if (raw_f < 0.0f) raw_f = 0.0f;
    if (raw_f > 4095.0f) raw_f = 4095.0f;
    return (uint16_t)raw_f;
}

/**
 * @brief  温度值 → ADC 码值反向映射（NTC 反转后）
 * @param  t 温度值 (℃)，范围 [-40.0, 250.0]
 * @return ADC 码值 [0, 4095]（注意：返回的是反转前的码值）
 */
static uint16_t temp_to_raw(float t)
{
    /* 1. 边界处理 */
    if (t <= -40.0f) return 4095;  /* 极低温度 → 高 ADC */
    if (t >= 250.0f) return 0;    /* 极高温度 → 低 ADC */

    /* 2. 在 t_val_lut 上反向搜索 t 所在区间 */
    int lo = 0, hi = T_LUT_LEN - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (t < t_val_lut[mid])
            hi = mid;
        else
            lo = mid;
    }

    /* 3. 线性插值得到 adc_val（反转后的 ADC 值） */
    float span_val = t_val_lut[hi] - t_val_lut[lo];
    float frac = (t - t_val_lut[lo]) / span_val;
    float adc_val_f = (float)t_adc_lut[lo] + (float)(t_adc_lut[hi] - t_adc_lut[lo]) * frac;

    /* 4. 反转：NTC 发热 → 电阻降低 → ADC 下降，所以 raw = 4095 - adc_val */
    float raw_f = 4095.0f - adc_val_f;
    if (raw_f < 0.0f) raw_f = 0.0f;
    if (raw_f > 4095.0f) raw_f = 4095.0f;
    return (uint16_t)raw_f;
}

/**
 * @brief  Testbench 初始化（INIT_APP_EXPORT 自动调用）
 * @return 0=成功
 */
int tb_init(void)
{
    /* 读取编译时配置宏 */
    g_cur_fault = TB_EXPERIMENT_FAULT;
    g_fault_start_s = TB_FAULT_START_S;

    /* 设置实验元数据 */
    g_exp_info.experiment_id = TB_EXP_ID;
    g_exp_info.scenario_id = TB_SCN_ID;
    g_exp_info.fault = g_cur_fault;
    g_exp_info.fault_start_s = g_fault_start_s;

    /* 判断是否启用 testbench */
    if (g_cur_fault == TB_FAULT_NONE)
    {
        g_active = 0;
        rt_kprintf("[TB] Testbench DISABLED (FAULT_NONE)\r\n");
    }
    else
    {
        g_active = 1;
        g_last_tick = rt_tick_get();
        {
            int si = (int)g_fault_start_s;
            int sd = (int)((g_fault_start_s - si) * 10);
            if (sd < 0) sd = -sd;
            rt_kprintf("[TB] EXP=%d, Fault=%s, Start=%d.%ds\r\n",
                       g_exp_info.experiment_id,
                       tb_fault_name[g_cur_fault],
                       si, sd);
        }
    }

    return 0;
}
INIT_APP_EXPORT(tb_init);

/**
 * @brief  判断 testbench 是否激活
 * @return 1=激活, 0=未激活
 */
uint8_t tb_is_active(void)
{
    return g_active;
}

/**
 * @brief  推进实验时间（基于 rt_tick_get() 差值）
 *         在 acquire 线程中调用
 */
void tb_tick(void)
{
    if (!g_active) return;

    rt_tick_t now = rt_tick_get();
    float dt = (float)(now - g_last_tick) / RT_TICK_PER_SECOND;
    g_last_tick = now;
    g_exp_time += dt;
}

/**
 * @brief  获取实验经过时间（秒）
 */
float tb_get_time(void)
{
    return g_exp_time;
}

/**
 * @brief  获取实验元数据
 */
const TB_ExperimentInfo* tb_get_experiment_info(void)
{
    return &g_exp_info;
}

/**
 * @brief  劫持电压 ADC 码值
 * @param  real 真实 ADC 码值
 * @return 劫持后的 ADC 码值
 */
uint16_t tb_read_volt_raw(uint16_t real)
{
    if (!g_active) return real;  /* 双保险 */

    TB_PhysicsState state;
    tb_scenario_get_state(g_exp_time, &state);

    /* 硬件级故障：ADC 卡死 → 返回固定码值（看起来正常） */
    if (g_cur_fault == TB_FAULT_ADC_STUCK && g_exp_time >= g_fault_start_s)
    {
        return volt_to_raw(24.0f);
    }

    /* 物理量级故障：通过 fault 引擎叠加 */
    tb_fault_apply(g_exp_time, g_fault_start_s, g_cur_fault, &state);

    return volt_to_raw(state.voltage);
}

/**
 * @brief  劫持温度 ADC 码值
 * @param  real 真实 ADC 码值
 * @return 劫持后的 ADC 码值
 */
uint16_t tb_read_temp_raw(uint16_t real)
{
    if (!g_active) return real;  /* 双保险 */

    TB_PhysicsState state;
    tb_scenario_get_state(g_exp_time, &state);

    /* 硬件级故障：NTC 开路 → 返回极端低码值 */
    if (g_cur_fault == TB_FAULT_NTC_OPEN && g_exp_time >= g_fault_start_s)
    {
        return 0;
    }

    /* 物理量级故障：通过 fault 引擎叠加 */
    tb_fault_apply(g_exp_time, g_fault_start_s, g_cur_fault, &state);

    return temp_to_raw(state.temperature);
}
