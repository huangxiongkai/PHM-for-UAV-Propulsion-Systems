/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-28     12811       the first version
 */
#ifndef MODULES_MIDDLE_MID_DATABUS_H_
#define MODULES_MIDDLE_MID_DATABUS_H_

#include <rtthread.h>
#include <stdio.h>
#include <stdint.h>

/* ---------- DMA 传输完成事件（供 acquire 线程使用） ---------- */
#define EVENT_ADC_HALF  (1 << 0)
#define EVENT_ADC_FULL  (1 << 1)

/* ---------- 告警事件位（supervisor → actuator） ---------- */
#define EVT_SAFE        (1 << 0)
#define EVT_WARNING     (1 << 1)
#define EVT_DANGER      (1 << 2)
#define EVT_HARDFAULT   (1 << 3)

/* ---------- 告警等级（uint8_t） ---------- */
#define ALARM_SAFE      0u
#define ALARM_WARNING   1u
#define ALARM_DANGER    2u
#define ALARM_HARDFAULT 3u

/* ---------- 故障原因枚举 ---------- */
typedef enum {
    FAULT_NONE          = 0,  /* 无故障 */
    FAULT_PREDICT_INIT  = 1,  /* Predict 启动超时（sem 500ms） */
    FAULT_PREDICT_LOST  = 2,  /* Predict 运行卡死（timestamp 200ms） */
    FAULT_SENSOR        = 3,  /* 传感器故障（sensor_fault≠0） */
    FAULT_OVERTEMP      = 4,  /* 过温（hard_fault bit0） */
    FAULT_UNDERVOLT     = 5,  /* 欠压（hard_fault bit1） */
} fault_cause_t;

/* ---------- Predict 超时检测 ---------- */
#define PREDICT_TIMEOUT_MS  200
#define PREDICT_TIMEOUT_TICKS \
    ((RT_TICK_PER_SECOND * PREDICT_TIMEOUT_MS) / 1000)

/* ---------- 原始 ADC 数据 ---------- */
typedef struct {
    float voltage;
    float temp_voltage;
} raw_data_t;

/* ---------- 监控消息结构体 ---------- */
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

extern monitor_msg_t monitor_msg;
extern struct rt_messagequeue monitor_mq;
extern rt_sem_t alarm_sem;
extern rt_sem_t predict_ready_sem;
extern struct rt_event adc_event;

int sys_ipc_init(void);

#endif /* MODULES_MIDDLE_MID_DATABUS_H_ */
