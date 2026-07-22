# Actuator 编码指南 V1.0

> 本文档是 Actuator 线程的唯一权威参考，编码阶段严格遵循。
> 定位：本文档遵循"先冻结最小可行架构、写出能跑的代码、再迭代"的原则，
> 刻意砍掉了讨论阶段中一切没有对应硬件/用例支撑的层级与接口。

---

## 〇、写在前面：为什么是这个版本

讨论阶段（ADR）中出现过一个六层方案（Event Receive → Context Snapshot →
Policy Resolver → Action Planner → Dispatcher → Driver Adapter），并且遗留了
5 个未决问题（Policy/Plan 边界、Throttle Slew、Failsafe 策略、Execution Plan
结构、Driver 粒度）。

本指南的态度是：**当前硬件只有 LED + 蜂鸣器 + 电位器 + NTC，没有 ESC、没有
真实油门输出。为不存在的执行对象设计通用分发框架、Capability 预留、Throttle
Slew，是在为假想复杂度买单。** 所以本版本：

- 六层压缩为 **四层**（不改变各层"应该做什么"的思想，只是不强行拆代码文件/拆结构体）
- Throttle Slew、Capability、ACK/Retry 全部列为 **Future Work，明确不做**
- Execution Plan 就是一个两字段 struct，不做命令集合抽象
- 一个设备一个 Driver 函数，不做统一 `execute_action_plan()` 分发

等真正接入第二种/第三种硬件（比如 UART 日志 + CAN）时，再回来抽象——那时候
才有"至少两个真实用例"可以归纳，而不是提前猜。

---

## 〇·五、CubeMX 硬件配置现状

> 本节记录 CubeMX 已完成的引脚配置，Driver Adapter 直接引用这些配置，
> 不重复初始化。CubeMX 重新生成会覆盖 `cubemx/` 目录，但不会改变引脚映射——
> Driver Adapter 通过 `main.h` 宏间接引用，因此是安全的。

### RGB LED

| 通道 | 引脚 | 宏名 | 逻辑 | 初始电平 |
|------|------|------|------|---------|
| 红(R) | PA1 | `LED_R_Pin` / `LED_R_GPIO_Port` | Active-low: `GPIO_PIN_RESET`=亮, `GPIO_PIN_SET`=灭 | `GPIO_PIN_SET`（灭） |
| 绿(G) | PA2 | `LED_G_Pin` / `LED_G_GPIO_Port` | 同上 | `GPIO_PIN_SET`（灭） |
| 蓝(B) | PA3 | `LED_B_Pin` / `LED_B_GPIO_Port` | 同上 | `GPIO_PIN_SET`（灭） |

配置来源：`cubemx/Src/gpio.c` → `MX_GPIO_Init()`，推挽输出、无上下拉、低速。

### 蜂鸣器

| 项目 | 值 |
|------|-----|
| 引脚 | PB11（TIM2 CH4, `__HAL_AFIO_REMAP_TIM2_PARTIAL_2()`） |
| 驱动方式 | PWM（TIM2, PSC=72-1, 定时器时钟 1MHz） |
| 已有接口 | `beep_stop()` / `beep_slow(3kHz)` / `beep_fast(4kHz)` |
| 内部函数 | `pwm_set(freq, duty)` — 设置任意频率+占空比，**当前未在 `bsp_beep.h` 声明** |
| 句柄 | `extern TIM_HandleTypeDef htim2`（`bsp_beep.h` 已声明） |

### ADR-ACT-01：LED Driver 直接调用 HAL GPIO

**决策**：LED Driver Adapter 直接调用 `HAL_GPIO_WritePin()`，不引入 RT-Thread PIN
设备框架（`rt_pin_write` / `rt_device_find`）。

**理由**：
- 本项目绑定 CubeMX + HAL 生态，PIN 框架需要额外的设备注册和查找开销
- CubeMX 已生成引脚宏（`LED_R_Pin` 等），直接使用最简洁
- 三路 LED 需要同时操作不同 GPIO Port（虽然当前都在 GPIOA），PIN 框架不提供额外收益

**CubeMX 重新生成影响**：
- LED/蜂鸣器引脚配置由 CubeMX 自动管理（`gpio.c` / `tim.c`）
- Driver Adapter 仅引用 `main.h` 中的宏定义，不依赖具体实现细节
- 若引脚变更，只需在 CubeMX 中修改，Driver Adapter 代码无需改动

---

## 一、架构总览

```
Supervisor (优先级10, 20ms周期)
      │ rt_event_send(adc_event, evt_bit)  ← 边沿触发
      ▼
┌─────────────────────────────────────────────┐
│ Actuator Thread (优先级11, 事件驱动)          │
│                                               │
│ L1  Event + Context                          │
│     rt_event_recv(超时500ms) → 读monitor_msg │
│     快照 → 用timestamp陈旧度判断Supervisor存活│
│                                               │
│ L2  Policy Table                             │
│     (alarm_level, hard_fault, sensor_fault,  │
│      supervisor_lost) → {led_mode_t,         │
│      beep_mode_t}  查表，不重新决策           │
│                                               │
│ L3  Dispatcher                               │
│     幂等检查(与上次plan相同则跳过) → 调用Driver│
│                                               │
│ L4  Driver Adapter                           │
│     drv_led_set(led_mode_t) → HAL_GPIO三路   │
│     drv_beep_set(beep_mode_t) → PWM+状态机   │
│     内部用 rt_timer (SOFT_TIMER) 实现闪烁/节奏│
└─────────────────────────────────────────────┘
      │                    │
      ▼                    ▼
  RGB LED (PA1/PA2/PA3)  蜂鸣器 (PB11, TIM2 CH4 PWM)
```

**单向流水线原则（保留自讨论阶段，这条是对的，不砍）**：
- Actuator 不做健康评估，不重新 Decision——Decision 只发生在 Supervisor
- Actuator 不直接在 while 循环里 `delay` 做闪烁——会阻塞事件响应的实时性
- 所有硬件相关代码收敛在 L4，L1~L3 未来换 UART/CAN 也不用改

---

## 二、架构决策表

| # | 决策项 | 结论 | 理由 |
|---|--------|------|------|
| 1 | 层级数量 | 4层（非讨论阶段的6层） | 无对应硬件/用例支撑通用分发框架 |
| 2 | Event 等待方式 | 带超时的 `rt_event_recv`，非 `RT_WAITING_FOREVER` | 避免 Supervisor 卡死时 Actuator 永久沉睡 |
| 3 | 超时后如何判断 Supervisor 失联 | 检查 `monitor_msg.timestamp` 陈旧度 > 500ms | Supervisor 用边沿事件，长期 SAFE 不发事件，超时≠失联 |
| 4 | Policy 与 Plan 是否拆两层代码 | 不拆，一个函数内查表+映射即可 | 拆分只在未来有多平台差异化时才有价值 |
| 5 | Execution Plan 数据结构 | `{led_mode_t led; beep_mode_t beep;}` 两 struct 组合 | 不做命令集合抽象，YAGNI；struct 内含多维度字段（颜色/闪烁/音高/节奏） |
| 6 | Dispatcher 幂等检查 | 保留：与上次 plan 相同则跳过 Driver 调用 | 减少不必要的 GPIO 翻转，成本几乎为零 |
| 7 | ACK / Retry / 优先队列 | 不做 | 当前无 UART/CAN，过度设计 |
| 8 | Capability 预留 | 不做接口预留，等真实接入第二种硬件再抽象 | 提前抽象没有真实约束依据，容易抽错 |
| 9 | Throttle Slew | **不做**（Future Work） | 无 ESC，无真实油门对象，没有验证价值 |
| 10 | LED/蜂鸣器周期动作实现方式 | `rt_timer`（SOFT_TIMER + PERIODIC） | 避免线程内阻塞延时，不影响事件响应 |
| 11 | FAILSAFE（Supervisor 失联）策略 | 仅报警（蜂鸣器急促 + LED常亮），不做限流/强制动作 | 无真实执行对象可限，做限流没有验证意义 |
| 12 | Driver 接口粒度 | 一个设备一个函数 | 只有2种设备，统一分发框架无必要 |
| 13 | Actuator 栈大小 | 建议 1536B（讨论阶段提议） | `rt_kprintf` 等调用容易占栈，1024B 偏紧 |
| 14 | 定时器软/硬模式 | `RT_TIMER_FLAG_SOFT_TIMER`，需确认 RT-Thread Nano 已启用软件定时器线程 | 回调运行在系统 timer 线程上下文，不在中断里，可安全调用 GPIO/简单逻辑；但仍不能阻塞 |
| 15 | LED 驱动方式 | `HAL_GPIO_WritePin` 直接调用，不走 RT-Thread PIN 框架 | 本项目绑定 CubeMX+HAL 生态，PIN 框架无真实收益（见〇·五节 ADR-ACT-01） |
| 16 | LED 数据结构 | RGB 组合 struct（`r_on/g_on/b_on/blink/blink_period_ms`），非单色枚举 | 硬件是三路独立 GPIO，需颜色+闪烁两维度 |
| 17 | 蜂鸣器节奏实现 | `rt_timer`（SOFT_TIMER + PERIODIC）+ PWM 通断控制 | 需要"鸣叫节奏"维度，仅靠 PWM 频率不足以区分所有等级（如 DANGER 双连鸣） |
| 18 | 蜂鸣器数据结构 | 多维度 struct（`pwm_freq/on_ms/inter_beep_ms/group_gap_ms/beats_per_group/is_mute`） | 需支持分组鸣叫（双连鸣），简单 on/off 枚举无法表达 |

---

## 三、数据结构

### 3.1 Execution Plan（L2 → L3）

> **重要：硬件 LED 声明**
>
> 开发板配备 3 颗独立单色 LED（红 PA1 / 绿 PA2 / 蓝 PA3），均为 active-low。
> **硬件上没有"黄色 LED"**。本文档所说的"黄色"（WARNING 状态）= **红 + 绿 同亮** = 人眼感知为黄光。
>
> | 告警状态 | 文档描述 | 实际 LED 组合 |
> |---------|----------|---------------|
> | SAFE    | 绿色 ✅ | G ON，R OFF，B OFF |
> | WARNING | **黄色** ⚠️ | **R ON + G ON，B OFF** |
> | DANGER  | 红色 🔴 | R ON，G OFF，B OFF |
> | HARDFAULT | 红色（急促）| R ON，G OFF，B OFF |
> | 失联 | 蓝色 🔵 | B ON，R OFF，G OFF |

```c
/* ---------- LED 模式（RGB 色彩 + 闪烁） ---------- */
typedef struct {
    rt_bool_t   r_on;              /* 红灯亮 */
    rt_bool_t   g_on;              /* 绿灯亮 */
    rt_bool_t   b_on;              /* 蓝灯亮 */
    rt_bool_t   blink;             /* 是否闪烁 */
    rt_uint32_t blink_period_ms;   /* 闪烁周期(ms)，blink=false 时无效 */
} led_mode_t;

/* ---------- 蜂鸣器模式（音高 + 节奏分组） ---------- */
typedef struct {
    rt_uint32_t pwm_freq;        /* 音高: 3000Hz 或 4000Hz, 0=不使用 */
    rt_uint32_t on_ms;           /* 单次鸣叫时长(ms), 0=持续发声 */
    rt_uint32_t inter_beep_ms;   /* 组内 beep 间隔(ms), 0=无分组 */
    rt_uint32_t group_gap_ms;    /* 组间静默(ms), 0=连续/长鸣 */
    rt_uint8_t  beats_per_group; /* 每组 beep 数: 1=单次, 2=双连, 0=连续不分组 */
    rt_bool_t   is_mute;         /* 静音标志 */
} beep_mode_t;

/* ---------- Actuator 执行计划（Policy Table 输出） ---------- */
typedef struct {
    led_mode_t  led;
    beep_mode_t beep;
} action_plan_t;
```

#### RGB 色彩映射表

| 告警状态 | 颜色 | r_on | g_on | b_on | 闪烁 |
|---------|------|------|------|------|------|
| SAFE | 绿 | ✗ | ✓ | ✗ | 常亮 |
| WARNING | 黄 | ✓ | ✓ | ✗ | 慢闪 500ms |
| DANGER | 红 | ✓ | ✗ | ✗ | 快闪 100ms |
| HARDFAULT | 红 | ✓ | ✗ | ✗ | 快闪 100ms（靠蜂鸣器节奏区分） |
| Supervisor 失联 | 蓝 | ✗ | ✗ | ✓ | 常亮 |

#### 蜂鸣器告警映射表（完整五维参数）

| 告警状态 | 音高 | 鸣叫时长 | 组内间隔 | 组间静默 | 每组拍数 | 静音 | 听觉效果 |
|---------|------|---------|---------|---------|---------|------|---------|
| SAFE | — | — | — | — | — | ✓ | 无声 |
| WARNING | 3kHz | 100ms | — | 2000ms | 1 | — | 单次短鸣，2s 间隔 |
| DANGER | 3kHz | 100ms | 100ms | 1000ms | 2 | — | 双连鸣，1s 周期 |
| HARDFAULT | 4kHz | 50ms | 50ms | — | 0(连续) | — | 急促连续鸣 |
| Supervisor 失联 | 4kHz | 0(持续) | — | — | — | — | 长鸣不间断 |

### 3.2 Policy Table（核心映射逻辑）

> **注意**：`monitor_msg_t` 没有 `fault_cause` 字段。故障信息分散在三个位置：
> - `hard_fault`（uint8_t 位图：bit0=过温, bit1=欠压）
> - `sensor_fault`（uint8_t 枚举：0=正常, 1=NTC故障, 2=ADC卡死, 3=电压传感器开路/短路）
> - `timestamp` 陈旧度（用于判断 Predict 超时）
>
> 因此 `resolve_policy` 签名接收 `hard_fault` 和 `sensor_fault` 而非单一的 `fault_cause`。

**函数签名**：
```c
static action_plan_t resolve_policy(uint8_t alarm_level,
                                     uint8_t hard_fault,
                                     uint8_t sensor_fault,
                                     rt_bool_t supervisor_lost)
```

**查表逻辑**：supervisor_lost 优先级最高 → ALARM_HARDFAULT → ALARM_DANGER → ALARM_WARNING → ALARM_SAFE，返回对应的 `{led_mode_t, beep_mode_t}` 组合。具体参数见 3.1 节的 RGB 色彩映射表和蜂鸣器告警映射表。

---

## 四、Actuator 主线程（L1 + L3 胶合）

**关键宏定义**：
```c
#define ACTUATOR_EVENT_TIMEOUT_TICKS   ((RT_TICK_PER_SECOND * 500) / 1000)  /* 500ms */
#define SUPERVISOR_LOST_TICKS          ((RT_TICK_PER_SECOND * 500) / 1000)  /* 500ms */
```

**主循环逻辑**：
1. `rt_event_recv` 带 500ms 超时等待 Supervisor 告警事件
2. 无论是否超时都读 `monitor_msg` 快照（用 `timestamp` 陈旧度判断 Supervisor 是否存活）
3. 事件超时且 Supervisor 仍存活 → 只是长期 SAFE 没发事件，跳过本轮
4. 调用 `resolve_policy` 查表得到执行计划
5. 幂等检查：与上次 plan 相同则跳过 Driver 调用

**要点说明**：
- `rt_event_recv` 超时不再等价于"故障"，必须叠加 `timestamp` 陈旧度判断，这一点在讨论阶段已经反复确认过，是本设计中最容易踩坑的地方。
- `resolve_policy` 签名接收 `hard_fault` + `sensor_fault` 两个独立字段（而非 `fault_cause`），因为 `monitor_msg_t` 没有 `fault_cause` 字段。
- Dispatcher 幂等检查使用 `rt_memcmp` 整体比较 struct（含对齐填充），成本可忽略（24字节比较），但避免了每 500ms 无意义地重复调用 Driver。

---

## 五、Driver Adapter（L4，唯一硬件相关层）

RT-Thread 的软件定时器分为一次性和周期性两种模式，周期定时器到期后会自动重新
装载并继续触发，直到手动停止；此外定时器还分 `HARD_TIMER`（回调在时钟中断上下
文执行）和 `SOFT_TIMER`（回调在独立的系统定时器线程中执行）两种模式。<cite index="5-1">RT-Thread timer provides two types of timer mechanisms: the first type is a one-shot timer, which only triggers a timer event for onetime after startup, and then the timer stops automatically. The second type is a periodic trigger timer, which periodically triggers a timer event until the user manually stops it, otherwise it will continue to execute forever.</cite> 对 Actuator 这种要在回调里做 GPIO 翻转、且不希望占用中断时间的场景，应选 `RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER`。

> ⚠️ **前置检查项**：`RT_USING_TIMER_SOFT` 已在 `rtconfig.h` 第 15 行确认启用，
> 软件定时器线程可用，无需额外配置。

> ⚠️ **`bsp_beep.h` 需补充声明**：`pwm_set(uint32_t fre, uint32_t duty)` 已存在于
> `bsp_beep.c` 第 45 行，但未在头文件中声明。Driver Adapter 需要调用它来设置任意
> 音高频率，因此需在 `bsp_beep.h` 中新增一行：
> ```c
> void pwm_set(uint32_t fre, uint32_t duty);
> ```

### 5.1 LED Driver

**接口**：`drv_led_set(led_mode_t mode)` — 设置 LED 颜色+闪烁模式

**实现要点**：
- `drv_led_write(r, g, b)` 封装 active-low 反转逻辑（GPIO_PIN_RESET=亮）
- 闪烁模式使用 `rt_timer`（SOFT_TIMER + PERIODIC），回调中翻转 led_phase
- 停止旧 timer → 更新 led_current → 启动新 timer（或直接常亮）

### 5.2 Beep Driver（分组鸣叫状态机）

**接口**：`drv_beep_set(beep_mode_t mode)` — 设置蜂鸣器音高+节奏

**状态机设计**：
- 两相状态：`BEEP_PHASE_ON`（发声）/ `BEEP_PHASE_OFF`（静默）
- 回调驱动：timer 到期 → 切换相位 → 重设计时器时长
- 三种模式分支：静音（is_mute）/ 长鸣（on_ms=0）/ 分组鸣叫（状态机接管）

**分组鸣叫逻辑**：
- 每次发声结束 → beat_count++
- 组内还有 beep → 设 inter_beep_ms 短间隔
- 组完成 → 设 group_gap_ms 长间隔
- 连续模式（beats_per_group=0）→ 一直用 inter_beep_ms

### 5.3 初始化

**创建两个软件定时器**：
- `led_timer`：500ms 周期，用于 LED 闪烁
- `beep_timer`：100ms 周期，用于蜂鸣器分组鸣叫

均使用 `RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER`。

**未来替换路径（仅作说明，不需要现在实现）**：`drv_led_set` / `drv_beep_set`
换成 `drv_uart_send()` / `drv_can_send()` 时，L1~L3 不需要任何改动——这正是
"Driver Adapter 是唯一硬件相关层"这条原则要保证的东西。

---

## 六、FAILSAFE 策略说明（工业界对标，仅供讲述用，不需要额外实现）

业界（如 PX4）对通信/控制信号丢失的处理，通常是先进入一个短暂的 Hold（悬停）
状态给用户反应时间，随后才升级到更严重的动作，并且这个动作的严重程度会随失效
类型叠加取最高等级；<cite index="14-1">If multiple failsafes are triggered, the more severe action is taken. For example if both RC and GPS are lost, and manual control loss is set to Return mode and GCS link loss to Land, Land is executed.</cite> 同时，失效动作本身有一个从轻到重的严重度序列（悬停 <
返航 < 降落 < 断电/终止），<cite index="18-1">The vehicle will enter Hold mode. For multicopters this means the vehicle will hover... The vehicle will enter Return mode... The vehicle will enter Land mode, and lands immediately. Stops the motors immediately.</cite> 本项目没有真实执行对象去实现"返航/降落"，所以只
借用这个思想的最小子集：**Supervisor 失联 = 最高严重度告警（蜂鸣器急促+LED常亮），不做任何执行层动作**。这一点可以在面试中作为"我知道工业界怎么做分级失效响应，但受限于硬件现状选择了最小实现"来讲述，比强行模拟一个不存在的降落动作更真实可信。

---

## 七、验证矩阵

| 场景 | 触发方式 | 预期 LED | 预期蜂鸣 |
|------|---------|---------|---------|
| 正常 SAFE | 电位器/NTC 在正常范围 | 绿色常亮 | 无声 |
| WARNING | 拉高风险值使 HI < 80 超过200ms | 黄色慢闪(500ms) | 单次短鸣(3kHz, 100ms/2s间隔) |
| DANGER | HI < 40 超过100ms | 红色快闪(100ms) | 双连鸣(3kHz, 100-100-1000ms) |
| HARDFAULT - 过温 | NTC模拟>105℃ | 红色快闪(100ms) | 急促连续(4kHz, 50ms周期) |
| HARDFAULT - 欠压 | 电位器模拟<16V | 红色快闪(100ms) | 急促连续(4kHz, 50ms周期) |
| HARDFAULT - 传感器故障 | 断开NTC/ADC卡死 | 红色快闪(100ms) | 急促连续(4kHz, 50ms周期) |
| Predict初始化超时 | 启动时不让Predict写入monitor_msg | 红色快闪(100ms) | 急促连续(4kHz, 50ms周期) |
| Predict运行中卡死 | Predict线程挂起, timestamp停更 | 红色快闪(100ms) | 急促连续(4kHz, 50ms周期) |
| Supervisor失联 | 挂起Supervisor线程 | 蓝色常亮 | 长鸣不间断(4kHz) |

> 最后一行是 Actuator 自主判定（不依赖 Supervisor 发出的 alarm_level），
> 与其余场景的 LED 表现（闪烁）刻意做了区分，方便肉眼快速识别"这是 Actuator
> 自己判断的失联，不是 Supervisor 上报的健康告警"。
>
> **颜色语义**：绿=正常 / 黄=警告 / 红=危险 / 蓝=失联，即使不看蜂鸣器也能通过
> LED 颜色快速判断系统状态等级。

---

## 八、编码约束（与 Supervisor 指南保持一致）

1. **不启用 `%f` 打印** — 涉及浮点打印时手动拆分整数/小数部分
2. **不修改 `rtconfig.h` / `Kconfig`**，唯一例外：需先确认 `RT_USING_TIMER_SOFT` 是否已启用（见第五节前置检查项），若未启用需告知用户手动确认，而非代码里静默处理
3. **代码输出在聊天框** — 用户手动复制粘贴
4. **每步等用户确认** — 回复"继续"进入下一步
5. **每步附带三要素** — 改动总结 + 验证点 + 下一步建议
6. **中文注释** — 所有代码注释使用中文
7. **架构不因编码难度而改变** — 代码不好写就改代码，不改本文档定的架构
8. **LED 为 active-low** — `HAL_GPIO_WritePin` 时 `GPIO_PIN_RESET`=亮、`GPIO_PIN_SET`=灭，此逻辑仅出现在 `drv_led_write` 内部，上层（Policy Table / Dispatcher）不感知硬件极性
9. **`bsp_beep.h` 需暴露 `pwm_set()`** — 原为 `.c` 内部函数，Driver Adapter 需要直接调用以设置任意音高频率（3kHz/4kHz），需在头文件新增声明

---

## 九、明确列为 Future Work（现在不做，写在这里防止讨论反复横跳）

| 项目 | 原因 |
|------|------|
| Throttle Slew（软件定时器/周期任务两种方案） | 无 ESC/真实油门输出对象 |
| Capability 预留（LED/UART/CAN/ESC 动态注册） | 只有2种设备，抽象无真实约束依据 |
| ACK / Retry / Sequence Number | 当前无 UART/CAN 通信链路 |
| 统一 `execute_action_plan()` 分发框架 | 设备种类不足以驱动抽象需求 |
| ~~Failsafe 分级动作（Hold/Return/Land等价物）~~ | 无飞控/真实执行对象，仅保留思想供讲述 |
| 蜂鸣器旋律模式（开机自检音调序列等） | 当前仅支持告警节奏，未来可扩展为任意 on/off 序列播放 |

等 Fault Injection Framework（电压跌落/过温/传感器丢失/ADC冻结/Predict延迟的
统一注入测试）做完后，如果确实要接入 UART 日志或 CAN 总线，再回来展开这张表，
届时会有两个真实设备可以归纳出真正需要的抽象边界。

---

## 十、设计原则总结

```
✓ 四层压缩 (Event+Context / Policy Table / Dispatcher / Driver Adapter)
✓ Event 超时 + timestamp陈旧度双重判断 (区分"长期SAFE"与"Supervisor失联")
✓ Decision Only Once (Actuator不重新决策，只查表映射)
✓ Driver Adapter 唯一硬件相关层 (未来换UART/CAN不改上层)
✓ 幂等检查 (plan不变则跳过Driver调用)
✓ 软件定时器实现周期动作 (不阻塞事件响应)
✓ FAILSAFE最小实现 (仅报警，不做不存在的执行层动作)
✓ hard_fault + sensor_fault 细分蜂鸣模式 (复用Supervisor V2.2设计)
✓ RGB 三路独立 GPIO + 色彩语义映射 (绿=正常/黄=警告/红=危险/蓝=失联)
✓ 蜂鸣器多维度告警 (音高+节奏双维度，支持分组鸣叫)
✓ Active-low LED 极性收敛在 Driver Adapter 层 (上层不感知硬件细节)
✓ 蜂鸣器节奏状态机 (rt_timer 回调驱动分组 beep 模式)

✗ 六层强行拆分 (无对应用例)
✗ Throttle Slew (无ESC)
✗ Capability预留 (无第二种硬件驱动需求)
✗ ACK/Retry/优先队列 (无通信链路)
✗ 统一Action Plan分发框架 (设备种类不足)
✗ RT_WAITING_FOREVER等待事件 (Supervisor卡死会导致永久沉睡)
✗ 线程内delay实现闪烁 (阻塞实时性)
✗ 蜂鸣器仅靠音高区分告警等级 (人耳对节奏更敏感，音高仅作辅助)
✗ 简单 on/off 枚举表达蜂鸣模式 (无法支持双连鸣等分组节奏)
✗ rt_pin_write 驱动 LED (本项目绑定CubeMX+HAL，直接调HAL_GPIO更高效)
```
