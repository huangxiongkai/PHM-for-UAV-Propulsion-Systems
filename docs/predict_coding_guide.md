# 瞬态故障预测模块 — 编码指南

## 1. 项目背景概述

本模块运行于 **STM32F103 + RT-Thread Nano** 平台，通过 ADC 循环采样（约12.82kHz, 64点/批 ≈ 5ms/cycle）采集电池电压与 NTC 温度信号，执行**去极值滤波 → 物理量转换 → 瞬态趋势预测（IIR/微分/双时间尺度）→ 多级滞回判决（Supervisor）→ 蜂鸣器告警（Actuator）** 的全链路。

### 线程拓扑

```
┌──────────────────┐      MQ        ┌──────────────────┐
│ app_acquire.c     │──────┼───────→│ app_predict.c     │
│ (优先级 8, 1024B) │     ───      │ (优先级 9, 1536B) │
│ DMA信号量触发     │               │ 特征提取链        │
└──────────────────┘               └────────┬─────────┘
                                             │ 共享monitor_msg(互斥量保护)
                                             ▼
                              ┌──────────────────────────────┐
                              │ app_supervisor.c             │
                              │ (优先级 10, 1024B, 20ms)    │
                              │ 9层管线 → 事件发布            │
                              └────────────┬─────────────────┘
                                           │ 事件集
                                           ▼
                                  ┌──────────────────┐
                                  │ app_actuator.c    │
                                  │ (优先级 11, 512B) │
                                  │ LED/蜂鸣器告警     │
                                  └──────────────────┘

┌──────────────────┐
│ app_display.c     │ 互斥量读取 monitor_msg
│ (优先级 15, 512B) │
│ 每100ms打印       │
└──────────────────┘
```

### 数据流

```
ADC DMA(双缓冲) → acquire(去极值均值 → 物理量转换) → MQ → predict(冷启动→自检→Median3→特征提取→写回+timestamp)
                                                                                       ↓
                                                                               monitor_msg(全局,互斥量保护)
                                                                               ↙        ↘
                                                                       display(100ms轮询)  supervisor(9层管线)
                                                                                              ↓
                                                                                         actuator(事件驱动)
```

---

## 2. 核心数据结构

### `monitor_msg_t` (mid_databus.h)

完整字段（11个，天然32-bit对齐）：

```c
typedef struct {
    float temperature;       /* 滤波后温度 (℃) */
    float voltage;           /* 滤波后电压 (V) */
    float dt_tem;            /* 温度微分 (℃/s) */
    float dv_vol;            /* 电压微分 (V/s) */
    float drop_ratio;        /* 归一化压降比 [0,1] */
    float temp_risk_contrib; /* 温度风险贡献 [0,50] */
    float drop_risk_contrib; /* 压降风险贡献 [0,50] */
    uint32_t timestamp;      /* Predict写回时刻 (rt_tick_get) */
    uint8_t alarm_level;     /* ALARM_SAFE / WARNING / DANGER / HARDFAULT */
    uint8_t hard_fault;      /* 位图: bit0=过温, bit1=欠压 */
    uint8_t sensor_fault;    /* 0=正常, 1=NTC故障, 2=ADC卡死 */
    uint8_t reserved;        /* 保留对齐 */
} monitor_msg_t;
```

### `predict_param_t` (app_predict.h)

所有可调参数集中管理，`static const` 全局只读，编译时优化：

```c
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
    uint16_t stuck_threshold;    /* 10000  ADC卡死判定次数(≈50s@200Hz) */
    float temp_fault_lo;         /* -40    NTC故障下限 ℃ */
    float temp_fault_hi;         /* 150    NTC故障上限 ℃ */
    float volt_fault_lo;         /* 14.0   电压故障下限 V */
    float volt_fault_hi;         /* 27.0   电压故障上限 V */
    float temp_abs_threshold;    /* 60.0   绝对温度风险起始阈值 ℃ */
    float temp_abs_slope;        /* 0.5    绝对温度风险斜率 (risk/℃) */
} predict_param_t;
```

---

## 3. 文件清单与状态

| 文件 | 状态 | 内容 |
|------|------|------|
| `modules/middle/mid_databus.h` | ✅ 已更新 | `monitor_msg_t`(11字段,含timestamp)、事件位定义(含EVT_HARDFAULT)、IPC extern声明 |
| `modules/middle/mid_databus.c` | ✅ 已更新 | IPC对象创建：MQ池、事件集、信号量 |
| `modules/middle/mid_filter.c/.h` | ✅ 已冻结 | `fast_filing`(64点去极值)、`Pot_To_SimBatteryVol`(电位器→电压)、`calculate_temp`(查表→温度，已反转ADC方向)、`median3`、`iir_lpf` |
| `modules/drivers/bsp_adc.c/.h` | ✅ 已冻结 | DMA双缓冲(约12.82kHz)、影子缓冲、半满中断信号量 |
| `modules/drivers/bsp_throttle.c/.h` | ✅ 已实现(占位) | `throttle_read()` 返回1000(空载) |
| `modules/drivers/bsp_beep.c/.h` | ✅ 已就绪 | PWM蜂鸣器 fast/slow/stop 接口 |
| `modules/app/app_acquire.c` | ✅ 完成 | 信号量等待→数据提取→去极值滤波→物理量转换→MQ发送(5ms周期) |
| `modules/app/app_predict.c` | ✅ **完成** | **7阶段管线：冷启动→自检→Median3→温度链→电压链→油门迟滞→写回** |
| `modules/app/app_predict.h` | ✅ 完成 | `predict_param_t` 结构体 + `P` 常量实例 |
| `modules/app/app_display.c` | ✅ 完成 | 互斥量保护读取 + 手动整数/小数拆分打印(100ms) |
| `modules/app/app_supervisor.c` | 🔴 待实现 | 9层管线：快照→冷启动→HI计算→条件保持→表驱动FSM→故障锁存→告警融合→边沿事件→看门狗预留 |
| `modules/app/app_actuator.c` | 🔴 待实现 | 事件等待 + 蜂鸣器控制 |

---

## 4. Predict 管线详解 (app_predict.c)

### 4.1 辅助宏与工具函数

```c
#define PREDICT_DT  0.005f   /* 200Hz 采样周期 (s) */
#define K_TEMP      2.0f     /* 温度风险释放斜率 */
#define K_DROP      5.0f     /* 压降风险释放斜率 */

static inline float float_clamp(float val, float lo, float hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}
```

### 4.2 7阶段流水线

```
MQ接收(msg_local)
  │
  ├─ Stage 1: 冷启动保护
  │   首帧将所有滤波状态对齐到当前采样值，continue跳过
  │   防止第一帧微分产生 (25-0)/0.005 = 5000℃/s 的荒谬值
  │
  ├─ Stage 2: 传感器健康自检
  │   ├─ 温度范围检查 [-40, 150]℃
  │   ├─ 电压范围检查 [14, 27]V
  │   └─ ADC卡死检测：dv<0.0001V && dt<0.001℃ 持续1000帧
  │       若触发故障 → 加锁写回sensor_fault → continue
  │
  ├─ Stage 3: 滑动窗口 + Median3 前置滤波
  │   t_win/v_win 长度3的滑窗步进，取中值
  │   拦截Acquire层漏网的EMI尖峰
  │
  ├─ Stage 4: 温度特征提取链
  │   一次IIR平滑(t_smooth) → 离散微分(dt_raw) → SlewLimit(±10℃/s) → 二次IIR → 风险贡献
  │   hard_fault: t_smooth > 105℃ → bit0=1
  │
  ├─ Stage 5: 电压特征提取链(双时间尺度模型)
  │   v_fast快尺度IIR(α=0.15) → 微分(dv_raw) → SlewLimit(±20V/s) → 二次IIR
  │   v_ref慢尺度IIR(gamma由油门控制) → (v_ref - v_fast)/v_ref → drop_ratio
  │   hard_fault: v_fast < 16V → bit1=1
  │   油门冻结：thr>1700入冻，thr<1600解冻(双阈值迟滞)
  │             5秒内gamma=0彻底锁死，超过5秒gamma=0.0002缓释跟踪
  │
  └─ Stage 6: 加锁写回全局 monitor_msg
        rt_mutex_take(sensor_mutex)
        写入所有预测特征字段
        rt_mutex_release(sensor_mutex)
```

### 4.3 线程参数

| 参数 | 值 |
|------|-----|
| 线程名 | `Logic_thread3` |
| 优先级 | 9 (低于 acquire 的 8，高于 supervisor 的 10) |
| 栈大小 | 1536 字节 |
| 节拍 | 20ms |
| 创建方式 | `rt_thread_create`（动态） |
| 初始化 | `INIT_APP_EXPORT(app_alarm_init)` |

### 4.4 静态状态变量

| 变量 | 类型 | 用途 |
|------|------|------|
| `first_frame` | uint8_t | 冷启动标志 |
| `stuck_cnt` | uint16_t | ADC卡死计数器 |
| `t_win[3]` | float | 温度滑动窗口 |
| `v_win[3]` | float | 电压滑动窗口 |
| `t_smooth` | float | 温度IIR平滑值(兼作状态) |
| `t_smooth_prev` | float | 上一帧平滑值(微分用) |
| `dt_iir_state` | float | 温度微分二次IIR状态 |
| `v_fast` | float | 电压快尺度IIR状态 |
| `v_fast_prev` | float | 上一帧快尺度值(微分用) |
| `dv_iir_state` | float | 电压微分二次IIR状态 |
| `v_ref` | float | 电压慢尺度(参考)IIR状态 |
| `freeze_cnt` | uint16_t | 油门冻结计时(帧数) |
| `is_frozen` | uint8_t | 油门冻结迟滞标志(函数内static) |

### 4.5 辅助函数调用链

```
mid_filter.h  ──┬── median3(float a, float b, float c)
                │       3次比较交换，返回中值
                │
                └── iir_lpf(float input, float *state, float alpha)
                         *state += alpha * (input - *state)
                         通过指针更新状态 + 返回值

bsp_throttle.h ──┬── throttle_read(void)
                  │       返回 uint16_t(thr)，当前占位 1000
                  │
app_predict.c  ──┬── float_clamp(val, lo, hi)
                   │       static inline，零开销
                   │
                   └── predict_param_t P(全局const)
                           所有可调参数集中管理
```

---

## 5. 关键参数表

| 参数 | 值 | 含义 |
|------|-----|------|
| `SAMPLE_COUNT` | 64 | 每批ADC采样点数 |
| ADC 采样率 | 约 12.82 kHz | TIM3 触发（PSC=71, ARR=77） |
| 批次周期 | ~5.00ms (≈200Hz) | 64/12820 |
| `PREDICT_DT` | 0.005f | 微分周期(s) |
| `temp_iir_alpha` | 0.08 | 温度一次IIR平滑系数 |
| `temp_diff_alpha` | 0.06 | 温度微分二次IIR系数 |
| `v_fast_alpha` | 0.15 | 电压快尺度IIR系数 |
| `v_diff_alpha` | 0.06 | 电压微分二次IIR系数 |
| `v_ref_freeze_alpha` | 0.0 | 油门冻结时v_ref锁死 |
| `v_ref_release_alpha` | 0.003 | 空闲时v_ref跟踪 |
| `v_ref_longhold_alpha` | 0.0002 | 长推5秒后缓释 |
| `temp_slew_limit` | 10.0 ℃/s | 温度微分限幅 |
| `volt_slew_limit` | 20.0 V/s | 电压微分限幅 |
| `temp_risk_slope` | 1.2 ℃/s | 温度风险起始（变化率） |
| `temp_abs_threshold` | 60.0 ℃ | 绝对温度风险起始阈值 |
| `temp_abs_slope` | 0.5 risk/℃ | 绝对温度风险斜率 |
| `drop_risk_threshold` | 0.08 (8%) | 压降风险起始 |
| `K_TEMP` | 2.0 | 温度风险释放斜率 |
| `K_DROP` | 5.0 | 压降风险释放斜率 |
| `risk_cap` | 50.0 | 单项风险上限 |
| `stuck_threshold` | 10000 | ADC卡死判定帧数(≈50s@200Hz) |
| `temp_fault_lo/hi` | -40℃ / 150℃ | NTC故障阈值 |
| `volt_fault_lo/hi` | 14.0V / 27.0V | 电压故障阈值 |
| 油门冻结阈值(入) | 1700 | thr > 1700 入冻结 |
| 油门冻结阈值(出) | 1600 | thr < 1600 解冻结 |
| 冻结超时 | 5s (1000帧) | 持续大推力5秒后缓释 |
| 硬故障位0 | t_smooth > 105℃ | 过温故障 |
| 硬故障位1 | v_fast < 16V | 欠压故障 |
| 预测线程栈 | 1536 B | app_predict.c |

---

## 6. 离线打印方案 (RT-Thread Nano)

RT-Thread Nano 的 `rt_kprintf` 不支持 `%f` 浮点格式化。所有浮点打印使用**手动整数/小数拆分**：

```c
int v_i = (int)val;
int v_d = (int)((val - v_i) * 10);
if (v_d < 0) v_d = -v_d;
rt_kprintf("[TAG] %d.%d\r\n", v_i, v_d);
```

如需2位小数，将 `* 10` 改为 `* 100`。

---

## 7. IPC 对象一览

| 对象 | 类型 | 创建位置 | 用途 |
|------|------|----------|------|
| `monitor_mq` | `rt_messagequeue` | `mid_databus.c` | acquire → predict (2048B池, FIFO, 单消费者) |
| `adc_event` | `rt_event` | `mid_databus.c` | supervisor → actuator 告警事件 (EVT_SAFE/WARNING/DANGER/HARDFAULT) |
| `alarm_sem` | `rt_semaphore` | `mid_databus.c` | DMA ISR → acquire 通知(初值0, FIFO) |
| `sensor_mutex` | `rt_mutex_t` | `app_acquire.c` | 保护`monitor_msg`全局实例读写(PRIO) |

---

## 8. 实现状态与下一步

### ✅ 已完成 (8/10 模块)

| 模块 | 验证方法 |
|------|---------|
| IPC基础设施 | 编译通过，串口无异常 |
| ADC采集 + 滤波 | DMA正常运行，MQ有数据 |
| acquire → MQ | 串口可见 `[PREDICT] Cold-start` |
| predict 全部管线 | 串口可见 `Volt: 24.3V, Temp: 25.1C` |
| display 打印 | 正常输出所有特征字段 |
| throttle占位 | 编译通过 |

### 🔴 待实现

**Step 8 — Supervisor 9层管线** (`app_supervisor.c`)
```
输入: monitor_msg (mutex快照, 含timestamp)
逻辑 (V2.1 Final, 9层管线):
  L1: 快照 (rt_mutex_take → memcpy → rt_mutex_release)
  L2: 冷启动首帧定态 (evaluate_initial_state → 同步alarm_level+last_alarm → 不发事件)
  L3: HI计算 (hi_x10 = (100 - temp_risk - drop_risk) × 10, 定点化)
  L4: 条件保持 (hold_target + hold_cnt 统一计数器, 恶化快恢复慢)
  L5: 表驱动Health FSM (SAFE/WARNING/DANGER 三态)
  L6: 故障锁存 (sensor_fault || hard_fault || predict_timeout → latched, 无防抖)
  L7: 告警融合 (latched → ALARM_HARDFAULT, 否则 = health_state)
  L8: 边沿事件发布 (alarm != last_alarm → rt_event_send)
  L9: 看门狗预留(当前不实现)
输出:
  monitor_msg.alarm_level = ALARM_SAFE/WARNING/DANGER/HARDFAULT
  rt_event_send(&adc_event, EVT_SAFE/WARNING/DANGER/HARDFAULT)
```

**Step 9 — Actuator 执行器** (`app_actuator.c`)
```
输入: adc_event 事件等待 (rt_event_recv, OR+CLR, FOREVER)
逻辑:
  EVT_SAFE      → 蜂鸣器停止 (beep_stop)
  EVT_WARNING   → 蜂鸣器慢速 (beep_slow)
  EVT_DANGER    → 蜂鸣器快速 (beep_fast)
  EVT_HARDFAULT → 蜂鸣器快速 (beep_fast)
```

---

## 9. 常见陷阱

1. **MQ 单消费模型**：一条消息只能被一个线程接收。当前 `monitor_mq` 由 predict 独享，supervisor 通过共享 `monitor_msg` 获取数据。
2. **中断内禁止阻塞**：DMA ISR 中只允许 `rt_sem_release()`/`rt_mq_send()` 的 ISR 变体，严禁 `rt_thread_mdelay`、`rt_mutex_take`、`rt_malloc`。
3. **浮点运算与栈**：Cortex-M3 无 FPU，浮点运算由 libgcc 软件模拟(每次调用约40B栈帧)，predict 线程栈 1536B 已确认足够。
4. **`iir_lpf` 原地更新状态**：`iir_lpf(x, &state, alpha)` 通过指针修改 `state` **并** 返回新值。调用 `x = iir_lpf(x, &x, a)` 是冗余但正确的——`x` 被赋值回自身。
5. **`Pot_To_SimBatteryVol` 硬clamp**：转换函数将 ADC 输出 clamp 到 [15V, 26V]，导致无法通过物理量检测电压传感器故障。故障检测的阈值保留作为防御性编程。
6. **NTC ADC 方向反转**：`calculate_temp` 入口已加入 `temp_value = 4095 - temp_value` 反转映射方向，适应实际电路接线。
