# 项目进展报告

**最后更新**: 2026-07-13  
**当前状态**: Phase 4 编码完成，**联调阶段 — CRITICAL/HIGH/MEDIUM bug 全部修复**

---

## 项目概述

基于 STM32F103 + RT-Thread Nano 的瞬态故障预测系统（PHM Pipeline），实现电池电压/温度信号的采集、特征提取、健康评估与告警执行。

---

## 阶段划分与完成状态

### Phase 1: 基础设施层 ✅ 已完成

**目标**: 搭建硬件驱动、IPC 通信、数据采集框架

| 模块 | 状态 | 说明 |
|------|------|------|
| DMA ADC 双缓冲采集 | ✅ 完成 | bsp_adc.c，12.658kHz 采样，64 点/批 |
| 传感器滤波+物理转换 | ✅ 完成（冻结） | mid_filter.c，去极值+查表插值，ADC 方向已反转 |
| IPC 基础设施 | ✅ 完成 | mid_databus.c，MQ/Event/Semaphore/Mutex |
| 显示线程 | ✅ 完成 | app_display.c，100ms 周期串口打印，手动 int/dec 拆分 |

---

### Phase 2: 特征提取层 ✅ 已完成

**目标**: IIR 滤波、微分计算、双时间尺度模型、风险贡献提取

| 模块 | 状态 | 说明 |
|------|------|------|
| app_predict.c | ✅ 完成 | 7 阶段管线：冷启动 → 自检 → Median3 → 温度链 → 电压链 → 油门迟滞 → 写回+timestamp+predict_ready_sem |
| app_predict.h | ✅ 完成 | `predict_param_t` + `P` 全局 const 实例（17 个参数） |
| bsp_throttle | ✅ 完成（占位） | `throttle_read()` 返回 1000，未来接 SBUS |

---

### Phase 3: 健康评估层 ✅ 已完成

**目标**: 多级滞回状态机，判定系统健康度

| 模块 | 状态 | 说明 |
|------|------|------|
| app_supervisor.c | ✅ 完成 | 9 层管线：快照 → 冷启动 → HI 计算 → 条件保持 → 表驱动 FSM → 故障锁存 → 告警融合 → 边沿事件 → watchdog 预留 |
| 心跳机制 | ✅ 完成 | `monitor_msg.supervisor_heartbeat` 在主循环开头刷新 |

**⚠️ 致命部署问题**: Supervisor 主循环逻辑正确，但 **`INIT_APP_EXPORT` 未写**，导致线程从不上报 RT-Thread 启动队列，**线程永不启动**（见下方 CRITICAL 问题清单）。

---

### Phase 4: 执行控制层 🟡 编码完成, 存在多个 bug

**目标**: 事件驱动蜂鸣器告警 + LED 状态指示

| 模块 | 状态 | 说明 |
|------|------|------|
| app_actuator.c | 🟡 编码完成 | 四层架构实现：L1 事件+上下文 → L2 查表 → L3 幂等分发 → L4 Driver Adapter |
| bsp_beep.h/c | 🟡 部分修复 | `calc_pwm_param` 已加 return 防除零、`pwm_set` 声明待补 |
| 定时器回调 | ✅ 已完成 | LED PERIODIC 翻转 + BEEP ONE_SHOT 状态机（支持分组鸣叫） |

---

### Phase 5: 系统集成与测试 🔴 当前卡在这里

**目标**: 全链路联调、故障注入测试、参数整定

| 任务 | 状态 | 说明 |
|------|------|------|
| 全链路联调 | 🔴 阻塞 | 有 CRITICAL bug，必须先修复（见下方清单） |
| 故障注入测试 | ⏳ 待进行 | 计划：NTC 断开、Pot 极端值、Predict 挂起、Supervisor 挂起 |
| 参数整定 | ⏳ 待进行 | K_TEMP、K_DROP、FSM 阈值、Supervisor 心跳/超时窗口调参 |

---

## 🔥 活跃 Bug 清单（按严重度排序）

### ✅ CRITICAL-01: Supervisor 线程未启动 — 已修复

**位置**: `modules/app/app_supervisor.c` 末尾  
**现象**: 烧录后 ALM=0, FC=0, 蜂鸣器 500ms 后开始 4kHz 持续长鸣  
**根因**: `app_supervisor_init()` 函数定义完成后，**没有写 `INIT_APP_EXPORT(app_supervisor_init);`**，导致 RT-Thread 启动队列里根本没有 supervisor 线程  
**修复**: 已在 `app_supervisor.c` 文件末尾追加 `INIT_APP_EXPORT(app_supervisor_init);`  
**状态**: ✅ 已修复 (2026-07-13)

---

### ✅ CRITICAL-02: Actuator Beep 分支 2 条件写反 — 已修复

**位置**: `modules/app/app_actuator.c` → `dispatch_plan()` 函数  
**现象**: 叠加在 CRITICAL-01 上，让蜂鸣器的长鸣分支完全失效  
**根因**: 分支 2 的判断条件 `else if (current_plan.beep.on_ms)` 写反了  
**修复**: 已改为 `else if (current_plan.beep.on_ms == 0)`  
**状态**: ✅ 已修复 (2026-07-13)

---

### ✅ HIGH-01: Actuator 线程栈大小和优先级未使用宏 — 已修复

**位置**: `modules/app/app_actuator.c` → `app_actuator_init()`  
**现象**: `rt_thread_create` 直接写死 `512 / 10`，而不是 `ACTUATOR_STACK_SIZE(1536) / ACTUATOR_PRIORITY(11)`  
**影响**:
- 栈 512 字节偏紧（定时器回调 + 浮点 memcpy + struct 赋值），有栈溢出风险
- 优先级 10 与 Supervisor 相同，失去"被抢占"的实时性保证，与架构文档不符

**修复**: 把 `512, 10` 改成 `ACTUATOR_STACK_SIZE, ACTUATOR_PRIORITY`
**状态**: ✅ 已修复 (2026-07-14)

---

### ✅ MEDIUM-01: Actuator while(1) 末尾多余 rt_thread_mdelay(100) — 已修复

**位置**: `modules/app/app_actuator.c` → `actuator_thread_entry()` 末尾  
**现象**: `rt_event_recv` 已经按 500ms 超时阻塞，再加 `rt_thread_mdelay(100)` 导致响应延迟最大 600ms  
**影响**: 不符合架构设计中"500ms 心跳检测窗口"的预期，告警响应被无谓推迟  
**修复**: 删除 `rt_thread_mdelay(100);` 这一行
**状态**: ✅ 已修复 (2026-07-14)

---

### ✅ MEDIUM-02: bsp_beep.h 暴露了 static 函数声明 — 已修复

**位置**: `modules/drivers/bsp_beep.h` 第 15 行  
**现象**: `static void calc_pwm_param(...)` 出现在头文件  
**影响**:
- 每个包含此头文件的 .c 都会得到警告或"未使用 function"提示
- 同时 `pwm_set()` 未在头文件声明，`app_actuator.c` 调用会触发"implicit declaration"

**修复**:
- 删除头文件中的 `static void calc_pwm_param(...);` 一行
- 新增 `void pwm_set(uint32_t fre, uint32_t duty);` 声明

---

### 🟢 LOW-01: mid_databus.c 初始化器注释错位

**位置**: `modules/middle/mid_databus.c` 第 12~20 行  
**现象**: 初始化器按位置赋值，但注释对不上实际字段顺序  
**影响**: 不影响运行时行为，但读代码时极易误导  
**建议修复**: 改成 designated initializer 命名初始化，消除对位置顺序的依赖

---

## 📂 文档状态与问题清单

### predict_coding_guide.md

**问题**:
- 第 59~71 行对 `monitor_msg_t` 的描述是**旧版 11 字段**（含 `uint32_t timestamp` 和 `uint8_t reserved`），与当前代码（`rt_tick_t timestamp` + `rt_tick_t supervisor_heartbeat` + `uint8_t fault_cause`）**不一致**
- 第 116~117 行说 supervisor/actuator "🔴 待实现"，**已过时**（两者都已编码完成）
- 第 237~256 行参数表中未记录 `predict_ready_sem` 的 500ms 超时参数

**优先级**: 中（不阻塞编译，但容易让新人或 AI 助手理解错结构体字段）

---

### supervisor_coding_guide.md

**问题**:
- 第 93~105 行对 `monitor_msg_t` 的描述是旧版（缺 `fault_cause` 字段、缺 `supervisor_heartbeat`），与代码不一致
- 第 17~22 行"已完成"表格未反映 V2.2 的 fault_cause 升级（代码中已实现，文档列为"🔴 待实现"）
- 第 21 行"看门狗预留"在代码中对应 line 256 的注释，这部分一致
- 文档末尾没提 `INIT_APP_EXPORT` 必须补，正是 CRITICAL-01 的根因

**优先级**: 高（结构体描述错误会直接误导后续开发）

---

### actuator_coding_guide.md

**问题**:
- 第 139~165 行对数据结构定义的描述是**完全过时**的：
  - 用了 `rt_bool_t` / `rt_uint32_t` / `rt_uint16_t`，实际代码用 `uint8_t` / `uint32_t` / `uint16_t`
  - 用了 6 层架构（Policy/Plan 分离），实际代码压缩为 4 层
  - 用 `rt_timer_t` 指针（动态 create），实际代码用 `static struct rt_timer`（静态 init）
- 第 188~261 行的 `resolve_policy` 示例接收 `hard_fault + sensor_fault` 参数，实际代码压缩成只接收 `(alarm_level, supervisor_lost)`
- 第 287~291 行使用 `RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR` 一致
- 第 325 行的"要点说明"里对 `timestamp` 陈旧度的描述**已过时**：实际用 `supervisor_heartbeat` 而非 `timestamp` 判断 Supervisor 存活
- 全文大量引用 `DRV-ACT-01` 等 ADR 编号，但项目 ADR 文档不存在

**优先级**: 高（架构差异极大，新开发者照着文档写会与代码完全对不上）

---

### supervisor_architecture_evolution.md

**问题**:
- 第 104 行 `timestamp` 字段描述与当前代码（拆成 `timestamp` + `supervisor_heartbeat` 两字段）不一致
- 第 187~213 行"决策九：边沿事件发布"一致
- 第 243~267 行"决策十一：看门狗"描述了线程心跳机制，但实际代码只实现了 supervisor 自己的心跳，其他线程 (acquire/predict/actuator) 未实现

**优先级**: 低（这是归档分析文档，不直接指导编码）

---

### project_progress.md（本文件）

**本文件刚刚重写**，之前版本停留在 Phase 2 阶段，对实际进度描述严重过时。

---

## 📌 关键参数速查

| 参数 | 值 | 含义 |
|------|-----|------|
| 采样周期 | 5ms | 64 点 / 12.658kHz |
| 温度风险阈值 | 1.2 ℃/s | 超过开始累积风险 |
| 压降风险阈值 | 8% | 超过开始累积风险 |
| 单项风险上限 | 50 | temp_risk_contrib / drop_risk_contrib 各自上限 |
| 风险融合 | risk_sum = temp + drop | [0, 100] 限幅 |
| FSM 滞回系数 | 进入/退出 = 80/84 (HI×10) | SAFE↔WARNING |
| 预测线程栈 | 1536B | 含 float 栈帧 |
| Supervisor 周期 | 20ms (50Hz) | 主循环末尾 rt_thread_mdelay |
| Actuator 栈 | 1536B (应) | HIGH-01 bug 当前为 512B |
| Actuator 优先级 | 11 (应) | HIGH-01 bug 当前为 10 |
| Actuator 事件超时 | 500ms | `rt_event_recv` 带超时 |
| Supervisor 心跳超时 | 500ms | Actuator 判定失联的阈值 |
| Predict 写回超时 | 200ms | Supervisor 判定 Predict 卡死的阈值 |
| Predict 启动超时 | 500ms | 信号量 predict_ready_sem 超时 |

---

## 下一步行动清单

### 立即执行（修复联调 bug）
- [x] ~~**修复 CRITICAL-01**: 在 `app_supervisor.c` 末尾添加 `INIT_APP_EXPORT(app_supervisor_init);`~~ ✅ 已完成
- [x] ~~**修复 CRITICAL-02**: `app_actuator.c` `dispatch_plan` 蜂鸣器分支 2 条件改为 `on_ms == 0`~~ ✅ 已完成
- [x] ~~**修复 HIGH-01**: \pp_actuator.c\ 创建线程参数改用宏~~ ✅ 已完成
- [x] ~~修复 MEDIUM-01: 删除 \ctuator_thread_entry\ 末尾的 t_thread_mdelay(100);\~~ ✅ 已完成
- [x] ~~修复 MEDIUM-02: \sp_beep.h\ 调整声明~~ ✅ 已完成
- [ ] 烧录验证：ALM/FC 数值正常、SAFE 时绿常亮静音、WARNING/DANGER/HARDFAULT/Supervisor Lost 各分支可切换

### 短期（文档同步）
- [ ] 更新 `predict_coding_guide.md` 的 `monitor_msg_t` 描述
- [ ] 更新 `supervisor_coding_guide.md` 标记 fault_cause 已完成 + 补 INIT_APP_EXPORT 提示
- [ ] 重写 `actuator_coding_guide.md` 反映实际 4 层架构 + uint8_t 字段 + 静态定时器

### 中期（验证与整定）
- [ ] 故障注入测试：模拟 NTC 断开、电位器极端值、Predict 挂起、Supervisor 挂起
- [ ] 参数整定：K_TEMP、K_DROP、FSM 阈值、心跳/超时窗口
- [ ] 长时间稳定性测试（>24 小时连续运行）

### 长期
- [ ] Layer9 看门狗实现（IWDG + 各线程心跳）
- [ ] 油门解码替换 bsp_throttle 占位（接 SBUS/PWM 输入捕获）
- [ ] fault_cause 细分响应（过温限油门、欠压强降落等差异化动作）
