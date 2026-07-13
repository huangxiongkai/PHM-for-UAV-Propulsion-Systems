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

monitor_msg_t monitor_msg = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0,          /* alarm_level*/
    ALARM_SAFE, /* hard_fault */
    0,          /* sensor_fault  */
    0,           /* timestamp */
    0,          /* supervisor_heartbeat */
    0,          /* fault_cause */ 
};
struct rt_messagequeue monitor_mq;
static rt_uint8_t monitor_pool[512];

rt_sem_t alarm_sem = RT_NULL;
rt_sem_t predict_ready_sem = RT_NULL;
struct rt_event adc_event;

int sys_ipc_init(void)
{
    rt_err_t ret_mq = rt_mq_init(&monitor_mq, "monitor_mq",
                                 monitor_pool,
                                 sizeof(monitor_msg_t),
                                 sizeof(monitor_pool),
                                 RT_IPC_FLAG_FIFO);
    if (ret_mq != RT_EOK)
        return -RT_ERROR;

    //用于通知Acquire线程DMA传输完成
    rt_err_t ret_evt = rt_event_init(&adc_event, "adc_evt", RT_IPC_FLAG_FIFO);
    if (ret_evt != RT_EOK)
        return -RT_ERROR;

    //用于通知Acquire线程DMA传输完成
    alarm_sem = rt_sem_create("alarm_sem", 0, RT_IPC_FLAG_FIFO);
    if (alarm_sem == RT_NULL)
        return -RT_ERROR;


    //冷启动时，Supervisor线程需要等待Predict线程至少写入一次数据，才能进行首帧定态计算。因此创建一个信号量，用于Predict线程在首次写入完成后释放，通知Supervisor线程可以继续执行。
    predict_ready_sem = rt_sem_create("pred_rdy", 0, RT_IPC_FLAG_FIFO);
    if (predict_ready_sem == RT_NULL)
        return -RT_ERROR;

    return RT_EOK;
}
INIT_APP_EXPORT(sys_ipc_init);
