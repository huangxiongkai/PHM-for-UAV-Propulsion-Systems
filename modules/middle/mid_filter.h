/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-30     12811       the first version
 */
#ifndef MODULES_MIDDLE_MID_FILTER_H_
#define MODULES_MIDDLE_MID_FILTER_H_

#include <stdint.h>
#include <rtdef.h>
#include <drivers/bsp_adc.h>

/* 原始数据去极值滤波 */
uint16_t fast_filing(uint16_t *data_start, uint16_t len);

/* 电位器ADC → 模拟电池电压 (线性映射) */
float Pot_To_SimBatteryVol(uint16_t adc_raw);

/* NTC热敏电阻ADC → 温度℃ (查表+线性插值, MF52A 103F3950) */
float calculate_temp(uint16_t temp_value);

/* 三点真中值滤波 */
float median3(float a, float b, float c);

/* 一阶IIR低通滤波 */
float iir_lpf(float input, float *state, float alpha);

#endif /* MODULES_MIDDLE_MID_FILTER_H_ */
