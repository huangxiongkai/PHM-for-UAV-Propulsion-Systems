/*
 * Copyright (c) 2024, UAV Tech
 *
 * Infrastructure Definition for UAV-HMS
 * Version: 3.0 (Professional)
 */

#ifndef APPLICATIONS_INFRASTRUCTURE_SYS_DEF_H_
#define APPLICATIONS_INFRASTRUCTURE_SYS_DEF_H_

#include <rtthread.h>

/* 1. 系统阈值定义 */
#define TEMP_THRESHOLD_HIGH     50.0f   // 摄氏度
#define VOLT_THRESHOLD_LOW      1.0f    // 1.0V 对应危险电压
#define ADC_CH_MAX              2       // 频道数量：电位器 + 热敏

/* 2. 报警等级枚举 */
typedef enum {
    ALARM_NONE = 0,
    ALARM_LOW_POWER,    // 低电量
    ALARM_OVER_HEAT,    // 过热
    ALARM_CRITICAL      // 紧急（两项都触发）
} alarm_level_t;

/* 3. 系统健康状态快照 (核心数据结构) */
typedef struct {
    float battery_voltage;      // 动力电池电量 (V)
    float motor_temperature;    // 电机温度 (C)
    uint32_t timestamp;         // 采样时间戳
    alarm_level_t alarm_lv;     // 当前系统警报等级
} uav_status_t;

/* 4. IPC 资源句柄声明 (由 main.c 或 sys_status.c 初始化) */
extern struct rt_messagequeue mq_uav_status;
extern struct rt_semaphore    sem_alarm_trigger;

#endif /* APPLICATIONS_INFRASTRUCTURE_SYS_DEF_H_ */
