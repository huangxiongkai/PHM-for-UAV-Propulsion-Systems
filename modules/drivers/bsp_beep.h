/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-27     12811       the first version
 */
#ifndef MODULES_DRIVERS_BSP_BEEP_H_
#define MODULES_DRIVERS_BSP_BEEP_H_
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include "main.h"


void beep_stop(void);
void beep_slow(void);
void beep_fast(void);
void pwm_set(uint32_t fre, uint32_t duty);
 extern TIM_HandleTypeDef htim2;

#endif /* MODULES_DRIVERS_BSP_BEEP_H_ */
