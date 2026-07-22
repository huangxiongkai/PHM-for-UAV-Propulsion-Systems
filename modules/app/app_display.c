/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-28     12811       the first version
 */
#include <middle/mid_databus.h>

#include <testbench/tb.h>      /* tb.h 内部定义 USE_TESTBENCH，后续代码用 #ifdef 门控 */

static rt_thread_t log_show_thread = RT_NULL;

/* 与 acquire 线程共享的互斥量 */
extern rt_mutex_t sensor_mutex;

void log_show_thread_entry(void *parameter)
{
    rt_kprintf("[SYSTEM] Monitor log thread started.\r\n");

    while (1)
    {
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);          // 防止读取到半更新的 monitor_msg
        monitor_msg_t local_copy = monitor_msg;                   // 快照副本
        rt_mutex_release(sensor_mutex);

#ifdef USE_TESTBENCH
        if (tb_is_active())
        {
            tb_log_csv(&local_copy);
        }
        else
#endif
        {
            /* 数据进一位处理 */
            int v_i = (int)local_copy.voltage;
            int v_d = (int)((local_copy.voltage - v_i) * 10);
            if (v_d < 0) v_d = -v_d;

            int t_i = (int)local_copy.temperature;
            int t_d = (int)((local_copy.temperature - t_i) * 10);
            if (t_d < 0) t_d = -t_d;

            rt_kprintf("[MONITOR] Volt: %d.%dV, Temp: %d.%dC, ALM:%d, FC:%d\r\n",
                       v_i, v_d, t_i, t_d, local_copy.alarm_level, local_copy.fault_cause);
        }

        rt_thread_mdelay(100);
    }
}

int app_show_init(void)
{
    log_show_thread = rt_thread_create("log_show",
                                        log_show_thread_entry,
                                        RT_NULL,
                                        1536,
                                        10,
                                        20);

    if (log_show_thread != RT_NULL)
    {
        rt_thread_startup(log_show_thread);
    }
    else
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_APP_EXPORT(app_show_init);
