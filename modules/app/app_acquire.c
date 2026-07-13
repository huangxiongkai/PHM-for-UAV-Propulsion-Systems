/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-28     12811       the first version
 */

#include <rtthread.h>
#include <middle/mid_databus.h>
#include <middle/mid_filter.h>
#include <drivers/bsp_adc.h>

//关键分析：
//1.【互斥量】在数据写入时，因为这个结构体也有可能被别的线程读取，有可能打断线程（防止数据竞争）。因此需要在写入数据前打开互斥锁，写入成功后重新关闭
//RT_IPC_FLAG_PRIO (优先级唤醒)   |    RT_IPC_FLAG_FIFO (先进先出唤醒)

/* 任务拆解：提取DMA数据(DMA触发中断后，通过发送信号量，通知线程一开始工作)  +  原始数据去噪 */
extern rt_sem_t alarm_sem;
extern volatile uint8_t shadow_ready_half;  // SAFE: 0=前半段就绪, 1=后半段就绪

/* 静态线程 */
ALIGN(RT_ALIGN_SIZE)
static struct rt_thread rawdata_thread1;
static rt_uint8_t rawdata_rstack[1024];

rt_mutex_t sensor_mutex = RT_NULL;  // SAFE: 非static，供display线程通过extern访问


void rawdata_proc_entry(void *parameter)
{
    sensor_mutex = rt_mutex_create("sens_mtx", RT_IPC_FLAG_PRIO);

    uint16_t *raw_buf = adc_get_shadow_buf();  // SAFE: 从影子缓冲区读取，非活跃DMA缓冲
    uint16_t ch_volt[SAMPLE_COUNT];
    uint16_t ch_temp[SAMPLE_COUNT];

    while (1)
    {
        //1.提取数据并滤波
        //确认数据是否收完
        rt_sem_take(alarm_sem, RT_WAITING_FOREVER);

        //存入数据临时缓冲区
        uint16_t offset = (shadow_ready_half == 0) ? 0 : HALF_BUF_SZ;  // SAFE: 根据最新就绪的半区确定偏移
        for (int i = 0; i < SAMPLE_COUNT; i++)
        {
            ch_volt[i] = raw_buf[offset + i * 2];
            ch_temp[i] = raw_buf[offset + i * 2 + 1];
        }

        //用去极值滤波去除毛刺
        uint16_t v_avg = fast_filing(ch_volt , SAMPLE_COUNT);
        uint16_t t_avg = fast_filing(ch_temp , SAMPLE_COUNT);

        /* ---- 电压传感器开路/短路检测（原始ADC码值极端值） ---- */
        uint8_t volt_fault = 0;
        if (v_avg <= 10 || v_avg >= 4085)
        {
            volt_fault = 3;
        }

        //物理量转化
        float v_now = Pot_To_SimBatteryVol(v_avg);
        float t_now = calculate_temp(t_avg);

        //2.写入数据
        rt_mutex_take(sensor_mutex, RT_WAITING_FOREVER);
        monitor_msg.voltage = v_now;
        monitor_msg.temperature = t_now;
        monitor_msg.sensor_fault = volt_fault;
        rt_mutex_release(sensor_mutex);

                //通过消息队列发送数据
        rt_mq_send(&monitor_mq, &monitor_msg, sizeof(monitor_msg_t));

    }
}


int app_monitor_init(void)
{
    rt_err_t result1 = rt_thread_init(&rawdata_thread1, "rawdata_thread1",
            rawdata_proc_entry, RT_NULL,
            rawdata_rstack, sizeof(rawdata_rstack),
            8, 20);

    if (result1 != RT_EOK)
        return -RT_ERROR;

    rt_thread_startup(&rawdata_thread1);
    return RT_EOK;
}
INIT_APP_EXPORT(app_monitor_init);
