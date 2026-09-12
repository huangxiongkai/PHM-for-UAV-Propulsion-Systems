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
| — | 故障原因记录 (fault_cause) | ✅ 完成 |

### 🔴 待实现

| 项目 | 说明 |
|------|------|
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
    uint8_t alarm_level;     /* ALARM_SAFE / ALARM_WARNING / ALARM_DANGER / ALARM_HARDFAULT */
    uint8_t hard_fault;      /* 位图: bit0=过温, bit1=欠压 */
    uint8_t sensor_fault;    /* 0=正常, 1=NTC故障, 2=ADC卡死, 3=电压传感器开路/短路 */
    rt_tick_t timestamp;            /* Predict线程心跳时间戳 */
    rt_tick_t supervisor_heartbeat; /* Supervisor线程心跳时间戳 */
    uint8_t fault_cause;     /* 故障原因: fault_cause_t 枚举值 */
} monitor_msg_t;
```

**当前大小**: 40 字节（7×4 float + 4×1 uint8 + 2×4 rt_tick_t = 40）

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

## 六、故障原因与锁存机制（已实现）

Supervisor 在故障锁存（Layer6）阶段记录最先触发的故障根因，写入 `monitor_msg.fault_cause` 并传递给 Actuator，用于告警差异化展示。

### fault_cause_t 取值（mid_databus.h）

| 值 | 枚举 | 触发条件 |
|:---|:---|:---|
| 0 | FAULT_NONE | 无故障 |
| 1 | FAULT_PREDICT_INIT | Predict 启动超时（信号量等待 500ms 超时） |
| 2 | FAULT_PREDICT_LOST | Predict 运行卡死（心跳时间戳 200ms 未刷新） |
| 3 | FAULT_SENSOR | 传感器故障（sensor_fault ≠ 0） |
| 4 | FAULT_OVERTEMP | 过温（hard_fault bit0） |
| 5 | FAULT_UNDERVOLT | 欠压（hard_fault bit1） |

### 锁存规则

- 多故障并发时**只记录最先触发者**，后续故障不覆盖根因字段，避免竞态导致根因丢失
- 锁存后 `fault_cause` 保持不变，直到系统重启清除

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
