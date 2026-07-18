/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-06-04     12811       the first version
 */
#ifndef MODULES_APP_APP_PREDICT_H_
#define MODULES_APP_APP_PREDICT_H_
#include <rtthread.h>
#include <middle/mid_databus.h>
#include <middle/mid_filter.h>
#include <drivers/bsp_throttle.h>

/* ---------- 参数管理 ---------- */
typedef struct {
    float temp_iir_alpha;        /* 0.08   温度一次IIR平滑系数 */
    float temp_diff_alpha;       /* 0.06   温度微分二次IIR系数 */
    float v_fast_alpha;          /* 0.15   电压快尺度IIR系数 */
    float v_diff_alpha;          /* 0.06   电压微分二次IIR系数 */
    float v_ref_freeze_alpha;    /* 0.0    油门冻结时v_ref彻底锁死 */
    float v_ref_release_alpha;   /* 0.003  空闲时v_ref慢速跟踪 */
    float v_ref_longhold_alpha;  /* 0.0002 长推5秒后缓释跟踪 */
    float temp_slew_limit;       /* 10.0   温度微分限幅 ±℃/s */
    float volt_slew_limit;       /* 20.0   电压微分限幅 ±V/s */
    float temp_risk_slope;       /* 1.2    温度风险起始阈值 ℃/s */
    float temp_abs_threshold;    /* 60.0   绝对温度风险起始阈值 ℃（慢速升温场景） */
    float temp_abs_slope;        /* 0.5    绝对温度风险斜率 (risk/℃) */
    float temp_overtemp_prot;    /* 105.0  过温保护硬触发阈值 ℃（hard_fault bit0） */
    float drop_risk_threshold;   /* 0.08   压降风险起始阈值 8% */
    float risk_cap;              /* 50.0   单项风险贡献上限 */
    uint16_t stuck_threshold;    /* 10000  ADC卡死判定次数（原1000在悬停场景误触发，改为10000≈50s@200Hz） */
    float temp_fault_lo;         /* -40    NTC传感器诊断下限 ℃（Stage 2 自检用，检测传感器开路/ADC故障） */
    float temp_fault_hi;         /* 150    NTC传感器诊断上限 ℃（Stage 2 自检用，非过温保护；过温保护见 temp_overtemp_prot） */
    float volt_fault_lo;        /* 14.0      电压故障下限 V */  
    float volt_fault_hi;        /* 27.0     电压故障上限 V */   
} predict_param_t;

static const predict_param_t P = {
    .temp_iir_alpha       = 0.08f,
    .temp_diff_alpha      = 0.06f,
    .v_fast_alpha         = 0.15f,
    .v_diff_alpha         = 0.06f,
    .v_ref_freeze_alpha   = 0.0f,
    .v_ref_release_alpha  = 0.003f,
    .v_ref_longhold_alpha = 0.0002f,
    .temp_slew_limit      = 10.0f,
    .volt_slew_limit      = 20.0f,
    .temp_risk_slope      = 1.2f,
    .temp_abs_threshold   = 60.0f,
    .temp_abs_slope       = 0.5f,
    .temp_overtemp_prot   = 105.0f,
    .drop_risk_threshold  = 0.08f,
    .risk_cap             = 50.0f,
    .stuck_threshold      = 10000,
    .temp_fault_lo        = -40.0f,
    .temp_fault_hi        = 150.0f,
    .volt_fault_lo        = 14.0f,
    .volt_fault_hi        = 27.0f
};



#endif /* MODULES_APP_APP_PREDICT_H_ */
