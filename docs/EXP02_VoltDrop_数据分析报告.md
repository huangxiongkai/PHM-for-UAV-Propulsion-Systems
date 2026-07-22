# EXP-02 快速掉压故障注入 测试报告

**测试日期**: 2026-07-17
**测试配置**: `TB_FAULT_VOLT_FAST_DROP`，故障注入起始 5.0s
**测试时长**: ~15.3 秒

---

## 1. Executive Summary

**结论**：EXP-02 快速掉压测试**通过**，Detection Latency=960ms，Total Response=1.6s，Protection Time=145µs。

| 指标 | 预期 | 实测 | 判定 |
|------|------|------|------|
| Detection Latency | < 1s | 960ms | ✅ |
| Early Warning | > 0 | 649ms | ✅ |
| Total Response | < 2s | 1.6s | ✅ |
| Protection Time | < 1ms | 145µs | ✅ |

**遗留问题**：
- DropRatio 冷启动 > 1.0（建议 clamp）
- FSM 恢复滞回死区（需恢复场景测试）

---

## 2. 测试概述

**测试目标**：验证 PHM 管线在电压快速下降（-5V/s）场景下的检测、决策与执行链路。

**测试范围**：
- 故障类型：快速掉压（`TB_FAULT_VOLT_FAST_DROP`）
- 注入起始时间：5.0s
- 验证重点：Detection Latency、Early Warning、Protection Response Time

---

## 3. 检测方法

**测试架构**：基于内部 Testbench 故障注入框架，通过编译时宏 `TB_FAULT_VOLT_FAST_DROP` 启用。

**数据采集**：系统内部 CSV 日志输出，通过串口抓取，每帧包含 18 个字段（Time/Phase/Volt/Temp/.../AcqUs/PredUs/SupUs/ActUs/EvtUs）。

**故障注入**：在 Acquire 线程数据流路径中插入劫持点 `tb_read_volt_raw()`，将真实 ADC 码值替换为以 -5V/s 速率递减的仿真输出。

**计时机制**：DWT Cycle Counter（72MHz），精度 1µs，每秒输出 `[PERF]` 统计。

---

## 4. 参数说明

| 缩写 | 全称 | 含义 |
|------|------|------|
| ALM | Alarm Level | 0=SAFE, 1=WARNING, 2=DANGER, 3=HARDFAULT |
| HardFlt | Hard Fault Bitmap | bit0=过温, bit1=欠压 |
| SenFlt | Sensor Fault | 0=正常, 1=NTC故障, 2=ADC卡死, 3=V传感器开短路 |
| FCause | Fault Cause | 1=PREDICT_INIT, 2=PREDICT_LOST, 3=SENSOR, 4=OVERTEMP, 5=UNDERVOLT |
| DropR | Drop Ratio | 归一化压降比 = (V_ref - V_fast) / V_ref |
| VRisk | Voltage Risk | 电压风险贡献（0-50） |
| HI | Health Index | 综合健康指数 = 100 - TRisk - VRisk |
| ActUs | Actuator Exec Time | 执行器执行时间（µs） |

---

## 5. 测试结果

### 5.1 状态转移验证

#### 关键转折点

| 时间 | 事件 | ALM | Volt(V) | VRisk | HardFlt | FCause |
|------|------|-----|---------|-------|---------|--------|
| 5.0s | 故障注入 | 0 | 23.7 | 0 | 0 | 0 |
| 5.53s | VRisk 首次 > 0 | 0 | 21.1 | 5.4 | 0 | 0 |
| **5.96s** | **WARNING 触发** | **1** | **18.9** | **37.7** | **0** | **0** |
| 6.61s | HardFlt=2 (V<16V) | 1 | 15.7 | 50 | 2 | 0 |
| **6.61s** | **HARDFAULT 触发** | **3** | **15.7** | **50** | **2** | **5** |
| 6.82s | SenFlt=3 出现 | 3 | 15.0 | 50 | 2 | 5 |

#### 转移链路图

```
t=5.0s   故障注入（电压 -5V/s）
         │
t=5.53s  VRisk > 0（压降比超过 8% 阈值）
         │  +430ms
t=5.96s  WARNING（HI<80 持续 200ms）
         │  +650ms
t=6.61s  HARDFAULT（V<16V → Fault Latch 锁存）
```

#### 预期 vs 实测

| 验证项 | 预期现象 | 实测数据 | 判定 |
|--------|----------|----------|------|
| 电压下降速率 | -5V/s | ~5V/s（23.7V → 15V / 1.6s） | ✅ 符合 |
| WARNING 触发条件 | HI < 80 持续 200ms | t=5.96s, HI≈62, 持续 200ms | ✅ 符合 |
| HARDFAULT 触发 | V < 16V → Fault Latch | t=6.61s, V=15.7V, FCause=5 | ✅ 符合 |
| DANGER 阶段 | 可能出现 | 跳过（Fault Latch 优先级更高） | ✅ 符合设计 |
| Fault Latch 锁存 | 首次故障不覆盖 | FCause=5 持续至结束 | ✅ 符合 |

### 5.2 Protection Response Time

| 阶段 | 定义 | 预期上限 | 实测值 | 判定 |
|------|------|---------|--------|------|
| Detection Time | 故障发生 → WARNING | < 1s | **960ms** | ✅ |
| Decision Time | WARNING → HARDFAULT | — | **649ms** | — |
| Protection Time | Event 收到 → LED/Beep 执行 | < 1ms | **~145µs** | ✅ |
| **Total** | **故障注入 → HARDFAULT 稳定** | < 2s | **~1.6s** | ✅ |

**Detection Time 分解**（960ms）：

| 阶段 | 耗时 | 说明 |
|------|------|------|
| IIR 滤波器收敛 | ~530ms | 电压从 23.7V 降至 ~19V |
| VRisk 累积 | ~430ms | 从 0 升至 37.7（HI 从 100 降至 ~62） |
| FSM 条件保持 | 200ms | HI < 80 需持续 10 帧 |

### 5.3 实时性能

| 线程 | 预期上限 | 实测平均 | 实测最大 | 判定 |
|------|---------|---------|---------|------|
| Acquire | < 300µs | 228µs | 291µs | ✅ |
| Predict | < 200µs | 92µs | 99µs | ✅ |
| Supervisor | < 100µs | 37µs | 53µs | ✅ |
| Actuator（事件处理） | < 200µs | 145µs | 149µs | ✅ |

### 5.4 堆栈安全

| 线程 | 分配 (B) | 峰值 (B) | 使用率 | 判定 |
|------|---------|---------|--------|------|
| rawdata_thread1 | 1024 | 508 | 49% | ✅ < 50% |
| Logic_thread3 | 1536 | 388 | 25% | ✅ < 50% |
| supervisor | 1024 | 260 | 25% | ✅ < 50% |
| actuator | 1536 | 420 | 27% | ✅ < 50% |
| log_show | 1536 | 636 | 41% | ✅ < 50% |

---

## 6. 数据质量

### 电压曲线

```
t=0.0s   23.9V（正常）
t=5.0s   23.7V（故障注入前）
t=5.96s  18.9V（WARNING 触发）
t=6.61s  15.7V（HARDFAULT 触发）
t=6.82s  15.0V（钳位到下限，持续至结束）
```

**预期**：电压以 -5V/s 下降。**实测**：23.7V → 15V / 1.6s ≈ -5.4V/s。✅ 符合。

### DropRatio 冷启动异常

冷启动阶段（t < 3s）出现 DropRatio > 1.0 的值，原因是 IIR 滤波器收敛过程中 `v_ref`（慢尺度）和 `v_fast`（快尺度）收敛速度不同。

**影响**：不影响告警判定（VRisk=0 时 DropRatio 无意义），但应在 Predict 代码中 clamp 到 [0,1]。

### 数据稳定性

**预期**：故障注入前数据稳定。**实测**：电压波动 < 0.1V，温度波动 < 0.6℃。✅ 符合。

---

## 7. Issues & Risks

| # | 问题 | 严重度 | 影响 | 建议 |
|---|------|--------|------|------|
| 1 | DropRatio 冷启动 > 1.0 | 低 | 不影响告警判定（VRisk=0 时无意义） | Predict 代码 clamp 到 [0,1] |
| 2 | FSM 恢复滞回死区未验证 | 中 | 无法确认 WARNING → SAFE 恢复行为 | 补充恢复场景测试 |

---

## 8. 结论

| 验证项 | 预期 | 实测 | 判定 |
|--------|------|------|------|
| 状态转移链 | SAFE → WARNING → HARDFAULT | 完整链路验证通过 | ✅ |
| Detection Latency | < 1s | 960ms | ✅ |
| Early Warning 提前量 | > 0（PHM 价值体现） | 649ms | ✅ |
| Total Protection Response | < 2s | 1.6s | ✅ |
| Protection Time | < 1ms | 145µs | ✅ |
| Fault Latch 锁存 | 不覆盖 | FCause=5 持续至结束 | ✅ |
| 堆栈安全 | < 50% | 最高 49% | ✅ |

---

*报告版本：v4.0 | 2026-07-17*
