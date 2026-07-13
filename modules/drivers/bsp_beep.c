/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-27     12811       the first version
 */
#include "bsp_beep.h"

 //PB11:蜂鸣器模块【控制范围：3k -  4k】，占空比：【50%】，初始为0

///* ===== BEEP ===== */


//蜂鸣器功能拆解：通过  【信号集的事件】  来决定  【鸣叫频率】
//鸣叫频率 分为三档：   1.  【0】    2.   【3000】     3.   【4000】
//鸣叫频率 依靠 【 TIM2】 的  【ARR控制】  ，    【CCR】 控制音调高低    ->     因此我需要【知道】且【控制】       【ARR    CCR的值】

//目前所需函数： 1.计算ARR,CCR的函数
//           2.写入ARR,CCR的函数
//           3.控制挡位的三个函数，  停止函数    |   慢函数     |     快函数



//计算ARR   CCR
//1.目标fre   2.占空比   3.ARR值     4.CCR值
static void calc_pwm_param(uint32_t fre_h , uint8_t duty_prevent , uint32_t *arr , uint32_t *ccr)
{
   //    fcount（计数频率）  =  clk  /   PSC + 1
   //    Fpwm（目标频率）       =  fcount（计数频率） / ARR + 1

                if (fre_h == 0)
                {
                     *arr = 0;
                     *ccr = 0;
                     return;
                }


     uint32_t timer_clk  =  1000000;
                   *arr  =  (timer_clk / fre_h) - 1 ;
                   *ccr  =  ((*arr + 1) *duty_prevent) / 100 ;
}


//写入函数（写入fre与占空比）
void pwm_set(uint32_t fre , uint32_t duty)
{
     uint32_t arr , ccr;

     calc_pwm_param(fre, duty, &arr, &ccr);

     //写入前先停止输出
     HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_4);
     TIM2 ->ARR  = arr;
     TIM2 ->CCR4 = ccr;

     //重新打开
     if (fre != 0) {
         HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);
    }

}


//控制档位
void beep_stop(void)
{
    HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_4);
}

void beep_slow(void)
{
    pwm_set(3000, 50);
}


void beep_fast(void)
{
   pwm_set(4000, 50);
}



