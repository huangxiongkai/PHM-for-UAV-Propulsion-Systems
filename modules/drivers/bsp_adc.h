/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-27     12811       the first version
 */
#ifndef MODULES_DRIVERS_BSP_ADC_H_
#define MODULES_DRIVERS_BSP_ADC_H_
#include "stm32f1xx_hal.h"
#include <rtthread.h>
#include <stdio.h>
#include <middle/mid_databus.h>

#define ADC_CHANNELS    2
#define SAMPLE_COUNT    64  // 采样值  12.658kHZ  *  5.057ms = 64
#define HALF_BUF_SZ     (ADC_CHANNELS * SAMPLE_COUNT)  // 128 — 半区元素数
#define FULL_BUF_SZ     (HALF_BUF_SZ * 2)              // 256 — 全缓冲元素数

/* CubeMX生成的ADC句柄声明 */
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

extern TIM_HandleTypeDef htim3;

//用于通知DMA传输完成
extern rt_sem_t alarm_sem;

extern uint16_t adc_raw_buf[FULL_BUF_SZ];
extern uint16_t adc_shadow_buf[FULL_BUF_SZ];
extern volatile uint8_t shadow_ready_half;  // 0=半区0就绪, 1=半区1就绪

void bsp_adc_init(ADC_HandleTypeDef *hadc);
uint16_t *adc_get_shadow_buf(void);

#endif /* MODULES_DRIVERS_BSP_ADC_H_ */
