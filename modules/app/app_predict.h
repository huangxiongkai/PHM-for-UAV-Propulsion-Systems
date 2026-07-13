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
    float drop_risk_threshold;   /* 0.08   压降风险起始阈值 8% */
    float risk_cap;              /* 50.0   单项风险贡献上限 */
    uint16_t stuck_threshold;    /* 1000   ADC卡死判定次数 */
    float temp_fault_lo;         /* -40    NTC故障下限 ℃ */
    float temp_fault_hi;         /* 150    NTC故障上限 ℃ */
    float volt_fault_lo;        /* 14.0      电压故障下限 V */  
    float volt_fault_hi;        /* 27.0     电压故障上限 V */   
    //当前架构下，通过物理量检查电压传感器故障实际上是无法触发的。 但这不意味着不该加——这是防御性编程，未来如果你改了映射函数或传了原始码，这个检查就生效了。
    //内容补充：
    #if 0
          根据 `docs/predict_coding_guide.md` 中的关键参数速查表和电池规格，以及 `Pot_To_SimBatteryVol` 的映射逻辑，来分析这两个值的合理设定。

## 分析依据

**电池规格**（指南第 4 节）：
- 6S LiPo 满电：25.2V（4.2V × 6）
- 3S 保护电压：约 15.0V（最低安全线）
- DANGER 电压：16.0V

**你的转换函数限制**（`mid_filter.c` `Pot_To_SimBatteryVol`）：
```c
if (adc_raw >= 4095) return 26.0f;   // 硬上限
if (adc_raw <= 0)    return 15.0f;   // 硬下限
return 15.0f + (adc_raw / 4095.0f) * 11.0f;
```

## ⚠️ 关键问题

由于 `Pot_To_SimBatteryVol` 已经做了 **硬 clamp**，转换后的 `voltage` 永远在 **[15.0, 26.0]** 范围内。这意味着：

| 实际故障 | ADC 读值 | 转换后电压 | 能否被检测到 |
|---------|---------|-----------|-------------|
| 分压电阻短路 | 0 | 15.0V | ❌ 在正常范围内 |
| 分压电阻开路 | 4095 | 26.0V | ❌ 在正常范围内 |

**结论：当前架构下，通过物理量检查电压传感器故障实际上是无法触发的。** 但这不意味着不该加——这是防御性编程，未来如果你改了映射函数或传了原始码，这个检查就生效了。

## 推荐值

基于工程实际，推荐紧贴转换函数的 clamp 边界并留小余量：

```c
float volt_fault_lo;    /* 14.0   电压故障下限 V (低于 Pot_To_SimBatteryVol 的 15V 下限) */
float volt_fault_hi;    /* 27.0   电压故障上限 V (高于 Pot_To_SimBatteryVol 的 26V 上限) */
```

初始化：

```c
.volt_fault_lo = 14.0f,
.volt_fault_hi = 27.0f,
```

**为什么不建议用你注释里的 0 和 24：**
- `0`：转换函数最低输出 15V，设 0 毫无意义，永远达不到
- `24`：6S 满电 25.2V，一上电就会误报电压故障 → 直接炸机

## 补充建议

如果你**真的想检测电压传感器开路/短路**，有两个方案：

**方案 A**（推荐）：修改 `Pot_To_SimBatteryVol`，去掉硬 clamp，让它如实反映 ADC 极端值：
```c
// 去掉 if 保护，直接映射
// adc=0 → 15V 是"合理"的，但如果线断了电压应该是 0V 而不是 15V
```

**方案 B**：用原始 ADC 码值判断（需要传 `raw_adc_v` 到 MQ），在预测层检查 `adc_raw == 0` 或 `adc_raw >= 4090`。

这两个方案都要改 `app_acquire.c`，不属于当前步骤 4 的范围。**建议先用 14.0/27.0 作为防御性占位，后续再优化。**
    #endif
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
    .drop_risk_threshold  = 0.08f,
    .risk_cap             = 50.0f,
    .stuck_threshold      = 1000,
    .temp_fault_lo        = -40.0f,
    .temp_fault_hi        = 150.0f,
    .volt_fault_lo        = 14.0f,
    .volt_fault_hi        = 27.0f
};



#endif /* MODULES_APP_APP_PREDICT_H_ */
