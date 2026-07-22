/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-30     12811       the first version
 */

//综合绝对值与微分值，运行多级滞回状态机，判定系统健康度，并通过事件集发布状态
#include <rtthread.h>
#include <middle/mid_databus.h>
#include <testbench/tb.h>




/* ===== 健康状态 ===== */
#define HEALTH_SAFE     0u
#define HEALTH_WARNING  1u
#define HEALTH_DANGER   2u

/* ===== 比较类型 ===== */
#define CMP_BELOW       0u   /* hi_x10 < 阈值 */
#define CMP_ABOVE       1u   /* hi_x10 > 阈值 */

/* ===== FSM转换规则结构体 ===== */
typedef struct {
    uint8_t  current;       //当前状态
    uint8_t  target;        //目标状态
    int16_t  threshold;     //HI阈值(放大10倍)
    uint16_t hold_frames;   //条件持续帧数(20ms/帧)
    uint8_t  cmp_type;      //比较类型: CMP_BELOW / CMP_ABOVE
} transition_rule_t;

/* ===== FSM规则表 ===== */
static const transition_rule_t g_rules[] = {
    { HEALTH_SAFE,    HEALTH_WARNING, 800, 10, CMP_BELOW }, /* HI<80, 200ms */
    { HEALTH_WARNING, HEALTH_DANGER,  400, 5,  CMP_BELOW }, /* HI<40, 100ms */
    { HEALTH_WARNING, HEALTH_SAFE,    840, 50, CMP_ABOVE }, /* HI>84, 1000ms */
    { HEALTH_DANGER,  HEALTH_WARNING, 450, 25, CMP_ABOVE }, /* HI>45, 500ms */
};
#define RULE_COUNT  (sizeof(g_rules) / sizeof(g_rules[0]))

/* ===== FSM状态变量 ===== */
static rt_thread_t supervisor_thread = RT_NULL;

static uint8_t  health_state  = HEALTH_SAFE;    /* 当前FSM健康状态 */
static uint8_t  hold_target   = HEALTH_SAFE;    /* 当前等待确认的目标状态 */
static uint16_t hold_cnt      = 0;              /* 条件持续帧计数器 */

/* ===== 冷启动标志位 ===== */
static uint8_t  first_run     = 1;

/* ===== 故障锁存 ===== */
static uint8_t  fault_latched = 0;            /* 1=已锁存(需重启清除) */
static fault_cause_t fault_cause;             /* 故障原因(仅记录最先触发) */
/* ===== 边沿事件检测 ===== */
static uint8_t  last_alarm    = ALARM_SAFE;   /* 上次已发布的事件 */

extern rt_mutex_t sensor_mutex;

/**
 * @brief  冷启动首帧定态: 根据HI直接判定初始状态
 * @param  hi_x10  HI值(放大10倍, 范围0~1000)
 * @return HEALTH_SAFE / HEALTH_WARNING / HEALTH_DANGER
 */
static uint8_t evaluate_initial_state(int16_t hi_x10)
{
    if (hi_x10 < 400)  return HEALTH_DANGER;
    if (hi_x10 < 800)  return HEALTH_WARNING;
    return HEALTH_SAFE;
}

/**
 * @brief  Supervisor线程入口函数 (20ms周期, 50Hz)
 */
void supervisor_thread_entry(void *parameter)
{
    monitor_msg_t local;  /* 快照副本, 锁外计算 */

    /* 心跳先赋初值: 在阻塞前更新心跳值，防止actuator误判失联  */
    monitor_msg.supervisor_heartbeat = rt_tick_get();

    /* 等待Predict首次写入完成(500ms超时, 防止Predict卡死) */
    rt_err_t ret = rt_sem_take(predict_ready_sem, 500);
    if (ret != RT_EOK)
    {
        rt_kprintf("[SUPERVISOR] Predict init timeout\n");
        fault_latched = 1;
        fault_cause = FAULT_PREDICT_INIT;
    }

    while (1)
    {
#ifdef USE_PERF
        uint32_t perf_t0 = perf_get_cyc();
#endif
        /* 心跳刷新 */
        monitor_msg.supervisor_heartbeat = rt_tick_get();

        /* ===== 互斥量快照 ===== */
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
        rt_memcpy(&local, &monitor_msg, sizeof(monitor_msg_t));
        rt_mutex_release(sensor_mutex);

        /* ===== 冷启动首帧定态 ===== */
        if (first_run)
        {
            first_run = 0;

            /* 计算首帧HI */
            float risk_sum = local.temp_risk_contrib + local.drop_risk_contrib;
            if (risk_sum < 0.0f)   risk_sum = 0.0f;
            if (risk_sum > 100.0f) risk_sum = 100.0f;
            int16_t hi_x10 = (int16_t)((100.0f - risk_sum) * 10.0f);

            /* 直接定态 */
            health_state = evaluate_initial_state(hi_x10);

            /* 首帧告警融合: 若信号量等待阶段已锁存故障(如Predict初始化超时),
             * 首帧上报值必须是HARDFAULT并同步真实fault_cause,
             * 不能被刚算出来的health_state盲目覆盖成SAFE */
            uint8_t init_alarm = fault_latched ? ALARM_HARDFAULT : health_state;

            /* 同步alarm_level到monitor_msg */
            rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
            monitor_msg.alarm_level = init_alarm;
            monitor_msg.fault_cause = (uint8_t)fault_cause;
            rt_mutex_release(sensor_mutex);

            /* 初始化last_alarm(不发事件, 开机不是状态变化) */
            last_alarm = health_state;

            /* 清空hold计数器 */
            hold_cnt = 0;
            hold_target = health_state;

            /* 20ms周期 */
            rt_thread_mdelay(20);
            continue;
        }

        /* ===== HI计算 ===== */
        float risk_sum = local.temp_risk_contrib + local.drop_risk_contrib;
        if (risk_sum < 0.0f)   risk_sum = 0.0f;
        if (risk_sum > 100.0f) risk_sum = 100.0f;
        int16_t hi_x10 = (int16_t)((100.0f - risk_sum) * 10.0f);

        /* ===== 条件保持 + 表驱动FSM ===== */
        uint8_t fsm_matched = 0;  /* 本轮是否匹配到规则 */

        for (uint8_t i = 0; i < RULE_COUNT; i++)
        {
            const transition_rule_t *rule = &g_rules[i];

            /* 只处理当前状态的规则 */
            if (rule->current != health_state)
                continue;

            /* 检查条件是否满足 */
            uint8_t cond_ok = 0;
            if (rule->cmp_type == CMP_BELOW && hi_x10 < rule->threshold)
                cond_ok = 1;
            else if (rule->cmp_type == CMP_ABOVE && hi_x10 > rule->threshold)
                cond_ok = 1;

            if (cond_ok)
            {
                fsm_matched = 1;

                /* 条件持续中 */
                if (hold_target == rule->target)
                {
                    hold_cnt++;
                }
                else
                {
                    /* 新目标出现, 重置计数 */
                    hold_target = rule->target;
                    hold_cnt = 1;
                }

                /* 达到持续帧数, 执行转移 */
                if (hold_cnt >= rule->hold_frames)
                {
                    health_state = rule->target;
                    hold_cnt = 0;
                    hold_target = rule->target;
                }
                break;  /* 找到匹配规则, 退出循环 */
            }
        }

        /* 条件不满足时, 重置hold */
        if (!fsm_matched)
        {
            hold_cnt = 0;
            hold_target = health_state;
        }

        
        /* ===== 故障锁存 ===== */
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

        /* ===== 告警融合 ===== */
        uint8_t final_alarm;
        if (fault_latched)
            final_alarm = ALARM_HARDFAULT;
        else
            final_alarm = health_state;

        /* 同步alarm_level到monitor_msg */
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
        monitor_msg.alarm_level = final_alarm;
        monitor_msg.fault_cause = (uint8_t)fault_cause;
        rt_mutex_release(sensor_mutex);


        /* ===== 边沿事件发布 ===== */
        if (final_alarm != last_alarm)
        {
            rt_uint32_t evt_bit;

            switch (final_alarm)
            {
                case ALARM_SAFE:      evt_bit = EVT_SAFE;      break;
                case ALARM_WARNING:   evt_bit = EVT_WARNING;   break;
                case ALARM_DANGER:    evt_bit = EVT_DANGER;    break;
                case ALARM_HARDFAULT: evt_bit = EVT_HARDFAULT; break;
                default:              evt_bit = EVT_SAFE;      break;
            }

#ifdef USE_PERF
            perf_mark_event_send();
#endif
            rt_event_send(&adc_event, evt_bit);

            last_alarm = final_alarm;
        }

                /* ===== 看门狗预留 ===== */

#ifdef USE_PERF
        perf_update_stat(perf_get_stat(PERF_SUPERVISOR),
                         perf_diff_us(perf_t0, perf_get_cyc()));
#endif

        /* 20ms周期 */
        rt_thread_mdelay(20);
    }
}

int app_supervisor_init(void)
{
    supervisor_thread = rt_thread_create("supervisor",
                                    supervisor_thread_entry,
                                    RT_NULL,
                                    1024,
                                    10,
                                    20);
    if (supervisor_thread != RT_NULL)
    {
        rt_thread_startup(supervisor_thread);
    }
    else
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_APP_EXPORT(app_supervisor_init);
