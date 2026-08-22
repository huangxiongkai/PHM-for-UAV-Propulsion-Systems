# 无人机智能动力系统健康管理（PHM）与主动防护系统

## 技术栈

**硬件平台**：STM32F103C8 (Cortex-M3, 72MHz) | **实时操作系统**：RT-Thread Nano | **开发工具**：STM32CubeMX + RT-Thread Studio

**外设驱动**：ADC1/2 + DMA双缓冲 + TIM2/3 + PWM + GPIO | **IPC机制**：消息队列 / 信号量 / 互斥量 / 事件集

**核心算法**：IIR低通滤波 | Median3滑动窗口 | 双时间尺度电压模型 | 离散微分 + SlewLimit钳位 | 表驱动滞回FSM

**系统架构**：PHM分层架构（数据采集→特征提取→健康评估→决策执行）| 5线程优先级调度 | 故障锁存 + 根因追溯

---

## 项目概述

**Safety Copilot** — 面向**工业巡检、特种物流、高负载无人机作业**等严苛场景的嵌入式电池健康监测系统。通过多传感器融合与多级滞回状态机，实现电池热失控与欠压故障的**提前预警**（实测3.7s）与**分级告警防护**。系统运行在 STM32F103C8 上，采用 RT-Thread Nano 实时操作系统，与主飞控物理/逻辑隔离，符合航空电子"故障安全"规范。

### 核心创新点

- **独立模块设计**：与主飞控物理/逻辑隔离，符合航空电子"故障安全"规范
- **分层 PHM 架构**：对齐 PHM Society 标准分层结构（数据采集→特征提取→健康评估→决策执行）
- **主动防护预留**：多模态声光告警分级执行，预留油门串联拦截接口（SBUS/PWM），支持扩展为保姿态动态降级
- **双重底层防御**：猝发过采样 + 原始码熔断，信噪比提升 12dB，基线测试零误报
- **双时间尺度模型**：快速尺度跟踪瞬态压降，慢速基准跟踪 SOC 稳态，油门联动冰冻机制
- **表驱动 FSM**：滞回状态机 + 条件保持，4% 滞回裕量防止状态抖动

### 核心能力

- **高频信号采集**：12.658kHz ADC 采样，DMA 双缓冲无阻塞传输，5ms 调度周期
- **多尺度特征提取**：温度/电压双通道 IIR 滤波 + 微分 + SlewLimit 钳位 + 风险量化
- **智能健康评估**：综合健康指数 (HI) 计算，表驱动有限状态机 (FSM)，支持滞回阈值与条件保持
- **故障根因追溯**：6 类故障原因枚举（PREDICT_INIT/LOST、SENSOR、OVERTEMP、UNDERVOLT），锁存最先触发事件
- **多模态告警**：RGB LED 闪烁 + PWM 蜂鸣器多模式鸣响 + 油门动态限幅

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        硬件抽象层 (drivers/)                      │
├──────────────┬──────────────┬──────────────┬────────────────────┤
│  bsp_adc.c   │  bsp_beep.c  │ bsp_throttle │   STM32 HAL/CubeMX │
│  DMA双缓冲   │  PWM蜂鸣器   │  油门占位    │   ADC/TIM/GPIO     │
└──────┬───────┴──────┬───────┴──────┬───────┴────────────────────┘
       │              │              │
┌──────▼──────────────▼──────────────▼────────────────────────────┐
│                        中间件层 (middle/)                        │
├─────────────────────────┬───────────────────────────────────────┤
│     mid_databus.c       │           mid_filter.c                │
│  IPC对象 + 共享数据总线 │  去极值/IIR/Median3/物理量转换       │
└──────┬──────────────────┴───────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────┐
│                        应用层 (app/)                             │
├─────────────┬─────────────┬─────────────┬─────────────┬─────────┤
│ app_acquire │ app_predict │app_supervisor│ app_actuator│app_display│
│  数据采集   │  特征提取   │  健康评估    │  告警执行   │  串口日志 │
│  优先级 8   │  优先级 9   │  优先级 10   │  优先级 11  │  优先级 15│
└─────────────┴─────────────┴─────────────┴─────────────┴─────────┘
```

## 线程设计

系统包含 5 个应用线程，按优先级从高到低排列：

| 线程名 | 优先级 | 栈大小 | 周期 | 职责 |
|--------|--------|--------|------|------|
| `rawdata_thread1` | 8 | 1024B | 事件驱动 | DMA 数据提取 + 去极值滤波 + 物理量转换 |
| `Logic_thread3` | 9 | 1536B | 事件驱动 | 7 阶段特征提取链 + 传感器自检 |
| `supervisor` | 10 | 1024B | 20ms | 9 层管线：HI 计算 + FSM + 故障锁存 |
| `actuator` | 11 | 1536B | 事件驱动 | 4 层架构：策略查表 + 幂等分发 |
| `log_show` | 15 | 512B | 100ms | 串口打印监控数据 |

## IPC 通信机制

系统使用 RT-Thread 原生 IPC 对象实现线程间同步与数据传递：

| IPC 对象 | 类型 | 创建位置 | 用途 |
|----------|------|----------|------|
| `monitor_mq` | 消息队列 | mid_databus.c | Acquire → Predict 数据传递 |
| `alarm_sem` | 信号量 | mid_databus.c | DMA ISR → Acquire 传输完成通知 |
| `predict_ready_sem` | 信号量 | mid_databus.c | Predict 首次写入 → Supervisor 冷启动同步 |
| `adc_event` | 事件集 | mid_databus.c | Supervisor → Actuator 告警事件发布 |
| `sensor_mutex` | 互斥量 | app_acquire.c | 保护 `monitor_msg` 全局共享结构 |

## 核心算法

### 1. 数据采集层双重防御 (app_acquire.c)

Acquire 线程实现双重底层防御机制，确保原始数据质量：

| 防御层 | 具体实现 | 技术参数 |
|--------|----------|----------|
| **猝发过采样** | ADC DMA 连续采集 64 次取平均 | 5ms 调度周期，耗时约 336μs，信噪比提升 12dB |
| **原始码熔断** | 监控 ADC 原始寄存器码值 | 连续 1000 次 (5 秒) 无抖动判定死锁 |

**传感器故障检测**：
- NTC 开路判定：Raw ≥ 4090（逼近满量程）
- NTC 短路判定：Raw ≤ 5（逼近零值）
- 电压传感器开路/短路：Raw ≤ 10 或 Raw ≥ 4085

### 2. 特征提取链 (app_predict.c)

Predict 线程实现 7 阶段信号处理管线：

```
原始ADC → 冷启动保护 → 传感器自检 → Median3滑动窗口
        → 温度链(IIR+微分+SlewLimit+风险量化)
        → 电压链(双时间尺度IIR+油门冻结+压降比+风险量化)
        → 加锁写回全局总线
```

#### 双时间尺度电压模型

| 尺度 | 符号 | 作用 | 滤波系数 |
|------|------|------|----------|
| 快速尺度 | V_fast | 跟踪 5ms 负载瞬态 | α=0.15 |
| 慢速基准 | V_ref | 电池稳态电动势特征 | 空闲 α=0.003，冰冻 α=0.0002 |

**油门冰冻机制 (Throttle Freeze)**：
- 进入阈值：油门指令 > 1700（高负载爬升）
- 退出阈值：油门指令 < 1600（安全死区）
- 迟滞设计：100 单位死区防止信号抖动导致频繁清零
- 超时保护：持续大油门超过 5 秒，系数从 0.0 切换为 0.0002（微弱跟踪）

**压降比计算公式**：
```c
DropRatio = clamp((V_ref - V_fast) / V_ref, 0.0f, 1.0f)
```

#### 温度微分调理链

```
Median3去极值 → 一次IIR平滑 → 离散微分 → SlewLimit钳位(±10°C/s) → 二次IIR平滑 → 风险量化
```

**关键参数**：
- 温度风险阈值：1.2 ℃/s（超过开始累积风险）
- 压降风险阈值：8%（超过开始累积风险）
- 单项风险上限：50（temp_risk_contrib / drop_risk_contrib 各自上限）
- 温度释放斜率：K_TEMP = 2.0
- 压降释放斜率：K_DROP = 5.0

### 3. 健康评估状态机 (app_supervisor.c)

Supervisor 线程实现 9 层管线，核心为表驱动 FSM：

#### 综合健康指数 (HI) 计算

```c
HI = clamp(100 - (temp_risk_contrib + drop_risk_contrib), 0, 100) × 10
```

#### 滞回状态机规则

```c
static const transition_rule_t g_rules[] = {
    { HEALTH_SAFE,    HEALTH_WARNING, 800, 10, CMP_BELOW }, // HI<80, 200ms
    { HEALTH_WARNING, HEALTH_DANGER,  400, 5,  CMP_BELOW }, // HI<40, 100ms
    { HEALTH_WARNING, HEALTH_SAFE,    840, 50, CMP_ABOVE }, // HI>84, 1000ms (4%滞回裕量)
    { HEALTH_DANGER,  HEALTH_WARNING, 450, 25, CMP_ABOVE }, // HI>45, 500ms
};
```

#### 并发硬故障位图

```c
#define HARDFAULT_OVERTEMP     (1 << 0)   // 严重超温 (>105℃)
#define HARDFAULT_UNDERVOLT    (1 << 1)   // 严重欠压 (<16V)
```

**故障锁存机制**：
- 6 类故障原因：PREDICT_INIT / PREDICT_LOST / SENSOR / OVERTEMP / UNDERVOLT
- 仅记录最先触发的故障，防止后续故障覆盖根因
- 故障一旦锁存，需重启清除

### 3. 告警执行策略 (app_actuator.c)

Actuator 线程实现 4 层架构：

| 层级 | 职责 | 实现 |
|------|------|------|
| L1 Event+Context | 等待事件 + 读取快照 | `rt_event_recv` 500ms 超时 |
| L2 Policy Table | 查表得到执行计划 | 5 种策略：SAFE/WARNING/DANGER/HARDFAULT/SUPERVISOR_LOST |
| L3 Dispatcher | 幂等检查，避免重复下发 | `rt_memcmp` 比较新旧 plan |
| L4 Driver Adapter | 调用 LED/蜂鸣器驱动 | GPIO + PWM |

**告警模式**：

| 状态 | LED | 蜂鸣器 |
|------|-----|--------|
| SAFE | 绿常亮 | 静音 |
| WARNING | 黄慢闪 500ms | 3kHz 单次 100ms / 2s 间隔 |
| DANGER | 红快闪 100ms | 3kHz 双连鸣 (100-100-1000ms) |
| HARDFAULT | 红快闪 100ms | 4kHz 急促连续 (50-50 周期) |
| SUPERVISOR_LOST | 蓝常亮 | 4kHz 长鸣不间断 |

## 数据结构

### monitor_msg_t — 全局共享监控消息

```c
typedef struct {
    float temperature;       // 滤波后温度 (℃)
    float voltage;           // 滤波后电压 (V)
    float dt_tem;            // 温度微分 (℃/s)
    float dv_vol;            // 电压微分 (V/s)
    float drop_ratio;        // 归一化压降比 [0,1]
    float temp_risk_contrib; // 温度风险贡献 [0,50]
    float drop_risk_contrib; // 压降风险贡献 [0,50]
    uint8_t alarm_level;     // 告警等级: 0=SAFE, 1=WARNING, 2=DANGER, 3=HARDFAULT
    uint8_t hard_fault;      // 硬故障位图: bit0=过温, bit1=欠压
    uint8_t sensor_fault;    // 传感器故障: 0=正常, 1=NTC, 2=ADC卡死, 3=电压传感器
    rt_tick_t timestamp;     // Predict 线程心跳时间戳
    rt_tick_t supervisor_heartbeat; // Supervisor 线程心跳时间戳
    uint8_t fault_cause;     // 故障原因枚举
} monitor_msg_t;
```

## 硬件资源

| 外设 | 用途 | 配置 |
|------|------|------|
| ADC1 | 电池电压采集 | 12.658kHz，DMA 双缓冲 |
| ADC2 | NTC 温度采集 | 与 ADC1 同步采样 |
| TIM2 | PWM 蜂鸣器 | CH4，频率 3-4kHz |
| TIM3 | ADC 采样触发 | 12.658kHz 定时触发 |
| GPIO | RGB LED | 低电平点亮 |
| USART1 | 串口日志 | 115200bps |

## 构建与烧录

### 环境要求

- RT-Thread Studio 2.0+
- ARM GCC 工具链 (arm-none-eabi-gcc)
- J-Link 或 ST-Link 调试器

### 编译步骤

1. 打开 RT-Thread Studio，导入项目
2. 右键项目 → "构建项目" (或按 Ctrl+B)
3. 生成 `rt-thread.elf` 和 `rt-thread.bin`

### 烧录

使用 RT-Thread Studio 内置烧录功能，或通过 OpenOCD：

```bash
openocd -f interface/jlink.cfg -f target/stm32f1x.cfg -c "program rt-thread.elf verify reset exit"
```

## 调试方法

### 串口日志

系统启动后通过 USART1 输出监控数据：

```
[MONITOR] Volt: 22.3V, Temp: 45.2C, ALM:0, FC:0
```

### 栈诊断

Actuator 线程每 10 秒打印一次所有线程栈峰值使用率：

```
[STACK] rawdata_thread1: 456/1024 (44%)
[STACK] Logic_thread3: 892/1536 (58%)
...
```

### FinSH 命令

```
list_thread      # 查看线程状态
list_sem         # 查看信号量状态
list_mq          # 查看消息队列状态
free             # 查看堆内存使用情况
```

## 项目结构

```
test_5_signer/
├── applications/          # CubeMX 生成的 main.c 入口
├── cubemx/                # CubeMX 自动生成的 HAL 代码
├── modules/
│   ├── app/               # 应用层线程
│   │   ├── app_acquire.c      # 数据采集
│   │   ├── app_predict.c      # 特征提取
│   │   ├── app_supervisor.c   # 健康评估
│   │   ├── app_actuator.c     # 告警执行
│   │   └── app_display.c      # 串口日志
│   ├── drivers/           # 硬件驱动层
│   │   ├── bsp_adc.c/h        # DMA ADC 双缓冲
│   │   ├── bsp_beep.c/h       # PWM 蜂鸣器
│   │   └── bsp_throttle.c/h   # 油门输入(占位)
│   └── middle/            # 中间件层
│       ├── mid_databus.c/h    # IPC 对象 + 共享数据
│       └── mid_filter.c/h     # 滤波算法库
├── rt-thread/             # RT-Thread Nano 内核
└── docs/                  # 设计文档
```

## 设计亮点

1. **DMA 双缓冲 + 影子缓冲**：ADC 采样与数据处理完全解耦，零拷贝无阻塞
2. **双时间尺度电压模型**：快尺度跟踪瞬态压降，慢尺度跟踪 SOC 基准，油门联动冻结逻辑
3. **表驱动 FSM**：状态转移规则集中管理，易于调整阈值与滞回参数
4. **故障根因锁存**：6 类故障枚举，仅记录最先触发事件，便于事后分析
5. **幂等分发机制**：Actuator 仅在策略变化时执行，避免重复操作外设

## 扩展方向

- **Layer9 看门狗**：实现 IWDG + 各线程心跳检测
- **油门解码**：替换 bsp_throttle 占位，接 SBUS/PWM 输入捕获
- **差异化响应**：过温时限制油门，欠压时降低采样率
- **数据记录**：SD 卡或 Flash 记录故障日志，支持事后回放

## 许可证

SPDX-License-Identifier: Apache-2.0
