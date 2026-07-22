/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-30     12811       the first version
 */
//文件内容：算法
#include "mid_filter.h"

// 查表法 — MF52A 103F3950 NTC (10kΩ上拉, B=3950)  ADC→温度映射
#define T_ADC_STEP  256
#define T_LUT_LEN   17
static const uint16_t t_adc_lut[T_LUT_LEN] = { 0, 256, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304, 2560, 2816, 3072, 3328, 3584, 3840, 4095 };
static const float   t_val_lut[T_LUT_LEN] = { -40.0f, -25.5f, -13.2f, -4.8f, 2.2f, 8.2f, 13.9f, 19.5f, 25.0f, 30.8f, 37.0f, 43.9f, 52.0f, 62.1f, 76.4f, 101.8f, 250.0f };


/* 1.原始数据去极值滤波 */
//情况解析：无人机电调开关噪声偶尔会产生一个远高于正常值的尖峰或低谷（毛刺），
uint16_t fast_filing(uint16_t *data_start, uint16_t len)
{
  uint32_t sum = 0;
  uint16_t max_val = 0;
  uint16_t min_val = 0xFFFF;

  for (int i = 0; i < len; i++)
  {
    uint16_t val = data_start[i];  // 使用实际数据而非通道索引
    sum += val;

    if (val > max_val)
    {
       max_val = val;
    }
    if (val < min_val)
    {
        min_val = val;
    }
  }
  return (uint16_t)((sum - max_val - min_val) / (len - 2));
  
}



/*2. 电位器ADC → 模拟电池电压 */
float Pot_To_SimBatteryVol(uint16_t adc_raw)
{
    if (adc_raw >= 4095) return 26.0f;  // 6S 满电
    if (adc_raw <= 0)    return 15.0f;  // 3S 保护
    return 15.0f + (adc_raw / 4095.0f) * (26.0f - 15.0f);
}

/*3. NTC热敏电阻ADC → 温度℃ — 二分查找 + 线性插值 */
float calculate_temp(uint16_t temp_value)
{
    temp_value = 4095 - temp_value;  // 反转ADC映射方向（NTC发热→电阻降低→ADC下降）
    
    if (temp_value <= 10)   return -40.0f;
    if (temp_value >= 4085) return 250.0f;

    int lo = 0, hi = T_LUT_LEN - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (temp_value < t_adc_lut[mid])
            hi = mid;
        else
            lo = mid;
    }

    uint16_t span = t_adc_lut[hi] - t_adc_lut[lo];
    uint16_t frac = temp_value - t_adc_lut[lo];
    float Temp = t_val_lut[lo] + (t_val_lut[hi] - t_val_lut[lo]) * (float)frac / (float)span;

    if (Temp < -40.0f) Temp = -40.0f;
    if (Temp > 250.0f) Temp = 250.0f;
    return Temp;
}


/**
 * 4.三点真中值滤波器 — 用于去除偶尔出现的毛刺（单个异常值），适合小样本数据
 * @brief 3点中值滤波器
 * @note  仅需3次比较操作，约15个时钟周期（72MHz下约0.2μs）
 * @param a 第一个值
 * @param b 第二个值
 * @param c 第三个值
 * @return 3个值的中值
 */
float median3(float a, float b, float c)
{
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}

/* *
 5.一阶 IIR 低通滤波器 - 用于 predict 线程对温度和电压信号做二次平滑
 * @brief 一阶 IIR 低通滤波器
 * @param input  当前采样值
 * @param state  滤波器状态（调用者维护的静态变量）
 * @param alpha  滤波系数 (0~1)，越小越平滑
 * @return 滤波后值 
*/
float iir_lpf(float input, float *state, float alpha)
{
    *state = *state + alpha * (input - *state);
    return *state;
}

