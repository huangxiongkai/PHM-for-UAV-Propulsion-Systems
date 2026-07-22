#include "bsp_adc.h"
#include <string.h>

//PA6:电位器模块（ADC1_IN6）
//PA4:热敏模块（ADC2_IN4）

/**
 * @brief DMA物理缓冲区（DMA硬件写入，CPU不得读取）
 *        DMA Circular 模式下硬件自动循环写入，永不停止
 */
__attribute__((aligned(4)))
uint16_t adc_raw_buf[FULL_BUF_SZ];

/**
 * @brief DMA影子缓冲区（ISR中 memcpy 冻结，CPU安全读取）
 *        4字节对齐以满足F103 AHB总线DMA要求
 */
__attribute__((aligned(4)))
uint16_t adc_shadow_buf[FULL_BUF_SZ];

volatile uint8_t shadow_ready_half = 0;  // 0=半区0已就绪, 1=半区1已就绪

void bsp_adc_init(ADC_HandleTypeDef *hadc)
{
    HAL_ADCEx_Calibration_Start(hadc);
    HAL_TIM_Base_Start(&htim3);
    HAL_ADC_Start_DMA(hadc, (uint32_t *)adc_raw_buf, FULL_BUF_SZ);
}

/**
 * @brief 返回影子缓冲区指针
 * @return uint16_t* 指向 adc_shadow_buf，DMA全传输完成后由ISR更新
 */
uint16_t *adc_get_shadow_buf(void)
{
    return adc_shadow_buf;
}

/**
 * @brief DMA半传输完成回调（前半段 128 元素就绪）
 *        仅做 memcpy + 置标志 + 释放信号量，无阻塞操作
 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        /* 冻结前半段 [0..HALF_BUF_SZ-1] 到影子缓冲区前半 */
        memcpy(adc_shadow_buf, adc_raw_buf, HALF_BUF_SZ * sizeof(uint16_t));
        shadow_ready_half = 0;  // 前半段就绪
      
        /* 通知 acquire 线程处理 */
        rt_sem_release(alarm_sem);
    }
}

/**
 * @brief DMA全传输完成回调（后半段 128 元素就绪）
 *        仅做 memcpy + 置标志 + 释放信号量，无阻塞操作
 *        Circular DMA 永不停止，由硬件自动翻转
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        /* 冻结后半段 [HALF_BUF_SZ..FULL_BUF_SZ-1] 到影子缓冲区后半 */
        memcpy(adc_shadow_buf + HALF_BUF_SZ, adc_raw_buf + HALF_BUF_SZ,
               HALF_BUF_SZ * sizeof(uint16_t));
        shadow_ready_half = 1;  // 后半段就绪

        /* 通知 acquire 线程处理 */
        rt_sem_release(alarm_sem);
    }
}


