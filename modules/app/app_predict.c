/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-28     12811       the first version
 */
#include "app_predict.h"

#define PREDICT_DT  0.005f   /* 200Hz 采样周期 (s) */
#define K_TEMP      2.0f     /* 温度风险释放斜率 */
#define K_DROP      5.0f     /* 压降风险释放斜率 */

/**
 * @brief 浮点数硬限幅器 — 将 val 限制在 [lo, hi] 闭区间内
 * @param val  输入值
 * @param lo   下限
 * @param hi   上限
 * @return     clamp(lo, val, hi)
 * @note      用于微分后 SlewLimit 掐断EMI尖峰放大，以及风险贡献上限截断
 */
static inline float float_clamp(float val, float lo, float hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

//任务内容拆解：
//1.消息队列永久等待，作为数据接收标志
//2.互斥量用于读取数据
//3.计算部分，并进行微分处理，输出预测（微分）值，用以生成预测数据

extern rt_mutex_t sensor_mutex;

static rt_thread_t Logic_thread3 = RT_NULL;

void Logic_thread3_entry(void *parameter)
{
    static uint8_t  first_frame = 1;         //冷启动标志
    static uint16_t stuck_cnt = 0;           //ADC卡死计数器
    static uint8_t  first_write_done = 0;    //首次写入完成标志

    /* ---------- 滤波状态变量（按管线阶段排列） ---------- */
    /* Stage 3: 滑动窗口 (Median3) */
    static float t_win[3];                    //温度滑窗: [0]最旧—[2]最新, 帧步进
    static float v_win[3];                    //电压滑窗: 同上

    /* Stage 4: 温度特征提取链 */
    static float t_smooth;                    //温度一次IIR平滑值(兼作滤波器状态)
    static float t_smooth_prev;               //上一帧t_smooth,用于微分: (t_smooth - t_smooth_prev)/DT
    static float dt_iir_state;                //温度微分二次IIR状态,对dt_raw限幅后的信号再平滑

    /* Stage 5: 电压特征提取链(双时间尺度) */
    static float v_fast;                      //快尺度IIR值(α=0.15),跟踪瞬态内阻压降
    static float v_fast_prev;                 //上一帧v_fast,用于微分: (v_fast - v_fast_prev)/DT
    static float dv_iir_state;                //电压微分二次IIR状态
    static float v_ref;                       //慢尺度IIR值(gamma由油门冻结逻辑控制),跟踪SOC稳态基准

    /* 油门冻结状态 */
    static uint16_t freeze_cnt = 0;

    monitor_msg_t msg_local;   // SAFE: 局部接收缓冲区

    while (1)
    {
        if (rt_mq_recv(&monitor_mq, &msg_local, sizeof(msg_local), RT_WAITING_FOREVER) != RT_EOK)
            continue;

        float raw_temp = msg_local.temperature;
        float raw_volt = msg_local.voltage;

        /* ==================== 1. 冷启动保护 ==================== */
        if (first_frame)
        {
            t_win[0] = t_win[1] = t_win[2] = raw_temp;
            v_win[0] = v_win[1] = v_win[2] = raw_volt;
            t_smooth = t_smooth_prev = raw_temp;
            dt_iir_state = 0.0f;
            v_fast = v_fast_prev = raw_volt;
            dv_iir_state = 0.0f;
            v_ref = raw_volt;
            freeze_cnt = 0;
            first_frame = 0;
            rt_kprintf("[PREDICT] Cold-start T=%.1f V=%.1f\r\n", raw_temp, raw_volt);
            continue;
        }

        /* ==================== 1.5 传递 acquire 层传感器故障 ==================== */
        if (msg_local.sensor_fault != 0)
        {
            rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
            monitor_msg.sensor_fault = msg_local.sensor_fault;
            rt_mutex_release(sensor_mutex);
            continue;
        }

        /* ==================== 2. 传感器健康自检 ==================== */
        uint8_t fault_flag = 0;

        /* 2a. 范围检查：温度 + 电压 */
        if (raw_temp < P.temp_fault_lo || raw_temp > P.temp_fault_hi ||
            raw_volt < P.volt_fault_lo || raw_volt > P.volt_fault_hi)
        {
            fault_flag = 1;
            stuck_cnt = 0;
        }
        else
        {
            /* 2b. ADC 卡死检测（阈值收紧：仅真正死机才触发） */
            float dv = raw_volt - v_fast;
            float dt = raw_temp - t_smooth;

            if (dv > -0.0001f && dv < 0.0001f && dt > -0.001f && dt < 0.001f)
            {
                stuck_cnt++;
                if (stuck_cnt >= P.stuck_threshold)
                {
                    fault_flag = 2;
                }
            }
            else
            {
                stuck_cnt = 0;
            }
        }

        /* 2c. 传感器故障：加锁写回后跳过本帧 */
        if (fault_flag != 0)
        {
            rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
            monitor_msg.sensor_fault = fault_flag;
            rt_mutex_release(sensor_mutex);
            continue;
        }

        /* ==================== 3. 滑动窗口 + Median3 ==================== */
        t_win[0] = t_win[1]; t_win[1] = t_win[2]; t_win[2] = raw_temp;
        v_win[0] = v_win[1]; v_win[1] = v_win[2]; v_win[2] = raw_volt;

        float med_t = median3(t_win[0], t_win[1], t_win[2]);
        float med_v = median3(v_win[0], v_win[1], v_win[2]);

        /* ==================== 4. 温度特征提取链 ==================== */
        uint8_t hard_fault = 0;

        /* 4a. 一次IIR平滑 */
        t_smooth = iir_lpf(med_t, &t_smooth, P.temp_iir_alpha);

        /* 4b. 温度硬故障检测 */
        if (t_smooth > 105.0f)
            hard_fault |= 0x01;

        /* 4c. 离散微分 → SlewLimit → 二次IIR */
        float dt_raw = (t_smooth - t_smooth_prev) / PREDICT_DT;
        t_smooth_prev = t_smooth;

        float dt_clamp = float_clamp(dt_raw, -P.temp_slew_limit, P.temp_slew_limit);
        float dt_final = iir_lpf(dt_clamp, &dt_iir_state, P.temp_diff_alpha);

        /* 4d. 温度风险贡献 */
        float temp_risk_raw = 0.0f;
        if (dt_final > P.temp_risk_slope)
            temp_risk_raw = (dt_final - P.temp_risk_slope) * K_TEMP;
        float final_temp_risk = float_clamp(temp_risk_raw, 0.0f, P.risk_cap);

        /* ==================== 5. 电压特征提取链（双时间尺度） ==================== */

        /* 5a. 快尺度IIR */
        v_fast_prev = v_fast;
        v_fast = iir_lpf(med_v, &v_fast, P.v_fast_alpha);

        /* 5b. 电压硬故障检测 */
        if (v_fast < 16.0f)
            hard_fault |= 0x02;

        /* 5c. 电压微分 → SlewLimit → 二次IIR */
        float dv_raw = (v_fast - v_fast_prev) / PREDICT_DT;
        float dv_clamp = float_clamp(dv_raw, -P.volt_slew_limit, P.volt_slew_limit);
        float dv_final = iir_lpf(dv_clamp, &dv_iir_state, P.v_diff_alpha);

        /* 5d. 油门联动双阈值迟滞冻结逻辑 */
        static uint8_t is_frozen = 0;
        uint16_t thr = throttle_read();
        float gamma;

        if (!is_frozen && thr > 1700)
            is_frozen = 1;
        else if (is_frozen && thr < 1600)
            is_frozen = 0;

        if (is_frozen)
        {
            freeze_cnt++;
            if (freeze_cnt < 1000)
                gamma = P.v_ref_freeze_alpha;
            else
                gamma = P.v_ref_longhold_alpha;
        }
        else
        {
            freeze_cnt = 0;
            gamma = P.v_ref_release_alpha;
        }

        /* 5e. 慢尺度IIR（参考电压跟踪SOC） */
        v_ref = iir_lpf(med_v, &v_ref, gamma);

        /* 5f. 归一化压降比 */
        float drop_ratio = 0.0f;
        if (v_ref > 1.0f)
            drop_ratio = (v_ref - v_fast) / v_ref;
        drop_ratio = float_clamp(drop_ratio, 0.0f, 1.0f);

        /* 5g. 电压风险贡献 */
        float drop_risk_raw = 0.0f;
        if (drop_ratio > P.drop_risk_threshold)
            drop_risk_raw = (drop_ratio - P.drop_risk_threshold) * 100.0f * K_DROP;
        float final_drop_risk = float_clamp(drop_risk_raw, 0.0f, P.risk_cap);

        /* ==================== 6. 加锁写回全局数据总线 ==================== */
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);

        monitor_msg.voltage           = med_v;
        monitor_msg.temperature       = med_t;
        monitor_msg.dt_tem            = dt_final;
        monitor_msg.dv_vol            = dv_final;
        monitor_msg.drop_ratio        = drop_ratio;
        monitor_msg.temp_risk_contrib = final_temp_risk;
        monitor_msg.drop_risk_contrib = final_drop_risk;
        monitor_msg.hard_fault        = hard_fault;
        monitor_msg.sensor_fault      = 0;
        monitor_msg.timestamp       = rt_tick_get();
        rt_mutex_release(sensor_mutex);

        if (!first_write_done)
        {
            rt_sem_release(predict_ready_sem);
            first_write_done = 1;
        }
    }
}

int app_alarm_init(void)
{
    Logic_thread3 = rt_thread_create("Logic_thread3", Logic_thread3_entry, RT_NULL,
                 1536, 9, 20);

    if (Logic_thread3 != RT_NULL)
    {
        rt_thread_startup(Logic_thread3);
    }
    else
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_APP_EXPORT(app_alarm_init);
