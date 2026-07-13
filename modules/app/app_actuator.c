/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-30     12811       the first version
 */

//阻塞等待状态机发送的事件 ， 执行对应操作

#include <rtthread.h>
#include <middle/mid_databus.h>
#include <drivers/bsp_beep.h>
#include "main.h"


extern rt_mutex_t sensor_mutex;

#if 0
数据结构定义 (Step 1 完成)
   ↓
POLICY_* 宏 + g_policy_table[] (Step 2 完成)
   ↓
resolve_policy()      ← Step 3 在这里
   ↓
drv_led_write() / drv_beep_freq()  ← Step 4
   ↓
定时器回调            ← Step 5
   ↓
dispatch_plan()       ← Step 6
   ↓
actuator_thread_entry()  ← Step 7
   ↓
app_actuator_init() + INIT_APP_EXPORT  ← Step 8
#endif

/* ===== 数据结构定义 ===== */
typedef struct {
    uint8_t  r_on;
    uint8_t  g_on;
    uint8_t  b_on;
    uint8_t  blink;
    uint16_t blink_period_ms;
} led_mode_t;

typedef struct {
    uint32_t pwm_freq;        //频率设置
    uint16_t on_ms;           //单次鸣叫持续时间
    uint16_t inter_beep_ms;   //组内鸣叫间隔
    uint16_t group_gap_ms;    //组间鸣叫间隔静默时长
    uint8_t  beats_per_group; //每组鸣叫次数
    uint8_t  is_mute;         //静音开关  1= 静音, 0 = 非静音
} beep_mode_t;

typedef struct {
    led_mode_t led;
    beep_mode_t beep;
} action_plan_t;

/* 线程配置宏*/
#define ACTUATOR_PRIORITY 11
#define ACTUATOR_STACK_SIZE 1536
/* 超时配置宏*/
#define SUPERVISOR_TIMEOUT_MS 500u
#define SUPERVISOR_TIMEOUT_TICKS \
    ((rt_tick_t)((RT_TICK_PER_SECOND * SUPERVISOR_TIMEOUT_MS) / 1000))


/* ===== timer状态变量 ===== */    
static uint8_t led_phase   = 0;   /* LED 闪烁相位: 1=亮, 0=灭 */
static uint8_t beep_phase  = 0;   /* 蜂鸣器相位: 0=鸣响中, 1=组内间隔, 2=组间间隔 */
static uint8_t beep_count  = 0;   /* 当前组内已完成的鸣响次数 */

static rt_thread_t   actuator_thread = RT_NULL;
/* ========== 策略计划变量 ========== */
static action_plan_t last_plan;
static action_plan_t current_plan;

/* ========== timer结构体 ========== */
static struct rt_timer led_timer;
static struct rt_timer beep_timer;


/* ========== 策略表索引 ========== */
#define POLICY_SAFE             0
#define POLICY_WARNING          1
#define POLICY_DANGER           2
#define POLICY_HARDFAULT        3
#define POLICY_SUPERVISOR_LOST  4
/* ===== 静态只读策略表 ===== */
static const action_plan_t g_policy_table[5] = 
{
    [POLICY_SAFE] = {
        .led  = { .r_on = 0, .g_on = 1, .b_on = 0, .blink = 0, .blink_period_ms = 0   },
        .beep = { .pwm_freq = 0,    .on_ms = 0,   .inter_beep_ms = 0,
                  .group_gap_ms = 0, .beats_per_group = 0, .is_mute = 1 }
    },
    [POLICY_WARNING] = {
        .led  = { .r_on = 1, .g_on = 1, .b_on = 0, .blink = 1, .blink_period_ms = 500 },
        .beep = { .pwm_freq = 3000, .on_ms = 100, .inter_beep_ms = 0,
                  .group_gap_ms = 2000, .beats_per_group = 1,   .is_mute = 0 }
    },
    [POLICY_DANGER] = {
        .led  = { .r_on = 1, .g_on = 0, .b_on = 0, .blink = 1, .blink_period_ms = 100 },
        .beep = { .pwm_freq = 3000, .on_ms = 100, .inter_beep_ms = 100,
                  .group_gap_ms = 1000, .beats_per_group = 2,   .is_mute = 0 }
    },
    [POLICY_HARDFAULT] = {
        .led  = { .r_on = 1, .g_on = 0, .b_on = 0, .blink = 1, .blink_period_ms = 100 },
        .beep = { .pwm_freq = 4000, .on_ms = 50,  .inter_beep_ms = 50,
                  .group_gap_ms = 0,  .beats_per_group = 0, .is_mute = 0 }
    },
    [POLICY_SUPERVISOR_LOST] = {
        .led  = { .r_on = 0, .g_on = 0, .b_on = 1, .blink = 0, .blink_period_ms = 0   },
        .beep = { .pwm_freq = 4000, .on_ms = 0,   .inter_beep_ms = 0,
                  .group_gap_ms = 0,  .beats_per_group = 0, .is_mute = 0 }
    },
};

/* ===== L2: Policy Table 查表函数 ===== */
/**
 * @brief  根据告警上下文返回对应的执行计划
 * @param  alarm_level      ALARM_SAFE / WARNING / DANGER / HARDFAULT
 * @param  supervisor_lost  1=Supervisor心跳超时, 0=正常
 * @return action_plan_t 指针 (const, 调用方只读)
 * @note   supervisor_lost 优先级最高 (覆盖 alarm_level)
 */
static const action_plan_t* resolve_policy(uint8_t alarm_level,uint8_t supervisor_lost)
{
   if (supervisor_lost)
   {
      return &g_policy_table[POLICY_SUPERVISOR_LOST];
   }
   
   switch (alarm_level)
   {
        case ALARM_HARDFAULT: return &g_policy_table[POLICY_HARDFAULT];
        case ALARM_DANGER:    return &g_policy_table[POLICY_DANGER];
        case ALARM_WARNING:   return &g_policy_table[POLICY_WARNING];
        default:              return &g_policy_table[POLICY_SAFE];
   }
}


/* ===== L4 Driver Adapter ===== */
#define LED_LEVEL_ON     GPIO_PIN_RESET
#define LED_LEVEL_OFF    GPIO_PIN_SET
static void drv_led_write(uint8_t r, uint8_t g, uint8_t b)
{
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, r ? LED_LEVEL_ON : LED_LEVEL_OFF);
    HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, g ? LED_LEVEL_ON : LED_LEVEL_OFF);
    HAL_GPIO_WritePin(LED_B_GPIO_Port, LED_B_Pin, b ? LED_LEVEL_ON : LED_LEVEL_OFF);
}

static void drv_beep_freq(uint32_t freq)
{
    if (freq == 0)
        beep_stop();
    else
        pwm_set(freq, 50);
}


/* ===== 换算成ms并保护不小于1ms ===== */
static inline rt_tick_t ms_to_tick_safe(uint16_t ms)
{
    rt_tick_t t = (rt_tick_t)((RT_TICK_PER_SECOND * (uint32_t)ms) / 1000);
    return (t < 1) ? 1 : t;
}


/*============  定时器回调    ===== */

static void led_timer_callback(void *parameter)
{
    led_phase = !led_phase;
    if (led_phase)
        drv_led_write(current_plan.led.r_on,
                      current_plan.led.g_on,
                      current_plan.led.b_on);
    else
        drv_led_write(0, 0, 0);
}

static void beep_timer_callback(void *parameter)
{
    const beep_mode_t *bp = &current_plan.beep;
    rt_tick_t t;

    /* 防御: 静音或长鸣不应进入此状态机 */
    if (bp->is_mute || bp->on_ms == 0)
        return;

    if (beep_phase == 0)
    {
        /* 一次鸣响结束 → 静音 */
        drv_beep_freq(0);
        beep_count++;

        if (bp->beats_per_group > 0 && beep_count >= bp->beats_per_group)
        {
            /* 一组鸣响完成 → 进入组间间隔 */
            beep_count = 0;
            beep_phase = 2;
            t = ms_to_tick_safe(bp->group_gap_ms);
        }
        else
        {
            /* 组内还有 beep → 进入组内间隔 */
            beep_phase = 1;
            t = ms_to_tick_safe(bp->inter_beep_ms);
        }
    }
    else
    {
        /* 组内间隔(1)或组间间隔(2)结束 → 直接开始下一次鸣响 */
        beep_phase = 0;
        drv_beep_freq(bp->pwm_freq);
        t = ms_to_tick_safe(bp->on_ms);
    }

    rt_timer_control(&beep_timer, RT_TIMER_CTRL_SET_TIME, &t);
    rt_timer_start(&beep_timer);
}


/* ===== L3: Dispatcher ===== */
/**
 * @brief  幂等分发: plan 未变化则跳过, 否则配置 LED/BEEP 并启动定时器
 * @param  new_plan  新的执行计划 (const, 不修改传入内容)
 * @note   写入 current_plan 前先 stop 定时器, 防止回调读半成品数据
 */
static void dispatch_plan(const action_plan_t *new_plan)
{
    rt_tick_t t;

    /* ① 幂等检查必须放在最前面, 没变化直接return, 完全不碰定时器 */
    if (rt_memcmp(new_plan, &last_plan, sizeof(action_plan_t)) == 0)
        return;

    /* ② 确认变了之后, 才停止定时器 */
    rt_timer_stop(&led_timer);
    rt_timer_stop(&beep_timer);

    /* TODO: ③ 写入 current_plan 和 last_plan */
    rt_memcpy(&current_plan, new_plan, sizeof(action_plan_t));
    rt_memcpy(&last_plan,    new_plan, sizeof(action_plan_t));
    /* TODO: ④ LED 分支 (blink ? PERIODIC : 常亮) */
        if (current_plan.led.blink)
    {
        led_phase = 1;
        drv_led_write(current_plan.led.r_on, current_plan.led.g_on, current_plan.led.b_on);
        t = ms_to_tick_safe(current_plan.led.blink_period_ms);
        rt_timer_control(&led_timer, RT_TIMER_CTRL_SET_TIME, &t);
        rt_timer_start(&led_timer);
    }
    else
    {
        drv_led_write(current_plan.led.r_on, current_plan.led.g_on, current_plan.led.b_on);
    }
    /* TODO: ⑤ Beep 三分支 (is_mute / on_ms==0 / 状态机) */
    /* 状态机重置: 切换 plan 时必须清零, 否则旧状态影响新状态 */
    beep_count = 0;
    beep_phase = 0;

    if(current_plan.beep.is_mute)
    {
        /* 分支 1: 静音 (SAFE) */
        drv_beep_freq(0);
    }
    else if (!current_plan.beep.on_ms)
    {
        /* 分支 2: 持续长鸣 (SUPERVISOR_LOST) */
        drv_beep_freq(current_plan.beep.pwm_freq);
    }
    else
    {
        /* 分支 3: 启动状态机定时器 (WARNING/DANGER/HARDFAULT) */
        drv_beep_freq(current_plan.beep.pwm_freq);
        t = ms_to_tick_safe(current_plan.beep.on_ms);
        rt_timer_control(&beep_timer, RT_TIMER_CTRL_SET_TIME, &t);
        rt_timer_start(&beep_timer);
    }

}

void actuator_thread_entry(void *parameter)
{
    rt_uint32_t e;
    monitor_msg_t local;
    uint8_t     supervisor_lost;
    const action_plan_t *plan;

    /* ===== 绑定回调函数下 ===== */
    rt_timer_init(&led_timer,  "act_led",  led_timer_callback,  RT_NULL,
                  1, RT_TIMER_FLAG_PERIODIC  | RT_TIMER_FLAG_SOFT_TIMER);
    rt_timer_init(&beep_timer, "act_beep", beep_timer_callback, RT_NULL,
                  1, RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);

    /* ===== 0xFF 哨兵: 防止因与表内数据重合而误触 ===== */
    rt_memset(&last_plan, 0xFF, sizeof(action_plan_t));
    dispatch_plan(&g_policy_table[POLICY_SAFE]);

    while (1)
    {
         /* L1: 带超时等待 Supervisor 告警事件 (500ms 超时, 非永久阻塞) */
        rt_event_recv(&adc_event,
                      EVT_SAFE | EVT_WARNING | EVT_DANGER | EVT_HARDFAULT,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      SUPERVISOR_TIMEOUT_TICKS,
                      &e);

        /* L1: 无论事件是否超时都读快照 (timestamp 陈旧度判断用) */
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
        rt_memcpy(&local, &monitor_msg, sizeof(monitor_msg_t));
        rt_mutex_release(sensor_mutex);

        /* L1: 真正失联判据是 supervisor_heartbeat 字段陈旧度 */
        supervisor_lost = ((rt_tick_get() - local.supervisor_heartbeat) >
                            SUPERVISOR_TIMEOUT_TICKS);

        /* L2: 查表 */
        plan = resolve_policy(local.alarm_level, supervisor_lost);

        /* L3: 幂等分发 */
        dispatch_plan(plan);

        rt_thread_mdelay(100);
    }
}

int app_actuator_init(void)
{
    actuator_thread = rt_thread_create("actuator",
                                    actuator_thread_entry,
                                    RT_NULL,
                                    1536,
                                    11,
                                    20);
    if (actuator_thread != RT_NULL)
    {
        rt_thread_startup(actuator_thread);
    }
    else
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_APP_EXPORT(app_actuator_init);
