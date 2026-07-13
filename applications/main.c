/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-27     RT-Thread    first version
 */

#include <rtthread.h>
#include "stm32f1xx_hal.h"
#include "gpio.h"
#include "adc.h"
#include "usart.h"
#include "dma.h"
#include "tim.h"

int main(void)
{
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_TIM3_Init();
    MX_TIM2_Init();
    MX_ADC1_Init();




    
    return RT_EOK;
}
