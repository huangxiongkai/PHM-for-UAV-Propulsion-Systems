/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-07-15     12811       the first version
 */
#ifndef MODULES_TESTBENCH_TB_H_
#define MODULES_TESTBENCH_TB_H_

#include <rtthread.h>
#include <middle/mid_databus.h>

/* ---- 故障测试 开关 ---- */
#define USE_TESTBENCH   /* 注释此行即可关闭testbench，Release 零开销 */

/* ---- 数据测试 开关 ---- */
/* SAFE: USE_PERF 由 bsp_perf.h 统一管理；此处通过包含传递，确保 tb_logger.c 等模块可见 */


#include <drivers/bsp_perf.h>

/* ---- 飞行阶段枚举 ---- */
typedef enum {
    PHASE_IDLE    = 0,
    PHASE_TAKEOFF = 1,
    PHASE_HOVER   = 2,
    PHASE_CRUISE  = 3,
    PHASE_LAND    = 4
} TB_Phase;

/* ---- 采集数据枚举 ---- */
typedef struct {
    float      voltage;      /* 真实电压 V */
    float      temperature;  /* 真实温度 ℃ */
    uint16_t   throttle;     /* 真实油门 */
    TB_Phase   phase;        /* 当前飞行阶段 */
} TB_PhysicsState;

/* ---- 故障类型枚举（TB_FAULT_ 前缀避免与 mid_databus.h fault_cause_t 冲突） ---- */
typedef enum {
    TB_FAULT_NONE = 0,
    TB_FAULT_VOLT_FAST_DROP,   /* 快速掉压 5V/s */
    TB_FAULT_TEMP_FAST_RISE,   /* 快速升温 3℃/s */
    TB_FAULT_ADC_STUCK,        /* ADC 卡死（返回固定码值） */
    TB_FAULT_NTC_OPEN,         /* NTC 开路（返回极端码值） */
    TB_FAULT_VOLT_UNDERVOLT,   /* 直接强制欠压 */
    TB_FAULT_TEMP_OVERTEMP,    /* 直接强制过温 */
    TB_FAULT_COUNT             /* 哨兵：故障类型总数 */
} TB_FaultType;

/* ---- 实验元数据 ---- */
typedef struct {
    uint8_t      experiment_id;
    uint8_t      scenario_id;
    TB_FaultType fault;
    float        fault_start_s;
} TB_ExperimentInfo;

/* ---- 实验配置宏（编译时选择） ---- */
#define TB_EXP_ID           1u
#define TB_SCN_ID           1u
#define TB_EXPERIMENT_FAULT TB_FAULT_ADC_STUCK      /* 故障事件 -  关闭故障注入时也要调整事件 */
#define TB_FAULT_START_S    5.0f                    /* 故障开始时间 */

/* ---- 故障类型名称表（tb_core.c 中定义，其他文件 extern 引用） ---- */
extern const char *tb_fault_name[];

/* ---- 对外接口 ---- */
int      tb_init(void);                    /* INIT_APP_EXPORT 自动调用（需返回 int） */
uint8_t  tb_is_active(void);              /* 供 display 层判断 */
void     tb_tick(void);                    /* 无参，内部用 rt_tick_get() */
float    tb_get_time(void);               /* 获取实验经过时间(秒) */
uint16_t tb_read_volt_raw(uint16_t real); /* 劫持电压码值 */
uint16_t tb_read_temp_raw(uint16_t real);  /* 劫持温度码值 */
void     tb_scenario_get_state(float t, TB_PhysicsState *s);
void     tb_fault_apply(float t, float start_t, TB_FaultType f, TB_PhysicsState *s);
void     tb_log_csv(monitor_msg_t *msg);
const TB_ExperimentInfo* tb_get_experiment_info(void);

#endif /* MODULES_TESTBENCH_TB_H_ */
