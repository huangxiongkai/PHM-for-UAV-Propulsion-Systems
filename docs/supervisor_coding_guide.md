# Supervisor 编码指南 V2.2

> 本文档是 Supervisor 线程的唯一权威参考，编码阶段严格遵循。

---

## 一、当前实现状态（截至 2026-07-02）

### ✅ 已完成

| 层级 | 功能 | 状态 |
|------|------|------|
| Layer1 | 互斥量快照 (mutex副本机制) | ✅ 完成 |
| Layer2 | 冷启动首帧定态 (信号量等待 + evaluate_initial_state) | ✅ 完成 |
| Layer3 | HI计算 (定点化, risk_sum限幅) | ✅ 完成 |
| Layer4 | Condition Hold (统一hold_target+hold_cnt) | ✅ 完成 |
| Layer5 | Health FSM (表驱动, SAFE/WARNING/DANGER三态) | ✅ 完成 |
| Layer6 | Fault Latch (sensor_fault ∥ hard_fault ∥ predict_timeout) | ✅ 完成 |
| Layer7 | Alarm Fusion (每轮同步alarm_level) | ✅ 完成 |
| Layer8 | Event Publish (边沿触发, rt_uint32_t evt_bit) | ✅ 完成 |
| Layer9 | Watchdog Qualification | 🔴 待实现 |
| — | 故障原因记录 (fault_cause) | 🔴 待实现 |

### 🔴 待实现（V2.2 升级）

| 项目 | 说明 |
|------|------|
| 故障原因传递 | Layer6 记录具体故障源，通过 monitor_msg.fault_cause 传递给 Actuator |
| 看门狗喂狗 | Layer9 检测各线程心跳，决定是否喂 IWDG |

---

## 二、架构最终定稿

| # | 决策项 | 最终结论 |
|---|--------|---------|
| 1 | `timestamp` 字段 | ✅ 已新增到 monitor_msg_t |
| 2 | `hi_x10` 字段 | ❌ 不新增，Supervisor 内部计算 |
| 3 | `EVT_HARDFAULT` | ✅ 已新增 `(1<<3)` |
| 4 | 优先级 | Acquire(8) > Predict(9) > Supervisor(10) > Actuator(11) > Display(15) |
| 5 | Fault Latch 解除 | ❌ 不实现，上电复位解除 |
| 6 | Predict 超时 | 200ms，使用可移植宏 `(RT_TICK_PER_SECOND * 200) / 1000` |
| 7 | 冷启动首帧定态 | ✅ 信号量等待 Predict 首次写入，超时则 fault_latched=1 |
| 8 | FSM 风格 | 表驱动，4 条转换规则 |
| 9 | Supervisor 再滤波 | ❌ 取消 |
| 10 | Mutex 策略 | 快照复制后立即释放 |
| 11 | Supervisor 栈 | 1024B |
| 12 | Supervisor 调度 | 20ms 周期(50Hz) |
| 13 | Fault Latch 触发 | sensor_fault ∥ hard_fault ∥ predict_timeout（HI 低值不触发） |
| 14 | FSM 计数器 | 统一 hold_target + hold_cnt |
| 15 | monitor_msg_t 对齐 | float区 → uint8区 → rt_tick_t区，天然 32-bit 对齐 |
| 16 | 初始同步机制 | ✅ 信号量 predict_ready_sem（Predict 首次写入后释放） |

---

## 三、数据流

```
Predict 写回 monitor_msg (mutex保护, timestamp=rt_tick_get())
  │
  ├─ rt_sem_release(predict_ready_sem)  ← 仅首次
  │
  ▼
Supervisor (20ms周期):
  ├─ rt_sem_take(predict_ready_sem, 500)  ← 主循环之前，等待 Predict 首次写入
  │   └─ 超时 → fault_latched=1, fault_cause=FAULT_PREDICT_INIT
  │
  └─ while(1):
      L1: rt_mutex_take → memcpy → rt_mutex_release (锁外计算)
      L2: Cold Start: first_run → evaluate_initial_state(hi_x10) → 同步 alarm_level + last_alarm → 不发事件 → continue
      L3: HI Compute: hi_x10 = (100 - risk_sum) × 10, risk_sum 限幅 [0,100]
      L4: Condition Hold: 对照 FSM 表，hold_cnt 计时
      L5: Health FSM: 查表匹配 current_state → target 转移
      L6: Fault Latch: sensor_fault || hard_fault || predict_timeout → latched=1
          └─ 🔴 待升级: 记录 fault_cause
      L7: Alarm Fusion: latched → ALARM_HARDFAULT; 否则 = health_state
          └─ 每轮同步 monitor_msg.alarm_level
      L8: Event Publish: alarm != last_alarm → rt_event_send(边沿触发)
      L9: Watchdog(🔴 待实现)
  │
  ▼
Actuator (优先级 11):
  rt_event_recv(adc_event, EVT_*, OR+CLR, FOREVER)
  → beep_stop() / beep_slow() / beep_fast()
  └─ 🔴 待升级: 根据 fault_cause 差异化响应
```

---

## 四、当前 monitor_msg_t 结构

```c
typedef struct {
    float temperature;       /* 滤波后温度 (℃) */
    float voltage;           /* 滤波后电压 (V) */
    float dt_tem;            /* 温度微分 (℃/s) */
    float dv_vol;            /* 电压微分 (V/s) */
    float drop_ratio;        /* 归一化压降比 [0,1] */
    float temp_risk_contrib; /* 温度风险贡献 [0,50] */
    float drop_risk_contrib; /* 压降风险贡献 [0,50] */
    uint8_t alarm_level;     /* ALARM_SAFE / WARNING / DANGER / HARDFAULT */
    uint8_t hard_fault;      /* 位图: bit0=过温, bit1=欠压 */
    uint8_t sensor_fault;    /* 0=正常, 1=NTC故障, 2=ADC卡死 */
    rt_tick_t timestamp;     /* Predict写回时刻 (rt_tick_get) */
} monitor_msg_t;
```

**当前大小**: 36 字节（7×4 float + 3×1 uint8 + 4 rt_tick_t = 35，对齐到 36）

---

## 五、FSM 转换规则

| 转移 | HI 条件 (×10) | 持续时间 | hold_frames (20ms/帧) |
|------|--------------|---------|----------------------|
| SAFE → WARNING | hi_x10 < 800 (HI<80) | 200ms | 10 |
| WARNING → DANGER | hi_x10 < 400 (HI<40) | 100ms | 5 |
| WARNING → SAFE | hi_x10 > 840 (HI>84) | 1000ms | 50 |
| DANGER → WARNING | hi_x10 > 450 (HI>45) | 500ms | 25 |

**设计原则**: 恶化快、恢复慢（符合动力系统物理规律）

---

## 六、V2.2 升级方案：故障原因传递

### 6.1 问题背景

当前 Layer6 只设置 `fault_latched = 1`，无法区分故障源：

| 故障源 | 语义 | 当前表现 |
|--------|------|---------|
| Predict 启动超时 | 信号量 500ms 超时 | `ALARM_HARDFAULT`，无细分 |
| Predict 运行中卡死 | timestamp 停止更新 | `ALARM_HARDFAULT`，无细分 |
| sensor_fault != 0 | ADC卡死 / NTC故障 | `ALARM_HARDFAULT`，无细分 |
| hard_fault bit0 | 过温 (>105℃) | `ALARM_HARDFAULT`，无细分 |
| hard_fault bit1 | 欠压 (<16V) | `ALARM_HARDFAULT`，无细分 |

Actuator 无法根据故障类型做差异化响应。

### 6.2 设计方案

#### 6.2.1 新增故障原因枚举（mid_databus.h）

```c
/* ---------- 故障原因（Supervisor → Actuator） ---------- */
typedef enum {
    FAULT_NONE          = 0,  /* 无故障 */
    FAULT_PREDICT_INIT  = 1,  /* Predict启动超时(信号量) */
    FAULT_PREDICT_LOST  = 2,  /* Predict运行时卡死(时间戳) */
    FAULT_SENSOR        = 3,  /* 传感器故障(sensor_fault!=0) */
    FAULT_OVERTEMP      = 4,  /* 过温(hard_fault bit0) */
    FAULT_UNDERVOLT     = 5,  /* 欠压(hard_fault bit1) */
} fault_cause_t;
```

#### 6.2.2 monitor_msg_t 新增字段

```c
typedef struct {
    // ... 原有字段 ...
    uint8_t reserved;        /* 保留对齐 */
    uint8_t fault_cause;     /* fault_cause_t, 0=无故障 */
} monitor_msg_t;
```

**结构体大小变化**: 36 → 40 字节

#### 6.2.3 Supervisor 新增静态变量

```c
static fault_cause_t fault_cause = FAULT_NONE;
```

#### 6.2.4 Layer6 改造：记录最先触发的故障原因

```c
/* 故障锁存：记录最早触发的原因，后续不覆盖 */
if (!fault_latched)
{
    if (local.sensor_fault != 0)
    {
        fault_latched = 1;
        fault_cause   = FAULT_SENSOR;
    }
    else if (local.hard_fault & 0x01)
    {
        fault_latched = 1;
        fault_cause   = FAULT_OVERTEMP;
    }
    else if (local.hard_fault & 0x02)
    {
        fault_latched = 1;
        fault_cause   = FAULT_UNDERVOLT;
    }
    else if ((rt_tick_get() - local.timestamp) > PREDICT_TIMEOUT_TICKS)
    {
        fault_latched = 1;
        fault_cause   = FAULT_PREDICT_LOST;
    }
}
```

**设计选择**: 只记录**最先触发**的原因（`if (!fault_latched)` 保护），不被后续故障覆盖。排查 bug 时看 `fault_cause` 就知道根因。

#### 6.2.5 Layer7 改造：同步 fault_cause 到 monitor_msg

```c
/* ===== Layer7: 告警融合 + 故障原因同步 ===== */
uint8_t final_alarm;
if (fault_latched)
    final_alarm = ALARM_HARDFAULT;
else
    final_alarm = health_state;

/* 同步alarm_level + fault_cause到monitor_msg */
rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
monitor_msg.alarm_level = final_alarm;
monitor_msg.fault_cause = (uint8_t)fault_cause;
rt_mutex_release(sensor_mutex);
```

#### 6.2.6 信号量超时处设置原因

```c
/* 主循环之前 */
if (rt_sem_take(predict_ready_sem, 500) != RT_EOK)
{
    fault_latched = 1;
    fault_cause   = FAULT_PREDICT_INIT;
}
```

### 6.3 Actuator 差异化响应（可选）

Actuator 目前是事件驱动，如需读 `fault_cause` 则需额外读 `monitor_msg`：

```c
if (evt_bit == EVT_HARDFAULT)
{
    rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
    uint8_t cause = monitor_msg.fault_cause;
    rt_mutex_release(sensor_mutex);

    switch (cause)
    {
        case FAULT_PREDICT_INIT:
        case FAULT_PREDICT_LOST:
            /* Predict故障: 蜂鸣器急促提示 */
            beep_fast();
            break;
        case FAULT_SENSOR:
            /* 传感器故障: 蜂鸣器持续提示 */
            beep_slow();
            break;
        case FAULT_OVERTEMP:
            /* 过温: 蜂鸣器持续提示 */
            beep_slow();
            break;
        case FAULT_UNDERVOLT:
            /* 欠压: 蜂鸣器急促提示 */
            beep_fast();
            break;
    }
}
```

> 注：当前版本只做蜂鸣器/LED 声光提示，不存在 `throttle_limit()` 之类的油门控制接口；限油门、自动降落等动力处置属于后续规划，未实现。

### 6.4 修改影响范围

| 文件 | 改动量 | 风险 |
|------|--------|------|
| `mid_databus.h` | +枚举定义 +1字段 | 低，结构体大小 36→40 |
| `mid_databus.c` | +1 初始化值 | 无 |
| `app_supervisor.c` | +1 静态变量，Layer6/Layer7 改写 | 中，FSM 不变 |
| `app_actuator.c` | 可选扩展 | 无，不影响现有逻辑 |
| `app_predict.c` | 无改动 | — |

### 6.5 验证矩阵

| 场景 | 预期 `fault_cause` | 预期 `alarm_level` |
|------|-------------------|-------------------|
| 正常启动 | `FAULT_NONE (0)` | `SAFE` |
| Predict 500ms 未响应 | `FAULT_PREDICT_INIT (1)` | `HARDFAULT` |
| Predict 运行中卡死 | `FAULT_PREDICT_LOST (2)` | `HARDFAULT` |
| NTC 故障 | `FAULT_SENSOR (3)` | `HARDFAULT` |
| 温度超过 105℃ | `FAULT_OVERTEMP (4)` | `HARDFAULT` |
| 电压低于 16V | `FAULT_UNDERVOLT (5)` | `HARDFAULT` |

---

## 七、V2.2 升级方案：看门狗设计（Layer9）

### 7.1 设计目标

利用 STM32 硬件 IWDG（独立看门狗），监控整个系统健康状态：

- 所有关键线程正常运行 → 喂狗
- 任一关键线程卡死 → 停止喂狗 → IWDG 复位系统

### 7.2 线程心跳机制

每个关键线程定期更新自己的"心跳时间戳"：

```c
/* 各线程入口函数中 */
static rt_tick_t acquire_heartbeat = 0;
static rt_tick_t predict_heartbeat = 0;
static rt_tick_t supervisor_heartbeat = 0;
static rt_tick_t actuator_heartbeat = 0;

/* 每轮循环更新 */
acquire_heartbeat = rt_tick_get();
```

### 7.3 Supervisor Layer9 实现

```c
/* ===== Layer9: 看门狗 Qualification ===== */
#define HEARTBEAT_TIMEOUT_TICKS  ((RT_TICK_PER_SECOND * 500) / 1000)  /* 500ms */

static void watchdog_qualify(void)
{
    rt_tick_t now = rt_tick_get();

    /* 检查各线程心跳 */
    rt_bool_t all_alive = RT_TRUE;

    if ((now - acquire_heartbeat) > HEARTBEAT_TIMEOUT_TICKS)
        all_alive = RT_FALSE;
    if ((now - predict_heartbeat) > HEARTBEAT_TIMEOUT_TICKS)
        all_alive = RT_FALSE;
    if ((now - actuator_heartbeat) > HEARTBEAT_TIMEOUT_TICKS)
        all_alive = RT_FALSE;

    /* 所有线程正常才喂狗 */
    if (all_alive && !fault_latched)
    {
        HAL_IWDG_Refresh(&hiwdg);  /* 喂狗 */
    }
    /* 否则不喂狗，等待 IWDG 复位 */
}
```

### 7.4 IWDG 配置

在 `main.c` 中初始化 IWDG：

```c
/* IWDG 超时时间: 2秒 */
hiwdg.Instance = IWDG;
hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
hiwdg.Init.Reload = 40000;  /* 约 2 秒 @ 40kHz LSI */
HAL_IWDG_Init(&hiwdg);
```

### 7.5 与现有故障检测的关系

| 机制 | 检测范围 | 响应方式 |
|------|---------|---------|
| 信号量超时 | Predict 启动失败 | fault_latched=1，HARDFAULT 告警 |
| 时间戳检查 | Predict 运行中卡死 | fault_latched=1，HARDFAULT 告警 |
| 看门狗 | 整个系统（含 Supervisor 自身） | IWDG 复位 |

**层次关系**:
- 时间戳/信号量 → 线程级检测 → 软件告警
- 看门狗 → 系统级检测 → 硬件复位

### 7.6 注意事项

1. **Supervisor 自身卡死**: 如果 Supervisor 卡死，Layer9 不会执行，IWDG 不喂狗 → 复位
2. **fault_latched 时不喂狗**: 故障锁存后主动停止喂狗，触发复位（可选策略）
3. **心跳变量作用域**: 需要 `extern` 声明或放在共享头文件中

---

## 八、最终参数表

### 调度周期

```
20ms, 50Hz
```

### HI 计算

```
hi_x10 = (int16_t)((100.0f - risk_sum) * 10.0f)
risk_sum 限幅: [0, 100]
hi_x10 范围: [0, 1000]
```

### FSM 阈值 (×10)

```
800 (HI<80 → SAFE→WARNING)
840 (HI>84 → WARNING→SAFE)
400 (HI<40 → WARNING→DANGER)
450 (HI>45 → DANGER→WARNING)
```

### 时间确认 (帧数 × 20ms)

```
10帧 = 200ms  (SAFE→WARNING)
5帧  = 100ms  (WARNING→DANGER)
50帧 = 1000ms (WARNING→SAFE)
25帧 = 500ms  (DANGER→WARNING)
```

### Predict 超时

```
200ms
宏: ((RT_TICK_PER_SECOND * 200) / 1000)
```

### 信号量超时

```
500ms (等待 Predict 首次写入)
```

### 优先级分配

```
Acquire    8
Predict    9
Supervisor 10
Actuator   11
Display    15
```

---

## 九、IPC 对象一览

| 对象 | 类型 | 创建位置 | 用途 |
|------|------|---------|------|
| `monitor_mq` | MessageQueue | mid_databus.c | Acquire → Predict 数据传输 |
| `alarm_sem` | Semaphore | mid_databus.c | DMA ISR → Acquire 通知 |
| `predict_ready_sem` | Semaphore | mid_databus.c | Predict 首次写入 → Supervisor 冷启动同步 |
| `adc_event` | Event | mid_databus.c | Supervisor → Actuator 告警事件 |
| `sensor_mutex` | Mutex | app_acquire.c | 保护 monitor_msg 读写 |

---

## 十、编码约束

1. **不启用 `%f` 打印** — 使用 `int_part + dec_part` 手动拆分
2. **不修改 `rtconfig.h` / `Kconfig`**
3. **代码输出在聊天框** — 用户手动复制粘贴
4. **每步等用户确认** — 回复"继续"进入下一步
5. **每步附带三要素** — 改动总结 + 验证点 + 下一步建议
6. **中文注释** — 所有代码注释使用中文

---

## 十一、设计原则总结

```
✓ Mutex副本机制
✓ 信号量初始同步 (predict_ready_sem)
✓ 冷启动首帧定态 + 状态同步
✓ HI定点化 (Supervisor内部计算)
✓ risk_sum 限幅 [0, 100]
✓ 表驱动FSM (统一hold_target+hold_cnt)
✓ Condition Hold (恶化快/恢复慢)
✓ Health/Fault解耦 (两个维度不混淆)
✓ Fault Latch (上电复位解除)
✓ Predict超时检测 (可移植宏)
✓ 边沿事件发布 (含EVT_HARDFAULT)
✓ alarm_level 每轮同步
✓ rt_uint32_t evt_bit (类型正确)
✓ Watchdog扩展预留

✗ Supervisor二次IIR
✗ MQ重构现有架构
✗ HARDFAULT混入FSM三态
✗ 周期性重复发事件
✗ 故障自动恢复
✗ HI低值触发Fault Latch
✗ 每条规则独立hold计数器
✗ monitor_msg中存储hi_x10 (派生数据不在多线程维护)
✗ rt_event_t evt_bit (类型错误, 应为 rt_uint32_t)
✗ 冷启动轮询等待 (应使用信号量)
```
